"""UPT 로컬 포즈 레퍼런스 분석기.

레퍼런스 영상을 LLM/API 없이 분석해 UniversalProductionTools의 Reference Plan JSON(schema v1)을 만든다.
- 컷 경계: PySceneDetect ContentDetector
- 인물·관절: rtmlib YOLOX(HumanArt) + RTMPose (COCO 17 keypoints), ONNX Runtime CPU
- 카메라 모션: 인물 영역을 제외한 배경 특징점의 부분 어파인 변환 + 인물 화면 크기 변화
- 체형 보정: 영상 안에 전신이 잘 보이는 샷이 있으면 그 인물의 상체(어깨·얼굴) 높이 비율로 기본 비율표를 보정한다.

출력 JSON의 shot 값은 UPTReferenceBlockingSolver.cpp가 그대로 카메라로 역산할 수 있게 맞춘다.
특히 subject_screen_position은 "샷 크기별 조준 높이(focus height)"에 해당하는 몸 지점의 화면 좌표다.
"""
from __future__ import annotations

import argparse
import itertools
import json
import math
import os
import sys
from dataclasses import dataclass

import cv2
import numpy as np

ANALYZER_VERSION = "pose-v8"

# COCO 17 관절이 서 있는 사람 키에서 차지하는 높이 비율 기본값(발바닥 0, 정수리 1), 인체 측정 평균 기준.
# 팔꿈치·손목은 동작에 따라 크게 움직이므로 키 추정에 쓰지 않는다.
DEFAULT_KEYPOINT_RATIO = {
    0: 0.925, 1: 0.94, 2: 0.94, 3: 0.935, 4: 0.935,
    5: 0.83, 6: 0.83, 11: 0.53, 12: 0.53, 13: 0.285, 14: 0.285, 15: 0.045, 16: 0.045,
}
UPPER_BODY_KEYPOINTS = (0, 1, 2, 3, 4, 5, 6)
LOWER_BODY_KEYPOINTS = (13, 14, 15, 16)
HIP_AND_LOWER_KEYPOINTS = (11, 12) + LOWER_BODY_KEYPOINTS
FACE_KEYPOINTS = (0, 1, 2, 3, 4)
EYE_TO_EYE_BODY_RATIO = 0.036
# 어깨 관절 사이·귀 사이 거리가 키에서 차지하는 비율 기본값. 영상에 전신 샷이 있으면 calibrate_ratios가 그 인물 값으로 바꾼다.
DEFAULT_WIDTH_RATIO = {"shoulder_width": 0.21, "ear_width": 0.085}
# 인물별 체형 보정을 사람 평균 대비 어디까지 허용할지.
# 저폴리·갑옷 캐릭터는 어깨·머리 폭이 사람보다 훨씬 넓다(측정값 어깨 1.63배, 귀 2.12배).
# 상한이 1.35배였을 때는 보정이 한계에 걸려 클로즈업 키가 부풀려졌다. height_* 정답 126프레임에서
# 상한을 재 보니 2.0배가 가장 좋았다(전체 중앙 오차 9.8%→7.3%, 잘린 몸 13.8%→8.7%). 그 위로는 나아지지 않는다.
# 하한은 이 방향으로 벗어나는 표본이 없어 그대로 둔다.
WIDTH_RATIO_CLAMP = (0.75, 2.0)
# 화면 아래로 잘린 엉덩이·하체를 모델이 아래 가장자리에 붙여 예측하는 경우가 많아 아래쪽은 더 넓게 제외한다.
LOWER_EDGE_MARGIN = 0.06

# 아래 세 표는 UPTReferenceBlockingSolver.cpp의 표와 반드시 같아야 한다.
# 전신 화면 높이(H, 화면 세로 대비 발바닥~정수리 길이)로 샷 크기를 나눈다.
SHOT_SIZE_THRESHOLDS = (("wide", 0.45), ("full", 1.3), ("medium", 2.8), ("close_up", 5.5))
VISIBLE_BODY_RATIO = {"extreme_close_up": 0.12, "close_up": 0.22, "medium": 0.5, "full": 1.0, "wide": 1.0}
FOCUS_HEIGHT_RATIO = {"extreme_close_up": 0.92, "close_up": 0.88, "medium": 0.72, "full": 0.5, "wide": 0.5}

# 어깨 너머(OTS) 앞사람 단서: 사람 영역 분할(DeepLab v3, Pascal VOC)에서 인물 검출 박스 밖의 사람 영역이 화면 좌우 가장자리에 크게 붙어 있을 때.
SEGMENTATION_MODEL = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..",
                                                  "ThirdParty", "UPTAnalyzer", "models", "deeplab_v3.tflite"))
PASCAL_PERSON_CLASS = 15
FOREGROUND_MIN_AREA = 0.03
FOREGROUND_MIN_HEIGHT = 0.3
FOREGROUND_BOX_GROW = 0.03


@dataclass
class BodyMeasure:
    box: np.ndarray
    score: float
    height_px: float
    ay: float
    by: float
    ax: float
    bx: float
    # 기울거나 누운 자세로 판단해 몸 축 방향 직선을 쓴 경우. 서 있는 자세 전제의 단서(앵글 등)에서 뺄 때 쓴다.
    tilted: bool = False

    def point_at(self, ratio: float) -> tuple[float, float]:
        return self.ax + self.bx * ratio, self.ay + self.by * ratio

    def main_person_key(self) -> float:
        # 가장 크게 보이는 사람을 주인공으로 두되, 관절 점수가 낮은 오검출(배경 물체 등)이 크게 잡혀도 뽑히지 않게 점수로 깎는다.
        return self.height_px * min(1.0, self.score / 0.6)

    def tilt_degrees(self) -> float:
        """발→머리 방향이 화면 위쪽에서 기운 각도. 서 있으면 0 근처, 누우면 90 근처, 거꾸로면 90 이상."""
        return math.degrees(math.atan2(abs(self.bx), -self.by))


# 이보다 덜 기운 몸은 서 있는 자세로 보고 기존 세로 기준 계산을 그대로 쓴다(서 있는 장면의 결과를 바꾸지 않기 위함).
UPRIGHT_TILT_DEGREES = 25.0
# 이 이상 기울었을 때만 '기울어진 자세'로 표시한다(앵글 판정 제외, 상체만 보일 때 몸 축 직선 사용).
TILTED_BODY_DEGREES = 35.0


def axis_tilt_degrees(bx: float, by: float) -> float:
    return math.degrees(math.atan2(abs(bx), -by))


def is_confirmed_tilt(keypoints: np.ndarray, visible: list[int], axis: tuple[float, float]) -> bool:
    """몸 축 기울기는 얼굴 관절이나 한쪽만 보이는 짝 관절(어깨·엉덩이)에 끌려 서 있는 사람도 크게 기울어 보일 수 있다
    (실사 영상에서 한쪽 어깨만 보인 미디엄 샷이 70°로 기울어 키가 3배로 잡혔다).
    몸이 실제로 기울면 양 어깨선·양 엉덩이선도 같은 만큼 기울므로, 그 짝 관절 선이 몸 축과 맞을 때만 기울어진 자세로 믿는다."""
    tilt = axis_tilt_degrees(axis[0], axis[1])
    # 135°를 넘으면(머리가 발보다 아래) 누운 몸이 아니라 위에서 내려다본(부감) 서 있는 사람이다.
    if tilt < TILTED_BODY_DEGREES or tilt > 135.0:
        return False
    for left, right in ((5, 6), (11, 12)):
        if left in visible and right in visible:
            dx, dy = keypoints[left] - keypoints[right]
            if math.hypot(float(dx), float(dy)) < 6.0:
                continue
            line_tilt = math.degrees(math.atan2(abs(float(dy)), abs(float(dx))))
            return abs(min(tilt, 180.0 - tilt) - line_tilt) <= 25.0
    return False


def fitted_body_length(by: float, bx: float) -> float:
    """비율 1(정수리)당 픽셀 길이. 서 있으면 세로 성분, 기울거나 누우면 몸 축 전체 길이."""
    if by < 0.0 and math.degrees(math.atan2(abs(bx), -by)) < UPRIGHT_TILT_DEGREES:
        return -by
    return math.hypot(bx, by)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--video", required=True)
    parser.add_argument("--output", required=True, help="Reference Plan JSON 출력 경로")
    parser.add_argument("--mode", default="scene", choices=("scene", "edit"))
    parser.add_argument("--debug-dir", default="", help="관절·조준점 오버레이 이미지 저장 폴더")
    parser.add_argument("--model", default="balanced", choices=("lightweight", "balanced", "performance"))
    parser.add_argument("--samples-per-shot", type=int, default=4)
    parser.add_argument("--det-thr", type=float, default=0.5)
    parser.add_argument("--kpt-thr", type=float, default=0.35)
    parser.add_argument("--scene-threshold", type=float, default=27.0)
    parser.add_argument("--min-shot-seconds", type=float, default=0.5)
    parser.add_argument("--segmentation-model", default=SEGMENTATION_MODEL, help="어깨 너머 앞사람 검출용 DeepLab v3 TFLite 모델(없으면 이 단서만 끈다)")
    parser.add_argument("--no-foreground", action="store_true", help="어깨 너머 앞사람 검출을 끈다")
    return parser.parse_args()


def timecode_seconds(timecode) -> float:
    value = getattr(timecode, "seconds", None)
    if callable(value):
        value = value()
    return float(value if value is not None else timecode.get_seconds())


