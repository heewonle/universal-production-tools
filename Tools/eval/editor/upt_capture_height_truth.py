"""언리얼 에디터 전용: 여러 화각·자세로 인물을 찍고, 전신이 화면에서 차지하는 '정확한' 높이를 남긴다.

포즈 분석기는 몸이 프레임에 잘리면 보이는 부분만으로 전신 키를 외삽한다. 그 오차가 검증 점수의
크기 항목을 흔드는데, 지금까지는 오차를 잴 방법이 없었다. 엔진은 Bounds와 카메라를 알고 있으니
정확한 값을 만들 수 있다. 이 데이터로 추정기를 시험하고 고친다.

에디터 월드는 애니메이션을 평가하지 않으므로 시뮬레이트(PIE)를 켜고 찍는다
(자세한 함정은 upt_capture_body_truth.py 주석 참고).

사용(에디터 Output Log의 Cmd 칸에서):
  py "<프로젝트>/Plugins/UniversalProductionTools/Tools/eval/editor/upt_capture_height_truth.py"
  py "...upt_capture_height_truth.py" AutoHero

결과: Saved/UniversalProductionTools/GroundTruth/height_<시각>/
  frames/frame_0000.png ...
  height_truth.json   (프레임별 정확한 전신 화면 높이 + 잘림 여부 + 카메라)
"""
import json
import math
import os
import sys
import time

import unreal

V = unreal.Vector
WIDTH, HEIGHT = 1280, 720
FOCAL_LENGTH_MM = 35.0
SENSOR_WIDTH_MM = 36.0
# 목표 전신 화면 높이(화면 대비). 1보다 크면 몸이 프레임 밖으로 잘린다.
TARGET_HEIGHTS = [0.35, 0.55, 0.8, 1.2, 1.8, 2.6, 3.6]
# 각 화각마다 이만큼의 자세를 담는다(애니메이션 시간을 나눠 쓴다).
POSES_PER_FRAMING = 6
# 카메라를 인물 정면에서 좌우로 조금씩 돌려 자세·기울기가 다양해지게 한다.
YAW_OFFSETS_DEG = [0.0, 25.0, -25.0]
MAX_WAIT_TICKS = 600
# 카메라 위치의 키 라이트 밝기(루멘, 400cm 기준). 너무 밝으면 흰 재질이 날아가 관절 검출이 실패한다.
LIGHT_LUMENS = 25000.0


def find_editor_actor(label: str):
    for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        if actor.get_actor_label() == label:
            return actor
    raise RuntimeError(f"'{label}' Actor를 찾지 못했습니다.")


def find_game_actor(world, name: str):
    for actor in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor):
        if actor.get_name() == name:
            return actor
    return None


