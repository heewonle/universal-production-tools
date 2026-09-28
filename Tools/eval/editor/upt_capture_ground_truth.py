"""언리얼 에디터 Python 전용: 레벨의 캐릭터(기본 AutoHero)를 여러 카메라로 캡처해 포즈 분석 평가용 정답 데이터를 만든다.

사용(에디터 Output Log의 Cmd 칸에서 Python 모드 또는 py 명령):
  py "<프로젝트>/Plugins/UniversalProductionTools/Tools/eval/editor/upt_capture_ground_truth.py" size
  py ".../upt_capture_ground_truth.py" twoshot AutoHero --keep-partner
데이터셋: framing, motion, angle_train, angle_val, angle_test, size, twoshot, all
- 결과는 Saved/UniversalProductionTools/GroundTruth/<접두사>_<시각>/ 에 저장되며 eval/run_regression.py가 가장 최근 폴더를 쓴다.
- 캐릭터는 UE Mannequin 계열 스켈레톤(head, pelvis, ball_l/r, foot_l/r, spine_03/05, neck_01)이어야 정답 몸 곡선을 계산할 수 있다.
- 영상(mp4)은 ffmpeg가 있어야 만든다(PATH 또는 winget 설치 위치).
- motion은 배경 특징점용 임시 큐브(UPT_TEMP_BG_*), twoshot은 캐릭터 복제본(UPT_TEMP_Partner)을 만들고 끝나면 지운다.
  --keep-partner를 주면 투샷 E2E 검사(UPT.ReferenceE2E ... AutoHero UPT_TEMP_Partner)를 위해 파트너를 남긴다. 레벨은 저장하지 않는다.
"""
import glob
import json
import math
import os
import random
import shutil
import subprocess
import sys
import time

import unreal

V = unreal.Vector
W, H = 1280, 720
PARTNER_LABEL = "UPT_TEMP_Partner"


def add(a, b):
    return V(a.x + b.x, a.y + b.y, a.z + b.z)


def sub(a, b):
    return V(a.x - b.x, a.y - b.y, a.z - b.z)


def mul(a, s):
    return V(a.x * s, a.y * s, a.z * s)


def dot(a, b):
    return a.x * b.x + a.y * b.y + a.z * b.z


def actor_subsystem():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def editor_world():
    return unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()


def find_actor(label):
    for actor in actor_subsystem().get_all_level_actors():
        if actor.get_actor_label() == label:
            return actor
    raise RuntimeError(f"'{label}' Actor를 현재 레벨에서 찾지 못했습니다.")


def make_output_dir(prefix):
    saved = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())
    path = os.path.join(saved, "UniversalProductionTools", "GroundTruth", f"{prefix}_{time.strftime('%Y%m%d_%H%M%S')}")
    os.makedirs(path, exist_ok=True)
    return path


def write_json(directory, name, data):
    with open(os.path.join(directory, name), "w", encoding="utf-8") as handle:
        json.dump(data, handle, ensure_ascii=False, indent=2)


class Body:
    """발바닥~정수리를 본(발 중점-골반-척추-목-머리-정수리)으로 잇는 몸 곡선. 비율 0=발, 1=정수리."""

    def __init__(self, actor):
        mesh = actor.get_components_by_class(unreal.SkeletalMeshComponent)[0]
        loc = mesh.get_socket_location
        head, pelvis, ball_l, ball_r = loc("head"), loc("pelvis"), loc("ball_l"), loc("ball_r")
        feet_z = min(loc(name).z for name in ("ball_l", "ball_r", "foot_l", "foot_r"))
        chain = [V((ball_l.x + ball_r.x) / 2, (ball_l.y + ball_r.y) / 2, feet_z - 3.0), pelvis]
        chain += [loc(name) for name in ("spine_03", "spine_05", "neck_01") if mesh.get_bone_index(name) != -1]
        chain += [head, V(head.x, head.y, head.z + 15.0)]
        chain.sort(key=lambda point: point.z)
        self.chain = chain
        self.bottom_z = chain[0].z
        self.height = chain[-1].z - chain[0].z
        forward = mesh.get_right_vector()  # Skeletal Mesh 에셋은 +Y가 정면이다.
        length = math.hypot(forward.x, forward.y)
        self.fwd = V(forward.x / length, forward.y / length, 0.0)
        self.right = V(-self.fwd.y, self.fwd.x, 0.0)

    def point(self, ratio):
        z = self.bottom_z + self.height * ratio
        for a, b in zip(self.chain, self.chain[1:]):
            if a.z <= z <= b.z:
                t = 0.0 if b.z == a.z else (z - a.z) / (b.z - a.z)
                return add(a, mul(sub(b, a), t))
        return self.chain[-1]

    def feet(self):
        return self.chain[0]

    def at_height(self, point, ratio):
        return V(point.x, point.y, self.bottom_z + self.height * ratio)


