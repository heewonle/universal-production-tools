"""기울어진·누운 자세 평가: 크기 정답 세트(size_*) 이미지를 여러 각도로 돌려 몸이 기울거나 누운 장면을 만들고,
인물 길이(발바닥~정수리)와 조준점 추정 오차를 잰다.

- 체형 비율 보정은 실제 사용처럼 돌리기 전(서 있는) 전신 샷으로 한 번 구해 모든 각도에 쓴다.
- length_ratio: 추정 몸 길이 / 정답 몸 길이(중앙값), spread: log 비율의 강건 표준편차, focus_error: 조준점 거리 오차(원본 화면 높이 대비).
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
ANGLES = (0, 30, 60, 90)
SIZES = ("full", "medium", "close_up")


def rotate(image: np.ndarray, angle: float) -> tuple[np.ndarray, np.ndarray]:
    height, width = image.shape[:2]
    matrix = cv2.getRotationMatrix2D((width / 2.0, height / 2.0), angle, 1.0)
    cos, sin = abs(matrix[0, 0]), abs(matrix[0, 1])
    new_width, new_height = int(round(height * sin + width * cos)), int(round(height * cos + width * sin))
    matrix[0, 2] += new_width / 2.0 - width / 2.0
    matrix[1, 2] += new_height / 2.0 - height / 2.0
    return cv2.warpAffine(image, matrix, (new_width, new_height), borderMode=cv2.BORDER_REPLICATE), matrix


def run(output_dir: str) -> dict:
    data_dir = latest_dataset("size")
    if not data_dir:
        return {"skipped": "정답 데이터 없음(size)"}
    with open(os.path.join(data_dir, "size_truth.json"), encoding="utf-8") as handle:
        truth = [item for item in json.load(handle) if item["shot_size"] in SIZES]
    detector, pose = load_pose_models()
    images = {item["image"]: cv2.imread(os.path.join(data_dir, item["image"])) for item in truth}
    first = next(iter(images.values()))
    upright_raw = [person for image in images.values() for person in analyzer.detect_raw_people(detector, pose, image, KPT_THR)]
    ratios, _ = analyzer.calibrate_ratios(upright_raw, KPT_THR, first.shape[1], first.shape[0])

    metrics = {"dataset": os.path.basename(data_dir)}
    for angle in ANGLES:
        per_size = {size: [] for size in SIZES}
        totals = {size: 0 for size in SIZES}
        for item in truth:
            image = images[item["image"]]
            height, width = image.shape[:2]
            rotated, matrix = rotate(image, angle)
            rotated_height, rotated_width = rotated.shape[:2]
            totals[item["shot_size"]] += 1
            raw = analyzer.detect_raw_people(detector, pose, rotated, KPT_THR)
            people = [m for m in (analyzer.fit_body(b, k, s, KPT_THR, rotated_width, rotated_height, ratios) for b, k, s in raw) if m and m.score >= 0.2]
            if not people:
                continue
            person = max(people, key=lambda measure: measure.main_person_key())
            focus_ratio = analyzer.FOCUS_HEIGHT_RATIO[item["shot_size"]]
            curve = {point[0]: point[1:] for point in item["focus_curve"]}
            truth_x, truth_y = min(curve.items(), key=lambda entry: abs(entry[0] - focus_ratio))[1]
            truth_point = matrix @ np.array([truth_x * width, truth_y * height, 1.0])
            focus_x, focus_y = person.point_at(focus_ratio)
            truth_length = item["full_body_height"] * height
            per_size[item["shot_size"]].append((math.log(max(person.height_px, 1e-6) / truth_length),
                                                math.hypot(focus_x - truth_point[0], focus_y - truth_point[1]) / height))
        metrics[str(angle)] = {}
        for size in SIZES:
            values = per_size[size]
            if not values:
                metrics[str(angle)][size] = {"found": 0, "total": totals[size]}
                continue
            logs = np.array([value[0] for value in values])
            errors = np.array([value[1] for value in values])
            bias = float(np.median(logs))
            metrics[str(angle)][size] = {
                "found": len(values), "total": totals[size], "length_ratio": round(math.exp(bias), 3),
                "spread": round(1.4826 * float(np.median(np.abs(logs - bias))), 3),
                "focus_error_median": round(float(np.median(errors)), 3), "focus_error_max": round(float(np.max(errors)), 3),
            }
    return metrics


if __name__ == "__main__":
    use_utf8_output()
    result = run("")
    print(f"{'angle':>5s} {'size':9s} {'found':>6s} {'length':>7s} {'spread':>7s} {'focus_med':>9s} {'focus_max':>9s}")
    for angle in ANGLES:
        for size in SIZES:
            m = result[str(angle)][size]
            if not m.get("found"):
                print(f"{angle:5d} {size:9s} {0:>3d}/{m['total']:<2d}")
                continue
            print(f"{angle:5d} {size:9s} {m['found']:>3d}/{m['total']:<2d} {m['length_ratio']:7.3f} {m['spread']:7.3f} {m['focus_error_median']:9.3f} {m['focus_error_max']:9.3f}")
