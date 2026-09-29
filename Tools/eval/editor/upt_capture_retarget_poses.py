"""언리얼 에디터 전용: 리타기팅 전후 애니메이션의 자세를 같은 시각에 뽑아 남긴다.

리타기팅이 동작을 보존했는지 재려면 원본과 결과를 같은 기준으로 비교해야 한다.
체형이 다르므로 위치를 그대로 비교할 수는 없고, 뼈마디 **방향**을 비교해야 한다.
여기서는 각 뼈의 컴포넌트 공간 트랜스폼(부모를 따라 합성한 값)을 남기고, 판단은 eval_retarget.py가 한다.

뼈 이름은 스켈레톤마다 다르다(`thigh_l` / `Thigh_L` / `L-Thigh`). 그래서 뼈 이름을 박아 두지 않고,
`UPT.SkeletonAudit`로 메시마다 **역할 → 뼈 이름**을 받아 와서 역할 기준으로 표본을 남긴다.
남는 JSON의 키는 뼈 이름이 아니라 역할 이름이므로, 명명 규칙이 다른 캐릭터끼리도 그대로 비교된다.

애니메이션을 재생하지 않고 AnimSequence에서 직접 포즈를 읽으므로 시뮬레이트가 필요 없다.

사용(에디터 Output Log의 Cmd 칸에서):
  py "<프로젝트>/Plugins/UniversalProductionTools/Tools/eval/editor/upt_capture_retarget_poses.py"
  py "...upt_capture_retarget_poses.py" <manifest1.json> <manifest2.json> ...

먼저 UPT.RetargetE2E로 리타기팅을 돌려 manifest를 만들어 두어야 한다.

결과: Saved/UniversalProductionTools/GroundTruth/retarget_<시각>/retarget_poses.json
"""
import json
import os
import sys
import time

import unreal

SAMPLES = 24
# 방향을 비교할 뼈마디. 뼈 이름이 아니라 분석기가 붙이는 **역할** 이름으로 적는다.
SEGMENTS = [
    ("LeftThigh", "LeftCalf"), ("LeftCalf", "LeftFoot"),
    ("RightThigh", "RightCalf"), ("RightCalf", "RightFoot"),
    ("LeftUpperArm", "LeftLowerArm"), ("LeftLowerArm", "LeftHand"),
    ("RightUpperArm", "RightLowerArm"), ("RightLowerArm", "RightHand"),
    ("Pelvis", "Spine"), ("Neck", "Head"),
]
# 접지 판정과 크기 정규화에 쓰는 역할
GROUND_BONES = ["LeftFoot", "RightFoot"]
SCALE_CHAIN = ["Pelvis", "LeftThigh", "LeftCalf", "LeftFoot"]


def saved_dir() -> str:
    return unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())


def manifest_path() -> str:
    return os.path.join(saved_dir(), "UniversalProductionTools", "retarget_manifest.json")


def role_map(mesh) -> dict:
    """메시 하나를 분석기에 걸어 역할 → 뼈 이름을 받는다. 스켈레톤마다 명명 규칙이 다르므로 필요하다."""
    path = mesh.get_path_name()
    out = os.path.join(saved_dir(), "UniversalProductionTools", "retarget_roles_tmp.json")
    if os.path.isfile(out):
        os.remove(out)
    unreal.SystemLibrary.execute_console_command(
        unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world(),
        f"UPT.SkeletonAudit -filter={path} -out={out}")
    if not os.path.isfile(out):
        unreal.log_error(f"UPT_RETARGET_POSE 역할 분석 결과가 없습니다: {path}")
        return {}
    with open(out, encoding="utf-8") as handle:
        report = json.load(handle)
    for row in report.get("rows", []):
        if row.get("asset") == path:
            return {entry["role"]: entry["bone"] for entry in row.get("roles", [])}
    unreal.log_error(f"UPT_RETARGET_POSE 분석 결과에서 메시를 찾지 못했습니다: {path}")
    return {}


def required_roles() -> list:
    roles = set()
    for parent, child in SEGMENTS:
        roles.add(parent)
        roles.add(child)
    roles.update(GROUND_BONES)
    roles.update(SCALE_CHAIN)
    return sorted(roles)


