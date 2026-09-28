#include "UPTPoseReferenceAnalyzer.h"

#include "UPTEndpoint.h"
#include "UPTFraming.h"
#include "UPTSettings.h"

#include "Async/Async.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

#include <atomic>

namespace
{
std::atomic<bool> bPoseAnalysisRunning{false};

FString ResolveProjectPath(const FString& Value)
{
    const FString Trimmed = Value.TrimStartAndEnd();
    return FPaths::IsRelative(Trimmed) ? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Trimmed) : FPaths::ConvertRelativePathToFull(Trimmed);
}

bool IsPureAscii(const FString& Value)
{
    for (const TCHAR Character : Value)
    {
        if (Character > 127) return false;
    }
    return true;
}

FString Quote(const FString& Value)
{
    return TEXT("\"") + Value + TEXT("\"");
}
}

bool FUPTPoseReferenceAnalyzer::IsRunning()
{
    return bPoseAnalysisRunning.load();
}

bool FUPTPoseReferenceAnalyzer::Preflight(FString& OutError)
{
    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
    const FString Python = ResolveProjectPath(Settings->PoseAnalyzerPythonPath);
    const FString Script = ResolveProjectPath(Settings->PoseAnalyzerScript);
    if (!FPaths::FileExists(Python))
    {
        OutError = FString::Printf(TEXT("로컬 포즈 분석용 Python을 찾지 못했습니다: %s (ThirdParty/UPTAnalyzer 가상환경 설치가 필요합니다)"), *Python);
        return false;
    }
    if (!FPaths::FileExists(Script))
    {
        OutError = FString::Printf(TEXT("포즈 분석 스크립트를 찾지 못했습니다: %s"), *Script);
        return false;
    }
    return true;
}

void FUPTPoseReferenceAnalyzer::Analyze(const FString& VideoPath, const FString& MatchMode, FUPTPoseAnalysisComplete Completion)
{
    FString PreflightError;
    if (!Preflight(PreflightError))
    {
        Completion.ExecuteIfBound(false, FUPTPoseAnalysisResult(), PreflightError);
        return;
    }
    bool bExpected = false;
    if (!bPoseAnalysisRunning.compare_exchange_strong(bExpected, true))
    {
        Completion.ExecuteIfBound(false, FUPTPoseAnalysisResult(), TEXT("로컬 포즈 분석이 이미 진행 중입니다."));
        return;
    }

    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
    const FString Python = ResolveProjectPath(Settings->PoseAnalyzerPythonPath);
    const FString Script = ResolveProjectPath(Settings->PoseAnalyzerScript);
    const FString Model = Settings->PoseAnalyzerModel.TrimStartAndEnd().IsEmpty() ? FString(TEXT("balanced")) : Settings->PoseAnalyzerModel.TrimStartAndEnd();
    const int32 Samples = FMath::Clamp(Settings->PoseSamplesPerShot, 1, 12);
    const FString SafeMode = MatchMode == TEXT("edit") ? TEXT("edit") : TEXT("scene");
    const FString FullVideoPath = FPaths::ConvertRelativePathToFull(VideoPath);

    // 영상·분석 설정·스크립트가 같으면 이전 분석 결과를 재사용한다.
    const FString Fingerprint = FMD5::HashAnsiString(*FString::Printf(TEXT("pose-v1|%s|%lld|%lld|%s|%s|%d|%lld"),
        *FullVideoPath, IFileManager::Get().FileSize(*FullVideoPath), IFileManager::Get().GetTimeStamp(*FullVideoPath).ToUnixTimestamp(),
        *SafeMode, *Model, Samples, IFileManager::Get().GetTimeStamp(*Script).ToUnixTimestamp()));
    const FString OutputDirectory = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UniversalProductionTools"), TEXT("PoseAnalysis"), Fingerprint.Left(16)));

    FUPTPoseAnalysisResult Result;
    Result.PlanFile = FPaths::Combine(OutputDirectory, TEXT("reference_plan.json"));
    Result.DebugDirectory = FPaths::Combine(OutputDirectory, TEXT("debug"));

    Async(EAsyncExecution::ThreadPool, [FullVideoPath, Python, Script, Model, Samples, SafeMode, OutputDirectory, Result, Completion]() mutable
    {
        FString Error;
        if (FFileHelper::LoadFileToString(Result.PlanJson, *Result.PlanFile))
        {
            Result.bFromCache = true;
        }
        else
        {
            IFileManager::Get().MakeDirectory(*OutputDirectory, true);
            FString InputVideo = FullVideoPath;
            // OpenCV는 Windows에서 비ASCII 경로의 영상을 열지 못하므로 분석용 사본을 만든다.
            if (!IsPureAscii(InputVideo))
            {
                const FString Copy = FPaths::Combine(OutputDirectory, TEXT("input") + FPaths::GetExtension(InputVideo, true));
                if (IFileManager::Get().Copy(*Copy, *InputVideo) != COPY_OK) Error = TEXT("한글 등 비ASCII 경로의 영상을 분석용으로 복사하지 못했습니다.");
                InputVideo = Copy;
            }
            if (Error.IsEmpty())
            {
                const FString Args = FString::Printf(TEXT("%s --video %s --output %s --debug-dir %s --mode %s --model %s --samples-per-shot %d"),
                    *Quote(Script), *Quote(InputVideo), *Quote(Result.PlanFile), *Quote(Result.DebugDirectory), *SafeMode, *Model, Samples);
                int32 ReturnCode = -1;
                FString StdOut, StdErr;
                const bool bLaunched = FPlatformProcess::ExecProcess(*Python, *Args, &ReturnCode, &StdOut, &StdErr);
                if (!bLaunched || ReturnCode != 0 || !FFileHelper::LoadFileToString(Result.PlanJson, *Result.PlanFile))
                {
                    Error = FString::Printf(TEXT("로컬 포즈 분석 실패 (code %d): %s"), ReturnCode, *(StdErr.IsEmpty() ? StdOut : StdErr).Right(1500));
                }
            }
        }
        bPoseAnalysisRunning = false;
        AsyncTask(ENamedThreads::GameThread, [Completion, Result, Error]()
        {
            Completion.ExecuteIfBound(Error.IsEmpty(), Result, Error);
        });
    });
}

