#ifndef FBX_LOADER_H
#define FBX_LOADER_H

#include <stddef.h>
#include "ufbx.h"

typedef struct {
    float position[3];
    float uv[2];
    float normal[3];
} FBXVertex;

typedef struct {
    FBXVertex *vertices;
    size_t vertex_count;
} FBXMesh;

typedef struct {
    ufbx_scene *scene;
    ufbx_anim_stack *animation;
    double duration;
    float center[3];
    float scale;
} FBXModel;

int fbx_load(FBXModel *model, const char *path);
int fbx_build_mesh(FBXModel *model, double time, FBXMesh *mesh);
void fbx_free_mesh(FBXMesh *mesh);
void fbx_free(FBXModel *model);

#endif