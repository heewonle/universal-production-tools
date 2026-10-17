#include "UPTRetargetQualityReport.h"

#include "UPTSkeletonAnalyzer.h"

#include "AnimPose.h"
#include "Animation/AnimSequence.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ReferenceSkeleton.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
// 샘플 수·접지 문턱은 파이썬 스위트와 같은 값을 쓴다. 다르면 두 수치를 비교할 수 없다.
constexpr int32 Samples = 24;
constexpr double ContactHeightRatio = 0.08;

/** 방향을 비교할 뼈마디. 본 이름이 아니라 **역할** 이름이다. */
const TArray<TPair<FString, FString>>& BodySegments()
{
    static const TArray<TPair<FString, FString>> Segments = {
        {TEXT("LeftThigh"), TEXT("LeftCalf")}, {TEXT("LeftCalf"), TEXT("LeftFoot")},
        {TEXT("RightThigh"), TEXT("RightCalf")}, {TEXT("RightCalf"), TEXT("RightFoot")},
        {TEXT("LeftUpperArm"), TEXT("LeftLowerArm")}, {TEXT("LeftLowerArm"), TEXT("LeftHand")},
        {TEXT("RightUpperArm"), TEXT("RightLowerArm")}, {TEXT("RightLowerArm"), TEXT("RightHand")},
        {TEXT("Pelvis"), TEXT("Spine")}, {TEXT("Neck"), TEXT("Head")},
    };
    return Segments;
}

TArray<TPair<FString, FString>> FingerSegments()
{
    TArray<TPair<FString, FString>> Segments;
    for (const TCHAR* Side : {TEXT("Left"), TEXT("Right")})
    {
        for (const TCHAR* Finger : {TEXT("Thumb"), TEXT("Index"), TEXT("Middle"), TEXT("Ring"), TEXT("Pinky")})
        {
            Segments.Add({FString::Printf(TEXT("%s%s1"), Side, Finger), FString::Printf(TEXT("%s%s2"), Side, Finger)});
        }
    }
    return Segments;
}

/** 트위스트 본 → 매달린 역할. 베이커·측정기와 **같은 규칙**이어야 한다(가까운 순, 동점은 이름 순). */
TMap<FString, FName> TwistByRole(const FReferenceSkeleton& Ref, const TMap<FString, FName>& Roles)
{
    static const TCHAR* Tokens[] = {TEXT("twist"), TEXT("roll"), TEXT("torsion")};
    TMap<FName, FString> BoneToRole;
    for (const TPair<FString, FName>& Pair : Roles) BoneToRole.Add(Pair.Value, Pair.Key);

    TMap<FString, TPair<int32, FName>> Nearest;
    for (int32 Index = 0; Index < Ref.GetNum(); ++Index)
    {
        const FName BoneName = Ref.GetBoneName(Index);
        const FString Lower = BoneName.ToString().ToLower();
        bool bTwist = false;
        for (const TCHAR* Token : Tokens) bTwist |= Lower.Contains(Token);
        if (!bTwist) continue;

        int32 Walk = Ref.GetParentIndex(Index);
        for (int32 Distance = 0; Walk != INDEX_NONE && Distance < 16; Walk = Ref.GetParentIndex(Walk), ++Distance)
        {
            const FString* Role = BoneToRole.Find(Ref.GetBoneName(Walk));
            if (!Role) continue;
            const TPair<int32, FName>* Existing = Nearest.Find(*Role);
            const bool bBetter = !Existing || Distance < Existing->Key
                || (Distance == Existing->Key && BoneName.ToString() < Existing->Value.ToString());
            if (bBetter) Nearest.Add(*Role, {Distance, BoneName});
            break;
        }
    }
    TMap<FString, FName> Out;
    for (const TPair<FString, TPair<int32, FName>>& Pair : Nearest) Out.Add(Pair.Key, Pair.Value.Value);
    return Out;
}