void FUPTPoseReferenceAnalyzer::AnalyzeLink(const FString& Url, const FString& Section, const bool bRightsConfirmed, const FString& MatchMode, FUPTPoseAnalysisComplete Completion)
{
    FString PreflightError;
    if (!Preflight(PreflightError))
    {
        Completion.ExecuteIfBound(false, FUPTPoseAnalysisResult(), PreflightError);
        return;
    }
    const FString CleanUrl = Url.TrimStartAndEnd();
    const bool bValidUrl = (CleanUrl.StartsWith(TEXT("https://")) || CleanUrl.StartsWith(TEXT("http://")))
        && !CleanUrl.Contains(TEXT(" ")) && !CleanUrl.Contains(TEXT("\"")) && CleanUrl.Len() < 2048;
    if (!bValidUrl)
    {
        Completion.ExecuteIfBound(false, FUPTPoseAnalysisResult(), TEXT("영상 링크는 http:// 또는 https://로 시작하는 주소여야 합니다."));
        return;
    }
    // 구간은 숫자·콜론·마침표·하이픈만 남겨 명령줄에 그대로 넘겨도 안전하게 한다.
    FString CleanSection;
    for (const TCHAR Character : Section)
    {
        if (FChar::IsDigit(Character) || Character == TEXT(':') || Character == TEXT('.') || Character == TEXT('-')) CleanSection.AppendChar(Character);
    }

    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
    const FString Python = ResolveProjectPath(Settings->PoseAnalyzerPythonPath);
    const FString AnalyzerScript = ResolveProjectPath(Settings->PoseAnalyzerScript);
    const FString LinkScript = FPaths::Combine(FPaths::GetPath(AnalyzerScript), TEXT("upt_reference_link.py"));
    if (!FPaths::FileExists(LinkScript))
    {
        Completion.ExecuteIfBound(false, FUPTPoseAnalysisResult(), FString::Printf(TEXT("링크 분석 스크립트를 찾지 못했습니다: %s"), *LinkScript));
        return;
    }
    bool bExpected = false;
    if (!bPoseAnalysisRunning.compare_exchange_strong(bExpected, true))
    {
        Completion.ExecuteIfBound(false, FUPTPoseAnalysisResult(), TEXT("로컬 포즈 분석이 이미 진행 중입니다."));
        return;
    }

    const FString Model = Settings->PoseAnalyzerModel.TrimStartAndEnd().IsEmpty() ? FString(TEXT("balanced")) : Settings->PoseAnalyzerModel.TrimStartAndEnd();
    const int32 Samples = FMath::Clamp(Settings->PoseSamplesPerShot, 1, 12);
    const FString SafeMode = MatchMode == TEXT("edit") ? TEXT("edit") : TEXT("scene");
    const FString FFmpeg = Settings->FFmpegExecutablePath.TrimStartAndEnd();
    // 같은 링크·구간·설정·스크립트면 이전 분석 결과를 재사용한다(다시 받지 않는다).
    const FString Fingerprint = FMD5::HashAnsiString(*FString::Printf(TEXT("link-v1|%s|%s|%s|%s|%d|%lld|%lld"),
        *CleanUrl, *CleanSection, *SafeMode, *Model, Samples,
        IFileManager::Get().GetTimeStamp(*AnalyzerScript).ToUnixTimestamp(), IFileManager::Get().GetTimeStamp(*LinkScript).ToUnixTimestamp()));
    const FString OutputDirectory = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UniversalProductionTools"), TEXT("LinkAnalysis"), Fingerprint.Left(16)));

    FUPTPoseAnalysisResult Result;
    Result.PlanFile = FPaths::Combine(OutputDirectory, TEXT("reference_plan.json"));
    Result.DebugDirectory = FPaths::Combine(OutputDirectory, TEXT("debug"));

    Async(EAsyncExecution::ThreadPool, [Python, LinkScript, CleanUrl, CleanSection, bRightsConfirmed, SafeMode, Model, Samples, FFmpeg, OutputDirectory, Result, Completion]() mutable
    {
        FString Error;
        if (FFileHelper::LoadFileToString(Result.PlanJson, *Result.PlanFile) && FPaths::FileExists(FPaths::Combine(OutputDirectory, TEXT("source.json"))))
        {
            Result.bFromCache = true;
        }
        else
        {
            IFileManager::Get().MakeDirectory(*OutputDirectory, true);
            FString Args = FString::Printf(TEXT("%s --url %s --output-dir %s --mode %s --model %s --samples-per-shot %d"),
                *Quote(LinkScript), *Quote(CleanUrl), *Quote(OutputDirectory), *SafeMode, *Model, Samples);
            if (!CleanSection.IsEmpty()) Args += TEXT(" --section ") + CleanSection;
            if (bRightsConfirmed) Args += TEXT(" --rights-confirmed");
            if (!FFmpeg.IsEmpty()) Args += TEXT(" --ffmpeg ") + Quote(FFmpeg);
            int32 ReturnCode = -1;
            FString StdOut, StdErr;
            const bool bLaunched = FPlatformProcess::ExecProcess(*Python, *Args, &ReturnCode, &StdOut, &StdErr);
            if (!bLaunched || ReturnCode != 0 || !FFileHelper::LoadFileToString(Result.PlanJson, *Result.PlanFile))
            {
                const FString Output = StdErr + StdOut;
                if (ReturnCode == 3 || Output.Contains(TEXT("rights_required")))
                {
                    Error = TEXT("YouTube 등 다운로드를 허용하지 않는 사이트의 영상입니다. 이 영상을 분석에 쓸 권리·허락이 있을 때만 '권리 확인'에 체크한 뒤 다시 생성하세요.");
                }
                else
                {
                    Error = FString::Printf(TEXT("링크 영상 분석 실패 (code %d): %s"), ReturnCode, *Output.Right(1500));
                }
            }
        }
        bPoseAnalysisRunning = false;
        AsyncTask(ENamedThreads::GameThread, [Completion, Result, Error]()
        {
            Completion.ExecuteIfBound(Error.IsEmpty(), Result, Error);
        });
    });
}

