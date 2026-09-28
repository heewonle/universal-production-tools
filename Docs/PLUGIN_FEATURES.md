# Universal Production Tools

UE 5.7용 Editor 전용 프로토타입 플러그인입니다.

## 현재 제공 기능

- 자연어 시네마틱 스크립트를 OpenAI 호환 Chat Completions API로 전송
- LLM 응답을 제한된 Shot Plan JSON으로 변환
- Shot 수, 길이, 프레임레이트, 초점거리, 카메라 위치 범위 검증
- 검증된 Plan을 사용자에게 먼저 표시
- 각 Shot의 Cine Camera Actor와 Camera Cut Track을 포함한 Level Sequence 생성
- 레벨에서 선택한 Actor를 이름 기반 등장인물 컨텍스트로 전달
- Subject Actor 기준 거리·높이·측면 오프셋 카메라 프레이밍
- Look-at Actor의 Bounds 중심을 향하는 카메라 회전 자동 계산
- Visibility Sphere Sweep 기반 벽·지형 내부 카메라 보정
- 선택한 Animation/Audio 에셋을 안전한 임시 ID로 LLM에 제공
- Subject의 Skeletal Mesh Component에 Animation Track 자동 생성
- Shot 시작·종료 프레임에 맞춘 Skeletal Animation Section 배치
- Master Sequence Audio Track 및 Shot 구간 Audio Section 생성
- Content Browser에서 선택한 Skeletal Mesh의 이름·계층·레퍼런스 포즈 기반 Semantic Bone 분석
- 스켈레톤 Bounds 기반 좌우축 및 좌측 부호 추론
- Humanoid 핵심 팔·다리·척추 체인 후보와 항목별 신뢰도 출력
- 리타기팅 준비도와 A/B/C 검수 등급 출력
- Twist/Roll, Finger, Face 보조 본 분류
- 핵심 역할을 JSON으로 수동 덮어쓰기하고 즉시 재검증
- 검증된 Semantic Mapping을 재사용 가능한 Skeleton Profile DataAsset으로 저장
- LLM 없이 Shot Plan JSON을 직접 입력하는 오프라인 테스트 경로

## API 설정

API 키를 프로젝트 파일이나 Unreal 설정에 저장하지 않습니다. Unreal Editor를 실행하기 전에 사용자 환경변수로 지정합니다.

```powershell
[Environment]::SetEnvironmentVariable("UPT_LLM_API_KEY", "YOUR_KEY", "User")
```

환경변수 설정 후 Unreal Editor를 완전히 다시 시작해야 합니다. Endpoint, 모델, 환경변수 이름은 다음 위치에서 변경합니다.

```text
Editor Preferences > Plugins > Universal Production Tools
```

기본 Endpoint는 `https://api.openai.com/v1/chat/completions`, 기본 모델은 `gpt-5.6-sol`입니다. Chat Completions 및 `response_format: json_object`와 호환되는 사내 게이트웨이도 사용할 수 있지만 HTTPS만 허용합니다.

## 사용 순서

1. Unreal Editor에서 `Window > Universal Production Tools`를 엽니다.
2. 레벨에서 등장인물 Actor들을 선택하고 `현재 선택 Actor 가져오기`를 누릅니다.
3. Content Browser에서 사용할 Animation Sequence/Montage와 Sound Wave/Cue를 선택하고 `Content Browser 선택 미디어 가져오기`를 누릅니다.
4. 시네마틱 대본과 연출 지시를 입력합니다.
5. `LLM으로 Shot Plan 생성`을 누릅니다.
6. 반환된 JSON에서 `JSON 검증 및 미리보기`를 누릅니다.
7. Actor Binding, 미디어 ID, Shot 길이, 렌즈를 확인합니다.
8. `검증된 Plan으로 Level Sequence 생성`을 누릅니다.

생성 에셋 기본 경로는 `/Game/Cinematics/Generated`입니다. 카메라는 Level Sequence의 Spawnable로 생성되므로 현재 레벨을 수정하거나 자동 저장하지 않습니다. 선택된 등장인물 Actor는 Possessable Binding으로 연결됩니다.

