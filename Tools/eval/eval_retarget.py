"""리타기팅이 원본 동작을 보존했는지 잰다.

정답: 원본 애니메이션의 자세. 비교 대상: 같은 애니메이션을 다른 체형으로 옮긴 결과.
체형이 다르면 뼈 길이가 달라 위치를 그대로 비교할 수 없다. 그래서 두 가지를 본다.

- 뼈마디 방향(segment_angle): 관절과 관절을 잇는 방향이 얼마나 같은가. 체형과 무관하다.
- 발 미끄러짐(foot_slide): 발이 땅에 붙어 있어야 할 구간에서 발이 옆으로 얼마나 흐르는가.
  다리 길이로 정규화해 체형이 달라도 비교할 수 있게 한다. 리타기팅에서 가장 먼저 깨지는 지점이다.
- 발 높이 곡선(foot_clearance, contact_mismatch): 발이 바닥에서 얼마나 떠 있는지의 시간별 곡선과
  접지 구간이 원본과 같은지. 각도 지표와 **독립**이라, 각도를 고치려는 수정이 실제로 디딤을
  좋게 했는지(또는 지표에만 맞춘 것인지) 가려낸다.

정답 데이터는 editor/upt_capture_retarget_poses.py로 만든다(UPT.RetargetE2E를 먼저 돌려야 한다).
"""
from __future__ import annotations

import json
import math
import os

import numpy as np

from common import GROUND_TRUTH_DIR, latest_dataset, use_utf8_output

# 발이 땅에 붙어 있다고 보는 높이: 그 클립에서 발이 가장 낮았던 지점부터 다리 길이의 이 비율까지
CONTACT_HEIGHT_RATIO = 0.08


def vector(row: dict, bone: str):
    value = row["bones"].get(bone)
    return np.array(value, dtype=np.float64) if value else None


def leg_length(frames: list, chain: list) -> float:
    """다리 길이(골반→발). 체형이 다른 두 캐릭터를 같은 기준으로 놓기 위한 척도."""
    row = frames[0]
    total = 0.0
    for parent, child in zip(chain, chain[1:]):
        a, b = vector(row, parent), vector(row, child)
        if a is None or b is None:
            return 0.0
        total += float(np.linalg.norm(b - a))
    return total


def segment_angles(frames: list, segments: list) -> dict:
    """샘플마다 각 뼈마디의 방향 단위벡터."""
    result = {}
    for parent, child in segments:
        directions = []
        for row in frames:
            a, b = vector(row, parent), vector(row, child)
            if a is None or b is None:
                directions.append(None)
                continue
            delta = b - a
            norm = float(np.linalg.norm(delta))
            directions.append(delta / norm if norm > 1e-6 else None)
        result[f"{parent}->{child}"] = directions
    return result


def foot_slide(frames: list, bones: list, scale: float) -> list:
    """접지 구간에서 발이 수평으로 흐른 거리(다리 길이 대비). 샘플 간 이동량으로 잰다."""
    if scale <= 1e-6:
        return []
    slides = []
    for bone in bones:
        positions = [vector(row, bone) for row in frames]
        if any(position is None for position in positions):
            continue
        heights = np.array([position[2] for position in positions])
        threshold = heights.min() + scale * CONTACT_HEIGHT_RATIO
        for index in range(1, len(positions)):
            if heights[index] > threshold or heights[index - 1] > threshold:
                continue  # 두 샘플 모두 접지 상태일 때만 센다
            horizontal = np.linalg.norm(positions[index][:2] - positions[index - 1][:2])
            slides.append(float(horizontal) / scale)
    return slides


def motion_deviation(angles: list) -> float:
    """각도 오차에서 '움직이는 만큼'만 뽑는다. 중앙값에서 얼마나 흔들리는지(MAD).

    각도 오차는 두 가지가 섞여 있다. 두 스켈레톤의 레스트 포즈가 달라 생기는 **고정 오프셋**과,
    동작이 실제로 뭉개져 생기는 **변동분**이다. 앞의 것은 시간에 따라 변하지 않으므로 중앙값에 들어가고,
    뒤의 것만 중앙값 주위의 흩어짐으로 남는다. 동작 보존을 보려면 뒤의 것을 봐야 한다.
    """
    values = np.asarray(angles, dtype=np.float64)
    return round(float(np.median(np.abs(values - np.median(values)))), 2)


