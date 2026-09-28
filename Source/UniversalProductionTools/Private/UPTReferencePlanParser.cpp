#include "UPTReferencePlanParser.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool FUPTReferencePlanParser::Parse(const FString& Json, FUPTReferencePlan& OutPlan, FString& OutError)
{
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
    { OutError = TEXT("Reference Shot Plan이 유효한 JSON이 아닙니다."); return false; }
    double SchemaVersion = 1;
    Root->TryGetNumberField(TEXT("schema_version"), SchemaVersion);
    OutPlan.SchemaVersion = FMath::RoundToInt(SchemaVersion);
    if (OutPlan.SchemaVersion < 1 || OutPlan.SchemaVersion > 1)
    { OutError = FString::Printf(TEXT("Unsupported Reference Plan schema version: %d"), OutPlan.SchemaVersion); return false; }
    Root->TryGetStringField(TEXT("title"), OutPlan.Title);
    Root->TryGetStringField(TEXT("match_mode"), OutPlan.MatchMode);
    Root->TryGetStringField(TEXT("vision_model"), OutPlan.VisionModel);
    Root->TryGetStringField(TEXT("analysis_fingerprint"), OutPlan.AnalysisFingerprint);
    double AspectRatio = OutPlan.AspectRatio;
    if (Root->TryGetNumberField(TEXT("aspect_ratio"), AspectRatio)) OutPlan.AspectRatio = FMath::Clamp(static_cast<float>(AspectRatio), 0.25f, 4.0f);
    if (OutPlan.MatchMode != TEXT("scene") && OutPlan.MatchMode != TEXT("edit"))
    { OutError = TEXT("match_mode는 scene 또는 edit여야 합니다."); return false; }
    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (!Root->TryGetArrayField(TEXT("shots"), Shots) || !Shots || Shots->IsEmpty())
    { OutError = TEXT("Reference Shot Plan에 shots 배열이 없습니다."); return false; }
    OutPlan.Shots.Reset();
    if (Shots->Num() > 120) { OutError = TEXT("Reference Shot Plan은 최대 120개 Shot만 지원합니다."); return false; }
    const TSet<FString> AllowedShotSizes = { TEXT("wide"), TEXT("full"), TEXT("medium"), TEXT("close_up"), TEXT("extreme_close_up") };
    const TSet<FString> AllowedAngles = { TEXT("low"), TEXT("eye"), TEXT("high"), TEXT("overhead"), TEXT("dutch") };
    const TSet<FString> AllowedMotions = { TEXT("static"), TEXT("pan"), TEXT("tilt"), TEXT("dolly_in"), TEXT("dolly_out"), TEXT("zoom_in"), TEXT("zoom_out"), TEXT("truck_left"), TEXT("truck_right"), TEXT("pedestal"), TEXT("orbit"), TEXT("tracking"), TEXT("handheld") };
    const TSet<FString> AllowedCompositions = { TEXT("center"), TEXT("thirds"), TEXT("symmetry"), TEXT("leading_space") };
    float PreviousEnd = 0.0f;
    for (int32 Index = 0; Index < Shots->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> Object = (*Shots)[Index]->AsObject();
        if (!Object.IsValid()) { OutError = FString::Printf(TEXT("shots[%d]가 객체가 아닙니다."), Index); return false; }
        FUPTReferenceShot Shot;
        Object->TryGetStringField(TEXT("name"), Shot.Name); Object->TryGetStringField(TEXT("subject_role"), Shot.SubjectRole);
        Object->TryGetStringField(TEXT("shot_size"), Shot.ShotSize); Object->TryGetStringField(TEXT("camera_angle"), Shot.CameraAngle);
        Object->TryGetStringField(TEXT("camera_motion"), Shot.CameraMotion); Object->TryGetStringField(TEXT("composition"), Shot.Composition);
        double Start = 0, End = 0, Confidence = 0;
        Object->TryGetNumberField(TEXT("start_seconds"), Start); Object->TryGetNumberField(TEXT("end_seconds"), End); Object->TryGetNumberField(TEXT("confidence"), Confidence);
        Shot.StartSeconds = Start; Shot.EndSeconds = End; Shot.Confidence = FMath::Clamp(static_cast<float>(Confidence), 0.0f, 1.0f);
        const TSharedPtr<FJsonObject>* Position = nullptr;
        if (Object->TryGetObjectField(TEXT("subject_screen_position"), Position) && Position && Position->IsValid())
        { double X = .5, Y = .5; (*Position)->TryGetNumberField(TEXT("x"), X); (*Position)->TryGetNumberField(TEXT("y"), Y); Shot.SubjectScreenPosition = FVector2D(X, Y); }
        const TSharedPtr<FJsonObject>* Size = nullptr;
        if (Object->TryGetObjectField(TEXT("subject_screen_size"), Size) && Size && Size->IsValid())
        { double X = .3, Y = .5; (*Size)->TryGetNumberField(TEXT("width"), X); (*Size)->TryGetNumberField(TEXT("height"), Y); Shot.SubjectScreenSize = FVector2D(X, Y); }
        const TArray<TSharedPtr<FJsonValue>>* Subjects = nullptr;
        if (Object->TryGetArrayField(TEXT("subjects"), Subjects) && Subjects)
        {
            for (const TSharedPtr<FJsonValue>& SubjectValue : *Subjects)
            {
                const TSharedPtr<FJsonObject> SubjectObject = SubjectValue->AsObject();
                if (!SubjectObject.IsValid()) continue;
                FUPTReferenceSubject Subject;
                SubjectObject->TryGetStringField(TEXT("role"), Subject.Role);
                double Depth = 0; SubjectObject->TryGetNumberField(TEXT("depth_order"), Depth); Subject.DepthOrder = FMath::RoundToInt(Depth);
                const TSharedPtr<FJsonObject>* SubjectPosition = nullptr;
                if (SubjectObject->TryGetObjectField(TEXT("screen_position"), SubjectPosition) && SubjectPosition && SubjectPosition->IsValid())
                { double X=.5,Y=.5; (*SubjectPosition)->TryGetNumberField(TEXT("x"),X); (*SubjectPosition)->TryGetNumberField(TEXT("y"),Y); Subject.ScreenPosition=FVector2D(X,Y); }
                const TSharedPtr<FJsonObject>* SubjectSize = nullptr;
                if (SubjectObject->TryGetObjectField(TEXT("screen_size"), SubjectSize) && SubjectSize && SubjectSize->IsValid())
                { double X=.25,Y=.5; (*SubjectSize)->TryGetNumberField(TEXT("width"),X); (*SubjectSize)->TryGetNumberField(TEXT("height"),Y); Subject.ScreenSize=FVector2D(X,Y); }
                if (!Subject.Role.IsEmpty() && Subject.ScreenPosition.X >= 0 && Subject.ScreenPosition.X <= 1 && Subject.ScreenPosition.Y >= 0 && Subject.ScreenPosition.Y <= 1) Shot.Subjects.Add(MoveTemp(Subject));
            }
        }
        const TSharedPtr<FJsonObject>* OverShoulder = nullptr;
        if (Object->TryGetObjectField(TEXT("over_the_shoulder"), OverShoulder) && OverShoulder && OverShoulder->IsValid())
        {
            FString Side; double X = 0.0, Width = 0.0;
            (*OverShoulder)->TryGetStringField(TEXT("side"), Side);
            (*OverShoulder)->TryGetNumberField(TEXT("screen_x"), X); (*OverShoulder)->TryGetNumberField(TEXT("screen_width"), Width);
            if (Side == TEXT("left") || Side == TEXT("right"))
            {
                Shot.OverShoulderSide = Side;
                Shot.OverShoulderScreenX = FMath::Clamp(static_cast<float>(X), 0.0f, 1.0f);
                Shot.OverShoulderScreenWidth = FMath::Clamp(static_cast<float>(Width), 0.0f, 1.0f);
            }
        }
        if (Shot.Subjects.IsEmpty() && !Shot.SubjectRole.IsEmpty())
        { FUPTReferenceSubject Subject; Subject.Role=Shot.SubjectRole; Subject.ScreenPosition=Shot.SubjectScreenPosition; Subject.ScreenSize=Shot.SubjectScreenSize; Shot.Subjects.Add(MoveTemp(Subject)); }
        if (Shot.Name.IsEmpty() || Shot.EndSeconds <= Shot.StartSeconds || Shot.StartSeconds < PreviousEnd - KINDA_SMALL_NUMBER || Shot.EndSeconds > 7200.0f ||
            Shot.SubjectScreenPosition.X < 0 || Shot.SubjectScreenPosition.X > 1 || Shot.SubjectScreenPosition.Y < 0 || Shot.SubjectScreenPosition.Y > 1 ||
            Shot.SubjectScreenSize.X <= 0 || Shot.SubjectScreenSize.X > 1 || Shot.SubjectScreenSize.Y <= 0 || Shot.SubjectScreenSize.Y > 1)
        { OutError = FString::Printf(TEXT("shots[%d]의 이름·시간·화면 좌표가 유효하지 않습니다."), Index); return false; }
        if (!AllowedShotSizes.Contains(Shot.ShotSize) || !AllowedAngles.Contains(Shot.CameraAngle) || !AllowedMotions.Contains(Shot.CameraMotion) || !AllowedCompositions.Contains(Shot.Composition))
        { OutError = FString::Printf(TEXT("shots[%d]에 지원하지 않는 샷 크기·각도·모션·구도 값이 있습니다."), Index); return false; }
        PreviousEnd = Shot.EndSeconds;
        OutPlan.Shots.Add(MoveTemp(Shot));
    }
    return true;
}