def detect_shots(video: str, fps: float, duration: float, threshold: float, min_seconds: float,
                 crop: tuple[int, int, int, int] | None = None) -> list[tuple[float, float]]:
    from scenedetect import ContentDetector, SceneManager, detect, open_video

    min_frames = max(1, int(round(min_seconds * fps)))
    if crop:
        manager = SceneManager()
        manager.add_detector(ContentDetector(threshold=threshold, min_scene_len=min_frames))
        # SceneManager.crop은 끝 좌표를 포함하는 값으로 받는다.
        manager.crop = (crop[0], crop[1], crop[2] - 1, crop[3] - 1)
        manager.detect_scenes(open_video(video))
        scenes = manager.get_scene_list(start_in_scene=True)
    else:
        scenes = detect(video, ContentDetector(threshold=threshold, min_scene_len=min_frames), start_in_scene=True)
    shots = [(timecode_seconds(start), timecode_seconds(end)) for start, end in scenes]
    return [shot for shot in shots if shot[1] - shot[0] > 0.05] or [(0.0, duration)]


def read_frame(capture: cv2.VideoCapture, frame_index: int):
    capture.set(cv2.CAP_PROP_POS_FRAMES, frame_index)
    ok, frame = capture.read()
    return frame if ok else None


ACTIVE_AREA_SAMPLES = 40


def detect_active_area(capture: cv2.VideoCapture, frame_count: int, width: int, height: int) -> tuple[int, int, int, int] | None:
    """영상 내내 바뀌지 않는 가장자리 띠(세로 쇼츠의 위아래 검은 띠, 그 위 고정 제목·로고)를 뺀 실제 화면 영역 (x0, y0, x1, y1).

    띠가 남아 있으면 고정 글자·로고가 배경 특징점으로 잡혀 모든 컷이 '같은 카메라'로 합쳐지고, 넓은 검은 영역이 화면 변화량을 희석해
    컷이 검출 기준에 못 미친다(쇼츠 드라마 클립에서 실제 컷 20개가 0개로). 인물 위치·크기·화면비도 띠를 뺀 영역 기준이어야 맞다.
    띠가 없거나 거의 전부 고정(정지 화면)이면 None.
    """
    frames = []
    for index in range(ACTIVE_AREA_SAMPLES):
        frame = read_frame(capture, int((index + 0.5) * frame_count / ACTIVE_AREA_SAMPLES))
        if frame is not None:
            frames.append(cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY).astype(np.float32))
    if len(frames) < 8:
        return None
    stack = np.stack(frames)
    temporal_std = stack.std(axis=0)
    brightness = stack.mean(axis=0)

    def band(std_line: np.ndarray, mean_line: np.ndarray) -> tuple[int, int]:
        # 변화가 거의 없고, 어둡거나(검은 띠와 그 위 글자) 완전히 고정된(밝은 로고·테두리) 줄은 띠로 본다.
        # 압축 잡음 때문에 고정 자막·로고도 std가 1~3으로 나오는 인코딩이 있어(같은 영상의 다른 화질) 기준을 조금 넓힌다.
        static = (std_line < 4.0) & ((mean_line < 60.0) | (std_line < 2.5))
        start, end = 0, len(static)
        while start < end and static[start]:
            start += 1
        while end > start and static[end - 1]:
            end -= 1
        return start, end

    y0, y1 = band(np.percentile(temporal_std, 90, axis=1), brightness.mean(axis=1))
    x0, x1 = band(np.percentile(temporal_std, 90, axis=0), brightness.mean(axis=0))
    if (y1 - y0) < 0.3 * height or (x1 - x0) < 0.3 * width:
        return None
    if (y1 - y0) > 0.95 * height and (x1 - x0) > 0.95 * width:
        return None
    return int(x0), int(y0), int(x1), int(y1)


class CroppedCapture:
    """cv2.VideoCapture처럼 쓰되 실제 화면 영역만 잘라 돌려준다(샘플·배경 움직임·가짜 컷 판정이 모두 같은 영역을 본다)."""

    def __init__(self, capture: cv2.VideoCapture, area: tuple[int, int, int, int]):
        self.capture, self.area = capture, area

    def set(self, prop: int, value: float) -> bool:
        return self.capture.set(prop, value)

    def get(self, prop: int) -> float:
        return self.capture.get(prop)

    def read(self):
        ok, frame = self.capture.read()
        if ok and frame is not None:
            x0, y0, x1, y1 = self.area
            frame = np.ascontiguousarray(frame[y0:y1, x0:x1])
        return ok, frame

    def release(self) -> None:
        self.capture.release()


# 두 프레임의 화면 내용 차이가 이보다 크면 같은 카메라가 이어진 것으로 보지 않는다.
# (같은 장면 안 흔들림은 1~3, 실제 컷은 15 이상으로 나온다)
MERGE_CONTENT_DIFFERENCE = 8.0


def frame_difference(before: np.ndarray | None, after: np.ndarray | None) -> float:
    """가장자리(자막·로고가 있는 곳)를 뺀 가운데 영역의 평균 밝기·색 차이."""
    if before is None or after is None:
        return 0.0
    height, width = before.shape[:2]
    y0, y1 = int(height * 0.1), int(height * 0.9)
    x0, x1 = int(width * 0.1), int(width * 0.9)
    small_before = cv2.cvtColor(cv2.resize(before[y0:y1, x0:x1], (96, 96)), cv2.COLOR_BGR2HSV).astype(np.float32)
    small_after = cv2.cvtColor(cv2.resize(after[y0:y1, x0:x1], (96, 96)), cv2.COLOR_BGR2HSV).astype(np.float32)
    return float(np.abs(small_before - small_after).mean())


def merge_false_cuts(capture: cv2.VideoCapture, shots: list[tuple[float, float]], fps: float, frame_count: int) -> tuple[list[tuple[float, float]], int]:
    """카메라 흔들림이나 빠른 이동 때문에 생긴 가짜 컷을 합친다.

    컷 직전·직후 프레임의 배경이 작은 이동·배율 변화로 대부분 맞아떨어지면 같은 카메라가 이어진 것으로 본다.
    단, 화면 내용 자체가 확 바뀌면 합치지 않는다. 고정 자막·로고가 남아 있으면 특징점이 그쪽에 몰려
    '같은 카메라'로 잘못 맞아떨어지기 때문이다(쇼츠 드라마 클립에서 실제 컷 12개가 합쳐졌다).
    """
    if len(shots) < 2:
        return shots, 0
    merged = [list(shots[0])]
    merged_count = 0
    for start, end in shots[1:]:
        boundary = int(round(start * fps))
        before = read_frame(capture, max(0, boundary - 2))
        after = read_frame(capture, min(frame_count - 1, boundary + 1))
        motion = background_motion(before, after, [], []) if before is not None and after is not None else None
        continuous = (motion is not None and motion["inlier_ratio"] >= 0.6 and abs(motion["tx"]) < 0.08
                      and abs(motion["ty"]) < 0.08 and 0.92 <= motion["scale"] <= 1.08)
        if continuous and frame_difference(before, after) > MERGE_CONTENT_DIFFERENCE:
            continuous = False
        if continuous:
            merged[-1][1] = end
            merged_count += 1
        else:
            merged.append([start, end])
    return [(start, end) for start, end in merged], merged_count


def usable_keypoints(keypoints: np.ndarray, scores: np.ndarray, kpt_thr: float, frame_w: int, frame_h: int, ratios: dict) -> list[int]:
    # 화면 밖으로 잘린 관절은 모델이 가장자리 근처에 높은 점수로 붙여 예측하는 경우가 많다.
    # 가장자리 띠 안의 관절과, 잘리기 쉬운 하체의 낮은 점수 관절은 키 추정에서 제외한다.
    margin_x, margin_y = frame_w * 0.03, frame_h * 0.04
    usable = []
    for index in ratios:
        if not isinstance(index, int):
            continue
        x, y = keypoints[index]
        threshold = max(kpt_thr, 0.5) if index in LOWER_BODY_KEYPOINTS else kpt_thr
        bottom = frame_h - frame_h * (LOWER_EDGE_MARGIN if index in HIP_AND_LOWER_KEYPOINTS else 0.04)
        if scores[index] >= threshold and margin_x <= x <= frame_w - margin_x and margin_y <= y <= bottom:
            usable.append(index)
    return usable


def fit_body(box: np.ndarray, keypoints: np.ndarray, scores: np.ndarray, kpt_thr: float, frame_w: int, frame_h: int,
             ratios: dict = DEFAULT_KEYPOINT_RATIO) -> BodyMeasure | None:
    visible = usable_keypoints(keypoints, scores, kpt_thr, frame_w, frame_h, ratios)
    if not visible:
        return None
    mean_score = float(np.mean([scores[i] for i in visible]))

    vertical, points = fit_vertical_line(keypoints, scores, visible, ratios)
    if vertical and any(i in HIP_AND_LOWER_KEYPOINTS for i in points):
        # 화면 밖으로 잘린 하체를 모델이 손·허리띠 위치에 붙여 예측하면 키가 절반 이하로 잡힌다.
        # 하체 관절이 하나뿐이거나, 세로 추정 키가 어깨 폭·귀 간격으로 본 키보다 크게 작으면(폭은 몸을 돌려도 줄어들기만 한다) 하체를 버린다.
        width_height = width_based_height(keypoints, visible, ratios)
        lower_count = sum(1 for i in points if i in HIP_AND_LOWER_KEYPOINTS)
        if lower_count >= 2 and (width_height is None or vertical[5] >= 0.65 * width_height):
            ay, by, ax, bx, _, length = vertical
            return BodyMeasure(box, mean_score, length, ay, by, ax, bx, tilted=TILTED_BODY_DEGREES <= axis_tilt_degrees(bx, by) <= 135.0)
        visible = [i for i in visible if i not in HIP_AND_LOWER_KEYPOINTS]
        vertical, _ = fit_vertical_line(keypoints, scores, visible, ratios)
    return fit_upper_body(box, keypoints, scores, visible, mean_score, ratios, vertical)


