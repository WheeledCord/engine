/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "model_import.h"
#include "config.h"
#include "raymath.h"
#include "rlgl.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ufbx declares its scene through anonymous unions, which C99 only has as an extension. The
   extension is the whole of what is being relied on, so only its diagnostic is silenced, and only
   for the header. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "ufbx.h"
#pragma GCC diagnostic pop

/* raylib's glTF loader samples every clip at this fixed step (GLTF_ANIMDELAY, 17 ms); the clips it
   returns carry no rate of their own. */
#define RAYLIB_GLTF_FPS (1000.0f / 17.0f)
/* Which of a mesh's vertex buffers hold bone ids and weights: raylib's own numbering, from its
   config.h, which rlgl.h only exposes when a program defines RL_SUPPORT_MESH_GPU_SKINNING itself. */
#define MESH_BUFFER_BONE_IDS 7
#define MESH_BUFFER_BONE_WEIGHTS 8

ModelImportOptions ModelImportDefaults(void)
{
    return (ModelImportOptions){.sampleFps = 30, .inPlace = true, .flipUv = false};
}

static const char *Extension(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    return dot ? dot + 1 : "";
}
static bool ExtensionIs(const char *path, const char *ext)
{
    const char *have = Extension(path);
    size_t i = 0;
    for (; have[i] && ext[i]; i++)
        if (tolower((unsigned char)have[i]) != ext[i])
            return false;
    return !have[i] && !ext[i];
}
static bool ViaUfbx(const char *path)
{
    return ExtensionIs(path, "fbx") || ExtensionIs(path, "obj");
}
bool ModelImportSupported(const char *path)
{
    static const char *kinds[] = {"fbx", "obj", "glb", "gltf", "iqm", "m3d", "vox"};
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
        if (ExtensionIs(path, kinds[i]))
            return true;
    return false;
}

static void Report(ImportedModel *m, const char *format, ...)
{
    size_t used = strlen(m->report);
    if (used >= sizeof m->report - 1)
        return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(m->report + used, sizeof m->report - used, format, arguments);
    va_end(arguments);
}

static Matrix FromUfbx(ufbx_matrix m)
{
    /* ufbx stores columns; raylib's Matrix names its elements row by row. */
    return (Matrix){(float)m.m00, (float)m.m01, (float)m.m02, (float)m.m03,
                    (float)m.m10, (float)m.m11, (float)m.m12, (float)m.m13,
                    (float)m.m20, (float)m.m21, (float)m.m22, (float)m.m23,
                    0,            0,            0,            1};
}
static Transform ToTransform(Matrix m)
{
    Transform t = {0};
    MatrixDecompose(m, &t.translation, &t.rotation, &t.scale);
    t.rotation = QuaternionNormalize(t.rotation);
    return t;
}
static Matrix FromTransform(Transform t)
{
    return MatrixMultiply(MatrixMultiply(MatrixScale(t.scale.x, t.scale.y, t.scale.z),
                                         QuaternionToMatrix(t.rotation)),
                          MatrixTranslate(t.translation.x, t.translation.y, t.translation.z));
}

/* ---- textures ---------------------------------------------------------------------------------- */

static bool JoinedFile(char *out, size_t capacity, const char *directory, const char *name)
{
    size_t a = strlen(directory), b = strlen(name);
    if (a + b + 2 > capacity) return false;
    memcpy(out, directory, a);
    out[a] = '/';
    memcpy(out + a + 1, name, b + 1);
    return FileExists(out);
}
static bool FileNamed(char *out, size_t capacity, const char *directory, const char *name)
{
    if (!name || !*name)
        return false;
    char portable[1024];
    if (strlen(name) >= sizeof portable) return false;
    snprintf(portable, sizeof portable, "%s", name);
    for (char *p = portable; *p; p++) if (*p == '\\') *p = '/';
    if (portable[0] == '/' && FileExists(portable))
    {
        if (strlen(portable) >= capacity) return false;
        snprintf(out, capacity, "%s", portable);
        return true;
    }
    if (JoinedFile(out, capacity, directory, portable))
        return true;
    /* Exporters record the path on the artist's machine; the file usually sits beside the model. */
    const char *base = GetFileName(portable);
    return JoinedFile(out, capacity, directory, base);
}
static Texture2D FileTextureFromUfbx(const ufbx_texture *texture, const char *directory)
{
    Texture2D result = {0};
    if (!texture)
        return result;
    if (texture->content.size > 0 && texture->content.size < 0x7fffffff)
    {
        const char *name = texture->filename.length ? texture->filename.data : "embedded.png";
        char kind[16];
        snprintf(kind, sizeof kind, ".%s", Extension(name));
        const unsigned char *bytes = texture->content.data;
        if (texture->content.size >= 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8))
            snprintf(kind, sizeof kind, ".png");
        else if (texture->content.size >= 3 && bytes[0] == 255 && bytes[1] == 216 && bytes[2] == 255)
            snprintf(kind, sizeof kind, ".jpg");
        Image image = LoadImageFromMemory(kind, texture->content.data, (int)texture->content.size);
        if (image.data)
        {
            result = LoadTextureFromImage(image);
            UnloadImage(image);
            return result;
        }
    }
    char path[1024];
    if (FileNamed(path, sizeof path, directory, texture->relative_filename.data) ||
        FileNamed(path, sizeof path, directory, texture->filename.data))
        result = LoadTexture(path);
    return result;
}
static Texture2D TextureFromUfbx(const ufbx_texture *texture, const char *directory)
{
    if (!texture) return (Texture2D){0};
    /* Like Godot modules/fbx/fbx_document.cpp:126-136: unwrap material/shader nodes to
       actual image files. This is first usable image support, not layered shader evaluation. */
    for (size_t i = 0; i < texture->file_textures.count; i++)
    {
        Texture2D image = FileTextureFromUfbx(texture->file_textures.data[i], directory);
        if (image.id) return image;
    }
    return texture->file_textures.count ? (Texture2D){0} : FileTextureFromUfbx(texture, directory);
}
/* A model exported without its texture link is common -- the PSX character in the isometric game is
   one -- and the picture is nearly always beside it named after it. Only then is it guessed. */
