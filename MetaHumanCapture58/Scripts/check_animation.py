"""처리된 Performance의 애니메이션 값이 실제로 프레임마다 움직이는지 수치로 확인한다(GUI 없이 품질 가늠).

UPT_CAPTURE=CD_이름 으로 대상 지정(기본 CD_ShortsCloseup).
"""
import os

import unreal

PERFORMANCE = "/Game/Capture/" + os.environ.get("UPT_CAPTURE", "CD_ShortsCloseup") + "_Performance"


def curve_map(frame) -> dict:
    """FFrameAnimationData에서 컨트롤 이름→값 사전을 찾아 돌려준다(구조는 런타임에 확인)."""
    for name in ("animation_data", "pose", "controls", "curves"):
        try:
            value = frame.get_editor_property(name)
        except Exception:
            continue
        if isinstance(value, (dict, unreal.Map)):
            return dict(value)
    return {}


def main() -> None:
    performance = unreal.load_asset(PERFORMANCE)
    if not performance:
        unreal.log_error(f"UPT_ANIM 에셋 없음: {PERFORMANCE}")
        return
    processed = performance.get_number_of_processed_frames()
    frames = performance.get_animation_data()
    unreal.log(f"UPT_ANIM asset={PERFORMANCE} processed_frames={processed} data_frames={len(frames)}")
    if not frames:
        return
    unreal.log(f"UPT_ANIM frame_fields={[f for f in dir(frames[0]) if not f.startswith('_')][:20]}")

    ranges, samples = {}, 0
    for frame in frames:
        curves = curve_map(frame)
        if not curves:
            continue
        samples += 1
        for key, value in curves.items():
            try:
                number = float(value)
            except (TypeError, ValueError):
                continue
            low, high = ranges.get(str(key), (number, number))
            ranges[str(key)] = (min(low, number), max(high, number))
    moving = {key: round(high - low, 4) for key, (low, high) in ranges.items() if high - low > 0.01}
    unreal.log(f"UPT_ANIM curve_frames={samples} curves={len(ranges)} moving_curves={len(moving)}")
    top = sorted(moving.items(), key=lambda item: -item[1])[:12]
    unreal.log(f"UPT_ANIM most_active={top}")


if __name__ == "__main__":
    main()
