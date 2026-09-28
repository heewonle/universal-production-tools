#include "UPTPlanParser.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
bool ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& OutValue)
{
    const TSharedPtr<FJsonObject>* Value = nullptr;
    if (!Object->TryGetObjectField(Field, Value) || !Value || !Value->IsValid()) return false;
    double X = 0, Y = 0, Z = 0;
    if (!(*Value)->TryGetNumberField(TEXT("x"), X) || !(*Value)->TryGetNumberField(TEXT("y"), Y) || !(*Value)->TryGetNumberField(TEXT("z"), Z)) return false;
    OutValue = FVector(X, Y, Z);
    return true;
}

bool ReadRotator(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FRotator& OutValue)
{
    const TSharedPtr<FJsonObject>* Value = nullptr;
    if (!Object->TryGetObjectField(Field, Value) || !Value || !Value->IsValid()) return false;
    double Pitch = 0, Yaw = 0, Roll = 0;
    if (!(*Value)->TryGetNumberField(TEXT("pitch"), Pitch) || !(*Value)->TryGetNumberField(TEXT("yaw"), Yaw) || !(*Value)->TryGetNumberField(TEXT("roll"), Roll)) return false;
    OutValue = FRotator(Pitch, Yaw, Roll);
    return true;
}
}

bool FUPTPlanParser::Parse(const FString& JsonText, FUPTCinematicPlan& OutPlan, FString& OutError)
{
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) { OutError = TEXT("LLM 응답이 유효한 JSON 객체가 아닙니다."); return false; }
    Root->TryGetStringField(TEXT("title"), OutPlan.Title);
    OutPlan.Title.TrimStartAndEndInline();
    // 작은 로컬 모델은 제목을 비워 두는 경우가 있다. 제목은 에셋 이름에만 쓰이므로 생성을 막지 않고 기본값을 채운다.
    if (OutPlan.Title.IsEmpty()) OutPlan.Title = TEXT("GeneratedCinematic");
    double FrameRate = 30; Root->TryGetNumberField(TEXT("frame_rate"), FrameRate); OutPlan.FrameRate = FMath::RoundToInt(FrameRate);
    double AspectRatio = OutPlan.AspectRatio; Root->TryGetNumberField(TEXT("aspect_ratio"), AspectRatio); OutPlan.AspectRatio = static_cast<float>(AspectRatio);
    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (!Root->TryGetArrayField(TEXT("shots"), Shots) || !Shots) { OutError = TEXT("필수 shots 배열이 없습니다."); return false; }
    OutPlan.Shots.Reset();
    for (int32 Index = 0; Index < Shots->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> ShotObject = (*Shots)[Index]->AsObject();
        if (!ShotObject.IsValid()) { OutError = FString::Printf(TEXT("shots[%d]가 객체가 아닙니다."), Index); return false; }
        FUPTCinematicShot Shot;
        ShotObject->TryGetStringField(TEXT("name"), Shot.Name); ShotObject->TryGetStringField(TEXT("description"), Shot.Description);
        ShotObject->TryGetStringField(TEXT("subject"), Shot.Subject); ShotObject->TryGetStringField(TEXT("look_at"), Shot.LookAt);
        ShotObject->TryGetStringField(TEXT("animation_id"), Shot.AnimationId); ShotObject->TryGetStringField(TEXT("audio_id"), Shot.AudioId);
        ShotObject->TryGetStringField(TEXT("speaker"), Shot.Speaker); ShotObject->TryGetStringField(TEXT("dialogue"), Shot.Dialogue);
        ShotObject->TryGetStringField(TEXT("lip_sync_animation_id"), Shot.LipSyncAnimationId);
        ShotObject->TryGetStringField(TEXT("camera_motion"), Shot.CameraMotion);
        double Duration = 0, FocalLength = 50, Distance = 300, Height = 60, Side = 0;
        ShotObject->TryGetNumberField(TEXT("duration_seconds"), Duration); ShotObject->TryGetNumberField(TEXT("focal_length"), FocalLength);
        ShotObject->TryGetNumberField(TEXT("distance_cm"), Distance); ShotObject->TryGetNumberField(TEXT("height_cm"), Height); ShotObject->TryGetNumberField(TEXT("side_cm"), Side);
        Shot.DurationSeconds = Duration; Shot.FocalLength = FocalLength; Shot.DistanceCm = Distance; Shot.HeightCm = Height; Shot.SideCm = Side;
        double Roll = 0, ScreenX = 0.5, ScreenY = 0.5, FocusHeight = 0.5;
        ShotObject->TryGetNumberField(TEXT("camera_roll"), Roll); ShotObject->TryGetNumberField(TEXT("subject_screen_x"), ScreenX);
        ShotObject->TryGetNumberField(TEXT("subject_screen_y"), ScreenY); ShotObject->TryGetNumberField(TEXT("focus_height"), FocusHeight);
        Shot.CameraRollDegrees = Roll; Shot.SubjectScreenX = ScreenX; Shot.SubjectScreenY = ScreenY; Shot.FocusHeightRatio = FocusHeight;
        const bool bHasLocation = ReadVector(ShotObject, TEXT("camera_location"), Shot.CameraLocation);
        const bool bHasRotation = ReadRotator(ShotObject, TEXT("camera_rotation"), Shot.CameraRotation);
        // 레퍼런스 블로킹이 샷마다 옮겨 둔 등장인물 위치. 저장했다가 JSON으로 다시 생성해도 같은 배치가 나오도록 읽는다.
        const TArray<TSharedPtr<FJsonValue>>* Placements = nullptr;
        if (ShotObject->TryGetArrayField(TEXT("actor_placements"), Placements) && Placements)
        {
            for (const TSharedPtr<FJsonValue>& PlacementValue : *Placements)
            {
                const TSharedPtr<FJsonObject> PlacementObject = PlacementValue->AsObject();
                FUPTActorBlockingPlacement Placement;
                if (!PlacementObject.IsValid() || !PlacementObject->TryGetStringField(TEXT("actor"), Placement.ActorLabel)
                    || !ReadVector(PlacementObject, TEXT("location"), Placement.WorldLocation))
                {
                    continue;
                }
                ReadRotator(PlacementObject, TEXT("rotation"), Placement.WorldRotation);
                Shot.ActorPlacements.Add(MoveTemp(Placement));
            }
        }
        const TSharedPtr<FJsonObject>* OverShoulder = nullptr;
        if (ShotObject->TryGetObjectField(TEXT("over_shoulder"), OverShoulder) && OverShoulder && OverShoulder->IsValid())
        {
            FString OverShoulderActor, OverShoulderSide; double EdgeX = 0.85;
            (*OverShoulder)->TryGetStringField(TEXT("actor"), OverShoulderActor);
            (*OverShoulder)->TryGetStringField(TEXT("side"), OverShoulderSide);
            (*OverShoulder)->TryGetNumberField(TEXT("edge_x"), EdgeX);
            if (!OverShoulderActor.IsEmpty())
            {
                Shot.OverShoulderActor = OverShoulderActor;
                Shot.OverShoulderSide = OverShoulderSide == TEXT("left") ? TEXT("left") : TEXT("right");
                Shot.OverShoulderEdgeX = FMath::Clamp(static_cast<float>(EdgeX), 0.02f, 0.98f);
            }
        }
        if (Shot.Subject.IsEmpty() && (!bHasLocation || !bHasRotation))
        { OutError = FString::Printf(TEXT("shots[%d]에 subject 또는 절대 카메라 Transform이 필요합니다."), Index); return false; }
        OutPlan.Shots.Add(MoveTemp(Shot));
    }
    return true;
}

