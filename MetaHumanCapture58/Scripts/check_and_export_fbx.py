"""뽑아낸 Anim Sequence를 확인하고 FBX로 내보낸다(5.7 팀 프로젝트로 가져가기 위해).

  UPT_CAPTURE=CD_이름 UnrealEditor.exe <uproject> -ExecCmds="py <이 파일>, Quit" -nosplash
결과: <프로젝트>/Exported/<에셋이름>.fbx
"""
import os

import unreal

ANIM = "/Game/Capture/Exported/AS_" + os.environ.get("UPT_CAPTURE", "CD_ShortsCloseup") + "_Performance"
OUT_DIR = os.path.join(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir()), "Exported")


def main() -> None:
    anim = unreal.load_asset(ANIM)
    if not anim:
        unreal.log_error(f"UPT_FBX 애니메이션 에셋 없음: {ANIM}")
        return
    frames = anim.get_editor_property("number_of_sampled_keys")
    length = anim.get_play_length()
    skeleton = anim.get_editor_property("skeleton")
    curve_names = []
    try:
        controller = anim.get_controller()
        curve_names = [str(name) for name in controller.get_curve_names(unreal.RawCurveTrackTypes.RCT_FLOAT)]
    except Exception as error:
        unreal.log_warning(f"UPT_FBX 커브 목록을 읽지 못했습니다: {error}")
    unreal.log(f"UPT_FBX asset={ANIM} frames={frames} length={length:.2f}s curves={len(curve_names)} skeleton={skeleton.get_name() if skeleton else None}")
    if curve_names:
        unreal.log(f"UPT_FBX sample_curves={sorted(curve_names)[:8]}")

    os.makedirs(OUT_DIR, exist_ok=True)
    target = os.path.join(OUT_DIR, anim.get_name() + ".fbx")
    task = unreal.AssetExportTask()
    task.object = anim
    task.filename = target
    task.automated = True
    task.prompt = False
    task.exporter = unreal.AnimSequenceExporterFBX()
    task.options = unreal.FbxExportOption()
    task.options.set_editor_property("ascii", False)
    task.options.set_editor_property("collision", False)
    # 애니메이션만 담으면 5.7에서 "가져올 데이터가 없다"며 실패한다. 얼굴 메시를 함께 넣어야
    # 받는 쪽에서 스켈레톤·메시·애니메이션이 한 번에 만들어진다(팀 프로젝트에 MetaHuman 플러그인을 켜지 않아도 된다).
    for name, value in (("export_preview_mesh", True), ("export_morph_targets", True), ("map_skeletal_motion_to_root", False)):
        try:
            task.options.set_editor_property(name, value)
        except Exception as error:
            unreal.log_warning(f"UPT_FBX 옵션 {name} 설정 실패(무시): {error}")
    if unreal.Exporter.run_asset_export_task(task) and os.path.isfile(target):
        unreal.log(f"UPT_FBX exported {target} ({os.path.getsize(target)} bytes)")
    else:
        unreal.log_error("UPT_FBX FBX 내보내기에 실패했습니다.")


if __name__ == "__main__":
    main()
