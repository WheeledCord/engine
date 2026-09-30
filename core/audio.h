/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_AUDIO_H
#define CORE_AUDIO_H
#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CORE_AUDIO_NAME_CAPACITY 64
/* How many copies of one sound the fire-and-forget calls may play at once, so a shot fired twice
   quickly is heard twice instead of the first being cut off -- Godot's max_polyphony. */
#define CORE_AUDIO_POLYPHONY 8

typedef struct CoreAudioBus { char name[CORE_AUDIO_NAME_CAPACITY]; float volume; bool muted; } CoreAudioBus;
typedef struct CoreAudioSound
{
    char path[256];
    Sound sound;                          /* owns the samples; voices[0] is this sound itself */
    Sound voices[CORE_AUDIO_POLYPHONY];   /* the rest are aliases sharing its samples */
    float voiceGain[CORE_AUDIO_POLYPHONY];
    int voiceCount, next;
    size_t bus;
} CoreAudioSound;
typedef struct CoreAudioMusic { char path[256]; Music music; size_t bus; bool playing; } CoreAudioMusic;

/* A voice the caller holds on to: started, moved, looped, stopped and asked about for as long as it
   wants, the way Godot's AudioStreamPlayer3D is -- a mortar's whistle that follows its shell, an
   electric hum that loops while a battery is connected. */
typedef struct CoreAudioVoice { uint32_t index, generation; } CoreAudioVoice;
typedef struct CoreAudioHeldVoice
{
    Sound alias;
    size_t bus;
    float gain;
    Vector3 position;
    float range;    /* zero: not placed, heard at its gain wherever the listener is */
    bool placed, loop, playing, live;
    uint32_t generation;
} CoreAudioHeldVoice;

typedef struct CoreAudio
{
    CoreAudioBus *buses; size_t busCount, busCapacity;
    CoreAudioSound *sounds; size_t soundCount, soundCapacity;
    CoreAudioMusic *music; size_t musicCount, musicCapacity;
    CoreAudioHeldVoice *held; size_t heldCount, heldCapacity;
    Vector3 listenerPosition, listenerForward, listenerUp;
    bool ready, ownsDevice;
} CoreAudio;

/** Initializes an empty caller-owned audio service. It opens no device until a sound or music call. */
bool CoreAudioInit(CoreAudio *audio);
/** Stops and unloads all resources owned by audio; closes the device only when this service opened it. */
void CoreAudioFree(CoreAudio *audio);
/** Adds a named volume group. Names are unique; volume is clamped to zero or greater. */
bool CoreAudioAddBus(CoreAudio *audio, const char *name, float volume);
/** Changes one named group's effective volume. Existing cached resources on that group update immediately. */
bool CoreAudioSetBusVolume(CoreAudio *audio, const char *name, float volume);
/** Mutes or unmutes a named group without discarding its configured volume. */
bool CoreAudioSetBusMuted(CoreAudio *audio, const char *name, bool muted);
/** Loads path through the engine data root ahead of its first play, so the first play does not
    stall on the load. Returns false on device/load/bus failure. */
bool CoreAudioLoadSound(CoreAudio *audio, const char *path, const char *bus);
/** Loads path through the engine data root once, assigns it to bus, and plays it. Returns false on device/load/bus failure. */
bool CoreAudioPlaySound(CoreAudio *audio, const char *path, const char *bus);
/** Plays a sound once at a gain, on a free voice of it; the oldest is cut off when all are busy. */
bool CoreAudioPlaySoundGain(CoreAudio *audio, const char *path, const char *bus, float gain);
/** Plays a sound once at a place in the world: its gain falls off with distance from the listener
    to silence at range, and it pans toward the side it is on. False when it could not be played,
    or when it is out of range and so was not started. */
bool CoreAudioPlaySoundAt(CoreAudio *audio, const char *path, const char *bus, Vector3 position,
                          float range, float gain);
/** Stops every voice of a sound the fire-and-forget calls started. */
void CoreAudioStopSound(CoreAudio *audio, const char *path);
/** Whether any voice of a sound the fire-and-forget calls started is still playing. */
bool CoreAudioSoundPlaying(const CoreAudio *audio, const char *path);
/** Loads path through the engine data root once, assigns it to bus, and starts its stream. Returns false on failure. */
bool CoreAudioPlayMusic(CoreAudio *audio, const char *path, const char *bus);
/** Advances every playing music stream, restarts looping voices, and re-places held voices for
    where the listener now is. Call once per rendered frame. */
void CoreAudioUpdate(CoreAudio *audio);
/** Stops a cached music stream by path. False means it was never loaded. */
bool CoreAudioStopMusic(CoreAudio *audio, const char *path);

/** Where sounds are heard from: the camera, usually. up and forward need not be exactly square. */
void CoreAudioSetListener(CoreAudio *audio, Vector3 position, Vector3 forward, Vector3 up);
/** Linear falloff with distance: 1 at the source, 0 at range and beyond; a range of zero or less
    never falls off. The same curve as Godot's max_distance with attenuation disabled. */
float CoreAudioFalloff(float distance, float range);
/** raylib's pan for a sound at a place, heard by the listener: 0.5 is centred, above it left,
    below it right. Computed like Godot's stereo panning, from the side the sound is on. */
float CoreAudioPanAt(const CoreAudio *audio, Vector3 position);
/** A placed sound's gain as the listener hears it: gain times the falloff at its distance. */
float CoreAudioGainAt(const CoreAudio *audio, Vector3 position, float range, float gain);

/** A voice of its own for a sound, held until CoreAudioVoiceFree. Invalid when the sound cannot load. */
CoreAudioVoice CoreAudioVoiceCreate(CoreAudio *audio, const char *path, const char *bus);
/** Whether a voice handle still names a voice. */
bool CoreAudioVoiceValid(const CoreAudio *audio, CoreAudioVoice voice);
/** Starts a voice from the beginning at a gain, heard the same wherever the listener is. */
bool CoreAudioVoicePlay(CoreAudio *audio, CoreAudioVoice voice, float gain);
/** Starts a voice from the beginning at a place, falling off to silence at range. */
bool CoreAudioVoicePlayAt(CoreAudio *audio, CoreAudioVoice voice, Vector3 position, float range, float gain);
/** Moves a placed voice; its gain and pan follow at once and on every update. */
bool CoreAudioVoiceMove(CoreAudio *audio, CoreAudioVoice voice, Vector3 position);
/** Changes a voice's own gain, before distance is applied. */
bool CoreAudioVoiceSetGain(CoreAudio *audio, CoreAudioVoice voice, float gain);
/** The gain a voice is heard at now, distance included and bus volume not. Zero for a bad handle. */
float CoreAudioVoiceGain(const CoreAudio *audio, CoreAudioVoice voice);
/** Makes a voice start again whenever it ends, until it is stopped. */
bool CoreAudioVoiceSetLoop(CoreAudio *audio, CoreAudioVoice voice, bool loop);
/** Stops a voice; a looping one stays stopped. */
bool CoreAudioVoiceStop(CoreAudio *audio, CoreAudioVoice voice);
/** Whether a voice is playing. */
bool CoreAudioVoicePlaying(const CoreAudio *audio, CoreAudioVoice voice);
/** Stops and releases a voice; its handle is refused afterwards. */
bool CoreAudioVoiceFree(CoreAudio *audio, CoreAudioVoice voice);

#endif