def find_animation(skeleton_path: str):
    """자세를 바꿀 애니메이션 하나(정면 걷기)를 찾는다."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    for data in registry.get_assets_by_class(unreal.TopLevelAssetPath("/Script/Engine", "AnimSequence")):
        tag = data.get_tag_value("Skeleton")
        if not tag or skeleton_path.split(".")[0] not in str(tag):
            continue
        if str(data.asset_name).endswith("Walk_F_Neut"):
            try:
                return data.get_asset()
            except Exception:
                return None
    return None


def tangents(focal: float) -> tuple:
    tan_h = (SENSOR_WIDTH_MM * 0.5) / focal
    return tan_h, tan_h * HEIGHT / WIDTH


def project(camera, rotation, point, tan_h, tan_v):
    delta = unreal.Vector(point.x - camera.x, point.y - camera.y, point.z - camera.z)
    forward, right, up = rotation.get_forward_vector(), rotation.get_right_vector(), rotation.get_up_vector()
    depth = delta.x * forward.x + delta.y * forward.y + delta.z * forward.z
    if depth <= 1.0:
        return None
    side = delta.x * right.x + delta.y * right.y + delta.z * right.z
    rise = delta.x * up.x + delta.y * up.y + delta.z * up.z
    return [0.5 + 0.5 * (side / depth) / tan_h, 0.5 - 0.5 * (rise / depth) / tan_v]


def spawn_editor_capture(location, rotation):
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(
        unreal.SceneCapture2D, location, rotation)


def spawn_editor_key_light(location):
    actor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(
        unreal.PointLight, V(location.x, location.y, location.z + 150.0), unreal.Rotator())
    component = actor.get_editor_property("point_light_component")
    component.set_editor_property("intensity", LIGHT_LUMENS)
    component.set_editor_property("attenuation_radius", 3000.0)
    component.set_editor_property("cast_shadows", False)
    return actor


class HeightCapture:
    """시뮬레이트가 뜨면 화각을 바꿔 가며 찍고, 매 프레임의 정확한 전신 높이를 기록한다."""

    def __init__(self, actor_name, capture_name, light_name, directory, animation_path, editor_actors):
        self.actor_name, self.capture_name, self.light_name = actor_name, capture_name, light_name
        self.directory = directory
        self.animation_path, self.editor_actors = animation_path, editor_actors
        self.light_actor = None
        self.frames_dir = os.path.join(directory, "frames")
        os.makedirs(self.frames_dir, exist_ok=True)
        self.tan_h, self.tan_v = tangents(FOCAL_LENGTH_MM)
        self.steps, self.index, self.pending, self.rows = [], 0, None, []
        self.waited, self.ready = 0, False
        self.mesh = self.actor = self.capture_actor = self.target = None
        self.handle = unreal.register_slate_post_tick_callback(self.tick)

    def setup(self) -> bool:
        world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()
        if world is None:
            return False
        self.actor = find_game_actor(world, self.actor_name)
        capture_actor = find_game_actor(world, self.capture_name)
        if self.actor is None or capture_actor is None:
            return False
        self.mesh = self.actor.get_components_by_class(unreal.SkeletalMeshComponent)[0]
        for other in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor):
            if other.get_name() != self.actor.get_name() and other.get_components_by_class(unreal.SkeletalMeshComponent):
                other.set_actor_hidden_in_game(True)
        self.actor.set_actor_tick_enabled(False)
        for component in self.actor.get_components_by_class(unreal.MovementComponent):
            component.set_component_tick_enabled(False)
        animation = unreal.load_asset(self.animation_path) if self.animation_path else None
        if animation:
            self.mesh.set_animation_mode(unreal.AnimationMode.ANIMATION_SINGLE_NODE)
            self.mesh.set_editor_property("visibility_based_anim_tick_option",
                                          unreal.VisibilityBasedAnimTickOption.ALWAYS_TICK_POSE_AND_REFRESH_BONES)
            self.mesh.set_animation(animation)
            self.mesh.stop()

        self.capture_actor = capture_actor
        self.light_actor = find_game_actor(world, self.light_name)
        component = capture_actor.get_editor_property("capture_component2d")
        world_for_target = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()
        self.target = unreal.RenderingLibrary.create_render_target2d(world_for_target, WIDTH, HEIGHT, unreal.TextureRenderTargetFormat.RTF_RGBA8)
        component.set_editor_property("texture_target", self.target)
        component.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        component.set_editor_property("fov_angle", math.degrees(2 * math.atan(self.tan_h)))
        # 대상 인물만 렌더한다. PIE는 적·이펙트를 계속 스폰해서 한 번 숨기는 것으로는 화면이 깨끗해지지 않는다.
        component.set_editor_property("primitive_render_mode", unreal.SceneCapturePrimitiveRenderMode.PRM_USE_SHOW_ONLY_LIST)
        component.set_editor_property("show_only_actors", [self.actor])
        self.component = component

        length = animation.get_play_length() if animation else 0.0
        for target_height in TARGET_HEIGHTS:
            for yaw_offset in YAW_OFFSETS_DEG:
                for pose in range(POSES_PER_FRAMING):
                    self.steps.append({"target_height": target_height, "yaw_offset": yaw_offset,
                                       "time": (length * pose / POSES_PER_FRAMING) if length else 0.0})
        return True

    def facing_yaw(self) -> float:
        """인물이 바라보는 방향(월드 yaw).

        Actor 회전은 쓰지 않는다. Character 블루프린트는 메시를 -90° 돌려 붙이고, 그냥 놓은 SkeletalMeshActor는
        그렇지 않아서 Actor yaw로 잡으면 옆모습이 찍힌다. Skeletal Mesh 컴포넌트의 +Y(우측 벡터)가 두 경우 모두
        인물 정면이다(upt_capture_body_truth.py와 같은 규약).
        어깨 뼈로 정면을 추정해 보기도 했는데, 서 있는 자세에서는 머리와 골반이 거의 수직으로 겹쳐
        앞뒤를 가리지 못하고 뒤통수를 고르는 경우가 있었다.
        """
        forward = self.mesh.get_right_vector()
        return math.degrees(math.atan2(forward.y, forward.x))

    def place_camera(self, step) -> dict:
        """목표 전신 높이가 나오도록 거리를 잡고, 인물 중심을 화면 가운데에 둔다."""
        origin, extent = self.actor.get_actor_bounds(False)
        body_height = max(30.0, extent.z * 2.0)
        # 화면 높이 h(화면 대비) = body / (2 * tan_v * distance)  →  distance = body / (2 * tan_v * h)
        distance = body_height / (2.0 * self.tan_v * step["target_height"])
        yaw = math.radians(self.facing_yaw() + step["yaw_offset"])
        camera = V(origin.x + math.cos(yaw) * distance, origin.y + math.sin(yaw) * distance, origin.z)
        rotation = unreal.MathLibrary.find_look_at_rotation(camera, origin)
        self.capture_actor.set_actor_location_and_rotation(camera, rotation, False, False)
        # 조명을 카메라와 함께 옮긴다. 고정해 두면 가까운 화각에서 인물이 하얗게 날아간다.
        if self.light_actor:
            self.light_actor.set_actor_location(V(camera.x, camera.y, camera.z + 80.0), False, False)
            component = self.light_actor.get_component_by_class(unreal.PointLightComponent)
            if component:
                # 거리가 멀어지면 빛이 약해지므로 거리 제곱에 맞춰 밝기를 올린다(기준 400cm에서 6만 루멘).
                component.set_intensity(LIGHT_LUMENS * max(0.25, (distance / 400.0) ** 2))
        return {"camera": camera, "rotation": rotation, "origin": origin, "extent": extent}

    def record(self, step, placement):
        name = f"frame_{len(self.rows):04d}.png"
        for _ in range(2):
            self.component.capture_scene()
        unreal.RenderingLibrary.export_render_target(self.actor.get_world(), self.target, self.frames_dir, name)

        camera, rotation = placement["camera"], placement["rotation"]
        # 정답은 뼈에서 잡는다. Actor Bounds는 캡슐·무기를 포함하고 자세를 따라가지 않아(웅크려도 그대로)
        # 화면에 보이는 몸 높이와 30% 넘게 벌어진다.
        head = self.mesh.get_socket_transform("head", unreal.RelativeTransformSpace.RTS_WORLD).translation
        feet = [self.mesh.get_socket_transform(bone, unreal.RelativeTransformSpace.RTS_WORLD).translation
                for bone in ("ball_l", "ball_r", "foot_l", "foot_r")]
        lowest = min(feet, key=lambda point: point.z)
        top = project(camera, rotation, head, self.tan_h, self.tan_v)
        bottom = project(camera, rotation, lowest, self.tan_h, self.tan_v)
        if not top or not bottom:
            return
        screen_height = abs(bottom[1] - top[1])
        origin, extent = self.actor.get_actor_bounds(False)
        bounds_top = project(camera, rotation, V(origin.x, origin.y, origin.z + extent.z), self.tan_h, self.tan_v)
        bounds_bottom = project(camera, rotation, V(origin.x, origin.y, origin.z - extent.z), self.tan_h, self.tan_v)
        self.rows.append({
            "file": name,
            # 참고용: Actor Bounds 기준 높이(엔진 검증 manifest가 쓰는 정의와 같다)
            "bounds_height": round(abs(bounds_bottom[1] - bounds_top[1]), 4) if bounds_top and bounds_bottom else None,
            "target_height": step["target_height"],
            "yaw_offset": step["yaw_offset"],
            "time": round(step["time"], 4),
            "screen_height": round(screen_height, 4),
            "top_y": round(top[1], 4),
            "bottom_y": round(bottom[1], 4),
            # 위아래가 프레임 밖으로 나가면 분석기는 전신 키를 외삽해야 한다.
            "cropped": bool(top[1] < 0.0 or bottom[1] > 1.0),
            "head_y": round(top[1], 4),
            "camera": {"location": [round(camera.x, 1), round(camera.y, 1), round(camera.z, 1)],
                       "rotation": [round(rotation.roll, 2), round(rotation.pitch, 2), round(rotation.yaw, 2)]},
        })

    def tick(self, delta_time):
        try:
            if not self.ready:
                self.waited += 1
                if self.waited > MAX_WAIT_TICKS:
                    unreal.log_error("UPT_HEIGHT 시뮬레이트 월드를 찾지 못했습니다.")
                    self.finish()
                    return
                self.ready = self.setup()
                return
            if self.pending is not None:
                self.record(self.pending[0], self.pending[1])
                self.pending = None
            if self.index >= len(self.steps):
                self.finish()
                return
            step = self.steps[self.index]
            self.index += 1
            self.mesh.set_position(step["time"], False)
            placement = self.place_camera(step)
            self.pending = (step, placement)
        except Exception as error:
            unreal.log_error(f"UPT_HEIGHT 촬영 실패: {error}")
            self.finish()

    def finish(self):
        unreal.unregister_slate_post_tick_callback(self.handle)
        unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_request_end_play()
        for actor in self.editor_actors:
            try:
                actor.destroy_actor()
            except Exception:
                pass
        if not self.rows:
            unreal.log_error("UPT_HEIGHT 기록된 프레임이 없습니다.")
            return
        truth = {"width": WIDTH, "height": HEIGHT, "focal_length": FOCAL_LENGTH_MM,
                 "sensor_width": SENSOR_WIDTH_MM, "character": self.actor_name, "frames": self.rows}
        with open(os.path.join(self.directory, "height_truth.json"), "w", encoding="utf-8") as handle:
            json.dump(truth, handle, ensure_ascii=False)
        cropped = sum(1 for row in self.rows if row["cropped"])
        unreal.log(f"UPT_HEIGHT_GROUND_TRUTH {self.directory} 프레임 {len(self.rows)} (잘린 프레임 {cropped})")


def main(argv) -> None:
    label = argv[0] if argv else "AutoHero"
    actor = find_editor_actor(label)
    mesh = actor.get_components_by_class(unreal.SkeletalMeshComponent)[0]
    skeleton = mesh.get_skeletal_mesh_asset().get_editor_property("skeleton")
    animation = find_animation(skeleton.get_path_name())

    saved = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
    directory = os.path.join(saved, "UniversalProductionTools", "GroundTruth", f"height_{time.strftime('%Y%m%d_%H%M%S')}")
    os.makedirs(directory, exist_ok=True)

    origin, _extent = actor.get_actor_bounds(False)
    capture = spawn_editor_capture(V(origin.x + 400.0, origin.y, origin.z), unreal.Rotator())
    light = spawn_editor_key_light(V(origin.x + 400.0, origin.y, origin.z))
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_play_simulate()
    HeightCapture(actor.get_name(), capture.get_name(), light.get_name(), directory,
                  animation.get_path_name() if animation else "", [capture, light])
    unreal.log(f"UPT_HEIGHT 촬영 시작 → {directory}")


if __name__ == "__main__":
    main(sys.argv[1:])