FUPTPlanValidationResult FUPTPlanParser::Validate(const FUPTCinematicPlan& Plan)
{
    FUPTPlanValidationResult Result;
    if (Plan.Title.TrimStartAndEnd().IsEmpty()) Result.Errors.Add(TEXT("title이 비어 있습니다."));
    if (Plan.FrameRate < 1 || Plan.FrameRate > 120) Result.Errors.Add(TEXT("frame_rate는 1~120이어야 합니다."));
    if (!FMath::IsFinite(Plan.AspectRatio) || Plan.AspectRatio < 0.25f || Plan.AspectRatio > 4.0f) Result.Errors.Add(TEXT("aspect_ratio는 0.25~4.0이어야 합니다."));
    if (Plan.Shots.IsEmpty()) Result.Errors.Add(TEXT("최소 한 개의 Shot이 필요합니다."));
    if (Plan.Shots.Num() > 100) Result.Errors.Add(TEXT("한 번에 생성 가능한 Shot은 최대 100개입니다."));
    float TotalDuration = 0;
    for (int32 Index = 0; Index < Plan.Shots.Num(); ++Index)
    {
        const FUPTCinematicShot& Shot = Plan.Shots[Index];
        if (Shot.Name.TrimStartAndEnd().IsEmpty()) Result.Errors.Add(FString::Printf(TEXT("Shot %d의 이름이 비어 있습니다."), Index + 1));
        if (Shot.DurationSeconds < 0.1f || Shot.DurationSeconds > 120.0f) Result.Errors.Add(FString::Printf(TEXT("%s: 길이는 0.1~120초여야 합니다."), *Shot.Name));
        if (Shot.FocalLength < 8.0f || Shot.FocalLength > 200.0f) Result.Errors.Add(FString::Printf(TEXT("%s: 초점거리는 8~200mm여야 합니다."), *Shot.Name));
        if (!Shot.AnimationId.IsEmpty() && Shot.Subject.IsEmpty()) Result.Errors.Add(FString::Printf(TEXT("%s: animation_id에는 subject가 필요합니다."), *Shot.Name));
        if (Shot.DistanceCm < 30.0f || Shot.DistanceCm > 10000.0f) Result.Errors.Add(FString::Printf(TEXT("%s: 상대 카메라 거리는 30~10000cm여야 합니다."), *Shot.Name));
        if (FMath::Abs(Shot.HeightCm) > 5000.0f || FMath::Abs(Shot.SideCm) > 5000.0f) Result.Errors.Add(FString::Printf(TEXT("%s: 상대 카메라 오프셋이 허용 범위를 벗어났습니다."), *Shot.Name));
        if (Shot.SubjectScreenX < 0.0f || Shot.SubjectScreenX > 1.0f || Shot.SubjectScreenY < 0.0f || Shot.SubjectScreenY > 1.0f) Result.Errors.Add(FString::Printf(TEXT("%s: subject_screen_x/y는 0~1이어야 합니다."), *Shot.Name));
        if (Shot.FocusHeightRatio < 0.0f || Shot.FocusHeightRatio > 1.0f) Result.Errors.Add(FString::Printf(TEXT("%s: focus_height는 0~1이어야 합니다."), *Shot.Name));
        if (Shot.CameraLocation.Size() > 1000000.0f) Result.Errors.Add(FString::Printf(TEXT("%s: 카메라 위치가 허용 범위를 벗어났습니다."), *Shot.Name));
        if (!Shot.LipSyncAnimationId.IsEmpty() && Shot.Subject.IsEmpty()) Result.Errors.Add(FString::Printf(TEXT("%s: lip_sync_animation_id에는 subject가 필요합니다."), *Shot.Name));
        if (!Shot.Dialogue.IsEmpty() && Shot.AudioId.IsEmpty()) Result.Warnings.Add(FString::Printf(TEXT("%s: 대사는 있지만 audio_id가 없습니다."), *Shot.Name));
        TotalDuration += Shot.DurationSeconds;
    }
    if (TotalDuration > 1800.0f) Result.Errors.Add(TEXT("전체 길이는 30분을 초과할 수 없습니다."));
    Result.bValid = Result.Errors.IsEmpty();
    return Result;
}

