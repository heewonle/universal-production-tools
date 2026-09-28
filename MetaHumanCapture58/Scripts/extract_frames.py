"""영상에서 MetaHuman 캡처용 이미지 시퀀스를 뽑는다(언리얼 밖에서 실행).

- 세로 쇼츠의 검은 띠·고정 자막처럼 영상 내내 안 바뀌는 가장자리는 자동으로 잘라낸다(5.7 플러그인의 분석기 함수 재사용).
- 얼굴 캡처는 얼굴이 화면을 채울수록 잘 되므로, --face-crop을 주면 사람 얼굴 주변만 잘라 키운다.

사용(5.7 분석 가상환경의 Python으로):
  python extract_frames.py --video D:/clip.mp4 --name MyShot --start 5.0 --end 13.0
  python extract_frames.py --video D:/clip.mp4 --name MyShot --no-auto-crop
결과: <프로젝트>/Footage/<name>/frame.%04d.png  → 언리얼에서 Scripts/create_capture_data.py 로 에셋 생성
"""
from __future__ import annotations

import argparse
import glob
import os
import shutil
import subprocess
import sys

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# 5.7 팀 프로젝트의 분석 도구(검은 띠 검출)를 그대로 쓴다.
UPT_TOOLS = os.path.abspath(os.path.join(PROJECT_DIR, "..", "GoddessSlot", "Plugins", "UniversalProductionTools", "Tools"))


def find_ffmpeg() -> str | None:
    found = shutil.which("ffmpeg")
    if found:
        return found
    pattern = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "WinGet", "Packages", "Gyan.FFmpeg*", "*", "bin", "ffmpeg.exe")
    matches = glob.glob(pattern)
    return matches[0] if matches else None


def active_area(video: str) -> tuple[int, int, int, int] | None:
    """영상 내내 안 바뀌는 가장자리(검은 띠·고정 자막)를 뺀 실제 화면 영역."""
    if UPT_TOOLS not in sys.path:
        sys.path.insert(0, UPT_TOOLS)
    try:
        import cv2
        import upt_pose_reference_analyzer as analyzer
    except ImportError as error:
        print(f"[알림] 자동 잘라내기를 건너뜁니다({error}). 5.7 분석 가상환경의 Python으로 실행하면 동작합니다.")
        return None
    capture = cv2.VideoCapture(video)
    if not capture.isOpened():
        return None
    frame_count = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
    width, height = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH)), int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    area = analyzer.detect_active_area(capture, frame_count, width, height)
    capture.release()
    return area


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--video", required=True)
    parser.add_argument("--name", required=True, help="Footage 폴더 이름(에셋 이름에도 쓰인다)")
    parser.add_argument("--start", type=float, default=0.0)
    parser.add_argument("--end", type=float, default=0.0, help="0이면 끝까지")
    parser.add_argument("--no-auto-crop", action="store_true", help="검은 띠 자동 잘라내기를 끈다")
    args = parser.parse_args()

    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        print("FFmpeg를 찾지 못했습니다. winget install Gyan.FFmpeg 로 설치하세요.", file=sys.stderr)
        return 2
    if not os.path.isfile(args.video):
        print(f"영상을 찾지 못했습니다: {args.video}", file=sys.stderr)
        return 2

    out_dir = os.path.join(PROJECT_DIR, "Footage", args.name)
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    os.makedirs(out_dir, exist_ok=True)

    filters = []
    if not args.no_auto_crop:
        area = active_area(args.video)
        if area:
            x0, y0, x1, y1 = area
            filters.append(f"crop={x1 - x0}:{y1 - y0}:{x0}:{y0}")
            print(f"검은 띠를 빼고 자릅니다: x{x0}-{x1} y{y0}-{y1}")

    command = [ffmpeg, "-hide_banner", "-loglevel", "error"]
    if args.start:
        command += ["-ss", str(args.start)]
    if args.end:
        command += ["-to", str(args.end)]
    command += ["-i", args.video]
    if filters:
        command += ["-vf", ",".join(filters)]
    # 엔진은 "이름 + 구분자(_ 공백 -) + 숫자.확장자" 형식만 시퀀스로 인식한다(점으로 구분하면 0프레임으로 본다).
    command += ["-start_number", "0", os.path.join(out_dir, "frame_%04d.png")]
    subprocess.run(command, check=True)

    frames = sorted(os.listdir(out_dir))
    print(f"프레임 {len(frames)}장 → {out_dir}")
    if frames:
        print("다음: 언리얼에서 Scripts/create_capture_data.py 의 FOOTAGE 목록에 이 폴더를 넣고 실행하세요.")
    return 0 if frames else 1


if __name__ == "__main__":
    sys.exit(main())