FString FUPTReferencePlanParser::ToPreviewText(const FUPTReferencePlan& Plan)
{
    FString Text = FString::Printf(TEXT("%s | %s match | %d shots | aspect %.3f:1\n\n"), *Plan.Title, *Plan.MatchMode, Plan.Shots.Num(), Plan.AspectRatio);
    for (int32 Index = 0; Index < Plan.Shots.Num(); ++Index)
    {
        const FUPTReferenceShot& Shot = Plan.Shots[Index];
        Text += FString::Printf(TEXT("%02d  %.2f-%.2fs  %s\n    role=%s | %s / %s / %s | screen=(%.2f, %.2f) size=(%.2f, %.2f) | confidence %.0f%%\n"),
            Index + 1, Shot.StartSeconds, Shot.EndSeconds, *Shot.Name, *Shot.SubjectRole, *Shot.ShotSize, *Shot.CameraAngle, *Shot.CameraMotion,
            Shot.SubjectScreenPosition.X, Shot.SubjectScreenPosition.Y, Shot.SubjectScreenSize.X, Shot.SubjectScreenSize.Y, Shot.Confidence * 100.0f);
        if (Shot.Subjects.Num() > 1) Text += FString::Printf(TEXT("    multi-subject blocking: %d roles\n"), Shot.Subjects.Num());
    }
    return Text;
}

