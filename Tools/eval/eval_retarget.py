"""리타기팅이 원본 동작을 보존했는지 잰다.

정답: 원본 애니메이션의 자세. 비교 대상: 같은 애니메이션을 다른 체형으로 옮긴 결과.
체형이 다르면 뼈 길이가 달라 위치를 그대로 비교할 수 없다. 그래서 두 가지를 본다.

- 뼈마디 방향(segment_angle): 관절과 관절을 잇는 방향이 얼마나 같은가. 체형과 무관하다.
- 발 미끄러짐(foot_slide): 발이 땅에 붙어 있어야 할 구간에서 발이 옆으로 얼마나 흐르는가.
  다리 길이로 정규화해 체형이 달라도 비교할 수 있게 한다. 리타기팅에서 가장 먼저 깨지는 지점이다.

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


def compare_pair(pair: dict, segments: list, ground_bones: list, scale_chain: list) -> dict:
    source, target = pair["source_frames"], pair["target_frames"]
    source_scale = leg_length(source, scale_chain)
    target_scale = leg_length(target, scale_chain)

    source_dirs = segment_angles(source, segments)
    target_dirs = segment_angles(target, segments)
    per_segment, all_angles = {}, []
    for name, source_list in source_dirs.items():
        target_list = target_dirs.get(name, [])
        angles = []
        for a, b in zip(source_list, target_list):
            if a is None or b is None:
                continue
            angles.append(math.degrees(math.acos(float(np.clip(np.dot(a, b), -1.0, 1.0)))))
        if angles:
            per_segment[name] = round(float(np.median(angles)), 1)
            all_angles += angles

    source_slides = foot_slide(source, ground_bones, source_scale)
    target_slides = foot_slide(target, ground_bones, target_scale)
    return {
        "name": pair["name"],
        "length_match": abs(pair["source_length"] - pair["target_length"]) < 0.01,
        "segment_angle_median_deg": round(float(np.median(all_angles)), 1) if all_angles else None,
        "segment_angle_p90_deg": round(float(np.percentile(all_angles, 90)), 1) if all_angles else None,
        "worst_segment": max(per_segment, key=per_segment.get) if per_segment else None,
        "worst_segment_deg": max(per_segment.values()) if per_segment else None,
        "per_segment_deg": per_segment,
        "source_leg_length": round(source_scale, 1),
        "target_leg_length": round(target_scale, 1),
        # 원본 자체도 완벽히 고정되지는 않으므로 원본 대비 증가분을 함께 본다.
        "foot_slide_source": round(float(np.median(source_slides)), 4) if source_slides else None,
        "foot_slide_target": round(float(np.median(target_slides)), 4) if target_slides else None,
    }


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
    rows = [compare_pair(pair, segments, data["ground_bones"], data["scale_chain"]) for pair in data["pairs"]]
    angles = [row["segment_angle_median_deg"] for row in rows if row["segment_angle_median_deg"] is not None]
    worst = [row["worst_segment_deg"] for row in rows if row["worst_segment_deg"] is not None]
    target_slides = [row["foot_slide_target"] for row in rows if row["foot_slide_target"] is not None]
    source_slides = [row["foot_slide_source"] for row in rows if row["foot_slide_source"] is not None]

    os.makedirs(output_dir, exist_ok=True)
    metrics = {
        "pairs": len(rows),
        "length_mismatch": sum(1 for row in rows if not row["length_match"]),
        "segment_angle_median_deg": round(float(np.median(angles)), 1) if angles else None,
        "worst_segment_deg": round(float(max(worst)), 1) if worst else None,
        "foot_slide_target": round(float(np.median(target_slides)), 4) if target_slides else None,
        "foot_slide_source": round(float(np.median(source_slides)), 4) if source_slides else None,
        "dataset": os.path.basename(data_dir),
    }
    with open(os.path.join(output_dir, "retarget_rows.json"), "w", encoding="utf-8") as handle:
        json.dump({"metrics": metrics, "rows": rows}, handle, ensure_ascii=False)
    return metrics


if __name__ == "__main__":
    use_utf8_output()
    print(json.dumps(run(os.path.join(os.getcwd(), "eval_output")), ensure_ascii=False, indent=2))
