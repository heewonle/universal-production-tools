#include "UPTRenderMatchReport.h"

#include "Serialization/JsonSerializer.h"

bool FUPTRenderMatchReportParser::Parse(const FString& JsonText, FUPTRenderMatchReport& OutReport, FString& OutError)
{
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), Root) || !Root.IsValid())
    { OutError = TEXT("렌더 비교 JSON 형식이 올바르지 않습니다."); return false; }
    double OverallScore = -1.0;
    if (!Root->TryGetNumberField(TEXT("overall_score"), OverallScore) || OverallScore < 0.0 || OverallScore > 100.0)
    { OutError = TEXT("overall_score는 0~100 숫자여야 합니다."); return false; }
    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (!Root->TryGetArrayField(TEXT("shots"), Shots) || !Shots || Shots->IsEmpty())
    { OutError = TEXT("비어 있지 않은 shots 배열이 필요합니다."); return false; }
    static const TCHAR* ScoreFields[] = { TEXT("composition_score"), TEXT("subject_position_score"), TEXT("shot_size_score"), TEXT("camera_angle_score") };
    for (int32 Index = 0; Index < Shots->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> Shot = (*Shots)[Index]->AsObject();
        if (!Shot.IsValid()) { OutError = FString::Printf(TEXT("Shot %d가 객체가 아닙니다."), Index + 1); return false; }
        for (const TCHAR* Field : ScoreFields)
        {
            double Score = -1.0;
            if (!Shot->TryGetNumberField(Field, Score) || Score < 0.0 || Score > 100.0)
            { OutError = FString::Printf(TEXT("Shot %d의 %s는 0~100 숫자여야 합니다."), Index + 1, Field); return false; }
        }
    }
    OutReport = FUPTRenderMatchReport();
    OutReport.OverallScore = OverallScore;
    Root->TryGetStringField(TEXT("summary"), OutReport.Summary);
    OutReport.ShotCount = Shots->Num();
    return true;
}
