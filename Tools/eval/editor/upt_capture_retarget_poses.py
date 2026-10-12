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

# 손가락. 분석기의 역할 표에는 손가락이 없다(1,691개 감사의 준비도 계산을 건드리지 않기 위해).
# 대신 손 역할 본의 자손을 이름 토큰으로 갈라 **손가락 정체성**으로 대응시킨다. 이름 규칙이
# 달라도(`index_01_l` / `L_Index1` / `f_index_01_l`) 토큰은 같으므로 역할과 같은 역할을 한다.
FINGER_TOKENS = ["thumb", "index", "middle", "ring", ["pinky", "little"]]
FINGER_NAMES = ["Thumb", "Index", "Middle", "Ring", "Pinky"]
# 손가락마다 첫 두 관절의 방향을 본다. 손가락이 굽었는지는 이 마디가 말해 준다.
FINGER_SEGMENTS = [(f"{side}{name}1", f"{side}{name}2")
                   for side in ("Left", "Right") for name in FINGER_NAMES]
# 트위스트 본은 팔·다리 축을 따라 **구르는** 본이라 위치로는 거의 아무것도 알 수 없다.
# 대신 로컬 회전이 샷 동안 얼마나 움직였는지(범위)를 재고, 그 본이 매달린 역할로 대응시킨다.
TWIST_TOKENS = ["twist", "roll", "torsion"]


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


def descendants(parents: dict, root: str) -> list:
    """root 아래의 모든 본. (본 이름, 깊이) 순서로 돌려준다."""
    children = {}
    for bone, parent in parents.items():
        children.setdefault(parent, []).append(bone)
    out, stack = [], [(root, 0)]
    while stack:
        bone, depth = stack.pop()
        for child in children.get(bone, []):
            out.append((child, depth + 1))
            stack.append((child, depth + 1))
    return out


def finger_roles(parents: dict, roles: dict) -> dict:
    """손 역할 본의 자손을 손가락 토큰으로 갈라 `LeftIndex1` 같은 역할 이름을 붙인다.

    토큰이 같은 자손들을 계층 깊이로 줄 세워 앞의 두 개를 1·2로 쓴다. 뼈 개수가 달라도
    (관절 3개짜리 / 4개짜리) 앞의 두 관절은 같은 의미이므로 대응이 어긋나지 않는다.
    """
    out = {}
    for side in ("Left", "Right"):
        hand = roles.get(f"{side}Hand")
        if not hand or hand not in parents:
            continue
        found = descendants(parents, hand)
        for tokens, name in zip(FINGER_TOKENS, FINGER_NAMES):
            keys = tokens if isinstance(tokens, list) else [tokens]
            # 중수골(metacarpal)은 세지 않는다. Manny에는 있고 Synty 원본에는 없어 그대로 세면
            # 관절 번호가 한 칸 밀린다. C++ 분석기(AddFingerRoles)와 같은 규칙이어야 한다.
            chain = sorted((depth, bone) for bone, depth in found
                           if any(token in bone.lower() for token in keys)
                           and "metacarpal" not in bone.lower() and "carpus" not in bone.lower())
            for order, (_, bone) in enumerate(chain[:2], start=1):
                out[f"{side}{name}{order}"] = bone
    return out


def twist_bones(parents: dict, roles: dict) -> dict:
    """트위스트 본을 **매달린 역할**로 키를 잡아 모은다.

    이름은 스켈레톤마다 다르지만(`lowerarm_twist_01_l` / `L_Forearm_Roll`) 어느 역할 아래에
    있는지는 같다. 가장 가까운 조상 역할을 키로 쓰면 대응이 생긴다.
    """
    bone_to_role = {bone: role for role, bone in roles.items()}
    out = {}
    for bone in parents:
        if not any(token in bone.lower() for token in TWIST_TOKENS):
            continue
        walk, guard = parents.get(bone, ""), 0
        while walk and guard < 16:
            if walk in bone_to_role:
                out.setdefault(bone_to_role[walk], []).append(bone)
                break
            walk = parents.get(walk, "")
            guard += 1
    return {role: sorted(bones) for role, bones in out.items()}


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


def sample_animation(animation, parents: dict, roles: dict, fingers: dict, twists: dict) -> list:
    """샘플 시각마다 필요한 역할의 컴포넌트 공간 위치를 기록한다. 키는 뼈 이름이 아니라 역할이다.

    손가락은 같은 방식으로 위치를 남긴다. 트위스트는 위치로는 알 수 없는 본이라
    **로컬 회전**을 남긴다(쿼터니언 네 값). 비교는 eval_retarget.py가 한다.
    """
    length = animation.get_play_length()
    wanted = [(role, roles[role]) for role in required_roles() if role in roles]
    wanted += [(role, bone) for role, bone in sorted(fingers.items())]

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
        if twists:
            local = {}
            for role, bones in sorted(twists.items()):
                for bone in bones:
                    transform = unreal.AnimationLibrary.get_bone_pose_for_time(animation, bone, seconds, False)
                    if transform is None:
                        continue
                    q = transform.rotation
                    # 역할 아래에 트위스트 본이 여럿이면(twist_01, twist_02) 첫 번째만 쓴다.
                    local.setdefault(role, [round(q.x, 5), round(q.y, 5), round(q.z, 5), round(q.w, 5)])
            if local:
                row["twist"] = local
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
        source_parents = parent_cache[source_mesh.get_path_name()]
        target_parents = parent_cache[target_mesh.get_path_name()]
        source_fingers = finger_roles(source_parents, source_roles)
        target_fingers = finger_roles(target_parents, target_roles)
        source_twists = twist_bones(source_parents, source_roles)
        target_twists = twist_bones(target_parents, target_roles)
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
            "source_frames": sample_animation(source_animation, source_parents, source_roles, source_fingers, source_twists),
            "target_frames": sample_animation(target_animation, target_parents, target_roles, target_fingers, target_twists),
            # 어떤 손가락·트위스트가 양쪽에 다 있었는지. 없는 쪽은 비교에서 빠진다.
            "finger_roles": sorted(set(source_fingers) & set(target_fingers)),
            "twist_roles": sorted(set(source_twists) & set(target_twists)),
        })
        unreal.log(f"UPT_RETARGET_POSE {pairs[-1]['name']} 표본 {SAMPLES}개")

    output = {"segments": SEGMENTS, "finger_segments": FINGER_SEGMENTS,
              "ground_bones": GROUND_BONES, "scale_chain": SCALE_CHAIN, "pairs": pairs}
    with open(os.path.join(directory, "retarget_poses.json"), "w", encoding="utf-8") as handle:
        json.dump(output, handle, ensure_ascii=False)
    unreal.log(f"UPT_RETARGET_POSE_DONE {len(pairs)}쌍 → {directory}")


if __name__ == "__main__":
    main(sys.argv[1:])