def tangents(focal):
    tan_h = 18.0 / focal
    return tan_h, tan_h * H / W


def project(camera, rotation, point, tan_h, tan_v):
    delta = sub(point, camera)
    depth = dot(delta, rotation.get_forward_vector())
    if depth <= 1.0:
        return None
    return [round(0.5 + 0.5 * (dot(delta, rotation.get_right_vector()) / depth) / tan_h, 4),
            round(0.5 - 0.5 * (dot(delta, rotation.get_up_vector()) / depth) / tan_v, 4)]


def body_curve(body, camera, rotation, focal):
    tan_h, tan_v = tangents(focal)
    return [[round(i / 50.0, 2)] + (project(camera, rotation, body.point(i / 50.0), tan_h, tan_v) or [None, None]) for i in range(51)]


def framing_rotation(camera, target, focal, screen_x=0.5, screen_y=0.5, yaw_offset=0.0, pitch_offset=0.0):
    """target이 화면 (screen_x, screen_y)에 오도록 카메라 회전을 구한다."""
    tan_h, tan_v = tangents(focal)
    look = unreal.MathLibrary.find_look_at_rotation(camera, target)
    return unreal.Rotator(roll=0.0, pitch=look.pitch + pitch_offset - math.degrees(math.atan((1 - 2 * screen_y) * tan_v)),
                          yaw=look.yaw + yaw_offset - math.degrees(math.atan((2 * screen_x - 1) * tan_h)))


class Capturer:
    def __init__(self):
        self.world = editor_world()
        self.target = unreal.RenderingLibrary.create_render_target2d(self.world, W, H, unreal.TextureRenderTargetFormat.RTF_RGBA8)
        self.actor = actor_subsystem().spawn_actor_from_class(unreal.SceneCapture2D, V(0, 0, 0), unreal.Rotator())
        self.component = self.actor.get_editor_property("capture_component2d")
        self.component.set_editor_property("texture_target", self.target)
        self.component.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_FINAL_COLOR_LDR)

    def shoot(self, directory, name, camera, rotation, focal, warmup=4):
        self.actor.set_actor_location_and_rotation(camera, rotation, False, False)
        self.component.set_editor_property("fov_angle", math.degrees(2 * math.atan(18.0 / focal)))
        for _ in range(warmup):
            self.component.capture_scene()
        unreal.RenderingLibrary.export_render_target(self.world, self.target, directory, name)

    def close(self):
        self.actor.destroy_actor()


def find_ffmpeg():
    found = shutil.which("ffmpeg")
    if found:
        return found
    pattern = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "WinGet", "Packages", "Gyan.FFmpeg*", "*", "bin", "ffmpeg.exe")
    matches = glob.glob(pattern)
    return matches[0] if matches else None


def make_video(directory, files, seconds_per_file, video_name):
    ffmpeg = find_ffmpeg()
    video = os.path.join(directory, video_name)
    if not ffmpeg:
        unreal.log_warning("ffmpeg를 찾지 못해 영상은 만들지 않았습니다(이미지와 정답 JSON은 저장됨).")
        return video
    time.sleep(1.0)  # 렌더 타깃 PNG 내보내기가 파일로 다 써지기를 기다린다.
    concat = os.path.join(directory, "concat.txt")
    with open(concat, "w", encoding="utf-8") as handle:
        for name in files:
            handle.write(f"file '{name}'\nduration {seconds_per_file:.6f}\n")
        handle.write(f"file '{files[-1]}'\n")
    subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "concat", "-safe", "0", "-i", concat,
                    "-vf", "fps=30,format=yuv420p", "-c:v", "libx264", video], cwd=directory, capture_output=True, timeout=900)
    return video


