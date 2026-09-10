#ifndef CORE_FILE_H
#define CORE_FILE_H
/* Disk-only, NUL-terminated text. Caller releases with CoreFreeFile.
   CoreSetDataRoot sets a fallback prefix for relative paths that do not
   exist from the working directory; engine.c sets it to GetApplicationDirectory(). */
void CoreSetDataRoot(const char *path);
char *CoreReadFile(const char *path);
void CoreFreeFile(char *text);
#endif
