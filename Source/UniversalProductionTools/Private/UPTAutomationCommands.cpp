#include "UPTBlockingValidator.h"
#include "UPTFraming.h"
#include "UPTPlanParser.h"
#include "UPTPoseReferenceAnalyzer.h"
#include "UPTReferenceBlockingSolver.h"
#include "UPTReferencePlanParser.h"
#include "UPTSequenceBuilder.h"

#include "Animation/AnimSequenceBase.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "LevelSequence.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "MovieSceneTimeHelpers.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundBase.h"
#include "Tracks/MovieSceneCameraCutTrack.h"

DEFINE_LOG_CATEGORY_STATIC(LogUPTAutomation, Log, All);

namespace
{
struct FE2EOptions
{
    int32 MaxRefine = 0;
    int32 EditShot = 0;
    FString Link;
    FString Section;
    bool bRightsConfirmed = false;
    FString Library;
    FString PromptFile;
};

bool FindActors(UWorld* World, const TArray<FString>& Labels, TMap<FString, TWeakObjectPtr<AActor>>& OutActors)
{
    for (const FString& Label : Labels)
    {
        for (TActorIterator<AActor> It(World); World && It; ++It)
        {
            if (It->GetActorLabel() == Label)
            {
                OutActors.Add(Label, *It);
                break;
            }
        }
    }
    return !Labels.IsEmpty() && OutActors.Num() == Labels.Num();
}

// 시퀀스가 샷 수와 맞는 카메라 컷·바인딩을 갖는지(그 자리 갱신 때 쌓이지 않는지) 로그로 남긴다.
void LogSequenceShape(const TCHAR* Stage, ULevelSequence* Sequence, const int32 ShotCount)
{
    UMovieScene* Scene = Sequence ? Sequence->GetMovieScene() : nullptr;
    if (!Scene)
    {
        UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_SHAPE %s: 시퀀스 없음"), Stage);
        return;
    }
    const UMovieSceneCameraCutTrack* CutTrack = Cast<UMovieSceneCameraCutTrack>(Scene->GetCameraCutTrack());
    UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_SHAPE %s sequence=%s cuts=%d shots=%d spawnables=%d possessables=%d bindings=%d tracks=%d end_tick=%d"),
        Stage, *Sequence->GetPathName(), CutTrack ? CutTrack->GetAllSections().Num() : -1, ShotCount, Scene->GetSpawnableCount(),
        Scene->GetPossessableCount(), static_cast<const UMovieScene*>(Scene)->GetBindings().Num(), Scene->GetTracks().Num(), UE::MovieScene::DiscreteExclusiveUpper(Scene->GetPlaybackRange()).Value);
}

