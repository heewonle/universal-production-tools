"""영상에서 뽑은 관절이 실제 캐릭터 자세와 얼마나 맞는지 잰다.

정답: 에디터에서 만든 bodypose_* 데이터(프레임별 실제 뼈 위치 + 화면 투영 좌표).
추정: rtmlib의 3D 포즈 모델(RTMW3D)로 같은 영상을 분석한 관절.

무엇을 재는가
- 화면 위치(screen_error): 관절 화면 좌표의 차이. 프레임 대비와 몸 높이 대비 둘 다 낸다.
- 깊이(depth_correlation): 추정한 z가 카메라 기준 실제 깊이와 같이 움직이는지. 0에 가까우면 깊이는 잡음이다.
- 자세 구조(pose_error_cm): 회전·크기를 맞춘 뒤 남는 관절 거리(cm).
  정답은 3D인데 추정의 깊이는 단위가 달라, 이 값만 보면 오해한다. 정답의 화면 좌표를 그대로 넣었을 때
  남는 값(pose_error_floor_cm)과 나란히 봐야 한다. 둘이 비슷하면 깊이가 전혀 기여하지 않은 것이다.
좌우는 영상 좌우와 캐릭터 좌우가 뒤집힐 수 있어 두 대응을 모두 재고 좋은 쪽을 쓴다.
"""
from __future__ import annotations

import json
import math
import os

import cv2
import numpy as np

from common import latest_dataset, use_utf8_output

# COCO 17 관절 번호 → 정답 뼈 이름
JOINT_TO_BONE = {
    5: "upperarm_l", 6: "upperarm_r", 7: "lowerarm_l", 8: "lowerarm_r", 9: "hand_l", 10: "hand_r",
    11: "thigh_l", 12: "thigh_r", 13: "calf_l", 14: "calf_r", 15: "foot_l", 16: "foot_r",
}
FLIPPED = {5: "upperarm_r", 6: "upperarm_l", 7: "lowerarm_r", 8: "lowerarm_l", 9: "hand_r", 10: "hand_l",
           11: "thigh_r", 12: "thigh_l", 13: "calf_r", 14: "calf_l", 15: "foot_r", 16: "foot_l"}
# 정답이 정지 자세로 굳었는지 보는 기준(cm). 이보다 안 움직이면 촬영이 잘못된 것이라 평가하지 않는다.
MIN_TRUTH_MOTION_CM = 5.0


def load_truth(data_dir: str) -> dict | None:
    path = os.path.join(data_dir, "body_truth.json")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def truth_motion_cm(truth: dict) -> float:
    """정답에서 손·발이 실제로 움직인 범위(cm). 0에 가까우면 촬영이 정지 자세로 굳은 것이다."""
    moved = 0.0
    for bone in ("hand_l", "hand_r", "foot_l", "foot_r"):
        values = np.array([frame["bones"][bone]["loc"] for frame in truth["frames"] if bone in frame["bones"]])
        if len(values) > 1:
            moved = max(moved, float((values.max(axis=0) - values.min(axis=0)).max()))
    return moved


def procrustes_error(truth_points: np.ndarray, predicted_points: np.ndarray) -> float:
    """회전·크기·위치를 맞춘 뒤 남는 관절 거리 오차(cm). 축·단위 규약이 달라도 자세 구조만 비교한다."""
    truth_centered = truth_points - truth_points.mean(axis=0)
    predicted_centered = predicted_points - predicted_points.mean(axis=0)
    predicted_norm = np.linalg.norm(predicted_centered)
    if predicted_norm < 1e-9:
        return float("inf")
    covariance = predicted_centered.T @ truth_centered
    u_matrix, singular, v_matrix = np.linalg.svd(covariance)
    rotation = u_matrix @ v_matrix
    if np.linalg.det(rotation) < 0:  # 거울상 방지
        v_matrix[-1] *= -1
        singular[-1] *= -1
        rotation = u_matrix @ v_matrix
    scale = singular.sum() / (predicted_norm ** 2)
    aligned = scale * predicted_centered @ rotation
    return float(np.sqrt(((truth_centered - aligned) ** 2).sum(axis=1)).mean())


def camera_forward(truth: dict) -> tuple:
    """정답 카메라의 위치와 정면 벡터. 깊이 비교에 쓴다."""
    location = np.array(truth["camera"]["location"], dtype=np.float64)
    _roll, pitch, yaw = truth["camera"]["rotation"]
    pitch, yaw = math.radians(pitch), math.radians(yaw)
    forward = np.array([math.cos(pitch) * math.cos(yaw), math.cos(pitch) * math.sin(yaw), math.sin(pitch)])
    return location, forward


def body_height(bones: dict) -> float:
    """화면에서 차지하는 몸 높이(머리~발). 화면 오차를 사람 크기 대비로 환산할 때 쓴다."""
    head = bones.get("head", {}).get("screen")
    feet = [bones[name]["screen"] for name in ("foot_l", "foot_r") if bones.get(name, {}).get("screen")]
    if not head or not feet:
        return 0.0
    return max(abs(head[1] - foot[1]) for foot in feet)


def pose_error(bones: dict, prediction: dict, use_truth_screen: bool, width: int, height: int) -> float | None:
    """자세 구조 오차. use_truth_screen이면 정답의 화면 좌표를 넣어 이 지표의 바닥값을 낸다."""
    best = None
    for mapping in (JOINT_TO_BONE, FLIPPED):
        truth_points, predicted_points = [], []
        for joint, bone in mapping.items():
            if bone not in bones or joint >= len(prediction["keypoints"]):
                continue
            if use_truth_screen:
                screen = bones[bone].get("screen")
                if not screen:
                    continue
                point = [screen[0] * width, screen[1] * height, 0.0]
            else:
                point = prediction["keypoints"][joint]
            truth_points.append(bones[bone]["loc"])
            predicted_points.append(point)
        if len(truth_points) < 8:
            continue
        error = procrustes_error(np.array(truth_points, dtype=np.float64), np.array(predicted_points, dtype=np.float64))
        best = error if best is None else min(best, error)
    return best


