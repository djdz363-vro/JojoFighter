
#include <stdio.h>
#include "ufbx.h"

static void inspect_animation(const char *path)
{
    ufbx_error error;
    ufbx_scene *scene = ufbx_load_file(path, NULL, &error);

    printf("\n========== ANIMATION TEST: %s ==========\n", path);

    if (!scene) {
        printf("FAILED TO LOAD FILE\n");
        return;
    }

    if (scene->anim_stacks.count == 0 || scene->meshes.count == 0) {
        printf("Missing animation or mesh\n");
        ufbx_free_scene(scene);
        return;
    }

    ufbx_anim_stack *stack = scene->anim_stacks.data[0];

    printf("Animation duration: %.3f seconds\n",
        stack->time_end - stack->time_begin);

    ufbx_evaluate_opts opts = {0};
    opts.evaluate_skinning = true;

    double times[] = {0.0, 2.5, 4.9};

    for (size_t i = 0; i < 3; i++) {
        ufbx_scene *evaluated = ufbx_evaluate_scene(
            scene, stack->anim, times[i], &opts, &error);

        if (!evaluated) {
            printf("Evaluation failed at %.2f seconds\n", times[i]);
            continue;
        }

        ufbx_mesh *mesh = evaluated->meshes.data[0];

        printf("\nTime: %.2f seconds\n", times[i]);
        printf("Skinned positions available: %s\n",
            mesh->skinned_position.exists ? "yes" : "no");

        if (mesh->skinned_position.exists) {
            ufbx_vec3 p = ufbx_get_vertex_vec3(
                &mesh->skinned_position, 0);

            printf("Vertex 0: X=%.5f Y=%.5f Z=%.5f\n",
                p.x, p.y, p.z);
        }

        ufbx_free_scene(evaluated);
    }

    ufbx_free_scene(scene);
}

int main(void)
{
    inspect_animation("Android/app/src/main/assets/Mma Idle.fbx");
    return 0;
}