// 패널의 '선택한 샷 수정'과 같은 계산으로 샷 하나를 바꾸고, 같은 시퀀스를 두 번 연속 그 자리에서 다시 채운다.
void RunEditShotCheck(FUPTCinematicPlan Plan, const TMap<FString, TWeakObjectPtr<AActor>>& Actors, ULevelSequence* Sequence, const int32 EditShot)
{
    if (!Plan.Shots.IsValidIndex(EditShot - 1))
    {
        UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_EDIT_ERROR shot %d가 없습니다(샷 %d개)"), EditShot, Plan.Shots.Num());
        return;
    }
    LogSequenceShape(TEXT("generated"), Sequence, Plan.Shots.Num());
    FUPTCinematicShot& Shot = Plan.Shots[EditShot - 1];
    const TWeakObjectPtr<AActor>* SubjectPtr = Actors.Find(Shot.Subject);
    const FBox Bounds = UPTFraming::GetSubjectBounds(SubjectPtr ? SubjectPtr->Get() : nullptr);
    const float SubjectHeight = Bounds.IsValid ? static_cast<float>(Bounds.GetSize().Z) : 180.0f;
    const FString BeforeLabel = FUPTReferenceBlockingSolver::InferShotSize(Shot) + TEXT("/") + FUPTReferenceBlockingSolver::InferCameraAngle(Shot);
    FUPTReferenceBlockingSolver::ApplyShotFraming(Shot, TEXT("medium"), TEXT("low"), SubjectHeight,
        FUPTReferenceBlockingSolver::DefaultScreenHeightFraction(TEXT("medium")), Plan.AspectRatio);
    const float Radius = Shot.DistanceCm;
    Shot.DistanceCm = Radius * FMath::Cos(FMath::DegreesToRadians(30.0f));
    Shot.SideCm = Radius * FMath::Sin(FMath::DegreesToRadians(30.0f));
    Shot.DurationSeconds += 0.5f;
    UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_EDIT shot=%d before=%s after=%s/%s azimuth=%.1f duration=%.2f"), EditShot, *BeforeLabel,
        *FUPTReferenceBlockingSolver::InferShotSize(Shot), *FUPTReferenceBlockingSolver::InferCameraAngle(Shot),
        FMath::RadiansToDegrees(FMath::Atan2(Shot.SideCm, Shot.DistanceCm)), Shot.DurationSeconds);

    const TMap<FString, TWeakObjectPtr<UAnimSequenceBase>> NoAnimations;
    const TMap<FString, TWeakObjectPtr<USoundBase>> NoSounds;
    for (int32 Pass = 1; Pass <= 2; ++Pass)
    {
        FString Error;
        ULevelSequence* Rebuilt = FUPTSequenceBuilder::Build(Plan, Actors, NoAnimations, NoSounds, Error, Sequence);
        if (!Rebuilt)
        {
            UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_EDIT_ERROR pass %d: %s"), Pass, *Error);
            return;
        }
        UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_EDIT pass=%d same_asset=%d"), Pass, Rebuilt == Sequence ? 1 : 0);
        LogSequenceShape(Pass == 1 ? TEXT("edited") : TEXT("edited_again"), Rebuilt, Plan.Shots.Num());
    }
}

