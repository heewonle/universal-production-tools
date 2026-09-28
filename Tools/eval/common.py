"""평가 스크립트 공용: 프로젝트 경로, 정답 데이터 위치, 포즈 모델 로드, 분석기 실행."""
from __future__ import annotations

import glob
import os
import subprocess
import sys

EVAL_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.dirname(EVAL_DIR)
PROJECT_DIR = os.path.abspath(os.path.join(TOOLS_DIR, "..", "..", ".."))
# 정답 데이터는 버전 관리에서 빠지는 Saved 아래에 있다. 없으면 editor/upt_capture_ground_truth.py로 에디터에서 만든다.
GROUND_TRUTH_DIR = os.path.join(PROJECT_DIR, "Saved", "UniversalProductionTools", "GroundTruth")
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

# 데이터셋별 폴더 이름 패턴. 앞쪽 패턴이 우선하며, 초기 구도 정답지는 접두사 없이 날짜로만 저장돼 있다.
DATASET_PATTERNS = {
    "framing": ["framing_*", "20[0-9][0-9][0-9][0-9][0-9][0-9]_*"],
    "motion": ["motion_*"],
    "size": ["size_2*"],
    "angle_train": ["angle_2*"],
    "angle_val": ["angleval_*"],
    "angle_test": ["angletest_*"],
    "twoshot": ["twoshot_*"],
    "letterbox": ["letterbox_*"],
    "bodypose": ["bodypose_*"],
    "height": ["height_*"],
    "retarget": ["retarget_*"],
    "peasant_video": ["peasant_video_*"],
}


def use_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass


def latest_dataset(name: str) -> str | None:
    for pattern in DATASET_PATTERNS[name]:
        matches = sorted(path for path in glob.glob(os.path.join(GROUND_TRUTH_DIR, pattern)) if os.path.isdir(path))
        if matches:
            return matches[-1]
    return None


def dataset_video(data_dir: str) -> tuple[str, dict] | tuple[None, None]:
    """정답 폴더의 truth JSON과 영상 경로. 영상 경로는 만들 때의 절대 경로 대신 폴더 기준으로 찾는다."""
    import json

    for truth_name in ("ground_truth.json", "twoshot_truth.json"):
        truth_file = os.path.join(data_dir, truth_name)
        if os.path.isfile(truth_file):
            with open(truth_file, encoding="utf-8") as handle:
                truth = json.load(handle)
            video = os.path.join(data_dir, os.path.basename(truth.get("video", "")))
            return (video, truth) if os.path.isfile(video) else (None, None)
    return None, None


_POSE_MODELS = None


def load_pose_models(mode: str = "balanced"):
    global _POSE_MODELS
    if _POSE_MODELS is None:
        from rtmlib import RTMPose, YOLOX
        from rtmlib.tools.solution.body import Body

        urls = Body.MODE[mode]
        _POSE_MODELS = (
            YOLOX(urls["det"], model_input_size=urls["det_input_size"], score_thr=0.5, backend="onnxruntime", device="cpu"),
            RTMPose(urls["pose"], model_input_size=urls["pose_input_size"], backend="onnxruntime", device="cpu"),
        )
    return _POSE_MODELS


def run_analyzer(video: str, output: str) -> None:
    command = [sys.executable, os.path.join(TOOLS_DIR, "upt_pose_reference_analyzer.py"), "--video", video, "--output", output]
    proc = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0 or not os.path.isfile(output):
        raise RuntimeError(f"분석기 실패: {(proc.stderr or proc.stdout)[-1500:]}")
