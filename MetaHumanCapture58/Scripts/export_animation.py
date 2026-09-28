"""처리된 Performance에서 Anim Sequence를 뽑는다(5.7로 가져갈 애니메이션).

프로젝트에 MetaHuman 캐릭터가 하나 있어야 한다(얼굴 스켈레톤 Face_Archetype_Skeleton 필요).
Fab/Bridge로 MetaHuman을 가져오면 보통 /Game/MetaHumans/Common/Face/Face_Archetype_Skeleton 에 생긴다.

  UPT_CAPTURE=CD_이름 UnrealEditor.exe <uproject> -ExecCmds="py <이 파일>" -nosplash
"""
import os

import unreal

PERFORMANCE = "/Game/Capture/" + os.environ.get("UPT_CAPTURE", "CD_ShortsCloseup") + "_Performance"
STORAGE = os.environ.get("UPT_EXPORT_PATH", "/Game/Capture/Exported")
# 얼굴 스켈레톤 후보(프로젝트에 MetaHuman을 넣은 위치에 따라 다를 수 있다).
SKELETON_CANDIDATES = [
    # Fab/Bridge로 MetaHuman을 가져온 경우
    "/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton.Face_Archetype_Skeleton",
    "/Game/MetaHumans/Common/Common/Face_Archetype_Skeleton.Face_Archetype_Skeleton",
    # 가져오지 않아도 엔진 플러그인 콘텐츠에 같은 스켈레톤이 있다.
    "/MetaHuman/IdentityTemplate/Face_Archetype_Skeleton.Face_Archetype_Skeleton",
    "/MetaHumanCharacter/Face/Face_Archetype_Skeleton.Face_Archetype_Skeleton",
]


# 스켈레톤만 주는 것보다 얼굴 스켈레탈 메시를 주는 편이 안전하다(스켈레톤+미리보기 메시가 함께 잡힌다).
MESH_CANDIDATES = [
    "/MetaHumanCharacter/Face/SKM_Face.SKM_Face",
    "/Game/MetaHumans/Common/Face/SKM_Face.SKM_Face",
]


def find_face_target():
    for path in MESH_CANDIDATES:
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            return unreal.load_asset(path)
    return find_face_skeleton()


def find_face_skeleton():
    for path in SKELETON_CANDIDATES:
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            return unreal.load_asset(path)
    # 경로가 다르면 에셋 레지스트리에서 이름으로 찾는다.
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    for asset in registry.get_assets_by_class(unreal.TopLevelAssetPath("/Script/Engine", "Skeleton")):
        if "Face_Archetype_Skeleton" in str(asset.asset_name):
            return unreal.load_asset(str(asset.get_soft_object_path()))
    return None


def main() -> None:
    performance = unreal.load_asset(PERFORMANCE)
    if not performance:
        unreal.log_error(f"UPT_EXPORT Performance 에셋 없음: {PERFORMANCE}")
        return
    if not performance.contains_animation_data_type(unreal.FrameAnimationDataType.FACE):
        unreal.log_error("UPT_EXPORT 처리된 얼굴 애니메이션이 없습니다. run_mono_performance.py를 먼저 실행하세요.")
        return
    skeleton = find_face_target()
    if not skeleton:
        unreal.log_error("UPT_EXPORT 얼굴 스켈레톤/메시를 찾지 못했습니다. Fab/Bridge로 MetaHuman을 먼저 가져오세요.")
        return
    unreal.log(f"UPT_EXPORT target={skeleton.get_path_name()} class={skeleton.get_class().get_name()}")
    # 앞선 실패로 남은 빈 에셋이 있으면 재사용돼 오류가 이어지므로 지우고 새로 만든다.
    existing = f"{STORAGE.rstrip('/')}/AS_{performance.get_name()}"
    if unreal.EditorAssetLibrary.does_asset_exist(existing):
        try:
            unreal.EditorAssetLibrary.delete_asset(existing)
        except Exception as error:
            # 앞선 실패로 스켈레톤이 없는 에셋은 불러오는 것만으로도 오류가 난다. 지우지 못해도 계속 진행한다.
            unreal.log_warning(f"UPT_EXPORT 기존 에셋을 지우지 못했습니다(무시): {str(error)[:120]}")
    # 엔진 예제(export_performance.run_anim_sequence_export)는 스켈레톤 경로를 /Game/MetaHumans/...로 하드코딩해
    # MetaHuman을 가져오지 않은 프로젝트에서는 None이 들어가 "invalid target Skeleton"으로 빈 에셋이 나온다.
    settings = unreal.MetaHumanPerformanceExportAnimationSettings()
    settings.show_export_dialog = False
    settings.package_path = STORAGE
    settings.asset_name = f"AS_{performance.get_name()}"
    settings.target_skeleton_or_skeletal_mesh = skeleton
    settings.enable_head_movement = True
    settings.export_range = unreal.PerformanceExportRange.PROCESSING_RANGE
    anim = unreal.MetaHumanPerformanceExportUtils.export_animation_sequence(performance, settings)
    if anim:
        unreal.EditorAssetLibrary.save_loaded_asset(anim)
        unreal.log(f"UPT_EXPORT created {anim.get_path_name()}")
    else:
        unreal.log_error("UPT_EXPORT 내보내기에 실패했습니다.")


if __name__ == "__main__":
    main()
