"""생성된 시네마틱의 샷별 캡처 이미지를 레퍼런스 포즈 분석 결과와 비교해 구도 오차를 계산한다(API 불필요).

레퍼런스 영상 분석과 같은 검출기·같은 규칙으로 두 쪽을 측정하므로, 결과 점수는 "화면 속 인물 위치와 크기가 얼마나 같은가"를 뜻한다.
체형 비율은 레퍼런스 인물과 언리얼 캐릭터가 다를 수 있어 각자의 샷들에서 따로 보정한다.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upt_pose_reference_analyzer as analyzer  # noqa: E402


# 검출 상자가 화면 가장자리에 이만큼 붙으면 몸이 잘린 것으로 본다(픽셀).
FRAME_EDGE_MARGIN_PX = 4
# 어깨 너머 앞사람은 카메라에 아주 가까워 주 피사체보다 훨씬 크게 잡힌다.
FOREGROUND_HEIGHT_FACTOR = 1.5


# 추정 전신 키가 보이는 상자보다 이 배수 이상 크면 분석기가 프레임 밖까지 외삽한 것으로 본다.
EXTRAPOLATION_FACTOR = 1.05


def is_cropped(person, image_w: int, image_h: int) -> bool:
    """몸을 끝까지 보고 잰 게 아닌가. 그러면 축도 키도 프레임 밖까지 늘여 잡은 값이라 믿을 수 없다.

    가장자리에 상자가 닿았는지만 보면 놓친다(상자가 가장자리에서 몇 픽셀 떨어져도 분석기는 외삽한다).
    추정 전신 키를 보이는 상자 높이와 견주는 쪽이 외삽 여부를 직접 말해 준다.
    """
    left, top, right, bottom = person.box[0], person.box[1], person.box[2], person.box[3]
    touches_edge = (left <= FRAME_EDGE_MARGIN_PX or top <= FRAME_EDGE_MARGIN_PX
                    or right >= image_w - FRAME_EDGE_MARGIN_PX or bottom >= image_h - FRAME_EDGE_MARGIN_PX)
    visible_height = max(1.0, float(bottom - top))
    # numpy 비교 결과를 그대로 두면 결과 JSON 저장에서 터진다.
    return bool(touches_edge or person.height_px > visible_height * EXTRAPOLATION_FACTOR)


def measure_focus(person, focus_ratio: float, image_w: int, image_h: int) -> tuple[float, float, str]:
    """조준 높이의 화면 좌표. 가로는 몸 축 대신 검출 상자의 가로 중앙으로 잰다.

    잘린 몸은 보이는 부분만으로 축을 세우고 프레임 밖까지 늘여 조준점을 잡는다. 인물이 기울어 서 있으면
    그 연장선이 옆으로 밀려, 카메라가 정확한데도 오차가 크게 나온다.
    카메라가 실제로 조준한 위치(manifest의 subject_screen)를 정답으로 두고 두 장면 15샷에서 재 보면:
      축점 그대로        평균 0.052 / 중앙 0.021 / 최대 0.235
      상자 가로 중앙     평균 0.031 / 중앙 0.025 / 최대 0.115
      축점 ±10% 제한     평균 0.034 / 중앙 0.013 / 최대 0.156
    중앙값은 ±10% 제한이 조금 낫지만 크게 빗나가는 경우가 남는다. 회귀 검사용 지표에서는 최악을 줄이는 쪽이
    중요하고 규칙도 단순해서, 평균과 최대가 모두 가장 좋은 '상자 가로 중앙'을 쓴다.
    세로는 조준 높이 비율이 뜻을 갖는 값이라 그대로 몸 축에서 읽는다.
    """
    _focus_x, focus_y = person.point_at(focus_ratio)
    # box는 numpy 배열이라 float로 바꿔 둔다(그대로 쓰면 결과 JSON 저장에서 터진다).
    return float(0.5 * (person.box[0] + person.box[2])), float(focus_y), "box_center"


def drop_over_shoulder_foreground(people: list, side: str, image_w: int, image_h: int) -> list:
    """어깨 너머 샷에서 앞사람을 후보에서 뺀다.

    앞사람은 지정한 쪽 가장자리에 붙어 있고 주 피사체보다 훨씬 크게 잡힌다. 남는 사람이 없으면 그대로 둔다.
    """
    if len(people) < 2 or side not in ("left", "right"):
        return people
    heights = sorted(person.height_px for person in people)
    median_height = heights[len(heights) // 2]
    kept = []
    for person in people:
        touches_side = (person.box[0] <= FRAME_EDGE_MARGIN_PX) if side == "left" else (person.box[2] >= image_w - FRAME_EDGE_MARGIN_PX)
        if touches_side and person.height_px >= FOREGROUND_HEIGHT_FACTOR * median_height:
            continue
        kept.append(person)
    return kept or people


# 앞사람을 숨긴 장면과 밝기가 이만큼(0~255) 다르면 그 화소는 앞사람이 차지한 곳으로 본다.
HIDDEN_DIFF_THRESHOLD = 12
# 화면의 이 비율보다 작으면 그림자 등 잔재로 보고 '앞사람이 프레임에 없음'으로 판정한다.
HIDDEN_MIN_AREA = 0.01


def measure_hidden_foreground(image, plain_path: str) -> dict | None:
    """앞사람만 숨기고 찍은 장면과 비교해, 앞사람이 화면에서 차지한 영역을 잰다.

    인물 검출기도 분할 모델도 무늬 없는 마네킹의 어깨 조각은 사람으로 보지 못한다(둘 다 0개).
    두 장의 차이는 모델을 쓰지 않으므로 '앞사람이 실제로 프레임 안에 있는가'를 확실히 알려 준다.
    앞사람이 지던 그림자도 함께 사라지므로 차이에 섞인다. 방향은 가장자리 접촉이 아니라
    화소 무게중심으로 정해 얇게 퍼지는 그림자에 휘둘리지 않게 한다.
    """
    if not plain_path or not os.path.isfile(plain_path):
        return None
    plain = cv2.imdecode(np.fromfile(plain_path, dtype=np.uint8), cv2.IMREAD_COLOR)
    if plain is None or plain.shape != image.shape:
        return None
    mask = cv2.absdiff(image, plain).max(axis=2) > HIDDEN_DIFF_THRESHOLD
    image_h, image_w = mask.shape
    result = {"area": float(mask.mean()), "side": None}
    if result["area"] < HIDDEN_MIN_AREA:
        return result
    columns = np.where(mask.any(axis=0))[0]
    lines = np.where(mask.any(axis=1))[0]
    center_x = float(np.average(np.arange(image_w), weights=mask.sum(axis=0)))
    result.update(x0=round(float(columns[0]) / image_w, 3), x1=round(float(columns[-1]) / image_w, 3),
                  y0=round(float(lines[0]) / image_h, 3), y1=round(float(lines[-1]) / image_h, 3),
                  center_x=round(center_x / image_w, 3),
                  side="left" if center_x < image_w * 0.5 else "right")
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", required=True, help="레퍼런스 영상 분석 결과 reference_plan.json")
    parser.add_argument("--manifest", required=True, help="생성 샷 캡처 목록 manifest.json")
    parser.add_argument("--output", required=True, help="검증 결과 JSON 경로")
    parser.add_argument("--debug-dir", default="")
    parser.add_argument("--model", default="balanced", choices=("lightweight", "balanced", "performance"))
    parser.add_argument("--kpt-thr", type=float, default=0.35)
    parser.add_argument("--det-thr", type=float, default=0.5)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    with open(args.reference, encoding="utf-8") as handle:
        reference = json.load(handle)
    with open(args.manifest, encoding="utf-8") as handle:
        manifest = json.load(handle)

    from rtmlib import RTMPose, YOLOX
    from rtmlib.tools.solution.body import Body

    urls = Body.MODE[args.model]
    detector = YOLOX(urls["det"], model_input_size=urls["det_input_size"], score_thr=args.det_thr, backend="onnxruntime", device="cpu")
    pose_model = RTMPose(urls["pose"], model_input_size=urls["pose_input_size"], backend="onnxruntime", device="cpu")

    captures = []
    for entry in manifest["shots"]:
        image = cv2.imdecode(np.fromfile(entry["image"], dtype=np.uint8), cv2.IMREAD_COLOR) if os.path.isfile(entry["image"]) else None
        person_boxes = []
        raw = analyzer.detect_raw_people(detector, pose_model, image, args.kpt_thr, person_boxes) if image is not None else []
        captures.append((entry, image, raw, person_boxes))
    # 레퍼런스에 어깨 너머(OTS) 샷이 있으면 생성 캡처에도 같은 쪽에 잘린 앞사람이 보이는지 분석과 같은 규칙으로 확인한다(점수에는 넣지 않음).
    segmenter = None
    if any(shot.get("over_the_shoulder") for shot in reference.get("shots", [])):
        segmenter = analyzer.ForegroundSegmenter(analyzer.SEGMENTATION_MODEL)
        if segmenter.interpreter is None:
            print(f"UPT_VERIFY over_shoulder_check off ({segmenter.reason})", flush=True)
            segmenter = None

    first = next((image for _, image, _, _ in captures if image is not None), None)
    frame_h, frame_w = first.shape[:2] if first is not None else (720, 1280)
    # 캡처는 샷당 1장뿐이라 보정 표본이 적다. 2개 이상이면 캐릭터 자체 비율을 쓰고, 부족하면 레퍼런스 인물의 보정값을 빌려 쓴다.
    ratios, calibration = analyzer.calibrate_ratios([r for _, _, raw, _ in captures for r in raw], args.kpt_thr, frame_w, frame_h, min_samples=2)
    if not calibration:
        reference_calibration = (reference.get("analysis_notes") or {}).get("proportion_calibration") or {}
        ratios = dict(analyzer.DEFAULT_KEYPOINT_RATIO)
        ratios.update({(int(key) if key.isdigit() else key): float(value) for key, value in reference_calibration.items()})
        calibration = {"source": "reference", **reference_calibration}
    if args.debug_dir:
        os.makedirs(args.debug_dir, exist_ok=True)

    rows, scores = [], []
    reference_shots = reference.get("shots", [])
    for index, (entry, image, raw, person_boxes) in enumerate(captures):
        row = {"name": entry.get("name", f"Shot_{index + 1:02d}"), "image": entry["image"]}
        ref = reference_shots[index] if index < len(reference_shots) else None
        if ref is None or not ref.get("subject_role"):
            row.update(status="skipped", note="레퍼런스 샷에 인물이 없어 비교하지 않음")
            rows.append(row)
            continue
        if image is None:
            row.update(status="error", score=0.0, note="캡처 이미지를 읽지 못함")
            rows.append(row)
            scores.append(0.0)
            continue

        image_h, image_w = image.shape[:2]
        people = [m for m in (analyzer.fit_body(b, k, s, args.kpt_thr, image_w, image_h, ratios) for b, k, s in raw) if m and m.score >= 0.2]
        people.sort(key=lambda person: person.main_person_key(), reverse=True)
        if not people:
            row.update(status="no_person", score=0.0, note="생성된 샷에서 인물을 찾지 못함(화면 밖이거나 가려짐)")
            rows.append(row)
            scores.append(0.0)
            continue

        reference_size = ref["shot_size"]
        focus_ratio = analyzer.FOCUS_HEIGHT_RATIO[reference_size]
        if ref.get("over_the_shoulder"):
            people = drop_over_shoulder_foreground(people, ref["over_the_shoulder"].get("side", ""), image_w, image_h)
        ref_x, ref_y = ref["subject_screen_position"]["x"], ref["subject_screen_position"]["y"]
        # 여러 명이 비슷한 크기로 보이면 가장 큰 사람이 주 피사체라는 보장이 없다(투샷에서 상대가 더 가까이 서면 뒤바뀐다).
        subject_screen = entry.get("subject_screen")
        if subject_screen:
            # 생성기가 캡처 카메라로 계산한 '주 피사체 Actor 조준점의 화면 위치'에 가장 가까운 사람을 비교한다.
            # 레퍼런스 위치로 고르면 카메라가 틀려 다른 사람이 그 자리에 와도 좋은 점수가 나오지만, 이 방식은 틀린 만큼 오차로 드러난다.
            target_x, target_y = subject_screen["x"], subject_screen["y"]
            def distance_to_target(person):
                measured_x, measured_y, _ = measure_focus(person, focus_ratio, image_w, image_h)
                return math.hypot(measured_x / image_w - target_x, measured_y / image_h - target_y)

            main_person = min(people, key=distance_to_target)
            selection = "projected_subject"
        else:
            # 투영 정보가 없는 이전 manifest: 크게 보이는 사람(가장 큰 사람의 절반 이상) 중 레퍼런스 조준 위치에 가장 가까운 사람.
            candidates = [person for person in people if person.main_person_key() >= 0.5 * people[0].main_person_key()]
            main_person = min(candidates, key=lambda person: abs(measure_focus(person, focus_ratio, image_w, image_h)[0] / image_w - ref_x))
            selection = "reference_position"
        focus_x, focus_y, measure = measure_focus(main_person, focus_ratio, image_w, image_h)
        focus_x, focus_y = focus_x / image_w, focus_y / image_h
        # 크기는 레퍼런스와 같은 추정기로 잰 값끼리 비교한다.
        # 엔진이 Bounds로 준 정확한 높이(geometric_height)로 바꿔 보면 오히려 점수가 나빠진다(Shot_03 46→21, Shot_05 93→80).
        # 레퍼런스 쪽 키도 같은 추정기가 외삽한 값이라, 한쪽만 정확한 값으로 바꾸면 두 값의 편향이 어긋나기 때문이다.
        # 정확한 값은 '추정이 얼마나 빗나갔는지'를 보는 진단용으로만 남긴다.
        detected_height = main_person.height_px / image_h
        geometric_height = entry.get("subject_screen_height")
        render_height, size_source = detected_height, "detected"
        size_confidence = (round(detected_height / float(geometric_height), 3)
                           if geometric_height and float(geometric_height) > 1e-6 else None)
        reference_height = (ref.get("diagnostics") or {}).get("full_body_screen_height")
        height_ratio = render_height / reference_height if reference_height else None

        # 위치는 화면 대각 방향 0.15(15%) 벗어나면 0점, 크기는 1.5배 차이면 0점.
        position_error = math.hypot(focus_x - ref_x, focus_y - ref_y)
        position_score = max(0.0, 1.0 - position_error / 0.15)
        size_score = max(0.0, 1.0 - abs(math.log(height_ratio)) / math.log(1.5)) if height_ratio else 0.5
        score = round(100.0 * (0.5 * position_score + 0.5 * size_score), 1)
        moving = entry.get("camera_motion") not in (None, "", "static")
        row.update(
            status="ok", score=score, subject_selection=selection, subject_measure=measure, size_source=size_source,
            subject_cropped=is_cropped(main_person, image_w, image_h),
            detected_height=round(detected_height, 3),
            geometric_height=None if geometric_height is None else round(float(geometric_height), 3),
            size_confidence=size_confidence,
            error_x=round(focus_x - ref_x, 3), error_y=round(focus_y - ref_y, 3),
            height_ratio=None if height_ratio is None else round(height_ratio, 3),
            reference_size=reference_size, render_size=analyzer.classify_shot_size(render_height),
            note="모션 샷은 시작 프레임 기준 비교" if moving else "",
        )
        foreground = []
        if ref.get("over_the_shoulder"):
            row["over_shoulder_expected"] = ref["over_the_shoulder"]["side"]
            hidden = measure_hidden_foreground(image, entry.get("image_without_foreground", ""))
            if hidden is not None:
                # 앞사람을 숨긴 장면과 비교한 결과. 모델을 쓰지 않으므로 이 값이 가장 믿을 만하다.
                row["over_shoulder_check"] = "hidden_capture"
                row["over_shoulder_area"] = round(hidden["area"], 4)
                row["over_shoulder_found"] = hidden["side"] or "not_in_frame"
                if hidden.get("center_x") is not None:
                    row["over_shoulder_box"] = [hidden["x0"], hidden["y0"], hidden["x1"], hidden["y1"]]
            elif segmenter is not None:
                foreground = analyzer.foreground_candidates(segmenter, image, person_boxes)
                row["over_shoulder_check"] = "segmentation"
                # 분할 모델은 무늬 없는 마네킹 어깨 조각을 사람으로 못 보는 경우가 있어, 못 찾은 것을 '앞사람 없음'으로 단정하지 않는다.
                row["over_shoulder_found"] = max(foreground, key=lambda f: f["area"])["side"] if foreground else "not_detected"
        rows.append(row)
        scores.append(score)

        if args.debug_dir:
            canvas = analyzer.draw_debug(image, people, f"{row['name']} score={score:.0f}", focus_ratio,
                                         f"dx={row['error_x']:+.3f} dy={row['error_y']:+.3f} size x{row['height_ratio']}")
            ref_px = (int(ref_x * image_w), int(ref_y * image_h))
            cv2.drawMarker(canvas, ref_px, (0, 255, 0), cv2.MARKER_TILTED_CROSS, 32, 3)
            for f in foreground:
                cv2.rectangle(canvas, (int(f["x0"] * image_w), int(f["y0"] * image_h)), (int(f["x1"] * image_w) - 1, int(f["y1"] * image_h) - 1), (255, 0, 255), 3)
            analyzer.write_image(os.path.join(args.debug_dir, f"{row['name']}_verify.jpg"), canvas)

    report = {
        "overall_score": round(float(np.mean(scores)), 1) if scores else 0.0,
        "compared_shots": len(scores),
        "render_calibration": calibration,
        "output_directory": os.path.dirname(os.path.abspath(args.manifest)),
        "shots": rows,
    }
    with open(args.output, "w", encoding="utf-8") as handle:
        json.dump(report, handle, ensure_ascii=False, indent=2)
    print(f"UPT_VERIFY_SCORE={report['overall_score']}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
