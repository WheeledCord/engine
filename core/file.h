#ifndef CORE_FILE_H
#define CORE_FILE_H
/* Disk-only, NUL-terminated text. Caller releases with CoreFreeFile. */
char *CoreReadFile(const char *path);
void CoreFreeFile(char *text);
#endif
