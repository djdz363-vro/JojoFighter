
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

    size_t data_size = 0;
void *data = SDL_LoadFile(path, &data_size);

if (!data) {
    SDL_Log("FBX file read failed: %s", SDL_GetError());
    return 0;
}

model->scene = ufbx_load_memory(
    data,
    data_size,
    &options,
    &error
);

SDL_free(data);

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
    for (size_t m = 0; m < model->scene->meshes.count; m++) {
        ufbx_mesh *mesh = model->scene->meshes.data[m];

        float umin = 1e30f, umax = -1e30f;
        float vmin = 1e30f, vmax = -1e30f;

        if (mesh->vertex_uv.exists) {
            for (size_t i = 0; i < mesh->vertex_uv.values.count; i++) {
                ufbx_vec2 uv = mesh->vertex_uv.values.data[i];

                if (uv.x < umin) umin = (float)uv.x;
                if (uv.x > umax) umax = (float)uv.x;
                if (uv.y < vmin) vmin = (float)uv.y;
                if (uv.y > vmax) vmax = (float)uv.y;
            }
        }
        SDL_Log(
            "MESH %u '%s': verts=%u tris=%u UV=%d U[%.2f..%.2f] V[%.2f..%.2f] skins=%u",
            (unsigned)m,
            mesh->name.data ? mesh->name.data : "",
            (unsigned)mesh->num_vertices,
            (unsigned)mesh->num_triangles,
            (int)mesh->vertex_uv.exists,
            umin, umax, vmin, vmax,
            (unsigned)mesh->skin_deformers.count
        );
    }

    for (size_t k = 0; k < model->scene->materials.count; k++) {
        ufbx_material *mat = model->scene->materials.data[k];
        ufbx_texture *tex = mat->fbx.diffuse_color.texture;

        SDL_Log(
            "MATERIAL %u '%s' texture: %s",
            (unsigned)k,
            mat->name.data ? mat->name.data : "",
            tex && tex->filename.data ? tex->filename.data : "(none)"
        );
    }
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

    static Uint64 acc_eval = 0, acc_total = 0;
    static int acc_n = 0;

    Uint64 t_start = SDL_GetPerformanceCounter();
    Uint64 t_eval_end = t_start;

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
        t_eval_end = SDL_GetPerformanceCounter();
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

    acc_eval += t_eval_end - t_start;
    acc_total += SDL_GetPerformanceCounter() - t_start;

    if (++acc_n == 60) {
        double f = (double)SDL_GetPerformanceFrequency() / 1000.0;
        SDL_Log("PERF evaluate %.2f ms, total build %.2f ms",
                acc_eval / f / 60.0, acc_total / f / 60.0);

        acc_eval = acc_total = 0;
        acc_n = 0;
    }

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
