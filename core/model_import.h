/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CORE_MODEL_IMPORT_H
#define CORE_MODEL_IMPORT_H
#include "raylib.h"
#include <stdbool.h>

/* Loading a model someone else made, whatever made it, into the units and axes the engine uses:
   metres, Y up, a skeleton with parents before children, and clips sampled into raylib's pose
   format so Actor and the sprite tools can play them.

   FBX and OBJ go through ufbx, which does the unit and axis conversion itself -- including the
   centimetre armature every Mixamo download carries, whose scale a converter that only fixes the
   mesh leaves in the animation curves. glTF, IQM, M3D and VOX go through raylib's own loaders.
   Everything comes out in the same shape either way, so nothing downstream asks which it was. */

typedef struct ModelImportOptions
{
    float sampleFps; /* clips from ufbx are sampled at this rate */
    bool inPlace;    /* cancel the root's steady horizontal travel, keeping its sway */
    bool flipUv;     /* for sources whose texture rows run the other way; FBX is handled already */
} ModelImportOptions;

ModelImportOptions ModelImportDefaults(void);

typedef struct ImportedModel
{
    Model model;
    ModelAnimation *animations;
    float *clipFps; /* the rate each clip's frames were sampled at */
    int animationCount;
    Vector3 boundsMin, boundsMax; /* of the bind pose, after conversion */
    char path[512];
    /* What the import did, in a sentence or two a tool can show: which loader, what it converted,
       what it dropped. */
    char report[512];
    /* Importer-owned override and saved material maps; callers must not free these. */
    Texture2D albedoOverride;
    MaterialMap *originalAlbedo;
} ImportedModel;

/* False with out left empty, and the reason in out->report, when the file cannot be read or has
   nothing drawable. */
bool ModelImport(ImportedModel *out, const char *path, const ModelImportOptions *options);
/* Appends the clips of another file -- a Mixamo "without skin" download, say -- matched onto the
   model's skeleton by bone name. Bones the file does not animate keep their bind pose; bones only
   the file has are ignored. Returns how many clips were added, or -1 when the file cannot be read
   or shares no bones with the model. */
int ModelImportAnimations(ImportedModel *model, const char *path, const ModelImportOptions *options);
void ModelImportFree(ImportedModel *model);
/* Replace every material's base-colour image, retaining original UVs. NULL/empty restores
   imported maps and tints. Failed loads leave the current appearance untouched. The importer
   owns the texture; no rig reload is needed. Paths are resolved as supplied by the caller. */
bool ModelImportSetAlbedo(ImportedModel *model, const char *path);
/* Whether a path names a format ModelImport can read, going by its extension. */
bool ModelImportSupported(const char *path);
#endif
