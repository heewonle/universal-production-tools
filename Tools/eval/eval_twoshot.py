"""투샷 정답 영상(twoshot_*)을 분석해 여러 인물 검출과 샷 간 인물 구분(역할 일관성)을 잰다.

- 샷별로 화면에 보이는 정답 인물 수와 분석이 찾은 인물 수가 같은지
- 가로 위치 오차(분석 인물과 정답 인물을 x 순서로 대응)
- 역할 일관성: 한 역할 이름이 샷이 바뀌어도 한 실제 인물만 가리키고, 한 실제 인물이 여러 역할로 쪼개지지 않는지
"""
from __future__ import annotations

import json
import os
from collections import Counter, defaultdict

from common import dataset_video, latest_dataset, run_analyzer, use_utf8_output


def in_frame(point, pad: float = 0.02) -> bool:
    return point is not None and -pad <= point[0] <= 1 + pad and -pad <= point[1] <= 1 + pad


def expected_over_shoulder(truth_shot: dict) -> str:
    """정답의 어깨 너머 앞사람 쪽('left'/'right', 없으면 '').

    머리·몸 중앙은 화면 밖이지만 카메라 앞에 있고 보이는 인물보다 가까우며, 머리가 화면 가로 폭 한 배 이내에 있는 인물.
    (어깨 조각만 화면 가장자리에 걸친다. 카메라 뒤에 있는 인물은 제외.)
    """
    visible_depths = [s["depth_cm"] for s in truth_shot["subjects"] if in_frame(s["head"]) or in_frame(s["mid"])]
    for subject in truth_shot["subjects"]:
        head = subject["head"]
        if in_frame(head) or in_frame(subject["mid"]) or head is None or not visible_depths:
            continue
        if 0 < subject["depth_cm"] < min(visible_depths) and -1.0 <= head[0] <= 2.0:
            return "right" if head[0] > 0.5 else "left"
    return ""


def compare(plan: dict, truth: dict) -> dict:
    role_to_label = defaultdict(Counter)
    count_ok, x_errors, rows = 0, [], []
    ots_expected, ots_hits, ots_false = 0, 0, 0
    for truth_shot in truth["shots"]:
        center = (truth_shot["start"] + truth_shot["end"]) / 2
        shot = next((s for s in plan["shots"] if s["start_seconds"] <= center <= s["end_seconds"]), None)
        visible = []
        for subject in truth_shot["subjects"]:
            # 머리(0.88) 또는 몸 중앙이 화면 안이면 보이는 인물로 본다.
            point = subject["head"] if in_frame(subject["head"]) else (subject["mid"] if in_frame(subject["mid"]) else None)
            if point is not None:
                visible.append((point[0], subject["label"]))
        visible.sort()
        found = sorted((s["screen_position"]["x"], s["role"]) for s in (shot or {}).get("subjects", []))
        matched = len(found) == len(visible)
        count_ok += int(matched)
        pairs = list(zip(found, visible)) if matched else []
        for (found_x, role), (truth_x, label) in pairs:
            x_errors.append(abs(found_x - truth_x))
            role_to_label[role][label] += 1
        expected = expected_over_shoulder(truth_shot)
        found_side = ((shot or {}).get("over_the_shoulder") or {}).get("side", "")
        if expected:
            ots_expected += 1
            ots_hits += int(found_side == expected)
        elif found_side:
            ots_false += 1
        rows.append(f"{truth_shot['name']}: 보이는 인물 {len(visible)} / 찾은 인물 {len(found)} {'OK' if matched else 'MISS'} "
                    + " ".join(f"[{role}->{label} dx={found_x - truth_x:+.3f}]" for (found_x, role), (truth_x, label) in pairs)
                    + f" | 어깨 너머 정답={expected or '-'} 분석={found_side or '-'}")
    labels_per_role = {role: dict(counter) for role, counter in role_to_label.items()}
    single_label = all(len(counter) == 1 for counter in role_to_label.values())
    distinct = len({next(iter(counter)) for counter in role_to_label.values()}) == len(role_to_label) if single_label else False
    return {
        "count_ok": count_ok, "shots": len(truth["shots"]),
        "mean_abs_dx": round(sum(x_errors) / len(x_errors), 4) if x_errors else None,
        "roles_consistent": bool(role_to_label) and single_label and distinct,
        "ots_expected": ots_expected, "ots_hits": ots_hits, "ots_false": ots_false,
        "role_groups": labels_per_role, "rows": rows,
    }


def run(output_dir: str) -> dict:
    data_dir = latest_dataset("twoshot")
    if not data_dir:
        return {"skipped": "정답 데이터 없음(twoshot)"}
    video, truth = dataset_video(data_dir)
    if not video:
        return {"skipped": f"정답 영상 없음({os.path.basename(data_dir)})"}
    os.makedirs(output_dir, exist_ok=True)
    plan_file = os.path.join(output_dir, "twoshot_plan.json")
    run_analyzer(video, plan_file)
    with open(plan_file, encoding="utf-8") as handle:
        plan = json.load(handle)
    metrics = compare(plan, truth)
    metrics["dataset"] = os.path.basename(data_dir)
    metrics["identities"] = ((plan.get("analysis_notes") or {}).get("identities") or {}).get("count")
    return metrics


if __name__ == "__main__":
    use_utf8_output()
    print(json.dumps(run(os.path.join(os.getcwd(), "eval_output")), ensure_ascii=False, indent=2))
