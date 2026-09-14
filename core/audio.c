/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. */
#include "audio.h"
#include "file.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static bool Grow(void **items, size_t *capacity, size_t itemSize)
{
    size_t next = *capacity ? *capacity * 2 : 8;
    void *grown = realloc(*items, next * itemSize);
    if (!grown) return false;
    *items = grown; *capacity = next; return true;
}
static size_t Bus(const CoreAudio *a, const char *name)
{
    for (size_t i = 0; i < a->busCount; i++) if (!strcmp(a->buses[i].name, name)) return i;
    return SIZE_MAX;
}
static float Volume(const CoreAudio *a, size_t bus)
{
    size_t master = Bus(a, "master");
    if (a->buses[bus].muted || (master != SIZE_MAX && a->buses[master].muted)) return 0;
    if (bus == master) return a->buses[bus].volume;
    return a->buses[bus].volume * (master == SIZE_MAX ? 1 : a->buses[master].volume);
}
static bool Ready(CoreAudio *a)
{
    if (a->ready) return true;
    if (IsAudioDeviceReady()) { a->ready = true; return true; }
    InitAudioDevice(); a->ready = IsAudioDeviceReady(); a->ownsDevice = a->ready;
    return a->ready;
}
bool CoreAudioInit(CoreAudio *a)
{
    if (!a) return false;
    *a = (CoreAudio){0};
    return CoreAudioAddBus(a, "master", 1) && CoreAudioAddBus(a, "sfx", 1) && CoreAudioAddBus(a, "music", 1);
}
void CoreAudioFree(CoreAudio *a)
{
    if (!a) return;
    for (size_t i = 0; i < a->soundCount; i++) UnloadSound(a->sounds[i].sound);
    for (size_t i = 0; i < a->musicCount; i++) UnloadMusicStream(a->music[i].music);
    if (a->ownsDevice) CloseAudioDevice();
    free(a->buses); free(a->sounds); free(a->music); *a = (CoreAudio){0};
}
bool CoreAudioAddBus(CoreAudio *a, const char *name, float volume)
{
    if (!a || !name || !*name || strlen(name) >= CORE_AUDIO_NAME_CAPACITY || Bus(a, name) != SIZE_MAX) return false;
    if (a->busCount == a->busCapacity && !Grow((void **)&a->buses, &a->busCapacity, sizeof *a->buses)) return false;
    a->buses[a->busCount++] = (CoreAudioBus){.volume = volume < 0 ? 0 : volume};
    strcpy(a->buses[a->busCount - 1].name, name); return true;
}
bool CoreAudioSetBusVolume(CoreAudio *a, const char *name, float volume)
{
    if (!a) return false;
    size_t bus = Bus(a, name);
    if (bus == SIZE_MAX) return false;
    a->buses[bus].volume = volume < 0 ? 0 : volume;
    bool master = !strcmp(name, "master");
    for (size_t i = 0; i < a->soundCount; i++) if (master || a->sounds[i].bus == bus) SetSoundVolume(a->sounds[i].sound, Volume(a, a->sounds[i].bus));
    for (size_t i = 0; i < a->musicCount; i++) if (master || a->music[i].bus == bus) SetMusicVolume(a->music[i].music, Volume(a, a->music[i].bus));
    return true;
}
bool CoreAudioSetBusMuted(CoreAudio *a, const char *name, bool muted)
{ size_t bus = a ? Bus(a, name) : SIZE_MAX; if (bus == SIZE_MAX) return false; a->buses[bus].muted = muted; return CoreAudioSetBusVolume(a, name, a->buses[bus].volume); }
bool CoreAudioPlaySound(CoreAudio *a, const char *path, const char *busName)
{
    if (!path || strlen(path) >= sizeof ((CoreAudioSound *)0)->path) return false;
    size_t bus = a ? Bus(a, busName) : SIZE_MAX; if (bus == SIZE_MAX || !Ready(a)) return false;
    for (size_t i = 0; i < a->soundCount; i++) if (!strcmp(a->sounds[i].path, path)) { a->sounds[i].bus = bus; SetSoundVolume(a->sounds[i].sound, Volume(a, bus)); PlaySound(a->sounds[i].sound); return true; }
    char resolved[512]; const char *file = CoreResolvePath(path, resolved, sizeof resolved); Sound sound = file ? LoadSound(file) : (Sound){0};
    if (!IsSoundValid(sound) || (a->soundCount == a->soundCapacity && !Grow((void **)&a->sounds, &a->soundCapacity, sizeof *a->sounds))) { if (IsSoundValid(sound)) UnloadSound(sound); return false; }
    CoreAudioSound *item = &a->sounds[a->soundCount++]; *item = (CoreAudioSound){.sound = sound, .bus = bus}; strcpy(item->path, path); SetSoundVolume(sound, Volume(a, bus)); PlaySound(sound); return true;
}
bool CoreAudioPlayMusic(CoreAudio *a, const char *path, const char *busName)
{
    if (!path || strlen(path) >= sizeof ((CoreAudioMusic *)0)->path) return false;
    size_t bus = a ? Bus(a, busName) : SIZE_MAX; if (bus == SIZE_MAX || !Ready(a)) return false;
    for (size_t i = 0; i < a->musicCount; i++) if (!strcmp(a->music[i].path, path)) { a->music[i].bus = bus; SetMusicVolume(a->music[i].music, Volume(a,bus)); PlayMusicStream(a->music[i].music); a->music[i].playing = true; return true; }
    char resolved[512]; const char *file = CoreResolvePath(path, resolved, sizeof resolved); Music music = file ? LoadMusicStream(file) : (Music){0};
    if (!IsMusicValid(music) || (a->musicCount == a->musicCapacity && !Grow((void **)&a->music, &a->musicCapacity, sizeof *a->music))) { if (IsMusicValid(music)) UnloadMusicStream(music); return false; }
    CoreAudioMusic *item = &a->music[a->musicCount++]; *item = (CoreAudioMusic){.music = music, .bus = bus, .playing = true}; strcpy(item->path,path); SetMusicVolume(music, Volume(a,bus)); PlayMusicStream(music); return true;
}
void CoreAudioUpdate(CoreAudio *a) { if (a) for (size_t i = 0; i < a->musicCount; i++) if (a->music[i].playing) UpdateMusicStream(a->music[i].music); }
bool CoreAudioStopMusic(CoreAudio *a, const char *path) { if (!a || !path) return false; for (size_t i=0;i<a->musicCount;i++) if (!strcmp(a->music[i].path,path)) { StopMusicStream(a->music[i].music); a->music[i].playing=false; return true; } return false; }