# ---------------------------------------------------------------- 구도(샷 크기·화면 위치) 정답 영상
FRAMING_SHOTS = [
    dict(name="GT_01_Wide", dist=1000, height=0, side=0, focal=24, aim=0.5, sx=0.5, sy=0.55, size="wide"),
    dict(name="GT_02_Full_Low", dist=450, height=-60, side=0, focal=35, aim=0.5, sx=0.42, sy=0.5, size="full"),
    dict(name="GT_03_Medium_Right", dist=280, height=0, side=60, focal=50, aim=0.72, sx=0.66, sy=0.45, size="medium"),
    dict(name="GT_04_CloseUp_Left", dist=170, height=0, side=-30, focal=80, aim=0.9, sx=0.33, sy=0.4, size="close_up"),
    dict(name="GT_05_ExtremeCloseUp", dist=90, height=0, side=0, focal=100, aim=0.93, sx=0.5, sy=0.42, size="extreme_close_up"),
]


def dataset_framing(body, capturer, _actor, _options):
    directory = make_output_dir("framing")
    truth, files, start = [], [], 0.0
    for shot in FRAMING_SHOTS:
        focus = body.point(shot["aim"])
        camera = add(add(add(focus, mul(body.fwd, shot["dist"])), mul(body.right, shot["side"])), V(0.0, 0.0, shot["height"]))
        rotation = framing_rotation(camera, focus, shot["focal"], shot["sx"], shot["sy"])
        name = shot["name"] + ".png"
        capturer.shoot(directory, name, camera, rotation, shot["focal"])
        files.append(name)
        curve = body_curve(body, camera, rotation, shot["focal"])
        truth.append(dict(name=shot["name"], start=start, end=start + 2.0, intended_shot_size=shot["size"], intended_motion="static",
                          focal_length=shot["focal"], camera_distance_cm=shot["dist"], top_screen=curve[-1][1:], bottom_screen=curve[0][1:],
                          full_body_height=round(curve[0][2] - curve[-1][2], 4), focus_curve=curve))
        start += 2.0
    video = make_video(directory, files, 2.0, "ground_truth.mp4")
    write_json(directory, "ground_truth.json", dict(video=video, shots=truth))
    return directory


# ---------------------------------------------------------------- 카메라 모션 정답 영상
MOTION_SHOTS = [
    dict(name="MT_01_Static", motion="static", dist=(350, 350), focal=(45, 45), aim=0.72, sx=0.5, sy=0.45, yaw=(0, 0), jitter=False),
    dict(name="MT_02_Pan", motion="pan", dist=(600, 600), focal=(35, 35), aim=0.5, sx=0.5, sy=0.5, yaw=(-9, 9), jitter=False),
    dict(name="MT_03_DollyIn", motion="dolly_in", dist=(700, 380), focal=(35, 35), aim=0.6, sx=0.5, sy=0.5, yaw=(0, 0), jitter=False),
    dict(name="MT_04_ZoomIn", motion="zoom_in", dist=(600, 600), focal=(30, 55), aim=0.6, sx=0.5, sy=0.5, yaw=(0, 0), jitter=False),
    dict(name="MT_05_Handheld", motion="handheld", dist=(400, 400), focal=(40, 40), aim=0.6, sx=0.5, sy=0.5, yaw=(0, 0), jitter=True),
]


