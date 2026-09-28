"""일반 영상(모노) Capture Data로 MetaHuman Performance를 만들고 처리한 뒤 저장한다.

엔진 기본 예제(process_monocular_performance.py)는 에셋을 저장하지 않아 헤드리스 실행이 끝나면 결과가 사라진다.
여기서는 생성 → 처리 → 저장까지 한다.

  UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript="<이 파일>" -unattended -nosplash
  (처리에는 DX12 GPU가 필요하므로 -nullrhi를 쓰지 않는다)
"""
import os

import unreal

from process_monocular_performance import create_performance_asset
from process_performance import process_shot

# 환경 변수로 대상을 바꿀 수 있다: UPT_CAPTURE=CD_이름, UPT_BODY=1이면 몸 추적도 켠다.
CAPTURE_DATA = "/Game/Capture/" + os.environ.get("UPT_CAPTURE", "CD_ShortsCloseup")
STORAGE = "/Game/Capture/"
BODY_TRACKING = os.environ.get("UPT_BODY", "0") not in ("", "0", "false", "False")


def count_frames(capture) -> int:
    """캡처 데이터가 가리키는 이미지 시퀀스 폴더의 프레임 수."""
    sequences = capture.get_editor_property("image_sequences") or []
    if not sequences:
        return 0
    path = sequences[0].get_sequence_path()
    if not os.path.isabs(path):
        path = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()), path.lstrip("./"))
    return len([name for name in os.listdir(path)]) if os.path.isdir(path) else 0


def main() -> None:
    performance_path = f"{STORAGE.rstrip('/')}/{unreal.load_asset(CAPTURE_DATA).get_name()}_Performance"
    if unreal.EditorAssetLibrary.does_asset_exist(performance_path):
        # 이미 만들어 둔 Performance는 다시 만들 수 없다(unattended에서 create_asset이 None을 돌려준다).
        performance = unreal.load_asset(performance_path)
        performance.set_editor_property("input_type", unreal.DataInputType.MONO_FOOTAGE)
        performance.set_editor_property("footage_capture_data", unreal.load_asset(CAPTURE_DATA))
    else:
        performance = create_performance_asset(path_to_capture_data=CAPTURE_DATA, save_performance_location=STORAGE)
    if not performance:
        unreal.log_error("UPT_PERF Performance 에셋을 만들지 못했습니다.")
        return
    capture = performance.get_editor_property("footage_capture_data")
    if not capture:
        unreal.log_error("UPT_PERF 캡처 데이터가 연결되지 않았습니다(프레임레이트 0이면 거부됩니다).")
        return
    # 처리 범위가 0이면 파이프라인이 곧바로 끝나고 결과가 비어 있다. 프레임 수로 범위를 직접 지정한다.
    frame_count = count_frames(capture)
    before = (performance.get_editor_property("start_frame_to_process"), performance.get_editor_property("end_frame_to_process"))
    unreal.log(f"UPT_PERF frames={frame_count} range_before={before} face={performance.get_editor_property('face_tracking')}")
    if frame_count:
        performance.set_editor_property("start_frame_to_process", 0)
        performance.set_editor_property("end_frame_to_process", frame_count)
    after = (performance.get_editor_property("start_frame_to_process"), performance.get_editor_property("end_frame_to_process"))
    if BODY_TRACKING:
        try:
            performance.set_editor_property("body_tracking", True)
        except Exception as error:
            unreal.log_warning(f"UPT_PERF 몸 추적을 켜지 못했습니다: {error}")
    unreal.log(f"UPT_PERF body={performance.get_editor_property('body_tracking')} range_after={after} can_process={performance.can_process()}")
    unreal.log(f"UPT_PERF processing {performance.get_name()} …")
    process_shot(performance_asset=performance)
    # 모노 경로는 blocking 설정과 관계없이 비동기로 돈다(엔진 코드상 blocking은 깊이 푸티지 분기 전용).
    # 에디터 틱마다 확인해 처리가 끝나면 저장하고 에디터를 닫는다.
    wait_for_processing(performance)


def report(performance) -> None:
    contains = {}
    for name in sorted(n for n in dir(unreal.FrameAnimationDataType) if n.isupper()):
        try:
            contains[name] = performance.contains_animation_data_type(getattr(unreal.FrameAnimationDataType, name))
        except Exception as error:
            contains[name] = f"확인 실패: {error}"
    saved = unreal.EditorAssetLibrary.save_loaded_asset(performance)
    unreal.log(f"UPT_PERF saved={saved} path={performance.get_path_name()}")
    unreal.log(f"UPT_PERF animation_data={contains}")


def wait_for_processing(performance, timeout_seconds: float = 1800.0) -> None:
    state = {"handle": None, "elapsed": 0.0, "logged": 0.0}

    def on_tick(delta_seconds: float) -> None:
        state["elapsed"] += delta_seconds
        if performance.is_processing() and state["elapsed"] < timeout_seconds:
            if state["elapsed"] - state["logged"] >= 15.0:
                state["logged"] = state["elapsed"]
                unreal.log(f"UPT_PERF still processing… {state['elapsed']:.0f}s")
            return
        unreal.unregister_slate_post_tick_callback(state["handle"])
        if state["elapsed"] >= timeout_seconds:
            unreal.log_error(f"UPT_PERF 시간 초과({timeout_seconds:.0f}s)")
        report(performance)
        unreal.SystemLibrary.quit_editor()

    state["handle"] = unreal.register_slate_post_tick_callback(on_tick)
    unreal.log("UPT_PERF waiting for async pipeline…")


if __name__ == "__main__":
    main()