def fit_vertical_line(keypoints: np.ndarray, scores: np.ndarray, visible: list[int], ratios: dict) -> tuple[tuple | None, list[int]]:
    """관절 화면 좌표를 키 비율에 대한 2D 직선(발→정수리)으로 맞춘다. 몸이 기울거나 누워도 직선 방향을 그대로 쓴다.

    반환: ((ay, by, ax, bx, 비율 범위, 몸 길이 픽셀) 또는 None, 사용한 관절).
    """
    points = list(visible)
    while len(points) >= 2:
        point_ratios = np.array([ratios[i] for i in points])
        if point_ratios.max() - point_ratios.min() < 0.1:
            break
        xs = np.array([keypoints[i][0] for i in points])
        ys = np.array([keypoints[i][1] for i in points])
        weights = np.sqrt(np.array([scores[i] for i in points]))
        design = np.stack([np.ones_like(point_ratios), point_ratios], axis=1) * weights[:, None]
        (ay, by), *_ = np.linalg.lstsq(design, ys * weights, rcond=None)
        (ax, bx), *_ = np.linalg.lstsq(design, xs * weights, rcond=None)
        norm = math.hypot(float(bx), float(by))
        if norm <= 1.0:
            break
        # 가로 기울기(bx)는 좌우 짝 관절이 한쪽만 보이면 크게 틀어진다. 하체까지 보여 몸 축이 길게 잡히거나
        # 어깨선도 같은 만큼 기울어 기울어진 자세가 확인될 때만 몸 축 기준으로 재고, 아니면 서 있는 자세로 세로 기준을 쓴다.
        axis_unit = (float(bx) / norm, float(by) / norm)
        if axis_tilt_degrees(*axis_unit) < TILTED_BODY_DEGREES:
            # 조금 기운 몸: 하체까지 보여 몸 축이 길게 잡힐 때만 축 길이를 쓴다.
            allow_tilt = any(i in HIP_AND_LOWER_KEYPOINTS for i in points)
        else:
            # 크게 기운 몸: 하체가 보여도 짝 관절 선으로 확인될 때만 믿는다(로봇·소품 오검출이 크게 잡히는 것도 막는다).
            allow_tilt = is_confirmed_tilt(keypoints, points, axis_unit)
        length = fitted_body_length(float(by), float(bx)) if allow_tilt else float(-by)
        # 너무 짧거나, 정수리가 발보다 확연히 아래에 오는 뒤집힌 직선은 잘못 맞춘 것으로 본다.
        if length <= 1.0 or by > 0.5 * norm:
            break
        # 몸 직선에서 크게 벗어난 관절(잘못 예측된 관절)은 하나씩 빼고 다시 맞춘다.
        # 기울어진 몸은 좌우 짝 관절(어깨·귀)이 몸 축 옆으로 벌어져 있으므로 몸 축 방향 거리로만 잰다.
        if allow_tilt:
            residuals = np.abs((xs - (ax + bx * point_ratios)) * (bx / norm) + (ys - (ay + by * point_ratios)) * (by / norm))
        else:
            residuals = np.abs(ys - (ay + by * point_ratios))
        worst = int(np.argmax(residuals))
        if len(points) > 3 and residuals[worst] > max(8.0, 0.08 * length):
            points.pop(worst)
            continue
        return (float(ay), float(by), float(ax), float(bx), float(point_ratios.max() - point_ratios.min()), float(length)), points
    return None, points


def body_axis_direction(keypoints: np.ndarray, visible: list[int], vertical: tuple | None) -> tuple[float, float] | None:
    """발→머리 방향 단위 벡터. 직선 맞춤이 있으면 그 방향, 없으면 양 어깨 중점→머리 중심 방향(충분히 떨어져 있을 때)."""
    if vertical:
        _, by, _, bx, _, _ = vertical
        norm = math.hypot(bx, by)
        return (bx / norm, by / norm) if norm > 1.0 else None
    head_pair = next((pair for pair in ((3, 4), (1, 2)) if pair[0] in visible and pair[1] in visible), None)
    if not head_pair or 5 not in visible or 6 not in visible:
        return None
    delta = np.mean([keypoints[i] for i in head_pair], axis=0) - np.mean([keypoints[5], keypoints[6]], axis=0)
    norm = float(np.hypot(*delta))
    shoulder_width = float(np.hypot(*(keypoints[5] - keypoints[6])))
    if norm < max(6.0, 0.3 * shoulder_width):
        return None
    return float(delta[0] / norm), float(delta[1] / norm)


def width_based_height(keypoints: np.ndarray, visible: list[int], ratios: dict) -> float | None:
    """어깨 폭·귀 간격으로 본 키(픽셀). 폭은 몸을 돌리면 줄어들기만 하므로 두 값 중 큰 쪽을 쓴다."""
    estimates = []
    for (a, b), key, minimum in (((5, 6), "shoulder_width", 4.0), ((3, 4), "ear_width", 2.0)):
        if a in visible and b in visible:
            distance = float(np.hypot(*(keypoints[a] - keypoints[b])))
            if distance > minimum:
                estimates.append(distance / ratios.get(key, DEFAULT_WIDTH_RATIO[key]))
    return max(estimates) if estimates else None


def fit_upper_body(box: np.ndarray, keypoints: np.ndarray, scores: np.ndarray, visible: list[int], mean_score: float,
                   ratios: dict, vertical: tuple | None) -> BodyMeasure | None:
    """하체가 안 보이는 샷(미디엄 클로즈업~초근접)의 키·조준선을 추정한다.

    얼굴~어깨 세로 간격은 비율 차이가 작아 관절 몇 픽셀 오차로 키가 크게 흔들리고, 위에서 내려다보면 원근으로 짧아진다.
    어깨 폭·귀 간격은 그 영향을 받지 않으므로 세 추정을 오차 크기에 반비례하게 가중 평균한다.
    폭은 몸을 돌리면 줄어들기만 하므로, 세로 추정보다 크게 작은 폭 추정은 돌아선 자세로 보고 버린다.
    """
    def distance(a: int, b: int) -> float | None:
        if a in visible and b in visible:
            return float(np.hypot(*(keypoints[a] - keypoints[b])))
        return None

    vertical_log = float(np.log(vertical[5])) if vertical else None
    estimates = []  # (log 키 픽셀, 가중치)
    if vertical:
        # 정답지에서 비율 범위 약 0.12의 세로 맞춤은 log 흩어짐 약 0.12였다. 범위가 좁을수록 오차가 커진다.
        sigma = 0.12 * 0.12 / max(vertical[4], 0.03)
        estimates.append((vertical_log, 1.0 / sigma ** 2))
    width_estimates = []
    shoulder, ear, eye = distance(5, 6), distance(3, 4), distance(1, 2)
    if shoulder and shoulder > 4.0:
        width_estimates.append((float(np.log(shoulder / ratios.get("shoulder_width", DEFAULT_WIDTH_RATIO["shoulder_width"]))), 1.0 / 0.14 ** 2))
    if ear and ear > 2.0:
        width_estimates.append((float(np.log(ear / ratios.get("ear_width", DEFAULT_WIDTH_RATIO["ear_width"]))), 1.0 / 0.14 ** 2))
    elif eye and eye > 2.0:
        width_estimates.append((float(np.log(eye / EYE_TO_EYE_BODY_RATIO)), 1.0 / 0.18 ** 2))
    estimates += [e for e in width_estimates if vertical_log is None or e[0] >= vertical_log - 0.35]
    if not estimates:
        return None
    center = float(np.median([e[0] for e in estimates]))
    kept = [e for e in estimates if abs(e[0] - center) <= 0.3] or estimates
    height_px = float(np.exp(sum(e[0] * e[1] for e in kept) / sum(e[1] for e in kept)))

    upper = [i for i in visible if i in UPPER_BODY_KEYPOINTS]
    if not upper:
        return None
    weights = np.array([scores[i] for i in upper])
    anchor_ratio = float(np.average([ratios[i] for i in upper], weights=weights))
    anchor_y = float(np.average([keypoints[i][1] for i in upper], weights=weights))

    # 기울거나 누운 자세: 몸 축 방향으로 키만큼 뻗은 직선을 쓴다(관절 가중 중심을 지나게 한다).
    axis = body_axis_direction(keypoints, visible, vertical)
    if axis is not None and is_confirmed_tilt(keypoints, visible, axis):
        anchor_x = float(np.average([keypoints[i][0] for i in upper], weights=weights))
        tilted_bx, tilted_by = axis[0] * height_px, axis[1] * height_px
        return BodyMeasure(box, mean_score, height_px, anchor_y - tilted_by * anchor_ratio, tilted_by,
                           anchor_x - tilted_bx * anchor_ratio, tilted_bx, tilted=True)

    by = -height_px
    ay = anchor_y - by * anchor_ratio

    # 가로 위치: 한쪽 어깨만 보이면 직선이 그쪽으로 기울므로 머리 중심(귀·눈 중점)과 양 어깨 중점만 쓴다.
    head_pair = next((pair for pair in ((3, 4), (1, 2)) if pair[0] in visible and pair[1] in visible), None)
    head = None
    if head_pair:
        head = (float(np.mean([keypoints[i][0] for i in head_pair])), float(np.mean([ratios[i] for i in head_pair])))
    elif 0 in visible:
        head = (float(keypoints[0][0]), float(ratios[0]))
    shoulders = (float(np.mean([keypoints[5][0], keypoints[6][0]])), float(np.mean([ratios[5], ratios[6]]))) if 5 in visible and 6 in visible else None
    bx = 0.0
    if head and shoulders and abs(head[1] - shoulders[1]) > 0.02:
        bx = float(np.clip((head[0] - shoulders[0]) / (head[1] - shoulders[1]), -height_px, height_px))
        ax = head[0] - bx * head[1]
    elif head or shoulders:
        ax = (head or shoulders)[0]
    else:
        ax = float(np.average([keypoints[i][0] for i in upper], weights=weights))
    return BodyMeasure(box, mean_score, height_px, ay, by, ax, bx)


