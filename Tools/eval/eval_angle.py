"""카메라 앵글 정답 세트(학습 angle_*, 검증 angleval_*, 최종 테스트 angletest_*)로 앵글 판정 정확도를 잰다.

검증 세트는 임계값 조정에 쓰였으므로 실제 성능은 최종 테스트 세트 수치로 판단한다.
"""
from __future__ import annotations

import json
import os

import cv2

from common import latest_dataset, load_pose_models, use_utf8_output
import upt_pose_reference_analyzer as analyzer

KPT_THR = 0.35
ANGLE_ORDER = ["low", "eye", "high", "overhead"]
SETS = [("train", "angle_train"), ("validation", "angle_val"), ("test", "angle_test")]


def run(output_dir: str) -> dict:
    detector, pose = load_pose_models()
    metrics = {}
    for label, dataset in SETS:
        data_dir = latest_dataset(dataset)
        if not data_dir:
            metrics[label] = {"skipped": f"정답 데이터 없음({dataset})"}
            continue
        with open(os.path.join(data_dir, "angle_truth.json"), encoding="utf-8") as handle:
            truth = json.load(handle)
        exact, adjacent, misses = 0, 0, []
        for item in truth:
            image = cv2.imread(os.path.join(data_dir, item["image"]))
            height, width = image.shape[:2]
            raw_people = analyzer.detect_raw_people(detector, pose, image, KPT_THR)
            fitted = [(analyzer.fit_body(raw[0], raw[1], raw[2], KPT_THR, width, height), raw) for raw in raw_people]
            fitted = sorted([entry for entry in fitted if entry[0] and entry[0].score >= 0.2], key=lambda entry: entry[0].main_person_key(), reverse=True)
            expected = item["intended_angle"]
            if not fitted:
                misses.append(f"{item['image']}: 정답 {expected} → 인물 없음")
                continue
            sample = {"main": fitted[0][0], "main_raw": fitted[0][1], "horizon": analyzer.horizon_estimate(image, [raw[0] for raw in raw_people])}
            angle, _, _ = analyzer.estimate_camera_angle([sample], width, height, KPT_THR, analyzer.FOCUS_HEIGHT_RATIO[item["shot_size"]])
            exact += int(angle == expected)
            adjacent += int(abs(ANGLE_ORDER.index(angle) - ANGLE_ORDER.index(expected)) <= 1)
            if angle != expected:
                misses.append(f"{item['image']}: 정답 {expected} → {angle}")
        metrics[label] = {"dataset": os.path.basename(data_dir), "exact": exact, "adjacent": adjacent, "total": len(truth), "misses": misses}
    return metrics


if __name__ == "__main__":
    use_utf8_output()
    print(json.dumps(run(""), ensure_ascii=False, indent=2))
