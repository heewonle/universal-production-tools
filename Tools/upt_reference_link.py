"""영상 링크(유튜브·Pexels·Pixabay 등)를 레퍼런스로 쓰기 위해 영상을 받아 로컬 포즈 분석을 하고 분석 결과를 남긴다(API 불필요).

- 다운로드를 허용하는 사이트(Pexels, Pixabay, Internet Archive, Wikimedia Commons)의 영상은 받아서 분석하고 원본을 함께 보관한다.
- 그 밖의 사이트(YouTube 등)는 사용자가 '분석에 쓸 권리·허락이 있음'을 확인(--rights-confirmed)했을 때만 받는다.
  저해상도 영상 트랙만 임시 폴더에 받아 분석하고, 끝나면 영상과 분석용 프레임 이미지를 남기지 않는다.
  남는 것은 샷 구성 분석 JSON(reference_plan.json)과 출처 정보(source.json)뿐이다.

사용:
  python upt_reference_link.py --url URL --output-dir DIR [--section 1:20-2:05] [--rights-confirmed] [--ffmpeg PATH]
종료 코드: 0 성공, 2 입력·다운로드 오류, 3 권리 확인 필요, 4 분석 실패
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from urllib.parse import urlparse

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ANALYZER = os.path.join(TOOLS_DIR, "upt_pose_reference_analyzer.py")
# 라이선스상 내려받아 쓰는 것을 허용하는 사이트. 이 목록 밖의 링크는 권리 확인이 필요하다.
DOWNLOAD_ALLOWED_DOMAINS = ("pexels.com", "pixabay.com", "archive.org", "wikimedia.org")
MAX_SECONDS = 20 * 60


def print_utf8() -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass


def fail(code: int, kind: str, message: str) -> int:
    print(f"UPT_LINK_ERROR {kind}: {message}", file=sys.stderr, flush=True)
    return code


def is_download_allowed(url: str) -> bool:
    host = (urlparse(url).hostname or "").lower()
    return any(host == domain or host.endswith("." + domain) for domain in DOWNLOAD_ALLOWED_DOMAINS)


def parse_time(text: str) -> float:
    parts = [float(part) for part in text.strip().split(":")]
    if not parts or len(parts) > 3 or any(part < 0 for part in parts):
        raise ValueError(text)
    seconds = 0.0
    for part in parts:
        seconds = seconds * 60.0 + part
    return seconds


def parse_section(text: str) -> tuple[float, float] | None:
    """"1:20-2:05" → (80, 125). 비어 있으면 None."""
    text = (text or "").strip()
    if not text:
        return None
    match = re.fullmatch(r"([\d:.]+)\s*-\s*([\d:.]+)", text)
    if not match:
        raise ValueError("구간은 '시작-끝' 형식이어야 합니다(예: 1:20-2:05).")
    start, end = parse_time(match.group(1)), parse_time(match.group(2))
    if end <= start:
        raise ValueError("구간의 끝이 시작보다 뒤여야 합니다.")
    return start, end


def analyzer_signature() -> str:
    with open(ANALYZER, encoding="utf-8") as handle:
        version = re.search(r'ANALYZER_VERSION\s*=\s*"([^"]+)"', handle.read())
    return f"{version.group(1) if version else 'unknown'}:{int(os.path.getmtime(ANALYZER))}"


def safe_title(title: str) -> str:
    cleaned = "".join(ch if ch.isalnum() or ch in "_-" else "_" for ch in (title or ""))
    return re.sub(r"_+", "_", cleaned).strip("_")[:60] or "LinkReference"


def resolve_ffmpeg(setting: str) -> str | None:
    """설정값(전체 경로 또는 'ffmpeg.exe' 같은 이름)을 실제 실행 파일 경로로 바꾼다.

    yt-dlp는 ffmpeg_location에 이름만 주면 현재 폴더 기준 경로로 보고 '없음' 처리하므로 반드시 전체 경로로 넘긴다.
    에디터가 PATH 갱신 전에 켜졌을 수 있어 winget 설치 위치도 찾아본다.
    """
    setting = (setting or "").strip().strip('"')
    if setting and os.path.isfile(setting):
        return os.path.abspath(setting)
    for name in filter(None, (setting, "ffmpeg")):
        found = shutil.which(name)
        if found:
            return found
    local = os.environ.get("LOCALAPPDATA", "")
    if local:
        candidates = [os.path.join(local, "Microsoft", "WinGet", "Links", "ffmpeg.exe")]
        packages = os.path.join(local, "Microsoft", "WinGet", "Packages")
        if os.path.isdir(packages):
            for package in sorted(os.listdir(packages)):
                if "ffmpeg" not in package.lower():
                    continue
                for root, _dirs, names in os.walk(os.path.join(packages, package)):
                    if "ffmpeg.exe" in names:
                        candidates.append(os.path.join(root, "ffmpeg.exe"))
        for candidate in candidates:
            if os.path.isfile(candidate):
                return candidate
    return None


def pick_video_format(info: dict, max_short_side: int) -> str | None:
    """분석에 쓸 영상 트랙 format_id. 짧은 변이 max_short_side 이하인 것 중 OpenCV가 읽기 쉬운 코덱·높은 해상도를 고른다.

    yt-dlp 형식 문자열의 height<=480은 세로 영상(쇼츠)에서 긴 변 기준이라 240x426까지 떨어진다(480x854가 있어도).
    포맷 목록에 크기 정보가 없으면(직접 링크 등) None을 돌려 기존 형식 문자열을 쓴다.
    """
    candidates = []
    for fmt in info.get("formats") or []:
        width, height, codec = fmt.get("width"), fmt.get("height"), (fmt.get("vcodec") or "").lower()
        if codec in ("", "none") or not width or not height or min(width, height) > max_short_side:
            continue
        readable = codec.startswith(("avc", "h264", "vp9", "vp09"))
        # 같은 해상도면 조각 스트림(m3u8)보다 파일 하나로 받는 형식이 구간 받기·재시도에 안정적이다.
        progressive = "m3u8" not in (fmt.get("protocol") or "")
        candidates.append((readable, min(width, height), codec.startswith(("avc", "h264")), progressive, fmt.get("tbr") or 0, str(fmt.get("format_id"))))
    return max(candidates)[-1] if candidates else None


def download_video(url: str, temp_dir: str, max_height: int, section: tuple[float, float] | None, ffmpeg: str | None) -> tuple[str, dict]:
    import yt_dlp

    options = {
        "quiet": True, "no_warnings": True, "noplaylist": True, "retries": 3, "noprogress": True,
        "outtmpl": os.path.join(temp_dir, "video.%(ext)s"),
        # 포즈 분석에는 소리가 필요 없고 480p면 충분하다. 영상 트랙만 받아 용량과 병합 과정을 줄인다.
        "format": f"bv*[height<={max_height}][ext=mp4]/bv*[height<={max_height}]/b[height<={max_height}]/wv*/w",
        "max_filesize": 2 * 1024 ** 3,
    }
    if ffmpeg:
        options["ffmpeg_location"] = ffmpeg
    with yt_dlp.YoutubeDL(options) as ydl:
        info = ydl.extract_info(url, download=False)
    if info.get("_type") in ("playlist", "multi_video"):
        raise ValueError("재생목록 링크입니다. 영상 하나의 링크를 넣어 주세요.")
    duration = info.get("duration") or 0.0
    if section and duration and section[0] >= duration:
        raise ValueError(f"구간 시작이 영상 길이({duration:.0f}초)보다 뒤입니다.")
    length = (min(section[1], duration or section[1]) - section[0]) if section else duration
    if length and length > MAX_SECONDS:
        raise ValueError(f"분석할 길이가 {length / 60:.0f}분입니다. 구간을 {MAX_SECONDS // 60}분 이하로 지정하세요(예: 1:20-3:00).")

    picked = pick_video_format(info, max_height)
    if picked:
        options["format"] = picked
    if section:
        options["download_ranges"] = yt_dlp.utils.download_range_func(None, [section])
        options["force_keyframes_at_cuts"] = True
    print(f"UPT_LINK_PROGRESS downloading {info.get('extractor_key', '')} {duration:.0f}s", flush=True)
    with yt_dlp.YoutubeDL(options) as ydl:
        ydl.extract_info(url, download=True)
    files = [name for name in os.listdir(temp_dir) if name.startswith("video.") and not name.endswith((".part", ".ytdl"))]
    if not files:
        raise RuntimeError("영상 파일을 받지 못했습니다.")
    return os.path.join(temp_dir, files[0]), info


def main() -> int:
    print_utf8()
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--section", default="", help="분석할 구간(예: 1:20-2:05). 비우면 전체")
    parser.add_argument("--rights-confirmed", action="store_true", help="다운로드 비허용 사이트 영상을 분석에 쓸 권리·허락이 있음")
    parser.add_argument("--ffmpeg", default="")
    parser.add_argument("--max-height", type=int, default=480)
    parser.add_argument("--mode", default="scene", choices=("scene", "edit"))
    parser.add_argument("--model", default="balanced", choices=("lightweight", "balanced", "performance"))
    parser.add_argument("--samples-per-shot", type=int, default=4)
    args = parser.parse_args()

    url = args.url.strip()
    if not re.match(r"^https?://", url) or any(ch.isspace() for ch in url):
        return fail(2, "invalid_url", "영상 링크는 http:// 또는 https://로 시작해야 합니다.")
    try:
        section = parse_section(args.section)
    except ValueError as error:
        return fail(2, "invalid_section", str(error))
    allowed = is_download_allowed(url)
    if not allowed and not args.rights_confirmed:
        return fail(3, "rights_required", "다운로드를 허용하지 않는 사이트(YouTube 등)의 영상입니다. 분석에 쓸 권리·허락이 있을 때만 확인 후 다시 시도하세요.")

    output_dir = os.path.abspath(args.output_dir)
    os.makedirs(output_dir, exist_ok=True)
    plan_file = os.path.join(output_dir, "reference_plan.json")
    source_file = os.path.join(output_dir, "source.json")
    signature = analyzer_signature()
    request_key = hashlib.md5(f"{url}|{section}|{args.mode}|{args.model}|{args.samples_per_shot}|{signature}".encode("utf-8")).hexdigest()
    if os.path.isfile(plan_file) and os.path.isfile(source_file):
        with open(source_file, encoding="utf-8") as handle:
            if json.load(handle).get("request_key") == request_key:
                print(f"UPT_REFERENCE_PLAN={plan_file} cached", flush=True)
                return 0

    ffmpeg = resolve_ffmpeg(args.ffmpeg)
    if section and not ffmpeg:
        return fail(2, "ffmpeg_missing", "구간 분석에는 FFmpeg가 필요합니다. 설치 후 PATH에 추가하거나 Editor Preferences > Universal Production Tools에서 전체 경로를 지정하세요.")
    temp_dir = tempfile.mkdtemp(prefix="upt_link_")
    retained_video = ""
    try:
        try:
            video, info = download_video(url, temp_dir, args.max_height, section, ffmpeg)
        except Exception as error:  # yt-dlp는 사이트·네트워크마다 다른 예외를 던진다.
            return fail(2, "download_failed", f"{type(error).__name__}: {str(error)[-600:]}")

        command = [sys.executable, ANALYZER, "--video", video, "--output", plan_file, "--mode", args.mode,
                   "--model", args.model, "--samples-per-shot", str(args.samples_per_shot)]
        if allowed:
            # 다운로드 허용 사이트는 원본과 관절 오버레이 이미지를 보관해 결과를 눈으로 확인할 수 있게 한다.
            command += ["--debug-dir", os.path.join(output_dir, "debug")]
        print("UPT_LINK_PROGRESS analyzing", flush=True)
        proc = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=3600)
        if proc.returncode != 0 or not os.path.isfile(plan_file):
            return fail(4, "analysis_failed", (proc.stderr or proc.stdout)[-1200:])

        with open(plan_file, encoding="utf-8") as handle:
            plan = json.load(handle)
        plan["title"] = safe_title(info.get("title", ""))
        notes = plan.setdefault("analysis_notes", {})
        notes["source_url"] = url
        notes["source_title"] = info.get("title", "")
        if section:
            notes["section_seconds"] = list(section)
        with open(plan_file, "w", encoding="utf-8") as handle:
            json.dump(plan, handle, ensure_ascii=False, indent=2)

        if allowed:
            retained_video = os.path.join(output_dir, "source_video" + os.path.splitext(video)[1])
            shutil.move(video, retained_video)
        source = {
            "url": url, "title": info.get("title", ""), "uploader": info.get("uploader", ""), "extractor": info.get("extractor_key", ""),
            "duration": info.get("duration"), "section_seconds": list(section) if section else None, "license": info.get("license", ""),
            "download_allowed_site": allowed, "rights_confirmed": bool(args.rights_confirmed), "video_retained": bool(retained_video),
            "retrieved_at": time.strftime("%Y-%m-%d %H:%M:%S"), "analyzer": signature, "request_key": request_key,
        }
        with open(source_file, "w", encoding="utf-8") as handle:
            json.dump(source, handle, ensure_ascii=False, indent=2)
        print(f"UPT_REFERENCE_PLAN={plan_file}", flush=True)
        return 0
    finally:
        # 권리 확인으로 받은 영상은 분석이 끝나면(실패해도) 임시 폴더째 지운다.
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
