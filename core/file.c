#include "file.h"
#include <stdio.h>
#include <stdlib.h>
static char s_root[512];

void CoreSetDataRoot(const char *path)
{
    if (path && *path)
        snprintf(s_root, sizeof s_root, "%s", path);
    else
        s_root[0] = 0;
}

char *CoreReadFile(const char *path)
{
    if (!path)
        return NULL;
    FILE *f = fopen(path, "rb");
    if (!f && s_root[0] && path[0] != '/')
    {
        char buf[1024];
        snprintf(buf, sizeof buf, "%s/%s", s_root, path);
        f = fopen(buf, "rb");
    }
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
