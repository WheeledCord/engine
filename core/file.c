#include "file.h"
#include <stdio.h>
#include <stdlib.h>
char *CoreReadFile(const char *path)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END))
    {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET))
    {
        fclose(f);
        return NULL;
    }
    char *s = malloc((size_t)size + 1);
    if (!s)
    {
        fclose(f);
        return NULL;
    }
    size_t n = fread(s, 1, (size_t)size, f);
    int failed = ferror(f);
    fclose(f);
    if (failed || n != (size_t)size)
    {
        free(s);
        return NULL;
    }
    s[n] = 0;
    return s;
}
void CoreFreeFile(char *text)
{
    free(text);
}
