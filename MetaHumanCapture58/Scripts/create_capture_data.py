"""이미지 시퀀스 폴더로 MetaHuman용 Capture Data(Footage) 에셋을 만든다.

쓰임: 일반 영상을 ffmpeg로 프레임 이미지로 푼 뒤, Capture Manager(아이폰·HMC 전용)를 거치지 않고
      깊이 정보 없는 mono 푸티지로 MetaHuman Identity/Performance에 넣기 위한 준비 단계.

에디터에서:  py "<프로젝트>/Scripts/create_capture_data.py"
명령줄에서:  UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript="<이 파일>" -unattended -nullrhi
"""
import os

import unreal

PROJECT_DIR = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
# 환경 변수 UPT_FOOTAGE로 폴더 이름을 넘길 수 있다(없으면 아래 목록을 쓴다).
_NAMES = [name.strip() for name in os.environ.get("UPT_FOOTAGE", "").split(",") if name.strip()] or ["ShortsCloseup"]
FOOTAGE = [
    # (이미지 시퀀스 폴더, 에셋 이름, 프레임레이트)
    (os.path.join(PROJECT_DIR, "Footage", name), name, unreal.FrameRate(24000, 1001)) for name in _NAMES
]
PACKAGE_PATH = "/Game/Capture"
FRAME_RATE_HZ = 24000.0 / 1001.0  # 23.976


def get_or_create(tools, name: str, asset_class, factory):
    """이미 있으면 그 에셋을 쓴다(다른 에셋이 참조 중이면 지울 수 없어 create_asset이 None을 돌려준다)."""
    asset_path = f"{PACKAGE_PATH}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(asset_path):
        return unreal.load_asset(asset_path)
    return tools.create_asset(name, PACKAGE_PATH, asset_class, factory)


def make_image_sequence(tools, folder: str, name: str, frame_rate: unreal.FrameRate) -> unreal.ImgMediaSource:
    source = get_or_create(tools, f"IMG_{name}", unreal.ImgMediaSource, unreal.ImgMediaSourceFactoryNew())
    source.set_sequence_path(folder)
    source.set_editor_property("frame_rate_override", frame_rate)
    unreal.EditorAssetLibrary.save_loaded_asset(source)
    return source


def make_capture_data(tools, name: str, source: unreal.ImgMediaSource) -> unreal.FootageCaptureData:
    capture = get_or_create(tools, f"CD_{name}", unreal.FootageCaptureData, unreal.FootageCaptureDataFactory())
    capture.set_editor_property("image_sequences", [source])
    # 프레임레이트가 0이면 Performance가 이 캡처 데이터를 거부한다("frame rate is zero").
    metadata = capture.get_editor_property("metadata")
    metadata.set_editor_property("frame_rate", FRAME_RATE_HZ)
    metadata.set_editor_property("device_class", unreal.FootageDeviceClass.UNSPECIFIED)
    metadata.set_editor_property("device_model_name", "GenericMonoVideo")
    capture.set_editor_property("metadata", metadata)
    unreal.EditorAssetLibrary.save_loaded_asset(capture)
    return capture


def main() -> None:
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    for folder, name, frame_rate in FOOTAGE:
        frames = sorted(f for f in os.listdir(folder)) if os.path.isdir(folder) else []
        if not frames:
            unreal.log_error(f"UPT_CAPTURE 프레임 폴더가 비었습니다: {folder}")
            continue
        source = make_image_sequence(tools, folder, name, frame_rate)
        capture = make_capture_data(tools, name, source)
        unreal.log(f"UPT_CAPTURE created {capture.get_path_name()} frames={len(frames)} path={source.get_sequence_path()}")


if __name__ == "__main__":
    main()
