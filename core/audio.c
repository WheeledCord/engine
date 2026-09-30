/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include "audio.h"
#include "file.h"
#include "raymath.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool Grow(void **items, size_t *capacity, size_t itemSize)
{
    size_t next = *capacity ? *capacity * 2 : 8;
    void *grown = realloc(*items, next * itemSize);
    if (!grown)
        return false;
    *items = grown;
    *capacity = next;
    return true;
}

static size_t Bus(const CoreAudio *a, const char *name)
{
    if (!name)
        return SIZE_MAX;
    for (size_t i = 0; i < a->busCount; i++)
        if (!strcmp(a->buses[i].name, name))
            return i;
    return SIZE_MAX;
}

// A bus's effective volume: its own, times the master's, and nothing when either is muted.
static float Volume(const CoreAudio *a, size_t bus)
{
    size_t master = Bus(a, "master");
    if (a->buses[bus].muted || (master != SIZE_MAX && a->buses[master].muted))
        return 0;
    if (bus == master)
        return a->buses[bus].volume;
    return a->buses[bus].volume * (master == SIZE_MAX ? 1 : a->buses[master].volume);
}

// The device opens on the first sound anyone plays, not before.
static bool Ready(CoreAudio *a)
{
    if (a->ready)
        return true;
    if (IsAudioDeviceReady())
    {
        a->ready = true;
        return true;
    }
    InitAudioDevice();
    a->ready = IsAudioDeviceReady();
    a->ownsDevice = a->ready;
    return a->ready;
}

bool CoreAudioInit(CoreAudio *a)
{
    if (!a)
        return false;
    *a = (CoreAudio){0};
    a->listenerForward = (Vector3){0, 0, 1};
    a->listenerUp = (Vector3){0, 1, 0};
    return CoreAudioAddBus(a, "master", 1) && CoreAudioAddBus(a, "sfx", 1) &&
           CoreAudioAddBus(a, "music", 1);
}

void CoreAudioFree(CoreAudio *a)
{
    if (!a)
        return;
    for (size_t i = 0; i < a->heldCount; i++)
        if (a->held[i].live)
        {
            StopSound(a->held[i].alias);
            UnloadSoundAlias(a->held[i].alias);
        }
    for (size_t i = 0; i < a->soundCount; i++)
    {
        for (int v = 1; v < a->sounds[i].voiceCount; v++)
            UnloadSoundAlias(a->sounds[i].voices[v]);
        UnloadSound(a->sounds[i].sound);
    }
    for (size_t i = 0; i < a->musicCount; i++)
        UnloadMusicStream(a->music[i].music);
    if (a->ownsDevice)
        CloseAudioDevice();
    free(a->buses);
    free(a->sounds);
    free(a->music);
    free(a->held);
    *a = (CoreAudio){0};
}

bool CoreAudioAddBus(CoreAudio *a, const char *name, float volume)
{
    if (!a || !name || !*name || strlen(name) >= CORE_AUDIO_NAME_CAPACITY || Bus(a, name) != SIZE_MAX)
        return false;
    if (a->busCount == a->busCapacity && !Grow((void **)&a->buses, &a->busCapacity, sizeof *a->buses))
        return false;
    a->buses[a->busCount++] = (CoreAudioBus){.volume = volume < 0 ? 0 : volume};
    strcpy(a->buses[a->busCount - 1].name, name);
    return true;
}

// Every voice on a bus hears a change to it at once: fire-and-forget, held, and music.
static void ApplyBus(CoreAudio *a, size_t bus, bool all)
{
    for (size_t i = 0; i < a->soundCount; i++)
        if (all || a->sounds[i].bus == bus)
            for (int v = 0; v < a->sounds[i].voiceCount; v++)
                SetSoundVolume(a->sounds[i].voices[v],
                               Volume(a, a->sounds[i].bus) * a->sounds[i].voiceGain[v]);
    for (size_t i = 0; i < a->musicCount; i++)
        if (all || a->music[i].bus == bus)
            SetMusicVolume(a->music[i].music, Volume(a, a->music[i].bus));
}

bool CoreAudioSetBusVolume(CoreAudio *a, const char *name, float volume)
{
    size_t bus = a ? Bus(a, name) : SIZE_MAX;
    if (bus == SIZE_MAX)
        return false;
    a->buses[bus].volume = volume < 0 ? 0 : volume;
    ApplyBus(a, bus, !strcmp(name, "master"));
    return true;
}