def drop_duplicate_people(raw: list) -> list:
    """같은 사람을 전신 박스와 상반신 박스로 두 번 검출한 경우 하나만 남긴다.

    박스 겹침만으로 판단하면 앞사람 박스 안에 들어간 뒷사람까지 지우므로, 두 검출에서 모두 확실한 관절 3개 이상이
    거의 같은 자리(큰 박스 높이의 8% 이내)에 있을 때만 같은 사람으로 본다. 넓고 점수 높은 검출을 남긴다.
    """
    def strength(person) -> float:
        box, _, scores = person
        return float((box[2] - box[0]) * (box[3] - box[1])) * float(np.mean(scores))

    kept = []
    for person in sorted(raw, key=strength, reverse=True):
        box, keypoints, scores = person
        duplicate = False
        for other_box, other_keypoints, other_scores in kept:
            shared = [i for i in range(len(scores)) if scores[i] >= 0.5 and other_scores[i] >= 0.5]
            if len(shared) < 3:
                continue
            limit = 0.08 * max(float(box[3] - box[1]), float(other_box[3] - other_box[1]))
            distances = [float(np.hypot(*(keypoints[i] - other_keypoints[i]))) for i in shared]
            if float(np.median(distances)) <= limit:
                duplicate = True
                break
        if not duplicate:
            kept.append(person)
    return kept


def detect_raw_people(detector, pose_model, frame: np.ndarray, kpt_thr: float,
                      all_boxes: list | None = None) -> list[tuple[np.ndarray, np.ndarray, np.ndarray]]:
    """all_boxes를 주면 관절 맞춤 성공 여부와 관계없이 사람으로 본 영역(검출 박스·초근접 전체 화면)을 담는다."""
    frame_h, frame_w = frame.shape[:2]
    boxes = detector(frame)
    boxes = np.asarray(boxes if boxes is not None else [], dtype=np.float32).reshape(-1, 4)
    boxes = np.array([b for b in boxes if (b[2] - b[0]) * (b[3] - b[1]) >= frame_w * frame_h * 0.001]).reshape(-1, 4)
    if all_boxes is not None:
        all_boxes.extend(boxes)
    raw = []
    if len(boxes) > 0:
        keypoints, scores = pose_model(frame, bboxes=list(boxes))
        for index, box in enumerate(boxes):
            measure = fit_body(box, keypoints[index], scores[index], kpt_thr, frame_w, frame_h)
            if measure and measure.score >= 0.2:
                raw.append((box, keypoints[index], scores[index]))
        raw = drop_duplicate_people(raw)
    if not raw:
        # 얼굴이 화면을 채운 초근접 샷은 사람 검출기가 놓치기 쉬워 화면 전체를 한 사람 영역으로 보고 관절을 한 번 더 찾는다.
        full_box = np.array([0.0, 0.0, float(frame_w), float(frame_h)], dtype=np.float32)
        keypoints, scores = pose_model(frame, bboxes=[full_box])
        strict = max(kpt_thr, 0.5)
        face_points = sum(1 for i in FACE_KEYPOINTS if scores[0][i] >= strict)
        measure = fit_body(full_box, keypoints[0], scores[0], strict, frame_w, frame_h)
        if measure and face_points >= 2 and measure.score >= 0.5:
            raw.append((full_box, keypoints[0], scores[0]))
            if all_boxes is not None:
                all_boxes.append(full_box)
    return raw


class ForegroundSegmenter:
    """DeepLab v3(Pascal VOC 21클래스, LiteRT)로 사람 영역을 나눠, 인물 검출기가 놓치는 화면 가장자리의 잘린 앞사람을 찾는다.

    어깨 너머 샷의 앞사람은 어깨·뒤통수 조각만 보여 YOLOX가 찾지 못한다(OTS 정답지에서 검출 기준 0.15로도 0개).
    모델 파일이나 실행기(ai-edge-litert)가 없으면 interpreter가 None이 되고 이 단서만 빠진다.
    """

    def __init__(self, model_path: str):
        self.interpreter, self.reason = None, ""
        if not model_path or not os.path.isfile(model_path):
            self.reason = f"model not found: {model_path}"
            return
        try:
            from ai_edge_litert.interpreter import Interpreter

            self.interpreter = Interpreter(model_path=model_path, num_threads=4)
            self.interpreter.allocate_tensors()
            self.input = self.interpreter.get_input_details()[0]
            self.output = self.interpreter.get_output_details()[0]
        except Exception as error:  # 실행기 미설치·DLL·모델 형식 오류
            self.interpreter, self.reason = None, f"{type(error).__name__}: {error}"

    def person_probability(self, frame: np.ndarray) -> np.ndarray:
        _, input_h, input_w, _ = (int(v) for v in self.input["shape"])
        rgb = cv2.cvtColor(cv2.resize(frame, (input_w, input_h), interpolation=cv2.INTER_AREA), cv2.COLOR_BGR2RGB)
        self.interpreter.set_tensor(self.input["index"], (rgb.astype(np.float32) / 127.5 - 1.0)[None])
        self.interpreter.invoke()
        logits = self.interpreter.get_tensor(self.output["index"])[0]
        exp = np.exp(logits - logits.max(axis=-1, keepdims=True))
        person = exp[..., PASCAL_PERSON_CLASS] / exp.sum(axis=-1)
        return cv2.resize(person.astype(np.float32), (frame.shape[1], frame.shape[0]), interpolation=cv2.INTER_LINEAR)


def foreground_candidates(segmenter: ForegroundSegmenter | None, frame: np.ndarray, person_boxes) -> list[dict]:
    """인물 검출 박스(조금 넓힘) 밖의 사람 영역 중 화면 좌우 가장자리에 닿고 충분히 큰 덩어리 = 잘린 앞사람 후보.

    검출된 인물 자신의 영역은 박스로 지우므로 클로즈업의 어깨가 가장자리에 닿아도 후보가 되지 않는다.
    """
    if segmenter is None or segmenter.interpreter is None:
        return []
    frame_h, frame_w = frame.shape[:2]
    probability = segmenter.person_probability(frame)
    outside = (probability >= 0.5).astype(np.uint8)
    grow_x, grow_y = FOREGROUND_BOX_GROW * frame_w, FOREGROUND_BOX_GROW * frame_h
    for box in person_boxes:
        outside[max(0, int(box[1] - grow_y)):max(0, int(box[3] + grow_y)), max(0, int(box[0] - grow_x)):max(0, int(box[2] + grow_x))] = 0
    count, components, stats, _ = cv2.connectedComponentsWithStats(outside)
    found = []
    for index in range(1, count):
        x, y, width, height, area = (int(v) for v in stats[index])
        side = "right" if x + width >= frame_w - 3 else ("left" if x <= 2 else "")
        if not side or area < FOREGROUND_MIN_AREA * frame_w * frame_h or height < FOREGROUND_MIN_HEIGHT * frame_h:
            continue
        found.append({"side": side, "x0": x / frame_w, "x1": (x + width) / frame_w, "y0": y / frame_h, "y1": (y + height) / frame_h,
                      "area": area / (frame_w * frame_h), "probability": float(probability[components == index].mean())})
    return found


def calibrate_ratios(raw_people: list, kpt_thr: float, frame_w: int, frame_h: int, min_samples: int = 3) -> tuple[dict, dict]:
    """전신(엉덩이·발목)이 선명하게 보이는 검출로 그 인물의 상체 관절 높이 비율을 추정한다.

    클로즈업은 어깨~얼굴 간격 하나로 키를 추정하므로, 머리가 큰 캐릭터처럼 체형이 평균과 다르면 오차가 크다.
    같은 영상의 전신 샷에서 비율을 구해 적용하면 샷 사이의 크기 판정이 일관된다.
    """
    base = DEFAULT_KEYPOINT_RATIO
    implied = {index: [] for index in UPPER_BODY_KEYPOINTS}
    widths = {key: [] for key in DEFAULT_WIDTH_RATIO}
    for _, keypoints, scores in raw_people:
        visible = set(usable_keypoints(keypoints, scores, max(kpt_thr, 0.6), frame_w, frame_h, base))
        hips = [i for i in (11, 12) if i in visible]
        ankles = [i for i in (15, 16) if i in visible]
        if not hips or not ankles:
            continue
        hip_y = float(np.mean([keypoints[i][1] for i in hips]))
        feet_y = float(np.mean([keypoints[i][1] for i in ankles]))
        if feet_y - hip_y < 30.0:
            continue
        # 체형 비율은 세로 간격으로 재므로, 다리가 기울어진(앉거나 누운) 자세는 보정에 쓰지 않는다.
        hip_x = float(np.mean([keypoints[i][0] for i in hips]))
        feet_x = float(np.mean([keypoints[i][0] for i in ankles]))
        if abs(feet_x - hip_x) > 0.35 * (feet_y - hip_y):
            continue
        pixels_per_ratio = (feet_y - hip_y) / (base[11] - base[15])
        for index in UPPER_BODY_KEYPOINTS:
            if index in visible:
                implied[index].append(base[11] + (hip_y - keypoints[index][1]) / pixels_per_ratio)
        # pixels_per_ratio는 키 1.0에 해당하는 픽셀 수이므로 어깨 폭·귀 간격을 그대로 나누면 키 대비 비율이다.
        # 인물이 작으면 귀 간격이 몇 픽셀뿐이라 관절 오차가 그대로 비율 오차가 되므로, 화면 세로의 절반 이상인 인물만 쓴다.
        if pixels_per_ratio < 0.5 * frame_h:
            continue
        if 5 in visible and 6 in visible:
            widths["shoulder_width"].append(float(np.hypot(*(keypoints[5] - keypoints[6]))) / pixels_per_ratio)
        if 3 in visible and 4 in visible:
            widths["ear_width"].append(float(np.hypot(*(keypoints[3] - keypoints[4]))) / pixels_per_ratio)

    ratios = dict(base)
    applied = {}
    for index, values in implied.items():
        if len(values) >= min_samples:
            ratios[index] = float(np.clip(np.median(values), base[index] - 0.06, base[index] + 0.06))
            applied[str(index)] = round(ratios[index], 3)
    for key, values in widths.items():
        if len(values) >= min_samples:
            default = DEFAULT_WIDTH_RATIO[key]
            ratios[key] = float(np.clip(np.median(values), default * WIDTH_RATIO_CLAMP[0], default * WIDTH_RATIO_CLAMP[1]))
            applied[key] = round(ratios[key], 3)
    return ratios, applied


