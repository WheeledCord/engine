/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CORE_ROOT_CAPACITY 512
#define CORE_ROOT_SEARCH_DEPTH 6

static char s_root[CORE_ROOT_CAPACITY];

static bool IsRegularFile(const char *path)
{
    // Not fopen: opening a directory succeeds on glibc, which would let a directory of the same
    // name shadow the file being looked for.
    struct stat entry;
    return stat(path, &entry) == 0 && S_ISREG(entry.st_mode);
}

static bool IsDirectory(const char *path)
{
    struct stat entry;
    return stat(path, &entry) == 0 && S_ISDIR(entry.st_mode);
}

// A drive letter or a UNC prefix is absolute on Windows, where the leading slash test is not enough.
static bool IsAbsolutePath(const char *path)
{
    if (path[0] == '/' || path[0] == '\\')
        return true;
    return path[0] && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

static bool HasEngineData(const char *directory)
{
    char probe[CORE_ROOT_CAPACITY + 32];
    if ((size_t)snprintf(probe, sizeof probe, "%s/core/shaders", directory) >= sizeof probe)
        return false;
    return IsDirectory(probe);
}

void CoreSetDataRoot(const char *path)
{
    s_root[0] = 0;
    if (!path || !*path)
        return;
    if ((size_t)snprintf(s_root, sizeof s_root, "%s", path) >= sizeof s_root)
    {
        s_root[0] = 0;
        return;
    }
    // The executable does not have to sit at the engine root: a build directory puts it several
    // levels below. Walk up until the engine's own data is in reach, and keep the given path when
    // it is not, so a project that packages the data beside its binary still works.
    for (int level = 0; level < CORE_ROOT_SEARCH_DEPTH; level++)
    {
        if (HasEngineData(s_root))
            return;
        char *slash = strrchr(s_root, '/');
        char *back = strrchr(s_root, '\\');
        if (back > slash)
            slash = back;
        if (!slash || slash == s_root)
            break;
        *slash = 0;
    }
    snprintf(s_root, sizeof s_root, "%s", path);
}

const char *CoreResolvePath(const char *path, char *buf, size_t buflen)
{
    if (!path)
        return NULL;
    if (!s_root[0] || IsAbsolutePath(path) || IsRegularFile(path))
        return path;
    if (!buf || buflen == 0)
        return path;
    // A path that will not fit is a miss, not a silently shortened path that names something else.
    if ((size_t)snprintf(buf, buflen, "%s/%s", s_root, path) >= buflen)
        return NULL;
    return buf;
}

char *CoreReadFile(const char *path)
{
    char resolved[1024];
    const char *actual = CoreResolvePath(path, resolved, sizeof resolved);
    FILE *f = actual ? fopen(actual, "rb") : NULL;
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
