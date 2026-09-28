"""영상 링크 도구(upt_reference_link.py)의 안전장치 회귀 검사. 네트워크 접속·다운로드는 하지 않는다.

- 권리 확인 없는 YouTube 링크는 받기 전에 거부(종료 코드 3)
- 잘못된 주소·구간은 거부(종료 코드 2)
- 다운로드 허용 사이트 판별(비슷한 가짜 도메인은 허용하지 않음), 구간 문자열 해석
"""
from __future__ import annotations

import json
import os
import subprocess
import sys

from common import TOOLS_DIR, use_utf8_output

LINK_TOOL = os.path.join(TOOLS_DIR, "upt_reference_link.py")


def run(output_dir: str) -> dict:
    import upt_reference_link as link

    guard_dir = os.path.join(output_dir or os.getcwd(), "link_guard")

    def exit_code(arguments: list[str]) -> int:
        return subprocess.run([sys.executable, LINK_TOOL, *arguments, "--output-dir", guard_dir], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=120).returncode

    checks = [
        ("권리 확인 없는 YouTube 링크 거부", exit_code(["--url", "https://www.youtube.com/watch?v=R6MlUcmOul8"]) == 3),
        ("http/https가 아닌 주소 거부", exit_code(["--url", "ftp://example.com/a.mp4"]) == 2),
        ("끝이 시작보다 앞인 구간 거부", exit_code(["--url", "https://www.pexels.com/video/1/", "--section", "2:00-1:00"]) == 2),
    ]
    for url, expected in (("https://www.pexels.com/video/x-1/", True), ("https://cdn.pixabay.com/v.mp4", True), ("https://archive.org/details/x", True),
                          ("https://upload.wikimedia.org/a.webm", True), ("https://youtu.be/abc", False), ("https://evilpexels.com/x", False)):
        checks.append((f"허용 사이트 판별 {url} = {expected}", link.is_download_allowed(url) == expected))
    for text, expected in (("1:20-2:05", (80.0, 125.0)), ("90-120.5", (90.0, 120.5)), ("", None)):
        checks.append((f"구간 해석 '{text}'", link.parse_section(text) == expected))
    # 분석 영상 트랙 고르기: 세로 쇼츠는 짧은 변(가로) 기준 480까지 받고, 크기 정보가 없으면 형식 문자열에 맡긴다.
    vertical = {"formats": [{"format_id": "133", "width": 240, "height": 426, "vcodec": "avc1.4d4015", "tbr": 70},
                            {"format_id": "134", "width": 360, "height": 640, "vcodec": "avc1.4d401e", "tbr": 120},
                            {"format_id": "135", "width": 480, "height": 854, "vcodec": "avc1.4d401f", "tbr": 180, "protocol": "https"},
                            {"format_id": "231", "width": 480, "height": 854, "vcodec": "avc1.4D401E", "tbr": 250, "protocol": "m3u8_native"},
                            {"format_id": "788", "width": 608, "height": 1080, "vcodec": "av01.0.04M.08", "tbr": 100},
                            {"format_id": "140", "vcodec": "none", "acodec": "mp4a.40.2"}]}
    landscape = {"formats": [{"format_id": "135", "width": 854, "height": 480, "vcodec": "avc1.4d401f"},
                             {"format_id": "136", "width": 1280, "height": 720, "vcodec": "avc1.4d401f"},
                             {"format_id": "244", "width": 854, "height": 480, "vcodec": "vp9"}]}
    checks += [
        ("세로 쇼츠는 480x854 선택", link.pick_video_format(vertical, 480) == "135"),
        ("가로 영상은 854x480 H.264 선택", link.pick_video_format(landscape, 480) == "135"),
        ("크기 정보 없으면 형식 문자열 사용", link.pick_video_format({"formats": [{"format_id": "0", "vcodec": "avc1"}]}, 480) is None),
    ]
    failures = [name for name, ok in checks if not ok]
    return {"passed": len(checks) - len(failures), "total": len(checks), "failures": failures}


if __name__ == "__main__":
    use_utf8_output()
    print(json.dumps(run(os.path.join(os.getcwd(), "eval_output")), ensure_ascii=False, indent=2))