def horizon_estimate(frame: np.ndarray, boxes) -> tuple[float, float] | None:
    """사람 박스 밖에서 세로 밝기 변화가 가장 강한 가로줄(지평선·바닥과 하늘 경계 후보)의 행 좌표와 선명도를 구한다."""
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY).astype(np.float32)
    grad = np.abs(cv2.Sobel(gray, cv2.CV_32F, 0, 1, ksize=5))
    mask = np.ones(gray.shape, dtype=bool)
    for box in boxes:
        x0, y0, x1, y1 = [int(v) for v in box]
        mask[max(0, y0):max(0, y1), max(0, x0 - 20):max(0, x1 + 20)] = False
    grad[~mask] = 0
    valid = mask.sum(axis=1)
    # 클로즈업은 인물 박스가 화면 폭 대부분을 덮으므로, 가려지지 않은 폭이 12%만 남아도 그 줄을 평가한다.
    usable_rows = valid > gray.shape[1] * 0.12
    strength = np.where(usable_rows, grad.sum(axis=1) / np.maximum(1, valid), 0.0)
    margin = int(gray.shape[0] * 0.03)
    if margin * 2 >= len(strength) or not np.any(strength > 0):
        return None
    row = int(np.argmax(strength[margin:-margin])) + margin
    contrast = float(strength[row] / (np.median(strength[usable_rows]) + 1e-6))
    return float(row), contrast


def estimate_camera_angle(detected_samples: list[dict], width: int, height: int, kpt_thr: float,
                          focus_ratio: float | None = None) -> tuple[str, float, dict]:
    """카메라 앵글을 추정한다. 반환: (앵글, 신뢰도, 사용한 단서).

    1) 코가 어깨선보다 아래로 보이면 부감(overhead).
    2) 지평선이 선명하면 지평선이 몸을 가로지르는 높이 = 카메라 높이로 판정한다(카메라 기울기와 무관).
    3) 지평선이 없으면 코-어깨 간격(어깨 폭 대비)으로 판정한다. 위에서 볼수록 간격이 줄고 아래에서 볼수록 늘어난다.
    임계값은 언리얼 정답지(Mannequin)로 정했으므로 배우가 고개를 숙이거나 드는 동작도 같은 방향으로 영향을 준다.
    """
    nose_shoulder_values, horizon_ratios, head_width_ratios = [], [], []
    for s in detected_samples:
        raw = s.get("main_raw")
        if raw is None:
            continue
        # 코-어깨 간격·지평선 높이·머리 폭 단서는 모두 서 있는 자세를 전제로 하므로 기울거나 누운 인물은 앵글 판정에 쓰지 않는다.
        if s["main"].tilted:
            continue
        _, keypoints, scores = raw
        usable = set(usable_keypoints(keypoints, scores, kpt_thr, width, height, DEFAULT_KEYPOINT_RATIO))
        if {0, 5, 6} <= usable:
            shoulder_width = float(np.hypot(*(keypoints[5] - keypoints[6])))
            if shoulder_width > 8.0:
                nose_shoulder_values.append(((keypoints[5][1] + keypoints[6][1]) / 2.0 - keypoints[0][1]) / shoulder_width)
        if {3, 4} <= usable and s["main"].height_px > 0:
            head_width_ratios.append(float(np.hypot(*(keypoints[3] - keypoints[4]))) / s["main"].height_px)
        horizon = s.get("horizon")
        if horizon and horizon[1] >= 20.0:
            main = s["main"]
            horizon_ratios.append((main.ay - horizon[0]) / -main.by)

    nose_shoulder = float(np.median(nose_shoulder_values)) if nose_shoulder_values else None
    horizon_ratio = float(np.median(horizon_ratios)) if horizon_ratios else None
    head_width = float(np.median(head_width_ratios)) if head_width_ratios else None
    cues = {"nose_to_shoulders": None if nose_shoulder is None else round(nose_shoulder, 3),
            "horizon_body_ratio": None if horizon_ratio is None else round(horizon_ratio, 3),
            "head_width_to_height": None if head_width is None else round(head_width, 3)}
    # 정답지에서 부감은 코-어깨 값이 항상 -0.24 이하, 하이앵글은 -0.08까지 내려갔으므로 -0.15를 경계로 둔다.
    if nose_shoulder is not None and nose_shoulder < -0.15:
        return "overhead", 0.7, cues
    if horizon_ratio is not None:
        # 시네마틱 생성기의 eye는 "카메라 높이 = 샷 크기별 조준 높이"다. 지평선으로 잰 높이는 실제의 약 0.8배로 나오므로
        # 조준 높이를 같은 비율로 환산한 값보다 충분히 낮을 때만 로우앵글로 본다(전신 샷의 허리 높이 카메라는 eye).
        low_limit = 0.4 if focus_ratio is None else focus_ratio * 0.8 - 0.15
        cues["low_limit"] = round(low_limit, 3)
        if horizon_ratio < low_limit:
            return "low", 0.75, cues
        # 정답지 eye 샷의 지평선 비율은 최대 0.80, 클로즈업 하이앵글은 0.97이었다(상체 샷 키 추정이 폭 단서로 커지며 비율이 줄었다).
        if horizon_ratio > 0.9:
            return "high", 0.75, cues
        return "eye", 0.75, cues
    if nose_shoulder is not None:
        # 로우앵글과 눈높이는 0.35 부근에서 겹치므로 애매하면 눈높이로 둔다.
        if nose_shoulder > 0.37:
            return "low", 0.45, cues
        if nose_shoulder < 0.2:
            return "high", 0.45, cues
        return "eye", 0.45, cues
    # 코·어깨가 안 보이는 경우: 위에서 내려다보면 몸이 짧아져 머리 폭 대비 키 비율이 커진다.
    if head_width is not None and head_width > 0.25:
        return "overhead", 0.5, cues
    return "eye", 0.3, cues


def appearance_descriptor(frame: np.ndarray, person: tuple) -> np.ndarray | None:
    """샷이 바뀌어도 같은 인물을 알아보기 위한 몸통(옷) 영역의 색 히스토그램(HSV의 색상·채도 72칸 + 밝기 8칸)."""
    box, keypoints, scores = person
    frame_h, frame_w = frame.shape[:2]
    rects = []
    if scores[5] >= 0.3 and scores[6] >= 0.3:
        left, right = sorted([float(keypoints[5][0]), float(keypoints[6][0])])
        shoulder_width = max(8.0, right - left)
        top = float(min(keypoints[5][1], keypoints[6][1]))
        hips = [float(keypoints[i][1]) for i in (11, 12) if scores[i] >= 0.3]
        bottom = float(np.mean(hips)) if hips and np.mean(hips) > top + 10.0 else top + 1.2 * shoulder_width
        rects.append((left + 0.1 * shoulder_width, top + 0.1 * (bottom - top), right - 0.1 * shoulder_width, bottom))
    box_w, box_h = float(box[2] - box[0]), float(box[3] - box[1])
    rects.append((box[0] + 0.25 * box_w, box[1] + 0.25 * box_h, box[2] - 0.25 * box_w, box[1] + 0.65 * box_h))
    for x0, y0, x1, y1 in rects:
        xi0, xi1 = int(max(0.0, x0)), int(min(float(frame_w), x1))
        yi0, yi1 = int(max(0.0, y0)), int(min(float(frame_h), y1))
        if (xi1 - xi0) * (yi1 - yi0) < 150:
            continue
        hsv = cv2.cvtColor(frame[yi0:yi1, xi0:xi1], cv2.COLOR_BGR2HSV)
        hue_saturation = cv2.calcHist([hsv], [0, 1], None, [12, 6], [0, 180, 0, 256]).flatten()
        value = cv2.calcHist([hsv], [2], None, [8], [0, 256]).flatten()
        hue_saturation /= max(float(hue_saturation.sum()), 1.0)
        value /= max(float(value.sum()), 1.0)
        return np.concatenate([hue_saturation, value]).astype(np.float32)
    return None


