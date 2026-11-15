
#include "fbx_loader.h"

#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int fbx_load(FBXModel *model, const char *path)
{
    ufbx_error error;
    memset(model, 0, sizeof(*model));

    ufbx_load_opts options = {0};
    options.generate_missing_normals = true;

    model->scene = ufbx_load_file(path, &options, &error);

    if (!model->scene) {
        char message[512];
        ufbx_format_error(message, sizeof(message), &error);
        SDL_Log("FBX load failed: %s", message);
        return 0;
    }

    if (model->scene->meshes.count == 0) {
        SDL_Log("FBX contains no mesh");
        fbx_free(model);
        return 0;
    }

    if (model->scene->anim_stacks.count > 0) {
        model->animation = model->scene->anim_stacks.data[0];
        model->duration =
            model->animation->time_end -
            model->animation->time_begin;
    }

    SDL_Log(
        "FBX loaded: %u meshes, animation %.2f seconds",
        (unsigned)model->scene->meshes.count,
        model->duration
    );

    return 1;
}

int fbx_build_mesh(
    FBXModel *model,
    double time,
    FBXMesh *output)
{
    if (!model || !model->scene || !output)
        return 0;

    memset(output, 0, sizeof(*output));

    ufbx_scene *scene = model->scene;
    ufbx_scene *evaluated = NULL;
    uint32_t *indices = NULL;

    if (model->animation && model->duration > 0.0) {
        double local_time = fmod(time, model->duration);
        if (local_time < 0.0)
            local_time += model->duration;

        ufbx_evaluate_opts options = {0};
        options.evaluate_skinning = true;

        ufbx_error error;

        evaluated = ufbx_evaluate_scene(
            model->scene,
            model->animation->anim,
            model->animation->time_begin + local_time,
            &options,
            &error
        );

        if (!evaluated) {
            SDL_Log("FBX animation evaluation failed");
            return 0;
        }

        scene = evaluated;
    }

    if (scene->meshes.count == 0)
        goto fail;

    ufbx_mesh *source = scene->meshes.data[0];

    if (source->num_triangles == 0 ||
        source->max_face_triangles == 0)
        goto fail;

    if (source->instances.count == 0)
        goto fail;

    ufbx_node *node = source->instances.data[0];

    size_t capacity = source->num_triangles * 3;

    output->vertices = malloc(capacity * sizeof(FBXVertex));

    if (!output->vertices)
        goto fail;

    indices = malloc(
        source->max_face_triangles * 3 * sizeof(uint32_t)
    );

    if (!indices)
        goto fail;

    float minimum[3] = {
        1e30f, 1e30f, 1e30f
    };

    float maximum[3] = {
        -1e30f, -1e30f, -1e30f
    };

    for (size_t f = 0; f < source->faces.count; f++) {
        ufbx_face face = source->faces.data[f];

        if (face.num_indices < 3)
            continue;

        uint32_t triangle_count = ufbx_triangulate_face(
            indices,
            source->max_face_triangles * 3,
            source,
            face
        );

        for (uint32_t t = 0; t < triangle_count; t++) {
            for (int k = 0; k < 3; k++) {
                uint32_t corner = indices[t * 3 + k];

                if (output->vertex_count >= capacity)
                    goto fail;

                ufbx_vec3 p;

                if (source->skinned_position.exists) {
                    p = ufbx_get_vertex_vec3(
                        &source->skinned_position,
                        corner
                    );
                } else {
                    p = ufbx_get_vertex_vec3(
                        &source->vertex_position,
                        corner
                    );
                }

                if (source->skinned_is_local) {
                    p = ufbx_transform_position(
                        &node->geometry_to_world,
                        p
                    );
                }

                ufbx_vec3 n;

                if (source->skinned_normal.exists) {
                    n = ufbx_get_vertex_vec3(
                        &source->skinned_normal,
                        corner
                    );
                } else {
                    n = ufbx_get_vertex_vec3(
                        &source->vertex_normal,
                        corner
                    );
                }

                if (source->skinned_is_local) {
                    n = ufbx_transform_direction(
                        &node->geometry_to_world,
                        n
                    );
                }

                float normal_length = sqrtf(
                    n.x * n.x +
                    n.y * n.y +
                    n.z * n.z
                );

                if (normal_length > 0.00001f) {
                    n.x /= normal_length;
                    n.y /= normal_length;
                    n.z /= normal_length;
                }

                ufbx_vec2 uv = {0};

                if (source->vertex_uv.exists) {
                    uv = ufbx_get_vertex_vec2(
                        &source->vertex_uv,
                        corner
                    );
                }

                FBXVertex *v =
                    &output->vertices[output->vertex_count++];

                v->position[0] = (float)p.x;
                v->position[1] = (float)p.y;
                v->position[2] = (float)p.z;

                v->normal[0] = (float)n.x;
                v->normal[1] = (float)n.y;
                v->normal[2] = (float)n.z;

                v->uv[0] = (float)uv.x;
                v->uv[1] = 1.0f - (float)uv.y;

                for (int j = 0; j < 3; j++) {
                    if (v->position[j] < minimum[j])
                        minimum[j] = v->position[j];

                    if (v->position[j] > maximum[j])
                        maximum[j] = v->position[j];
                }
            }
        }
    }

    free(indices);
    indices = NULL;

    if (output->vertex_count == 0)
        goto fail;

    /* Calculate character scale only on the first frame. */
    if (model->scale <= 0.0f) {
        float extent = fmaxf(
            maximum[0] - minimum[0],
            fmaxf(
                maximum[1] - minimum[1],
                maximum[2] - minimum[2]
            )
        );

        if (extent < 0.0001f)
            goto fail;

        for (int j = 0; j < 3; j++) {
            model->center[j] =
                (minimum[j] + maximum[j]) * 0.5f;
        }

        model->scale = 2.2f / extent;
    }

    for (size_t i = 0; i < output->vertex_count; i++) {
        FBXVertex *v = &output->vertices[i];

        for (int j = 0; j < 3; j++) {
            v->position[j] =
                (v->position[j] - model->center[j]) *
                model->scale;
        }
    }

    if (evaluated)
        ufbx_free_scene(evaluated);

    return 1;

fail:
    if (indices)
        free(indices);

    fbx_free_mesh(output);

    if (evaluated)
        ufbx_free_scene(evaluated);

    return 0;
}

void fbx_free_mesh(FBXMesh *mesh)
{
    if (!mesh)
        return;

    free(mesh->vertices);
    memset(mesh, 0, sizeof(*mesh));
}

void fbx_free(FBXModel *model)
{
    if (!model)
        return;

    if (model->scene)
        ufbx_free_scene(model->scene);

    memset(model, 0, sizeof(*model));
}