def compare(truth: dict, predictions: list[dict]) -> dict:
    width, height = truth["camera"]["width"], truth["camera"]["height"]
    location, forward = camera_forward(truth)
    screen_errors, body_errors, pose_errors, floor_errors, missing = [], [], [], [], 0
    true_depths, predicted_depths = [], []

    for frame, prediction in zip(truth["frames"], predictions):
        if prediction is None:
            missing += 1
            continue
        bones = frame["bones"]

        # 화면 오차: 좌우 대응 두 가지 중 가까운 쪽
        gaps = []
        for mapping in (JOINT_TO_BONE, FLIPPED):
            pairs = []
            for joint, bone in mapping.items():
                screen = bones.get(bone, {}).get("screen")
                if screen and joint < len(prediction["screen"]) and prediction["screen"][joint] is not None:
                    pairs.append(np.linalg.norm(np.array(screen) - np.array(prediction["screen"][joint])))
            if pairs:
                gaps.append(float(np.mean(pairs)))
        if gaps:
            screen_errors.append(min(gaps))
            span = body_height(bones)
            if span > 1e-6:
                body_errors.append(min(gaps) / span)

        # 자세 구조 오차와 그 바닥값
        error = pose_error(bones, prediction, False, width, height)
        floor = pose_error(bones, prediction, True, width, height)
        if error is not None:
            pose_errors.append(error)
        if floor is not None:
            floor_errors.append(floor)

        # 깊이: 골반(양 허벅지 중간)을 기준으로 한 상대 깊이끼리 비교
        hips = [bones[name]["loc"] for name in ("thigh_l", "thigh_r") if name in bones]
        if not hips:
            continue
        root_depth = float((np.mean(hips, axis=0) - location) @ forward)
        for joint, bone in JOINT_TO_BONE.items():
            if bone not in bones or joint >= len(prediction["keypoints"]):
                continue
            true_depths.append(float((np.array(bones[bone]["loc"]) - location) @ forward) - root_depth)
            predicted_depths.append(float(prediction["keypoints"][joint][2]))

    correlation = None
    if len(true_depths) > 2 and float(np.std(predicted_depths)) > 1e-9:
        correlation = float(np.corrcoef(true_depths, predicted_depths)[0, 1])

    return {
        "frames": len(truth["frames"]),
        "frames_without_person": missing,
        "screen_error": round(float(np.median(screen_errors)), 4) if screen_errors else None,
        "screen_error_body_pct": round(100 * float(np.median(body_errors)), 1) if body_errors else None,
        "depth_correlation": round(correlation, 3) if correlation is not None else None,
        "pose_error_cm": round(float(np.median(pose_errors)), 1) if pose_errors else None,
        "pose_error_floor_cm": round(float(np.median(floor_errors)), 1) if floor_errors else None,
    }


def predict(video: str, truth: dict) -> list[dict]:
    from rtmlib import Wholebody3d

    model = Wholebody3d(mode="balanced", backend="onnxruntime", device="cpu")
    capture = cv2.VideoCapture(video)
    width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH)) or truth["camera"]["width"]
    height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT)) or truth["camera"]["height"]
    predictions = []
    while True:
        ok, frame = capture.read()
        if not ok:
            break
        # Wholebody3d는 (3D 관절, 점수, simcc, 2D 관절)을 돌려준다.
        # 3D 관절의 x·y는 모델 입력 크기 기준이고 z는 별도 단위의 상대 깊이라, 화면 좌표는 4번째(2D)를 쓴다.
        keypoints, _scores, _simcc, keypoints_2d = model(frame)
        if keypoints is None or len(keypoints) == 0:
            predictions.append(None)
            continue
        person = np.asarray(keypoints[0], dtype=np.float64)
        person_2d = np.asarray(keypoints_2d[0], dtype=np.float64) if keypoints_2d is not None and len(keypoints_2d) else None
        screen = ([[float(point[0]) / width, float(point[1]) / height] for point in person_2d] if person_2d is not None
                  else [None] * len(person))
        predictions.append({"keypoints": person[:, :3].tolist(), "screen": screen})
    capture.release()
    return predictions


def run(output_dir: str) -> dict:
    data_dir = latest_dataset("bodypose")
    if not data_dir:
        return {"skipped": "정답 데이터 없음(bodypose)"}
    truth = load_truth(data_dir)
    video = os.path.join(data_dir, "body_truth.mp4")
    if not truth or not os.path.isfile(video):
        return {"skipped": f"정답 영상·JSON 없음({os.path.basename(data_dir)})"}
    moved = truth_motion_cm(truth)
    if moved < MIN_TRUTH_MOTION_CM:
        return {"skipped": f"정답이 정지 자세({moved:.1f}cm만 이동) — 촬영을 다시 해야 합니다"}
    os.makedirs(output_dir, exist_ok=True)
    predictions = predict(video, truth)
    metrics = compare(truth, predictions)
    metrics["dataset"] = os.path.basename(data_dir)
    with open(os.path.join(output_dir, "body_pose_predictions.json"), "w", encoding="utf-8") as handle:
        json.dump({"metrics": metrics, "predictions": predictions}, handle)
    return metrics


if __name__ == "__main__":
    use_utf8_output()
    print(json.dumps(run(os.path.join(os.getcwd(), "eval_output")), ensure_ascii=False, indent=2))
