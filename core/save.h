#ifndef CORE_SAVE_H
#define CORE_SAVE_H
#include <stdbool.h>
#include <stddef.h>
typedef enum CoreSaveType { CORE_SAVE_INT, CORE_SAVE_FLOAT, CORE_SAVE_BOOL, CORE_SAVE_STRING } CoreSaveType;
typedef struct CoreSaveField { const char *name; CoreSaveType type; size_t offset, size; } CoreSaveField;
typedef bool (*CoreSaveMigrateFn)(void *state, int fromVersion, int toVersion, void *user);
bool CoreSaveWrite(const char *path, int version, const void *state, const CoreSaveField *fields, size_t count);
bool CoreSaveRead(const char *path, int version, void *state, const CoreSaveField *fields, size_t count, CoreSaveMigrateFn migrate, void *user);
#endif
