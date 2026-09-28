"""크기 정답 세트(size_*)로 샷 크기별 인물 크기·조준점 추정 오차를 잰다.

구도 검증 루프는 같은 캐릭터의 서로 다른 구도끼리 비교하므로 '일정한 편향'은 상쇄되고 '흩어짐'이 점수를 흔든다.
그래서 log(추정 키/정답 키)의 중앙값(bias, 배율로 표시)과 강건 표준편차(spread = 1.4826×MAD), 조준점 거리 오차를 함께 낸다.
"""
from __future__ import annotations

import json
import math
import os

import cv2
import numpy as np

from common import latest_dataset, load_pose_models, use_utf8_output
import upt_pose_reference_analyzer as analyzer

KPT_THR = 0.35
SIZES = ["full", "medium", "close_up", "extreme_close_up"]


def run(output_dir: str) -> dict:
    data_dir = latest_dataset("size")
    if not data_dir:
        return {"skipped": "정답 데이터 없음(size)"}
    with open(os.path.join(data_dir, "size_truth.json"), encoding="utf-8") as handle:
        truth = json.load(handle)
    detector, pose = load_pose_models()
    records = []
    for item in truth:
        image = cv2.imread(os.path.join(data_dir, item["image"]))
        height, width = image.shape[:2]
        records.append((item, analyzer.detect_raw_people(detector, pose, image, KPT_THR), width, height))
    # 실제 사용과 같게, 세트 안의 전신 샷으로 체형 비율을 보정한 뒤 잰다.
    ratios, calibration = analyzer.calibrate_ratios([person for _, raw, _, _ in records for person in raw], KPT_THR, records[0][2], records[0][3])

    values = {size: [] for size in SIZES}
    totals = {size: 0 for size in SIZES}
    for item, raw, width, height in records:
        size = item["shot_size"]
        totals[size] += 1
        people = [m for m in (analyzer.fit_body(b, k, s, KPT_THR, width, height, ratios) for b, k, s in raw) if m and m.score >= 0.2]
        if not people:
            continue
        person = max(people, key=lambda measure: measure.main_person_key())
        focus_ratio = analyzer.FOCUS_HEIGHT_RATIO[size]
        curve = {point[0]: point[1:] for point in item["focus_curve"]}
        truth_x, truth_y = min(curve.items(), key=lambda entry: abs(entry[0] - focus_ratio))[1]
        focus_x, focus_y = person.point_at(focus_ratio)
        values[size].append((math.log((person.height_px / height) / item["full_body_height"]),
                             math.hypot(focus_x / width - truth_x, focus_y / height - truth_y)))

    metrics = {"dataset": os.path.basename(data_dir), "calibration": calibration}
    for size in SIZES:
        found = values[size]
        if not found:
            metrics[size] = {"found": 0, "total": totals[size]}
            continue
        logs = np.array([value[0] for value in found])
        positions = np.array([value[1] for value in found])
        bias = float(np.median(logs))
        metrics[size] = {
            "found": len(found), "total": totals[size], "bias": round(math.exp(bias), 3),
            "spread": round(1.4826 * float(np.median(np.abs(logs - bias))), 3),
            "max_deviation": round(float(np.max(np.abs(logs - bias))), 3),
            "position_median": round(float(np.median(positions)), 3), "position_max": round(float(np.max(positions)), 3),
        }
    return metrics


if __name__ == "__main__":
    use_utf8_output()
    print(json.dumps(run(""), ensure_ascii=False, indent=2))