void FUPTPoseReferenceAnalyzer::SearchLibrary(const FString& LibraryDirectory, const FString& Prompt, const int32 ResultCount, FUPTLibrarySearchComplete Completion)
{
    FString PreflightError;
    if (!Preflight(PreflightError))
    {
        Completion.ExecuteIfBound(false, FString(), PreflightError);
        return;
    }
    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
    const FString Python = ResolveProjectPath(Settings->PoseAnalyzerPythonPath);
    const FString Script = ResolveProjectPath(Settings->ReferenceLibraryScript);
    const FString Library = ResolveProjectPath(LibraryDirectory);
    if (LibraryDirectory.TrimStartAndEnd().IsEmpty() || !FPaths::DirectoryExists(Library))
    {
        Completion.ExecuteIfBound(false, FString(), FString::Printf(TEXT("레퍼런스 라이브러리 폴더를 찾지 못했습니다: %s"), *Library));
        return;
    }
    if (!FPaths::FileExists(Script))
    {
        Completion.ExecuteIfBound(false, FString(), FString::Printf(TEXT("라이브러리 검색 스크립트를 찾지 못했습니다: %s"), *Script));
        return;
    }
    bool bExpected = false;
    if (!bPoseAnalysisRunning.compare_exchange_strong(bExpected, true))
    {
        Completion.ExecuteIfBound(false, FString(), TEXT("로컬 포즈 분석이 이미 진행 중입니다."));
        return;
    }

    const FString WorkDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UniversalProductionTools"), TEXT("LibrarySearch")));
    const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
    const FString PromptFile = FPaths::Combine(WorkDirectory, FString::Printf(TEXT("prompt_%s.txt"), *Stamp));
    const FString ResultFile = FPaths::Combine(WorkDirectory, FString::Printf(TEXT("result_%s.json"), *Stamp));
    IFileManager::Get().MakeDirectory(*WorkDirectory, true);
    // 한글·따옴표가 섞인 프롬프트를 명령줄로 넘기면 깨지거나 인자가 갈라지므로 UTF-8 파일로 넘긴다.
    FFileHelper::SaveStringToFile(Prompt, *PromptFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    const int32 Top = FMath::Clamp(ResultCount, 1, 20);
    // 규칙이 못 알아듣는 표현은 로컬 LLM에 물어 해석한다. 프롬프트 문구가 밖으로 나가지 않도록 로컬 주소일 때만 넘긴다.
    const FString LlmEndpoint = Settings->ApiEndpoint.TrimStartAndEnd();
    const FString LlmModel = Settings->Model.TrimStartAndEnd();
    const bool bLocalLlm = !LlmEndpoint.IsEmpty() && !LlmModel.IsEmpty() && UPTEndpoint::IsLocal(LlmEndpoint);

    Async(EAsyncExecution::ThreadPool, [Python, Script, Library, PromptFile, ResultFile, Top, LlmEndpoint, LlmModel, bLocalLlm, Completion]()
    {
        FString Error;
        FString ResultJson;
        int32 ReturnCode = -1;
        FString StdOut, StdErr;
        // 1) 새로 넣었거나 바뀐 영상만 분석해 색인을 최신으로 만든다(처음에는 영상 수에 따라 몇 분 걸린다).
        // 링크로 분석해 둔 영상(영상 파일이 남지 않은 유튜브 등 포함)도 같은 검색 대상에 넣는다.
        const FString LinkAnalysisDir = FPaths::ConvertRelativePathToFull(
            FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UniversalProductionTools"), TEXT("LinkAnalysis")));
        const FString IndexArgs = FString::Printf(TEXT("%s index --library %s --link-analysis %s"),
            *Quote(Script), *Quote(Library), *Quote(LinkAnalysisDir));
        if (!FPlatformProcess::ExecProcess(*Python, *IndexArgs, &ReturnCode, &StdOut, &StdErr) || ReturnCode != 0)
        {
            Error = FString::Printf(TEXT("레퍼런스 라이브러리 색인 실패 (code %d): %s"), ReturnCode, *(StdErr.IsEmpty() ? StdOut : StdErr).Right(1500));
        }
        // 2) 프롬프트와 샷 구성이 비슷한 구간을 찾는다.
        if (Error.IsEmpty())
        {
            ReturnCode = -1;
            StdOut.Reset();
            StdErr.Reset();
            FString SearchArgs = FString::Printf(TEXT("%s search --library %s --prompt-file %s --top %d --output %s"),
                *Quote(Script), *Quote(Library), *Quote(PromptFile), Top, *Quote(ResultFile));
            SearchArgs += bLocalLlm
                ? FString::Printf(TEXT(" --llm-endpoint %s --llm-model %s"), *Quote(LlmEndpoint), *Quote(LlmModel))
                : FString(TEXT(" --no-llm"));
            if (!FPlatformProcess::ExecProcess(*Python, *SearchArgs, &ReturnCode, &StdOut, &StdErr) || ReturnCode != 0
                || !FFileHelper::LoadFileToString(ResultJson, *ResultFile))
            {
                Error = FString::Printf(TEXT("레퍼런스 라이브러리 검색 실패 (code %d): %s"), ReturnCode, *(StdErr.IsEmpty() ? StdOut : StdErr).Right(1500));
            }
        }
        bPoseAnalysisRunning = false;
        AsyncTask(ENamedThreads::GameThread, [Completion, ResultJson, Error]()
        {
            Completion.ExecuteIfBound(Error.IsEmpty(), ResultJson, Error);
        });
    });
}

void FUPTPoseReferenceAnalyzer::VerifyGeneratedCinematic(const FUPTCinematicPlan& Plan, const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors,
    const FString& ReferencePlanFile, FUPTPoseVerifyComplete Completion)
{
    FString PreflightError;
    if (!Preflight(PreflightError))
    {
        Completion.ExecuteIfBound(false, PreflightError);
        return;
    }
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || Plan.Shots.IsEmpty() || !FPaths::FileExists(ReferencePlanFile))
    {
        Completion.ExecuteIfBound(false, TEXT("검증에 필요한 월드·샷 또는 레퍼런스 분석 결과가 없습니다."));
        return;
    }
    bool bExpected = false;
    if (!bPoseAnalysisRunning.compare_exchange_strong(bExpected, true))
    {
        Completion.ExecuteIfBound(false, TEXT("로컬 포즈 분석이 이미 진행 중입니다."));
        return;
    }

    const FString OutputDirectory = FPaths::Combine(FPaths::GetPath(ReferencePlanFile), TEXT("verify_") + FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
    IFileManager::Get().MakeDirectory(*OutputDirectory, true);
    const int32 Width = 1280;
    const int32 Height = FMath::Max(64, FMath::RoundToInt(Width / FMath::Clamp(Plan.AspectRatio, 0.25f, 4.0f)));
    UTextureRenderTarget2D* RenderTarget = UKismetRenderingLibrary::CreateRenderTarget2D(World, Width, Height, RTF_RGBA8);
    FActorSpawnParameters SpawnParameters;
    SpawnParameters.ObjectFlags = RF_Transient;
    ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), FTransform::Identity, SpawnParameters);
    if (!RenderTarget || !CaptureActor)
    {
        if (CaptureActor) CaptureActor->Destroy();
        bPoseAnalysisRunning = false;
        Completion.ExecuteIfBound(false, TEXT("검증용 캡처 카메라를 만들지 못했습니다."));
        return;
    }
    USceneCaptureComponent2D* Capture = CaptureActor->GetCaptureComponent2D();
    Capture->TextureTarget = RenderTarget;
    Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;

    // 샷 시작 시점의 카메라(시퀀스 생성과 같은 프레이밍 계산)로 캡처한다. 모션 샷은 시작 프레임 기준 비교가 된다.
    TArray<TSharedPtr<FJsonValue>> Entries;
    // 샷별 액터 배치(ActorPlacements)는 시퀀스 재생 때만 적용되므로, 캡처하는 동안만 적용했다가 끝나면 원래 자리로 되돌린다.
    TMap<TWeakObjectPtr<AActor>, FTransform> OriginalTransforms;
    for (int32 Index = 0; Index < Plan.Shots.Num(); ++Index)
    {
        const FUPTCinematicShot& Shot = Plan.Shots[Index];
        for (const TPair<TWeakObjectPtr<AActor>, FTransform>& Pair : OriginalTransforms)
        {
            if (AActor* PlacedActor = Pair.Key.Get()) PlacedActor->SetActorTransform(Pair.Value);
        }
        for (const FUPTActorBlockingPlacement& Placement : Shot.ActorPlacements)
        {
            const TWeakObjectPtr<AActor>* Found = SceneActors.Find(Placement.ActorLabel);
            AActor* PlacedActor = Found ? Found->Get() : nullptr;
            if (!PlacedActor) continue;
            if (!OriginalTransforms.Contains(PlacedActor)) OriginalTransforms.Add(PlacedActor, PlacedActor->GetActorTransform());
            PlacedActor->SetActorLocationAndRotation(Placement.WorldLocation, Placement.WorldRotation);
        }
        FVector Location;
        FRotator Rotation;
        UPTFraming::ResolveShotCamera(World, Shot, Plan.AspectRatio, SceneActors, Location, Rotation);
        CaptureActor->SetActorLocationAndRotation(Location, Rotation);
        Capture->FOVAngle = UPTFraming::GetFieldOfViewDegrees(UPTFraming::SensorWidthMm, Shot.FocalLength);
        for (int32 Warmup = 0; Warmup < 3; ++Warmup) Capture->CaptureScene();
        const FString FileName = FString::Printf(TEXT("shot_%02d.png"), Index + 1);
        UKismetRenderingLibrary::ExportRenderTarget(World, RenderTarget, OutputDirectory, FileName);

        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Shot.Name);
        Entry->SetStringField(TEXT("image"), FPaths::ConvertRelativePathToFull(FPaths::Combine(OutputDirectory, FileName)));
        Entry->SetStringField(TEXT("camera_motion"), Shot.CameraMotion);
        // 주 피사체 Actor의 조준점이 이 캡처 화면의 어디에 오는지 알려 주면, 검증 스크립트가 여러 인물 중 실제 주 피사체를 고른다.
        const TWeakObjectPtr<AActor>* SubjectPtr = SceneActors.Find(Shot.Subject);
        FVector2D SubjectScreen;
        if (SubjectPtr && SubjectPtr->IsValid()
            && UPTFraming::ProjectToScreen(Location, Rotation, UPTFraming::GetFocusPoint(SubjectPtr->Get(), Shot.FocusHeightRatio), Shot.FocalLength, Plan.AspectRatio, SubjectScreen))
        {
            TSharedRef<FJsonObject> ScreenObject = MakeShared<FJsonObject>();
            ScreenObject->SetNumberField(TEXT("x"), SubjectScreen.X);
            ScreenObject->SetNumberField(TEXT("y"), SubjectScreen.Y);
            Entry->SetObjectField(TEXT("subject_screen"), ScreenObject);
        }
        // 주 피사체의 전신이 이 화면에서 차지하는 높이(화면 대비). 프레임에 잘린 인물은 포즈 검출기가
        // 보이는 부분만으로 전신 키를 추정해야 해서 오차가 크다. 엔진은 Bounds와 카메라를 알고 있으니 정확히 준다.
        FVector2D BoundsTop, BoundsBottom;
        const FBox SubjectBounds = SubjectPtr && SubjectPtr->IsValid() ? UPTFraming::GetSubjectBounds(SubjectPtr->Get()) : FBox(ForceInit);
        if (SubjectBounds.IsValid)
        {
            const FVector Center = SubjectBounds.GetCenter();
            if (UPTFraming::ProjectToScreen(Location, Rotation, FVector(Center.X, Center.Y, SubjectBounds.Max.Z), Shot.FocalLength, Plan.AspectRatio, BoundsTop)
                && UPTFraming::ProjectToScreen(Location, Rotation, FVector(Center.X, Center.Y, SubjectBounds.Min.Z), Shot.FocalLength, Plan.AspectRatio, BoundsBottom))
            {
                Entry->SetNumberField(TEXT("subject_screen_height"), FMath::Abs(BoundsBottom.Y - BoundsTop.Y));
            }
        }
        // 어깨 너머 샷은 앞사람만 숨기고 한 장 더 찍는다. 두 장의 차이가 앞사람이 차지한 화면 영역이다.
        // 인물 검출·분할 모델은 무늬 없는 마네킹의 어깨 조각을 사람으로 보지 못해(YOLOX·DeepLab 모두 0개)
        // 이 방식이 '앞사람이 실제로 프레임 안에 있는가'를 재는 유일하게 확실한 방법이다.
        const TWeakObjectPtr<AActor>* ForegroundPtr = Shot.OverShoulderActor.IsEmpty() ? nullptr : SceneActors.Find(Shot.OverShoulderActor);
        AActor* ForegroundActor = ForegroundPtr ? ForegroundPtr->Get() : nullptr;
        if (ForegroundActor)
        {
            Capture->HiddenActors.Add(ForegroundActor);
            Capture->MarkRenderStateDirty();
            for (int32 Warmup = 0; Warmup < 3; ++Warmup) Capture->CaptureScene();
            const FString PlainFileName = FString::Printf(TEXT("shot_%02d_nofg.png"), Index + 1);
            UKismetRenderingLibrary::ExportRenderTarget(World, RenderTarget, OutputDirectory, PlainFileName);
            Capture->HiddenActors.Remove(ForegroundActor);
            Capture->MarkRenderStateDirty();
            Entry->SetStringField(TEXT("image_without_foreground"), FPaths::ConvertRelativePathToFull(FPaths::Combine(OutputDirectory, PlainFileName)));
            Entry->SetStringField(TEXT("over_shoulder_actor"), Shot.OverShoulderActor);
            Entry->SetStringField(TEXT("over_shoulder_side"), Shot.OverShoulderSide);
        }
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }
    for (const TPair<TWeakObjectPtr<AActor>, FTransform>& Pair : OriginalTransforms)
    {
        if (AActor* PlacedActor = Pair.Key.Get()) PlacedActor->SetActorTransform(Pair.Value);
    }
    CaptureActor->Destroy();

    TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
    Manifest->SetArrayField(TEXT("shots"), Entries);
    FString ManifestText;
    FJsonSerializer::Serialize(Manifest, TJsonWriterFactory<>::Create(&ManifestText));
    const FString ManifestFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(OutputDirectory, TEXT("manifest.json")));
    FFileHelper::SaveStringToFile(ManifestText, *ManifestFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
    const FString Python = ResolveProjectPath(Settings->PoseAnalyzerPythonPath);
    const FString Script = FPaths::Combine(FPaths::GetPath(ResolveProjectPath(Settings->PoseAnalyzerScript)), TEXT("upt_pose_render_verify.py"));
    const FString ReportFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(OutputDirectory, TEXT("verify_report.json")));
    const FString DebugDirectory = FPaths::ConvertRelativePathToFull(FPaths::Combine(OutputDirectory, TEXT("debug")));
    const FString ReferenceFile = FPaths::ConvertRelativePathToFull(ReferencePlanFile);

    Async(EAsyncExecution::ThreadPool, [Python, Script, ReferenceFile, ManifestFile, ReportFile, DebugDirectory, Completion]()
    {
        FString Error;
        FString Report;
        if (!FPaths::FileExists(Script))
        {
            Error = FString::Printf(TEXT("구도 검증 스크립트를 찾지 못했습니다: %s"), *Script);
        }
        else
        {
            const FString Args = FString::Printf(TEXT("%s --reference %s --manifest %s --output %s --debug-dir %s"),
                *Quote(Script), *Quote(ReferenceFile), *Quote(ManifestFile), *Quote(ReportFile), *Quote(DebugDirectory));
            int32 ReturnCode = -1;
            FString StdOut, StdErr;
            const bool bLaunched = FPlatformProcess::ExecProcess(*Python, *Args, &ReturnCode, &StdOut, &StdErr);
            if (!bLaunched || ReturnCode != 0 || !FFileHelper::LoadFileToString(Report, *ReportFile))
            {
                Error = FString::Printf(TEXT("구도 검증 실패 (code %d): %s"), ReturnCode, *(StdErr.IsEmpty() ? StdOut : StdErr).Right(1000));
            }
        }
        bPoseAnalysisRunning = false;
        AsyncTask(ENamedThreads::GameThread, [Completion, Report, Error]()
        {
            Completion.ExecuteIfBound(Error.IsEmpty(), Error.IsEmpty() ? Report : Error);
        });
    });
}

