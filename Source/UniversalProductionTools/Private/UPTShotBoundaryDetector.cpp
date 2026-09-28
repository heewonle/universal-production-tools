#include "UPTShotBoundaryDetector.h"

#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"

namespace
{
bool BuildSignature(const FString& Filename, TArray<FLinearColor>& OutSignature)
{
    TArray64<uint8> Compressed;
    if (!FFileHelper::LoadFileToArray(Compressed, *Filename)) return false;
    FImage Image;
    IImageWrapperModule& ImageWrapper = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    if (!ImageWrapper.DecompressImage(Compressed.GetData(), Compressed.Num(), Image) || Image.SizeX <= 0 || Image.SizeY <= 0) return false;
    const int64 BytesPerPixel = ERawImageFormat::GetBytesPerPixel(Image.Format);
    if (BytesPerPixel <= 0) return false;

    constexpr int32 GridSize = 8;
    OutSignature.Reset(GridSize * GridSize);
    for (int32 GridY = 0; GridY < GridSize; ++GridY)
    {
        const int32 Y = FMath::Clamp((GridY * 2 + 1) * Image.SizeY / (GridSize * 2), 0, Image.SizeY - 1);
        for (int32 GridX = 0; GridX < GridSize; ++GridX)
        {
            const int32 X = FMath::Clamp((GridX * 2 + 1) * Image.SizeX / (GridSize * 2), 0, Image.SizeX - 1);
            const int64 Offset = (static_cast<int64>(Y) * Image.SizeX + X) * BytesPerPixel;
            OutSignature.Add(ERawImageFormat::GetOnePixelLinear(Image.RawData.GetData() + Offset, Image.Format, Image.GammaSpace));
        }
    }
    return true;
}

float CompareSignatures(const TArray<FLinearColor>& A, const TArray<FLinearColor>& B)
{
    if (A.Num() != B.Num() || A.IsEmpty()) return 1.0f;
    float Total = 0.0f;
    for (int32 Index = 0; Index < A.Num(); ++Index)
    {
        const float ColorDelta = (FMath::Abs(A[Index].R - B[Index].R) + FMath::Abs(A[Index].G - B[Index].G) + FMath::Abs(A[Index].B - B[Index].B)) / 3.0f;
        const float LumaDelta = FMath::Abs(A[Index].GetLuminance() - B[Index].GetLuminance());
        Total += ColorDelta * 0.7f + LumaDelta * 0.3f;
    }
    return FMath::Clamp(Total / A.Num(), 0.0f, 1.0f);
}
}

bool FUPTShotBoundaryDetector::Detect(
    const TArray<FString>& FrameFiles,
    const float SampleIntervalSeconds,
    const float ChangeThreshold,
    const float MinimumShotSeconds,
    TArray<FUPTShotBoundary>& OutBoundaries,
    FString& OutError)
{
    OutBoundaries.Reset();
    if (FrameFiles.IsEmpty()) { OutError = TEXT("Shot 경계를 분석할 프레임이 없습니다."); return false; }
    TArray<FLinearColor> Previous;
    if (!BuildSignature(FrameFiles[0], Previous)) { OutError = TEXT("첫 대표 프레임을 디코딩하지 못했습니다."); return false; }
    OutBoundaries.Add({0, 0.0f, 1.0f});
    float LastBoundaryTime = 0.0f;
    for (int32 Index = 1; Index < FrameFiles.Num(); ++Index)
    {
        TArray<FLinearColor> Current;
        if (!BuildSignature(FrameFiles[Index], Current))
        {
            OutError = FString::Printf(TEXT("대표 프레임 %d을 디코딩하지 못했습니다."), Index + 1);
            return false;
        }
        const float Time = Index * SampleIntervalSeconds;
        const float Score = CompareSignatures(Previous, Current);
        if (Score >= ChangeThreshold && Time - LastBoundaryTime >= MinimumShotSeconds)
        {
            OutBoundaries.Add({Index, Time, Score});
            LastBoundaryTime = Time;
        }
        Previous = MoveTemp(Current);
    }
    return true;
}