def foot_clearance(frames: list, bones: list, scale: float) -> dict:
    """발이 바닥에서 얼마나 떠 있는지의 시간별 곡선(다리 길이로 정규화).

    뼈마디 각도와 달리 **방향이 아니라 높이**를 본다. `calf→foot` 각도를 고치려는 수정이
    실제로 발 디딤을 좋게 만들었는지 각도와 무관하게 판정하기 위한 지표다.
    각도 지표만 보고 고치면 지표에 맞춰 튜닝하는 셈이 되므로, 따로 잰다.

    바닥은 그 클립에서 두 발이 닿은 가장 낮은 지점으로 잡는다. 원본과 결과가 같은 동작이면
    같은 시각에 같은 비율만큼 떠 있어야 한다.
    """
    if scale <= 1e-6:
        return {}
    tracks = {}
    floor = None
    for bone in bones:
        positions = [vector(row, bone) for row in frames]
        if any(position is None for position in positions):
            continue
        heights = np.array([position[2] for position in positions])
        floor = heights.min() if floor is None else min(floor, heights.min())
        tracks[bone] = heights
    if not tracks or floor is None:
        return {}
    return {bone: (heights - floor) / scale for bone, heights in tracks.items()}


def contact_metrics(source: list, target: list, bones: list,
                    source_scale: float, target_scale: float) -> dict:
    """원본과 결과의 발 높이 곡선을 비교한다."""
    source_tracks = foot_clearance(source, bones, source_scale)
    target_tracks = foot_clearance(target, bones, target_scale)
    shared = [bone for bone in bones if bone in source_tracks and bone in target_tracks]
    if not shared:
        return {"foot_clearance_error": None, "contact_mismatch": None}

    errors, mismatches, total = [], 0, 0
    for bone in shared:
        a, b = source_tracks[bone], target_tracks[bone]
        count = min(len(a), len(b))
        errors += list(np.abs(a[:count] - b[:count]))
        # 접지 상태(바닥 근처에 있는가)가 같은 시각에 같은지. 디딤 타이밍이 바뀌면 여기서 드러난다.
        source_down = a[:count] <= CONTACT_HEIGHT_RATIO
        target_down = b[:count] <= CONTACT_HEIGHT_RATIO
        mismatches += int(np.count_nonzero(source_down != target_down))
        total += count
    return {
        "foot_clearance_error": round(float(np.median(errors)), 4) if errors else None,
        "contact_mismatch": round(mismatches / total, 4) if total else None,
    }


def twist_motion_range(frames: list, role: str) -> float | None:
    """트위스트 본의 로컬 회전이 샷 동안 얼마나 움직였는가(도).

    트위스트 본은 팔·다리 축을 따라 **구르는** 본이라 위치로는 거의 아무것도 알 수 없다.
    실무에서 실제로 깨지는 방식은 "트위스트 트랙이 통째로 빠져 레퍼런스 포즈에 멈춘다"는
    것이고, 그러면 아래팔 스키닝이 뒤틀린다. 그래서 각도가 아니라 **움직인 범위**를 잰다.
    첫 샘플 기준 회전차의 최대값을 쓴다.
    """
    quats = [frame.get("twist", {}).get(role) for frame in frames]
    quats = [q for q in quats if q]
    if len(quats) < 2:
        return None
    first = np.array(quats[0], dtype=float)
    worst = 0.0
    for q in quats[1:]:
        # 쿼터니언 사이 각도. 부호가 뒤집힌 표현도 같은 회전이므로 절대값을 쓴다.
        dot = abs(float(np.clip(np.dot(first, np.array(q, dtype=float)), -1.0, 1.0)))
        worst = max(worst, math.degrees(2.0 * math.acos(dot)))
    return worst


def compare_fingers(source: list, target: list, finger_segments: list) -> dict:
    """손가락 마디 방향을 몸통과 같은 방식으로 비교한다.

    손가락은 지금까지 측정 범위 밖이었다. 리타기팅이 손가락을 옮기는지 아무도 재지 않았고,
    "범용"을 주장하려면 손이 빠진 채로는 부족하다.
    """
    source_dirs = segment_angles(source, [tuple(seg) for seg in finger_segments])
    target_dirs = segment_angles(target, [tuple(seg) for seg in finger_segments])
    per_segment = {}
    for name, source_list in source_dirs.items():
        angles = []
        for a, b in zip(source_list, target_dirs.get(name, [])):
            if a is None or b is None:
                continue
            angles.append(math.degrees(math.acos(float(np.clip(np.dot(a, b), -1.0, 1.0)))))
        if angles:
            per_segment[name] = round(float(np.median(angles)), 1)
    return per_segment