FString FUPTPlanParser::ToJson(const FUPTCinematicPlan& Plan)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("title"), Plan.Title);
    Root->SetNumberField(TEXT("frame_rate"), Plan.FrameRate);
    Root->SetNumberField(TEXT("aspect_ratio"), Plan.AspectRatio);
    TArray<TSharedPtr<FJsonValue>> ShotValues;
    for (const FUPTCinematicShot& Shot : Plan.Shots)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("name"), Shot.Name); Object->SetStringField(TEXT("description"), Shot.Description);
        Object->SetStringField(TEXT("subject"), Shot.Subject); Object->SetStringField(TEXT("look_at"), Shot.LookAt);
        Object->SetStringField(TEXT("animation_id"), Shot.AnimationId); Object->SetStringField(TEXT("audio_id"), Shot.AudioId);
        Object->SetStringField(TEXT("speaker"), Shot.Speaker); Object->SetStringField(TEXT("dialogue"), Shot.Dialogue);
        Object->SetStringField(TEXT("lip_sync_animation_id"), Shot.LipSyncAnimationId);
        Object->SetNumberField(TEXT("duration_seconds"), Shot.DurationSeconds); Object->SetNumberField(TEXT("focal_length"), Shot.FocalLength);
        Object->SetNumberField(TEXT("distance_cm"), Shot.DistanceCm); Object->SetNumberField(TEXT("height_cm"), Shot.HeightCm); Object->SetNumberField(TEXT("side_cm"), Shot.SideCm);
        Object->SetNumberField(TEXT("focus_height"), Shot.FocusHeightRatio);
        Object->SetNumberField(TEXT("subject_screen_x"), Shot.SubjectScreenX); Object->SetNumberField(TEXT("subject_screen_y"), Shot.SubjectScreenY);
        Object->SetStringField(TEXT("camera_motion"), Shot.CameraMotion); Object->SetNumberField(TEXT("camera_roll"), Shot.CameraRollDegrees);
        TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), Shot.CameraLocation.X); Location->SetNumberField(TEXT("y"), Shot.CameraLocation.Y); Location->SetNumberField(TEXT("z"), Shot.CameraLocation.Z);
        Object->SetObjectField(TEXT("camera_location"), Location);
        TSharedRef<FJsonObject> Rotation = MakeShared<FJsonObject>();
        Rotation->SetNumberField(TEXT("pitch"), Shot.CameraRotation.Pitch); Rotation->SetNumberField(TEXT("yaw"), Shot.CameraRotation.Yaw); Rotation->SetNumberField(TEXT("roll"), Shot.CameraRotation.Roll);
        Object->SetObjectField(TEXT("camera_rotation"), Rotation);
        TArray<TSharedPtr<FJsonValue>> PlacementValues;
        for (const FUPTActorBlockingPlacement& Placement : Shot.ActorPlacements)
        {
            TSharedRef<FJsonObject> PlacementObject = MakeShared<FJsonObject>();
            PlacementObject->SetStringField(TEXT("actor"), Placement.ActorLabel);
            TSharedRef<FJsonObject> PlacementLocation = MakeShared<FJsonObject>();
            PlacementLocation->SetNumberField(TEXT("x"), Placement.WorldLocation.X); PlacementLocation->SetNumberField(TEXT("y"), Placement.WorldLocation.Y); PlacementLocation->SetNumberField(TEXT("z"), Placement.WorldLocation.Z);
            PlacementObject->SetObjectField(TEXT("location"), PlacementLocation);
            TSharedRef<FJsonObject> PlacementRotation = MakeShared<FJsonObject>();
            PlacementRotation->SetNumberField(TEXT("pitch"), Placement.WorldRotation.Pitch); PlacementRotation->SetNumberField(TEXT("yaw"), Placement.WorldRotation.Yaw); PlacementRotation->SetNumberField(TEXT("roll"), Placement.WorldRotation.Roll);
            PlacementObject->SetObjectField(TEXT("rotation"), PlacementRotation);
            PlacementValues.Add(MakeShared<FJsonValueObject>(PlacementObject));
        }
        if (!PlacementValues.IsEmpty()) Object->SetArrayField(TEXT("actor_placements"), PlacementValues);
        if (!Shot.OverShoulderActor.IsEmpty())
        {
            TSharedRef<FJsonObject> OverShoulder = MakeShared<FJsonObject>();
            OverShoulder->SetStringField(TEXT("actor"), Shot.OverShoulderActor);
            OverShoulder->SetStringField(TEXT("side"), Shot.OverShoulderSide);
            OverShoulder->SetNumberField(TEXT("edge_x"), Shot.OverShoulderEdgeX);
            Object->SetObjectField(TEXT("over_shoulder"), OverShoulder);
        }
        ShotValues.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("shots"), ShotValues);
    FString Output;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
    FJsonSerializer::Serialize(Root, Writer);
    return Output;
}

