#ifndef CORE_FILE_H
#define CORE_FILE_H
#include <stdbool.h>
#include <stddef.h>
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
char *CoreReadFile(const char *path);
void CoreFreeFile(char *text);
#endif
