#include "UPTSequenceBuilder.h"
#include "UPTFraming.h"
#include "UPTSettings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Animation/AnimSequenceBase.h"
#include "CineCameraActor.h"
#include "CineCameraComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "IAssetTools.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequenceEditorSubsystem.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "MovieSceneMarkedFrame.h"
#include "MovieSceneBindingProxy.h"
#include "ExtensionLibraries/MovieSceneSequenceExtensions.h"
#include "MovieSceneObjectBindingID.h"
#include "MovieScenePossessable.h"
#include "ObjectTools.h"
#include "GameFramework/Actor.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sections/MovieSceneSkeletalAnimationSection.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sound/SoundBase.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneFloatChannel.h"

namespace
{
void ResolveCameraEndTransform(
    UWorld* World,
    const FUPTCinematicShot& Shot,
    const float AspectRatio,
    const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors,
    const FVector& StartLocation,
    const FRotator& StartRotation,
    FVector& OutLocation,
    FRotator& OutRotation)
{
    OutLocation = StartLocation;
    OutRotation = StartRotation;
    if (Shot.CameraMotion.IsEmpty() || Shot.CameraMotion == TEXT("static")) return;

    // 화면에서 잰 이동 비율을 **생성하는 카메라의 화각으로** 각도로 되돌린다.
    // 레퍼런스 카메라의 화각은 모르지만, 같은 화면 이동을 만드는 것이 목적이므로 알 필요가 없다.
    // 정지한 배경이 화면 가로로 Dx(프레임 폭 비율)만큼 움직였다면 회전각 α는 tan α = Dx·2·tan(hfov/2)이다.
    //
    // 한 번에 환산하면 안 된다. 측정값은 **구간별 이동량의 합**이고 tan은 볼록하므로
    // Σtan(αᵢ) < tan(Σαᵢ)다. 한 번에 환산했더니 정답 18° 패닝이 15.96°로 나왔다(-11%).
    // 구간마다 환산해 더하면 16.37°(-9%)가 된다.
    const float TanHalfH = FMath::Tan(FMath::DegreesToRadians(
        UPTFraming::GetFieldOfViewDegrees(UPTFraming::SensorWidthMm, Shot.FocalLength)) * 0.5f);
    const float TanHalfV = FMath::Tan(FMath::DegreesToRadians(
        UPTFraming::GetFieldOfViewDegrees(UPTFraming::GetSensorHeightMm(AspectRatio), Shot.FocalLength)) * 0.5f);
    const bool bMeasured = Shot.bHasMotionAmount;

    FUPTCinematicShot EndShot = Shot;
    if (Shot.CameraMotion == TEXT("dolly_in") || Shot.CameraMotion == TEXT("dolly_out"))
    {
        // 인물이 커진 비율만큼 가까워진 것으로 본다(화면 크기 ∝ 1/거리).
        const float SubjectScale = bMeasured ? Shot.MotionSubjectScale : 0.0f;
        if (SubjectScale > 0.05f)
        {
            EndShot.DistanceCm = FMath::Max(20.0f, Shot.DistanceCm / SubjectScale);
        }
        else
        {
            EndShot.DistanceCm *= Shot.CameraMotion == TEXT("dolly_in") ? 0.65f : 1.4f;
        }
    }
    else if (Shot.CameraMotion == TEXT("zoom_in") || Shot.CameraMotion == TEXT("zoom_out")) return;  // 렌즈만 바뀐다
    else if (Shot.CameraMotion == TEXT("truck_left") || Shot.CameraMotion == TEXT("truck_right") || Shot.CameraMotion == TEXT("tracking"))
    {
        // 방향은 모션 종류가 이미 말해 주므로 **크기만** 측정값으로 바꾼다.
        // 측면 이동 d는 거리 D에서 화면 가로로 d/(D·2·tan(hfov/2))만큼 밀어낸다.
        const float Sign = Shot.CameraMotion == TEXT("truck_left") ? -1.0f : 1.0f;
        const float Measured = bMeasured ? FMath::Abs(Shot.MotionScreenDX) * Shot.DistanceCm * 2.0f * TanHalfH : 0.0f;
        EndShot.SideCm += Sign * (Measured > 1.0f ? Measured : FMath::Max(100.0f, Shot.DistanceCm * 0.35f));
    }
    else if (Shot.CameraMotion == TEXT("pedestal"))
    {
        const float Measured = bMeasured ? FMath::Abs(Shot.MotionScreenDY) * Shot.DistanceCm * 2.0f * TanHalfV : 0.0f;
        EndShot.HeightCm += Measured > 1.0f ? Measured : FMath::Max(100.0f, Shot.DistanceCm * 0.25f);
    }
    // 오빗은 화면 이동만으로 회전 반경을 가를 수 없어(측면 이동과 구분 불가) 상수 폭을 유지한다.
    else if (Shot.CameraMotion == TEXT("orbit")) EndShot.SideCm += FMath::Max(150.0f, Shot.DistanceCm * 0.65f);
    else if (Shot.CameraMotion == TEXT("pan"))
    {
        // 배경이 오른쪽으로 갔다면 카메라는 왼쪽으로 돈 것이다(부호 반대).
        OutRotation.Yaw += bMeasured
            ? -UPTFraming::ScreenDeltaToRotationDegrees(Shot.MotionScreenDX, Shot.FocalLength, UPTFraming::SensorWidthMm, Shot.MotionSteps)
            : 25.0f;
        return;
    }
    else if (Shot.CameraMotion == TEXT("tilt"))
    {
        // 화면 y는 아래로 증가한다. 배경이 아래로 갔다면 카메라가 위를 본 것이다(+Pitch).
        OutRotation.Pitch += bMeasured
            ? UPTFraming::ScreenDeltaToRotationDegrees(Shot.MotionScreenDY, Shot.FocalLength, UPTFraming::GetSensorHeightMm(AspectRatio), Shot.MotionSteps)
            : -18.0f;
        return;
    }
    else if (Shot.CameraMotion == TEXT("handheld"))
    {
        // 흔들림 폭은 누적 경로 길이에 비례시킨다. 0.15는 지금까지 상수와 같은 폭이 나오는 기준값이다.
        const float Gain = bMeasured ? FMath::Clamp(Shot.MotionPath / 0.15f, 0.3f, 3.0f) : 1.0f;
        OutLocation += FVector(8.0f, 12.0f, 6.0f) * Gain;
        OutRotation += FRotator(-1.5f * Gain, 2.0f * Gain, 0.8f * Gain);
        return;
    }
    else return;
    UPTFraming::ResolveShotCamera(World, EndShot, AspectRatio, SceneActors, OutLocation, OutRotation);
}

void AddFocalLengthKeys(UMovieScene* MovieScene, const FGuid& ComponentGuid, const int32 StartFrame, const int32 EndFrame,
    const FUPTCinematicShot& Shot)
{
    const FString& Motion = Shot.CameraMotion;
    if (Motion != TEXT("zoom_in") && Motion != TEXT("zoom_out")) return;
    const float StartFocalLength = Shot.FocalLength;
    UMovieSceneFloatTrack* Track = MovieScene->AddTrack<UMovieSceneFloatTrack>(ComponentGuid);
    if (!Track) return;
    Track->SetPropertyNameAndPath(TEXT("CurrentFocalLength"), TEXT("CurrentFocalLength"));
    UMovieSceneFloatSection* Section = Cast<UMovieSceneFloatSection>(Track->CreateNewSection());
    if (!Section) return;
    Section->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
    Track->AddSection(*Section);
    FMovieSceneFloatChannel* Channel = Section->GetChannelProxy().GetChannel<FMovieSceneFloatChannel>(0);
    if (!Channel) return;
    // 배경 배율이 곧 초점거리 비다. 측정값이 없을 때만 상수(1.45 / 0.72)로 되돌아간다.
    const float Ratio = Shot.bHasMotionAmount && Shot.MotionBackgroundScale > 0.05f
        ? Shot.MotionBackgroundScale
        : (Motion == TEXT("zoom_in") ? 1.45f : 0.72f);
    const float EndFocalLength = FMath::Clamp(StartFocalLength * Ratio, 8.0f, 200.0f);
    Channel->AddCubicKey(FFrameNumber(StartFrame), StartFocalLength);
    Channel->AddCubicKey(FFrameNumber(FMath::Max(StartFrame, EndFrame - 1)), EndFocalLength);
}

void AddTransformKeys(UMovieScene* MovieScene, const FGuid& CameraGuid, const int32 StartFrame, const int32 EndFrame,
    const FVector& StartLocation, const FRotator& StartRotation, const FVector& EndLocation, const FRotator& EndRotation,
    const FString& Motion = FString())
{
    UMovieScene3DTransformTrack* TransformTrack = MovieScene->FindTrack<UMovieScene3DTransformTrack>(CameraGuid);
    if (!TransformTrack) TransformTrack = MovieScene->AddTrack<UMovieScene3DTransformTrack>(CameraGuid);
    if (!TransformTrack) return;
    UMovieScene3DTransformSection* Section = Cast<UMovieScene3DTransformSection>(TransformTrack->CreateNewSection());
    if (!Section) return;
    Section->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
    TransformTrack->AddSection(*Section);
    TArrayView<FMovieSceneDoubleChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
    if (Channels.Num() < 9) return;
    const double StartValues[9] = { StartLocation.X, StartLocation.Y, StartLocation.Z, StartRotation.Roll, StartRotation.Pitch, StartRotation.Yaw, 1.0, 1.0, 1.0 };
    const double EndValues[9] = { EndLocation.X, EndLocation.Y, EndLocation.Z, EndRotation.Roll, EndRotation.Pitch, EndRotation.Yaw, 1.0, 1.0, 1.0 };
    for (int32 ChannelIndex = 0; ChannelIndex < 9; ++ChannelIndex)
    {
        Channels[ChannelIndex]->AddCubicKey(FFrameNumber(StartFrame), StartValues[ChannelIndex]);
        Channels[ChannelIndex]->AddCubicKey(FFrameNumber(FMath::Max(StartFrame, EndFrame - 1)), EndValues[ChannelIndex]);
    }
    if (Motion == TEXT("handheld") && EndFrame - StartFrame >= 6)
    {
        constexpr int32 HandheldSamples = 6;
        for (int32 Sample = 1; Sample < HandheldSamples; ++Sample)
        {
            const float Alpha = static_cast<float>(Sample) / HandheldSamples;
            const int32 Frame = FMath::RoundToInt(FMath::Lerp(static_cast<float>(StartFrame), static_cast<float>(EndFrame - 1), Alpha));
            const FVector BaseLocation = FMath::Lerp(StartLocation, EndLocation, Alpha);
            const FRotator BaseRotation = FMath::Lerp(StartRotation, EndRotation, Alpha);
            const float Phase = Sample * 2.17f;
            const FVector Jitter(FMath::Sin(Phase) * 3.5f, FMath::Cos(Phase * 1.31f) * 5.0f, FMath::Sin(Phase * 0.83f) * 2.5f);
            const FRotator RotationJitter(FMath::Sin(Phase * 1.47f) * 0.7f, FMath::Cos(Phase * 0.91f) * 1.1f, FMath::Sin(Phase * 1.73f) * 0.45f);
            const double Values[6] = { BaseLocation.X + Jitter.X, BaseLocation.Y + Jitter.Y, BaseLocation.Z + Jitter.Z,
                BaseRotation.Roll + RotationJitter.Roll, BaseRotation.Pitch + RotationJitter.Pitch, BaseRotation.Yaw + RotationJitter.Yaw };
            for (int32 ChannelIndex = 0; ChannelIndex < 6; ++ChannelIndex) Channels[ChannelIndex]->AddCubicKey(FFrameNumber(Frame), Values[ChannelIndex]);
        }
    }
}
}

