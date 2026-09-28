# MetaHuman 캡처 전용 프로젝트 (UE 5.8)

일반 영상에서 MetaHuman **얼굴 애니메이션**을 뽑아 5.7 팀 프로젝트로 가져오기 위한 별도 프로젝트입니다.
팀 프로젝트(`GoddessSlot/Retrieve.uproject`)는 **5.7 그대로 둡니다**.

- 위치: `<CAPTURE58>`
- 팀 저장소 폴더 바깥이라 커밋에 섞이지 않습니다. C++ 없는 블루프린트 프로젝트라 빌드가 필요 없습니다.
- 2026-09-20 실제로 영상 2개를 끝까지 돌려 확인한 내용입니다.

## 왜 프로젝트를 나눴나

- **5.8에서 저장한 에셋은 5.7에서 열리지 않습니다.** 팀 프로젝트를 올리면 팀원 모두가 5.8로 올려야 합니다.
- 팀 프로젝트의 C++ 플러그인(`ALS-Refactored`, `Sidekick..._UE57`, `Monolith`, `UniversalProductionTools`)을 전부 다시 빌드해야 합니다.
- 캡처 프로젝트에서는 **애니메이션 데이터만** 만들어 5.7로 넘깁니다.

## 켜 둔 플러그인

UE 5.8은 MetaHuman 플러그인을 엔진에 포함하고 있어 따로 받을 필요가 없습니다.

| 플러그인 | 역할 |
|---|---|
| **MetaHuman Animator** (`MetaHuman`) | 핵심. Performance 솔브 |
| **MetaHuman Animation Tools** | 애니메이션 데이터 저장·불러오기 |
| **MetaHuman SDK** | MetaHuman 에셋 가져오기 |
| **Python Script Plugin**, **Editor Scripting Utilities** | 자동화 스크립트 |

## 중요: 일반 영상은 Capture Manager를 쓰지 않는다

Capture Manager가 받는 소스는 **iPhone Live Link Face**와 **HMC(헤드캠) 아카이브**뿐입니다. 일반 mp4를 넣는 항목이 없습니다.

대신 Performance의 **Monocular Footage(모노 영상)** 입력을 씁니다. 이 경로는
- **MetaHuman Identity가 필요 없고**(Identity는 깊이 정보가 있는 푸티지일 때만 필요),
- Epic 계정 오토리깅도 건너뜁니다.
- Capture Data는 깊이 없이 **이미지 시퀀스만으로** 만들 수 있습니다.

## 작업 순서

```
영상 → (ffmpeg) 이미지 시퀀스 → Capture Data → Performance(모노) 처리 → 애니메이션 내보내기 → 5.7로 반입
```

### 1. 프레임 뽑기 (언리얼 밖, 5.7 분석 가상환경의 Python)

```bash
cd <PROJECT>
ThirdParty/UPTAnalyzer/.venv/Scripts/python.exe ../MetaHumanCapture58/Scripts/extract_frames.py --video D:/clip.mp4 --name MyShot --start 5 --end 13
```

세로 쇼츠의 검은 띠·고정 자막은 자동으로 잘라냅니다(5.7 플러그인 분석기의 `detect_active_area` 재사용).

### 2. Capture Data 에셋 만들기

```bash
UPT_FOOTAGE=MyShot "<UE58>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "<CAPTURE58>/MetaHumanCapture58.uproject" -ExecutePythonScript="<CAPTURE58>/Scripts/create_capture_data.py" -unattended -nullrhi -nosplash
```

### 3. 처리하기 (GPU 필요, GUI 에디터로 실행)

```bash
UPT_CAPTURE=CD_MyShot "<UE58>/Engine/Binaries/Win64/UnrealEditor.exe" "<CAPTURE58>/MetaHumanCapture58.uproject" -ExecCmds="py <CAPTURE58>/Scripts/run_mono_performance.py" -nosplash
```

### 4. 결과 확인

```bash
UPT_CAPTURE=CD_MyShot ...UnrealEditor-Cmd.exe ... -ExecutePythonScript=".../Scripts/check_animation.py" -unattended -nullrhi -nosplash
```

처리 프레임 수와, 값이 실제로 움직이는 컨트롤 개수를 찍어 줍니다.

## 막혔던 곳 (같은 문제를 다시 만나지 않도록)

1. **프레임 파일 이름**: 엔진은 `이름 + 구분자(밑줄·공백·하이픈) + 숫자.확장자`만 시퀀스로 인식합니다.
   `frame.0000.png`처럼 **점으로 구분하면 0프레임**으로 보고 파이프라인이 **오류 없이** 즉시 끝납니다. `frame_0000.png`를 쓰세요.
2. **Capture Data의 프레임레이트**: `metadata.frame_rate`(double)를 넣지 않으면 Performance가 "frame rate is zero"로 거부합니다.
3. **모노 경로는 비동기**: 블로킹 처리 설정은 깊이 푸티지 분기 전용입니다. 엔진 기본 예제(`process_monocular_performance.py`)는 완료를 기다리지도, 에셋을 저장하지도 않아 헤드리스에서 결과가 사라집니다.
   `Scripts/run_mono_performance.py`는 틱 콜백으로 완료를 기다렸다가 저장합니다.
4. **실행 방식**: `-ExecutePythonScript`는 스크립트가 끝나면 에디터를 닫아 비동기 처리를 기다릴 수 없습니다. 처리는 `-ExecCmds="py <경로>"`로 실행하세요(경로에 공백이 없어야 합니다).
5. 처리에는 **DX12 GPU**가 필요합니다(`-nullrhi`로는 안 됨).

## 확인된 결과 (2026-09-20)

| 영상 | 처리 | 값이 움직이는 컨트롤 |
|---|---|---|
| 드라마 클로즈업 608×624, 8초 | 191/191 프레임 | 251개 중 122개 (입술·입 모양) |
| 인터뷰 전신 1296×647, 8초 | 193/193 프레임 | 251개 중 128개 (눈 깜빡임·눈썹) |

## 아직 안 되는 것

- **몸 동작**: 로그에 `MetaHumanBodyTracker plugin not found`. 몸 추적은 엔진에 없는 **별도 플러그인**입니다.
  데이터 구조에는 몸 필드(`body_animation_data`, `raw_body_animation_smplx_*`)가 있으므로 플러그인만 구하면 됩니다.
  구하지 못하면 5.7의 RTMPose 기반 2D 포즈 분석으로 몸을 다루는 편이 현실적입니다.
- **5.7로 반입**: 애니메이션 내보내기에 `/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton`이 필요합니다.
  Fab/Bridge로 MetaHuman 캐릭터를 하나 가져와야 합니다.

## 주의

- 얼굴 캡처는 원래 아이폰 촬영(깊이 정보)을 전제로 만든 기능이라, 일반 영상은 품질이 떨어질 수 있습니다.
- **5.8 프로젝트의 에셋을 5.7로 그대로 복사하면 안 됩니다.** 애니메이션은 FBX 내보내기가 안전합니다.
- 캡처 프로젝트는 팀 저장소에 올리지 않습니다.

## 관련

카메라 구도 자동 생성(레퍼런스 영상 → Level Sequence)은 5.7 팀 프로젝트의 `UniversalProductionTools`가 담당합니다. 이 프로젝트는 **동작·표정 애니메이션**만 만듭니다.