## 직접 입력 예제

```json
{
  "title": "Temple_Reveal",
  "frame_rate": 30,
  "shots": [
    {
      "name": "Establishing",
      "description": "폐허가 된 신전과 제단을 보여주는 와이드 샷",
      "subject": "",
      "look_at": "",
      "animation_id": "",
      "audio_id": "audio_1",
      "duration_seconds": 4.0,
      "distance_cm": 600,
      "height_cm": 100,
      "side_cm": 0,
      "camera_location": { "x": -600, "y": 0, "z": 220 },
      "camera_rotation": { "pitch": -5, "yaw": 0, "roll": 0 },
      "focal_length": 28
    },
    {
      "name": "Goddess_CloseUp",
      "description": "여신이 눈을 뜨는 클로즈업",
      "subject": "BP_Goddess_C_0",
      "look_at": "BP_Goddess_C_0",
      "animation_id": "anim_1",
      "audio_id": "audio_2",
      "duration_seconds": 3.0,
      "distance_cm": 150,
      "height_cm": 20,
      "side_cm": 25,
      "camera_location": { "x": -150, "y": 25, "z": 175 },
      "camera_rotation": { "pitch": 0, "yaw": 0, "roll": 0 },
      "focal_length": 85
    }
  ]
}
```

## 안전 및 제한 사항

- LLM은 코드, 에셋 경로, 콘솔 명령을 실행할 수 없습니다.
- 스키마에 없는 필드는 생성기에서 사용하지 않습니다.
- API 응답은 최대 500자까지만 오류 메시지에 표시합니다.
- 생성 버튼을 누르기 전에는 Actor나 에셋을 생성하지 않습니다.
- 생성 과정은 Level Sequence와 카메라 초안까지만 담당하며 레벨 저장과 렌더는 자동 실행하지 않습니다.
- 선택 Actor의 Label을 변경하거나 Actor를 삭제하면 Plan을 다시 검증해야 합니다.
- `animation_id`와 `audio_id`는 현재 선택 미디어에서 생성된 ID만 허용됩니다. LLM이 임의 에셋 경로를 지정할 수 없습니다.
- Animation과 Subject의 Skeleton이 다르면 생성 전에 중단됩니다. 리타기팅된 Animation을 선택해야 합니다.
- Animation Section이 원본 길이보다 길면 Sequencer의 기본 반복 동작을 따르며, 짧으면 Shot 종료 시 잘립니다.
- Audio Section은 Shot 범위에 맞춰 잘립니다.
- 충돌 보정은 Visibility 채널을 사용하므로 프로젝트의 Collision Preset에 따라 결과가 달라질 수 있습니다.
- 스켈레톤 분석은 이름·계층·공간 점수를 함께 사용하지만, 아직 IK Rig/Retargeter 에셋을 자동 생성하지 않습니다.
- `CHECK`와 `LOW`로 표시된 본은 자동 생성 전에 작업자 검수가 필요합니다.
- 수동 매핑 형식은 `{"Pelvis":"pelvis","LeftHand":"hand_l"}`입니다. 역할 이름은 분석 결과의 Semantic Role과 정확히 일치해야 합니다.
- 존재하지 않는 본, 지원하지 않는 역할, 한 본의 복수 수동 배정은 거부됩니다.
- 수동 매핑은 `MANUAL`과 100% 신뢰도로 표시되며 충돌하는 자동 후보는 해제됩니다.
- 분석 후 `검증 결과로 Skeleton Profile 생성`을 누르면 `/Game/Animation/UPT/Profiles`에 `UPT_SKP_메시이름` 에셋이 생성됩니다.
- Profile에는 Source Mesh, 역할별 본, 신뢰도, 준비도, 보조 본 목록과 사용한 Override JSON이 저장됩니다.
- Override 검증 오류가 있거나 Mapping이 비어 있으면 Profile을 생성하지 않습니다.
- Skeleton Profile을 선택하고 `Skeleton Profile로 IK Rig 체인 생성`을 누르면 `/Game/Animation/UPT/IKRigs`에 IK Rig가 생성됩니다.
- 생성 체인은 Root, Spine, LeftArm, RightArm, LeftLeg, RightLeg이며 Clavicle과 Neck은 Mapping이 있을 때 추가됩니다.
- Pelvis가 Retarget Root로 설정됩니다. 필수 Mapping 누락, 70% 미만 준비도, 끊어진 본 계층은 에셋 생성 전에 차단됩니다.
- IK Rig 체인 생성과 FBIK 추가는 별도 버튼으로 분리되어 Retarget Chain 전용 에셋도 만들 수 있습니다.
- `선택 IK Rig에 손·발 Full Body IK 추가` 버튼은 Left/Right Hand와 Foot Goal, Full Body IK Solver를 선택적으로 추가합니다.
- Solver Root는 Pelvis, Root Behavior는 Free, Global Pull Chain Alpha는 0으로 설정됩니다.
- Goal은 대응하는 LeftArm/RightArm/LeftLeg/RightLeg Retarget Chain에도 연결됩니다.
- Finger와 Face 보조 본은 FBIK 해석 대상에서 제외되며 Twist 본은 유지됩니다.
- 이미 Solver가 있거나 IK Rig Preview Mesh와 Profile Source Mesh가 다르면 중복 적용을 차단합니다.
- Twist/Finger/Face 분류는 보조 정보이며 핵심 Humanoid 리타기팅 준비도 계산에서는 제외됩니다.