def dataset_motion(body, capturer, _actor, _options):
    directory = make_output_dir("motion")
    fps, frames = 12, 24
    cube = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/Cube")
    material = unreal.EditorAssetLibrary.load_asset("/Engine/EngineMaterials/DefaultMaterial")
    props = []
    truth, files, start = [], [], 0.0
    try:
        # 배경 움직임을 잴 수 있도록 캐릭터 뒤쪽에 격자 재질 큐브를 흩어 둔다.
        random.seed(7)
        for index in range(14):
            depth, lateral = 500 + (index % 5) * 220, -900 + index * 140
            position = add(add(body.feet(), mul(body.fwd, -depth)), mul(body.right, lateral))
            position = V(position.x, position.y, body.bottom_z + 60 + random.uniform(0, 260))
            prop = actor_subsystem().spawn_actor_from_object(cube, position, unreal.Rotator(roll=0.0, pitch=0.0, yaw=random.uniform(0, 90)))
            prop.set_actor_scale3d(V(random.uniform(0.8, 1.8), random.uniform(0.8, 1.8), random.uniform(0.8, 2.6)))
            prop.static_mesh_component.set_material(0, material)
            prop.set_actor_label(f"UPT_TEMP_BG_{index:02d}")
            props.append(prop)
        random.seed(11)
        for shot in MOTION_SHOTS:
            middle = None
            for frame in range(frames):
                alpha = frame / (frames - 1)
                dist = shot["dist"][0] + (shot["dist"][1] - shot["dist"][0]) * alpha
                focal = shot["focal"][0] + (shot["focal"][1] - shot["focal"][0]) * alpha
                focus = body.point(shot["aim"])
                camera = add(focus, mul(body.fwd, dist))
                if shot["jitter"]:
                    camera = add(camera, V(random.uniform(-4, 4), random.uniform(-4, 4), random.uniform(-3, 3)))
                yaw = shot["yaw"][0] + (shot["yaw"][1] - shot["yaw"][0]) * alpha + (random.uniform(-1.2, 1.2) if shot["jitter"] else 0.0)
                pitch = random.uniform(-0.8, 0.8) if shot["jitter"] else 0.0
                rotation = framing_rotation(camera, focus, focal, shot["sx"], shot["sy"], yaw, pitch)
                name = f"{shot['name']}_{frame:03d}.png"
                capturer.shoot(directory, name, camera, rotation, focal, warmup=2 if frame else 4)
                files.append(name)
                if frame == frames // 2:
                    curve = body_curve(body, camera, rotation, focal)
                    middle = dict(focus_curve=curve, top_screen=curve[-1][1:], bottom_screen=curve[0][1:], full_body_height=round(curve[0][2] - curve[-1][2], 4))
            truth.append(dict(name=shot["name"], start=start, end=start + frames / fps, intended_shot_size="", intended_motion=shot["motion"], **middle))
            start += frames / fps
    finally:
        for prop in props:
            prop.destroy_actor()
    video = make_video(directory, files, 1.0 / fps, "motion_truth.mp4")
    write_json(directory, "ground_truth.json", dict(video=video, shots=truth))
    return directory


# ---------------------------------------------------------------- 카메라 앵글 정답 이미지(학습·검증·최종 테스트)
ANGLE_SETS = {
    "angle_train": dict(prefix="angle", image="ANG", eye=0.93, high=25, overhead=60, sizes=[
        dict(size="full", dist=450, focal=35, aim=0.5, side=0.0, low=0.12),
        dict(size="medium", dist=280, focal=50, aim=0.72, side=0.0, low=0.35),
        dict(size="close_up", dist=170, focal=80, aim=0.9, side=0.0, low=0.62)]),
    "angle_val": dict(prefix="angleval", image="VAL", eye=0.85, high=35, overhead=70, sizes=[
        dict(size="full", dist=560, focal=40, aim=0.5, side=0.35, low=0.25),
        dict(size="medium", dist=320, focal=45, aim=0.72, side=-0.3, low=0.45),
        dict(size="close_up", dist=190, focal=85, aim=0.9, side=0.25, low=0.7),
        dict(size="full", dist=700, focal=28, aim=0.5, side=-0.2, low=0.25)]),
    "angle_test": dict(prefix="angletest", image="TEST", eye=0.9, high=30, overhead=65, sizes=[
        dict(size="full", dist=500, focal=32, aim=0.5, side=0.5, low=0.18),
        dict(size="medium", dist=250, focal=55, aim=0.72, side=0.15, low=0.4),
        dict(size="close_up", dist=150, focal=100, aim=0.9, side=-0.4, low=0.66),
        dict(size="wide", dist=950, focal=24, aim=0.5, side=0.1, low=0.15)]),
}


