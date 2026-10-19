# UI 언어 — 원문을 영어로 옮기고 한국어를 번역으로 되돌려 넣었다 (2026-10-19)

## 왜 문제였나

패널의 모든 문자열이 **원문부터 한국어**였다.

```cpp
LOCTEXT("SizeCU", "클로즈업")
```

UE는 번역이 없으면 원문을 그대로 쓴다. 즉 영어 로캘에서도 한국어가 그대로 보인다.
이것이 제품의 시장을 국내로 묶는 **유일한 구조적 이유**였다(사업화 점검표의 2순위).

반대로 원문을 영어로 바꾸기만 하면, 이번에는 **한국어 사용자가 영어 UI를 보게 된다.**
그래서 둘을 같이 했다 — 원문을 영어로 옮기고, 원래 한국어를 **번역(ko)으로 되돌려 넣었다.**

## 한 일

| | 전 | 후 |
|---|---|---|
| LOCTEXT 원문 | 한국어 120개 | **영어 123개 전부** |
| ko 번역 | 없음 | **128개** (`Content/Localization/UPT/ko/UPT.po` → `.locres`) |
| 영어 로캘 | 한국어가 보임 | 영어 |
| 한국어 로캘 | 한국어 | 한국어(번역으로) |

수집·번역·컴파일은 엔진의 표준 경로(`GatherText` 커맨들렛)를 쓴다. 설정은
`Config/Localization/UPT.ini`이고, 번역 원본은 `Tools/localization/ko_translations.json`에
남겨 두었다 — `.po`가 지워져도 다시 만들 수 있다.

```bash
# 수집 → (번역 채우기) → import → compile
UnrealEditor-Cmd.exe <프로젝트>.uproject -run=GatherText \
  -config="Plugins/UniversalProductionTools/Config/Localization/UPT.ini" -unattended -nosplash
```

`.uplugin`에 `LocalizationTargets`를 등록해야 에디터가 `.locres`를 읽는다. 이것이 빠지면
빌드도 되고 테스트도 통과하는데 **번역만 조용히 사라진다.**

## 두 번 걸렸다

1. **경로는 프로젝트 루트 기준이다.** 플러그인 안에 둔 config라고 플러그인 기준이 되지 않는다.
   그대로 돌렸더니 프로젝트 전체 소스 **719개 파일**을 긁어 결과를 프로젝트 `Content`에 떨궜다.
   플러그인으로 좁히니 55개 파일이 됐다.
2. **일괄 치환이 네 항목을 조용히 빠뜨렸다.** 이스케이프된 따옴표(`\"`)가 있거나 여러 줄로
   이어 붙인 LOCTEXT라 정규식이 잡지 못했다. `.po`에서 "번역 없음"으로 드러나 찾았다 —
   **빠뜨린 것을 세는 쪽이 있어서** 알아챘다.

## 조용히 깨지는 종류라, 재서 지킨다

이 전환은 실패해도 빌드가 되고 기존 테스트도 전부 통과한다. 어느 날 한국어 사용자가 영어 UI를
보게 될 뿐이다. 그래서 컬처를 바꿔 가며 직접 확인하는 테스트를 넣었다
(`UniversalProductionTools.Localization.KoreanTranslations`).

```
ko 컬처 → SUPTMainPanel/SizeCU == "클로즈업"
          SUPTMainPanel/Step3  == "③ 생성"
          UPTAnimationRetargetWindow/Validate == "② 사전 점검"
en 컬처 → 같은 키가 "Close-up" / "(3) Generate" / "(2) Pre-flight check"
```

테스트는 끝나면 에디터 언어를 원래대로 되돌린다.

## 아직 한국어인 것

패널의 **상태 메시지 70곳**은 아직 한국어다(`SetStatus(TEXT("..."))`, `FString::Printf`).
이들은 `FText`가 아니라 `FString`이라 번역 대상에 들어가지 않는다. 다음 차례다.
로그(`UE_LOG`)는 개발자용이라 그대로 둔다.
