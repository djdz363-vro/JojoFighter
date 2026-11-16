# Mixamo FBX -> JoJo Fighter Animation (JFA1)
# Compatible with Blender 2.79's Python API.
#
# HOW TO USE:
# 1. Open Blender 2.79 and import your Mixamo FBX WITH SKIN.
# 2. Select the armature (rig) in Object Mode.
# 3. Open this script in the Text Editor and press Alt+P (Run Script).
# 4. The file character.jfa will be saved beside the .blend file.
#
# IMPORTANT:
# This is an initial exporter prototype. The PSP game's JFA loader must use
# the same binary layout documented below. Test with a small animation first.

import bpy
import struct
import os
from mathutils import Matrix

MAGIC = b'JFA1'
VERSION = 1
MAX_BONES = 64
MAX_INFLUENCES = 4


def write_u32(f, value):
    f.write(struct.pack('<I', int(value)))


def write_f32(f, value):
    f.write(struct.pack('<f', float(value)))


def write_matrix(f, matrix):
    # 4x4 matrix, row-major order
    for row in range(4):
        for col in range(4):
            write_f32(f, matrix[row][col])


def find_rig_and_mesh():
    rigs = [o for o in bpy.data.objects if o.type == 'ARMATURE']
    if len(rigs) != 1:
        raise Exception("Expected exactly one armature, found %d" % len(rigs))

    rig = rigs[0]
    meshes = []
    for obj in bpy.data.objects:
        if obj.type != 'MESH':
            continue
        for modifier in obj.modifiers:
            if modifier.type == 'ARMATURE' and modifier.object == rig:
                meshes.append(obj)
                break

    if len(meshes) != 1:
        raise Exception("Expected exactly one mesh skinned to the armature, found %d" % len(meshes))

    return rig, meshes[0]


def get_bone_data(rig):
    bones = list(rig.data.bones)
    if len(bones) > MAX_BONES:
        raise Exception("Rig has %d bones; limit is %d" % (len(bones), MAX_BONES))

    bone_index = {}
    for i, bone in enumerate(bones):
        bone_index[bone.name] = i

    parents = []
    names = []
    for bone in bones:
        parents.append(bone_index[bone.parent.name] if bone.parent else -1)
        names.append(bone.name.encode('utf-8'))

    return bones, bone_index, parents, names


def build_triangles(mesh_obj, rig, bone_index):
    mesh = mesh_obj.data
    uv_layer = mesh.uv_layers.active
    mesh_to_rig = rig.matrix_world.inverted() * mesh_obj.matrix_world
    normal_matrix = mesh_to_rig.to_3x3().inverted().transposed()

    vertices = []
    weights = []
    indices = []

    # Blender 2.79 does not provide calc_loop_triangles().
    # Split each polygon into a triangle fan.
    for poly in mesh.polygons:
        loop_ids = list(poly.loop_indices)
        if len(loop_ids) < 3:
            continue

        for corner in range(1, len(loop_ids) - 1):
            tri_loops = (loop_ids[0], loop_ids[corner], loop_ids[corner + 1])
            for loop_index in tri_loops:
                loop = mesh.loops[loop_index]
                vertex = mesh.vertices[loop.vertex_index]

                pos = mesh_to_rig * vertex.co
                normal = normal_matrix * poly.normal
                normal.normalize()

                if uv_layer:
                    uv = uv_layer.data[loop_index].uv
                    u, v = uv.x, uv.y
                else:
                    u, v = 0.0, 0.0

                influences = []
                for group_ref in vertex.groups:
                    if group_ref.group >= len(mesh_obj.vertex_groups):
                        continue
                    group_name = mesh_obj.vertex_groups[group_ref.group].name
                    if group_name in bone_index and group_ref.weight > 0.0:
                        influences.append((bone_index[group_name], group_ref.weight))

                influences.sort(key=lambda item: item[1], reverse=True)
                influences = influences[:MAX_INFLUENCES]
                total = sum(item[1] for item in influences)

                if total > 0.0:
                    influences = [(i, w / total) for i, w in influences]
                else:
                    influences = [(0, 1.0)]

                ids = [item[0] for item in influences]
                ws = [item[1] for item in influences]
                while len(ids) < MAX_INFLUENCES:
                    ids.append(0)
                    ws.append(0.0)

                vertices.append((pos.x, pos.y, pos.z,
                                 normal.x, normal.y, normal.z, u, v))
                weights.append((ids, ws))
                indices.append(len(indices))

    return vertices, weights, indices


def export_jfa():
    rig, mesh_obj = find_rig_and_mesh()
    bones, bone_index, parents, names = get_bone_data(rig)

    vertices, weights, indices = build_triangles(mesh_obj, rig, bone_index)

    scene = bpy.context.scene
    action = rig.animation_data.action if rig.animation_data else None
    if action:
        start = int(action.frame_range[0])
        end = int(action.frame_range[1])
    else:
        start = int(scene.frame_start)
        end = int(scene.frame_end)

    if end < start:
        raise Exception("Invalid animation frame range")

    frame_numbers = list(range(start, end + 1))
    fps = scene.render.fps

    blend_path = bpy.data.filepath
    output_dir = os.path.dirname(blend_path) if blend_path else os.path.expanduser("~")
    output_path = os.path.join(output_dir, "character.jfa")

    with open(output_path, 'wb') as f:
        # Header: magic, version, bone count, vertex count, index count,
        # frame count, FPS numerator, FPS denominator.
        f.write(MAGIC)
        write_u32(f, VERSION)
        write_u32(f, len(bones))
        write_u32(f, len(vertices))
        write_u32(f, len(indices))
        write_u32(f, len(frame_numbers))
        write_u32(f, fps)
        write_u32(f, 1)

        # Bone table: parent index, UTF-8 name length, name bytes.
        for i, bone in enumerate(bones):
            write_u32(f, parents[i] if parents[i] >= 0 else 0xFFFFFFFF)
            write_u32(f, len(names[i]))
            f.write(names[i])

        # Vertex data: position(3), normal(3), UV(2), then 4 bone IDs,
        # then 4 weights. All values are 32-bit floats except bone IDs.
        for vertex, influence in zip(vertices, weights):
            for value in vertex:
                write_f32(f, value)
            for bone_id in influence[0]:
                write_u32(f, bone_id)
            for weight in influence[1]:
                write_f32(f, weight)

        # Sequential indices (triangles are already expanded).
        for index in indices:
            write_u32(f, index)

        # Per frame, per bone: deformation matrix =
        # pose bone matrix * inverse of the bone's rest matrix.
        original_frame = scene.frame_current
        try:
            for frame in frame_numbers:
                scene.frame_set(frame)
                bpy.context.scene.update()
                for bone in bones:
                    pose_bone = rig.pose.bones[bone.name]
                    deform = pose_bone.matrix * bone.matrix_local.inverted()
                    write_matrix(f, deform)
        finally:
            scene.frame_set(original_frame)
            bpy.context.scene.update()

    print("JFA export complete: " + output_path)
    print("Bones: %d | Vertices: %d | Frames: %d" %
          (len(bones), len(vertices), len(frame_numbers)))


try:
    export_jfa()
except Exception as error:
    print("JFA EXPORT FAILED: %s" % str(error))
    raise
