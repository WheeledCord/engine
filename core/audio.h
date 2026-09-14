/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#ifndef CORE_AUDIO_H
#define CORE_AUDIO_H
#include "raylib.h"
#include <stdbool.h>
#include <stddef.h>

#define CORE_AUDIO_NAME_CAPACITY 64
typedef struct CoreAudioBus { char name[CORE_AUDIO_NAME_CAPACITY]; float volume; bool muted; } CoreAudioBus;
typedef struct CoreAudioSound { char path[256]; Sound sound; size_t bus; } CoreAudioSound;
typedef struct CoreAudioMusic { char path[256]; Music music; size_t bus; bool playing; } CoreAudioMusic;
typedef struct CoreAudio
{
    CoreAudioBus *buses; size_t busCount, busCapacity;
    CoreAudioSound *sounds; size_t soundCount, soundCapacity;
    CoreAudioMusic *music; size_t musicCount, musicCapacity;
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
/** Loads path through the engine data root once, assigns it to bus, and plays it. Returns false on device/load/bus failure. */
bool CoreAudioPlaySound(CoreAudio *audio, const char *path, const char *bus);
/** Loads path through the engine data root once, assigns it to bus, and starts its stream. Returns false on failure. */
bool CoreAudioPlayMusic(CoreAudio *audio, const char *path, const char *bus);
/** Advances every playing music stream. Call once per rendered frame. */
void CoreAudioUpdate(CoreAudio *audio);
/** Stops a cached music stream by path. False means it was never loaded. */
bool CoreAudioStopMusic(CoreAudio *audio, const char *path);
#endif
