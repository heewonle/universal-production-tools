#include "Misc/AutomationTest.h"

#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Text.h"

/**
 * UI 원문을 영어로 바꾸고 한국어를 **번역**으로 옮겼다. 그 번역이 실제로 로드되는지 잰다.
 *
 * 이 전환은 조용히 실패할 수 있는 종류다. `.locres`가 빠지거나 `.uplugin`의
 * LocalizationTargets 등록이 없으면 **한국어 사용자가 어느 날 영어 UI를 보게 된다** —
 * 빌드도 되고 테스트도 통과하는데 번역만 사라진다. 그래서 컬처를 바꿔 가며 직접 확인한다.
 *
 * 비교 대상은 패널에서 실제로 쓰는 키다(샷 크기·단계 제목·리타기팅 버튼).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUPTLocalizationTest, "UniversalProductionTools.Localization.KoreanTranslations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUPTLocalizationTest::RunTest(const FString& Parameters)
{
    struct FCase { const TCHAR* Namespace; const TCHAR* Key; const TCHAR* English; const TCHAR* Korean; };
    const FCase Cases[] = {
        {TEXT("SUPTMainPanel"), TEXT("SizeCU"), TEXT("Close-up"), TEXT("클로즈업")},
        {TEXT("SUPTMainPanel"), TEXT("Step3"), TEXT("(3) Generate"), TEXT("③ 생성")},
        {TEXT("SUPTMainPanel"), TEXT("AnimationTab"), TEXT("Universal animation retarget"), TEXT("애니메이션 범용화")},
        {TEXT("UPTAnimationRetargetWindow"), TEXT("Validate"), TEXT("(2) Pre-flight check"), TEXT("② 사전 점검")},
        // 상태 메시지도 본다. 서식이 있는 것은 `FString::Format`으로 옮겼으므로, 번역이 빠지면
        // 버튼 이름이 아니라 **진행·오류 메시지**가 영어로 돌아간다.
        {TEXT("UPTAnimationRetargetWindow"), TEXT("CheckPassed"),
         TEXT("Check passed: {0} usable as-is, {1} source characters need conversion."),
         TEXT("점검 통과: 그대로 사용 가능 {0}개, 변환이 필요한 원본 캐릭터 {1}종.")},
        {TEXT("SUPTMainPanel"), TEXT("ProgBuildSequence"),
         TEXT("[4/4] Building the Level Sequence..."), TEXT("[4/4] Level Sequence를 만드는 중입니다...")},
    };

    FInternationalization& I18N = FInternationalization::Get();
    const FString OriginalCulture = I18N.GetCurrentCulture()->GetName();

    // 원문이 영어인지부터 본다. 원문이 한국어로 돌아가 있으면 영어 사용자가 한국어를 보게 된다.
    bool bHasEnglishSource = true;
    for (const FCase& Case : Cases)
    {
        FText Found;
        if (!FText::FindTextInLiveTable_Advanced(Case.Namespace, Case.Key, Found))
        {
            AddError(FString::Printf(TEXT("%s/%s 를 텍스트 테이블에서 찾지 못했습니다. 모듈이 로드되지 않았거나 키가 바뀐 것입니다."),
                Case.Namespace, Case.Key));
            bHasEnglishSource = false;
        }
    }
    if (!bHasEnglishSource) return false;

    if (!I18N.SetCurrentCulture(TEXT("ko")))
    {
        AddWarning(TEXT("ko 컬처를 설정하지 못해 번역 확인을 건너뜁니다."));
        return true;
    }
    for (const FCase& Case : Cases)
    {
        FText Found;
        if (!FText::FindTextInLiveTable_Advanced(Case.Namespace, Case.Key, Found)) continue;
        TestEqual(FString::Printf(TEXT("ko 번역 %s/%s"), Case.Namespace, Case.Key),
            Found.ToString(), FString(Case.Korean));
    }

    if (I18N.SetCurrentCulture(TEXT("en")))
    {
        for (const FCase& Case : Cases)
        {
            FText Found;
            if (!FText::FindTextInLiveTable_Advanced(Case.Namespace, Case.Key, Found)) continue;
            TestEqual(FString::Printf(TEXT("en 원문 %s/%s"), Case.Namespace, Case.Key),
                Found.ToString(), FString(Case.English));
        }
    }

    // 테스트가 에디터의 언어를 바꿔 놓고 끝나면 안 된다.
    I18N.SetCurrentCulture(OriginalCulture);
    return true;
}
