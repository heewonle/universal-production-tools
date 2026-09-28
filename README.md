# Universal Production Tools

레퍼런스 영상의 **카메라 구도를 언리얼 시퀀스로 재현하는** 에디터 플러그인입니다. (UE 5.7)

영상을 넣으면 샷을 나누고, 각 샷의 인물 위치·크기·앵글을 읽어, 레벨에 있는 캐릭터로 같은 구도의
Level Sequence를 만듭니다. LLM 없이 로컬 포즈 분석(YOLOX + RTMPose)만으로 동작합니다.

[English](README.en.md) · [한 장 요약](Docs/PORTFOLIO.md)

## 이런 걸 잡습니다

어깨 너머(OTS) 샷. 솔버는 **두 경우 모두 "성공"이라고 보고**했지만, 왼쪽은 앞사람이 프레임 아래로
빠져 있었습니다. 가로 위치만 검사하고 세로는 전혀 보지 않았기 때문입니다.

| 수정 전 — 우하단 구석에 덩어리만 | 수정 후 — 어깨가 오른쪽 가장자리에 걸림 |
|---|---|
| ![수정 전](Docs/images/ots_before.jpg) | ![수정 후](Docs/images/ots_after.jpg) |

같은 코드로 반대쪽을 지정한 결과. 앞사람의 머리·어깨가 왼쪽에 걸리고 주 피사체 얼굴은 가려지지 않습니다.

![왼쪽 지정](Docs/images/ots_after_left.jpg)

이 문제를 사람 눈이 아니라 자동으로 잡기 위해, 생성된 샷을 다시 촬영해 측정합니다.
초록 X는 레퍼런스가 요구한 위치, 빨강 +는 실제로 측정된 위치입니다.

![검증 오버레이](Docs/images/verify_overlay.jpg)

## 무엇이 다른가 — 눈이 아니라 숫자로 검증합니다

이 저장소의 핵심은 기능 목록이 아니라 **정확도를 재는 장치**입니다.

생성된 시퀀스를 같은 포즈 분석기로 다시 촬영·측정해 레퍼런스와 비교하고, 그 수치를 회귀 검사로 잠급니다.
엔진 안에서 카메라와 뼈 위치를 알고 있으므로 "정답"을 직접 만들 수 있다는 점을 활용했습니다.

```
Tools/eval/
  editor/upt_capture_*.py   에디터에서 정답 데이터 촬영 (뼈 위치·화면 투영값을 기록)
  eval_*.py                 정답 대비 오차 측정
  run_regression.py         전체 스위트 실행 + baseline.json 기준 비교
```

현재 **11개 스위트**가 돌아갑니다. 예시:

| 스위트 | 무엇을 지키는가 | 현재 값 |
|---|---|---|
| `framing_video` | 샷 분할·인물 위치 오차 | 가로 오차 ≤ 0.02 |
| `twoshot_video` | 두 인물 배치·어깨 너머 판정 | 오검출 0 |
| `letterbox_video` | 레터박스·자막이 있는 세로 영상 | 20샷 검출 |
| `body_pose` | 영상 관절 ↔ 실제 뼈 오차 | 화면 오차 2.7% |
| `body_height` | 전신 키 추정 오차 (두 체형) | 중앙 11.6% |

## 이 접근으로 실제로 잡은 것들

측정 장치가 없었으면 그냥 넘어갔을 문제들입니다. 자세한 기록은 [Docs](Docs)에 있습니다.

- **어깨 너머(OTS) 샷이 가로만 맞고 세로로 프레임을 벗어났다** — 솔버가 "성공"이라고 보고하는데도
  실제 화면엔 앞사람이 없었습니다. 가로 위치만 검사하고 세로는 전혀 보지 않았기 때문입니다.
  → [OTS_FRAMING_FIX.md](Docs/OTS_FRAMING_FIX.md)
- **검증기가 잘린 인물의 위치를 9%나 틀리게 쟀다** — 프레임에 잘린 몸의 축을 연장해 조준점을 잡으면
  기울어 선 인물에서 옆으로 밀립니다. 세 가지 측정 규칙을 정답에 대고 채점해 가장 나은 것을 골랐습니다.
- **어깨 너머 자동 검증이 원리적으로 불가능했다** — 인물 검출기도 분할 모델도 카메라에 아주 가까운
  마네킹 어깨를 사람으로 보지 못합니다(둘 다 0개 검출). 앞사람만 숨기고 한 장 더 찍어 차이를 보는
  방식으로 바꾸니 모델 없이 정확히 판정됩니다.