def compare_pair(pair: dict, segments: list, ground_bones: list, scale_chain: list,
                 finger_segments: list | None = None) -> dict:
    source, target = pair["source_frames"], pair["target_frames"]
    source_scale = leg_length(source, scale_chain)
    target_scale = leg_length(target, scale_chain)

    source_dirs = segment_angles(source, segments)
    target_dirs = segment_angles(target, segments)
    per_segment, per_segment_motion, all_angles = {}, {}, []
    for name, source_list in source_dirs.items():
        target_list = target_dirs.get(name, [])
        angles = []
        for a, b in zip(source_list, target_list):
            if a is None or b is None:
                continue
            angles.append(math.degrees(math.acos(float(np.clip(np.dot(a, b), -1.0, 1.0)))))
        if angles:
            per_segment[name] = round(float(np.median(angles)), 1)
            per_segment_motion[name] = motion_deviation(angles)
            all_angles += angles

    # 손가락. 양쪽에 다 있는 손가락만 비교된다(없으면 segment_angles가 건너뛴다).
    finger_per_segment = compare_fingers(source, target, finger_segments or [])
    # 트위스트. 원본이 움직였는데 결과가 멈춰 있으면 트랙이 빠진 것이다.
    twist_rows = {}
    for role in pair.get("twist_roles", []):
        source_range = twist_motion_range(source, role)
        target_range = twist_motion_range(target, role)
        if source_range is None or target_range is None:
            continue
        twist_rows[role] = {"source_deg": round(source_range, 1), "target_deg": round(target_range, 1)}

    source_slides = foot_slide(source, ground_bones, source_scale)
    target_slides = foot_slide(target, ground_bones, target_scale)
    contact = contact_metrics(source, target, ground_bones, source_scale, target_scale)
    return {
        **contact,
        "name": pair["name"],
        "source_mesh": pair.get("source_mesh", "?"),
        "target_mesh": pair.get("target_mesh", "?"),
        "length_match": abs(pair["source_length"] - pair["target_length"]) < 0.01,
        "segment_angle_median_deg": round(float(np.median(all_angles)), 1) if all_angles else None,
        "segment_angle_p90_deg": round(float(np.percentile(all_angles, 90)), 1) if all_angles else None,
        "worst_segment": max(per_segment, key=per_segment.get) if per_segment else None,
        "worst_segment_deg": max(per_segment.values()) if per_segment else None,
        # 고정 오프셋을 뺀, 동작이 실제로 뭉개진 정도
        "motion_deviation_worst": max(per_segment_motion, key=per_segment_motion.get) if per_segment_motion else None,
        "motion_deviation_worst_deg": max(per_segment_motion.values()) if per_segment_motion else None,
        "per_segment_deg": per_segment,
        "per_segment_motion_deg": per_segment_motion,
        "source_leg_length": round(source_scale, 1),
        "target_leg_length": round(target_scale, 1),
        # 원본 자체도 완벽히 고정되지는 않으므로 원본 대비 증가분을 함께 본다.
        "foot_slide_source": round(float(np.median(source_slides)), 4) if source_slides else None,
        "foot_slide_target": round(float(np.median(target_slides)), 4) if target_slides else None,
        # 손가락·트위스트는 지금까지 측정 범위 밖이었다. 없는 데이터셋에서는 None이 된다.
        "finger_per_segment_deg": finger_per_segment or None,
        "finger_angle_median_deg": round(float(np.median(list(finger_per_segment.values()))), 1) if finger_per_segment else None,
        "finger_worst_deg": max(finger_per_segment.values()) if finger_per_segment else None,
        "finger_segments_compared": len(finger_per_segment),
        "twist": twist_rows or None,
        # 원본이 5° 이상 구른 트위스트 중, 결과가 그 20% 미만으로만 구른 것의 수.
        # 트위스트 트랙이 빠지면 여기가 올라간다.
        "twist_dropped": sum(1 for v in twist_rows.values()
                             if v["source_deg"] >= 5.0 and v["target_deg"] < 0.2 * v["source_deg"]),
        "twist_compared": len(twist_rows),
        # "빠지지 않았다"만으로는 부족하다. 원본 대비 **얼마나** 전달됐는지를 비로 본다.
        # 1.00이면 그대로, 0이면 통째로 사라진 것이다. 원본이 5° 이상 구른 역할만 센다.
        "twist_ratio_worst_error": round(max(
            (abs(v["target_deg"] / v["source_deg"] - 1.0) for v in twist_rows.values()
             if v["source_deg"] >= 5.0), default=0.0), 3),
    }


def by_target(rows: list) -> dict:
    """원본→대상 조합별로 나눠 본다.

    전체 중앙값 하나로는 한쪽 조합만 나쁜 경우가 묻힌다. 대상만으로 묶어도 안 되는데,
    같은 대상이라도 원본이 바뀌면 결과가 크게 달라지기 때문이다(UE4 마네킹 원본과
    Synty 원본은 같은 골렘에 대해 서로 다른 뼈마디에서 틀어진다).
    """
    groups = {}
    for row in rows:
        key = f"{row.get('source_mesh', '?')}->{row.get('target_mesh', '?')}"
        groups.setdefault(key, []).append(row)
    summary = {}
    for target, group in groups.items():
        angles = [row["segment_angle_median_deg"] for row in group if row["segment_angle_median_deg"] is not None]
        worst = [row["worst_segment_deg"] for row in group if row["worst_segment_deg"] is not None]
        summary[target] = {
            "pairs": len(group),
            "segment_angle_median_deg": round(float(np.median(angles)), 1) if angles else None,
            "worst_segment_deg": round(float(max(worst)), 1) if worst else None,
            "worst_segment": max(
                (row for row in group if row["worst_segment_deg"] is not None),
                key=lambda row: row["worst_segment_deg"], default={}).get("worst_segment"),
        }
    return summary