namespace
{
struct FRefineContext
{
    FUPTCinematicPlan Plan;
    TMap<FString, TWeakObjectPtr<AActor>> Actors;
    FString ReferenceFile;
    int32 MaxCorrections = 0;
    int32 Pass = 0;
    double InitialScore = -1.0;
    TArray<FUPTCinematicShot> BestShots;
    TArray<double> BestScores;
    TArray<TSharedPtr<FJsonObject>> BestRows;
    TArray<float> StepScales;
    TSharedPtr<FJsonObject> LastRoot;
    FUPTPoseRefineProgress Progress;
    FUPTPoseRefineComplete Completion;
};

// 이번 측정으로 샷별 최고 구도를 갱신하고, 최고 구도의 오차만큼 카메라를 옮긴다. 옮긴 샷 수를 반환한다.
int32 UpdateBestAndCorrect(FRefineContext& Context, const FJsonObject& Root, const bool bCorrect)
{
    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    if (!Root.TryGetArrayField(TEXT("shots"), Rows) || !Rows) return 0;
    const int32 Count = FMath::Min(Rows->Num(), Context.Plan.Shots.Num());
    if (Context.BestScores.Num() != Count)
    {
        Context.BestScores.Init(-1.0, Count);
        Context.StepScales.Init(1.0f, Count);
        Context.BestShots.SetNum(Count);
        Context.BestRows.SetNum(Count);
    }

    int32 Corrected = 0;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const TSharedPtr<FJsonObject> Row = (*Rows)[Index]->AsObject();
        FUPTCinematicShot& Shot = Context.Plan.Shots[Index];
        FString Status;
        if (!Row.IsValid() || !Row->TryGetStringField(TEXT("status"), Status) || Status == TEXT("skipped") || Shot.Subject.IsEmpty()) continue;
        double Score = 0.0;
        Row->TryGetNumberField(TEXT("score"), Score);
        if (Context.BestScores[Index] < 0.0 || Score > Context.BestScores[Index] + 0.5)
        {
            Context.BestScores[Index] = Score;
            Context.BestShots[Index] = Shot;
            Context.BestRows[Index] = Row;
        }
        else
        {
            // 보정이 오히려 나빴다면 최고 구도로 되돌리고 다음에는 절반만 움직인다.
            Shot = Context.BestShots[Index];
            Context.StepScales[Index] *= 0.5f;
        }
        if (!bCorrect || Context.BestScores[Index] >= 92.0 || Context.StepScales[Index] < 0.2f) continue;

        const TSharedPtr<FJsonObject>& Best = Context.BestRows[Index];
        FString BestStatus;
        // 인물을 찾지 못한 샷은 어느 쪽으로 옮겨야 할지 알 수 없어 보정하지 않는다.
        if (!Best->TryGetStringField(TEXT("status"), BestStatus) || BestStatus != TEXT("ok")) continue;
        double ErrorX = 0.0, ErrorY = 0.0, HeightRatio = 0.0;
        Best->TryGetNumberField(TEXT("error_x"), ErrorX);
        Best->TryGetNumberField(TEXT("error_y"), ErrorY);
        Best->TryGetNumberField(TEXT("height_ratio"), HeightRatio);
        FString ReferenceSize;
        Best->TryGetStringField(TEXT("reference_size"), ReferenceSize);
        // 클로즈업은 얼굴·어깨 몇 점으로 전신 크기를 추정하므로 측정값이 크게 흔들린다. 더 조금씩 움직인다.
        const bool bTightShot = ReferenceSize == TEXT("close_up") || ReferenceSize == TEXT("extreme_close_up");
        const double Gain = (bTightShot ? 0.5 : 0.8) * Context.StepScales[Index];

        // 인물이 레퍼런스보다 크게 보이면(비율>1) 카메라를 멀리 둔다. 높이·측면 오프셋도 같은 비율로 옮겨 앵글을 유지한다.
        if (HeightRatio > 0.05)
        {
            const double DistanceScale = FMath::Clamp(FMath::Pow(HeightRatio, Gain), 0.75, 1.33);
            if (!Shot.OverShoulderActor.IsEmpty())
            {
                // 어깨 너머 샷은 카메라를 옮기면 앞사람이 주 피사체를 가리거나 화면에서 빠지므로 렌즈로만 크기를 맞춘다.
                Shot.FocalLength = FMath::Clamp(static_cast<float>(Shot.FocalLength / DistanceScale), 8.0f, 200.0f);
            }
            else
            {
                Shot.DistanceCm = FMath::Clamp(static_cast<float>(Shot.DistanceCm * DistanceScale), 30.0f, 10000.0f);
                Shot.HeightCm = FMath::Clamp(static_cast<float>(Shot.HeightCm * DistanceScale), -5000.0f, 5000.0f);
                Shot.SideCm = FMath::Clamp(static_cast<float>(Shot.SideCm * DistanceScale), -5000.0f, 5000.0f);
            }
        }
        Shot.SubjectScreenX = FMath::Clamp(static_cast<float>(Shot.SubjectScreenX - ErrorX * Gain), 0.02f, 0.98f);
        Shot.SubjectScreenY = FMath::Clamp(static_cast<float>(Shot.SubjectScreenY - ErrorY * Gain), 0.02f, 0.98f);
        ++Corrected;
    }
    return Corrected;
}

