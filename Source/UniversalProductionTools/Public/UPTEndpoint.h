#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformMisc.h"

// LLM/Vision 엔드포인트 규칙: 원격 서버는 HTTPS만 허용하고, 로컬 모델 서버(Ollama, LM Studio 등)는 localhost HTTP를 허용한다.
namespace UPTEndpoint
{
inline bool IsLocal(const FString& Url)
{
    return Url.StartsWith(TEXT("http://localhost"), ESearchCase::IgnoreCase) || Url.StartsWith(TEXT("http://127.0.0.1"));
}

inline bool IsAllowed(const FString& Url)
{
    return Url.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase) || IsLocal(Url);
}

// .../chat/completions 이면 Chat Completions 형식, 그 외(.../responses)는 OpenAI Responses 형식으로 요청한다.
inline bool UsesChatCompletions(const FString& Url)
{
    return Url.TrimEnd().EndsWith(TEXT("/chat/completions"), ESearchCase::IgnoreCase);
}

// 로컬 서버는 키를 검사하지 않으므로 환경변수가 없으면 자리표시 값을 쓴다.
inline FString ResolveApiKey(const FString& EnvironmentVariable, const FString& Url)
{
    const FString Key = FPlatformMisc::GetEnvironmentVariable(*EnvironmentVariable).TrimStartAndEnd();
    return Key.IsEmpty() && IsLocal(Url) ? FString(TEXT("ollama")) : Key;
}
}