static Texture2D TextureBeside(const char *modelPath, char *found, size_t capacity)
{
    Texture2D result = {0};
    const char *directory = GetDirectoryPath(modelPath);
    char stem[256];
    snprintf(stem, sizeof stem, "%s", GetFileNameWithoutExt(modelPath));
    FilePathList files = LoadDirectoryFilesEx(directory, ".png;.jpg;.jpeg;.tga;.bmp", false);
    for (unsigned i = 0; i < files.count && !result.id; i++)
        if (!strncmp(GetFileName(files.paths[i]), stem, strlen(stem)))
        {
            result = LoadTexture(files.paths[i]);
            snprintf(found, capacity, "%s", GetFileName(files.paths[i]));
        }
    UnloadDirectoryFiles(files);
    return result;
}

/* ---- skeletons --------------------------------------------------------------------------------- */

/* Bones nothing is weighted to and nothing weighted hangs from -- Mixamo's *_End markers, most
   finger tips -- cost a shader uniform each and move nothing. They are dropped so that a full
   humanoid fits the engine's bone budget. */
static bool CullBones(Model *model, ModelAnimation *animations, int animationCount, ImportedModel *out)
{
    int count = model->boneCount;
    if (count <= 0)
        return true;
    bool *keep = calloc((size_t)count, sizeof *keep);
    int *map = malloc((size_t)count * sizeof *map);
    if (!keep || !map)
    {
        free(keep);
        free(map);
        return false;
    }
    for (int m = 0; m < model->meshCount; m++)
    {
        const Mesh *mesh = &model->meshes[m];
        if (!mesh->boneIds || !mesh->boneWeights)
            continue;
        for (int i = 0; i < mesh->vertexCount * 4; i++)
            if (mesh->boneWeights[i] > 0 && mesh->boneIds[i] < count)
                keep[mesh->boneIds[i]] = true;
    }
    /* Anything a kept bone hangs from is kept too, or it would lose the motion it inherits. */
    for (int b = 0; b < count; b++)
        if (keep[b])
            for (int p = model->bones[b].parent; p >= 0 && p < count && !keep[p]; p = model->bones[p].parent)
                keep[p] = true;
    int kept = 0;
    for (int b = 0; b < count; b++)
        map[b] = keep[b] ? kept++ : -1;
    if (kept == 0)
        kept = 1, map[0] = 0, keep[0] = true;
    if (kept > CORE_BONE_CAPACITY)
    {
        Report(out, "It has %d weighted bones; the engine skins at most %d. ", kept, CORE_BONE_CAPACITY);
        free(keep);
        free(map);
        return false;
    }
    if (kept < count)
    {
        for (int b = 0, to = 0; b < count; b++)
        {
            if (!keep[b])
                continue;
            BoneInfo bone = model->bones[b];
            while (bone.parent >= 0 && !keep[bone.parent])
                bone.parent = model->bones[bone.parent].parent;
            bone.parent = bone.parent >= 0 ? map[bone.parent] : -1;
            model->bones[to] = bone;
            model->bindPose[to] = model->bindPose[b];
            for (int a = 0; a < animationCount; a++)
                for (int f = 0; f < animations[a].frameCount; f++)
                    animations[a].framePoses[f][to] = animations[a].framePoses[f][b];
            to++;
        }
        for (int a = 0; a < animationCount; a++)
        {
            animations[a].boneCount = kept;
            for (int b = 0; b < kept; b++)
                animations[a].bones[b] = model->bones[b];
        }
        for (int m = 0; m < model->meshCount; m++)
        {
            Mesh *mesh = &model->meshes[m];
            if (mesh->boneIds)
            {
                for (int i = 0; i < mesh->vertexCount * 4; i++)
                {
                    int id = mesh->boneIds[i] < count ? map[mesh->boneIds[i]] : -1;
                    mesh->boneIds[i] = (unsigned char)(id >= 0 ? id : 0);
                    if (id < 0 && mesh->boneWeights)
                        mesh->boneWeights[i] = 0;
                }
                /* Sent again: the ids on the GPU still name the old numbering. */
                if (mesh->vboId && mesh->vboId[MESH_BUFFER_BONE_IDS])
                    UpdateMeshBuffer(*mesh, MESH_BUFFER_BONE_IDS, mesh->boneIds,
                                     mesh->vertexCount * 4 * (int)sizeof(unsigned char), 0);
                if (mesh->boneWeights && mesh->vboId &&
                    mesh->vboId[MESH_BUFFER_BONE_WEIGHTS])
                    UpdateMeshBuffer(*mesh, MESH_BUFFER_BONE_WEIGHTS,
                                     mesh->boneWeights, mesh->vertexCount * 4 * (int)sizeof(float), 0);
            }
            if (mesh->boneCount > kept)
                mesh->boneCount = kept;
        }
        model->boneCount = kept;
        Report(out, "Dropped %d unweighted end bones. ", count - kept);
    }
    free(keep);
    free(map);
    return true;
}

/* A walk exported with its travel moves the whole rig forward every cycle, and a sprite of it walks
   out of its own frame. The root's steady drift across the clip is taken out; its sway within the
   clip stays. */
