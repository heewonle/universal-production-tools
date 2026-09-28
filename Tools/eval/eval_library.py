"""레퍼런스 라이브러리 검색(upt_reference_library.py) 회귀 검사.

내용을 아는 정답 영상을 중립적인 이름(clip_a~d)으로 임시 라이브러리에 복사해 색인한 뒤, 질의마다 기대한 영상이 1위인지 확인한다.
파일 이름으로 맞히지 못하게 이름에는 내용 힌트를 넣지 않는다.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys

from common import TOOLS_DIR, dataset_video, latest_dataset, use_utf8_output

LIBRARY_TOOL = os.path.join(TOOLS_DIR, "upt_reference_library.py")
SOURCES = [("framing", "clip_a.mp4"), ("motion", "clip_b.mp4"), ("twoshot", os.path.join("sub", "clip_c.mp4"))]
DISTRACTOR = ("peasant_video", os.path.join("sub", "클립_d.mp4"))  # 있으면 방해 영상·한글 파일명 검사로 함께 넣는다.
# (기대 1위 데이터셋 또는 None=맞는 영상 없음, 프롬프트)
QUERIES = [
    ("framing", "주인공 전신 와이드로 시작해 얼굴 클로즈업, 마지막에 로우앵글로 돌아가며 마무리"),
    ("twoshot", "두 사람이 투샷으로 서 있다가 어깨 너머로 대화하고 클로즈업으로 끝"),
    ("motion", "돌리 인 다음 줌 인, 마지막은 핸드헬드"),
    ("motion", "panning shot then push in"),
    ("twoshot", "두 사람이 마주 보고 대화하는 장면, 한 사람 클로즈업"),
    (None, "군중이 달리는 부감 트래킹 샷"),
]
NO_MATCH_SCORE = 40.0


def run(output_dir: str) -> dict:
    library = os.path.join(output_dir, "library")
    shutil.rmtree(library, ignore_errors=True)
    names = {}
    for dataset, target in SOURCES + [DISTRACTOR]:
        data_dir = latest_dataset(dataset)
        video = dataset_video(data_dir)[0] if data_dir else None
        if not video:
            if (dataset, target) == DISTRACTOR:
                continue
            return {"skipped": f"정답 영상 없음({dataset})"}
        os.makedirs(os.path.dirname(os.path.join(library, target)), exist_ok=True)
        shutil.copyfile(video, os.path.join(library, target))
        names[dataset] = os.path.splitext(os.path.basename(target))[0]

    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    index = subprocess.run([sys.executable, LIBRARY_TOOL, "index", "--library", library], capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    if index.returncode != 0:
        raise RuntimeError(f"라이브러리 색인 실패: {(index.stderr or index.stdout)[-1500:]}")
    # 링크로 분석해 둔 영상(영상 파일 없이 분석 결과만 있는 경우)도 검색 대상에 들어오는지 확인한다.
    link_dir = os.path.join(output_dir, "link_analysis", "clip_motion_link")
    os.makedirs(link_dir, exist_ok=True)
    with open(os.path.join(library, ".upt_library", "library_index.json"), encoding="utf-8") as handle:
        indexed = {entry["path"]: entry for entry in json.load(handle)["videos"]}
    motion_entry = indexed.get("clip_b.mp4")
    if motion_entry:
        shutil.copyfile(os.path.join(library, motion_entry["plan"]), os.path.join(link_dir, "reference_plan.json"))
        with open(os.path.join(link_dir, "source.json"), "w", encoding="utf-8") as handle:
            json.dump({"url": "https://example.com/watch?v=link_test", "title": "링크분석_모션", "video_retained": False}, handle, ensure_ascii=False)
    index = subprocess.run([sys.executable, LIBRARY_TOOL, "index", "--library", library, "--link-analysis", os.path.dirname(link_dir)],
                           capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    if index.returncode != 0:
        raise RuntimeError(f"라이브러리 색인 실패: {(index.stderr or index.stdout)[-1500:]}")

    passed, details = 0, []
    for number, (expected, prompt) in enumerate(QUERIES, 1):
        result_file = os.path.join(output_dir, f"library_query_{number}.json")
        # 회귀 검사는 규칙 해석만 본다(--no-llm). 로컬 LLM이 떠 있는지에 따라 결과가 달라지면 기준값이 흔들린다.
        search = subprocess.run([sys.executable, LIBRARY_TOOL, "search", "--library", library, "--prompt", prompt, "--top", "3", "--output", result_file, "--no-llm"],
                                capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
        if search.returncode != 0:
            details.append({"prompt": prompt, "ok": False, "error": (search.stderr or search.stdout)[-500:]})
            continue
        with open(result_file, encoding="utf-8") as handle:
            results = json.load(handle)["results"]
        top = results[0] if results else None
        if expected is None:
            ok = top is None or top["score"] < NO_MATCH_SCORE
        else:
            ok = top is not None and names[expected] == os.path.splitext(os.path.basename(top["video"]))[0]
        passed += int(ok)
        details.append({"prompt": prompt, "expected": names.get(expected) if expected else None, "ok": ok,
                        "ranking": [(r["video"], r["score"], f"{r['segment']['start_shot']}-{r['segment']['end_shot']}") for r in results]})
    # 링크 항목 검사: 같은 내용을 링크 분석으로도 넣었으니 모션 질의 결과에 링크 항목이 함께 나와야 한다.
    link_result_file = os.path.join(output_dir, "library_query_link.json")
    subprocess.run([sys.executable, LIBRARY_TOOL, "search", "--library", library, "--prompt", "돌리 인 다음 줌 인, 마지막은 핸드헬드",
                    "--top", "5", "--output", link_result_file, "--no-llm"], capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    link_results = json.load(open(link_result_file, encoding="utf-8"))["results"] if os.path.isfile(link_result_file) else []
    link_hit = next((r for r in link_results if str(r["video"]).startswith("link:")), None)
    return {"passed": passed, "total": len(QUERIES), "videos": len(names), "details": details,
            "validator_failures": check_llm_validator(),
            "link_entry_found": bool(motion_entry) and link_hit is not None,
            "link_entry_has_url": bool(link_hit and link_hit.get("source_url")),
            "link_entry_plan": bool(link_hit and os.path.isfile(link_hit.get("reference_plan", ""))),
            }


def check_llm_validator() -> list[str]:
    """LLM 답에서 허용된 값만 받아들이는지 검사한다(네트워크 없이 함수만 호출)."""
    sys.path.insert(0, TOOLS_DIR)
    import upt_reference_library as library_tool

    cases = [
        ("정상 값 유지", {"size": "close_up", "motion": "dolly_in"}, {"size": "close_up", "motion": "dolly_in"}),
        ("대소문자·공백 정리", {"size": " Close_Up "}, {"size": "close_up"}),
        ("없는 샷 크기 버림", {"size": "banana"}, {}),
        ("없는 모션 버림", {"motion": "teleport"}, {}),
        ("인원 범위 밖 버림", {"people": 7}, {}),
        ("참/거짓은 인원이 아님", {"people": True}, {}),
        ("어깨 너머는 인원 1·미디엄 기본", {"ots": True}, {"people": 1, "ots": True, "size": "medium"}),
        ("모르는 키 무시", {"foo": "bar", "angle": "low"}, {"angle": "low"}),
        ("객체가 아니면 빈 결과", ["size"], {}),
    ]
    return [name for name, raw, expected in cases if library_tool.clean_llm_shot(raw) != expected]


if __name__ == "__main__":
    use_utf8_output()
    print(json.dumps(run(os.path.join(os.getcwd(), "eval_output")), ensure_ascii=False, indent=2))