## IK Retargeter 자동 생성

- Content Browser에서 Skeleton Profile과 이에 대응하는 IK Rig을 함께 선택해 Source와 Target 슬롯에 각각 지정합니다.
- Profile Source Mesh와 IK Rig Preview Mesh의 일치 여부, Root/Spine/양팔/양다리 필수 체인을 먼저 검증합니다.
- `/Game/Animation/UPT/Retargeters`에 Source/Target Rig 및 Preview Mesh, 기본 Retarget Ops가 연결된 IK Retargeter를 생성합니다.
- 플러그인이 생성한 표준 체인 이름은 Exact 방식으로 자동 매핑됩니다. Retarget Pose 보정은 캐릭터별 확인이 필요하므로 자동 변경하지 않습니다.

## Content Browser 애니메이션 범용화

- 대상 Skeletal Mesh를 우클릭하고 `애니메이션 범용화`를 선택하면 프로젝트의 Animation Sequence 목록이 열립니다.
- Ctrl/Shift로 여러 애니메이션을 선택하고 `선택 애니메이션 일괄 리타기팅`을 누르면 Source Skeleton별 Profile, IK Rig, IK Retargeter를 자동 준비합니다.
- 생성 결과에는 `_<TargetMeshName>` 접미사가 붙으며 원본 Animation Sequence와 같은 폴더에 생성됩니다.
- 변환 완료 후 `원본/대상 비교 열기`를 누르면 사용된 IK Retargeter가 열립니다. Asset Browser에서 원본 애니메이션을 재생하면 Source와 Target 캐릭터의 자세, 루트 이동, 손발 위치를 같은 시간축에서 나란히 확인할 수 있습니다.
- 자동 스켈레톤 분석 준비도가 70% 미만이면 해당 Source 그룹은 중단됩니다. 이 경우 메인 패널에서 수동 Bone Override를 검수해야 합니다.
- Source Skeleton에 Preview Mesh가 지정되어 있지 않은 애니메이션은 자동 리타기팅 대상에서 제외됩니다.
- Preview Mesh가 비어 있으면 프로젝트 Asset Registry에서 같은 Skeleton을 사용하는 Skeletal Mesh를 자동으로 찾아 Source로 사용합니다.
- ALS 포즈처럼 대상과 이미 같은 Skeleton인 Animation Sequence는 실패/건너뜀 대신 `직접 호환`으로 표시하며 별도 리타기팅 복제본을 만들지 않습니다.
- Forest Golem의 `L-Forearm` 계열과 Polygonal Golem의 `RigLArm1/RigLLeg1` 계열처럼 비표준 휴머노이드 명칭을 Semantic 역할로 인식합니다.
- 초보자용 화면은 `선택 → 적용 범위 → 생성/비교` 순서로 표시하며 IK Rig/Profile은 내부에서 처리합니다.
- `전체 몸`, `상체만`, `하체만`을 선택할 수 있고 공격·조준 계열 이름은 `자동 추천`에서 상체 모드를 제안합니다.
- 상체/하체 모드는 리타기팅된 Animation Sequence와 함께 `UpperBody` 또는 `LowerBody` 슬롯 Montage를 생성합니다.
- 슬롯 Montage가 실제로 부분 합성되려면 대상 AnimBP에 해당 Slot과 Layered Blend Per Bone 구성이 있어야 합니다. 없는 경우 Montage는 생성되지만 전체 포즈처럼 보일 수 있습니다.
- UE 5.7 Auto Characterizer를 1순위로 사용해 UE4/UE5 Mannequin, MetaHuman, Mixamo 및 알려진 상용 휴머노이드 계층을 표준 체인으로 변환합니다.
- 엔진 템플릿에 없는 구조는 이름·계층·공간 위치를 결합한 Semantic Analyzer로 재시도합니다.
- Source/Target의 A-Pose와 T-Pose가 다르면 생성된 비교용 IK Retargeter에서 필요한 팔·다리 체인만 보정합니다. UE 5.7의 전체 본 자동 정렬은 비표준 부분 매핑에서 크래시할 수 있어 호출하지 않습니다.
- “어떤 본 구조든”은 휴머노이드의 Root/Pelvis/척추/양팔/양다리가 연결된 계층이라는 전제입니다. 누락 본, 분리된 계층, 비정상 레퍼런스 포즈는 자동 변환 대신 검수 대상으로 차단합니다.