/** 한 애니메이션을 Samples번 뽑아 포즈를 들고 있는다. */
struct FSampledPoses
{
    TArray<FAnimPose> Poses;
    // GetBonePose는 없는 본에도 항등 변환을 돌려준다. 그대로 쓰면 **원점에 있는 본**으로
    // 읽혀 각도가 조용히 틀어지므로, 포즈에 실제로 있는 이름만 받아들인다.
    TSet<FName> BoneNames;

    bool Build(UAnimSequence* Animation, USkeletalMesh* Mesh)
    {
        if (!Animation || Animation->GetPlayLength() <= 0.0f) return false;
        FAnimPoseEvaluationOptions Options;
        // 압축 데이터가 아니라 **원시 데이터**를 본다. 베이커가 쓴 키가 그대로 읽힌다.
        Options.EvaluationType = EAnimDataEvalType::Raw;
        Options.OptionalSkeletalMesh = Mesh;

        TArray<double> Times;
        Times.Reserve(Samples);
        const double Length = Animation->GetPlayLength();
        for (int32 Index = 0; Index < Samples; ++Index)
        {
            Times.Add(Length * Index / FMath::Max(1, Samples - 1));
        }
        UAnimPoseExtensions::GetAnimPoseAtTimeIntervals(Animation, Times, Options, Poses);
        if (Poses.Num() != Samples || !UAnimPoseExtensions::IsValid(Poses[0])) return false;

        TArray<FName> Names;
        UAnimPoseExtensions::GetBoneNames(Poses[0], Names);
        BoneNames.Append(Names);
        return !BoneNames.IsEmpty();
    }

    /** 컴포넌트 공간 위치. 포즈에 없는 본이면 false. */
    bool Location(const int32 Sample, const FName Bone, FVector& Out) const
    {
        if (!Poses.IsValidIndex(Sample) || !BoneNames.Contains(Bone)) return false;
        Out = UAnimPoseExtensions::GetBonePose(Poses[Sample], Bone, EAnimPoseSpaces::World).GetLocation();
        return true;
    }

    bool LocalRotation(const int32 Sample, const FName Bone, FQuat& Out) const
    {
        if (!Poses.IsValidIndex(Sample) || !BoneNames.Contains(Bone)) return false;
        Out = UAnimPoseExtensions::GetBonePose(Poses[Sample], Bone, EAnimPoseSpaces::Local).GetRotation();
        return true;
    }
};

double Median(TArray<double> Values)
{
    if (Values.IsEmpty()) return 0.0;
    Values.Sort();
    const int32 Mid = Values.Num() / 2;
    return Values.Num() % 2 ? Values[Mid] : 0.5 * (Values[Mid - 1] + Values[Mid]);
}

/** 중앙값에서 얼마나 흔들리는가(MAD). 고정 오프셋을 뺀 '동작이 뭉개진 정도'다. */
double MedianAbsoluteDeviation(const TArray<double>& Values)
{
    if (Values.IsEmpty()) return 0.0;
    const double Center = Median(Values);
    TArray<double> Deviations;
    Deviations.Reserve(Values.Num());
    for (const double Value : Values) Deviations.Add(FMath::Abs(Value - Center));
    return Median(MoveTemp(Deviations));
}

/** 뼈마디 방향 각도 차이(도)를 샘플마다 모은다. */
TArray<double> SegmentAngles(
    const FSampledPoses& Source, const FSampledPoses& Target,
    const FName SourceParent, const FName SourceChild, const FName TargetParent, const FName TargetChild)
{
    TArray<double> Angles;
    for (int32 Sample = 0; Sample < Samples; ++Sample)
    {
        FVector SourceA, SourceB, TargetA, TargetB;
        if (!Source.Location(Sample, SourceParent, SourceA) || !Source.Location(Sample, SourceChild, SourceB)) continue;
        if (!Target.Location(Sample, TargetParent, TargetA) || !Target.Location(Sample, TargetChild, TargetB)) continue;
        const FVector SourceDirection = (SourceB - SourceA).GetSafeNormal();
        const FVector TargetDirection = (TargetB - TargetA).GetSafeNormal();
        if (SourceDirection.IsNearlyZero() || TargetDirection.IsNearlyZero()) continue;
        Angles.Add(FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(SourceDirection | TargetDirection, -1.0, 1.0))));
    }
    return Angles;
}

