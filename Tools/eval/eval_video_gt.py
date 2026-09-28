"""정답 영상(구도 framing_*, 카메라 모션 motion_*)을 분석기로 분석해 샷 크기·모션·조준점 오차를 잰다."""
from __future__ import annotations

import json
import os
import sys

from common import dataset_video, latest_dataset, run_analyzer, use_utf8_output
from upt_pose_analyzer_eval import evaluate


def _run(dataset: str, output_dir: str) -> dict:
    data_dir = latest_dataset(dataset)
    if not data_dir:
        return {"skipped": f"정답 데이터 없음({dataset})"}
    video, truth = dataset_video(data_dir)
    if not video:
        return {"skipped": f"정답 영상 없음({os.path.basename(data_dir)})"}
    os.makedirs(output_dir, exist_ok=True)
    plan_file = os.path.join(output_dir, f"{dataset}_plan.json")
    run_analyzer(video, plan_file)
    with open(plan_file, encoding="utf-8") as handle:
        plan = json.load(handle)
    result = evaluate(plan, truth)
    metrics = dict(result["metrics"])
    metrics["dataset"] = os.path.basename(data_dir)
    metrics["identities"] = ((plan.get("analysis_notes") or {}).get("identities") or {}).get("count")
    # 구도·모션 정답 영상은 한 명만 나오므로 어깨 너머(OTS) 판정이 나오면 모두 오검출이다.
    metrics["ots_false"] = sum(1 for shot in plan["shots"] if shot.get("over_the_shoulder"))
    metrics["rows"] = result["rows"]
    return metrics


def run_framing(output_dir: str) -> dict:
    return _run("framing", output_dir)


def run_motion(output_dir: str) -> dict:
    return _run("motion", output_dir)


if __name__ == "__main__":
    use_utf8_output()
    target = sys.argv[1] if len(sys.argv) > 1 else "framing"
    print(json.dumps(_run(target, os.path.join(os.getcwd(), "eval_output")), ensure_ascii=False, indent=2))
