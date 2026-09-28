"""포즈 분석기가 추정하는 '전신 화면 높이'가 실제와 얼마나 맞는지 잰다.

정답: 에디터에서 만든 height_* 데이터. 엔진이 Bounds와 카메라로 계산한 정확한 화면 높이다.
추정: 같은 프레임을 분석기(YOLOX + RTMPose + fit_body)로 재서 나온 height_px.

몸이 프레임에 잘리면 분석기는 보이는 부분만으로 전신 키를 외삽해야 한다. 이 오차가 구도 검증의
크기 점수를 흔들기 때문에, 잘린 경우와 전신이 보이는 경우를 나눠서 본다.
"""
from __future__ import annotations

import json
import os

import cv2
import numpy as np

from common import GROUND_TRUTH_DIR, load_pose_models, use_utf8_output

KEYPOINT_THRESHOLD = 0.35
MIN_BODY_SCORE = 0.2


def load_truth(data_dir: str) -> dict | None:
    path = os.path.join(data_dir, "height_truth.json")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def measure(data_dir: str, truth: dict) -> list[dict]:
    """프레임마다 분석기가 보는 전신 높이를 잰다. 비율 보정은 검증 스크립트와 같은 방식으로 한 번만 한다."""
    import upt_pose_reference_analyzer as analyzer

    detector, pose_model = load_pose_models()
    captures = []
    for frame in truth["frames"]:
        path = os.path.join(data_dir, "frames", frame["file"])
        image = cv2.imdecode(np.fromfile(path, dtype=np.uint8), cv2.IMREAD_COLOR) if os.path.isfile(path) else None
        raw = analyzer.detect_raw_people(detector, pose_model, image, KEYPOINT_THRESHOLD) if image is not None else []
        captures.append((frame, image, raw))

    first = next((image for _, image, _ in captures if image is not None), None)
    frame_h, frame_w = first.shape[:2] if first is not None else (truth["height"], truth["width"])
    ratios, calibration = analyzer.calibrate_ratios([r for _, _, raw in captures for r in raw],
                                                    KEYPOINT_THRESHOLD, frame_w, frame_h, min_samples=2)
    if not calibration:
        ratios = dict(analyzer.DEFAULT_KEYPOINT_RATIO)

    rows = []
    for frame, image, raw in captures:
        if image is None:
            rows.append({**frame, "estimated": None})
            continue
        people = [m for m in (analyzer.fit_body(b, k, s, KEYPOINT_THRESHOLD, frame_w, frame_h, ratios) for b, k, s in raw)
                  if m and m.score >= MIN_BODY_SCORE]
        if not people:
            rows.append({**frame, "estimated": None})
            continue
        person = max(people, key=lambda candidate: candidate.main_person_key())
        visible = float(person.box[3] - person.box[1]) / frame_h
        rows.append({**frame, "estimated": float(person.height_px) / frame_h, "visible_height": visible})
    return rows


def summarize(rows: list[dict]) -> dict:
    def errors(selected):
        return np.array([abs(row["estimated"] - row["screen_height"]) / row["screen_height"]
                         for row in selected if row.get("estimated")])

    measured = [row for row in rows if row.get("estimated")]
    full = [row for row in measured if not row["cropped"]]
    cropped = [row for row in measured if row["cropped"]]
    signed = np.array([(row["estimated"] - row["screen_height"]) / row["screen_height"] for row in measured])

    def stats(values):
        return (None if not len(values) else
                {"median_pct": round(100 * float(np.median(values)), 1),
                 "p90_pct": round(100 * float(np.percentile(values, 90)), 1)})

    return {
        "frames": len(rows),
        "frames_without_person": sum(1 for row in rows if not row.get("estimated")),
        "all": stats(errors(measured)),
        "full_body": stats(errors(full)),
        "cropped": stats(errors(cropped)),
        # 부호가 한쪽으로 쏠리면 보정 가능한 편향, 섞여 있으면 잡음이다.
        "bias_pct": round(100 * float(np.median(signed)), 1) if len(signed) else None,
    }


def run(output_dir: str) -> dict:
    """height_* 데이터셋을 모두 돌린다. 체형이 다른 캐릭터를 함께 지켜야 한쪽에 맞춘 보정을 걸러낼 수 있다."""
    import glob

    data_dirs = sorted(path for path in glob.glob(os.path.join(GROUND_TRUTH_DIR, "height_*")) if os.path.isdir(path))
    if not data_dirs:
        return {"skipped": "정답 데이터 없음(height)"}
    os.makedirs(output_dir, exist_ok=True)

    all_rows, per_dataset = [], {}
    for data_dir in data_dirs:
        truth = load_truth(data_dir)
        if not truth:
            continue
        rows = measure(data_dir, truth)
        per_dataset[os.path.basename(data_dir)] = summarize(rows)
        all_rows += rows
    if not all_rows:
        return {"skipped": "height_truth.json 없음"}

    metrics = summarize(all_rows)
    metrics["datasets"] = per_dataset
    # 한 캐릭터만 좋아지고 다른 캐릭터가 나빠지는 변경을 걸러내기 위해, 가장 나쁜 데이터셋 값도 함께 본다.
    metrics["worst_dataset_median_pct"] = max(value["all"]["median_pct"] for value in per_dataset.values() if value.get("all"))
    with open(os.path.join(output_dir, "body_height_rows.json"), "w", encoding="utf-8") as handle:
        json.dump({"metrics": metrics, "rows": all_rows}, handle, ensure_ascii=False)
    return metrics


if __name__ == "__main__":
    use_utf8_output()
    print(json.dumps(run(os.path.join(os.getcwd(), "eval_output")), ensure_ascii=False, indent=2))