/** 골반→발 길이. 체형이 다른 둘을 같은 기준에 놓기 위한 척도다. */
double LegLength(const FSampledPoses& Poses, const TMap<FString, FName>& Roles)
{
    static const TCHAR* Chain[] = {TEXT("Pelvis"), TEXT("LeftThigh"), TEXT("LeftCalf"), TEXT("LeftFoot")};
    double Total = 0.0;
    for (int32 Index = 0; Index + 1 < UE_ARRAY_COUNT(Chain); ++Index)
    {
        const FName* Parent = Roles.Find(Chain[Index]);
        const FName* Child = Roles.Find(Chain[Index + 1]);
        FVector A, B;
        if (!Parent || !Child || !Poses.Location(0, *Parent, A) || !Poses.Location(0, *Child, B)) return 0.0;
        Total += FVector::Dist(A, B);
    }
    return Total;
}

/** 발 높이 곡선(바닥 기준, 다리 길이로 정규화). 바닥은 그 클립에서 발이 가장 낮았던 지점이다. */
TMap<FString, TArray<double>> FootClearance(
    const FSampledPoses& Poses, const TMap<FString, FName>& Roles, const double Scale)
{
    TMap<FString, TArray<double>> Tracks;
    if (Scale <= KINDA_SMALL_NUMBER) return Tracks;

    double Floor = TNumericLimits<double>::Max();
    for (const TCHAR* Role : {TEXT("LeftFoot"), TEXT("RightFoot")})
    {
        const FName* Bone = Roles.Find(Role);
        if (!Bone) continue;
        TArray<double> Heights;
        bool bComplete = true;
        for (int32 Sample = 0; Sample < Samples; ++Sample)
        {
            FVector Location;
            if (!Poses.Location(Sample, *Bone, Location)) { bComplete = false; break; }
            Heights.Add(Location.Z);
        }
        if (!bComplete || Heights.IsEmpty()) continue;
        for (const double Height : Heights) Floor = FMath::Min(Floor, Height);
        Tracks.Add(Role, MoveTemp(Heights));
    }
    if (Tracks.IsEmpty()) return Tracks;
    for (TPair<FString, TArray<double>>& Track : Tracks)
    {
        for (double& Height : Track.Value) Height = (Height - Floor) / Scale;
    }
    return Tracks;
}

/** 트위스트 본의 로컬 회전이 샷 동안 움직인 범위(도). 첫 샘플 기준 최대 회전차다. */
bool TwistRange(const FSampledPoses& Poses, const FName Bone, double& OutDegrees)
{
    FQuat First;
    if (!Poses.LocalRotation(0, Bone, First)) return false;
    double Worst = 0.0;
    for (int32 Sample = 1; Sample < Samples; ++Sample)
    {
        FQuat Rotation;
        if (!Poses.LocalRotation(Sample, Bone, Rotation)) continue;
        const double Dot = FMath::Abs(First | Rotation);
        Worst = FMath::Max(Worst, FMath::RadiansToDegrees(2.0 * FMath::Acos(FMath::Clamp(Dot, -1.0, 1.0))));
    }
    OutDegrees = Worst;
    return true;
}
}

