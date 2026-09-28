"""언리얼 에디터 전용: 캐릭터에 애니메이션을 재생시켜 영상으로 찍고, 같은 순간의 실제 뼈 값을 정답으로 남긴다.

영상에서 몸 동작을 뽑는 기능(3D 포즈 → 애니메이션)의 정확도를 숫자로 재기 위한 정답 데이터다.

에디터 월드에서는 자세가 갱신되지 않는다. 확인한 것:
- SkeletalMeshComponent.set_position(단일 노드)만으로는 본 트랜스폼이 그대로여서 전 프레임이 레퍼런스 포즈로 굳는다.
- 시퀀서에 애니메이션을 깔고 스크럽해도 섹션이 바뀔 때만 자세가 바뀐다(프레임 단위 재평가가 안 된다).
그래서 시뮬레이트(PIE)를 켜 월드가 실제로 틱하게 한 뒤 찍는다. 틱 콜백으로 '이번 틱에 자세 지정 →
다음 틱에 촬영·기록' 순서로 진행해 이미지와 뼈 값이 같은 순간을 가리키게 한다.

사용(에디터 Output Log의 Cmd 칸에서):
  py "<프로젝트>/Plugins/UniversalProductionTools/Tools/eval/editor/upt_capture_body_truth.py"
  py "...upt_capture_body_truth.py" AutoHero 3.0

결과: Saved/UniversalProductionTools/GroundTruth/bodypose_<시각>/
  frames/frame_0000.png ...   (프레임 이미지)
  body_truth.mp4              (ffmpeg가 있으면)
  body_truth.json             (카메라 정보 + 프레임별 뼈 월드 위치·회전)
"""
import glob
import json
import math
import os
import shutil
import subprocess
import sys
import time

import unreal

V = unreal.Vector
WIDTH, HEIGHT = 1280, 720
FPS = 24
DEFAULT_SECONDS = 3.0
# 정답으로 남길 뼈(UE4 마네킹 기준). 3D 포즈가 추정하는 관절과 대응되는 것들.
BONES = [
    "pelvis", "spine_01", "spine_02", "spine_03", "neck_01", "head",
    "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l",
    "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r",
    "thigh_l", "calf_l", "foot_l", "ball_l",
    "thigh_r", "calf_r", "foot_r", "ball_r",
]
# 정면 고정 카메라: 캐릭터 정면에서 살짝 위, 전신이 들어오는 거리
CAMERA_DISTANCE_CM = 420.0
CAMERA_HEIGHT_RATIO = 0.62
FOCAL_LENGTH_MM = 35.0
# 자세가 갱신됐는지 보는 기준: 이웃 프레임 사이 손·발 이동 중앙값이 이보다 작으면 실패로 본다.
MIN_MOTION_CM = 0.5
# 시뮬레이트가 준비될 때까지 기다리는 최대 틱 수
MAX_WAIT_TICKS = 600


def find_editor_actor(label: str):
    for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
        if actor.get_actor_label() == label:
            return actor
    raise RuntimeError(f"'{label}' Actor를 찾지 못했습니다.")


def find_game_actor(world, name: str):
    """시뮬레이트 월드에서 같은 이름의 Actor를 찾는다(PIE에는 라벨이 없다)."""
    for actor in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor):
        if actor.get_name() == name:
            return actor
    return None