bool CoreAudioSetBusMuted(CoreAudio *a, const char *name, bool muted)
{
    size_t bus = a ? Bus(a, name) : SIZE_MAX;
    if (bus == SIZE_MAX)
        return false;
    a->buses[bus].muted = muted;
    return CoreAudioSetBusVolume(a, name, a->buses[bus].volume);
}

// ---- where the listener is ------------------------------------------------------------------

void CoreAudioSetListener(CoreAudio *a, Vector3 position, Vector3 forward, Vector3 up)
{
    if (!a)
        return;
    a->listenerPosition = position;
    a->listenerForward = forward;
    a->listenerUp = up;
}

float CoreAudioFalloff(float distance, float range)
{
    if (!(range > 0))
        return 1.0f;
    float left = 1.0f - distance / range;
    return left > 0 ? left : 0.0f;
}

float CoreAudioPanAt(const CoreAudio *a, Vector3 position)
{
    if (!a)
        return 0.5f;
    /* The listener's own right, and where the sound is across it and ahead of it: Godot's
       _calc_output_vol_stereo takes the cosine of the angle off the listener's side axis in the
       horizontal plane, so overhead and dead ahead both land in the middle. */
    Vector3 forward = Vector3Normalize(a->listenerForward);
    Vector3 up = Vector3Normalize(a->listenerUp);
    Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, up));
    Vector3 to = Vector3Subtract(position, a->listenerPosition);
    float across = Vector3DotProduct(to, right), ahead = Vector3DotProduct(to, forward);
    float flat = sqrtf(across * across + ahead * ahead);
    if (!(flat > 1e-6f) || Vector3Length(right) < 0.5f)
        return 0.5f;
    // raylib's pan is the left channel's share: 1 is hard left, 0 is hard right.
    return 0.5f - 0.5f * Clamp(across / flat, -1.0f, 1.0f);
}

float CoreAudioGainAt(const CoreAudio *a, Vector3 position, float range, float gain)
{
    if (!a)
        return 0;
    return gain * CoreAudioFalloff(Vector3Distance(position, a->listenerPosition), range);
}

// ---- sounds, loaded once and played on as many voices as there are ----------------------------

static CoreAudioSound *Cached(CoreAudio *a, const char *path, const char *busName)
{
    if (!a || !path || strlen(path) >= sizeof ((CoreAudioSound *)0)->path)
        return NULL;
    size_t bus = Bus(a, busName);
    if (bus == SIZE_MAX || !Ready(a))
        return NULL;
    for (size_t i = 0; i < a->soundCount; i++)
        if (!strcmp(a->sounds[i].path, path))
        {
            a->sounds[i].bus = bus;
            return &a->sounds[i];
        }
    char resolved[512];
    const char *file = CoreResolvePath(path, resolved, sizeof resolved);
    Sound sound = file ? LoadSound(file) : (Sound){0};
    if (!IsSoundValid(sound))
        return NULL;
    if (a->soundCount == a->soundCapacity &&
        !Grow((void **)&a->sounds, &a->soundCapacity, sizeof *a->sounds))
    {
        UnloadSound(sound);
        return NULL;
    }
    CoreAudioSound *item = &a->sounds[a->soundCount++];
    *item = (CoreAudioSound){.sound = sound, .bus = bus, .voiceCount = 1};
    item->voices[0] = sound;
    item->voiceGain[0] = 1;
    strcpy(item->path, path);
    return item;
}

// A free voice of a sound, making another alias while there is room, else the next in turn.
static int FreeVoice(CoreAudioSound *s)
{
    for (int v = 0; v < s->voiceCount; v++)
        if (!IsSoundPlaying(s->voices[v]))
            return v;
    if (s->voiceCount < CORE_AUDIO_POLYPHONY)
    {
        Sound alias = LoadSoundAlias(s->sound);
        if (IsSoundValid(alias))
        {
            s->voices[s->voiceCount] = alias;
            s->voiceGain[s->voiceCount] = 1;
            return s->voiceCount++;
        }
    }
    int oldest = s->next;
    s->next = (s->next + 1) % s->voiceCount;
    return oldest;
}

static bool Start(CoreAudio *a, CoreAudioSound *s, float gain, float pan)
{
    int v = FreeVoice(s);
    s->voiceGain[v] = gain < 0 ? 0 : gain;
    SetSoundVolume(s->voices[v], Volume(a, s->bus) * s->voiceGain[v]);
    SetSoundPan(s->voices[v], pan);
    PlaySound(s->voices[v]);
    return true;
}

bool CoreAudioLoadSound(CoreAudio *a, const char *path, const char *bus)
{
    return Cached(a, path, bus) != NULL;
}

