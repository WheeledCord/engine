#ifndef CORE_INPUT_MAP_H
#define CORE_INPUT_MAP_H
#include "input.h"
#include "ui.h"
#define CORE_INPUT_ACTION_NAME 48
#define CORE_INPUT_ACTION_BINDINGS 4
typedef struct InputMapAction { char name[CORE_INPUT_ACTION_NAME]; InputBinding defaults[CORE_INPUT_ACTION_BINDINGS], bindings[CORE_INPUT_ACTION_BINDINGS]; size_t count; } InputMapAction;
typedef struct InputMap { InputMapAction *actions; size_t count; } InputMap;
typedef struct InputMapDefinition { const char *name; const InputBinding *bindings; size_t count; } InputMapDefinition;
typedef struct InputMapRebind { const char *action; size_t binding; bool active; } InputMapRebind;
bool InputMapInit(InputMap *map, const InputMapDefinition *definitions, size_t count);
void InputMapFree(InputMap *map);
InputAction InputMapActionGet(const InputMap *map, const char *name);
bool InputMapSet(InputMap *map, const char *name, size_t binding, InputBinding value, const char **conflict);
bool InputMapReset(InputMap *map, const char *name);
bool InputMapWrite(const InputMap *map, const char *path);
bool InputMapRead(InputMap *map, const char *path);
bool InputMapCapture(const EngineInput *input, InputBinding *binding);
bool InputMapRebindButton(UiContext *ui, UiRect rect, InputMap *map, InputMapRebind *rebind, const EngineInput *input);
#endif