FUPTRetargetQualityRow FUPTRetargetQualityReport::Measure(
    UAnimSequence* SourceAnimation, USkeletalMesh* SourceMesh,
    UAnimSequence* TargetAnimation, USkeletalMesh* TargetMesh,
    const FUPTRetargetQualityThresholds& Thresholds)
{
    FUPTRetargetQualityRow Row;
    if (!SourceAnimation || !SourceMesh || !TargetAnimation || !TargetMesh) return Row;
    Row.Animation = TargetAnimation->GetName();
    Row.SourceMesh = SourceMesh->GetName();
    Row.TargetMesh = TargetMesh->GetName();

    const TMap<FString, FName> SourceRoles = FUPTSkeletonAnalyzer::AnalyzeDetailed(SourceMesh, FString()).BoneMappings;
    const TMap<FString, FName> TargetRoles = FUPTSkeletonAnalyzer::AnalyzeDetailed(TargetMesh, FString()).BoneMappings;

    FSampledPoses Source, Target;
    if (!Source.Build(SourceAnimation, SourceMesh) || !Target.Build(TargetAnimation, TargetMesh)) return Row;

    // 몸통 뼈마디
    TArray<double> AllAngles;
    for (const TPair<FString, FString>& Segment : BodySegments())
    {
        const FName* SourceParent = SourceRoles.Find(Segment.Key);
        const FName* SourceChild = SourceRoles.Find(Segment.Value);
        const FName* TargetParent = TargetRoles.Find(Segment.Key);
        const FName* TargetChild = TargetRoles.Find(Segment.Value);
        if (!SourceParent || !SourceChild || !TargetParent || !TargetChild) continue;

        const TArray<double> Angles = SegmentAngles(Source, Target, *SourceParent, *SourceChild, *TargetParent, *TargetChild);
        if (Angles.IsEmpty()) continue;

        const double SegmentMedian = Median(Angles);
        const double Deviation = MedianAbsoluteDeviation(Angles);
        ++Row.SegmentsCompared;
        AllAngles.Append(Angles);
        if (SegmentMedian > Row.WorstSegmentDeg)
        {
            Row.WorstSegmentDeg = static_cast<float>(SegmentMedian);
            Row.WorstSegment = FString::Printf(TEXT("%s->%s"), *Segment.Key, *Segment.Value);
        }
        Row.MotionDeviationWorstDeg = FMath::Max(Row.MotionDeviationWorstDeg, static_cast<float>(Deviation));
    }
    if (Row.SegmentsCompared == 0) return Row;  // 역할이 부족하면 틀린 숫자를 내놓지 않는다
    Row.SegmentMedianDeg = static_cast<float>(Median(AllAngles));
    Row.bMeasured = true;

    // 손가락 마디
    TArray<double> FingerMedians;
    for (const TPair<FString, FString>& Segment : FingerSegments())
    {
        const FName* SourceParent = SourceRoles.Find(Segment.Key);
        const FName* SourceChild = SourceRoles.Find(Segment.Value);
        const FName* TargetParent = TargetRoles.Find(Segment.Key);
        const FName* TargetChild = TargetRoles.Find(Segment.Value);
        if (!SourceParent || !SourceChild || !TargetParent || !TargetChild) continue;
        const TArray<double> Angles = SegmentAngles(Source, Target, *SourceParent, *SourceChild, *TargetParent, *TargetChild);
        if (Angles.IsEmpty()) continue;
        const double SegmentMedian = Median(Angles);
        FingerMedians.Add(SegmentMedian);
        Row.FingerWorstDeg = FMath::Max(Row.FingerWorstDeg, static_cast<float>(SegmentMedian));
    }
    Row.FingerSegmentsCompared = FingerMedians.Num();
    Row.FingerMedianDeg = static_cast<float>(Median(FingerMedians));

    // 접지. 각도와 독립인 지표라 각도를 고치려는 수정이 디딤을 망쳤는지 가려낸다.
    const double SourceScale = LegLength(Source, SourceRoles);
    const double TargetScale = LegLength(Target, TargetRoles);
    const TMap<FString, TArray<double>> SourceTracks = FootClearance(Source, SourceRoles, SourceScale);
    const TMap<FString, TArray<double>> TargetTracks = FootClearance(Target, TargetRoles, TargetScale);
    TArray<double> ClearanceErrors;
    int32 Mismatches = 0, Compared = 0;
    for (const TPair<FString, TArray<double>>& SourceTrack : SourceTracks)
    {
        const TArray<double>* TargetTrack = TargetTracks.Find(SourceTrack.Key);
        if (!TargetTrack) continue;
        const int32 Count = FMath::Min(SourceTrack.Value.Num(), TargetTrack->Num());
        for (int32 Index = 0; Index < Count; ++Index)
        {
            ClearanceErrors.Add(FMath::Abs(SourceTrack.Value[Index] - (*TargetTrack)[Index]));
            const bool bSourceDown = SourceTrack.Value[Index] <= ContactHeightRatio;
            const bool bTargetDown = (*TargetTrack)[Index] <= ContactHeightRatio;
            if (bSourceDown != bTargetDown) ++Mismatches;
            ++Compared;
        }
    }
    if (Compared > 0)
    {
        Row.FootClearanceError = static_cast<float>(Median(ClearanceErrors));
        Row.ContactMismatch = static_cast<float>(static_cast<double>(Mismatches) / Compared);
    }

    // 트위스트 전달. 원본이 구른 만큼 결과도 굴렀는가.
    const TMap<FString, FName> SourceTwists = TwistByRole(SourceMesh->GetRefSkeleton(), SourceRoles);
    const TMap<FString, FName> TargetTwists = TwistByRole(TargetMesh->GetRefSkeleton(), TargetRoles);
    for (const TPair<FString, FName>& Pair : TargetTwists)
    {
        const FName* SourceBone = SourceTwists.Find(Pair.Key);
        double SourceRange = 0.0, TargetRange = 0.0;
        if (!SourceBone || !TwistRange(Source, *SourceBone, SourceRange) || !TwistRange(Target, Pair.Value, TargetRange)) continue;
        ++Row.TwistCompared;
        if (SourceRange < 5.0) continue;  // 원본이 거의 구르지 않았으면 전달을 논할 수 없다
        if (TargetRange < 0.2 * SourceRange) ++Row.TwistDropped;
        Row.TwistRatioWorstError = FMath::Max(Row.TwistRatioWorstError,
            static_cast<float>(FMath::Abs(TargetRange / SourceRange - 1.0)));
    }

    // 판정. 숫자 하나로 합치지 않고 넘은 지표의 이름을 그대로 남긴다.
    auto Flag = [&Row](const bool bOver, const FString& Text)
    {
        if (bOver) { Row.Reasons.Add(Text); Row.bNeedsReview = true; }
    };
    Flag(Row.WorstSegmentDeg > Thresholds.WorstSegmentDeg,
        FString::Printf(TEXT("뼈마디 %s가 %.1f도 (문턱 %.0f도)"), *Row.WorstSegment, Row.WorstSegmentDeg, Thresholds.WorstSegmentDeg));
    Flag(Row.MotionDeviationWorstDeg > Thresholds.MotionDeviationDeg,
        FString::Printf(TEXT("동작 왜곡 %.1f도 (문턱 %.0f도)"), Row.MotionDeviationWorstDeg, Thresholds.MotionDeviationDeg));
    Flag(Row.FootClearanceError >= 0.0f && Row.FootClearanceError > Thresholds.FootClearanceError,
        FString::Printf(TEXT("발 높이 오차 %.3f (문턱 %.2f)"), Row.FootClearanceError, Thresholds.FootClearanceError));
    Flag(Row.ContactMismatch >= 0.0f && Row.ContactMismatch > Thresholds.ContactMismatch,
        FString::Printf(TEXT("접지 불일치 %.2f (문턱 %.2f)"), Row.ContactMismatch, Thresholds.ContactMismatch));
    Flag(Row.FingerSegmentsCompared > 0 && Row.FingerWorstDeg > Thresholds.FingerWorstDeg,
        FString::Printf(TEXT("손가락 %.1f도 (문턱 %.0f도)"), Row.FingerWorstDeg, Thresholds.FingerWorstDeg));
    Flag(Row.TwistDropped > 0,
        FString::Printf(TEXT("트위스트 %d개가 전달되지 않음"), Row.TwistDropped));
    Flag(Row.TwistDropped == 0 && Row.TwistRatioWorstError > Thresholds.TwistRatioError,
        FString::Printf(TEXT("트위스트 전달 오차 %.2f (문턱 %.2f)"), Row.TwistRatioWorstError, Thresholds.TwistRatioError));
    return Row;
}