bool CoreAudioPlaySoundGain(CoreAudio *a, const char *path, const char *bus, float gain)
{
    CoreAudioSound *s = Cached(a, path, bus);
    return s && Start(a, s, gain, 0.5f);
}

bool CoreAudioPlaySound(CoreAudio *a, const char *path, const char *bus)
{
    return CoreAudioPlaySoundGain(a, path, bus, 1.0f);
}

bool CoreAudioPlaySoundAt(CoreAudio *a, const char *path, const char *bus, Vector3 position,
                          float range, float gain)
{
    float heard = CoreAudioGainAt(a, position, range, gain);
    if (!(heard > 0))
        return false; // too far to hear: not worth a voice
    CoreAudioSound *s = Cached(a, path, bus);
    return s && Start(a, s, heard, CoreAudioPanAt(a, position));
}

void CoreAudioStopSound(CoreAudio *a, const char *path)
{
    if (!a || !path)
        return;
    for (size_t i = 0; i < a->soundCount; i++)
        if (!strcmp(a->sounds[i].path, path))
            for (int v = 0; v < a->sounds[i].voiceCount; v++)
                StopSound(a->sounds[i].voices[v]);
}

bool CoreAudioSoundPlaying(const CoreAudio *a, const char *path)
{
    if (!a || !path)
        return false;
    for (size_t i = 0; i < a->soundCount; i++)
        if (!strcmp(a->sounds[i].path, path))
            for (int v = 0; v < a->sounds[i].voiceCount; v++)
                if (IsSoundPlaying(a->sounds[i].voices[v]))
                    return true;
    return false;
}

// ---- music ----------------------------------------------------------------------------------

bool CoreAudioPlayMusic(CoreAudio *a, const char *path, const char *busName)
{
    if (!path || strlen(path) >= sizeof ((CoreAudioMusic *)0)->path)
        return false;
    size_t bus = a ? Bus(a, busName) : SIZE_MAX;
    if (bus == SIZE_MAX || !Ready(a))
        return false;
    for (size_t i = 0; i < a->musicCount; i++)
        if (!strcmp(a->music[i].path, path))
        {
            a->music[i].bus = bus;
            SetMusicVolume(a->music[i].music, Volume(a, bus));
            PlayMusicStream(a->music[i].music);
            a->music[i].playing = true;
            return true;
        }
    char resolved[512];
    const char *file = CoreResolvePath(path, resolved, sizeof resolved);
    Music music = file ? LoadMusicStream(file) : (Music){0};
    if (!IsMusicValid(music) ||
        (a->musicCount == a->musicCapacity && !Grow((void **)&a->music, &a->musicCapacity, sizeof *a->music)))
    {
        if (IsMusicValid(music))
            UnloadMusicStream(music);
        return false;
    }
    CoreAudioMusic *item = &a->music[a->musicCount++];
    *item = (CoreAudioMusic){.music = music, .bus = bus, .playing = true};
    strcpy(item->path, path);
    SetMusicVolume(music, Volume(a, bus));
    PlayMusicStream(music);
    return true;
}

bool CoreAudioStopMusic(CoreAudio *a, const char *path)
{
    if (!a || !path)
        return false;
    for (size_t i = 0; i < a->musicCount; i++)
        if (!strcmp(a->music[i].path, path))
        {
            StopMusicStream(a->music[i].music);
            a->music[i].playing = false;
            return true;
        }
    return false;
}

// ---- voices a caller holds ------------------------------------------------------------------

static CoreAudioHeldVoice *Held(const CoreAudio *a, CoreAudioVoice voice)
{
    if (!a || voice.index >= a->heldCount)
        return NULL;
    CoreAudioHeldVoice *h = &a->held[voice.index];
    return h->live && h->generation == voice.generation ? h : NULL;
}

static float HeldGain(const CoreAudio *a, const CoreAudioHeldVoice *h)
{
    return h->placed ? CoreAudioGainAt(a, h->position, h->range, h->gain) : h->gain;
}

// Sets what raylib plays a held voice at, from its gain, its place and its bus.
static void Place(CoreAudio *a, CoreAudioHeldVoice *h)
{
    SetSoundVolume(h->alias, Volume(a, h->bus) * HeldGain(a, h));
    SetSoundPan(h->alias, h->placed ? CoreAudioPanAt(a, h->position) : 0.5f);
}