def run(output_dir: str) -> dict:
    data_dir = latest_dataset("retarget")
    if not data_dir:
        return {"skipped": "정답 데이터 없음(retarget). UPT.RetargetE2E 후 upt_capture_retarget_poses.py를 돌리세요"}
    path = os.path.join(data_dir, "retarget_poses.json")
    if not os.path.isfile(path):
        return {"skipped": f"retarget_poses.json 없음({os.path.basename(data_dir)})"}
    with open(path, encoding="utf-8") as handle:
        data = json.load(handle)
    if not data.get("pairs"):
        return {"skipped": "비교할 리타기팅 쌍이 없습니다"}

    segments = [tuple(segment) for segment in data["segments"]]
    finger_segments = data.get("finger_segments") or []
    rows = [compare_pair(pair, segments, data["ground_bones"], data["scale_chain"], finger_segments)
            for pair in data["pairs"]]
    angles = [row["segment_angle_median_deg"] for row in rows if row["segment_angle_median_deg"] is not None]
    worst = [row["worst_segment_deg"] for row in rows if row["worst_segment_deg"] is not None]
    motion = [row["motion_deviation_worst_deg"] for row in rows if row["motion_deviation_worst_deg"] is not None]
    target_slides = [row["foot_slide_target"] for row in rows if row["foot_slide_target"] is not None]
    clearance = [row["foot_clearance_error"] for row in rows if row.get("foot_clearance_error") is not None]
    mismatch = [row["contact_mismatch"] for row in rows if row.get("contact_mismatch") is not None]
    source_slides = [row["foot_slide_source"] for row in rows if row["foot_slide_source"] is not None]
    fingers = [row["finger_angle_median_deg"] for row in rows if row.get("finger_angle_median_deg") is not None]
    fingers_worst = [row["finger_worst_deg"] for row in rows if row.get("finger_worst_deg") is not None]

    os.makedirs(output_dir, exist_ok=True)
    metrics = {
        "pairs": len(rows),
        "targets": by_target(rows),
        "length_mismatch": sum(1 for row in rows if not row["length_match"]),
        "segment_angle_median_deg": round(float(np.median(angles)), 1) if angles else None,
        "worst_segment_deg": round(float(max(worst)), 1) if worst else None,
        "motion_deviation_worst_deg": round(float(max(motion)), 2) if motion else None,
        "foot_slide_target": round(float(np.median(target_slides)), 4) if target_slides else None,
        "foot_slide_source": round(float(np.median(source_slides)), 4) if source_slides else None,
        # 각도와 독립인 접지 지표
        "foot_clearance_error": round(float(np.median(clearance)), 4) if clearance else None,
        "foot_clearance_worst": round(float(max(clearance)), 4) if clearance else None,
        "contact_mismatch": round(float(np.median(mismatch)), 4) if mismatch else None,
        # 손가락·트위스트. 예전 데이터셋에는 없으므로 None이 될 수 있다.
        # min을 쓰면 손가락 없는 대상(골렘) 하나 때문에 전체가 0으로 찍혀 "측정 안 됨"처럼 보인다.
        # 몇 쌍에서 쟀는지를 세는 쪽이 사실에 가깝다.
        "pairs_with_fingers": sum(1 for row in rows if row["finger_segments_compared"] > 0),
        "finger_angle_median_deg": round(float(np.median(fingers)), 1) if fingers else None,
        "finger_worst_deg": round(float(max(fingers_worst)), 1) if fingers_worst else None,
        "pairs_with_twist": sum(1 for row in rows if row["twist_compared"] > 0),
        "twist_dropped_worst": max((row["twist_dropped"] for row in rows), default=0),
        "twist_ratio_worst_error": round(max((row["twist_ratio_worst_error"] for row in rows), default=0.0), 3),
        "dataset": os.path.basename(data_dir),
    }
    with open(os.path.join(output_dir, "retarget_rows.json"), "w", encoding="utf-8") as handle:
        json.dump({"metrics": metrics, "rows": rows}, handle, ensure_ascii=False)
    return metrics


if __name__ == "__main__":
    use_utf8_output()
    print(json.dumps(run(os.path.join(os.getcwd(), "eval_output")), ensure_ascii=False, indent=2))
