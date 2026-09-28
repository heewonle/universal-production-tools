"""언리얼 에디터 전용: 리타기팅 전후 애니메이션의 자세를 같은 시각에 뽑아 남긴다.

리타기팅이 동작을 보존했는지 재려면 원본과 결과를 같은 기준으로 비교해야 한다.
체형이 다르므로 위치를 그대로 비교할 수는 없고, 뼈마디 **방향**을 비교해야 한다.
여기서는 각 뼈의 컴포넌트 공간 트랜스폼(부모를 따라 합성한 값)을 남기고, 판단은 eval_retarget.py가 한다.

애니메이션을 재생하지 않고 AnimSequence에서 직접 포즈를 읽으므로 시뮬레이트가 필요 없다.

사용(에디터 Output Log의 Cmd 칸에서):
  py "<프로젝트>/Plugins/UniversalProductionTools/Tools/eval/editor/upt_capture_retarget_poses.py"

먼저 UPT.RetargetE2E로 리타기팅을 돌려 manifest를 만들어 두어야 한다.

결과: Saved/UniversalProductionTools/GroundTruth/retarget_<시각>/retarget_poses.json
"""
import json
import os
import sys
import time

import unreal

SAMPLES = 24
# 방향을 비교할 뼈마디. (부모 뼈, 자식 뼈) 쌍이며 UE 표준 명칭을 따른다.
SEGMENTS = [
    ("thigh_l", "calf_l"), ("calf_l", "foot_l"),
    ("thigh_r", "calf_r"), ("calf_r", "foot_r"),
    ("upperarm_l", "lowerarm_l"), ("lowerarm_l", "hand_l"),
    ("upperarm_r", "lowerarm_r"), ("lowerarm_r", "hand_r"),
    ("pelvis", "spine_01"), ("neck_01", "head"),
]
# 접지 판정과 크기 정규화에 쓰는 뼈
GROUND_BONES = ["foot_l", "foot_r"]
SCALE_CHAIN = ["pelvis", "thigh_l", "calf_l", "foot_l"]


def manifest_path() -> str:
    saved = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
    return os.path.join(saved, "UniversalProductionTools", "retarget_manifest.json")


def build_parent_map(mesh) -> dict:
    """뼈 이름 → 부모 뼈 이름. 컴포넌트 공간으로 합성할 때 쓴다."""
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = subsystem.spawn_actor_from_class(unreal.SkeletalMeshActor, unreal.Vector(0, 0, -5000), unreal.Rotator())
    try:
        component = actor.get_components_by_class(unreal.SkeletalMeshComponent)[0]
        component.set_skeletal_mesh_asset(mesh)
        names = [str(name) for name in component.get_all_socket_names()] if False else None
        parents = {}
        for index in range(component.get_num_bones()):
            bone = str(component.get_bone_name(index))
            parent = component.get_parent_bone(bone)
            parents[bone] = str(parent) if parent and str(parent) else ""
        return parents
    finally:
        actor.destroy_actor()


def component_space(animation, bone: str, seconds: float, parents: dict, cache: dict):
    """부모를 따라 로컬 트랜스폼을 합성해 컴포넌트 공간 트랜스폼을 만든다."""
    key = (bone, seconds)
    if key in cache:
        return cache[key]
    if bone not in parents:
        return None
    local = unreal.AnimationLibrary.get_bone_pose_for_time(animation, bone, seconds, False)
    parent = parents.get(bone, "")
    if not parent:
        result = local
    else:
        parent_transform = component_space(animation, parent, seconds, parents, cache)
        result = local.multiply(parent_transform) if parent_transform else local
    cache[key] = result
    return result


def sample_animation(animation, parents: dict) -> list:
    """샘플 시각마다 필요한 뼈의 컴포넌트 공간 위치를 기록한다."""
    length = animation.get_play_length()
    bones = set()
    for parent, child in SEGMENTS:
        bones.add(parent)
        bones.add(child)
    bones.update(GROUND_BONES)
    bones.update(SCALE_CHAIN)

    frames = []
    for index in range(SAMPLES):
        seconds = length * index / max(1, SAMPLES - 1)
        cache = {}
        row = {"time": round(seconds, 4), "bones": {}}
        for bone in sorted(bones):
            transform = component_space(animation, bone, seconds, parents, cache)
            if transform is None:
                continue
            location = transform.translation
            row["bones"][bone] = [round(location.x, 3), round(location.y, 3), round(location.z, 3)]
        frames.append(row)
    return frames


def main(argv) -> None:
    path = argv[0] if argv else manifest_path()
    if not os.path.isfile(path):
        unreal.log_error(f"UPT_RETARGET_POSE manifest가 없습니다: {path}. 먼저 UPT.RetargetE2E를 돌리세요.")
        return
    with open(path, encoding="utf-8") as handle:
        manifest = json.load(handle)

    saved = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
    directory = os.path.join(saved, "UniversalProductionTools", "GroundTruth", f"retarget_{time.strftime('%Y%m%d_%H%M%S')}")
    os.makedirs(directory, exist_ok=True)

    parent_cache = {}
    pairs = []
    for entry in manifest.get("pairs", []):
        source_animation = unreal.load_asset(entry["source_animation"])
        target_animation = unreal.load_asset(entry["retargeted_animation"])
        source_mesh = unreal.load_asset(entry["source_mesh"])
        target_mesh = unreal.load_asset(entry["target_mesh"])
        if not all((source_animation, target_animation, source_mesh, target_mesh)):
            unreal.log_warning(f"UPT_RETARGET_POSE 에셋을 열지 못해 건너뜁니다: {entry.get('source_animation')}")
            continue
        for mesh in (source_mesh, target_mesh):
            if mesh.get_path_name() not in parent_cache:
                parent_cache[mesh.get_path_name()] = build_parent_map(mesh)

        pairs.append({
            "name": source_animation.get_name(),
            "source_animation": entry["source_animation"],
            "retargeted_animation": entry["retargeted_animation"],
            "source_length": round(source_animation.get_play_length(), 4),
            "target_length": round(target_animation.get_play_length(), 4),
            "source_frames": sample_animation(source_animation, parent_cache[source_mesh.get_path_name()]),
            "target_frames": sample_animation(target_animation, parent_cache[target_mesh.get_path_name()]),
        })
        unreal.log(f"UPT_RETARGET_POSE {source_animation.get_name()} 표본 {SAMPLES}개")

    output = {"segments": SEGMENTS, "ground_bones": GROUND_BONES, "scale_chain": SCALE_CHAIN, "pairs": pairs}
    with open(os.path.join(directory, "retarget_poses.json"), "w", encoding="utf-8") as handle:
        json.dump(output, handle, ensure_ascii=False)
    unreal.log(f"UPT_RETARGET_POSE_DONE {len(pairs)}쌍 → {directory}")


if __name__ == "__main__":
    main(sys.argv[1:])
