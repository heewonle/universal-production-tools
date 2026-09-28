"""Performance의 프레임별 얼굴 컨트롤 값을 JSON으로 뽑는다(5.7에서 애니메이션을 직접 만들기 위해).

  UPT_CAPTURE=CD_이름 UnrealEditor-Cmd.exe <uproject> -ExecutePythonScript="<이 파일>" -unattended -nullrhi
결과: <프로젝트>/Exported/<이름>_curves.json
"""
import json
import os

import unreal

NAME = os.environ.get("UPT_CAPTURE", "CD_ShortsCloseup")
PERFORMANCE = f"/Game/Capture/{NAME}_Performance"
OUT_DIR = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()), "Exported")


def main() -> None:
    performance = unreal.load_asset(PERFORMANCE)
    if not performance:
        unreal.log_error(f"UPT_DUMP Performance 없음: {PERFORMANCE}")
        return
    frames = performance.get_animation_data()
    if not frames:
        unreal.log_error("UPT_DUMP 애니메이션 데이터가 없습니다.")
        return
    capture = performance.get_editor_property("footage_capture_data")
    frame_rate = float(capture.get_editor_property("metadata").get_editor_property("frame_rate")) if capture else 24.0

    rows, names = [], set()
    for frame in frames:
        curves = frame.get_editor_property("animation_data")
        values = {}
        for key, value in dict(curves or {}).items():
            try:
                values[str(key)] = round(float(value), 5)
            except (TypeError, ValueError):
                continue
        names.update(values)
        rows.append(values)

    os.makedirs(OUT_DIR, exist_ok=True)
    target = os.path.join(OUT_DIR, f"{NAME}_curves.json")
    with open(target, "w", encoding="utf-8") as handle:
        json.dump({"performance": PERFORMANCE, "frame_rate": frame_rate, "frames": len(rows),
                   "curve_names": sorted(names), "values": rows}, handle)
    unreal.log(f"UPT_DUMP wrote {target} frames={len(rows)} curves={len(names)} fps={frame_rate:.3f}")


if __name__ == "__main__":
    main()