void FinishRefine(const TSharedRef<FRefineContext>& Context)
{
    FUPTCinematicPlan BestPlan = Context->Plan;
    TArray<TSharedPtr<FJsonValue>> Rows;
    const TArray<TSharedPtr<FJsonValue>>* LastRows = nullptr;
    Context->LastRoot->TryGetArrayField(TEXT("shots"), LastRows);
    double ScoreSum = 0.0;
    int32 ScoreCount = 0;
    for (int32 Index = 0; LastRows && Index < LastRows->Num(); ++Index)
    {
        TSharedPtr<FJsonObject> Row = (*LastRows)[Index]->AsObject();
        if (Context->BestRows.IsValidIndex(Index) && Context->BestRows[Index].IsValid() && BestPlan.Shots.IsValidIndex(Index))
        {
            Row = Context->BestRows[Index];
            BestPlan.Shots[Index] = Context->BestShots[Index];
        }
        if (!Row.IsValid()) continue;
        double Score = 0.0;
        if (Row->TryGetNumberField(TEXT("score"), Score))
        {
            ScoreSum += Score;
            ++ScoreCount;
        }
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->Values = Context->LastRoot->Values;
    Report->SetArrayField(TEXT("shots"), Rows);
    Report->SetNumberField(TEXT("overall_score"), ScoreCount > 0 ? FMath::RoundToDouble(ScoreSum / ScoreCount * 10.0) / 10.0 : 0.0);
    Report->SetNumberField(TEXT("initial_score"), Context->InitialScore);
    Report->SetNumberField(TEXT("refine_passes"), Context->Pass);
    FString ReportText;
    FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&ReportText));
    Context->Completion.ExecuteIfBound(true, BestPlan, ReportText);
}

