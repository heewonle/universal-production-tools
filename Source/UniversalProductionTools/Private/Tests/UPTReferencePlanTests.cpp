#include "Misc/AutomationTest.h"
#include "UPTReferenceBlockingSolver.h"
#include "UPTReferencePlanParser.h"
#include "UPTBlockingValidator.h"
#include "UPTEndpoint.h"
#include "UPTFraming.h"
#include "UPTPlanParser.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUPTReferencePlanRoundTripTest, "UniversalProductionTools.ReferencePlan.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUPTReferencePlanRoundTripTest::RunTest(const FString& Parameters)
{
    FUPTReferencePlan Source; Source.Title = TEXT("VerticalReference"); Source.MatchMode = TEXT("edit"); Source.VisionModel = TEXT("test-model"); Source.AspectRatio = 9.0f / 16.0f;
    FUPTReferenceShot Shot; Shot.Name = TEXT("Shot_01"); Shot.SubjectRole = TEXT("hero"); Shot.StartSeconds = 0.0f; Shot.EndSeconds = 2.5f; Shot.ShotSize = TEXT("medium"); Shot.CameraAngle = TEXT("eye"); Shot.CameraMotion = TEXT("zoom_in"); Shot.Composition = TEXT("center"); Shot.Confidence = 0.9f;
    FUPTReferenceSubject Subject; Subject.Role = TEXT("hero"); Shot.Subjects.Add(Subject); Source.Shots.Add(Shot);
    FString Json; TestTrue(TEXT("Serialize succeeds"), FUPTReferencePlanParser::ToJson(Source, Json));
    FUPTReferencePlan Parsed; FString Error; TestTrue(TEXT("Parse succeeds"), FUPTReferencePlanParser::Parse(Json, Parsed, Error));
    TestEqual(TEXT("Schema version"), Parsed.SchemaVersion, 1); TestEqual(TEXT("Match mode"), Parsed.MatchMode, FString(TEXT("edit"))); TestEqual(TEXT("Vision model"), Parsed.VisionModel, Source.VisionModel);
    TestTrue(TEXT("Aspect ratio"), FMath::IsNearlyEqual(Parsed.AspectRatio, Source.AspectRatio)); TestEqual(TEXT("Shot count"), Parsed.Shots.Num(), 1); TestEqual(TEXT("Motion"), Parsed.Shots[0].CameraMotion, FString(TEXT("zoom_in"))); TestEqual(TEXT("Subject count"), Parsed.Shots[0].Subjects.Num(), 1); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUPTReferencePlanVersionTest, "UniversalProductionTools.ReferencePlan.VersionValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUPTReferencePlanVersionTest::RunTest(const FString& Parameters)
{
    const FString FutureJson = TEXT("{\"schema_version\":99,\"title\":\"Future\",\"match_mode\":\"scene\",\"shots\":[{\"name\":\"S\",\"start_seconds\":0,\"end_seconds\":1}]}");
    FUPTReferencePlan Parsed; FString Error; TestFalse(TEXT("Future schema rejected"), FUPTReferencePlanParser::Parse(FutureJson, Parsed, Error)); TestTrue(TEXT("Version error reported"), Error.Contains(TEXT("version")));
    const FString LegacyJson = TEXT("{\"title\":\"Legacy\",\"match_mode\":\"scene\",\"shots\":[{\"name\":\"S\",\"start_seconds\":0,\"end_seconds\":1,\"shot_size\":\"medium\",\"camera_angle\":\"eye\",\"camera_motion\":\"static\",\"composition\":\"center\",\"subject_screen_position\":{\"x\":0.5,\"y\":0.5},\"subject_screen_size\":{\"width\":0.3,\"height\":0.5}}]}");
    Error.Reset(); TestTrue(TEXT("Unversioned schema defaults to v1"), FUPTReferencePlanParser::Parse(LegacyJson, Parsed, Error)); TestEqual(TEXT("Legacy version"), Parsed.SchemaVersion, 1); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUPTReferencePlanStrictValidationTest, "UniversalProductionTools.ReferencePlan.StrictValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUPTReferencePlanStrictValidationTest::RunTest(const FString& Parameters)
{
    const FString InvalidMotion = TEXT("{\"title\":\"Invalid\",\"match_mode\":\"scene\",\"shots\":[{\"name\":\"S\",\"start_seconds\":0,\"end_seconds\":1,\"shot_size\":\"medium\",\"camera_angle\":\"eye\",\"camera_motion\":\"teleport\",\"composition\":\"center\",\"subject_screen_position\":{\"x\":0.5,\"y\":0.5},\"subject_screen_size\":{\"width\":0.3,\"height\":0.5}}]}");
    FUPTReferencePlan Parsed; FString Error; TestFalse(TEXT("Unknown motion rejected"), FUPTReferencePlanParser::Parse(InvalidMotion, Parsed, Error));
    const FString Overlap = TEXT("{\"title\":\"Overlap\",\"match_mode\":\"scene\",\"shots\":[{\"name\":\"A\",\"start_seconds\":0,\"end_seconds\":2,\"shot_size\":\"medium\",\"camera_angle\":\"eye\",\"camera_motion\":\"static\",\"composition\":\"center\",\"subject_screen_position\":{\"x\":0.5,\"y\":0.5},\"subject_screen_size\":{\"width\":0.3,\"height\":0.5}},{\"name\":\"B\",\"start_seconds\":1,\"end_seconds\":3,\"shot_size\":\"medium\",\"camera_angle\":\"eye\",\"camera_motion\":\"static\",\"composition\":\"center\",\"subject_screen_position\":{\"x\":0.5,\"y\":0.5},\"subject_screen_size\":{\"width\":0.3,\"height\":0.5}}]}");
    Error.Reset(); TestFalse(TEXT("Overlapping shots rejected"), FUPTReferencePlanParser::Parse(Overlap, Parsed, Error)); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUPTReferenceRoleCapacityTest, "UniversalProductionTools.ReferencePlan.RoleCapacity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUPTReferenceRoleCapacityTest::RunTest(const FString& Parameters)
{
    FUPTReferencePlan Reference;
    FUPTReferenceShot A; A.Name = TEXT("A"); A.SubjectRole = TEXT("hero"); A.EndSeconds = 1.0f; Reference.Shots.Add(A);
    FUPTReferenceShot B; B.Name = TEXT("B"); B.SubjectRole = TEXT("enemy"); B.StartSeconds = 1.0f; B.EndSeconds = 2.0f; Reference.Shots.Add(B);
    TMap<FString, TWeakObjectPtr<AActor>> Actors; Actors.Add(TEXT("Hero"), TWeakObjectPtr<AActor>());
    FUPTCinematicPlan Plan; FString Report;
    // 영상 속 인물이 선택 Actor보다 많아도 생성은 계속하고, Actor를 재사용했다는 경고를 남긴다.
    TestTrue(TEXT("Insufficient actors reuse an assigned actor"), FUPTReferenceBlockingSolver::BuildPlan(Reference, Actors, {}, Plan, Report));
    TestTrue(TEXT("Reuse is reported"), Report.Contains(TEXT("재사용")));
    TestEqual(TEXT("Both shots keep a subject"), Plan.Shots.Num() == 2 && Plan.Shots[1].Subject == TEXT("Hero"), true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUPTFramingScreenPlacementTest, "UniversalProductionTools.Framing.ScreenPlacement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUPTFramingScreenPlacementTest::RunTest(const FString& Parameters)
{
    const FVector InFront = UPTFraming::ComputeDesiredCameraLocation(nullptr, 0.5f, 300.0f, 0.0f, 0.0f);
    TestTrue(TEXT("distance_cm places the camera in front of the subject"), InFront.X > 0.0f);

    const FVector Camera(0.0f, 0.0f, 150.0f);
    const FVector Target(400.0f, 120.0f, 60.0f);
    const float FocalLength = 35.0f;
    const float AspectRatio = 16.0f / 9.0f;
    const FVector2D DesiredPositions[] = { FVector2D(0.33, 0.4), FVector2D(0.66, 0.6), FVector2D(0.5, 0.5) };
    for (const FVector2D& Desired : DesiredPositions)
    {
        const FRotator Rotation = UPTFraming::ComputeFramingRotation(Camera, Target, Desired.X, Desired.Y, FocalLength, AspectRatio, 0.0f);
        FVector2D Actual;
        TestTrue(TEXT("Target stays in front of camera"), UPTFraming::ProjectToScreen(Camera, Rotation, Target, FocalLength, AspectRatio, Actual));
        TestTrue(FString::Printf(TEXT("Screen X %.2f -> %.3f"), Desired.X, Actual.X), FMath::IsNearlyEqual(Actual.X, Desired.X, 0.01));
        TestTrue(FString::Printf(TEXT("Screen Y %.2f -> %.3f"), Desired.Y, Actual.Y), FMath::IsNearlyEqual(Actual.Y, Desired.Y, 0.01));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUPTFramingPlanRoundTripTest, "UniversalProductionTools.Framing.PlanRoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUPTFramingPlanRoundTripTest::RunTest(const FString& Parameters)
{
    FUPTCinematicPlan Plan; Plan.Title = TEXT("Framing"); Plan.AspectRatio = 2.39f;
    FUPTCinematicShot Shot; Shot.Name = TEXT("S"); Shot.Subject = TEXT("Hero"); Shot.SubjectScreenX = 0.33f; Shot.SubjectScreenY = 0.4f; Shot.FocusHeightRatio = 0.88f; Shot.CameraMotion = TEXT("dolly_in"); Shot.CameraRollDegrees = 8.0f;
    FUPTActorBlockingPlacement Placement; Placement.ActorLabel = TEXT("Partner"); Placement.WorldLocation = FVector(170.0, -20.0, 95.0); Placement.WorldRotation = FRotator(0.0, 180.0, 0.0);
    Shot.ActorPlacements.Add(Placement);
    Shot.OverShoulderActor = TEXT("Partner"); Shot.OverShoulderSide = TEXT("left"); Shot.OverShoulderEdgeX = 0.2f;
    Plan.Shots.Add(Shot);
    FUPTCinematicPlan Parsed; FString Error;
    TestTrue(TEXT("Parse succeeds"), FUPTPlanParser::Parse(FUPTPlanParser::ToJson(Plan), Parsed, Error));
    if (!TestEqual(TEXT("Shot count"), Parsed.Shots.Num(), 1)) return true;
    TestTrue(TEXT("Aspect ratio"), FMath::IsNearlyEqual(Parsed.AspectRatio, 2.39f, 0.001f));
    TestTrue(TEXT("Screen X"), FMath::IsNearlyEqual(Parsed.Shots[0].SubjectScreenX, 0.33f, 0.001f));
    TestTrue(TEXT("Screen Y"), FMath::IsNearlyEqual(Parsed.Shots[0].SubjectScreenY, 0.4f, 0.001f));
    TestTrue(TEXT("Focus height"), FMath::IsNearlyEqual(Parsed.Shots[0].FocusHeightRatio, 0.88f, 0.001f));
    TestTrue(TEXT("Roll"), FMath::IsNearlyEqual(Parsed.Shots[0].CameraRollDegrees, 8.0f, 0.001f));
    TestEqual(TEXT("Motion"), Parsed.Shots[0].CameraMotion, FString(TEXT("dolly_in")));
    TestEqual(TEXT("Over-the-shoulder actor"), Parsed.Shots[0].OverShoulderActor, FString(TEXT("Partner")));
    TestEqual(TEXT("Over-the-shoulder side"), Parsed.Shots[0].OverShoulderSide, FString(TEXT("left")));
    TestTrue(TEXT("Over-the-shoulder edge"), FMath::IsNearlyEqual(Parsed.Shots[0].OverShoulderEdgeX, 0.2f, 0.001f));
    if (TestEqual(TEXT("Placement count"), Parsed.Shots[0].ActorPlacements.Num(), 1))
    {
        const FUPTActorBlockingPlacement& ParsedPlacement = Parsed.Shots[0].ActorPlacements[0];
        TestEqual(TEXT("Placement actor"), ParsedPlacement.ActorLabel, FString(TEXT("Partner")));
        TestTrue(TEXT("Placement location"), ParsedPlacement.WorldLocation.Equals(FVector(170.0, -20.0, 95.0), 0.01));
        TestTrue(TEXT("Placement yaw"), FMath::IsNearlyEqual(ParsedPlacement.WorldRotation.Yaw, 180.0, 0.01));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUPTShotFramingPresetTest, "UniversalProductionTools.Framing.ShotPreset", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUPTShotFramingPresetTest::RunTest(const FString& Parameters)
{
    const float AspectRatio = 16.0f / 9.0f;
    const float SensorHeight = UPTFraming::GetSensorHeightMm(AspectRatio);
    FUPTCinematicShot Shot;
    Shot.SubjectScreenX = 0.4f;
    // 클로즈업: 거리 = 키 × 0.22 × 65mm / (센서 높이 × 점유율)
    FUPTReferenceBlockingSolver::ApplyShotFraming(Shot, TEXT("close_up"), TEXT("eye"), 180.0f, 0.9f, AspectRatio);
    TestTrue(TEXT("Close-up distance formula"), FMath::IsNearlyEqual(Shot.DistanceCm, 180.0f * 0.22f * 65.0f / (SensorHeight * 0.9f), 0.5f));
    TestTrue(TEXT("Close-up lens"), FMath::IsNearlyEqual(Shot.FocalLength, 65.0f));
    TestTrue(TEXT("Close-up aim height"), FMath::IsNearlyEqual(Shot.FocusHeightRatio, 0.88f));
    TestTrue(TEXT("Eye level has no height offset"), FMath::IsNearlyZero(Shot.HeightCm));
    TestEqual(TEXT("Inferred size"), FUPTReferenceBlockingSolver::InferShotSize(Shot), FString(TEXT("close_up")));

    FUPTReferenceBlockingSolver::ApplyShotFraming(Shot, TEXT("wide"), TEXT("high"), 180.0f, 0.35f, AspectRatio);
    TestTrue(TEXT("High angle height"), FMath::IsNearlyEqual(Shot.HeightCm, Shot.DistanceCm * FMath::Tan(FMath::DegreesToRadians(25.0f)), 0.5f));
    TestEqual(TEXT("Inferred wide"), FUPTReferenceBlockingSolver::InferShotSize(Shot), FString(TEXT("wide")));
    TestEqual(TEXT("Inferred high"), FUPTReferenceBlockingSolver::InferCameraAngle(Shot), FString(TEXT("high")));

    FUPTReferenceBlockingSolver::ApplyShotFraming(Shot, TEXT("full"), TEXT("low"), 180.0f, 0.85f, AspectRatio);
    TestTrue(TEXT("Low angle stays above the floor"), Shot.HeightCm < 0.0f && Shot.HeightCm >= -(180.0f * 0.5f - 20.0f) - 0.01f);
    TestEqual(TEXT("Inferred low"), FUPTReferenceBlockingSolver::InferCameraAngle(Shot), FString(TEXT("low")));

    FUPTReferenceBlockingSolver::ApplyShotFraming(Shot, TEXT("medium"), TEXT("dutch"), 180.0f, 0.95f, AspectRatio);
    TestTrue(TEXT("Dutch roll toward the subject side"), FMath::IsNearlyEqual(Shot.CameraRollDegrees, -10.0f));
    TestEqual(TEXT("Inferred dutch"), FUPTReferenceBlockingSolver::InferCameraAngle(Shot), FString(TEXT("dutch")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUPTOriginCameraGuardTest, "UniversalProductionTools.Framing.OriginCameraGuard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUPTOriginCameraGuardTest::RunTest(const FString& Parameters)
{
    FUPTReferencePlan Reference;
    FUPTReferenceShot A; A.Name = TEXT("A"); A.SubjectRole = TEXT("hero"); A.EndSeconds = 1.0f; Reference.Shots.Add(A);
    const TMap<FString, TWeakObjectPtr<AActor>> NoActors;
    FUPTCinematicPlan Plan; FString Report;
    TestFalse(TEXT("Reference blocking without actors rejected"), FUPTReferenceBlockingSolver::BuildPlan(Reference, NoActors, {}, Plan, Report));

    FUPTCinematicPlan OriginPlan;
    FUPTCinematicShot OriginShot; OriginShot.Name = TEXT("Origin"); OriginPlan.Shots.Add(OriginShot);
    TestFalse(TEXT("Camera at world origin rejected"), FUPTBlockingValidator::Validate(OriginPlan).IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUPTEndpointRulesTest, "UniversalProductionTools.Endpoint.LocalModelRules", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUPTEndpointRulesTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("Ollama localhost is local"), UPTEndpoint::IsLocal(TEXT("http://localhost:11434/v1/chat/completions")));
    TestTrue(TEXT("Loopback IP is local"), UPTEndpoint::IsLocal(TEXT("http://127.0.0.1:1234/v1/chat/completions")));
    TestFalse(TEXT("Remote plain HTTP rejected"), UPTEndpoint::IsAllowed(TEXT("http://example.com/v1/chat/completions")));
    TestTrue(TEXT("Remote HTTPS allowed"), UPTEndpoint::IsAllowed(TEXT("https://api.openai.com/v1/responses")));
    TestTrue(TEXT("Chat Completions detected"), UPTEndpoint::UsesChatCompletions(TEXT("http://localhost:11434/v1/chat/completions")));
    TestFalse(TEXT("Responses API detected"), UPTEndpoint::UsesChatCompletions(TEXT("https://api.openai.com/v1/responses")));
    TestFalse(TEXT("Local endpoint never needs a real key"), UPTEndpoint::ResolveApiKey(TEXT("UPT_TEST_KEY_THAT_DOES_NOT_EXIST"), TEXT("http://localhost:11434/v1/chat/completions")).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUPTPoseAnalyzerPlanParseTest, "UniversalProductionTools.PoseAnalyzer.PlanFormat", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUPTPoseAnalyzerPlanParseTest::RunTest(const FString& Parameters)
{
    // upt_pose_reference_analyzer.py 출력 형식: diagnostics 같은 추가 필드와 인물이 없는 샷을 포함한다.
    const FString PoseJson = TEXT("{\"schema_version\":1,\"title\":\"pose\",\"match_mode\":\"scene\",\"vision_model\":\"upt-local-pose/balanced\",\"analysis_fingerprint\":\"\",\"aspect_ratio\":1.7778,\"shots\":["
        "{\"name\":\"Shot_01\",\"subject_role\":\"main_character\",\"start_seconds\":0.0,\"end_seconds\":2.0,\"shot_size\":\"close_up\",\"camera_angle\":\"eye\",\"camera_motion\":\"static\","
        "\"subject_screen_position\":{\"x\":0.33,\"y\":0.4},\"subject_screen_size\":{\"width\":0.3,\"height\":0.9},"
        "\"subjects\":[{\"role\":\"main_character\",\"screen_position\":{\"x\":0.33,\"y\":0.4},\"screen_size\":{\"width\":0.3,\"height\":0.9},\"depth_order\":0}],"
        "\"composition\":\"thirds\",\"confidence\":0.8,\"diagnostics\":{\"full_body_screen_height\":4.1},"
        "\"over_the_shoulder\":{\"side\":\"right\",\"screen_x\":0.94,\"screen_width\":0.11,\"screen_height\":0.49,\"confidence\":0.64}},"
        "{\"name\":\"Shot_02\",\"subject_role\":\"\",\"start_seconds\":2.0,\"end_seconds\":4.0,\"shot_size\":\"wide\",\"camera_angle\":\"eye\",\"camera_motion\":\"static\","
        "\"subject_screen_position\":{\"x\":0.5,\"y\":0.5},\"subject_screen_size\":{\"width\":0.3,\"height\":0.5},\"subjects\":[],\"composition\":\"center\",\"confidence\":0.1}]}");
    FUPTReferencePlan Parsed;
    FString Error;
    TestTrue(TEXT("Pose analyzer output parses"), FUPTReferencePlanParser::Parse(PoseJson, Parsed, Error));
    if (!TestEqual(TEXT("Shot count"), Parsed.Shots.Num(), 2)) return true;
    TestTrue(TEXT("Aspect ratio"), FMath::IsNearlyEqual(Parsed.AspectRatio, 1.7778f, 0.001f));
    TestTrue(TEXT("Thirds position kept"), FMath::IsNearlyEqual(static_cast<float>(Parsed.Shots[0].SubjectScreenPosition.X), 0.33f, 0.001f));
    TestEqual(TEXT("Empty-subject shot kept"), Parsed.Shots[1].SubjectRole, FString());
    TestEqual(TEXT("Over-the-shoulder side parsed"), Parsed.Shots[0].OverShoulderSide, FString(TEXT("right")));
    TestTrue(TEXT("Over-the-shoulder X parsed"), FMath::IsNearlyEqual(Parsed.Shots[0].OverShoulderScreenX, 0.94f, 0.001f));
    TestTrue(TEXT("Normal shot has no over-the-shoulder"), Parsed.Shots[1].OverShoulderSide.IsEmpty());
    FString Reserialized;
    FUPTReferencePlan Reparsed;
    TestTrue(TEXT("Reference plan re-serializes"), FUPTReferencePlanParser::ToJson(Parsed, Reserialized) && FUPTReferencePlanParser::Parse(Reserialized, Reparsed, Error));
    TestTrue(TEXT("Over-the-shoulder survives round trip"), Reparsed.Shots.Num() == 2 && Reparsed.Shots[0].OverShoulderSide == TEXT("right")
        && FMath::IsNearlyEqual(Reparsed.Shots[0].OverShoulderScreenWidth, 0.11f, 0.001f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUPTOverShoulderCameraTest, "UniversalProductionTools.Framing.OverShoulder", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUPTOverShoulderCameraTest::RunTest(const FString& Parameters)
{
    // 원점의 주 피사체(키 180)와 1.7m 앞에 마주 선 앞사람. 앞사람 몸 안쪽 윤곽을 화면 가장자리 쪽에 걸치게 한다.
    FUPTOverShoulderSetup Setup;
    Setup.AnchorFocus = FVector(0.0, 0.0, 158.0);
    Setup.AnchorHead = FVector(0.0, 0.0, 162.0);
    Setup.ForegroundBase = FVector(170.0, 0.0, 0.0);
    Setup.ForegroundHeightCm = 180.0f;
    Setup.BaseFocalLength = 65.0f;
    Setup.BaseDistanceCm = 200.0f;
    // 주 피사체가 화면 위쪽일 때와 아래쪽일 때를 모두 본다. 아래쪽(0.76)이면 카메라가 위를 향하는데,
    // 세로 조건이 없으면 앞사람이 통째로 화면 밖 아래로 빠진다(실제로 그랬다).
    for (const float SubjectY : { 0.42f, 0.76f })
    for (const bool bRight : { true, false })
    {
        Setup.SubjectScreenY = SubjectY;
        Setup.bForegroundOnRight = bRight;
        Setup.ForegroundEdgeX = bRight ? 0.85f : 0.15f;
        Setup.SubjectScreenX = bRight ? 0.4f : 0.6f;
        FVector Camera = FVector::ZeroVector;
        float Focal = 0.0f;
        if (!TestTrue(TEXT("Over-the-shoulder camera found"), FUPTReferenceBlockingSolver::SolveOverShoulderCamera(Setup, Camera, Focal))) return true;
        TestTrue(TEXT("Camera stands behind the foreground person"), Camera.X > Setup.ForegroundBase.X);
        const FRotator Rotation = UPTFraming::ComputeFramingRotation(Camera, Setup.AnchorFocus, Setup.SubjectScreenX, Setup.SubjectScreenY, Focal, Setup.AspectRatio, 0.0f);
        FVector2D Subject;
        TestTrue(TEXT("Subject in view"), UPTFraming::ProjectToScreen(Camera, Rotation, Setup.AnchorFocus, Focal, Setup.AspectRatio, Subject));
        TestTrue(FString::Printf(TEXT("Subject X %.3f"), Subject.X), FMath::IsNearlyEqual(Subject.X, static_cast<double>(Setup.SubjectScreenX), 0.01));
        FVector2D ForegroundCenter;
        TestTrue(TEXT("Foreground in front of camera"), UPTFraming::ProjectToScreen(Camera, Rotation, Setup.ForegroundBase + FVector(0.0, 0.0, 180.0 * 0.82), Focal, Setup.AspectRatio, ForegroundCenter));
        TestTrue(FString::Printf(TEXT("Foreground on requested side (x=%.2f)"), ForegroundCenter.X), bRight ? ForegroundCenter.X > 0.8 : ForegroundCenter.X < 0.2);
        // 앞사람이 세로로도 프레임 안에 있어야 어깨 너머로 보인다.
        TestTrue(FString::Printf(TEXT("Foreground shoulder stays in frame (subjectY=%.2f, y=%.2f)"), SubjectY, ForegroundCenter.Y),
            ForegroundCenter.Y > 0.4 && ForegroundCenter.Y < 1.1);
        FVector2D ForegroundHead;
        TestTrue(TEXT("Foreground head projects"), UPTFraming::ProjectToScreen(Camera, Rotation, Setup.ForegroundBase + FVector(0.0, 0.0, 180.0 * 0.95), Focal, Setup.AspectRatio, ForegroundHead));
        TestTrue(FString::Printf(TEXT("Foreground head visible (subjectY=%.2f, y=%.2f)"), SubjectY, ForegroundHead.Y), ForegroundHead.Y < 0.9);
        // 주 피사체까지 거리가 늘어난 비율만큼 렌즈를 길게 해 크기를 유지한다.
        TestTrue(FString::Printf(TEXT("Lens keeps subject size (%.1fmm)"), Focal), FMath::IsNearlyEqual(Focal, 65.0f * static_cast<float>(FVector::Dist(Camera, Setup.AnchorFocus)) / 200.0f, 0.5f));
    }
    return true;
}
#endif