static void CancelTravel(ModelAnimation *animation, Vector3 travel, ImportedModel *out)
{
    if (animation->frameCount < 2 || sqrtf(travel.x * travel.x + travel.z * travel.z) < 0.05f)
        return;
    for (int f = 0; f < animation->frameCount; f++)
    {
        float t = (float)f / (float)(animation->frameCount - 1);
        for (int b = 0; b < animation->boneCount; b++)
        {
            animation->framePoses[f][b].translation.x -= travel.x * t;
            animation->framePoses[f][b].translation.z -= travel.z * t;
        }
    }
    Report(out, "Held \"%s\" in place (it travelled %.2f m). ", animation->name,
           sqrtf(travel.x * travel.x + travel.z * travel.z));
}
/* Travel of a clip sampled with its closing frame: the drift from the first frame to the last. */
static Vector3 ClipTravel(const ModelAnimation *animation)
{
    if (animation->frameCount < 2 || animation->boneCount < 1)
        return (Vector3){0};
    int best = 0;
    float most = -1;
    for (int b = 0; b < animation->boneCount; b++)
    {
        Vector3 a = animation->framePoses[0][b].translation;
        Vector3 z = animation->framePoses[animation->frameCount - 1][b].translation;
        float d = (z.x - a.x) * (z.x - a.x) + (z.z - a.z) * (z.z - a.z);
        if (d > most)
            most = d, best = b;
    }
    Vector3 a = animation->framePoses[0][best].translation;
    Vector3 z = animation->framePoses[animation->frameCount - 1][best].translation;
    return (Vector3){z.x - a.x, 0, z.z - a.z};
}

static bool AllocateAnimation(ModelAnimation *animation, const char *name, const Model *model, int frames)
{
    *animation = (ModelAnimation){0};
    snprintf(animation->name, sizeof animation->name, "%s", name && *name ? name : "clip");
    animation->boneCount = model->boneCount;
    animation->frameCount = frames;
    animation->bones = RL_CALLOC((size_t)model->boneCount, sizeof(BoneInfo));
    animation->framePoses = RL_CALLOC((size_t)frames, sizeof(Transform *));
    if (!animation->bones || !animation->framePoses)
        return false;
    memcpy(animation->bones, model->bones, (size_t)model->boneCount * sizeof(BoneInfo));
    for (int f = 0; f < frames; f++)
        if (!(animation->framePoses[f] = RL_CALLOC((size_t)model->boneCount, sizeof(Transform))))
            return false;
    return true;
}
static bool GrowAnimations(ImportedModel *m, int more)
{
    ModelAnimation *animations = RL_REALLOC(m->animations, (size_t)(m->animationCount + more) * sizeof *animations);
    if (!animations)
        return false;
    m->animations = animations;
    float *rates = realloc(m->clipFps, (size_t)(m->animationCount + more) * sizeof *rates);
    if (!rates)
        return false;
    m->clipFps = rates;
    return true;
}

/* ---- ufbx scenes ------------------------------------------------------------------------------- */

static ufbx_scene *LoadUfbx(const char *path, ImportedModel *out)
{
    ufbx_load_opts options = {0};
    options.target_axes = ufbx_axes_right_handed_y_up;
    options.target_unit_meters = 1.0f;
    /* Scale goes into the geometry and the curves both, rather than onto a root node that a later
       step could forget: that forgotten root is what used to shrink Mixamo characters to a speck. */
    options.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    options.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    options.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    options.generate_missing_normals = true;
    options.clean_skin_weights = true;
    options.ignore_missing_external_files = true;
    ufbx_error error;
    ufbx_scene *scene = ufbx_load_file(path, &options, &error);
    if (!scene)
        Report(out, "Could not read %s: %s. ", GetFileName(path), error.description.data);
    return scene;
}

/* Every node that bends a vertex, and every node those hang from, in an order where parents come
   first. Returns how many; map[node typed_id] is the bone index or -1. */
static int CollectBones(const ufbx_scene *scene, int *map, const ufbx_node **order)
{
    size_t nodes = scene->nodes.count;
    bool *used = calloc(nodes, sizeof *used);
    if (!used)
        return -1;
    for (size_t i = 0; i < scene->skin_clusters.count; i++)
        for (const ufbx_node *n = scene->skin_clusters.data[i]->bone_node; n && !n->is_root; n = n->parent)
            used[n->typed_id] = true;
    int count = 0;
    /* Depth first from the root, so a parent is always numbered before its children. */
    const ufbx_node **stack = malloc(nodes * sizeof *stack);
    if (!stack)
    {
        free(used);
        return -1;
    }
    size_t top = 0;
    stack[top++] = scene->root_node;
    for (size_t i = 0; i < nodes; i++)
        map[i] = -1;
    while (top)
    {
        const ufbx_node *n = stack[--top];
        if (!n->is_root && used[n->typed_id])
        {
            map[n->typed_id] = count;
            order[count++] = n;
        }
        for (size_t c = n->children.count; c-- > 0;)
            stack[top++] = n->children.data[c];
    }
    free(stack);
    free(used);
    return count;
}

static int ParentBone(const ufbx_node *node, const int *map)
{
    for (const ufbx_node *p = node->parent; p && !p->is_root; p = p->parent)
        if (map[p->typed_id] >= 0)
            return map[p->typed_id];
    return -1;
}

typedef struct Soup
{
    float *positions, *normals, *uvs, *weights;
    unsigned char *ids;
    int count, capacity;
} Soup;
static bool SoupGrow(Soup *s, int more, bool skinned)
{
    if (s->count + more <= s->capacity)
        return true;
    int capacity = s->capacity ? s->capacity * 2 : 1024;
    while (capacity < s->count + more)
        capacity *= 2;
    float *p = RL_REALLOC(s->positions, (size_t)capacity * 3 * sizeof(float));
    if (p)
        s->positions = p;
    float *n = RL_REALLOC(s->normals, (size_t)capacity * 3 * sizeof(float));
    if (n)
        s->normals = n;
    float *t = RL_REALLOC(s->uvs, (size_t)capacity * 2 * sizeof(float));
    if (t)
        s->uvs = t;
    if (!p || !n || !t)
        return false;
    if (skinned)
    {
        float *w = RL_REALLOC(s->weights, (size_t)capacity * 4 * sizeof(float));
        if (w)
            s->weights = w;
        unsigned char *i = RL_REALLOC(s->ids, (size_t)capacity * 4);
        if (i)
            s->ids = i;
        if (!w || !i)
            return false;
    }
    s->capacity = capacity;
    return true;
}