def appearance_distance(a: np.ndarray, b: np.ndarray) -> float:
    """0(같은 색 분포)~1(전혀 다름). 조명에 흔들리는 밝기보다 색상·채도에 더 큰 비중을 둔다."""
    hue_saturation = cv2.compareHist(a[:72], b[:72], cv2.HISTCMP_BHATTACHARYYA)
    value = cv2.compareHist(a[72:], b[72:], cv2.HISTCMP_BHATTACHARYYA)
    return float(0.6 * hue_saturation + 0.4 * value)


# 기존 인물 누구와도 이 거리보다 멀면 새 인물로 본다.
IDENTITY_NEW_THRESHOLD = 0.35
# 같은 샷에 함께 나온 적 없는 두 인물은 이 거리보다 가까우면 같은 사람이 조명·거리 때문에 달라 보인 것으로 보고 합친다.
IDENTITY_MERGE_THRESHOLD = 0.65
IDENTITY_SINGLE_MERGE_THRESHOLD = 0.9


def assign_identities(plan_shots: list[dict]) -> dict:
    """샷마다 '가장 크게 보이는 사람'으로 붙던 역할 이름을, 옷 색이 같은 사람은 샷이 바뀌어도 같은 이름이 되도록 다시 붙인다.

    한 샷 안의 서로 다른 사람은 같은 인물일 수 없다는 조건을 두고, 인물이 많은 샷부터 기준 인물을 만든 뒤 나머지 샷을 비용이 가장 작게 대응시킨다.
    화면에 가장 크고 오래 나온 인물이 main_character가 된다. 각 샷의 subject_role은 그 샷에서 가장 크게 보이는 사람의 역할이다.
    """
    identities = []  # {"proto": 평균 히스토그램, "count": 등장 수, "weight": 화면 크기×길이 합}
    assignment = {}
    order = sorted(range(len(plan_shots)), key=lambda index: (-len(plan_shots[index].get("subjects", [])), index))
    for shot_index in order:
        shot = plan_shots[shot_index]
        subjects = shot.get("subjects", [])
        descriptors = shot.get("_appearance") or []
        people = [j for j in range(len(subjects)) if j < len(descriptors) and descriptors[j] is not None]
        if not people:
            continue
        options = list(range(len(identities))) + [None] * len(people)
        best, best_cost = None, float("inf")
        for combo in itertools.permutations(options, len(people)):
            used = [choice for choice in combo if choice is not None]
            if len(used) != len(set(used)):
                continue
            cost = sum(IDENTITY_NEW_THRESHOLD if choice is None else appearance_distance(descriptors[j], identities[choice]["proto"])
                       for j, choice in zip(people, combo))
            if cost < best_cost:
                best, best_cost = combo, cost
        duration = max(0.1, shot["end_seconds"] - shot["start_seconds"])
        for j, choice in zip(people, best):
            weight = subjects[j]["screen_size"]["height"] * duration
            if choice is None:
                identities.append({"proto": descriptors[j].copy(), "count": 1, "weight": weight})
                choice = len(identities) - 1
            else:
                identity = identities[choice]
                identity["proto"] = (identity["proto"] * identity["count"] + descriptors[j]) / (identity["count"] + 1)
                identity["count"] += 1
                identity["weight"] += weight
            assignment[(shot_index, j)] = choice

    # 인물 수가 한 샷에 동시에 나온 최대 인원보다 많으면, 한 번도 같은 샷에 함께 나오지 않은 인물끼리 가장 비슷한 쌍을 합친다.
    # 같은 사람이 멀리서 밝게/가까이서 어둡게 찍혀 둘로 나뉜 경우를 되돌리되, 색이 크게 다르면 다른 사람으로 둔다.
    max_people = max((len(shot.get("subjects", [])) for shot in plan_shots), default=0)
    # 두 명 이상이 함께 나온 샷이 없으면 여러 인물이라는 근거가 없다. 원거리 전신과 클로즈업은 같은 옷도 0.4~0.66까지 달라 보이므로 크게 다를 때만 나눈다.
    merge_threshold = IDENTITY_MERGE_THRESHOLD if max_people >= 2 else IDENTITY_SINGLE_MERGE_THRESHOLD

    def shots_of(identity_index: int) -> set:
        return {key[0] for key, choice in assignment.items() if choice == identity_index}

    while len(identities) > max(1, max_people):
        best_pair, best_distance = None, merge_threshold
        for a in range(len(identities)):
            for b in range(a + 1, len(identities)):
                if shots_of(a) & shots_of(b):
                    continue
                distance = appearance_distance(identities[a]["proto"], identities[b]["proto"])
                if distance < best_distance:
                    best_pair, best_distance = (a, b), distance
        if best_pair is None:
            break
        a, b = best_pair
        keep, merged = identities[a], identities[b]
        keep["proto"] = (keep["proto"] * keep["count"] + merged["proto"] * merged["count"]) / (keep["count"] + merged["count"])
        keep["count"] += merged["count"]
        keep["weight"] += merged["weight"]
        identities.pop(b)
        assignment = {key: (a if choice == b else (choice - 1 if choice > b else choice)) for key, choice in assignment.items()}

    ranking = sorted(range(len(identities)), key=lambda index: -identities[index]["weight"])
    names = {index: ("main_character" if rank == 0 else f"character_{rank + 1}") for rank, index in enumerate(ranking)}
    extra = len(identities)
    for shot_index, shot in enumerate(plan_shots):
        subjects = shot.get("subjects", [])
        used = {assignment[(shot_index, j)] for j in range(len(subjects)) if (shot_index, j) in assignment}
        for j, subject in enumerate(subjects):
            choice = assignment.get((shot_index, j))
            if choice is None:
                # 색을 못 잰 사람(너무 작거나 잘림)은 이 샷에 아직 안 나온 인물 중 비중이 큰 인물로 본다.
                choice = next((index for index in ranking if index not in used), None)
                if choice is None:
                    extra += 1
                    names[-extra] = f"character_{extra}"
                    choice = -extra
                used.add(choice)
            subject["role"] = names[choice]
        if subjects:
            shot["subject_role"] = subjects[0]["role"]
        shot.pop("_appearance", None)
    return {"count": len(identities), "shot_roles": [[s["role"] for s in shot.get("subjects", [])] for shot in plan_shots]}


def classify_shot_size(full_body_height: float) -> str:
    for label, limit in SHOT_SIZE_THRESHOLDS:
        if full_body_height <= limit:
            return label
    return "extreme_close_up"


def background_motion(frame_a: np.ndarray, frame_b: np.ndarray, boxes_a, boxes_b) -> dict | None:
    scale = 640.0 / frame_a.shape[1]
    size = (640, int(round(frame_a.shape[0] * scale)))
    gray_a = cv2.cvtColor(cv2.resize(frame_a, size), cv2.COLOR_BGR2GRAY)
    gray_b = cv2.cvtColor(cv2.resize(frame_b, size), cv2.COLOR_BGR2GRAY)

    def mask_for(boxes):
        mask = np.full(gray_a.shape, 255, dtype=np.uint8)
        for box in boxes:
            x0, y0, x1, y1 = (np.asarray(box) * scale).astype(int)
            pad = int(0.1 * max(x1 - x0, y1 - y0))
            cv2.rectangle(mask, (x0 - pad, y0 - pad), (x1 + pad, y1 + pad), 0, -1)
        return mask

    orb = cv2.ORB_create(1500)
    kp_a, des_a = orb.detectAndCompute(gray_a, mask_for(boxes_a))
    kp_b, des_b = orb.detectAndCompute(gray_b, mask_for(boxes_b))
    if des_a is None or des_b is None or len(kp_a) < 20 or len(kp_b) < 20:
        return None
    matches = sorted(cv2.BFMatcher(cv2.NORM_HAMMING, crossCheck=True).match(des_a, des_b), key=lambda m: m.distance)[:400]
    if len(matches) < 15:
        return None
    src = np.float32([kp_a[m.queryIdx].pt for m in matches])
    dst = np.float32([kp_b[m.trainIdx].pt for m in matches])
    matrix, inliers = cv2.estimateAffinePartial2D(src, dst, method=cv2.RANSAC, ransacReprojThreshold=3.0)
    if matrix is None:
        return None
    center = np.array([size[0] / 2.0, size[1] / 2.0, 1.0])
    moved = matrix @ center
    return {
        "scale": float(math.hypot(matrix[0, 0], matrix[1, 0])),
        "tx": float((moved[0] - center[0]) / size[0]),
        "ty": float((moved[1] - center[1]) / size[1]),
        "inlier_ratio": float(np.mean(inliers)) if inliers is not None else 0.0,
    }


