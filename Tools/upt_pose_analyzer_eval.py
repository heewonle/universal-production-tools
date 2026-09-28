"""포즈 분석기 결과(Reference Plan JSON)를 정답 데이터(ground_truth.json)와 비교해 오차를 출력한다.

회귀 검사(eval/run_regression.py)는 evaluate()가 돌려주는 수치를 기준값과 비교한다.
"""
from __future__ import annotations

import argparse
import json
import sys

FOCUS_HEIGHT_RATIO = {"extreme_close_up": 0.92, "close_up": 0.88, "medium": 0.72, "full": 0.5, "wide": 0.5}


def overlap(a0: float, a1: float, b0: float, b1: float) -> float:
    return max(0.0, min(a1, b1) - max(a0, b0))


def mean(values: list[float]) -> float | None:
    return round(sum(values) / len(values), 4) if values else None


def evaluate(plan: dict, truth: dict) -> dict:
    rows, size_hits, motion_hits, x_errors, y_errors, height_ratios, boundary_errors = [], 0, 0, [], [], [], []
    for expected in truth["shots"]:
        best = max(plan["shots"], key=lambda s: overlap(s["start_seconds"], s["end_seconds"], expected["start"], expected["end"]), default=None)
        if best is None or overlap(best["start_seconds"], best["end_seconds"], expected["start"], expected["end"]) <= 0:
            rows.append(f"{expected['name']}: 매칭되는 샷 없음")
            continue
        boundary_errors.append(abs(best["start_seconds"] - expected["start"]))
        ratio = FOCUS_HEIGHT_RATIO[best["shot_size"]]
        curve = expected.get("focus_curve")
        if curve:
            # 실제 본(발-골반-가슴-목-머리)을 따라 투영한 몸 곡선에서 분석기가 선택한 조준 높이의 정답 좌표를 보간한다.
            lower = max((p for p in curve if p[0] <= ratio), key=lambda p: p[0])
            upper = min((p for p in curve if p[0] >= ratio), key=lambda p: p[0])
            t = 0.0 if upper[0] == lower[0] else (ratio - lower[0]) / (upper[0] - lower[0])
            truth_x = lower[1] + (upper[1] - lower[1]) * t
            truth_y = lower[2] + (upper[2] - lower[2]) * t
        else:
            top, bottom = expected["top_screen"], expected["bottom_screen"]
            truth_x = bottom[0] + (top[0] - bottom[0]) * ratio
            truth_y = bottom[1] + (top[1] - bottom[1]) * ratio
        found_x, found_y = best["subject_screen_position"]["x"], best["subject_screen_position"]["y"]
        measured_height = best.get("diagnostics", {}).get("full_body_screen_height")
        size_ok = best["shot_size"] == expected["intended_shot_size"]
        motion_ok = best["camera_motion"] == expected.get("intended_motion", "static")
        size_hits += int(size_ok)
        motion_hits += int(motion_ok)
        x_errors.append(abs(found_x - truth_x))
        y_errors.append(abs(found_y - truth_y))
        if measured_height:
            height_ratios.append(measured_height / expected["full_body_height"])
        rows.append(
            f"{expected['name']}: size {best['shot_size']} (정답 {expected['intended_shot_size']}) {'OK' if size_ok else 'MISS'} | "
            f"조준점 ({found_x:.3f},{found_y:.3f}) vs 정답 ({truth_x:.3f},{truth_y:.3f}) 오차 x={abs(found_x - truth_x):.3f} y={abs(found_y - truth_y):.3f} | "
            f"전신높이 측정/정답 {measured_height if measured_height is None else round(measured_height / expected['full_body_height'], 3)} | "
            f"motion {best['camera_motion']} (정답 {expected.get('intended_motion', 'static')}) {'OK' if motion_ok else 'MISS'} | conf {best['confidence']}"
        )
    metrics = {
        "shots_detected": len(plan["shots"]), "shot_count": len(truth["shots"]),
        "size_hits": size_hits, "motion_hits": motion_hits,
        "mean_x_error": mean(x_errors), "mean_y_error": mean(y_errors),
        "mean_height_ratio": mean(height_ratios), "mean_boundary_error": mean(boundary_errors),
    }
    return {"rows": rows, "metrics": metrics}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", required=True)
    parser.add_argument("--truth", required=True)
    args = parser.parse_args()
    with open(args.plan, encoding="utf-8") as handle:
        plan = json.load(handle)
    with open(args.truth, encoding="utf-8") as handle:
        truth = json.load(handle)
    result = evaluate(plan, truth)
    metrics = result["metrics"]
    print("\n".join(result["rows"]))
    count = metrics["shot_count"]
    print(f"\nSUMMARY shots detected={metrics['shots_detected']}/{count} | shot_size 정확도={metrics['size_hits']}/{count} | "
          f"motion 정확도={metrics['motion_hits']}/{count} | 조준점 평균오차 x={metrics['mean_x_error']} y={metrics['mean_y_error']} (화면 비율) | "
          f"전신높이 비율 평균={metrics['mean_height_ratio']} | 컷 시작 평균오차={metrics['mean_boundary_error']}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