def find_animations(skeleton_path: str) -> dict:
    """정면 달리기·걷기 애니메이션을 이름으로 찾는다."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    found = {}
    for data in registry.get_assets_by_class(unreal.TopLevelAssetPath("/Script/Engine", "AnimSequence")):
        tag = data.get_tag_value("Skeleton")
        if not tag or skeleton_path.split(".")[0] not in str(tag):
            continue
        name = str(data.asset_name)
        for key, needle in (("run", "Run_F_Neut"), ("walk", "Walk_F_Neut")):
            if key not in found and name.endswith(needle):
                try:
                    found[key] = data.get_asset()
                except Exception as error:  # 리타게터 포즈 등 깨진 에셋은 건너뛴다
                    unreal.log_warning(f"UPT_BODY {name} 열기 실패: {str(error)[:80]}")
    return found


def tangents(focal: float) -> tuple:
    tan_h = 18.0 / focal
    return tan_h, tan_h * HEIGHT / WIDTH


def project(camera, rotation, point, tan_h, tan_v):
    delta = unreal.Vector(point.x - camera.x, point.y - camera.y, point.z - camera.z)
    forward, right, up = rotation.get_forward_vector(), rotation.get_right_vector(), rotation.get_up_vector()
    depth = delta.x * forward.x + delta.y * forward.y + delta.z * forward.z
    if depth <= 1.0:
        return None
    side = delta.x * right.x + delta.y * right.y + delta.z * right.z
    rise = delta.x * up.x + delta.y * up.y + delta.z * up.z
    return [round(0.5 + 0.5 * (side / depth) / tan_h, 4), round(0.5 - 0.5 * (rise / depth) / tan_v, 4)]


def spawn_editor_capture(location, rotation):
    """시뮬레이트를 켜면 에디터 월드가 복제되므로, 카메라는 미리 에디터 월드에 띄워 둔다."""
    actor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(
        unreal.SceneCapture2D, location, rotation)
    return actor


def spawn_editor_key_light(location):
    """카메라 쪽에서 인물을 비추는 조명. 역광 실루엣으로 찍히면 포즈를 읽을 수 없다."""
    actor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(
        unreal.PointLight, V(location.x, location.y, location.z + 150.0), unreal.Rotator())
    component = actor.get_editor_property("point_light_component")
    component.set_editor_property("intensity", 60000.0)  # 루멘. 4m 거리에서 인물이 과노출되지 않는 정도
    component.set_editor_property("attenuation_radius", 2000.0)
    component.set_editor_property("cast_shadows", False)
    return actor


class Capturer:
    """시뮬레이트 월드로 복제된 SceneCapture를 잡아 렌더 타깃으로 찍는다."""

    def __init__(self, world, actor):
        self.world = world
        self.target = unreal.RenderingLibrary.create_render_target2d(world, WIDTH, HEIGHT, unreal.TextureRenderTargetFormat.RTF_RGBA8)
        self.actor = actor
        self.component = self.actor.get_editor_property("capture_component2d")
        self.component.set_editor_property("texture_target", self.target)
        self.component.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)
        self.component.set_editor_property("fov_angle", math.degrees(2 * math.atan(18.0 / FOCAL_LENGTH_MM)))

    def shoot(self, directory, name, warmup=2):
        for _ in range(warmup):
            self.component.capture_scene()
        unreal.RenderingLibrary.export_render_target(self.world, self.target, directory, name)

    def close(self):
        try:
            self.actor.destroy_actor()
        except Exception:
            pass


def find_ffmpeg():
    found = shutil.which("ffmpeg")
    if found:
        return found
    pattern = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "WinGet", "Packages", "Gyan.FFmpeg*", "*", "bin", "ffmpeg.exe")
    matches = glob.glob(pattern)
    return matches[0] if matches else None


def camera_for(actor, mesh) -> tuple:
    """캐릭터 정면에 고정 카메라를 둔다. 위치·회전·화각 탄젠트를 돌려준다."""
    origin, extent = actor.get_actor_bounds(False)
    height = extent.z * 2.0
    forward = mesh.get_right_vector()  # Skeletal Mesh 에셋은 +Y가 정면
    length = math.hypot(forward.x, forward.y) or 1.0
    focus = V(origin.x, origin.y, origin.z - height * 0.5 + height * CAMERA_HEIGHT_RATIO)
    camera = V(focus.x + forward.x / length * CAMERA_DISTANCE_CM,
               focus.y + forward.y / length * CAMERA_DISTANCE_CM,
               focus.z)
    rotation = unreal.MathLibrary.find_look_at_rotation(camera, focus)
    tan_h, tan_v = tangents(FOCAL_LENGTH_MM)
    return camera, rotation, tan_h, tan_v


def build_steps(animations: dict, seconds: float) -> tuple:
    """찍을 순서(클립별 프레임 목록)와 클립 정보를 만든다."""
    steps, clips = [], []
    for kind in ("run", "walk"):
        animation = animations.get(kind)
        if not animation:
            continue
        count = int(min(seconds, animation.get_play_length()) * FPS)
        clips.append({"kind": kind, "animation": animation.get_name(), "start_frame": len(steps), "frames": count})
        start = len(steps)
        for index in range(count):
            steps.append({"kind": kind, "animation": animation, "time": round(index / float(FPS), 4),
                          "frame": start + index})
    return steps, clips


class CaptureSession:
    """시뮬레이트가 뜨기를 기다렸다가, 틱마다 자세를 지정하고 다음 틱에 찍는다."""

    def __init__(self, actor_name, capture_name, steps, clips, directory, header, camera, rotation, tan_h, tan_v, editor_capture, editor_light):
        self.actor_name, self.capture_name = actor_name, capture_name
        self.steps, self.clips = steps, clips
        self.directory, self.header = directory, header
        self.camera, self.rotation, self.tan_h, self.tan_v = camera, rotation, tan_h, tan_v
        self.editor_capture, self.editor_light = editor_capture, editor_light
        self.frames_dir = os.path.join(directory, "frames")
        os.makedirs(self.frames_dir, exist_ok=True)
        self.index, self.pending, self.current_animation, self.rows = 0, None, None, []
        self.waited, self.ready = 0, False
        self.mesh = self.capturer = None
        self.handle = unreal.register_slate_post_tick_callback(self.tick)

    def setup(self) -> bool:
        """시뮬레이트 월드가 준비되면 캐릭터·카메라를 잡는다."""
        world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()
        if world is None:
            return False
        actor = find_game_actor(world, self.actor_name)
        if actor is None:
            return False
        self.mesh = actor.get_components_by_class(unreal.SkeletalMeshComponent)[0]
        hidden = []
        for other in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Actor):
            if other.get_name() != actor.get_name() and other.get_components_by_class(unreal.SkeletalMeshComponent):
                other.set_actor_hidden_in_game(True)  # 뒤에 선 다른 캐릭터가 같이 잡히면 안 된다
                hidden.append(other.get_name())
        unreal.log(f"UPT_BODY 숨긴 캐릭터 {len(hidden)}개: {hidden[:6]}")
        actor.set_actor_tick_enabled(False)  # 이동 로직이 캐릭터를 화면 밖으로 끌고 가지 않게 한다
        for component in actor.get_components_by_class(unreal.MovementComponent):
            component.set_component_tick_enabled(False)
        self.mesh.set_animation_mode(unreal.AnimationMode.ANIMATION_SINGLE_NODE)
        try:
            self.mesh.set_editor_property("visibility_based_anim_tick_option",
                                          unreal.VisibilityBasedAnimTickOption.ALWAYS_TICK_POSE_AND_REFRESH_BONES)
        except Exception as error:
            unreal.log_warning(f"UPT_BODY 틱 옵션 설정 실패: {str(error)[:80]}")
        capture_actor = find_game_actor(world, self.capture_name)
        if capture_actor is None:
            return False
        self.capturer = Capturer(world, capture_actor)
        return True

    def record(self, step):
        self.capturer.shoot(self.frames_dir, f"frame_{step['frame']:04d}.png")
        bones = {}
        for bone in BONES:
            transform = self.mesh.get_socket_transform(bone, unreal.RelativeTransformSpace.RTS_WORLD)
            location, rotator = transform.translation, transform.rotation.rotator()
            bones[bone] = {
                "loc": [round(location.x, 2), round(location.y, 2), round(location.z, 2)],
                "rot": [round(rotator.roll, 2), round(rotator.pitch, 2), round(rotator.yaw, 2)],
                "screen": project(self.camera, self.rotation, location, self.tan_h, self.tan_v),
            }
        self.rows.append({"frame": step["frame"], "time": step["time"],
                          "animation": step["animation"].get_name(), "bones": bones})

    def tick(self, delta_time):
        try:
            if not self.ready:
                self.waited += 1
                if self.waited > MAX_WAIT_TICKS:
                    unreal.log_error("UPT_BODY 시뮬레이트 월드를 찾지 못했습니다.")
                    self.finish()
                    return
                self.ready = self.setup()
                return
            if self.pending is not None:
                self.record(self.pending)
                self.pending = None
            if self.index >= len(self.steps):
                self.finish()
                return
            step = self.steps[self.index]
            self.index += 1
            if step["animation"] is not self.current_animation:
                self.current_animation = step["animation"]
                self.mesh.set_animation(step["animation"])
                self.mesh.stop()
            self.mesh.set_position(step["time"], False)
            self.pending = step
        except Exception as error:
            unreal.log_error(f"UPT_BODY 촬영 실패: {error}")
            self.finish()

    def motion_range(self) -> float:
        """이웃한 프레임 사이에 손·발이 움직인 거리의 중앙값(cm).

        전체 범위로 재면 클립이 바뀔 때 한 번 튄 것만으로도 통과해 버린다. 프레임마다 자세가
        실제로 진행하는지 보려면 연속 프레임의 변화를 봐야 한다."""
        steps = []
        for index in range(1, len(self.rows)):
            if self.rows[index]["animation"] != self.rows[index - 1]["animation"]:
                continue  # 클립이 바뀌는 지점은 건너뛴다
            for bone in ("hand_l", "hand_r", "foot_l", "foot_r"):
                before = self.rows[index - 1]["bones"].get(bone)
                after = self.rows[index]["bones"].get(bone)
                if before and after:
                    steps.append(max(abs(a - b) for a, b in zip(after["loc"], before["loc"])))
        if not steps:
            return 0.0
        steps.sort()
        return steps[len(steps) // 2]

    def finish(self):
        unreal.unregister_slate_post_tick_callback(self.handle)
        unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_request_end_play()
        for actor in (self.editor_capture, self.editor_light):  # 에디터 월드에 남은 촬영용 액터를 치운다
            try:
                actor.destroy_actor()
            except Exception:
                pass

        moved = self.motion_range()
        if moved < MIN_MOTION_CM:
            unreal.log_error(f"UPT_BODY 자세가 프레임마다 갱신되지 않았습니다(프레임당 중앙값 {moved:.2f}cm). 정답 데이터를 남기지 않습니다.")
            return

        video = os.path.join(self.directory, "body_truth.mp4")
        ffmpeg = find_ffmpeg()
        if ffmpeg:
            subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-framerate", str(FPS),
                            "-i", os.path.join(self.frames_dir, "frame_%04d.png"),
                            "-c:v", "libx264", "-pix_fmt", "yuv420p", video], capture_output=True, timeout=900)
        else:
            unreal.log_warning("UPT_BODY ffmpeg가 없어 영상은 만들지 않았습니다(프레임 이미지는 있습니다).")

        truth = dict(self.header)
        truth.update({"video": video, "fps": float(FPS), "bones": BONES, "clips": self.clips, "frames": self.rows,
                      "camera": {"location": [self.camera.x, self.camera.y, self.camera.z],
                                 "rotation": [self.rotation.roll, self.rotation.pitch, self.rotation.yaw],
                                 "focal_length": FOCAL_LENGTH_MM, "width": WIDTH, "height": HEIGHT}})
        with open(os.path.join(self.directory, "body_truth.json"), "w", encoding="utf-8") as handle:
            json.dump(truth, handle, ensure_ascii=False)
        unreal.log(f"UPT_BODY_GROUND_TRUTH {self.directory} 프레임 {len(self.rows)} 클립 {len(self.clips)} 이동 {moved:.1f}cm")


def main(argv) -> None:
    label = argv[0] if argv else "AutoHero"
    seconds = float(argv[1]) if len(argv) > 1 else DEFAULT_SECONDS
    actor = find_editor_actor(label)
    mesh = actor.get_components_by_class(unreal.SkeletalMeshComponent)[0]
    skeleton = mesh.get_skeletal_mesh_asset().get_editor_property("skeleton")
    animations = find_animations(skeleton.get_path_name())
    if not animations:
        unreal.log_error("UPT_BODY 정면 달리기·걷기 애니메이션을 찾지 못했습니다.")
        return
    steps, clips = build_steps(animations, seconds)

    saved = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
    directory = os.path.join(saved, "UniversalProductionTools", "GroundTruth", f"bodypose_{time.strftime('%Y%m%d_%H%M%S')}")
    os.makedirs(directory, exist_ok=True)

    camera, rotation, tan_h, tan_v = camera_for(actor, mesh)
    editor_capture = spawn_editor_capture(camera, rotation)
    editor_light = spawn_editor_key_light(camera)
    unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_play_simulate()
    CaptureSession(actor.get_name(), editor_capture.get_name(), steps, clips, directory,
                   {"character": label, "skeleton": skeleton.get_name()},
                   camera, rotation, tan_h, tan_v, editor_capture, editor_light)
    unreal.log(f"UPT_BODY 촬영 시작 {len(steps)}프레임 → {directory}")


if __name__ == "__main__":
    main(sys.argv[1:])