typedef struct Builder
{
    Mesh *meshes;
    int *meshMaterial;
    int count, capacity;
} Builder;
static bool AddMesh(Builder *b, Soup *soup, int material, int boneCount)
{
    if (soup->count == 0)
        return true;
    if (b->count == b->capacity)
    {
        int capacity = b->capacity ? b->capacity * 2 : 8;
        Mesh *meshes = RL_REALLOC(b->meshes, (size_t)capacity * sizeof *meshes);
        if (meshes)
            b->meshes = meshes;
        int *materials = RL_REALLOC(b->meshMaterial, (size_t)capacity * sizeof *materials);
        if (materials)
            b->meshMaterial = materials;
        if (!meshes || !materials)
            return false;
        b->capacity = capacity;
    }
    Mesh mesh = {0};
    mesh.vertexCount = soup->count;
    mesh.triangleCount = soup->count / 3;
    mesh.vertices = soup->positions;
    mesh.normals = soup->normals;
    mesh.texcoords = soup->uvs;
    mesh.boneIds = soup->ids;
    mesh.boneWeights = soup->weights;
    if (boneCount > 0)
    {
        mesh.boneCount = boneCount;
        mesh.boneMatrices = RL_CALLOC((size_t)boneCount, sizeof(Matrix));
        if (!mesh.boneMatrices)
            return false;
        for (int i = 0; i < boneCount; i++)
            mesh.boneMatrices[i] = MatrixIdentity();
    }
    UploadMesh(&mesh, false);
    b->meshes[b->count] = mesh;
    b->meshMaterial[b->count] = material;
    b->count++;
    *soup = (Soup){0};
    return true;
}

/* One node's mesh, split by material, as flat triangle lists in model space. */
static bool BuildNodeMesh(Builder *builder, const ufbx_node *node, const int *bones, int boneCount,
                          const ufbx_scene *scene, int materialBase)
{
    const ufbx_mesh *mesh = node->mesh;
    Matrix toModel = FromUfbx(node->geometry_to_world);
    Matrix normalMatrix = FromUfbx(ufbx_matrix_for_normals(&node->geometry_to_world));
    const ufbx_skin_deformer *skin = mesh->skin_deformers.count ? mesh->skin_deformers.data[0] : NULL;
    int rigid = boneCount > 0 ? ParentBone(node, bones) : -1;
    if (boneCount > 0 && rigid < 0 && node->typed_id < scene->nodes.count && bones[node->typed_id] >= 0)
        rigid = bones[node->typed_id];
    if (rigid < 0)
        rigid = 0; /* nothing better to follow: the top of the skeleton */
    uint32_t triangles[3 * 64];
    size_t parts = mesh->materials.count ? mesh->materials.count : 1;
    for (size_t part = 0; part < parts; part++)
    {
        Soup soup = {0};
        for (size_t f = 0; f < mesh->faces.count; f++)
        {
            if (mesh->materials.count && mesh->face_material.count &&
                mesh->face_material.data[f] != part)
                continue;
            ufbx_face face = mesh->faces.data[f];
            if (face.num_indices < 3 || face.num_indices > 64)
                continue;
            uint32_t made = ufbx_triangulate_face(triangles, sizeof triangles / sizeof *triangles, mesh, face);
            if (!SoupGrow(&soup, (int)made * 3, boneCount > 0))
                return false;
            for (uint32_t t = 0; t < made * 3; t++)
            {
                uint32_t index = triangles[t];
                int at = soup.count++;
                ufbx_vec3 p = ufbx_get_vertex_vec3(&mesh->vertex_position, index);
                Vector3 position = Vector3Transform((Vector3){(float)p.x, (float)p.y, (float)p.z}, toModel);
                memcpy(&soup.positions[at * 3], &position, sizeof position);
                Vector3 normal = {0, 1, 0};
                if (mesh->vertex_normal.exists)
                {
                    ufbx_vec3 n = ufbx_get_vertex_vec3(&mesh->vertex_normal, index);
                    normal = Vector3Normalize(Vector3Transform((Vector3){(float)n.x, (float)n.y, (float)n.z},
                                                               normalMatrix));
                }
                memcpy(&soup.normals[at * 3], &normal, sizeof normal);
                float u = 0, v = 0;
                if (mesh->vertex_uv.exists)
                {
                    ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh->vertex_uv, index);
                    u = (float)uv.x;
                    v = 1.0f - (float)uv.y; /* FBX rows run bottom-up, textures top-down */
                }
                soup.uvs[at * 2] = u;
                soup.uvs[at * 2 + 1] = v;
                if (boneCount > 0)
                {
                    unsigned char *ids = &soup.ids[at * 4];
                    float *weights = &soup.weights[at * 4];
                    memset(ids, 0, 4);
                    memset(weights, 0, 4 * sizeof(float));
                    float total = 0;
                    int used = 0;
                    if (skin)
                    {
                        uint32_t vertex = mesh->vertex_indices.data[index];
                        if (vertex < skin->vertices.count)
                        {
                            ufbx_skin_vertex sv = skin->vertices.data[vertex];
                            for (uint32_t w = 0; w < sv.num_weights && used < 4; w++)
                            {
                                ufbx_skin_weight weight = skin->weights.data[sv.weight_begin + w];
                                const ufbx_skin_cluster *cluster = skin->clusters.data[weight.cluster_index];
                                int bone = cluster->bone_node ? bones[cluster->bone_node->typed_id] : -1;
                                if (bone < 0 || weight.weight <= 0)
                                    continue;
                                ids[used] = (unsigned char)bone;
                                weights[used] = (float)weight.weight;
                                total += (float)weight.weight;
                                used++;
                            }
                        }
                    }
                    if (total <= 0)
                    {
                        ids[0] = (unsigned char)rigid;
                        weights[0] = 1;
                    }
                    else
                        for (int w = 0; w < used; w++)
                            weights[w] /= total;
                }
            }
        }
        int material = materialBase;
        if (mesh->materials.count)
        {
            const ufbx_material *which = mesh->materials.data[part];
            material = materialBase + (int)which->typed_id;
        }
        if (!AddMesh(builder, &soup, material, boneCount))
            return false;
    }
    return true;
}

