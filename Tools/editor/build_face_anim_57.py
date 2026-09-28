"""5.8에서 뽑은 얼굴 컨트롤 값(JSON)으로 5.7에서 Anim Sequence를 직접 만든다.

FBX/Interchange 임포트가 애니메이션을 만들어 주지 않아(프레임레이트 스냅 경고 후에도 에셋 미생성),
커브 값만 옮겨 5.7에서 새로 굽는다. MetaHuman 얼굴 애니메이션은 사실상 컨트롤 커브 묶음이다.
"""
import json
import os

import unreal

JSON_DIR = r"<CAPTURE58>/Exported"
DESTINATION = "/Game/Animation/UPT/FaceCapture"
SKELETON = "/Game/Animation/UPT/FaceCapture/AS_CD_SeatedPerson_Performance_Skeleton"
MODEL_FPS = 30  # 5.7 애니메이션 데이터 모델의 기본 프레임레이트(바꿀 수 없어 길이 계산에만 쓴다)


def make_anim(name: str, skeleton) -> unreal.AnimSequence:
    path = f"{DESTINATION}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    factory = unreal.AnimSequenceFactory()
    factory.set_editor_property("target_skeleton", skeleton)
    return unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, DESTINATION, unreal.AnimSequence, factory)


def get_controller(anim):
    for attribute in ("get_controller", "get_data_controller"):
        getter = getattr(anim, attribute, None)
        if callable(getter):
            try:
                return getter()
            except Exception:
                continue
    for prop in ("controller", "data_controller"):
        try:
            value = anim.get_editor_property(prop)
            if value:
                return value
        except Exception:
            continue
    return None


def build(json_path: str, skeleton) -> None:
    with open(json_path, encoding="utf-8") as handle:
        data = json.load(handle)
    frames, fps = data["frames"], float(data["frame_rate"])
    values, curve_names = data["values"], data["curve_names"]
    duration = frames / fps
    name = "A_" + os.path.basename(json_path).replace("_curves.json", "")
    anim = make_anim(name, skeleton)
    if not anim:
        unreal.log_error(f"UPT_BUILD 에셋 생성 실패: {name}")
        return
    unreal.log(f"UPT_BUILD {name}: frames={frames} fps={fps:.3f} curves={len(curve_names)}")

    controller = get_controller(anim)
    unreal.log(f"UPT_BUILD controller={type(controller).__name__ if controller else None}")
    if controller:
        try:
            controller.open_bracket("UPT face curves")
        except Exception:
            pass
        # 5.7의 데이터 모델은 30fps 고정이다(23.976·24 모두 "30fps의 배수·약수가 아님"으로 거부).
        # 그래서 프레임 수는 30fps 기준으로 잡고, 키는 아래에서 원본 시간(초)에 찍어 타이밍을 유지한다.
        # SetNumberOfFrames는 int가 아니라 FFrameNumber를 받는다.
        model_frames = max(1, round(duration * MODEL_FPS))
        try:
            controller.set_number_of_frames(unreal.FrameNumber(model_frames), True)
            unreal.log(f"UPT_BUILD 길이 {duration:.2f}초 → {MODEL_FPS}fps {model_frames}프레임")
        except Exception as error:
            unreal.log_warning(f"UPT_BUILD set_number_of_frames 실패: {error}")

    times = [index / fps for index in range(frames)]  # 원본 촬영 시간 그대로
    added = 0
    for curve in curve_names:
        series = [float(row.get(curve, 0.0)) for row in values]
        if max(series) - min(series) < 1e-6 and abs(max(series)) < 1e-6:
            continue  # 값이 계속 0인 컨트롤은 넣지 않는다
        try:
            if not unreal.AnimationLibrary.does_curve_exist(anim, curve, unreal.RawCurveTrackTypes.RCT_FLOAT):
                unreal.AnimationLibrary.add_curve(anim, curve)
            unreal.AnimationLibrary.add_float_curve_keys(anim, curve, times, series)
            added += 1
        except Exception as error:
            unreal.log_warning(f"UPT_BUILD 커브 {curve} 실패: {str(error)[:100]}")
            break
    if controller:
        try:
            controller.close_bracket(True)
        except Exception:
            pass
    unreal.EditorAssetLibrary.save_loaded_asset(anim)
    unreal.log(f"UPT_BUILD saved {anim.get_path_name()} curves_added={added} length={anim.get_play_length():.2f}s")


def main() -> None:
    skeleton = unreal.load_asset(SKELETON)
    if not skeleton:
        unreal.log_error(f"UPT_BUILD 스켈레톤 없음: {SKELETON}")
        return
    files = [os.path.join(JSON_DIR, n) for n in sorted(os.listdir(JSON_DIR)) if n.endswith("_curves.json")]
    if not files:
        unreal.log_error("UPT_BUILD 커브 JSON이 없습니다.")
        return
    for path in files:
        build(path, skeleton)


if __name__ == "__main__":
    main()