void RunRefinePass(const TSharedRef<FRefineContext>& Context)
{
    FUPTPoseReferenceAnalyzer::VerifyGeneratedCinematic(Context->Plan, Context->Actors, Context->ReferenceFile,
        FUPTPoseVerifyComplete::CreateLambda([Context](const bool bSuccess, const FString& Result)
        {
            TSharedPtr<FJsonObject> Root;
            if (!bSuccess || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result), Root) || !Root.IsValid())
            {
                // 첫 측정부터 실패하면 오류로 끝내고, 보정 도중 실패하면 그때까지의 최고 구도로 마무리한다.
                if (Context->LastRoot.IsValid()) FinishRefine(Context);
                else Context->Completion.ExecuteIfBound(false, Context->Plan, bSuccess ? FString(TEXT("구도 검증 결과를 읽지 못했습니다.")) : Result);
                return;
            }
            double PassScore = 0.0;
            Root->TryGetNumberField(TEXT("overall_score"), PassScore);
            if (Context->Pass == 0) Context->InitialScore = PassScore;
            Context->LastRoot = Root;
            Context->Progress.ExecuteIfBound(Context->Pass, PassScore);

            const bool bCanCorrect = Context->Pass < Context->MaxCorrections;
            if (UpdateBestAndCorrect(*Context, *Root, bCanCorrect) == 0 || !bCanCorrect)
            {
                FinishRefine(Context);
                return;
            }
            ++Context->Pass;
            // 검증 잠금이 풀린 다음 틱에 다시 캡처한다.
            AsyncTask(ENamedThreads::GameThread, [Context]() { RunRefinePass(Context); });
        }));
}
}