/* A clip, as raylib wants it: the model-space pose of every bone at each sampled frame. */
static bool SampleStack(const ufbx_scene *scene, const ufbx_anim_stack *stack, const ufbx_node **order,
                        const int *parent, int boneCount, const Model *model, float fps,
                        ModelAnimation *out)
{
    double begin = stack->time_begin, end = stack->time_end;
    int intervals = (int)lround((end - begin) * fps);
    int frames = intervals > 0 ? intervals + 1 : 1;
    if (!AllocateAnimation(out, stack->name.data, model, frames))
        return false;
    Matrix *global = malloc((size_t)boneCount * sizeof *global);
    if (!global)
        return false;
    Matrix rootWorld = FromUfbx(scene->root_node->node_to_world);
    for (int f = 0; f < frames; f++)
    {
        double time = intervals > 0 ? begin + (end - begin) * ((double)f / intervals) : begin;
        for (int b = 0; b < boneCount; b++)
        {
            ufbx_transform local = ufbx_evaluate_transform(stack->anim, order[b], time);
            Matrix m = FromUfbx(ufbx_transform_to_matrix(&local));
            Matrix above = rootWorld;
            if (parent[b] >= 0)
                above = global[parent[b]];
            else
                /* Hanging from something that bends nothing: carry what that something's rest place is. */
                for (const ufbx_node *p = order[b]->parent; p && !p->is_root; p = p->parent)
                {
                    above = FromUfbx(p->node_to_world);
                    break;
                }
            global[b] = MatrixMultiply(m, above);
            out->framePoses[f][b] = ToTransform(global[b]);
        }
    }
    free(global);
    return true;
}

static bool ImportUfbx(ImportedModel *out, const char *path, const ModelImportOptions *options)
{
    ufbx_scene *scene = LoadUfbx(path, out);
    if (!scene)
        return false;
    size_t nodes = scene->nodes.count;
    int *bones = malloc(nodes * sizeof *bones);
    const ufbx_node **order = malloc(nodes * sizeof *order);
    int *parent = malloc(nodes * sizeof *parent);
    Builder builder = {0};
    bool ok = bones && order && parent;
    int boneCount = ok ? CollectBones(scene, bones, order) : -1;
    ok = ok && boneCount >= 0;
    Model *model = &out->model;
    *model = (Model){0};
    model->transform = MatrixIdentity();
    if (ok && boneCount > 0)
    {
        model->boneCount = boneCount;
        model->bones = RL_CALLOC((size_t)boneCount, sizeof(BoneInfo));
        model->bindPose = RL_CALLOC((size_t)boneCount, sizeof(Transform));
        ok = model->bones && model->bindPose;
        for (int b = 0; ok && b < boneCount; b++)
        {
            parent[b] = ParentBone(order[b], bones);
            snprintf(model->bones[b].name, sizeof model->bones[b].name, "%s", order[b]->name.data);
            model->bones[b].parent = parent[b];
            model->bindPose[b] = ToTransform(FromUfbx(order[b]->node_to_world));
        }
        /* Where a skin says a bone was when it was bound, that wins over wherever the file happens
           to have left the bone: the two differ whenever a rig was saved mid-animation. */
        for (size_t s = 0; ok && s < scene->skin_deformers.count; s++)
        {
            const ufbx_skin_deformer *skin = scene->skin_deformers.data[s];
            const ufbx_node *owner = NULL;
            for (size_t n = 0; n < nodes && !owner; n++)
                if (scene->nodes.data[n]->mesh)
                    for (size_t d = 0; d < scene->nodes.data[n]->mesh->skin_deformers.count; d++)
                        if (scene->nodes.data[n]->mesh->skin_deformers.data[d] == skin)
                            owner = scene->nodes.data[n];
            if (!owner)
                continue;
            for (size_t c = 0; c < skin->clusters.count; c++)
            {
                const ufbx_skin_cluster *cluster = skin->clusters.data[c];
                if (!cluster->bone_node || bones[cluster->bone_node->typed_id] < 0)
                    continue;
                ufbx_matrix boneToGeometry = ufbx_matrix_invert(&cluster->geometry_to_bone);
                ufbx_matrix bind = ufbx_matrix_mul(&owner->geometry_to_world, &boneToGeometry);
                model->bindPose[bones[cluster->bone_node->typed_id]] = ToTransform(FromUfbx(bind));
            }
        }
    }
    /* Materials first, so meshes can name them. Slot 0 is a plain white fallback. */
    int materials = (int)scene->materials.count + 1;
    if (ok)
    {
        model->materialCount = materials;
        model->materials = RL_CALLOC((size_t)materials, sizeof(Material));
        ok = model->materials != NULL;
        const char *directory = GetDirectoryPath(path);
        int textured = 0;
        for (int i = 0; ok && i < materials; i++)
        {
            model->materials[i] = LoadMaterialDefault();
            if (i == 0)
                continue;
            const ufbx_material *material = scene->materials.data[i - 1];
            const ufbx_texture *texture = material->pbr.base_color.texture ? material->pbr.base_color.texture
                                                                          : material->fbx.diffuse_color.texture;
            Texture2D image = TextureFromUfbx(texture, directory);
            if (image.id)
            {
                model->materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = image;
                textured++;
            }
            else if (material->fbx.diffuse_color.has_value)
            {
                ufbx_vec4 c = material->fbx.diffuse_color.value_vec4;
                model->materials[i].maps[MATERIAL_MAP_DIFFUSE].color =
                    (Color){(unsigned char)Clamp((float)c.x * 255, 0, 255), (unsigned char)Clamp((float)c.y * 255, 0, 255),
                            (unsigned char)Clamp((float)c.z * 255, 0, 255), 255};
            }
        }
        if (ok && textured == 0)
        {
            char found[256] = "";
            Texture2D guess = TextureBeside(path, found, sizeof found);
            if (guess.id)
            {
                for (int i = 0; i < materials; i++)
                    model->materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = guess;
                Report(out, "The file names no texture; used %s beside it. ", found);
            }
        }
    }
    for (size_t n = 0; ok && n < nodes; n++)
    {
        const ufbx_node *node = scene->nodes.data[n];
        if (node->mesh && node->visible)
            ok = BuildNodeMesh(&builder, node, bones, boneCount, scene, 1);
    }
    if (ok && builder.count == 0)
    {
        Report(out, "It holds no meshes. ");
        ok = false;
    }
    if (ok)
    {
        /* Materials the file did not assign start at 1 because slot 0 is the fallback; meshes with
           no material of their own take the fallback. */
        for (int i = 0; i < builder.count; i++)
            if (builder.meshMaterial[i] >= materials)
                builder.meshMaterial[i] = 0;
        model->meshes = builder.meshes;
        model->meshMaterial = builder.meshMaterial;
        model->meshCount = builder.count;
        builder = (Builder){0};
    }
    int stacks = ok && boneCount > 0 ? (int)scene->anim_stacks.count : 0;
    if (ok && stacks > 0 && GrowAnimations(out, stacks))
        for (int s = 0; s < stacks && ok; s++)
        {
            ModelAnimation *animation = &out->animations[out->animationCount];
            ok = SampleStack(scene, scene->anim_stacks.data[s], order, parent, boneCount, model,
                             options->sampleFps, animation);
            if (ok && stacks == 1)
                snprintf(animation->name, sizeof animation->name, "%s", GetFileNameWithoutExt(path));
            if (ok)
            {
                out->clipFps[out->animationCount] = options->sampleFps;
                out->animationCount++;
            }
            else
                UnloadModelAnimation(*animation);
        }
    if (scene->settings.unit_meters > 0)
        Report(out, "%s via ufbx, converted from %.3g m units to metres, Y up: ", Extension(path),
               scene->settings.unit_meters);
    else
        Report(out, "%s via ufbx, which states no units; read as metres: ", Extension(path));
    Report(out, "%d mesh(es), %d bone(s), %d clip(s). ", model->meshCount, model->boneCount, out->animationCount);
    for (int i = 0; i < builder.count; i++)
        UnloadMesh(builder.meshes[i]);
    RL_FREE(builder.meshes);
    RL_FREE(builder.meshMaterial);
    free(bones);
    free(order);
    free(parent);
    ufbx_free_scene(scene);
    return ok;
}