FString FUPTRetargetQualityReport::Save(const TArray<FUPTRetargetQualityRow>& Rows, const FString& Directory)
{
    if (Rows.IsEmpty()) return FString();
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FUPTRetargetQualityRow& Row : Rows)
    {
        TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetStringField(TEXT("animation"), Row.Animation);
        Object->SetStringField(TEXT("source_mesh"), Row.SourceMesh);
        Object->SetStringField(TEXT("target_mesh"), Row.TargetMesh);
        Object->SetBoolField(TEXT("measured"), Row.bMeasured);
        Object->SetBoolField(TEXT("needs_review"), Row.bNeedsReview);
        Object->SetNumberField(TEXT("segments_compared"), Row.SegmentsCompared);
        Object->SetNumberField(TEXT("segment_angle_median_deg"), Row.SegmentMedianDeg);
        Object->SetNumberField(TEXT("worst_segment_deg"), Row.WorstSegmentDeg);
        Object->SetStringField(TEXT("worst_segment"), Row.WorstSegment);
        Object->SetNumberField(TEXT("motion_deviation_worst_deg"), Row.MotionDeviationWorstDeg);
        Object->SetNumberField(TEXT("foot_clearance_error"), Row.FootClearanceError);
        Object->SetNumberField(TEXT("contact_mismatch"), Row.ContactMismatch);
        Object->SetNumberField(TEXT("finger_segments_compared"), Row.FingerSegmentsCompared);
        Object->SetNumberField(TEXT("finger_angle_median_deg"), Row.FingerMedianDeg);
        Object->SetNumberField(TEXT("finger_worst_deg"), Row.FingerWorstDeg);
        Object->SetNumberField(TEXT("twist_compared"), Row.TwistCompared);
        Object->SetNumberField(TEXT("twist_dropped"), Row.TwistDropped);
        Object->SetNumberField(TEXT("twist_ratio_worst_error"), Row.TwistRatioWorstError);
        TArray<TSharedPtr<FJsonValue>> Reasons;
        for (const FString& Reason : Row.Reasons) Reasons.Add(MakeShared<FJsonValueString>(Reason));
        Object->SetArrayField(TEXT("reasons"), Reasons);
        Items.Add(MakeShared<FJsonValueObject>(Object));
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("created"), FDateTime::Now().ToString());
    Root->SetNumberField(TEXT("rows"), Rows.Num());
    Root->SetArrayField(TEXT("items"), Items);

    FString Text;
    FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
    // 같은 초에 두 번 저장하면 이름이 겹쳐 앞의 리포트가 사라진다. 겹치면 번호를 붙인다.
    const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
    FString Path = FPaths::Combine(Directory, FString::Printf(TEXT("retarget_quality_%s.json"), *Stamp));
    for (int32 Suffix = 2; FPaths::FileExists(Path) && Suffix < 100; ++Suffix)
    {
        Path = FPaths::Combine(Directory, FString::Printf(TEXT("retarget_quality_%s_%d.json"), *Stamp, Suffix));
    }
    // 기본값은 UTF-16이라 파이썬·엑셀에서 읽히지 않는다.
    return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ? Path : FString();
}

FString FUPTRetargetQualityReport::Summarize(const TArray<FUPTRetargetQualityRow>& Rows)
{
    int32 Measured = 0, Review = 0;
    for (const FUPTRetargetQualityRow& Row : Rows)
    {
        if (Row.bMeasured) ++Measured;
        if (Row.bNeedsReview) ++Review;
    }
    const int32 Unmeasured = Rows.Num() - Measured;
    FString Text = FString::Printf(TEXT("품질 점검: %d개 중 검토 필요 %d개"), Measured, Review);
    if (Unmeasured > 0) Text += FString::Printf(TEXT(" (역할 부족으로 %d개는 재지 못함)"), Unmeasured);
    return Text;
}