FString FUPTPlanParser::ToPreviewText(const FUPTCinematicPlan& Plan)
{
    FString Output = FString::Printf(TEXT("%s | %d fps | %.3f:1 | %d shots\n\n"), *Plan.Title, Plan.FrameRate, Plan.AspectRatio, Plan.Shots.Num());
    float Time = 0;
    for (int32 Index = 0; Index < Plan.Shots.Num(); ++Index)
    {
        const FUPTCinematicShot& Shot = Plan.Shots[Index];
        Output += FString::Printf(TEXT("%02d  %6.2fs-%6.2fs  %s  %.1fmm\n    Subject: %s / LookAt: %s\n    Animation: %s / Audio: %s\n"), Index + 1, Time, Time + Shot.DurationSeconds, *Shot.Name, Shot.FocalLength, Shot.Subject.IsEmpty() ? TEXT("<absolute>") : *Shot.Subject, Shot.LookAt.IsEmpty() ? TEXT("<subject>") : *Shot.LookAt, Shot.AnimationId.IsEmpty() ? TEXT("-") : *Shot.AnimationId, Shot.AudioId.IsEmpty() ? TEXT("-") : *Shot.AudioId);
        if (!Shot.Subject.IsEmpty())
        {
            Output += FString::Printf(TEXT("    Camera: 정면 %.0fcm / 측면 %.0fcm / 높이 %.0fcm | 조준 높이 %.2f | 화면 위치 (%.2f, %.2f) | 모션 %s\n"),
                Shot.DistanceCm, Shot.SideCm, Shot.HeightCm, Shot.FocusHeightRatio, Shot.SubjectScreenX, Shot.SubjectScreenY, Shot.CameraMotion.IsEmpty() ? TEXT("static") : *Shot.CameraMotion);
        }
        Output += FString::Printf(TEXT("    %s\n"), *Shot.Description);
        Time += Shot.DurationSeconds;
    }
    return Output;
}
