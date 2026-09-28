"""레터박스·고정 자막이 있는 세로 영상(쇼츠 형태)에서 실제 화면 영역을 찾아 컷을 제대로 나누는지 검사한다.

구도 정답 영상(5샷)을 608x1080 세로 화면 가운데에 넣고, 위아래 검은 띠에 고정 도형(자막·로고 대용)을 그린 영상을 쓴다.
이 조합에서 예전에 두 번 실패했다.
  - 고정 도형이 배경 특징점으로 잡혀 merge_false_cuts가 실제 컷을 모두 합침(20샷 → 1샷)
  - 넓은 검은 띠가 화면 변화량을 희석해 컷이 검출 기준에 못 미침
"""
from __future__ import annotations

import json
import os

from common import latest_dataset, run_analyzer, use_utf8_output

EXPECTED_SHOTS = 5
EXPECTED_SIZES = ["wide", "full", "medium", "close_up"]


def run(output_dir: str) -> dict:
    data_dir = latest_dataset("letterbox")
    if not data_dir:
        return {"skipped": "정답 데이터 없음(letterbox)"}
    video = os.path.join(data_dir, "letterbox_truth.mp4")
    if not os.path.isfile(video):
        return {"skipped": f"정답 영상 없음({os.path.basename(data_dir)})"}

    os.makedirs(output_dir, exist_ok=True)
    plan_file = os.path.join(output_dir, "letterbox_plan.json")
    run_analyzer(video, plan_file)
    with open(plan_file, encoding="utf-8") as handle:
        plan = json.load(handle)

    notes = plan.get("analysis_notes") or {}
    area = notes.get("active_area")
    shots = plan.get("shots", [])
    sizes = [shot.get("shot_size") for shot in shots]
    size_hits = sum(1 for expected, actual in zip(EXPECTED_SIZES, sizes) if expected == actual)
    return {
        "dataset": os.path.basename(data_dir),
        "active_area_found": area is not None,
        # 실제 화면은 세로 가운데 약 32%(311~653 / 1080)에 있다.
        "area_height_ratio": round((area["y1"] - area["y0"]), 3) if area else None,
        "aspect_ratio": round(plan.get("aspect_ratio", 0.0), 2),
        "shots_detected": len(shots),
        "size_hits": size_hits,
        "sizes": sizes,
    }


if __name__ == "__main__":
    use_utf8_output()
    print(json.dumps(run(os.path.join(os.getcwd(), "eval_output")), ensure_ascii=False, indent=2))