def classify_motion(pairs: list[dict], samples: list[dict]) -> tuple[str, dict]:
    detected = [s for s in samples if s["main"]]
    subject_xs = [s["focus"][0] for s in detected]
    info = {"pairs": len(pairs)}
    if not pairs:
        return "static", info

    net_scale = float(np.prod([p["scale"] for p in pairs]))
    net_tx = float(sum(p["tx"] for p in pairs))
    net_ty = float(sum(p["ty"] for p in pairs))
    path = float(sum(math.hypot(p["tx"], p["ty"]) for p in pairs))

    def half_ratio(values: list[float]) -> float | None:
        # 첫·끝 샘플 두 장만 비교하면 한 장의 키 추정 잡음이나 주인공 뒤바뀜으로 돌리 방향이 뒤집힌다(실사 영상에서 확인).
        # 앞 절반과 뒤 절반의 중앙값을 비교하고, 샘플이 3장 미만이면 판단하지 않는다.
        if len(values) < 3 or min(values) <= 0:
            return None
        half = len(values) // 2
        return float(np.median(values[-half:]) / np.median(values[:half]))

    subject_scale = half_ratio([s["main"].height_px for s in detected])
    # 검출 박스 높이는 관절 기반 키보다 거칠지만 잡음의 원인이 달라, 두 값이 같은 방향으로 변할 때만 인물 크기 변화로 믿는다.
    box_scale = half_ratio([float(s["main"].box[3] - s["main"].box[1]) for s in detected])
    subject_dx = subject_xs[-1] - subject_xs[0] if len(subject_xs) >= 2 else None
    info.update(net_scale=round(net_scale, 3), net_tx=round(net_tx, 3), net_ty=round(net_ty, 3), path=round(path, 3),
                subject_scale=None if subject_scale is None else round(subject_scale, 3),
                box_scale=None if box_scale is None else round(box_scale, 3),
                subject_dx=None if subject_dx is None else round(subject_dx, 3))

    # 핸드헬드는 이동 방향이 자주 뒤집히고 누적 이동이 경로 길이에 비해 짧다. 패닝·줌은 한 방향으로 꾸준히 변한다.
    def reversals(axis: str) -> int:
        return sum(1 for a, b in zip(pairs, pairs[1:]) if a[axis] * b[axis] < 0 and abs(a[axis]) > 0.004 and abs(b[axis]) > 0.004)

    direction_changes = reversals("tx") + reversals("ty")
    info["direction_changes"] = direction_changes
    if (len(pairs) >= 3 and path > 0.05 and abs(math.log(max(net_scale, 1e-6))) < 0.06
            and (direction_changes >= 3 or math.hypot(net_tx, net_ty) < path * 0.5)):
        return "handheld", info
    # 방향이 매우 자주 뒤집히며 크게 움직이면(망원경·스코프 시점 등) 배경 배율 추정도 믿을 수 없으므로 줌보다 핸드헬드로 본다.
    if len(pairs) >= 4 and direction_changes >= 5 and path > 0.3:
        return "handheld", info
    # 가까운 인물이 배경보다 확연히 더 커지면 카메라가 실제로 다가간 것(시차), 함께 커지면 줌이다.
    if subject_scale and box_scale and subject_scale > 1.08 and box_scale > 1.04 and subject_scale > net_scale * 1.06:
        return "dolly_in", info
    if subject_scale and box_scale and subject_scale < 0.92 and box_scale < 0.96 and subject_scale < net_scale / 1.06:
        return "dolly_out", info
    if net_scale > 1.06:
        return "zoom_in", info
    if net_scale < 0.94:
        return "zoom_out", info
    if abs(net_tx) > 0.06 or abs(net_ty) > 0.06:
        if subject_dx is not None and abs(subject_dx) < 0.04 and abs(net_tx) > 0.06:
            return "tracking", info
        return ("pan" if abs(net_tx) >= abs(net_ty) else "tilt"), info
    return "static", info


def classify_composition(main_x: float, subject_xs: list[float]) -> str:
    if len(subject_xs) >= 2 and abs((subject_xs[0] + subject_xs[1]) / 2.0 - 0.5) < 0.08:
        return "symmetry"
    if abs(main_x - 0.5) < 0.07:
        return "center"
    if min(abs(main_x - 1.0 / 3.0), abs(main_x - 2.0 / 3.0)) < 0.09:
        return "thirds"
    return "leading_space"


def clamp01(value: float, low: float = 0.01, high: float = 0.99) -> float:
    return float(min(high, max(low, value)))


def write_image(path: str, image: np.ndarray) -> None:
    ok, encoded = cv2.imencode(os.path.splitext(path)[1] or ".jpg", image)
    if ok:
        encoded.tofile(path)


def draw_debug(frame: np.ndarray, people: list[BodyMeasure], label: str, focus_ratio: float, text: str) -> np.ndarray:
    canvas = frame.copy()
    for rank, person in enumerate(people):
        color = (0, 220, 255) if rank == 0 else (255, 180, 0)
        x0, y0, x1, y1 = person.box.astype(int)
        cv2.rectangle(canvas, (x0, y0), (x1, y1), color, 2)
        top = person.point_at(1.0)
        bottom = person.point_at(0.0)
        cv2.line(canvas, (int(top[0]), int(top[1])), (int(bottom[0]), int(bottom[1])), color, 1)
        fx, fy = person.point_at(focus_ratio)
        cv2.drawMarker(canvas, (int(fx), int(fy)), (0, 0, 255), cv2.MARKER_CROSS, 28, 3)
    cv2.putText(canvas, f"{label} | {text}", (16, 36), cv2.FONT_HERSHEY_SIMPLEX, 0.9, (0, 0, 0), 4)
    cv2.putText(canvas, f"{label} | {text}", (16, 36), cv2.FONT_HERSHEY_SIMPLEX, 0.9, (255, 255, 255), 2)
    return canvas


def sample_shots(capture, shots, fps, frame_count, detector, pose_model, args, segmenter: ForegroundSegmenter | None = None) -> tuple[list, list]:
    """1단계: 샷마다 대표 프레임을 뽑아 인물·관절을 검출하고 배경 움직임을 잰다(키 계산은 체형 보정 뒤에 한다)."""
    shot_samples, all_raw = [], []
    for shot_index, (start, end) in enumerate(shots):
        print(f"UPT_PROGRESS shot {shot_index + 1}/{len(shots)}", flush=True)
        shot_length = end - start
        margin = min(0.1 * shot_length, 0.25)
        count = max(1, args.samples_per_shot if shot_length > 0.4 else 1)
        if count > 1:
            times = [start + margin + (shot_length - 2 * margin) * (i / (count - 1)) for i in range(count)]
        else:
            times = [start + shot_length / 2]

        samples = []
        for sample_time in times:
            frame = read_frame(capture, min(max(0, frame_count - 1), int(round(sample_time * fps))))
            if frame is None:
                continue
            detector_boxes = []
            raw = detect_raw_people(detector, pose_model, frame, args.kpt_thr, detector_boxes)
            boxes = [person[0] for person in raw]
            sample = {"time": sample_time, "raw": raw, "boxes": boxes, "horizon": horizon_estimate(frame, boxes),
                      "appearance": [appearance_descriptor(frame, person) for person in raw],
                      "foreground": foreground_candidates(segmenter, frame, detector_boxes)}
            if args.debug_dir:
                sample["frame_jpg"] = cv2.imencode(".jpg", frame)[1]
            samples.append(sample)
            all_raw.extend(raw)

        # 카메라 모션은 관절 검출보다 가벼우므로 더 촘촘한 8개 프레임으로 배경 움직임을 잰다. 인물 영역은 샷 전체 검출 박스로 가린다.
        person_boxes = [box for s in samples for box in s["boxes"]]
        pairs, previous_frame = [], None
        if shot_length > 0.8:
            for motion_index in range(8):
                motion_time = start + margin + (shot_length - 2 * margin) * (motion_index / 7)
                frame = read_frame(capture, min(max(0, frame_count - 1), int(round(motion_time * fps))))
                if frame is None:
                    continue
                if previous_frame is not None:
                    motion = background_motion(previous_frame, frame, person_boxes, person_boxes)
                    if motion:
                        pairs.append(motion)
                previous_frame = frame
        shot_samples.append((start, end, samples, pairs))
    return shot_samples, all_raw