/* ---- animation from other files ---------------------------------------------------------------- */

/* Bone names across exports of the same rig differ in their namespace -- mixamorig:Hips,
   mixamorig1:Hips, Armature|Hips -- and nothing else. */
static const char *BareName(const char *name)
{
    const char *best = name;
    for (const char *c = name; *c; c++)
        if (*c == ':' || *c == '|')
            best = c + 1;
    return best;
}
static bool SameBone(const char *a, const char *b)
{
    if (!strcmp(a, b))
        return true;
    a = BareName(a);
    b = BareName(b);
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
    return !*a && !*b;
}

static Matrix *BindLocals(const Model *model)
{
    Matrix *local = malloc((size_t)model->boneCount * sizeof *local);
    if (!local)
        return NULL;
    for (int b = 0; b < model->boneCount; b++)
    {
        Matrix global = FromTransform(model->bindPose[b]);
        int p = model->bones[b].parent;
        local[b] = p >= 0 ? MatrixMultiply(global, MatrixInvert(FromTransform(model->bindPose[p]))) : global;
    }
    return local;
}
/* Poses the target skeleton from the source's local rotations. Lengths come from the target's own
   bind pose, so a clip made on a taller rig does not stretch this one. The topmost animated bone also
   moves -- that is where a walk's bounce lives -- but by how far it moves from the source's own rest
   place, not to the source's absolute place: two exports of one rig can disagree about where the
   root stands at rest, and copying that across lifts or sinks the whole character. */
static void Retarget(const Model *target, const Matrix *bindLocal, const int *source, const Matrix *sourceLocal,
                     const Matrix *sourceRest, Transform *pose)
{
    Matrix *global = malloc((size_t)target->boneCount * sizeof *global);
    if (!global)
        return;
    for (int b = 0; b < target->boneCount; b++)
    {
        int p = target->bones[b].parent;
        Matrix local = bindLocal[b];
        if (source[b] >= 0)
        {
            Transform animated = ToTransform(sourceLocal[source[b]]);
            Transform rest = ToTransform(bindLocal[b]);
            bool topmost = true;
            for (int q = p; q >= 0; q = target->bones[q].parent)
                if (source[q] >= 0)
                    topmost = false;
            if (!topmost)
                animated.translation = rest.translation;
            else if (sourceRest)
            {
                Vector3 home = ToTransform(sourceRest[source[b]]).translation;
                animated.translation = Vector3Add(rest.translation, Vector3Subtract(animated.translation, home));
            }
            animated.scale = rest.scale;
            local = FromTransform(animated);
        }
        global[b] = p >= 0 ? MatrixMultiply(local, global[p]) : local;
        pose[b] = ToTransform(global[b]);
    }
    free(global);
}

