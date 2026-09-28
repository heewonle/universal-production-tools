"""레터박스 정답 영상을 만든다(쇼츠처럼 위아래 검은 띠 + 고정 자막이 있는 세로 영상).

구도 정답 영상(framing)을 608x1080 세로 화면 가운데에 넣고, 띠 영역에 고정 도형을 그린다.
정답 데이터는 버전 관리에서 빠지는 Saved 아래에 있으므로 필요할 때 이 스크립트로 다시 만든다.

  ThirdParty\\UPTAnalyzer\\.venv\\Scripts\\python.exe Plugins\\UniversalProductionTools\\Tools\\eval\\make_letterbox_dataset.py
"""
from __future__ import annotations

import glob
import os
import shutil
import subprocess
import sys
import time

from common import GROUND_TRUTH_DIR, dataset_video, latest_dataset, use_utf8_output

# 세로 화면 608x1080 가운데(y=311~653)에 16:9 영상을 넣고, 띠에 고정 도형 세 개를 그린다.
FILTER = ("scale=608:342,pad=608:1080:0:311:black,"
          "drawbox=x=60:y=120:w=300:h=26:color=white@0.95:t=fill,"
          "drawbox=x=60:y=170:w=180:h=18:color=gray@0.9:t=fill,"
          "drawbox=x=40:y=980:w=200:h=36:color=white@0.85:t=fill")


def find_ffmpeg() -> str | None:
    found = shutil.which("ffmpeg")
    if found:
        return found
    pattern = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "WinGet", "Packages",
                           "Gyan.FFmpeg*", "*", "bin", "ffmpeg.exe")
    matches = glob.glob(pattern)
    return matches[0] if matches else None


def main() -> int:
    use_utf8_output()
    ffmpeg = find_ffmpeg()
    if not ffmpeg:
        print("FFmpeg를 찾지 못했습니다. winget install Gyan.FFmpeg", file=sys.stderr)
        return 2
    framing_dir = latest_dataset("framing")
    source = dataset_video(framing_dir)[0] if framing_dir else None
    if not source:
        print("구도 정답 영상(framing)이 없습니다. 에디터에서 먼저 만들어 주세요.", file=sys.stderr)
        return 2

    target_dir = os.path.join(GROUND_TRUTH_DIR, f"letterbox_{time.strftime('%Y%m%d')}")
    os.makedirs(target_dir, exist_ok=True)
    target = os.path.join(target_dir, "letterbox_truth.mp4")
    subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", source,
                    "-vf", FILTER, "-c:v", "libx264", "-pix_fmt", "yuv420p", target], check=True)
    print(f"만들었습니다: {target} (원본 {os.path.basename(source)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
