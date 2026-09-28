#pragma once

#include "CoreMinimal.h"
#include "UPTCinematicTypes.h"

class ULevelSequence;
class AActor;
class UAnimSequenceBase;
class USoundBase;

class FUPTSequenceBuilder
{
public:
    // ExistingSequence를 넘기면 새 에셋을 만들지 않고 그 시퀀스의 바인딩·트랙·카메라 컷·마커를 비운 뒤 다시 채운다(샷 수정 반영용).
    static ULevelSequence* Build(
        const FUPTCinematicPlan& Plan,
        const TMap<FString, TWeakObjectPtr<AActor>>& SceneActors,
        const TMap<FString, TWeakObjectPtr<UAnimSequenceBase>>& Animations,
        const TMap<FString, TWeakObjectPtr<USoundBase>>& Sounds,
        FString& OutError,
        ULevelSequence* ExistingSequence = nullptr);
};