def dataset_angle(body, capturer, _actor, options):
    spec = ANGLE_SETS[options["dataset"]]
    directory = make_output_dir(spec["prefix"])
    truth = []
    for index, size in enumerate(spec["sizes"]):
        focus = body.point(size["aim"])
        horizontal = add(focus, add(mul(body.fwd, size["dist"]), mul(body.right, size["dist"] * size["side"])))
        ground_distance = math.hypot(size["dist"], size["dist"] * size["side"])
        # low/eye는 카메라 높이를 몸 키 비율로, high/overhead는 조준점에서 내려다보는 각도로 정한다.
        heights = [("low", body.bottom_z + body.height * size["low"]), ("eye", body.bottom_z + body.height * spec["eye"]),
                   ("high", focus.z + ground_distance * math.tan(math.radians(spec["high"]))),
                   ("overhead", focus.z + ground_distance * math.tan(math.radians(spec["overhead"])))]
        for label, camera_z in heights:
            camera = V(horizontal.x, horizontal.y, camera_z)
            rotation = unreal.MathLibrary.find_look_at_rotation(camera, focus)
            name = f"{spec['image']}_{index}_{size['size']}_{label}.png"
            capturer.shoot(directory, name, camera, rotation, size["focal"])
            truth.append(dict(image=name, shot_size=size["size"], intended_angle=label, pitch_deg=round(rotation.pitch, 2),
                              camera_height_ratio=round((camera_z - body.bottom_z) / body.height, 3), distance_cm=size["dist"],
                              focal_length=size["focal"], side_ratio=size["side"]))
    write_json(directory, "angle_truth.json", truth)
    return directory


# ---------------------------------------------------------------- 크기 정답 이미지(거리·렌즈·측면·높이·화면 위치를 무작위로 섞음)
SIZE_PLAN = [("full", 8, (450, 650), (28, 40), 0.5), ("medium", 12, (240, 340), (45, 55), 0.72),
             ("close_up", 16, (140, 220), (65, 100), 0.88), ("extreme_close_up", 8, (80, 110), (95, 110), 0.92)]


def dataset_size(body, capturer, _actor, _options):
    directory = make_output_dir("size")
    random.seed(23)
    truth = []
    for size, count, dist_range, focal_range, aim in SIZE_PLAN:
        for index in range(count):
            dist, focal = random.uniform(*dist_range), random.uniform(*focal_range)
            side, pitch = random.uniform(-0.4, 0.4), random.uniform(-12.0, 15.0)
            screen_x = random.choice([0.33, 0.42, 0.5, 0.58, 0.66])
            screen_y = random.uniform(0.38, 0.55) if size in ("close_up", "extreme_close_up") else random.uniform(0.42, 0.55)
            focus = body.point(aim)
            horizontal = add(focus, add(mul(body.fwd, dist), mul(body.right, dist * side)))
            camera = V(horizontal.x, horizontal.y, focus.z + math.hypot(dist, dist * side) * math.tan(math.radians(pitch)))
            rotation = framing_rotation(camera, focus, focal, screen_x, screen_y)
            name = f"SZ_{size}_{index:02d}.png"
            capturer.shoot(directory, name, camera, rotation, focal)
            curve = body_curve(body, camera, rotation, focal)
            tan_h, tan_v = tangents(focal)
            truth.append(dict(image=name, shot_size=size, distance_cm=round(dist, 1), focal_length=round(focal, 1), side_ratio=round(side, 3),
                              pitch_offset_deg=round(pitch, 2), aim=aim, focus_screen=project(camera, rotation, focus, tan_h, tan_v),
                              full_body_height=round(curve[0][2] - curve[-1][2], 4), focus_curve=curve))
    write_json(directory, "size_truth.json", truth)
    return directory