// 레퍼런스 분석 결과 → 역할 배정·카메라 계산 → 구도 검증·자동 보정 → Level Sequence 생성(→ 선택 시 샷 수정 검사)
void RunPipeline(const FUPTReferencePlan& Reference, const FString& ReferenceFile, const TMap<FString, TWeakObjectPtr<AActor>>& Actors, const FE2EOptions& Options)
{
    FUPTCinematicPlan Plan;
    FString SolverReport;
    if (!FUPTReferenceBlockingSolver::BuildPlan(Reference, Actors, {}, Plan, SolverReport))
    {
        UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_ERROR blocking: %s"), *SolverReport);
        return;
    }
    Plan.Title = TEXT("UPT_E2E_") + Reference.Title;
    TArray<FString> ReportLines;
    SolverReport.ParseIntoArrayLines(ReportLines);
    for (const FString& Line : ReportLines) UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_BLOCKING %s"), *Line);
    const FUPTBlockingValidationResult Validation = FUPTBlockingValidator::Validate(Plan);
    if (!Validation.IsValid())
    {
        UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_ERROR validation: %s"), *Validation.ToText());
        return;
    }

    const FString PlanFile = FPaths::Combine(FPaths::GetPath(ReferenceFile), FString::Printf(TEXT("e2e_plan_%s.json"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
    FFileHelper::SaveStringToFile(FUPTPlanParser::ToJson(Plan), *PlanFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_PLAN=%s"), *PlanFile);

    FUPTPoseReferenceAnalyzer::RefineShotCameras(Plan, Actors, ReferenceFile, Options.MaxRefine,
        FUPTPoseRefineProgress::CreateLambda([](const int32 Pass, const double Score)
        {
            UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_PASS[%d]=%.1f"), Pass, Score);
        }),
        FUPTPoseRefineComplete::CreateLambda([Actors, PlanFile, Options](const bool bSuccess, const FUPTCinematicPlan& BestPlan, const FString& Result)
        {
            if (!bSuccess)
            {
                UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_ERROR verify: %s"), *Result);
                return;
            }
            const FString BestPlanFile = FPaths::ChangeExtension(PlanFile, TEXT("")) + TEXT("_refined.json");
            FFileHelper::SaveStringToFile(FUPTPlanParser::ToJson(BestPlan), *BestPlanFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

            FString BuildError;
            const TMap<FString, TWeakObjectPtr<UAnimSequenceBase>> NoAnimations;
            const TMap<FString, TWeakObjectPtr<USoundBase>> NoSounds;
            ULevelSequence* Sequence = FUPTSequenceBuilder::Build(BestPlan, Actors, NoAnimations, NoSounds, BuildError);
            if (Sequence)
            {
                UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_SEQUENCE=%s"), *Sequence->GetPathName());
            }
            else
            {
                UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_ERROR sequence: %s"), *BuildError);
            }

            double VerifyScore = -1.0;
            const FString Text = FUPTPoseReferenceAnalyzer::FormatVerifyReport(Result, VerifyScore);
            UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_SCORE=%.1f"), VerifyScore);
            TArray<FString> Lines;
            Text.ParseIntoArrayLines(Lines);
            for (const FString& Line : Lines) UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_DETAIL %s"), *Line);

            if (Sequence && Options.EditShot > 0) RunEditShotCheck(BestPlan, Actors, Sequence, Options.EditShot);
            UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_DONE"));
        }));
}

bool LoadReferencePlan(const FString& File, FUPTReferencePlan& OutPlan)
{
    FString Json;
    FString Error;
    if (!FFileHelper::LoadFileToString(Json, *File) || !FUPTReferencePlanParser::Parse(Json, OutPlan, Error))
    {
        UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_ERROR reference plan could not be loaded: %s %s"), *File, *Error);
        return false;
    }
    return true;
}

// 패널 조작 없이 패널과 같은 C++ 경로를 실행하고 결과를 Output Log의 UPT_E2E_* 줄로 남긴다.
// 사용 예:
//   UPT.ReferenceE2E C:/.../reference_plan.json AutoHero -refine=2 -editshot=4
//   UPT.ReferenceE2E AutoHero -link=https://... -section=1:00-2:00 [-rights] -refine=1
//   UPT.ReferenceE2E AutoHero -library=D:/RefVideos -promptfile=D:/prompt.txt -refine=1
void RunReferenceE2E(const TArray<FString>& InArgs, UWorld* World)
{
    FE2EOptions Options;
    TArray<FString> Positional;
    for (const FString& Arg : InArgs)
    {
        if (Arg.StartsWith(TEXT("-refine="))) Options.MaxRefine = FMath::Clamp(FCString::Atoi(*Arg.RightChop(8)), 0, 5);
        else if (Arg.StartsWith(TEXT("-editshot="))) Options.EditShot = FMath::Max(0, FCString::Atoi(*Arg.RightChop(10)));
        else if (Arg.StartsWith(TEXT("-link="))) Options.Link = Arg.RightChop(6);
        else if (Arg.StartsWith(TEXT("-section="))) Options.Section = Arg.RightChop(9);
        else if (Arg == TEXT("-rights")) Options.bRightsConfirmed = true;
        else if (Arg.StartsWith(TEXT("-library="))) Options.Library = Arg.RightChop(9);
        else if (Arg.StartsWith(TEXT("-promptfile="))) Options.PromptFile = Arg.RightChop(12);
        else Positional.Add(Arg);
    }
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : World;
    const bool bSourceOnly = !Options.Link.IsEmpty() || !Options.Library.IsEmpty();
    TArray<FString> ActorLabels = Positional;
    FString ReferenceFile;
    if (!bSourceOnly && !ActorLabels.IsEmpty())
    {
        ReferenceFile = ActorLabels[0];
        ReferenceFile.TrimQuotesInline();
        ActorLabels.RemoveAt(0);
    }
    TMap<FString, TWeakObjectPtr<AActor>> Actors;
    if (!FindActors(EditorWorld, ActorLabels, Actors))
    {
        UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_ERROR usage: UPT.ReferenceE2E [reference_plan.json] <ActorLabel...> [-refine=N] [-editshot=N] [-link=URL -section=A-B -rights] [-library=DIR -promptfile=FILE] (Actor를 찾지 못함)"));
        return;
    }

    if (!Options.Link.IsEmpty())
    {
        UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_LINK start %s section=%s rights=%d"), *Options.Link, *Options.Section, Options.bRightsConfirmed ? 1 : 0);
        FUPTPoseReferenceAnalyzer::AnalyzeLink(Options.Link, Options.Section, Options.bRightsConfirmed, TEXT("scene"),
            FUPTPoseAnalysisComplete::CreateLambda([Actors, Options](const bool bSuccess, const FUPTPoseAnalysisResult& Result, const FString& Error)
            {
                if (!bSuccess)
                {
                    UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_ERROR link: %s"), *Error);
                    return;
                }
                UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_LINK plan=%s cache=%d"), *Result.PlanFile, Result.bFromCache ? 1 : 0);
                FUPTReferencePlan Reference;
                if (LoadReferencePlan(Result.PlanFile, Reference)) RunPipeline(Reference, Result.PlanFile, Actors, Options);
            }));
        return;
    }

    if (!Options.Library.IsEmpty())
    {
        FString Prompt;
        if (!FFileHelper::LoadFileToString(Prompt, *Options.PromptFile) || Prompt.TrimStartAndEnd().IsEmpty())
        {
            UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_ERROR prompt file could not be read: %s"), *Options.PromptFile);
            return;
        }
        UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_LIBRARY start %s prompt=%s"), *Options.Library, *Prompt.TrimStartAndEnd());
        FUPTPoseReferenceAnalyzer::SearchLibrary(Options.Library, Prompt.TrimStartAndEnd(), 3,
            FUPTLibrarySearchComplete::CreateLambda([Actors, Options](const bool bSuccess, const FString& ResultJson, const FString& Error)
            {
                if (!bSuccess)
                {
                    UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_ERROR library: %s"), *Error);
                    return;
                }
                TSharedPtr<FJsonObject> Root;
                const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
                if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ResultJson), Root) || !Root.IsValid()
                    || !Root->TryGetArrayField(TEXT("results"), Results) || !Results || Results->IsEmpty())
                {
                    UE_LOG(LogUPTAutomation, Error, TEXT("UPT_E2E_ERROR library: 검색 결과 없음"));
                    return;
                }
                for (int32 Index = 0; Index < Results->Num(); ++Index)
                {
                    const TSharedPtr<FJsonObject> Item = (*Results)[Index]->AsObject();
                    if (!Item.IsValid()) continue;
                    FString Video;
                    double SearchScore = 0.0;
                    Item->TryGetStringField(TEXT("video"), Video);
                    Item->TryGetNumberField(TEXT("score"), SearchScore);
                    UE_LOG(LogUPTAutomation, Log, TEXT("UPT_E2E_LIBRARY rank=%d video=%s score=%.1f"), Index + 1, *Video, SearchScore);
                }
                FString PlanFile;
                (*Results)[0]->AsObject()->TryGetStringField(TEXT("reference_plan"), PlanFile);
                FUPTReferencePlan Reference;
                if (LoadReferencePlan(PlanFile, Reference)) RunPipeline(Reference, PlanFile, Actors, Options);
            }));
        return;
    }

    FUPTReferencePlan Reference;
    if (LoadReferencePlan(ReferenceFile, Reference)) RunPipeline(Reference, ReferenceFile, Actors, Options);
}

FAutoConsoleCommandWithWorldAndArgs GUPTReferenceE2ECommand(
    TEXT("UPT.ReferenceE2E"),
    TEXT("Runs the panel pipeline without the panel: [reference_plan.json] <ActorLabel...> [-refine=N] [-editshot=N] [-link=URL -section=A-B -rights] [-library=DIR -promptfile=FILE]"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&RunReferenceE2E));
}
