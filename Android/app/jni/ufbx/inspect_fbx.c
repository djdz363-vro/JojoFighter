#include <stdio.h>
#include "ufbx.h"

static void inspect_file(const char *path)
{
    ufbx_error error;
    ufbx_scene *scene = ufbx_load_file(path, NULL, &error);

    printf("\n========== %s ==========\n", path);

    if (!scene) {
        printf("FAILED TO LOAD FILE\n");
        return;
    }

    printf("Nodes: %zu\n", scene->nodes.count);
    printf("Meshes: %zu\n", scene->meshes.count);
    printf("Bones: %zu\n", scene->bones.count);
    printf("Skin deformers: %zu\n", scene->skin_deformers.count);
    printf("Animation stacks: %zu\n", scene->anim_stacks.count);

    for (size_t i = 0; i < scene->meshes.count; i++) {
        ufbx_mesh *mesh = scene->meshes.data[i];

        printf("\nMesh %zu:\n", i);
        printf("  Vertices: %zu\n", mesh->num_vertices);
        printf("  Triangles: %zu\n", mesh->num_triangles);
        printf("  UV available: %s\n",
            mesh->vertex_uv.exists ? "yes" : "no");
        printf("  Normals available: %s\n",
            mesh->vertex_normal.exists ? "yes" : "no");
        printf("  Skin deformers: %zu\n", mesh->skin_deformers.count);

        for (size_t j = 0; j < mesh->skin_deformers.count; j++) {
            ufbx_skin_deformer *skin = mesh->skin_deformers.data[j];
            printf("  Skin %zu: %zu clusters, max %zu weights/vertex\n",
                j, skin->clusters.count, skin->max_weights_per_vertex);
        }
    }

    for (size_t i = 0; i < scene->anim_stacks.count; i++) {
        ufbx_anim_stack *anim = scene->anim_stacks.data[i];

        printf("\nAnimation %zu: %.*s\n",
            i, (int)anim->name.length, anim->name.data);
        printf("  Start: %.3f seconds\n", anim->time_begin);
        printf("  End: %.3f seconds\n", anim->time_end);
        printf("  Duration: %.3f seconds\n",
            anim->time_end - anim->time_begin);
    }

    ufbx_free_scene(scene);
}

int main(void)
{
    inspect_file("Android/app/src/main/assets/model0.fbx");
    inspect_file("Android/app/src/main/assets/Mma Idle.fbx");
    return 0;
}