def build_parent_map(mesh) -> dict:
    """뼈 이름 → 부모 뼈 이름. 컴포넌트 공간으로 합성할 때 쓴다."""
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = subsystem.spawn_actor_from_class(unreal.SkeletalMeshActor, unreal.Vector(0, 0, -5000), unreal.Rotator())
    try:
        component = actor.get_components_by_class(unreal.SkeletalMeshComponent)[0]
        component.set_skeletal_mesh_asset(mesh)
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


def sample_animation(animation, parents: dict, roles: dict) -> list:
    """샘플 시각마다 필요한 역할의 컴포넌트 공간 위치를 기록한다. 키는 뼈 이름이 아니라 역할이다."""
    length = animation.get_play_length()
    wanted = [(role, roles[role]) for role in required_roles() if role in roles]

    frames = []
    for index in range(SAMPLES):
        seconds = length * index / max(1, SAMPLES - 1)
        cache = {}
        row = {"time": round(seconds, 4), "bones": {}}
        for role, bone in wanted:
            transform = component_space(animation, bone, seconds, parents, cache)
            if transform is None:
                continue
            location = transform.translation
            row["bones"][role] = [round(location.x, 3), round(location.y, 3), round(location.z, 3)]
        frames.append(row)
    return frames


def load_manifest_entries(paths: list) -> list:
    entries = []
    for path in paths:
        if not os.path.isfile(path):
            unreal.log_error(f"UPT_RETARGET_POSE manifest가 없습니다: {path}. 먼저 UPT.RetargetE2E를 돌리세요.")
            continue
        with open(path, encoding="utf-8") as handle:
            entries += json.load(handle).get("pairs", [])
    return entries


def main(argv) -> None:
    entries = load_manifest_entries(argv if argv else [manifest_path()])
    if not entries:
        return

    directory = os.path.join(saved_dir(), "UniversalProductionTools", "GroundTruth",
                             f"retarget_{time.strftime('%Y%m%d_%H%M%S')}")
    os.makedirs(directory, exist_ok=True)

    parent_cache, role_cache = {}, {}
    pairs = []
    for entry in entries:
        source_animation = unreal.load_asset(entry["source_animation"])
        target_animation = unreal.load_asset(entry["retargeted_animation"])
        source_mesh = unreal.load_asset(entry["source_mesh"])
        target_mesh = unreal.load_asset(entry["target_mesh"])
        if not all((source_animation, target_animation, source_mesh, target_mesh)):
            unreal.log_warning(f"UPT_RETARGET_POSE 에셋을 열지 못해 건너뜁니다: {entry.get('source_animation')}")
            continue

        for mesh in (source_mesh, target_mesh):
            key = mesh.get_path_name()
            if key not in parent_cache:
                parent_cache[key] = build_parent_map(mesh)
            if key not in role_cache:
                role_cache[key] = role_map(mesh)

        source_roles = role_cache[source_mesh.get_path_name()]
        target_roles = role_cache[target_mesh.get_path_name()]
        missing = [role for role in required_roles() if role not in source_roles or role not in target_roles]
        if missing:
            unreal.log_warning(
                f"UPT_RETARGET_POSE {source_mesh.get_name()}→{target_mesh.get_name()} 역할 누락으로 건너뜁니다: {', '.join(missing)}")
            continue

        pairs.append({
            "name": f"{source_animation.get_name()} → {target_mesh.get_name()}",
            "source_animation": entry["source_animation"],
            "retargeted_animation": entry["retargeted_animation"],
            "source_mesh": source_mesh.get_name(),
            "target_mesh": target_mesh.get_name(),
            "source_length": round(source_animation.get_play_length(), 4),
            "target_length": round(target_animation.get_play_length(), 4),
            "source_frames": sample_animation(source_animation, parent_cache[source_mesh.get_path_name()], source_roles),
            "target_frames": sample_animation(target_animation, parent_cache[target_mesh.get_path_name()], target_roles),
        })
        unreal.log(f"UPT_RETARGET_POSE {pairs[-1]['name']} 표본 {SAMPLES}개")

    output = {"segments": SEGMENTS, "ground_bones": GROUND_BONES, "scale_chain": SCALE_CHAIN, "pairs": pairs}
    with open(os.path.join(directory, "retarget_poses.json"), "w", encoding="utf-8") as handle:
        json.dump(output, handle, ensure_ascii=False)
    unreal.log(f"UPT_RETARGET_POSE_DONE {len(pairs)}쌍 → {directory}")


if __name__ == "__main__":
    main(sys.argv[1:])