- **영상에서 뽑은 3D 관절의 깊이는 쓸 수 없었다** — 깊이 상관 0.215, 오른손은 −0.35(반대로 움직임),
  양발은 0.0. 어느 발이 앞인지도 구분 못 합니다. 그래서 **애니메이션 굽기는 중단했습니다.**
  → [BODY_MOTION_FROM_VIDEO.md](Docs/BODY_MOTION_FROM_VIDEO.md)

  아래는 영상에서 추정한 관절(빨강)과 엔진이 아는 실제 뼈 위치(초록)를 겹친 것입니다.
  2D는 몸 높이 대비 7.2%로 쓸 만하지만, 깊이는 잡음이었습니다.

  ![관절 비교](Docs/images/bodypose_overlay.jpg)

측정 없이 고쳤다가 되돌린 것도 기록해 두었습니다. 크기 점수를 "정확한 기하값"으로 바꿨더니 오히려
나빠졌고(레퍼런스 쪽도 같은 추정기로 재기 때문), 체형 보정 한계를 넓힌 개선폭은 재촬영에서 재현되지
않았습니다. → [BODY_HEIGHT_ESTIMATOR.md](Docs/BODY_HEIGHT_ESTIMATOR.md)

## 구성

```
Source/UniversalProductionTools/   C++ (Slate 패널, 구도 솔버, 시퀀스 생성, 자동화 테스트)
Tools/                             파이썬 분석기 (포즈 분석, 링크 다운로드, 라이브러리 검색, 검증)
Tools/eval/                        정답 촬영 + 오차 측정 + 회귀 검사
Docs/                              작업 기록 (CH5_Project/Docs 사본)
MetaHumanCapture58/Scripts/        UE 5.8 MetaHuman 얼굴 캡처 스크립트
```

## 설치

### 1. 플러그인

프로젝트의 `Plugins/` 아래에 두고 에디터를 빌드합니다.

```bash
"C:/Program Files/Epic Games/UE_5.7/Engine/Build/BatchFiles/Build.bat" \
  <프로젝트>Editor Win64 Development -Project="<경로>/<프로젝트>.uproject" -WaitMutex
```

### 2. 분석기 환경

```powershell
Tools\setup_analyzer_env.ps1
```

`ThirdParty/UPTAnalyzer/.venv`에 파이썬 환경을 만들고 `requirements-pose-analyzer.txt`를 설치합니다.
포즈 모델(ONNX)은 첫 실행 때 자동으로 내려받습니다(저장소에는 포함하지 않습니다).

### 3. 확인

```bash
cd Tools/eval
../../../../ThirdParty/UPTAnalyzer/.venv/Scripts/python.exe run_regression.py
```

정답 데이터(`Saved/UniversalProductionTools/GroundTruth/`)가 없는 스위트는 건너뜁니다.
정답은 `Tools/eval/editor/upt_capture_*.py`를 에디터에서 실행해 만듭니다.

## 쓰는 법

에디터에서 `Window > Universal Production Tools`를 엽니다.

1. 레퍼런스 영상 파일을 고르거나 링크를 붙여넣습니다(구간 지정 가능).
2. 레벨에서 등장인물 Actor를 선택해 가져옵니다.
3. 분석 → 역할 매핑 확인 → Level Sequence 생성.
4. 생성 후 샷 목록에서 개별 샷의 크기·앵글·어깨 너머를 고쳐 같은 시퀀스에 다시 적용합니다.

패널 없이 같은 파이프라인을 돌리는 콘솔 명령도 있습니다.

```
UPT.ReferenceE2E "<reference_plan.json>" <ActorLabel...> [-editshot=N] [-link=URL -section=A-B -rights]
```

기능 전체 목록과 LLM 경로, 스켈레톤 분석·리타기팅 기능은 [PLUGIN_FEATURES.md](Docs/PLUGIN_FEATURES.md)를 보세요.

## 알려진 한계

- **얼굴 없는 마네킹**으로는 클로즈업 구도 점수가 낮게 나옵니다. 실사 인물의 익스트림 클로즈업을
  재현하는 것은 원리상 불가능합니다(20샷 쇼츠 세트에서 종합 23점).
- **흰 마네킹의 얼굴 근접**은 인물 검출 자체가 실패합니다(126프레임 중 30프레임).
- **몸 동작 추출은 중단** 상태입니다. 위 깊이 문제 때문입니다. MetaHumanBodyTracker 플러그인을
  구하면 5.8 경로가 정공법입니다.
- 정답 데이터는 캐릭터 2종(갑옷 캐릭터, 표준 마네킹)뿐입니다. 체형이 더 다양해지면 보정 상수를
  다시 확인해야 합니다.

## 라이선스

개인 프로젝트입니다. 별도 라이선스를 명시하기 전까지 모든 권리는 저자에게 있습니다.