void FUPTPoseReferenceAnalyzer::RefineShotCameras(const FUPTCinematicPlan& Plan, const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors,
    const FString& ReferencePlanFile, const int32 MaxCorrections, FUPTPoseRefineProgress Progress, FUPTPoseRefineComplete Completion)
{
    const TSharedRef<FRefineContext> Context = MakeShared<FRefineContext>();
    Context->Plan = Plan;
    Context->Actors = SceneActors;
    Context->ReferenceFile = ReferencePlanFile;
    Context->MaxCorrections = FMath::Clamp(MaxCorrections, 0, 5);
    Context->Progress = MoveTemp(Progress);
    Context->Completion = MoveTemp(Completion);
    RunRefinePass(Context);
}

FString FUPTPoseReferenceAnalyzer::FormatVerifyReport(const FString& ReportJson, double& OutScore)
{
    OutScore = -1.0;
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ReportJson), Root) || !Root.IsValid())
    {
        return TEXT("구도 검증 결과를 읽지 못했습니다.");
    }
    Root->TryGetNumberField(TEXT("overall_score"), OutScore);
    FString Text = FString::Printf(TEXT("[레퍼런스 대비 구도 검증] 종합 %.0f / 100 — 생성된 카메라 시점을 같은 포즈 분석기로 다시 측정\n"), OutScore);
    double InitialScore = -1.0, RefinePasses = 0.0;
    if (Root->TryGetNumberField(TEXT("initial_score"), InitialScore) && Root->TryGetNumberField(TEXT("refine_passes"), RefinePasses) && RefinePasses > 0.0)
    {
        Text += FString::Printf(TEXT("자동 구도 보정 %d회: %.0f → %.0f (샷마다 가장 잘 맞은 카메라를 채택)\n"),
            FMath::RoundToInt(RefinePasses), InitialScore, OutScore);
    }
    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (Root->TryGetArrayField(TEXT("shots"), Shots) && Shots)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Shots)
        {
            const TSharedPtr<FJsonObject> Shot = Value->AsObject();
            if (!Shot.IsValid()) continue;
            const FString Name = Shot->GetStringField(TEXT("name"));
            const FString Status = Shot->GetStringField(TEXT("status"));
            FString Note;
            Shot->TryGetStringField(TEXT("note"), Note);
            if (Status != TEXT("ok"))
            {
                Text += FString::Printf(TEXT("%s: %s%s\n"), *Name, *Status, Note.IsEmpty() ? TEXT("") : *(TEXT(" — ") + Note));
                continue;
            }
            double Score = 0.0, ErrorX = 0.0, ErrorY = 0.0, HeightRatio = 0.0;
            Shot->TryGetNumberField(TEXT("score"), Score);
            Shot->TryGetNumberField(TEXT("error_x"), ErrorX);
            Shot->TryGetNumberField(TEXT("error_y"), ErrorY);
            Shot->TryGetNumberField(TEXT("height_ratio"), HeightRatio);
            // 레퍼런스가 어깨 너머 샷이면 생성 캡처에서도 같은 쪽에 잘린 앞사람이 보였는지 함께 보여 준다(점수에는 넣지 않는다).
            FString OverShoulderText, ExpectedSide, FoundSide;
            if (Shot->TryGetStringField(TEXT("over_shoulder_expected"), ExpectedSide))
            {
                Shot->TryGetStringField(TEXT("over_shoulder_found"), FoundSide);
                OverShoulderText = FString::Printf(TEXT(" | 어깨 너머 앞사람 %s → %s"), *ExpectedSide, FoundSide.IsEmpty() ? TEXT("없음") : *FoundSide);
            }
            Text += FString::Printf(TEXT("%s: 점수 %.0f | 위치 오차 가로 %+.3f 세로 %+.3f | 인물 크기 %.2f배 | 샷 크기 %s → %s%s%s\n"),
                *Name, Score, ErrorX, ErrorY, HeightRatio, *Shot->GetStringField(TEXT("reference_size")), *Shot->GetStringField(TEXT("render_size")),
                *OverShoulderText, Note.IsEmpty() ? TEXT("") : *(TEXT(" (") + Note + TEXT(")")));
        }
    }
    FString OutputDirectory;
    if (Root->TryGetStringField(TEXT("output_directory"), OutputDirectory)) Text += TEXT("캡처·비교 이미지: ") + OutputDirectory + TEXT("\n");
    return Text;
}
