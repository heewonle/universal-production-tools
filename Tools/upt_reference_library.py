"""레퍼런스 영상 라이브러리: 폴더 속 영상을 로컬 포즈 분석으로 색인하고, 글로 적은 연출 요구와 샷 구성이 가장 비슷한 영상 구간을 찾는다(API 불필요).

사용:
  python upt_reference_library.py index  --library <폴더>
  python upt_reference_library.py parse  --prompt "투샷으로 시작해 어깨 너머 대화, 클로즈업으로 마무리"
  python upt_reference_library.py search --library <폴더> --prompt "..." [--top 5] [--output result.json]

검색 순서
1) 프롬프트를 절(→ , 그리고 다음 마지막에 ...)로 나눠 샷 요구 목록(샷 크기·앵글·카메라 모션·인원)으로 바꾼다.
2) 영상마다 샷 순서를 국소 정렬(Smith-Waterman)해 요구 순서와 가장 잘 맞는 연속 구간을 고른다. 영상 쪽 중간 샷이 끼어드는 것은 작게, 요구 샷이 빠지는 것은 크게 깎는다.
3) 고른 구간은 시간을 0부터 다시 매긴 Reference Plan JSON으로 저장해 시네마틱 생성에 바로 쓸 수 있다.
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
import urllib.request

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ANALYZER = os.path.join(TOOLS_DIR, "upt_pose_reference_analyzer.py")
VIDEO_EXTENSIONS = (".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v")
INDEX_DIR = ".upt_library"
INDEX_VERSION = 1

SIZE_ORDER = ["extreme_close_up", "close_up", "medium", "full", "wide"]
ANGLE_ORDER = ["low", "eye", "high", "overhead"]

# (정규식, 값). 같은 표 안에서는 앞쪽이 우선한다(예: "전신 와이드"는 wide, "익스트림 클로즈업"은 extreme_close_up).
SIZE_PATTERNS = [
    (r"익스트림\s*클로즈|초근접|초접사|extreme[\s-]*close|\becu\b|눈만|입만", "extreme_close_up"),
    (r"클로즈\s*업|얼굴|close[\s-]*up|\bcu\b", "close_up"),
    (r"미디엄|바스트|상반신|허리\s*위|medium|waist|bust", "medium"),
    (r"와이드|롱\s*샷|원경|설정\s*샷|wide|establishing|long[\s-]*shot", "wide"),
    (r"풀\s*샷|전신|full[\s-]*(shot|body)", "full"),
]
ANGLE_PATTERNS = [
    (r"부감|탑\s*뷰|버드\s*아이|수직으로\s*내려|overhead|top[\s-]*down|bird'?s?[\s-]*eye", "overhead"),
    (r"로우\s*앵글|아래에서\s*올려|올려\s*다|low[\s-]*angle|worm'?s?[\s-]*eye", "low"),
    (r"하이\s*앵글|위에서\s*내려|내려\s*다|high[\s-]*angle", "high"),
    (r"눈\s*높이|아이\s*레벨|eye[\s-]*level", "eye"),
]
MOTION_PATTERNS = [
    (r"돌리\s*인|다가가|다가오|밀고\s*들어|push[\s-]*in|dolly[\s-]*in", "dolly_in"),
    (r"돌리\s*아웃|멀어지|물러나|pull[\s-]*(out|back)|dolly[\s-]*out", "dolly_out"),
    (r"줌\s*인|zoom[\s-]*in", "zoom_in"),
    (r"줌\s*아웃|zoom[\s-]*out", "zoom_out"),
    (r"핸드\s*헬드|흔들리|들고\s*찍|handheld|hand[\s-]*held|shaky", "handheld"),
    (r"궤도|돌아가며|돌면서|주위를\s*돌|오비트|orbit|arc[\s-]*shot", "orbit"),
    (r"트래킹|따라가|따라\s*움직|tracking|follow", "tracking"),
    (r"틸트|tilt", "tilt"),
    (r"패닝|팬\s*(으로|샷|해|하)|훑|\bpan\b|panning", "pan"),
    (r"고정\s*(샷|카메라)|움직이지\s*않|static|locked[\s-]*off", "static"),
]
# 어깨 너머 샷은 앞사람이 잘려 한 명만 검출되는 경우가 많아 "인원 1명 이상 + 대화 장면(두 명)"으로 본다.
PEOPLE_PATTERNS = [
    (r"어깨\s*너머|오버\s*숄더|over[\s-]*the[\s-]*shoulder|\bots\b", "ots"),
    (r"투\s*샷|두\s*사람|두\s*명|2\s*인|둘이|마주|대화|two[\s-]*shot|two\s*people|dialogue|conversation", 2),
    (r"군중|여러\s*명|(?<!마)무리|그룹|crowd|group", 3),
    (r"혼자|단독|한\s*명|홀로|\bsolo\b|alone|single", 1),
]
# 절을 나누는 연결어. "다가가(돌리 인)"를 자르지 않도록 "다가" 뒤에 가/오가 오면 나누지 않는다.
SPLIT_PATTERN = re.compile(
    r"->|→|⇒|,|，|;|\bthen\b|\bfinally\b|\bafter\s+that\b|그리고|그\s*다음|다음(?:에|은)?|이어서|이후|뒤에|후에|"
    r"마지막(?:에|으로|은)?|(?:으로|로)\s*시작(?:해서|해|하고)?|시작해서|하다가|했다가|다가(?![가오])|하고", re.IGNORECASE)

RELATED_MOTIONS = {
    frozenset(("dolly_in", "zoom_in")): 0.6, frozenset(("dolly_out", "zoom_out")): 0.6,
    frozenset(("pan", "tracking")): 0.5, frozenset(("pan", "tilt")): 0.4, frozenset(("orbit", "tracking")): 0.5,
    frozenset(("orbit", "pan")): 0.4, frozenset(("static", "handheld")): 0.2,
}

MATCH_OFFSET = 0.55   # 샷 유사도가 이보다 높아야 정렬 점수가 오른다(완벽히 맞는 샷 하나 = +0.45).
# 요구에 없는 영상 샷이 구간 중간에 끼어들 때. "와이드 → 클로즈업" 사이에 샷 두세 개가 있어도 이어지게 작게 두되,
# 긴 영상에서 멀리 떨어진 샷끼리(샷 5개 이상 간격) 억지로 잇지는 않도록 0은 아니게 둔다.
SKIP_VIDEO = 0.1
SKIP_REQUEST = 0.6    # 요구한 샷이 영상에 없을 때


# 규칙이 못 알아들은 절만 로컬 LLM에 물어본다(선택). 없으면 규칙 결과만 쓴다.
DEFAULT_LLM_ENDPOINT = "http://localhost:11434/v1/chat/completions"
DEFAULT_LLM_MODEL = "qwen3-vl:8b-instruct"
LLM_TIMEOUT_SECONDS = 90  # 로컬 모델을 처음 올릴 때 30초를 넘기는 경우가 있다(그 뒤로는 몇 초).
LLM_ASSIST: dict = {}  # main이 채운다: {"endpoint": ..., "model": ...}
ALLOWED_VALUES = {
    "size": {"extreme_close_up", "close_up", "medium", "full", "wide"},
    "angle": {"low", "eye", "high", "overhead", "dutch"},
    "motion": {"static", "pan", "tilt", "dolly_in", "dolly_out", "zoom_in", "zoom_out",
               "truck_left", "truck_right", "pedestal", "orbit", "tracking", "handheld"},
}
LLM_SYSTEM_PROMPT = (
    "You convert one film shot description into JSON. Korean or English input.\n"
    'Reply ONLY with {"clauses":[{"index":<int>,"size":<v|null>,"angle":<v|null>,"motion":<v|null>,'
    '"people":<1|2|3|null>,"ots":<true|false>}]}\n'
    "size: extreme_close_up, close_up, medium, full, wide\n"
    "angle: low, eye, high, overhead, dutch\n"
    "motion: static, pan, tilt, dolly_in, dolly_out, zoom_in, zoom_out, truck_left, truck_right, pedestal, orbit, tracking, handheld\n"
    "people: how many people are on screen. ots: true only for over-the-shoulder framing.\n"
    "Use null when the clause does not say. Never invent values, never add other keys or text."
)


def clean_llm_shot(raw: dict) -> dict:
    """LLM이 준 값 중 허용된 것만 남긴다(엉뚱한 값·키는 버린다)."""
    shot = {}
    if not isinstance(raw, dict):
        return shot
    for key, allowed in ALLOWED_VALUES.items():
        value = raw.get(key)
        if isinstance(value, str) and value.strip().lower() in allowed:
            shot[key] = value.strip().lower()
    people = raw.get("people")
    if isinstance(people, bool):
        people = None
    if isinstance(people, (int, float)) and 1 <= int(people) <= 3:
        shot["people"] = int(people)
    if raw.get("ots") is True:
        shot["people"], shot["ots"] = 1, True
        shot.setdefault("size", "medium")
    return shot


def llm_parse_clauses(clauses: list[tuple[int, str]]) -> dict[int, dict]:
    """규칙이 못 알아들은 절들을 한 번에 로컬 LLM에 물어 {절 번호: 샷 요구}로 돌려준다. 실패하면 빈 결과."""
    endpoint, model = LLM_ASSIST.get("endpoint"), LLM_ASSIST.get("model")
    if not endpoint or not model or not clauses:
        return {}
    payload = {
        "model": model, "stream": False, "temperature": 0,
        "messages": [
            {"role": "system", "content": LLM_SYSTEM_PROMPT},
            {"role": "user", "content": json.dumps([{"index": index, "text": text} for index, text in clauses], ensure_ascii=False)},
        ],
    }
    request = urllib.request.Request(endpoint, data=json.dumps(payload).encode("utf-8"),
                                     headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=LLM_TIMEOUT_SECONDS) as response:
            body = json.loads(response.read().decode("utf-8", errors="replace"))
        content = ((body.get("choices") or [{}])[0].get("message") or {}).get("content") or ""
        start, end = content.find("{"), content.rfind("}")
        answer = json.loads(content[start:end + 1]) if start >= 0 < end else {}
    except Exception as error:  # 로컬 모델 미실행·응답 지연·형식 오류 등
        print(f"UPT_LIBRARY_LLM off ({type(error).__name__}: {str(error)[:150]})", file=sys.stderr, flush=True)
        return {}
    known = {index for index, _ in clauses}
    parsed = {}
    for item in answer.get("clauses") or []:
        index = item.get("index") if isinstance(item, dict) else None
        if not isinstance(index, int) or index not in known:
            continue
        shot = clean_llm_shot(item)
        if shot:
            parsed[index] = shot
    print(f"UPT_LIBRARY_LLM parsed {len(parsed)}/{len(clauses)} clauses", file=sys.stderr, flush=True)
    return parsed


def first_match(text: str, patterns: list) -> tuple[object, str] | tuple[None, None]:
    for pattern, value in patterns:
        found = re.search(pattern, text, re.IGNORECASE)
        if found:
            return value, found.group(0)
    return None, None


def parse_prompt(prompt: str) -> dict:
    """프롬프트 → {"shots": [샷 요구...], "keywords": [...], "people_required": n}."""
    entries, used_text = [], []
    for clause in SPLIT_PATTERN.split(prompt):
        clause = (clause or "").strip()
        if not clause:
            continue
        request = {"text": clause}
        for key, patterns in (("size", SIZE_PATTERNS), ("angle", ANGLE_PATTERNS), ("motion", MOTION_PATTERNS)):
            value, matched = first_match(clause, patterns)
            if value:
                request[key] = value
                used_text.append(matched)
        people, matched = first_match(clause, PEOPLE_PATTERNS)
        if people == "ots":
            request["people"], request["ots"] = 1, True
            request.setdefault("size", "medium")
            used_text.append(matched)
        elif people:
            request["people"] = people
            used_text.append(matched)
        entries.append(request)
    # 규칙이 아무것도 못 알아들은 절만 로컬 LLM에 물어 채운다(있을 때만, 실패하면 규칙 결과 그대로).
    unresolved = [(index, entry["text"]) for index, entry in enumerate(entries) if len(entry) == 1]
    for index, shot in llm_parse_clauses(unresolved).items():
        entries[index].update(shot)
        entries[index]["source"] = "llm"
    shots = [entry for entry in entries if len(entry) > 1]
    people_required = max([2 if s.get("ots") else s.get("people", 1) for s in shots] or [1])
    remainder = prompt
    for text in used_text:
        remainder = remainder.replace(text, " ")
    keywords = []
    for word in re.findall(r"[0-9A-Za-z가-힣]{2,}", SPLIT_PATTERN.sub(" ", remainder).lower()):
        # 파일 이름과 비교할 낱말만 남긴다. 한국어 조사(으로·에서·은/는 등)는 떼어 낸다.
        word = re.sub(r"(으로|에서|에게|하며|하고|해서|까지|부터|이랑|에|은|는|이|가|을|를|과|와|로|의|도)$", "", word) if re.search(r"[가-힣]", word) else word
        if len(word) >= 2 and word not in STOPWORDS:
            keywords.append(word)
    return {"shots": shots, "keywords": keywords, "people_required": people_required}


STOPWORDS = {"마무리", "시작", "그리고", "다음", "마지막", "장면", "카메라", "촬영", "영상", "the", "and", "with", "shot", "then", "to", "in", "of",
             "finally", "into", "from", "끝", "끝나", "서있", "있다"}


def shot_similarity(request: dict, shot: dict) -> float:
    total, weight = 0.0, 0.0
    if request.get("size"):
        distance = abs(SIZE_ORDER.index(request["size"]) - SIZE_ORDER.index(shot["size"])) if shot["size"] in SIZE_ORDER else 4
        total += 1.0 * max(0.0, 1.0 - distance / 2.0)
        weight += 1.0
    if request.get("angle"):
        if shot["angle"] == request["angle"]:
            score = 1.0
        elif shot["angle"] in ANGLE_ORDER and abs(ANGLE_ORDER.index(shot["angle"]) - ANGLE_ORDER.index(request["angle"])) == 1:
            score = 0.35
        else:
            score = 0.0
        total += 0.7 * score
        weight += 0.7
    if request.get("motion"):
        score = 1.0 if shot["motion"] == request["motion"] else RELATED_MOTIONS.get(frozenset((shot["motion"], request["motion"])), 0.0)
        total += 0.8 * score
        weight += 0.8
    if request.get("people"):
        count, want = shot["people"], request["people"]
        if request.get("ots"):
            score = 1.0 if count >= 1 else 0.0
        elif want >= 3:
            score = 1.0 if count >= 3 else (0.4 if count == 2 else 0.0)
        elif count == want:
            score = 1.0
        else:
            score = 0.3 if count > want else (0.2 if count >= 1 else 0.0)
        total += 0.8 * score
        weight += 0.8
    return total / weight if weight else 0.5


def align(requests: list[dict], shots: list[dict]) -> dict:
    """요구 샷 순서와 영상 샷 순서의 국소 정렬. 점수가 가장 높은 연속 구간과 샷 대응을 돌려준다."""
    rows, cols = len(requests), len(shots)
    score = [[0.0] * (cols + 1) for _ in range(rows + 1)]
    trace = [[None] * (cols + 1) for _ in range(rows + 1)]
    similarity = [[shot_similarity(requests[i], shots[j]) for j in range(cols)] for i in range(rows)]
    best = (0.0, 0, 0)
    for i in range(1, rows + 1):
        for j in range(1, cols + 1):
            options = (
                (score[i - 1][j - 1] + similarity[i - 1][j - 1] - MATCH_OFFSET, "match"),
                (score[i - 1][j] - SKIP_REQUEST, "skip_request"),
                (score[i][j - 1] - SKIP_VIDEO, "skip_video"),
            )
            value, move = max(options, key=lambda option: option[0])
            if value > 0.0:
                score[i][j], trace[i][j] = value, move
                if value > best[0]:
                    best = (value, i, j)
    pairs, (_, i, j) = [], best
    end_shot = j - 1
    while i > 0 and j > 0 and trace[i][j]:
        move = trace[i][j]
        if move == "match":
            pairs.append((i - 1, j - 1, round(similarity[i - 1][j - 1], 3)))
            i, j = i - 1, j - 1
        elif move == "skip_request":
            i -= 1
        else:
            j -= 1
    pairs.reverse()
    ideal = max(1e-6, (1.0 - MATCH_OFFSET) * rows)
    return {
        "score": min(1.0, best[0] / ideal),
        "start_shot": pairs[0][1] if pairs else -1,
        "end_shot": end_shot if pairs else -1,
        "matches": pairs,
        "coverage": len(pairs) / max(1, rows),
    }


def analyzer_signature() -> str:
    with open(ANALYZER, encoding="utf-8") as handle:
        version = re.search(r'ANALYZER_VERSION\s*=\s*"([^"]+)"', handle.read())
    return f"{version.group(1) if version else 'unknown'}:{int(os.path.getmtime(ANALYZER))}"


def video_fingerprint(path: str, signature: str) -> str:
    stat = os.stat(path)
    return hashlib.md5(f"{os.path.abspath(path)}|{stat.st_size}|{stat.st_mtime_ns}|{signature}".encode("utf-8")).hexdigest()


def summarize_plan(plan: dict) -> list[dict]:
    return [{
        "name": shot["name"], "start": shot["start_seconds"], "end": shot["end_seconds"],
        "size": shot.get("shot_size", ""), "angle": shot.get("camera_angle", ""), "motion": shot.get("camera_motion", ""),
        "people": len(shot.get("subjects") or []), "confidence": shot.get("confidence", 0.0),
    } for shot in plan.get("shots", [])]


def index_path(library: str) -> str:
    return os.path.join(library, INDEX_DIR, "library_index.json")


def load_index(library: str) -> dict:
    path = index_path(library)
    if os.path.isfile(path):
        with open(path, encoding="utf-8") as handle:
            index = json.load(handle)
        if index.get("version") == INDEX_VERSION:
            return index
    return {"version": INDEX_VERSION, "videos": []}


def list_videos(library: str) -> list[str]:
    videos = []
    for root, dirs, files in os.walk(library):
        dirs[:] = [d for d in dirs if d != INDEX_DIR]
        videos += [os.path.join(root, name) for name in sorted(files) if name.lower().endswith(VIDEO_EXTENSIONS)]
    return videos


LINK_ENTRY_PREFIX = "link:"


def list_link_analyses(link_dir: str) -> list[tuple[str, str, dict]]:
    """링크로 분석해 둔 폴더들 (폴더 이름, reference_plan.json 경로, source.json 내용).

    유튜브처럼 영상을 남기지 않는 출처도 분석 결과와 출처 정보는 남으므로, 영상 파일 없이 그대로 검색 대상에 넣는다.
    """
    found = []
    if not link_dir or not os.path.isdir(link_dir):
        return found
    for name in sorted(os.listdir(link_dir)):
        plan_file = os.path.join(link_dir, name, "reference_plan.json")
        source_file = os.path.join(link_dir, name, "source.json")
        if not (os.path.isfile(plan_file) and os.path.isfile(source_file)):
            continue
        try:
            with open(source_file, encoding="utf-8") as handle:
                source = json.load(handle)
        except (OSError, ValueError):
            continue
        found.append((name, plan_file, source))
    return found


def link_entries(link_dir: str) -> list[dict]:
    entries = []
    for name, plan_file, source in list_link_analyses(link_dir):
        try:
            with open(plan_file, encoding="utf-8") as handle:
                plan = json.load(handle)
        except (OSError, ValueError):
            continue
        shots = summarize_plan(plan)
        if not shots:
            continue
        stat = os.stat(plan_file)
        title = (source.get("title") or plan.get("title") or name).strip()
        entries.append({
            "path": LINK_ENTRY_PREFIX + title,
            "fingerprint": hashlib.md5(f"link|{os.path.abspath(plan_file)}|{stat.st_size}|{stat.st_mtime_ns}".encode("utf-8")).hexdigest(),
            "plan": os.path.abspath(plan_file),
            "identities": ((plan.get("analysis_notes") or {}).get("identities") or {}).get("count"),
            "shots": shots,
            "link": {"folder": name, "url": source.get("url", ""), "title": title,
                     "uploader": source.get("uploader", ""), "video_retained": bool(source.get("video_retained"))},
            "indexed_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        })
    return entries


def analyze_video(video: str, output: str, python: str) -> tuple[bool, str]:
    source, temp_dir = video, None
    if not video.isascii():
        # OpenCV는 한글 등 비ASCII 경로를 못 여는 경우가 있어 임시 영문 경로로 복사해 분석한다.
        temp_dir = tempfile.mkdtemp(prefix="upt_library_")
        source = os.path.join(temp_dir, "video" + os.path.splitext(video)[1])
        shutil.copyfile(video, source)
    try:
        proc = subprocess.run([python, ANALYZER, "--video", source, "--output", output], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=1800)
        return proc.returncode == 0 and os.path.isfile(output), (proc.stderr or proc.stdout)[-800:]
    finally:
        if temp_dir:
            shutil.rmtree(temp_dir, ignore_errors=True)


def command_index(args: argparse.Namespace) -> int:
    library = os.path.abspath(args.library)
    os.makedirs(os.path.join(library, INDEX_DIR, "plans"), exist_ok=True)
    signature = analyzer_signature()
    previous = {entry["path"]: entry for entry in load_index(library)["videos"]}
    videos = list_videos(library)
    entries, failures = [], []
    for number, video in enumerate(videos, 1):
        relative = os.path.relpath(video, library)
        fingerprint = video_fingerprint(video, signature)
        plan_file = os.path.join(library, INDEX_DIR, "plans", fingerprint + ".json")
        cached = previous.get(relative)
        if cached and cached.get("fingerprint") == fingerprint and os.path.isfile(plan_file):
            entries.append(cached)
            print(f"UPT_LIBRARY_PROGRESS {number}/{len(videos)} cached {relative}", flush=True)
            continue
        print(f"UPT_LIBRARY_PROGRESS {number}/{len(videos)} analyzing {relative}", flush=True)
        ok, log = analyze_video(video, plan_file, sys.executable)
        if not ok:
            failures.append({"path": relative, "error": log})
            print(f"UPT_LIBRARY_WARNING analysis failed: {relative}", flush=True)
            continue
        with open(plan_file, encoding="utf-8") as handle:
            plan = json.load(handle)
        entries.append({
            "path": relative, "fingerprint": fingerprint, "plan": os.path.relpath(plan_file, library),
            "identities": ((plan.get("analysis_notes") or {}).get("identities") or {}).get("count"),
            "shots": summarize_plan(plan), "indexed_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        })
    links = link_entries(getattr(args, "link_analysis", ""))
    if links:
        print(f"UPT_LIBRARY_PROGRESS link analyses {len(links)}", flush=True)
    entries += links
    index = {"version": INDEX_VERSION, "analyzer": signature, "videos": entries, "failures": failures}
    with open(index_path(library), "w", encoding="utf-8") as handle:
        json.dump(index, handle, ensure_ascii=False, indent=2)
    print(f"UPT_LIBRARY_INDEX={index_path(library)} videos={len(entries)} failed={len(failures)}", flush=True)
    return 0 if entries or not videos else 1


def write_segment_plan(library: str, entry: dict, start: int, end: int) -> str:
    with open(os.path.join(library, entry["plan"]), encoding="utf-8") as handle:
        plan = json.load(handle)
    shots = plan["shots"][start:end + 1]
    origin = shots[0]["start_seconds"]
    for shot in shots:
        shot["start_seconds"] = round(shot["start_seconds"] - origin, 3)
        shot["end_seconds"] = round(shot["end_seconds"] - origin, 3)
    plan["shots"] = shots
    plan["title"] = f"{plan.get('title', 'Reference')}_seg{start + 1}_{end + 1}"
    notes = plan.setdefault("analysis_notes", {})
    notes["source_video"] = entry["path"]
    notes["segment_seconds"] = [origin, round(origin + shots[-1]["end_seconds"], 3)]
    if entry.get("link"):
        notes["source_url"] = entry["link"].get("url", "")
    os.makedirs(os.path.join(library, INDEX_DIR, "segments"), exist_ok=True)
    path = os.path.join(library, INDEX_DIR, "segments", f"{entry['fingerprint']}_{start + 1}_{end + 1}.json")
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(plan, handle, ensure_ascii=False, indent=2)
    return path


def command_search(args: argparse.Namespace) -> int:
    library = os.path.abspath(args.library)
    index = load_index(library)
    if not index["videos"]:
        print("UPT_LIBRARY_ERROR 색인된 영상이 없습니다. 먼저 index를 실행하세요.", file=sys.stderr)
        return 2
    prompt = args.prompt
    if args.prompt_file:
        # 에디터는 한글·따옴표가 섞인 프롬프트를 명령줄 대신 UTF-8 파일로 넘긴다.
        with open(args.prompt_file, encoding="utf-8-sig") as handle:
            prompt = handle.read()
    prompt = " ".join(prompt.split())
    if not prompt:
        print("UPT_LIBRARY_ERROR 프롬프트가 비어 있습니다.", file=sys.stderr)
        return 2
    query = parse_prompt(prompt)
    results = []
    for entry in index["videos"]:
        shots = entry["shots"]
        if not shots:
            continue
        if query["shots"]:
            alignment = align(query["shots"], shots)
        else:
            alignment = {"score": 0.0, "start_shot": 0, "end_shot": len(shots) - 1, "matches": [], "coverage": 0.0}
        video_people = max([entry.get("identities") or 0] + [shot["people"] for shot in shots])
        people_factor = 0.6 if query["people_required"] >= 2 and video_people < 2 else 1.0
        name = os.path.splitext(os.path.basename(entry["path"]))[0].lower()
        keyword_bonus = min(0.15, 0.05 * sum(1 for word in query["keywords"] if word in name))
        final = min(1.0, alignment["score"] * people_factor + keyword_bonus)
        if final <= 0.0 or alignment["start_shot"] < 0:
            continue
        start, end = alignment["start_shot"], alignment["end_shot"]
        results.append({
            "video": entry["path"], "score": round(100.0 * final, 1), "coverage": round(alignment["coverage"], 2),
            "segment": {"start_shot": start + 1, "end_shot": end + 1, "start_seconds": shots[start]["start"], "end_seconds": shots[end]["end"]},
            "matches": [{"request": query["shots"][i]["text"], "shot": shots[j]["name"], "similarity": sim,
                         "shot_info": f"{shots[j]['size']}/{shots[j]['angle']}/{shots[j]['motion']}/{shots[j]['people']}명"}
                        for i, j, sim in alignment["matches"]],
            "people_factor": people_factor, "keyword_bonus": keyword_bonus,
            "source_url": (entry.get("link") or {}).get("url", ""),
            "_entry": entry,
        })
    # 점수가 같으면 요구 샷을 더 많이 맞춘 영상, 그다음 분석 신뢰도가 높은 영상을 앞에 둔다.
    def mean_confidence(result: dict) -> float:
        segment = result["_entry"]["shots"][result["segment"]["start_shot"] - 1:result["segment"]["end_shot"]]
        return sum(shot["confidence"] for shot in segment) / max(1, len(segment))

    results.sort(key=lambda result: (-result["score"], -result["coverage"], -mean_confidence(result)))
    results = results[:args.top]
    for result in results:
        entry = result.pop("_entry")
        result["reference_plan"] = write_segment_plan(library, entry, result["segment"]["start_shot"] - 1, result["segment"]["end_shot"] - 1)
    output = {"prompt": prompt, "query": query, "results": results}
    text = json.dumps(output, ensure_ascii=False, indent=2)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as handle:
            handle.write(text)
        print(f"UPT_LIBRARY_RESULT={os.path.abspath(args.output)}", flush=True)
    else:
        print(text)
    return 0


def main() -> int:
    # 에디터가 출력을 파이프로 받으면 Windows 기본 코드페이지(cp949)로 나가 한글 오류 메시지가 깨지므로 UTF-8로 고정한다.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    index_parser = sub.add_parser("index")
    index_parser.add_argument("--library", required=True)
    index_parser.add_argument("--link-analysis", default="", help="링크로 분석해 둔 폴더(Saved/UniversalProductionTools/LinkAnalysis)도 검색 대상에 넣는다")
    parse_parser = sub.add_parser("parse")
    parse_parser.add_argument("--prompt", required=True)
    search_parser = sub.add_parser("search")
    search_parser.add_argument("--library", required=True)
    search_parser.add_argument("--prompt", default="")
    search_parser.add_argument("--prompt-file", default="", help="UTF-8 텍스트 파일로 프롬프트 전달(에디터용)")
    search_parser.add_argument("--top", type=int, default=5)
    search_parser.add_argument("--output", default="")
    for subparser in (parse_parser, search_parser):
        subparser.add_argument("--llm-endpoint", default=DEFAULT_LLM_ENDPOINT, help="규칙이 못 알아들은 표현을 해석할 로컬 LLM(OpenAI 호환) 주소")
        subparser.add_argument("--llm-model", default=DEFAULT_LLM_MODEL)
        subparser.add_argument("--no-llm", action="store_true", help="규칙만 쓰고 LLM에 묻지 않는다")
    args = parser.parse_args()
    if getattr(args, "llm_endpoint", "") and not getattr(args, "no_llm", False):
        LLM_ASSIST.update({"endpoint": args.llm_endpoint.strip(), "model": args.llm_model.strip()})
    if args.command == "index":
        return command_index(args)
    if args.command == "parse":
        print(json.dumps(parse_prompt(args.prompt), ensure_ascii=False, indent=2))
        return 0
    return command_search(args)


if __name__ == "__main__":
    sys.exit(main())