int ModelImportAnimations(ImportedModel *m, const char *path, const ModelImportOptions *options)
{
    if (!m || m->model.boneCount < 1 || !path)
        return -1;
    ModelImportOptions defaults = ModelImportDefaults();
    if (!options)
        options = &defaults;
    int before = m->animationCount;
    Matrix *bindLocal = BindLocals(&m->model);
    int *source = malloc((size_t)m->model.boneCount * sizeof *source);
    if (!bindLocal || !source)
    {
        free(bindLocal);
        free(source);
        return -1;
    }
    int matched = 0;
    if (ViaUfbx(path))
    {
        ufbx_scene *scene = LoadUfbx(path, m);
        if (!scene)
        {
            free(bindLocal);
            free(source);
            return -1;
        }
        const ufbx_node **nodes = malloc(scene->nodes.count * sizeof *nodes);
        Matrix *local = malloc(scene->nodes.count * sizeof *local);
        Matrix *rest = malloc(scene->nodes.count * sizeof *rest);
        for (size_t n = 0; rest && n < scene->nodes.count; n++)
            rest[n] = FromUfbx(ufbx_transform_to_matrix(&scene->nodes.data[n]->local_transform));
        for (int b = 0; b < m->model.boneCount; b++)
        {
            source[b] = -1;
            for (size_t n = 0; nodes && n < scene->nodes.count && source[b] < 0; n++)
                if (!scene->nodes.data[n]->is_root && SameBone(m->model.bones[b].name, scene->nodes.data[n]->name.data))
                {
                    source[b] = (int)n;
                    matched++;
                }
        }
        if (nodes && local && matched > 0 && GrowAnimations(m, (int)scene->anim_stacks.count))
            for (size_t s = 0; s < scene->anim_stacks.count; s++)
            {
                const ufbx_anim_stack *stack = scene->anim_stacks.data[s];
                int intervals = (int)lround((stack->time_end - stack->time_begin) * options->sampleFps);
                int frames = intervals > 0 ? intervals + 1 : 1;
                ModelAnimation *animation = &m->animations[m->animationCount];
                /* Mixamo names every clip "mixamo.com" and other exporters "Take 001"; a file holding
                   one clip is better known by the file's own name. */
                const char *name = scene->anim_stacks.count == 1 || !stack->name.length ? GetFileNameWithoutExt(path)
                                                                                      : stack->name.data;
                if (!AllocateAnimation(animation, name, &m->model, frames))
                {
                    UnloadModelAnimation(*animation);
                    break;
                }
                for (int f = 0; f < frames; f++)
                {
                    double time = intervals > 0 ? stack->time_begin +
                                                  (stack->time_end - stack->time_begin) * ((double)f / intervals)
                                                : stack->time_begin;
                    for (size_t n = 0; n < scene->nodes.count; n++)
                    {
                        ufbx_transform t = ufbx_evaluate_transform(stack->anim, scene->nodes.data[n], time);
                        local[n] = FromUfbx(ufbx_transform_to_matrix(&t));
                    }
                    Retarget(&m->model, bindLocal, source, local, rest, animation->framePoses[f]);
                }
                m->clipFps[m->animationCount] = options->sampleFps;
                m->animationCount++;
            }
        free(nodes);
        free(local);
        free(rest);
        ufbx_free_scene(scene);
    }
    else
    {
        Model other = LoadModel(path);
        int count = 0;
        ModelAnimation *clips = LoadModelAnimations(path, &count);
        Matrix *local = other.boneCount > 0 ? malloc((size_t)other.boneCount * sizeof *local) : NULL;
        Matrix *rest = other.boneCount > 0 ? BindLocals(&other) : NULL;
        for (int b = 0; b < m->model.boneCount; b++)
        {
            source[b] = -1;
            for (int o = 0; o < other.boneCount && source[b] < 0; o++)
                if (SameBone(m->model.bones[b].name, other.bones[o].name))
                {
                    source[b] = o;
                    matched++;
                }
        }
        if (local && matched > 0 && count > 0 && GrowAnimations(m, count))
            for (int c = 0; c < count; c++)
            {
                ModelAnimation *animation = &m->animations[m->animationCount];
                if (!AllocateAnimation(animation, clips[c].name, &m->model, clips[c].frameCount))
                {
                    UnloadModelAnimation(*animation);
                    break;
                }
                for (int f = 0; f < clips[c].frameCount; f++)
                {
                    for (int o = 0; o < other.boneCount && o < clips[c].boneCount; o++)
                    {
                        Matrix g = FromTransform(clips[c].framePoses[f][o]);
                        int p = other.bones[o].parent;
                        local[o] = p >= 0 ? MatrixMultiply(g, MatrixInvert(FromTransform(clips[c].framePoses[f][p]))) : g;
                    }
                    Retarget(&m->model, bindLocal, source, local, rest, animation->framePoses[f]);
                }
                m->clipFps[m->animationCount] = RAYLIB_GLTF_FPS;
                m->animationCount++;
            }
        free(local);
        free(rest);
        if (clips)
            UnloadModelAnimations(clips, count);
        if (other.meshCount)
            UnloadModel(other);
    }
    free(bindLocal);
    free(source);
    if (matched == 0)
    {
        Report(m, "%s shares no bone names with the model. ", GetFileName(path));
        return -1;
    }
    for (int a = before; a < m->animationCount; a++)
        if (options->inPlace)
            CancelTravel(&m->animations[a], ClipTravel(&m->animations[a]), m);
    Report(m, "Added %d clip(s) from %s (%d bones matched). ", m->animationCount - before, GetFileName(path),
           matched);
    return m->animationCount - before;
}

/* ---- the entry point --------------------------------------------------------------------------- */

