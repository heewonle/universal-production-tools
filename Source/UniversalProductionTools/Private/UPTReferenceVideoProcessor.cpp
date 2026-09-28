#include "UPTReferenceVideoProcessor.h"

#include "UPTSettings.h"

#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
void ReadFrameDimensions(const FString& FrameFile, FUPTReferenceFrameResult& Result)
{
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *FrameFile)) return;
    IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    const EImageFormat Format = Module.DetectImageFormat(Bytes.GetData(), Bytes.Num());
    const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(Format);
    if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num())) return;
    Result.FrameWidth = static_cast<int32>(Wrapper->GetWidth());
    Result.FrameHeight = static_cast<int32>(Wrapper->GetHeight());
    if (Result.FrameWidth > 0 && Result.FrameHeight > 0) Result.AspectRatio = static_cast<float>(Result.FrameWidth) / Result.FrameHeight;
}
}

bool FUPTReferenceVideoProcessor::ValidateVideo(const FString& VideoPath, FString& OutError)
{
    if (!FPaths::FileExists(VideoPath))
    {
        OutError = TEXT("레퍼런스 영상 파일을 찾을 수 없습니다.");
        return false;
    }
    const FString Extension = FPaths::GetExtension(VideoPath).ToLower();
    if (Extension != TEXT("mp4") && Extension != TEXT("mov") && Extension != TEXT("m4v") && Extension != TEXT("avi"))
    {
        OutError = TEXT("지원 형식은 MP4, MOV, M4V, AVI입니다.");
        return false;
    }
    const int64 Size = IFileManager::Get().FileSize(*VideoPath);
    if (Size <= 0 || Size > 2ll * 1024ll * 1024ll * 1024ll)
    {
        OutError = TEXT("영상 파일은 0바이트보다 크고 2GB 이하여야 합니다.");
        return false;
    }
    return true;
}

void FUPTReferenceVideoProcessor::ExtractFrames(
    const FString& VideoPath,
    const float SampleIntervalSeconds,
    const int32 MaxFrames,
    FUPTReferenceFramesComplete Completion)
{
    FString Error;
    if (!ValidateVideo(VideoPath, Error))
    {
        Completion.ExecuteIfBound(false, FUPTReferenceFrameResult(), Error);
        return;
    }
    const float SafeInterval = FMath::Clamp(SampleIntervalSeconds, 0.1f, 10.0f);
    const int32 SafeMaxFrames = FMath::Clamp(MaxFrames, 1, 300);
    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
    const FString FFmpegPath = Settings->FFmpegExecutablePath.TrimStartAndEnd();
    const float ShotChangeThreshold = Settings->ShotChangeThreshold;
    const float MinimumShotSeconds = Settings->MinimumShotSeconds;
    if (FFmpegPath.IsEmpty())
    {
        Completion.ExecuteIfBound(false, FUPTReferenceFrameResult(), TEXT("FFmpeg 실행 파일 경로가 비어 있습니다."));
        return;
    }
    if ((FFmpegPath.Contains(TEXT("/")) || FFmpegPath.Contains(TEXT("\\"))) && !FPaths::FileExists(FFmpegPath))
    {
        Completion.ExecuteIfBound(false, FUPTReferenceFrameResult(), TEXT("설정된 FFmpeg 실행 파일을 찾을 수 없습니다."));
        return;
    }

    const FString VideoStamp = FString::Printf(TEXT("%08X_%lld"), GetTypeHash(FPaths::ConvertRelativePathToFull(VideoPath)), IFileManager::Get().GetTimeStamp(*VideoPath).ToUnixTimestamp());
    const FString OutputDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UniversalProductionTools"), TEXT("ReferenceFrames"), VideoStamp);
    IFileManager::Get().MakeDirectory(*OutputDirectory, true);
    const FString OutputPattern = FPaths::Combine(OutputDirectory, TEXT("frame_%04d.jpg"));
    const FString Arguments = FString::Printf(
        TEXT("-hide_banner -loglevel error -y -i \"%s\" -vf \"fps=1/%.3f,scale=960:-2:force_original_aspect_ratio=decrease\" -frames:v %d \"%s\""),
        *VideoPath, SafeInterval, SafeMaxFrames, *OutputPattern);

    Async(EAsyncExecution::ThreadPool, [FFmpegPath, Arguments, VideoPath, VideoStamp, OutputDirectory, SafeInterval, ShotChangeThreshold, MinimumShotSeconds, Completion]()
    {
        void* ReadPipe = nullptr;
        void* WritePipe = nullptr;
        FPlatformProcess::CreatePipe(ReadPipe, WritePipe);
        FProcHandle Process = FPlatformProcess::CreateProc(*FFmpegPath, *Arguments, true, true, true, nullptr, 0, nullptr, WritePipe);
        const bool bProcessStarted = Process.IsValid();
        FString ProcessOutput;
        int32 ReturnCode = -1;
        if (Process.IsValid())
        {
            while (FPlatformProcess::IsProcRunning(Process))
            {
                ProcessOutput += FPlatformProcess::ReadPipe(ReadPipe);
                FPlatformProcess::Sleep(0.05f);
            }
            ProcessOutput += FPlatformProcess::ReadPipe(ReadPipe);
            FPlatformProcess::GetProcReturnCode(Process, &ReturnCode);
            FPlatformProcess::CloseProc(Process);
        }
        FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

        FUPTReferenceFrameResult Result;
        Result.SourceVideo = VideoPath;
        Result.SourceFingerprint = VideoStamp;
        Result.OutputDirectory = OutputDirectory;
        Result.SampleIntervalSeconds = SafeInterval;
        IFileManager::Get().FindFiles(Result.FrameFiles, *FPaths::Combine(OutputDirectory, TEXT("*.jpg")), true, false);
        Result.FrameFiles.Sort();
        for (FString& Frame : Result.FrameFiles) Frame = FPaths::Combine(OutputDirectory, Frame);
        if (!Result.FrameFiles.IsEmpty()) ReadFrameDimensions(Result.FrameFiles[0], Result);
        bool bSuccess = bProcessStarted && ReturnCode == 0 && !Result.FrameFiles.IsEmpty();
        FString FinalError = bSuccess ? FString() : FString::Printf(TEXT("FFmpeg 프레임 추출 실패(%d): %s"), ReturnCode, *ProcessOutput.Left(1000));
        if (bSuccess && !FUPTShotBoundaryDetector::Detect(Result.FrameFiles, SafeInterval, ShotChangeThreshold, MinimumShotSeconds, Result.ShotBoundaries, FinalError))
        {
            bSuccess = false;
        }
        AsyncTask(ENamedThreads::GameThread, [bSuccess, Result = MoveTemp(Result), FinalError, Completion]()
        {
            Completion.ExecuteIfBound(bSuccess, Result, FinalError);
        });
    });
}