def build_shot(shot_index: int, start: float, end: float, samples: list, pairs: list, ratios: dict, width: int, height: int, kpt_thr: float) -> dict:
    """2단계: 보정된 비율표로 샷 크기·조준점·모션·신뢰도를 계산한다."""
    for s in samples:
        fitted = []
        appearances = s.get("appearance") or [None] * len(s["raw"])
        for raw, appearance in zip(s["raw"], appearances):
            measure = fit_body(raw[0], raw[1], raw[2], kpt_thr, width, height, ratios)
            if measure and measure.score >= 0.2:
                fitted.append((measure, raw, appearance))
        fitted.sort(key=lambda item: item[0].main_person_key(), reverse=True)
        s["people"] = [item[0] for item in fitted]
        s["people_appearance"] = [item[2] for item in fitted]
        s["main"] = fitted[0][0] if fitted else None
        s["main_raw"] = fitted[0][1] if fitted else None

    detected = [s for s in samples if s["main"]]
    name = f"Shot_{shot_index + 1:02d}"
    if not detected:
        return {
            "name": name, "subject_role": "", "start_seconds": round(start, 3), "end_seconds": round(end, 3),
            "shot_size": "wide", "camera_angle": "eye", "camera_motion": "static",
            "subject_screen_position": {"x": 0.5, "y": 0.5}, "subject_screen_size": {"width": 0.3, "height": 0.5},
            "subjects": [], "composition": "center", "confidence": 0.1,
            "diagnostics": {"people_max": 0, "samples": len(samples)},
        }

    full_body_height = float(np.median([s["main"].height_px / height for s in detected]))
    shot_size = classify_shot_size(full_body_height)
    focus_ratio = FOCUS_HEIGHT_RATIO[shot_size]
    for s in detected:
        fx, fy = s["main"].point_at(focus_ratio)
        s["focus"] = (fx / width, fy / height)
    focus_x = float(np.median([s["focus"][0] for s in detected]))
    focus_y = float(np.median([s["focus"][1] for s in detected]))
    box_width = float(np.median([(s["main"].box[2] - s["main"].box[0]) / width for s in detected]))
    motion, motion_info = classify_motion(pairs, samples)
    camera_angle, angle_confidence, angle_cues = estimate_camera_angle(detected, width, height, kpt_thr, focus_ratio)

    quality = float(np.mean([s["main"].score for s in detected]))
    coverage = len(detected) / max(1, len(samples))
    consistency = 1.0
    if motion == "static" and len(detected) >= 2:
        heights = [s["main"].height_px for s in detected]
        consistency -= 0.5 * min(1.0, float(np.std(heights) / max(1e-6, np.mean(heights))) * 4.0)
        consistency -= 0.3 * min(1.0, float(np.std([s["focus"][0] for s in detected])) * 8.0)
    confidence = clamp01(0.15 + 0.85 * min(1.0, quality) * coverage * max(0.0, consistency), 0.0, 1.0)

    # 어깨 너머(OTS): 인물이 검출된 샘플의 절반 이상에서 같은 쪽 가장자리에 잘린 앞사람 영역이 보일 때만 기록한다.
    over_shoulder = None
    largest = [max(s["foreground"], key=lambda f: f["area"]) for s in detected if s.get("foreground")]
    if largest:
        side = max(("left", "right"), key=lambda name: sum(1 for f in largest if f["side"] == name))
        chosen = [f for f in largest if f["side"] == side]
        if len(chosen) * 2 >= len(detected):
            over_shoulder = {
                "side": side,
                "screen_x": round(float(np.median([(f["x0"] + f["x1"]) / 2 for f in chosen])), 3),
                "screen_width": round(float(np.median([f["x1"] - f["x0"] for f in chosen])), 3),
                "screen_height": round(float(np.median([f["y1"] - f["y0"] for f in chosen])), 3),
                "confidence": round(min(1.0, len(chosen) / len(detected)) * float(np.mean([f["probability"] for f in chosen])), 2),
            }

    reference = max(detected, key=lambda s: len(s["people"]))
    subjects = []
    for rank, person in enumerate(reference["people"][:4]):
        px, py = person.point_at(focus_ratio if rank == 0 else 0.5)
        subjects.append({
            "role": "main_character" if rank == 0 else f"character_{rank + 1}",
            "screen_position": {"x": clamp01(px / width), "y": clamp01(py / height)},
            "screen_size": {"width": clamp01((person.box[2] - person.box[0]) / width),
                            "height": clamp01(VISIBLE_BODY_RATIO[shot_size] * person.height_px / height)},
            "depth_order": rank,
        })
    shot = {
        "name": name,
        "subject_role": "main_character",
        "start_seconds": round(start, 3),
        "end_seconds": round(end, 3),
        "shot_size": shot_size,
        "camera_angle": camera_angle,
        "camera_motion": motion,
        "subject_screen_position": {"x": clamp01(focus_x), "y": clamp01(focus_y)},
        "subject_screen_size": {"width": clamp01(box_width), "height": clamp01(VISIBLE_BODY_RATIO[shot_size] * full_body_height)},
        "subjects": subjects,
        "_appearance": reference.get("people_appearance", [])[:4],
        "composition": classify_composition(focus_x, sorted(s["screen_position"]["x"] for s in subjects)),
        "confidence": round(confidence, 2),
        "diagnostics": {
            "full_body_screen_height": round(full_body_height, 3),
            "focus_height_ratio": focus_ratio,
            "people_max": max(len(s["people"]) for s in samples),
            "detected_samples": len(detected),
            "samples": len(samples),
            "keypoint_quality": round(quality, 3),
            "motion": motion_info,
            "camera_angle_confidence": angle_confidence,
            "camera_angle_cues": angle_cues,
            "foreground_samples": len(largest),
        },
    }
    if over_shoulder:
        shot["over_the_shoulder"] = over_shoulder
    return shot


def main() -> int:
    args = parse_args()
    if not os.path.isfile(args.video):
        print(f"UPT_ERROR video not found: {args.video}", file=sys.stderr)
        return 2

    capture = cv2.VideoCapture(args.video)
    if not capture.isOpened():
        print("UPT_ERROR OpenCV could not open the video (non-ASCII paths may need a temporary copy).", file=sys.stderr)
        return 2
    fps = capture.get(cv2.CAP_PROP_FPS) or 30.0
    frame_count = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
    width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))
    duration = frame_count / fps if frame_count > 0 else 0.0
    print(f"UPT_PROGRESS video {width}x{height} {fps:.2f}fps {duration:.2f}s", flush=True)
    source_width, source_height = width, height
    active_area = detect_active_area(capture, frame_count, width, height)
    if active_area:
        area_x0, area_y0, area_x1, area_y1 = active_area
        capture = CroppedCapture(capture, active_area)
        width, height = area_x1 - area_x0, area_y1 - area_y0
        print(f"UPT_PROGRESS active_area x{area_x0}-{area_x1} y{area_y0}-{area_y1} -> {width}x{height} (static borders removed)", flush=True)

    shots = detect_shots(args.video, fps, duration, args.scene_threshold, args.min_shot_seconds, active_area)
    shots, merged_cuts = merge_false_cuts(capture, shots, fps, frame_count)
    # 영상을 구간으로 잘라 받으면 앞뒤 끝에 옆 컷의 몇 프레임이 짧은 샷으로 붙는다. 최소 길이보다 짧은 첫·마지막 샷은 이웃 샷에 합친다.
    # (샘플은 샷 양끝에서 떨어진 시점에서 뽑으므로 합친 몇 프레임은 분석에 쓰이지 않는다.)
    while len(shots) > 1 and shots[-1][1] - shots[-1][0] < args.min_shot_seconds:
        shots[-2:] = [(shots[-2][0], shots[-1][1])]
        merged_cuts += 1
    while len(shots) > 1 and shots[0][1] - shots[0][0] < args.min_shot_seconds:
        shots[:2] = [(shots[0][0], shots[1][1])]
        merged_cuts += 1
    print(f"UPT_PROGRESS shots {len(shots)} (merged false cuts or edge fragments: {merged_cuts})", flush=True)

    from rtmlib import RTMPose, YOLOX
    from rtmlib.tools.solution.body import Body

    urls = Body.MODE[args.model]
    detector = YOLOX(urls["det"], model_input_size=urls["det_input_size"], score_thr=args.det_thr, backend="onnxruntime", device="cpu")
    pose_model = RTMPose(urls["pose"], model_input_size=urls["pose_input_size"], backend="onnxruntime", device="cpu")

    segmenter = None
    if not args.no_foreground:
        segmenter = ForegroundSegmenter(args.segmentation_model)
        if segmenter.interpreter is None:
            print(f"UPT_PROGRESS foreground_detection off ({segmenter.reason})", flush=True)
            segmenter = None

    shot_samples, all_raw = sample_shots(capture, shots, fps, frame_count, detector, pose_model, args, segmenter)
    capture.release()
    ratios, calibration = calibrate_ratios(all_raw, args.kpt_thr, width, height)
    print(f"UPT_PROGRESS proportion_calibration {calibration or 'default'}", flush=True)

    plan_shots = []
    if args.debug_dir:
        os.makedirs(args.debug_dir, exist_ok=True)
    for shot_index, (start, end, samples, pairs) in enumerate(shot_samples):
        shot = build_shot(shot_index, start, end, samples, pairs, ratios, width, height, args.kpt_thr)
        plan_shots.append(shot)
        if args.debug_dir:
            focus_ratio = FOCUS_HEIGHT_RATIO[shot["shot_size"]]
            text = f"H={shot['diagnostics'].get('full_body_screen_height', 0):.2f} {shot['camera_motion']} conf={shot['confidence']:.2f}"
            if shot.get("over_the_shoulder"):
                text += f" OTS-{shot['over_the_shoulder']['side']}"
            for sample_index, s in enumerate(samples):
                frame = cv2.imdecode(s["frame_jpg"], cv2.IMREAD_COLOR)
                image = draw_debug(frame, s.get("people", []), f"{shot['name']} {shot['shot_size']}", focus_ratio, text)
                for f in s.get("foreground", []):
                    cv2.rectangle(image, (int(f["x0"] * width), int(f["y0"] * height)), (int(f["x1"] * width) - 1, int(f["y1"] * height) - 1), (255, 0, 255), 3)
                write_image(os.path.join(args.debug_dir, f"{shot['name']}_s{sample_index + 1}.jpg"), image)

    identity_notes = assign_identities(plan_shots)
    print(f"UPT_PROGRESS identities {identity_notes['count']}", flush=True)

    title = "".join(ch if ch.isalnum() or ch in "_-" else "_" for ch in os.path.splitext(os.path.basename(args.video))[0]) or "Reference"
    plan = {
        "schema_version": 1,
        "title": title,
        "match_mode": args.mode,
        "vision_model": f"upt-local-pose/{args.model}",
        "analysis_fingerprint": "",
        "aspect_ratio": round(width / height, 4) if height else 1.7778,
        "analysis_notes": {"analyzer": ANALYZER_VERSION, "proportion_calibration": calibration, "identities": identity_notes,
                           "foreground_detection": "deeplab_v3" if segmenter else "off",
                           # 원본 영상에서 분석에 쓴 실제 화면 영역(0~1). 화면 좌표·aspect_ratio는 이 영역 기준이다.
                           "active_area": None if not active_area else {
                               "x0": round(active_area[0] / source_width, 4), "y0": round(active_area[1] / source_height, 4),
                               "x1": round(active_area[2] / source_width, 4), "y1": round(active_area[3] / source_height, 4)}},
        "shots": plan_shots,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as handle:
        json.dump(plan, handle, ensure_ascii=False, indent=2)
    print(f"UPT_REFERENCE_PLAN={os.path.abspath(args.output)}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