static void Bounds(ImportedModel *m)
{
    Vector3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < m->model.meshCount; i++)
        for (int v = 0; v < m->model.meshes[i].vertexCount; v++)
        {
            const float *p = &m->model.meshes[i].vertices[v * 3];
            lo = Vector3Min(lo, (Vector3){p[0], p[1], p[2]});
            hi = Vector3Max(hi, (Vector3){p[0], p[1], p[2]});
        }
    if (lo.x > hi.x)
        lo = hi = (Vector3){0};
    m->boundsMin = lo;
    m->boundsMax = hi;
}

bool ModelImport(ImportedModel *out, const char *path, const ModelImportOptions *options)
{
    *out = (ImportedModel){0};
    ModelImportOptions defaults = ModelImportDefaults();
    if (!options)
        options = &defaults;
    snprintf(out->path, sizeof out->path, "%s", path ? path : "");
    if (!path || !FileExists(path))
    {
        Report(out, "There is no file at %s. ", path ? path : "(nothing)");
        return false;
    }
    if (!ModelImportSupported(path))
    {
        Report(out, "%s is not a format the importer reads (fbx, obj, glb, gltf, iqm, m3d, vox). ",
               GetFileName(path));
        return false;
    }
    bool ok;
    if (ViaUfbx(path))
        ok = ImportUfbx(out, path, options);
    else
    {
        out->model = LoadModel(path);
        ok = out->model.meshCount > 0;
        if (ok && out->model.boneCount > 0)
        {
            int count = 0;
            ModelAnimation *clips = LoadModelAnimations(path, &count);
            if (clips && count > 0)
            {
                out->animations = clips;
                out->animationCount = count;
                out->clipFps = malloc((size_t)count * sizeof *out->clipFps);
                for (int i = 0; out->clipFps && i < count; i++)
                    out->clipFps[i] = RAYLIB_GLTF_FPS;
            }
        }
        Report(out, "%s via raylib: %d mesh(es), %d bone(s), %d clip(s). ", Extension(path),
               out->model.meshCount, out->model.boneCount, out->animationCount);
        if (!ok)
            Report(out, "It holds no meshes. ");
    }
    if (ok && options->flipUv)
        for (int i = 0; i < out->model.meshCount; i++)
        {
            Mesh *mesh = &out->model.meshes[i];
            for (int v = 0; mesh->texcoords && v < mesh->vertexCount; v++)
                mesh->texcoords[v * 2 + 1] = 1.0f - mesh->texcoords[v * 2 + 1];
            if (mesh->texcoords && mesh->vboId && mesh->vboId[RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD])
                UpdateMeshBuffer(*mesh, RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD, mesh->texcoords,
                                 mesh->vertexCount * 2 * (int)sizeof(float), 0);
        }
    if (ok)
        ok = CullBones(&out->model, out->animations, out->animationCount, out);
    if (ok && options->inPlace)
        for (int a = 0; a < out->animationCount; a++)
            CancelTravel(&out->animations[a], ClipTravel(&out->animations[a]), out);
    if (!ok)
    {
        char report[sizeof out->report];
        memcpy(report, out->report, sizeof report);
        ModelImportFree(out);
        memcpy(out->report, report, sizeof report);
        return false;
    }
    int textured = 0;
    for (int i = 0; i < out->model.materialCount; i++)
    {
        Texture2D t = out->model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture;
        if (t.id && t.id != rlGetTextureIdDefault()) textured++;
    }
    Report(out, "%d material(s) with base-colour textures. ", textured);
    Bounds(out);
    Report(out, "Bind pose %.2f x %.2f x %.2f m.", out->boundsMax.x - out->boundsMin.x,
           out->boundsMax.y - out->boundsMin.y, out->boundsMax.z - out->boundsMin.z);
    return true;
}

bool ModelImportSetAlbedo(ImportedModel *m, const char *path)
{
    if (!m || m->model.materialCount <= 0 || !m->model.materials) return false;
    Texture2D replacement = {0};
    if (path && *path)
    {
        if (!FileExists(path)) return false;
        replacement = LoadTexture(path);
        if (!replacement.id) return false;
        SetTextureFilter(replacement, TEXTURE_FILTER_POINT);
        if (!m->originalAlbedo)
        {
            m->originalAlbedo = malloc((size_t)m->model.materialCount * sizeof *m->originalAlbedo);
            if (!m->originalAlbedo) { UnloadTexture(replacement); return false; }
            for (int i = 0; i < m->model.materialCount; i++)
                m->originalAlbedo[i] = m->model.materials[i].maps[MATERIAL_MAP_DIFFUSE];
        }
    }
    for (int i = 0; i < m->model.materialCount; i++)
    {
        MaterialMap *map = &m->model.materials[i].maps[MATERIAL_MAP_DIFFUSE];
        if (replacement.id) { map->texture = replacement; map->color = WHITE; }
        else if (m->originalAlbedo) *map = m->originalAlbedo[i];
    }
    if (m->albedoOverride.id) UnloadTexture(m->albedoOverride);
    m->albedoOverride = replacement;
    if (!replacement.id) { free(m->originalAlbedo); m->originalAlbedo = NULL; }
    return true;
}

void ModelImportFree(ImportedModel *m)
{
    if (!m)
        return;
    ModelImportSetAlbedo(m, NULL);
    if (m->animations)
        UnloadModelAnimations(m->animations, m->animationCount);
    free(m->clipFps);
    /* raylib leaves textures to their owner, and several materials may share one. */
    unsigned int fallback = rlGetTextureIdDefault();
    for (int i = 0; i < m->model.materialCount; i++)
    {
        Texture2D t = m->model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture;
        if (!t.id || t.id == fallback)
            continue;
        UnloadTexture(t);
        for (int j = i; j < m->model.materialCount; j++)
            if (m->model.materials[j].maps[MATERIAL_MAP_DIFFUSE].texture.id == t.id)
                m->model.materials[j].maps[MATERIAL_MAP_DIFFUSE].texture.id = 0;
    }
    if (m->model.meshCount || m->model.materialCount)
        UnloadModel(m->model);
    *m = (ImportedModel){0};
}