bool FUPTReferencePlanParser::ToJson(const FUPTReferencePlan& Plan, FString& OutJson)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetNumberField(TEXT("schema_version"), Plan.SchemaVersion);
    Root->SetStringField(TEXT("title"), Plan.Title);
    Root->SetStringField(TEXT("match_mode"), Plan.MatchMode);
    Root->SetStringField(TEXT("vision_model"), Plan.VisionModel);
    Root->SetStringField(TEXT("analysis_fingerprint"), Plan.AnalysisFingerprint);
    Root->SetNumberField(TEXT("aspect_ratio"), Plan.AspectRatio);
    TArray<TSharedPtr<FJsonValue>> Shots;
    for (const FUPTReferenceShot& Shot : Plan.Shots)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("name"), Shot.Name);
        Object->SetStringField(TEXT("subject_role"), Shot.SubjectRole);
        Object->SetNumberField(TEXT("start_seconds"), Shot.StartSeconds);
        Object->SetNumberField(TEXT("end_seconds"), Shot.EndSeconds);
        Object->SetStringField(TEXT("shot_size"), Shot.ShotSize);
        Object->SetStringField(TEXT("camera_angle"), Shot.CameraAngle);
        Object->SetStringField(TEXT("camera_motion"), Shot.CameraMotion);
        Object->SetStringField(TEXT("composition"), Shot.Composition);
        Object->SetNumberField(TEXT("confidence"), Shot.Confidence);
        TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
        Position->SetNumberField(TEXT("x"), Shot.SubjectScreenPosition.X); Position->SetNumberField(TEXT("y"), Shot.SubjectScreenPosition.Y);
        Object->SetObjectField(TEXT("subject_screen_position"), Position);
        TSharedRef<FJsonObject> Size = MakeShared<FJsonObject>();
        Size->SetNumberField(TEXT("width"), Shot.SubjectScreenSize.X); Size->SetNumberField(TEXT("height"), Shot.SubjectScreenSize.Y);
        Object->SetObjectField(TEXT("subject_screen_size"), Size);
        TArray<TSharedPtr<FJsonValue>> Subjects;
        for (const FUPTReferenceSubject& Subject : Shot.Subjects)
        {
            TSharedRef<FJsonObject> SubjectObject = MakeShared<FJsonObject>();
            SubjectObject->SetStringField(TEXT("role"), Subject.Role);
            SubjectObject->SetNumberField(TEXT("depth_order"), Subject.DepthOrder);
            TSharedRef<FJsonObject> SubjectPosition = MakeShared<FJsonObject>();
            SubjectPosition->SetNumberField(TEXT("x"), Subject.ScreenPosition.X); SubjectPosition->SetNumberField(TEXT("y"), Subject.ScreenPosition.Y);
            SubjectObject->SetObjectField(TEXT("screen_position"), SubjectPosition);
            TSharedRef<FJsonObject> SubjectSize = MakeShared<FJsonObject>();
            SubjectSize->SetNumberField(TEXT("width"), Subject.ScreenSize.X); SubjectSize->SetNumberField(TEXT("height"), Subject.ScreenSize.Y);
            SubjectObject->SetObjectField(TEXT("screen_size"), SubjectSize);
            Subjects.Add(MakeShared<FJsonValueObject>(SubjectObject));
        }
        Object->SetArrayField(TEXT("subjects"), Subjects);
        if (!Shot.OverShoulderSide.IsEmpty())
        {
            TSharedRef<FJsonObject> OverShoulder = MakeShared<FJsonObject>();
            OverShoulder->SetStringField(TEXT("side"), Shot.OverShoulderSide);
            OverShoulder->SetNumberField(TEXT("screen_x"), Shot.OverShoulderScreenX);
            OverShoulder->SetNumberField(TEXT("screen_width"), Shot.OverShoulderScreenWidth);
            Object->SetObjectField(TEXT("over_the_shoulder"), OverShoulder);
        }
        Shots.Add(MakeShared<FJsonValueObject>(Object));
    }
    Root->SetArrayField(TEXT("shots"), Shots);
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
    return FJsonSerializer::Serialize(Root, Writer);
}
