/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_FILE_H
#define CORE_FILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
/* Disk-only, NUL-terminated text. Caller releases with CoreFreeFile.

   CoreSetDataRoot names where the engine's own files (core/shaders, core/fonts) live, for programs
   started from another working directory. EngineRun passes EngineConfig.engine_path, or the
   executable's directory, and the root walks up from there until the engine data is in reach, so a
   binary in a build directory still finds it.

   CoreResolvePath leaves absolute paths and files that exist from the working directory alone, so a
   project can always override an engine file by shipping its own. Anything else is taken from the
   root. Returns NULL when the result would not fit in buf, rather than a shortened path naming
   something else; buf must outlive the returned pointer. */
void CoreSetDataRoot(const char *path);
const char *CoreResolvePath(const char *path, char *buf, size_t buflen);

/** @brief Read an entire binary file through the configured data root.
 * @param path Absolute, working-directory-relative, or data-root-relative path.
 * @param size Receives the byte count on success; may be NULL.
 * @return Heap storage owned by the caller, or NULL when the file cannot be read.
 * Release it with CoreFreeData. An empty file still returns a releasable pointer. */
unsigned char *CoreReadData(const char *path, size_t *size);

/** @brief Release bytes returned by CoreReadData.
 * @param data Bytes to release; NULL is allowed.
 * @return Nothing; the pointer is invalid once this returns. */
void CoreFreeData(void *data);

char *CoreReadFile(const char *path);
void CoreFreeFile(char *text);

/* Saving writes a temporary file beside the destination and renames it over the destination only
   after every byte reached the disk, so a failed or interrupted save leaves the old file intact.
   Write to the returned stream, then pass whether the writing succeeded to CoreAtomicCommit, which
   closes the stream, checks it, and either replaces the destination or deletes the temporary. */
typedef struct CoreAtomicFile
{
    FILE *file;
    char path[1024];
    char temporary[1024];
} CoreAtomicFile;
FILE *CoreAtomicBegin(CoreAtomicFile *atomic, const char *path);
bool CoreAtomicCommit(CoreAtomicFile *atomic, bool ok);
#endif
