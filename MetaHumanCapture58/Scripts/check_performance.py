"""처리한 MetaHuman Performance에 애니메이션 데이터가 실제로 들어갔는지 확인한다."""
import unreal

PERFORMANCE = "/Game/Capture/CD_ShortsCloseup_Performance"


def main() -> None:
    performance = unreal.load_asset(PERFORMANCE)
    if not performance:
        unreal.log_error(f"UPT_CHECK 에셋을 찾지 못했습니다: {PERFORMANCE}")
        return
    unreal.log(f"UPT_CHECK input_type={performance.get_editor_property('input_type')}")
    unreal.log(f"UPT_CHECK capture_data={performance.get_editor_property('footage_capture_data')}")
    for name in sorted(n for n in dir(unreal.FrameAnimationDataType) if n.isupper()):
        try:
            value = getattr(unreal.FrameAnimationDataType, name)
            unreal.log(f"UPT_CHECK contains[{name}]={performance.contains_animation_data_type(value)}")
        except Exception as error:
            unreal.log(f"UPT_CHECK contains[{name}] 확인 실패: {error}")
    try:
        unreal.log(f"UPT_CHECK can_export_animation={performance.can_export_animation()}")
    except Exception as error:
        unreal.log(f"UPT_CHECK can_export_animation 확인 실패: {error}")
    for prop in ("start_frame_to_process", "end_frame_to_process", "face_tracking", "body_tracking"):
        try:
            unreal.log(f"UPT_CHECK {prop}={performance.get_editor_property(prop)}")
        except Exception:
            pass


if __name__ == "__main__":
    main()
