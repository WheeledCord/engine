#ifndef CORE_FILE_H
#define CORE_FILE_H
#include <stddef.h>
/* Disk-only, NUL-terminated text. Caller releases with CoreFreeFile.
   CoreSetDataRoot sets a fallback prefix for relative paths that do not
   exist from the working directory; engine.c sets it to GetApplicationDirectory(). */
void CoreSetDataRoot(const char *path);
const char *CoreResolvePath(const char *path, char *buf, size_t buflen); /* buf must outlive use */
char *CoreReadFile(const char *path);
void CoreFreeFile(char *text);
#endif