# ---------------------------------------------------------------- 두 인물(투샷·어깨 너머·클로즈업) 정답 영상
def dataset_twoshot(body, capturer, actor, options):
    for old in [a for a in actor_subsystem().get_all_level_actors() if a.get_actor_label() == PARTNER_LABEL]:
        old.destroy_actor()
    partner = actor_subsystem().duplicate_actor(actor, editor_world(), V(0.0, 0.0, 0.0))
    keep = options.get("keep_partner", False)
    try:
        partner.set_actor_label(PARTNER_LABEL)
        partner.set_actor_location(add(actor.get_actor_location(), mul(body.fwd, 170.0)), False, False)
        partner.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=0.0, yaw=actor.get_actor_rotation().yaw + 180.0), False)
        # 색으로 두 인물을 구분할 수 있게 파트너에 밝은 기본 재질을 입힌다.
        partner_mesh = partner.get_components_by_class(unreal.SkeletalMeshComponent)[0]
        white = unreal.EditorAssetLibrary.load_asset("/Engine/BasicShapes/BasicShapeMaterial")
        for slot in range(partner_mesh.get_num_materials()):
            partner_mesh.set_material(slot, white)
        bodies = {actor.get_actor_label(): Body(actor), PARTNER_LABEL: Body(partner)}
        hero_label = actor.get_actor_label()
        hero_feet, partner_feet = bodies[hero_label].feet(), bodies[PARTNER_LABEL].feet()
        middle = mul(add(hero_feet, partner_feet), 0.5)
        shots = [
            dict(name="TS_01_TwoShot_Wide", camera=add(body.at_height(middle, 0.55), mul(body.right, 650)), target=body.at_height(middle, 0.5), focal=28, sx=0.5, sy=0.5),
            dict(name="TS_02_OTS_Hero", camera=add(add(body.at_height(partner_feet, 0.9), mul(body.fwd, 70)), mul(body.right, 45)),
                 target=bodies[hero_label].point(0.88), focal=50, sx=0.6, sy=0.42),
            dict(name="TS_03_OTS_Partner", camera=add(add(body.at_height(hero_feet, 0.9), mul(body.fwd, -70)), mul(body.right, -45)),
                 target=bodies[PARTNER_LABEL].point(0.88), focal=50, sx=0.6, sy=0.42),
            dict(name="TS_04_TwoShot_Medium", camera=add(body.at_height(middle, 0.7), mul(body.right, 300)), target=body.at_height(middle, 0.72), focal=40, sx=0.5, sy=0.45),
            dict(name="TS_05_CloseUp_Partner", camera=add(add(body.at_height(partner_feet, 0.9), mul(body.fwd, -150)), mul(body.right, 80)),
                 target=bodies[PARTNER_LABEL].point(0.88), focal=70, sx=0.45, sy=0.4),
        ]
        directory = make_output_dir("twoshot")
        truth, files, start = [], [], 0.0
        for shot in shots:
            rotation = framing_rotation(shot["camera"], shot["target"], shot["focal"], shot["sx"], shot["sy"])
            name = shot["name"] + ".png"
            capturer.shoot(directory, name, shot["camera"], rotation, shot["focal"])
            files.append(name)
            tan_h, tan_v = tangents(shot["focal"])
            subjects = []
            for label, subject_body in bodies.items():
                top = project(shot["camera"], rotation, subject_body.point(1.0), tan_h, tan_v)
                bottom = project(shot["camera"], rotation, subject_body.point(0.0), tan_h, tan_v)
                subjects.append(dict(label=label, depth_cm=round(dot(sub(subject_body.point(0.5), shot["camera"]), rotation.get_forward_vector()), 1),
                                     full_body_height=round(bottom[1] - top[1], 4) if top and bottom else None,
                                     mid=project(shot["camera"], rotation, subject_body.point(0.5), tan_h, tan_v),
                                     head=project(shot["camera"], rotation, subject_body.point(0.88), tan_h, tan_v)))
            truth.append(dict(name=shot["name"], start=start, end=start + 2.0, focal_length=shot["focal"], subjects=subjects))
            start += 2.0
        video = make_video(directory, files, 2.0, "twoshot_truth.mp4")
        write_json(directory, "twoshot_truth.json", dict(video=video, actors=list(bodies), actor_gap_cm=170, shots=truth))
        return directory
    finally:
        if not keep:
            partner.destroy_actor()


DATASETS = {
    "framing": dataset_framing, "motion": dataset_motion, "angle_train": dataset_angle, "angle_val": dataset_angle,
    "angle_test": dataset_angle, "size": dataset_size, "twoshot": dataset_twoshot,
}


def main(argv):
    positional = [arg for arg in argv if not arg.startswith("--")]
    if not positional or positional[0] not in list(DATASETS) + ["all"]:
        unreal.log_error(f"사용: upt_capture_ground_truth.py <{'|'.join(list(DATASETS) + ['all'])}> [ActorLabel] [--keep-partner]")
        return
    label = positional[1] if len(positional) > 1 else "AutoHero"
    actor = find_actor(label)
    body = Body(actor)
    capturer = Capturer()
    try:
        for name in (list(DATASETS) if positional[0] == "all" else [positional[0]]):
            options = {"dataset": name, "keep_partner": "--keep-partner" in argv}
            directory = DATASETS[name](body, capturer, actor, options)
            unreal.log(f"UPT_GROUND_TRUTH {name} -> {directory}")
    finally:
        capturer.close()


if __name__ == "__main__":
    main(sys.argv[1:])