## Shot 단위 검토

- Level Sequence를 생성한 뒤 `이전 Shot`과 `다음 Shot`으로 검토 구간을 선택할 수 있습니다.
- `현재 Shot 구간 재생`은 Sequencer 재생 헤드를 Shot 시작 프레임으로 옮기고 해당 Shot 종료 프레임까지만 재생합니다.
- 선택 구간도 Sequencer Selection Range로 표시되므로 타이밍 확인이 쉽습니다.
- 카메라 거리·높이·좌우 오프셋·위치·회전·초점거리·길이는 Shot Plan JSON에서 수정한 뒤 다시 검증하고 Sequence를 재생성합니다.
- JSON을 다시 검증하면 이전 Sequence와 Plan의 프레임 불일치를 막기 위해 기존 미리보기 연결을 해제합니다.

## 확장 자동화 기능

- Sequencer에서 수동으로 움직인 현재 Shot 카메라의 위치·회전·초점거리를 다시 Shot Plan JSON에 기록할 수 있습니다. Subject 상대 거리·높이·측면 오프셋도 역산하므로 재생성 후 구도를 보존합니다.
- Shot JSON은 `speaker`, `dialogue`, `lip_sync_animation_id`를 지원합니다. 대사는 `[SUB]` Sequencer Marked Frame으로, 립싱크는 `UPT Lip Sync` Skeletal Animation Track으로 생성됩니다.
- `구도 위험 분석`은 피사체 화면 점유율, 헤드룸 가능성, 측면 오프셋과 Visibility 차폐를 Shot별 0~100 점수로 보고합니다.
- Skeleton Profile에는 팔꿈치·무릎용 Joint Limit Preset이 생성됩니다. 본 축 확인 전에는 비활성 상태이며, 활성화된 프리셋만 Preferred Angle과 FBIK Limit으로 적용됩니다.
- 선택한 Movie Graph Config를 사용자 설정에 저장하고 마지막 생성 Sequence와 함께 Movie Render Queue Job으로 추가할 수 있습니다. 렌더 실행은 출력 설정 확인 후 사용자가 수행합니다.

## 추가 고도화 후보

현재 계획했던 자동화 범위는 모두 구현되었습니다. 이후 프로젝트별 선택 기능으로는 런타임 UMG 자막 렌더러, 음소 기반 얼굴 커브 생성, 실제 렌더 결과를 이용한 비전 품질 검사 등을 확장할 수 있습니다.