CoreAudioVoice CoreAudioVoiceCreate(CoreAudio *a, const char *path, const char *bus)
{
    CoreAudioVoice none = {UINT32_MAX, 0};
    CoreAudioSound *s = Cached(a, path, bus);
    if (!s)
        return none;
    Sound alias = LoadSoundAlias(s->sound);
    if (!IsSoundValid(alias))
        return none;
    size_t slot = a->heldCount;
    for (size_t i = 0; i < a->heldCount; i++)
        if (!a->held[i].live)
        {
            slot = i;
            break;
        }
    if (slot == a->heldCount)
    {
        if (a->heldCount == a->heldCapacity &&
            !Grow((void **)&a->held, &a->heldCapacity, sizeof *a->held))
        {
            UnloadSoundAlias(alias);
            return none;
        }
        a->held[a->heldCount++] = (CoreAudioHeldVoice){0};
    }
    CoreAudioHeldVoice *h = &a->held[slot];
    uint32_t generation = h->generation;
    *h = (CoreAudioHeldVoice){.alias = alias, .bus = s->bus, .gain = 1, .live = true,
                              .generation = generation};
    return (CoreAudioVoice){(uint32_t)slot, generation};
}

bool CoreAudioVoiceValid(const CoreAudio *a, CoreAudioVoice voice) { return Held(a, voice) != NULL; }

bool CoreAudioVoicePlay(CoreAudio *a, CoreAudioVoice voice, float gain)
{
    CoreAudioHeldVoice *h = Held(a, voice);
    if (!h)
        return false;
    h->gain = gain < 0 ? 0 : gain;
    h->placed = false;
    h->playing = true;
    Place(a, h);
    PlaySound(h->alias);
    return true;
}

bool CoreAudioVoicePlayAt(CoreAudio *a, CoreAudioVoice voice, Vector3 position, float range, float gain)
{
    CoreAudioHeldVoice *h = Held(a, voice);
    if (!h)
        return false;
    h->gain = gain < 0 ? 0 : gain;
    h->placed = true;
    h->position = position;
    h->range = range;
    h->playing = true;
    Place(a, h);
    PlaySound(h->alias);
    return true;
}

bool CoreAudioVoiceMove(CoreAudio *a, CoreAudioVoice voice, Vector3 position)
{
    CoreAudioHeldVoice *h = Held(a, voice);
    if (!h)
        return false;
    h->position = position;
    h->placed = true;
    Place(a, h);
    return true;
}

bool CoreAudioVoiceSetGain(CoreAudio *a, CoreAudioVoice voice, float gain)
{
    CoreAudioHeldVoice *h = Held(a, voice);
    if (!h)
        return false;
    h->gain = gain < 0 ? 0 : gain;
    Place(a, h);
    return true;
}

float CoreAudioVoiceGain(const CoreAudio *a, CoreAudioVoice voice)
{
    const CoreAudioHeldVoice *h = Held(a, voice);
    return h ? HeldGain(a, h) : 0.0f;
}

bool CoreAudioVoiceSetLoop(CoreAudio *a, CoreAudioVoice voice, bool loop)
{
    CoreAudioHeldVoice *h = Held(a, voice);
    if (!h)
        return false;
    h->loop = loop;
    return true;
}

bool CoreAudioVoiceStop(CoreAudio *a, CoreAudioVoice voice)
{
    CoreAudioHeldVoice *h = Held(a, voice);
    if (!h)
        return false;
    h->playing = false;
    StopSound(h->alias);
    return true;
}

bool CoreAudioVoicePlaying(const CoreAudio *a, CoreAudioVoice voice)
{
    const CoreAudioHeldVoice *h = Held(a, voice);
    return h && IsSoundPlaying(h->alias);
}

bool CoreAudioVoiceFree(CoreAudio *a, CoreAudioVoice voice)
{
    CoreAudioHeldVoice *h = Held(a, voice);
    if (!h)
        return false;
    StopSound(h->alias);
    UnloadSoundAlias(h->alias);
    uint32_t generation = h->generation + 1;
    *h = (CoreAudioHeldVoice){.generation = generation};
    return true;
}

void CoreAudioUpdate(CoreAudio *a)
{
    if (!a)
        return;
    for (size_t i = 0; i < a->musicCount; i++)
        if (a->music[i].playing)
            UpdateMusicStream(a->music[i].music);
    // Held voices follow the listener, and a looping one that ran out starts again.
    for (size_t i = 0; i < a->heldCount; i++)
    {
        CoreAudioHeldVoice *h = &a->held[i];
        if (!h->live || !h->playing)
            continue;
        Place(a, h);
        if (!IsSoundPlaying(h->alias))
        {
            if (h->loop)
                PlaySound(h->alias);
            else
                h->playing = false;
        }
    }
}
