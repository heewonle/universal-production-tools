#include "UPTSettings.h"

UUPTSettings::UUPTSettings()
{
    CategoryName = TEXT("Plugins");
    ApiEndpoint = TEXT("https://api.openai.com/v1/chat/completions");
    Model = TEXT("gpt-5.6-sol");
    ApiKeyEnvironmentVariable = TEXT("UPT_LLM_API_KEY");
    DefaultSequencePath = TEXT("/Game/Cinematics/Generated");
    DefaultSkeletonProfilePath = TEXT("/Game/Animation/UPT/Profiles");
    DefaultIKRigPath = TEXT("/Game/Animation/UPT/IKRigs");
    DefaultRetargeterPath = TEXT("/Game/Animation/UPT/Retargeters");
    DefaultRetargetedAnimationPath = TEXT("/Game/Animation/UPT/Retargeted");
    FFmpegExecutablePath = TEXT("ffmpeg.exe");
    VisionApiEndpoint = TEXT("https://api.openai.com/v1/responses");
    VisionModel = TEXT("gpt-5.6-sol");
}
