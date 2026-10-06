"""언리얼 에디터 전용: 리타기팅 품질 측정 한 벌을 한 번에 돌린다.

리타기팅을 손보고 나면 **같은 19쌍**을 다시 재서 전후를 비교해야 한다. 그 전까지는
콘솔 명령 네 줄을 손으로 치고 촬영 스크립트를 따로 돌렸는데, 조합을 하나 빠뜨리거나
manifest를 섞으면 수치가 조용히 달라져 비교가 무의미해진다. 그래서 조합 목록을 코드에 두고
한 번에 돌린다.

조합은 원본 2종(Synty Goblin, UE4 Mannequin) × 대상 3종(Manny, Forest Golem, Mixamo X_Bot)이고,
명명 규칙·본 개수·체형이 모두 달라 일반성을 보기에 쓸 만한 묶음이다.

사용:
  UnrealEditor.exe <프로젝트> -ExecCmds="py \"<이 파일>\"" -unattended

끝나면 Saved/UniversalProductionTools/GroundTruth/retarget_<시각>/retarget_poses.json 이 생기고,
Tools/eval/run_regression.py --only retarget 으로 베이스라인과 비교한다.
"""
import json
import os
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import upt_capture_retarget_poses as capture  # noqa: E402

SYNTY = [
    "/Game/External/Animations_Synty/Characters/Goblin/Locomotion/Run/A_MOD_GBL_Run_F_Neut.A_MOD_GBL_Run_F_Neut",
    "/Game/External/Animations_Synty/Characters/Goblin/Locomotion/Walk/A_MOD_GBL_Walk_F_Neut.A_MOD_GBL_Walk_F_Neut",
    "/Game/External/Animations_Synty/Characters/Goblin/Locomotion/Turn/A_MOD_GBL_Turn_Standing_180L_Neut.A_MOD_GBL_Turn_Standing_180L_Neut",
    "/Game/External/Animations_Synty/Characters/Goblin/InAir/Jump/A_MOD_GBL_Jump_Running_Neut.A_MOD_GBL_Jump_Running_Neut",
    "/Game/External/Animations_Synty/Characters/Goblin/Idles/A_MOD_GBL_Idle_Fidget_Swipe_Neut.A_MOD_GBL_Idle_Fidget_Swipe_Neut",
]
UE4_SRC = [
    "/Game/External/CombatMasterAnimBundle/Animations/DynamicKatanaV2/Mannequin_UE4/RM/Locomotions/Anim_DK2_Walk_F_RM.Anim_DK2_Walk_F_RM",
    "/Game/External/CombatMasterAnimBundle/Animations/DynamicKatanaV2/Mannequin_UE4/RM/Locomotions/Anim_DK2_Run_F_RM.Anim_DK2_Run_F_RM",
    "/Game/External/CombatMasterAnimBundle/Animations/DynamicKatanaV2/Mannequin_UE4/RM/Locomotions/Anim_DK2_Idle_RM.Anim_DK2_Idle_RM",
    "/Game/External/CombatMasterAnimBundle/Animations/DynamicKatanaV2/Mannequin_UE4/RM/Attack/Anim_DK2_Combo_A_RM.Anim_DK2_Combo_A_RM",
]

# (manifest 이름, 대상 메시, 애니메이션 목록)
COMBINATIONS = [
    ("retarget_manifest.json",
     "/Game/Retrieve/Character/Heroes/Mannequin/Meshes/SKM_Manny.SKM_Manny", SYNTY),
    ("retarget_manifest_golem.json",
     "/Game/External/ForestGolems/Meshes/Forest_Golem_1_PolyArt.Forest_Golem_1_PolyArt", SYNTY),
    ("retarget_manifest_xbot.json",
     "/Game/Retrieve/Retargeter/Mixamo/Character/X_Bot.X_Bot", SYNTY),
    ("retarget_manifest_ue4src.json",
     "/Game/External/ForestGolems/Meshes/Forest_Golem_1_PolyArt.Forest_Golem_1_PolyArt", UE4_SRC),
]


def run_console(command: str) -> None:
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    unreal.SystemLibrary.execute_console_command(world, command)


def wait_for_asset_registry() -> None:
    """에셋 레지스트리 스캔이 끝날 때까지 기다린다.

    리타기팅은 애니메이션의 Skeleton으로 **원본 메시를 역추적**한다. 그 조회가 레지스트리를
    쓰는데, -ExecCmds는 스캔이 끝나기 전에 실행된다. 그 상태로 돌리면 네 조합 모두
    "원본 메시를 찾지 못했습니다"로 빠져 manifest가 0쌍으로 저장된다(실제로 겪었다).
    실패가 아니라 **빈 성공**으로 보이기 때문에 알아채기 어렵다.
    """
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    registry.search_all_assets(True)
    registry.wait_for_completion()


def main() -> None:
    wait_for_asset_registry()
    directory = os.path.join(capture.saved_dir(), "UniversalProductionTools")
    os.makedirs(directory, exist_ok=True)

    manifests = []
    for name, target, animations in COMBINATIONS:
        out = os.path.join(directory, name)
        if os.path.isfile(out):
            os.remove(out)  # 지난 결과가 남아 있으면 실패를 성공으로 오해한다
        run_console(f"UPT.RetargetE2E {target} {' '.join(animations)} -out={out}")
        if not os.path.isfile(out):
            unreal.log_error(f"UPT_SUITE manifest를 만들지 못했습니다: {name}")
            continue
        with open(out, encoding="utf-8") as handle:
            count = len(json.load(handle).get("pairs", []))
        # 0쌍 manifest는 조용히 넘어가면 "쌍이 줄어든 채로 통과"하는 측정이 된다.
        if count != len(animations):
            unreal.log_error(f"UPT_SUITE {name}: {count}/{len(animations)}쌍만 만들어졌습니다.")
        if count:
            manifests.append(out)

    unreal.log(f"UPT_SUITE manifest {len(manifests)}/{len(COMBINATIONS)}개")
    if manifests:
        capture.main(manifests)


if __name__ == "__main__":
    main()
