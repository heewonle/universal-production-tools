"""UniversalProductionTools 분석 도구 회귀 검사: 모든 평가를 실행하고 baseline.json 기준보다 나빠졌는지 확인한다.

사용(프로젝트 폴더에서, 분석 가상환경의 Python으로):
  ThirdParty\\UPTAnalyzer\\.venv\\Scripts\\python.exe Plugins\\UniversalProductionTools\\Tools\\eval\\run_regression.py
  ... run_regression.py --only size_set,angle_sets        # 일부만
  ... run_regression.py --output-dir D:\\Temp\\upt_eval    # 결과 저장 위치 지정

- 결과: 항목별 OK / FAIL / SKIP 표와 report.json(기본 Saved/UniversalProductionTools/Eval/<시각>/). 하나라도 FAIL이면 종료 코드 1.
- 정답 데이터는 Saved/UniversalProductionTools/GroundTruth(버전 관리 제외)에 있어야 한다. 없는 항목은 SKIP(실패 아님)이며,
  에디터에서 editor/upt_capture_ground_truth.py로 다시 만들 수 있다.
- 분석기를 고쳐 수치가 좋아졌거나 의도적으로 바뀌면 baseline.json을 직접 고치고 이유를 남긴다.
- 전체 실행은 CPU에서 몇 분 걸린다(정답 영상 분석·라이브러리 색인 포함).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import traceback

EVAL_DIR = os.path.dirname(os.path.abspath(__file__))
if EVAL_DIR not in sys.path:
    sys.path.insert(0, EVAL_DIR)

from common import PROJECT_DIR, use_utf8_output  # noqa: E402
import eval_angle  # noqa: E402
import eval_body_height  # noqa: E402
import eval_body_pose  # noqa: E402
import eval_library  # noqa: E402
import eval_letterbox  # noqa: E402
import eval_link_guards  # noqa: E402
import eval_rotation  # noqa: E402
import eval_size  # noqa: E402
import eval_twoshot  # noqa: E402
import eval_video_gt  # noqa: E402

SUITES = {
    "link_guards": eval_link_guards.run,
    "framing_video": eval_video_gt.run_framing,
    "letterbox_video": eval_letterbox.run,
    "motion_video": eval_video_gt.run_motion,
    "size_set": eval_size.run,
    "tilted_bodies": eval_rotation.run,
    "angle_sets": eval_angle.run,
    "twoshot_video": eval_twoshot.run,
    "library_search": eval_library.run,
    "body_pose": eval_body_pose.run,
    "body_height": eval_body_height.run,
}


def lookup(metrics: dict, dotted: str):
    value = metrics
    for part in dotted.split("."):
        if not isinstance(value, dict) or part not in value:
            raise KeyError(dotted)
        value = value[part]
    return value


def find_skip(metrics: dict, dotted: str) -> str | None:
    """경로 중간에 'skipped'가 있으면(정답 데이터 없음) 그 사유를 돌려준다."""
    value = metrics
    for part in dotted.split("."):
        if isinstance(value, dict) and "skipped" in value:
            return value["skipped"]
        if not isinstance(value, dict) or part not in value:
            return None
        value = value[part]
    return None


def check(value, rule: dict) -> tuple[bool, str]:
    if value is None:
        return False, "값 없음"
    if "min" in rule:
        return value >= rule["min"], f">= {rule['min']}"
    if "max" in rule:
        return value <= rule["max"], f"<= {rule['max']}"
    return value == rule["equals"], f"== {rule['equals']}"


def main() -> int:
    use_utf8_output()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--only", default="", help="쉼표로 구분한 항목 이름: " + ", ".join(SUITES))
    parser.add_argument("--output-dir", default="")
    parser.add_argument("--baseline", default=os.path.join(EVAL_DIR, "baseline.json"))
    args = parser.parse_args()

    selected = [name.strip() for name in args.only.split(",") if name.strip()] or list(SUITES)
    unknown = [name for name in selected if name not in SUITES]
    if unknown:
        print(f"알 수 없는 항목: {', '.join(unknown)} (가능: {', '.join(SUITES)})", file=sys.stderr)
        return 2
    output_dir = os.path.abspath(args.output_dir or os.path.join(PROJECT_DIR, "Saved", "UniversalProductionTools", "Eval", time.strftime("%Y%m%d_%H%M%S")))
    os.makedirs(output_dir, exist_ok=True)
    with open(args.baseline, encoding="utf-8") as handle:
        baseline = json.load(handle)

    report, lines, failed = {"output_dir": output_dir, "suites": {}}, [], 0
    for name in selected:
        started = time.time()
        print(f"[{name}] 실행 중...", flush=True)
        try:
            metrics = SUITES[name](os.path.join(output_dir, name))
            error = None
        except Exception:
            metrics, error = {}, traceback.format_exc(limit=3)
        elapsed = round(time.time() - started, 1)
        results = []
        if error:
            failed += 1
            results.append({"rule": "(실행)", "status": "FAIL", "detail": error.strip().splitlines()[-1]})
        elif "skipped" in metrics:
            results.append({"rule": "(전체)", "status": "SKIP", "detail": metrics["skipped"]})
        else:
            for dotted, rule in baseline.get(name, {}).items():
                skip = find_skip(metrics, dotted)
                if skip:
                    results.append({"rule": dotted, "status": "SKIP", "detail": skip})
                    continue
                try:
                    value = lookup(metrics, dotted)
                except KeyError:
                    value = None
                ok, expectation = check(value, rule)
                failed += int(not ok)
                results.append({"rule": dotted, "status": "OK" if ok else "FAIL", "value": value, "expect": expectation})
        report["suites"][name] = {"elapsed_seconds": elapsed, "results": results, "metrics": metrics, "error": error}
        for result in results:
            detail = result.get("detail") or f"{result.get('value')} (기준 {result.get('expect')})"
            lines.append(f"  {result['status']:4s}  {name:15s} {result['rule']:32s} {detail}")
        print(f"[{name}] {elapsed}초", flush=True)

    with open(os.path.join(output_dir, "report.json"), "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2, default=str)
    print("\n회귀 검사 결과")
    print("\n".join(lines))
    print(f"\n{'실패 ' + str(failed) + '건' if failed else '모두 통과'} | 상세: {os.path.join(output_dir, 'report.json')}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
