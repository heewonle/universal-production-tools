import argparse
import pathlib
import sys

import bpy


def parse_args():
    values = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--scale", type=float, default=100.0)
    return parser.parse_args(values)


def make_dummy_mesh(armature):
    vertices, faces, names = [], [], []
    for bone in armature.data.bones:
        head, base, radius = bone.head_local, len(vertices), 0.35
        vertices.extend([(head.x - radius, head.y, head.z), (head.x + radius, head.y, head.z), (head.x, head.y, head.z + radius)])
        faces.append((base, base + 1, base + 2))
        names.append(bone.name)
    mesh = bpy.data.meshes.new("SOMA_DummyMesh")
    mesh.from_pydata(vertices, [], faces)
    obj = bpy.data.objects.new("SOMA_DummyMesh", mesh)
    bpy.context.collection.objects.link(obj)
    for index, name in enumerate(names):
        obj.vertex_groups.new(name=name).add([index * 3, index * 3 + 1, index * 3 + 2], 1.0, "REPLACE")
    obj.modifiers.new("Armature", "ARMATURE").object = armature
    obj.parent = armature
    return obj


def main():
    args = parse_args()
    source, target = pathlib.Path(args.input).resolve(), pathlib.Path(args.output).resolve()
    if not source.is_file():
        raise FileNotFoundError(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    bpy.ops.import_anim.bvh(filepath=str(source), global_scale=args.scale, frame_start=0, use_fps_scale=False, update_scene_fps=True)
    armatures = [obj for obj in bpy.context.scene.objects if obj.type == "ARMATURE"]
    if len(armatures) != 1:
        raise RuntimeError(f"Expected one BVH armature, found {len(armatures)}")
    armature = armatures[0]
    armature.name = "SOMA_Armature"
    dummy = make_dummy_mesh(armature)
    bpy.ops.object.select_all(action="DESELECT")
    armature.select_set(True)
    dummy.select_set(True)
    bpy.context.view_layer.objects.active = armature
    bpy.ops.export_scene.fbx(filepath=str(target), use_selection=True, object_types={"ARMATURE", "MESH"}, axis_forward="-Z", axis_up="Y", add_leaf_bones=False, bake_anim=True, bake_anim_use_all_actions=False, bake_anim_use_nla_strips=False, bake_anim_simplify_factor=0.0)
    if not target.is_file():
        raise RuntimeError("FBX export did not create output")
    print(f"UPT_FBX_OUTPUT={target}")


if __name__ == "__main__":
    main()
