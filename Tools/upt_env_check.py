"""로컬 포즈 분석 환경 점검: 패키지 import, 포즈 모델 파일, 사람 영역 분할 모델, ffmpeg. --download-models면 빠진 모델을 받는다.

setup_analyzer_env.ps1이 분석 가상환경의 Python으로 실행한다. 문제가 있으면 종료 코드 1.
사람 영역 분할(어깨 너머 앞사람 검출)은 선택 기능이라 없으면 [주의]만 표시한다.
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import importlib
import os
import shutil
import sys
import tempfile
import urllib.request

MODULES = ("numpy", "cv2", "onnxruntime", "rtmlib", "scenedetect", "yt_dlp")
MODEL_CACHE = os.path.join(os.path.expanduser("~"), ".cache", "rtmlib", "hub", "checkpoints")
# 어깨 너머(OTS) 앞사람 검출용 DeepLab v3(Google MediaPipe 공개 모델, Apache-2.0, 약 2.8MB). 실행기는 ai-edge-litert.
SEGMENTATION_MODEL = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                                                  "ThirdParty", "UPTAnalyzer", "models", "deeplab_v3.tflite"))
SEGMENTATION_URL = "https://storage.googleapis.com/mediapipe-models/image_segmenter/deeplab_v3/float32/1/deeplab_v3.tflite"
SEGMENTATION_SHA256 = "ff36e24d40547fe9e645e2f4e8745d1876d6e38b332d39a82f0bf0f5d1d561b3"


def sha256_of(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_segmentation(download: bool) -> None:
    try:
        from ai_edge_litert.interpreter import Interpreter  # noqa: F401
        print("  [OK] ai_edge_litert")
    except Exception as error:
        print(f"  [주의] ai_edge_litert import 실패: {error} — 어깨 너머 앞사람 자동 검출만 꺼집니다.")
        return
    if not os.path.isfile(SEGMENTATION_MODEL) and download:
        print(f"  사람 영역 분할 모델을 받는 중(약 2.8MB, {SEGMENTATION_MODEL})...", flush=True)
        os.makedirs(os.path.dirname(SEGMENTATION_MODEL), exist_ok=True)
        temp = tempfile.NamedTemporaryFile(delete=False, suffix=".tflite", dir=os.path.dirname(SEGMENTATION_MODEL))
        temp.close()
        try:
            urllib.request.urlretrieve(SEGMENTATION_URL, temp.name)
            if sha256_of(temp.name) != SEGMENTATION_SHA256:
                print("  [주의] 받은 모델의 해시가 기대값과 달라 쓰지 않습니다.")
            else:
                os.replace(temp.name, SEGMENTATION_MODEL)
        except Exception as error:
            print(f"  [주의] 모델 다운로드 실패: {error}")
        finally:
            if os.path.exists(temp.name):
                os.remove(temp.name)
    if not os.path.isfile(SEGMENTATION_MODEL):
        print("  [주의] 사람 영역 분할 모델 없음 — 어깨 너머 앞사람 자동 검출만 꺼집니다(설치 모드로 실행하면 받습니다).")
    elif sha256_of(SEGMENTATION_MODEL) != SEGMENTATION_SHA256:
        print(f"  [주의] 사람 영역 분할 모델 해시가 다릅니다: {SEGMENTATION_MODEL}")
    else:
        print(f"  [OK] 사람 영역 분할 모델 {SEGMENTATION_MODEL}")


def module_version(module) -> str:
    version = getattr(module, "__version__", "")
    if not version and hasattr(module, "version"):
        version = getattr(module.version, "__version__", "")
    return str(version)


def model_files(mode: str) -> list[str]:
    from rtmlib.tools.solution.body import Body

    urls = Body.MODE[mode]
    return [os.path.splitext(os.path.basename(urls[key]))[0] + ".onnx" for key in ("det", "pose")]


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--download-models", action="store_true")
    parser.add_argument("--model", default="balanced", choices=("lightweight", "balanced", "performance"))
    args = parser.parse_args()
    problems = []

    print(f"  Python {sys.version.split()[0]} ({sys.executable})")
    if sys.version_info[:2] != (3, 13):
        print("  [주의] 고정한 패키지 버전은 Python 3.13 기준입니다. 다른 버전이면 설치가 실패할 수 있습니다.")

    for name in MODULES:
        try:
            print(f"  [OK] {name} {module_version(importlib.import_module(name))}")
        except Exception as error:  # 설치 누락·DLL 오류 등 원인이 다양하다.
            problems.append(f"{name} import 실패: {error}")
            print(f"  [문제] {name} import 실패: {error}")

    if not any(problem.startswith("rtmlib") for problem in problems):
        missing = [name for name in model_files(args.model) if not os.path.isfile(os.path.join(MODEL_CACHE, name))]
        if missing and args.download_models:
            print(f"  포즈 모델을 받는 중(약 155MB, {MODEL_CACHE})...", flush=True)
            try:
                from rtmlib import RTMPose, YOLOX
                from rtmlib.tools.solution.body import Body

                urls = Body.MODE[args.model]
                YOLOX(urls["det"], model_input_size=urls["det_input_size"], backend="onnxruntime", device="cpu")
                RTMPose(urls["pose"], model_input_size=urls["pose_input_size"], backend="onnxruntime", device="cpu")
            except Exception as error:
                print(f"  [문제] 모델 다운로드 실패: {error}")
            missing = [name for name in model_files(args.model) if not os.path.isfile(os.path.join(MODEL_CACHE, name))]
        if missing:
            problems.append("포즈 모델 없음: " + ", ".join(missing))
            print("  [문제] 포즈 모델 없음: " + ", ".join(missing) + (" (설치 모드로 실행하면 받습니다)" if not args.download_models else ""))
        else:
            print(f"  [OK] 포즈 모델({args.model}) {MODEL_CACHE}")

    check_segmentation(args.download_models)

    ffmpeg = shutil.which("ffmpeg") or next(iter(glob.glob(os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "WinGet", "Packages",
                                                                          "Gyan.FFmpeg*", "*", "bin", "ffmpeg.exe"))), None)
    if ffmpeg:
        print(f"  [OK] ffmpeg {ffmpeg}")
    else:
        # 포즈 분석 자체에는 필요 없고, 링크 구간 다운로드·Vision 모드·정답 영상 생성에만 쓴다.
        print("  [주의] ffmpeg 없음: 링크 구간 다운로드·Vision 분석·정답 영상 생성에 필요합니다. 설치: winget install Gyan.FFmpeg")

    if problems:
        print(f"\n  문제 {len(problems)}건")
        return 1
    print("\n  분석 환경 정상")
    return 0


if __name__ == "__main__":
    sys.exit(main())