ULevelSequence* FUPTSequenceBuilder::Build(
    const FUPTCinematicPlan& Plan,
    const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors,
    const TMap<FString, TWeakObjectPtr<UAnimSequenceBase>>& Animations,
    const TMap<FString, TWeakObjectPtr<USoundBase>>& Sounds,
    FString& OutError,
    ULevelSequence* ExistingSequence)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { OutError = TEXT("열려 있는 Editor World가 없습니다."); return nullptr; }

    for (const FUPTCinematicShot& Shot : Plan.Shots)
    {
        if (!Shot.AnimationId.IsEmpty())
        {
            const TWeakObjectPtr<AActor>* ActorPtr = SceneActors.Find(Shot.Subject);
            const TWeakObjectPtr<UAnimSequenceBase>* AnimationPtr = Animations.Find(Shot.AnimationId);
            AActor* Actor = ActorPtr ? ActorPtr->Get() : nullptr;
            UAnimSequenceBase* Animation = AnimationPtr ? AnimationPtr->Get() : nullptr;
            USkeletalMeshComponent* Component = Actor ? Actor->FindComponentByClass<USkeletalMeshComponent>() : nullptr;
            if (!Component || !Component->GetSkeletalMeshAsset())
            { OutError = FString::Printf(TEXT("%s: Subject에 Skeletal Mesh Component가 없습니다."), *Shot.Name); return nullptr; }
            if (!Animation)
            { OutError = FString::Printf(TEXT("%s: Animation ID '%s'를 찾을 수 없습니다."), *Shot.Name, *Shot.AnimationId); return nullptr; }
            if (Animation->GetSkeleton() != Component->GetSkeletalMeshAsset()->GetSkeleton())
            { OutError = FString::Printf(TEXT("%s: 애니메이션과 Subject의 Skeleton이 다릅니다. 먼저 리타기팅이 필요합니다."), *Shot.Name); return nullptr; }
        }
        if (!Shot.LipSyncAnimationId.IsEmpty())
        {
            const TWeakObjectPtr<UAnimSequenceBase>* LipSyncPtr = Animations.Find(Shot.LipSyncAnimationId);
            if (!LipSyncPtr || !LipSyncPtr->IsValid())
            { OutError = FString::Printf(TEXT("%s: Lip Sync Animation ID '%s'를 찾을 수 없습니다."), *Shot.Name, *Shot.LipSyncAnimationId); return nullptr; }
        }
        if (!Shot.AudioId.IsEmpty())
        {
            const TWeakObjectPtr<USoundBase>* SoundPtr = Sounds.Find(Shot.AudioId);
            if (!SoundPtr || !SoundPtr->IsValid())
            { OutError = FString::Printf(TEXT("%s: Audio ID '%s'를 찾을 수 없습니다."), *Shot.Name, *Shot.AudioId); return nullptr; }
        }
    }

    ULevelSequence* Sequence = ExistingSequence;
    if (Sequence && Sequence->GetMovieScene())
    {
        // 샷 수정 반영: 같은 에셋을 유지한 채 내용만 비운다. 바인딩을 지우면 그 바인딩의 트랙도 함께 지워진다.
        Sequence->Modify();
        UMovieScene* ExistingScene = Sequence->GetMovieScene();
        ExistingScene->Modify();
        TArray<FGuid> PossessableGuids;
        TArray<FGuid> SpawnableGuids;
        for (int32 Index = 0; Index < ExistingScene->GetPossessableCount(); ++Index) PossessableGuids.Add(ExistingScene->GetPossessable(Index).GetGuid());
        for (int32 Index = 0; Index < ExistingScene->GetSpawnableCount(); ++Index) SpawnableGuids.Add(ExistingScene->GetSpawnable(Index).GetGuid());
        for (const FGuid& Guid : PossessableGuids)
        {
            Sequence->UnbindPossessableObjects(Guid);
            ExistingScene->RemovePossessable(Guid);
        }
        for (const FGuid& Guid : SpawnableGuids) ExistingScene->RemoveSpawnable(Guid);
        const TArray<UMovieSceneTrack*> GlobalTracks = ExistingScene->GetTracks();
        for (UMovieSceneTrack* Track : GlobalTracks)
        {
            if (Track) ExistingScene->RemoveTrack(*Track);
        }
        ExistingScene->RemoveCameraCutTrack();
        ExistingScene->DeleteMarkedFrames();
    }
    else
    {
        FString SafeName = ObjectTools::SanitizeObjectName(Plan.Title);
        if (SafeName.IsEmpty()) SafeName = TEXT("GeneratedSequence");
        const FString BasePackage = GetDefault<UUPTSettings>()->DefaultSequencePath / SafeName;
        FString PackageName, AssetName;
        FAssetToolsModule::GetModule().Get().CreateUniqueAssetName(BasePackage, TEXT(""), PackageName, AssetName);

        UPackage* Package = CreatePackage(*PackageName);
        Sequence = NewObject<ULevelSequence>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
        if (!Sequence) { OutError = TEXT("Level Sequence 에셋을 생성하지 못했습니다."); return nullptr; }

        Sequence->Initialize();
        FAssetRegistryModule::AssetCreated(Sequence);
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    const FFrameRate FrameRate(Plan.FrameRate, 1);
    MovieScene->SetDisplayRate(FrameRate);
    MovieScene->SetTickResolutionDirectly(FFrameRate(Plan.FrameRate * 1000, 1));
    UMovieSceneCameraCutTrack* CutTrack = Cast<UMovieSceneCameraCutTrack>(MovieScene->AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass()));
    if (!CutTrack) { OutError = TEXT("Camera Cut Track을 생성하지 못했습니다."); return nullptr; }

    TMap<FString, FGuid> ActorBindings;
    for (const TPair<FString, TWeakObjectPtr<AActor>>& Pair : SceneActors)
    {
        if (Pair.Value.IsValid()) ActorBindings.Add(Pair.Key, UMovieSceneSequenceExtensions::AddPossessable(Sequence, Pair.Value.Get()).BindingID);
    }

    UMovieSceneAudioTrack* AudioTrack = nullptr;

    // 카메라 Spawnable 생성 API(AddSpawnableFromInstance)는 대상 Sequence가 Sequencer에 열려 포커스된 상태를 요구한다.
    GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Sequence);
    if (ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence() != Sequence)
    {
        OutError = TEXT("생성한 Level Sequence를 Sequencer에서 열지 못했습니다. 다른 Sequencer 탭을 닫고 다시 생성하세요.");
        return nullptr;
    }

    int32 StartFrame = 0;
    for (int32 Index = 0; Index < Plan.Shots.Num(); ++Index)
    {
        const FUPTCinematicShot& Shot = Plan.Shots[Index];
        const int32 DurationFrames = FMath::Max(1, FMath::RoundToInt(Shot.DurationSeconds * Plan.FrameRate));
        const int32 EndFrame = StartFrame + DurationFrames;

        FVector CameraLocation;
        FRotator CameraRotation;
        UPTFraming::ResolveShotCamera(World, Shot, Plan.AspectRatio, SceneActors, CameraLocation, CameraRotation);
        ACineCameraActor* Camera = World->SpawnActor<ACineCameraActor>(CameraLocation, CameraRotation);
        if (!Camera) { OutError = FString::Printf(TEXT("%s 카메라를 생성하지 못했습니다."), *Shot.Name); return nullptr; }
        Camera->SetActorLabel(FString::Printf(TEXT("UPT_%02d_%s"), Index + 1, *Shot.Name));
        Camera->GetCineCameraComponent()->SetCurrentFocalLength(Shot.FocalLength);
        FCameraFilmbackSettings Filmback = Camera->GetCineCameraComponent()->Filmback;
        Filmback.SensorWidth = UPTFraming::SensorWidthMm;
        Filmback.SensorHeight = UPTFraming::GetSensorHeightMm(Plan.AspectRatio);
        Camera->GetCineCameraComponent()->SetFilmback(Filmback);

        ULevelSequenceEditorSubsystem* LevelSequenceSubsystem = GEditor->GetEditorSubsystem<ULevelSequenceEditorSubsystem>();
        const FMovieSceneBindingProxy CameraBinding = LevelSequenceSubsystem
            ? LevelSequenceSubsystem->AddSpawnableFromInstance(Sequence, Camera)
            : FMovieSceneBindingProxy();
        const FGuid CameraGuid = CameraBinding.BindingID;
        if (!CameraGuid.IsValid())
        {
            World->DestroyActor(Camera);
            OutError = FString::Printf(TEXT("%s 카메라 Spawnable을 생성하지 못했습니다."), *Shot.Name);
            return nullptr;
        }
        const FGuid CameraComponentGuid = UMovieSceneSequenceExtensions::AddPossessable(Sequence, Camera->GetCineCameraComponent()).BindingID;
        if (FMovieScenePossessable* ComponentPossessable = MovieScene->FindPossessable(CameraComponentGuid))
        {
            ComponentPossessable->SetParent(CameraGuid, MovieScene);
            AddFocalLengthKeys(MovieScene, CameraComponentGuid, StartFrame, EndFrame, Shot);
        }
        FVector EndCameraLocation;
        FRotator EndCameraRotation;
        ResolveCameraEndTransform(World, Shot, Plan.AspectRatio, SceneActors, CameraLocation, CameraRotation, EndCameraLocation, EndCameraRotation);
        AddTransformKeys(MovieScene, CameraGuid, StartFrame, EndFrame, CameraLocation, CameraRotation, EndCameraLocation, EndCameraRotation, Shot.CameraMotion);
        UMovieSceneCameraCutSection* CutSection = Cast<UMovieSceneCameraCutSection>(CutTrack->CreateNewSection());
        CutSection->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
        CutSection->SetCameraBindingID(UE::MovieScene::FRelativeObjectBindingID(CameraGuid));
        CutTrack->AddSection(*CutSection);
        World->DestroyActor(Camera);

        for (const FUPTActorBlockingPlacement& Placement : Shot.ActorPlacements)
        {
            const FGuid* ActorGuid = ActorBindings.Find(Placement.ActorLabel);
            if (!ActorGuid || !ActorGuid->IsValid()) continue;
            AddTransformKeys(MovieScene, *ActorGuid, StartFrame, EndFrame,
                Placement.WorldLocation, Placement.WorldRotation, Placement.WorldLocation, Placement.WorldRotation);
        }

        if (!Shot.AnimationId.IsEmpty())
        {
            AActor* Subject = SceneActors.FindChecked(Shot.Subject).Get();
            UAnimSequenceBase* Animation = Animations.FindChecked(Shot.AnimationId).Get();
            USkeletalMeshComponent* Component = Subject->FindComponentByClass<USkeletalMeshComponent>();
            const FGuid ComponentGuid = UMovieSceneSequenceExtensions::AddPossessable(Sequence, Component).BindingID;
            UMovieSceneSkeletalAnimationTrack* AnimationTrack = MovieScene->FindTrack<UMovieSceneSkeletalAnimationTrack>(ComponentGuid);
            if (!AnimationTrack) AnimationTrack = MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(ComponentGuid);
            if (!AnimationTrack) { OutError = FString::Printf(TEXT("%s: Animation Track 생성 실패"), *Shot.Name); return nullptr; }
            UMovieSceneSkeletalAnimationSection* AnimationSection = Cast<UMovieSceneSkeletalAnimationSection>(AnimationTrack->AddNewAnimation(FFrameNumber(StartFrame), Animation));
            if (!AnimationSection) { OutError = FString::Printf(TEXT("%s: Animation Section 생성 실패"), *Shot.Name); return nullptr; }
            AnimationSection->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
        }

        if (!Shot.LipSyncAnimationId.IsEmpty())
        {
            AActor* Subject = SceneActors.FindChecked(Shot.Subject).Get();
            UAnimSequenceBase* LipSyncAnimation = Animations.FindChecked(Shot.LipSyncAnimationId).Get();
            USkeletalMeshComponent* Component = Subject->FindComponentByClass<USkeletalMeshComponent>();
            if (!Component || !Component->GetSkeletalMeshAsset() || LipSyncAnimation->GetSkeleton() != Component->GetSkeletalMeshAsset()->GetSkeleton())
            { OutError = FString::Printf(TEXT("%s: 립싱크 Animation과 Subject Skeleton이 다릅니다."), *Shot.Name); return nullptr; }
            const FGuid ComponentGuid = UMovieSceneSequenceExtensions::AddPossessable(Sequence, Component).BindingID;
            UMovieSceneSkeletalAnimationTrack* LipSyncTrack = MovieScene->AddTrack<UMovieSceneSkeletalAnimationTrack>(ComponentGuid);
            if (!LipSyncTrack) { OutError = FString::Printf(TEXT("%s: Lip Sync Track 생성 실패"), *Shot.Name); return nullptr; }
            LipSyncTrack->SetDisplayName(FText::FromString(TEXT("UPT Lip Sync")));
            UMovieSceneSkeletalAnimationSection* LipSyncSection = Cast<UMovieSceneSkeletalAnimationSection>(LipSyncTrack->AddNewAnimation(FFrameNumber(StartFrame), LipSyncAnimation));
            if (!LipSyncSection) { OutError = FString::Printf(TEXT("%s: Lip Sync Section 생성 실패"), *Shot.Name); return nullptr; }
            LipSyncSection->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
        }

        if (!Shot.Dialogue.IsEmpty())
        {
            FMovieSceneMarkedFrame SubtitleMark{FFrameNumber(StartFrame)};
            SubtitleMark.Label = FString::Printf(TEXT("[SUB] %s%s%s"), *Shot.Speaker, Shot.Speaker.IsEmpty() ? TEXT("") : TEXT(": "), *Shot.Dialogue);
            MovieScene->AddMarkedFrame(SubtitleMark);
        }

        if (!Shot.AudioId.IsEmpty())
        {
            USoundBase* Sound = Sounds.FindChecked(Shot.AudioId).Get();
            if (!AudioTrack) AudioTrack = MovieScene->AddTrack<UMovieSceneAudioTrack>();
            if (!AudioTrack) { OutError = FString::Printf(TEXT("%s: Audio Track 생성 실패"), *Shot.Name); return nullptr; }
            UMovieSceneAudioSection* AudioSection = Cast<UMovieSceneAudioSection>(AudioTrack->AddNewSound(Sound, FFrameNumber(StartFrame)));
            if (!AudioSection) { OutError = FString::Printf(TEXT("%s: Audio Section 생성 실패"), *Shot.Name); return nullptr; }
            AudioSection->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
        }
        StartFrame = EndFrame;
    }

    MovieScene->SetPlaybackRange(FFrameNumber(0), StartFrame);
    Sequence->MarkPackageDirty();
    ULevelSequenceEditorBlueprintLibrary::RefreshCurrentLevelSequence();
    return Sequence;
}
