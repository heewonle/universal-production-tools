#include "UPTLLMClient.h"
#include "UPTEndpoint.h"
#include "UPTSettings.h"

#include "Dom/JsonObject.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
// 프롬프트는 함수로 반환한다. 전역 문자열 상수는 Live Coding 패치 후에도 이전 값이 남을 수 있다.
const TCHAR* GetSystemPrompt() { return TEXT(R"PROMPT(
You convert a cinematic script into an Unreal Engine camera shot plan.
Return exactly one JSON object and nothing else.
Schema (types only - choose real values for every shot; small models must not copy placeholder numbers):
{
  "title": "short English title, never empty",
  "frame_rate": 30,
  "shots": [{
    "name": "string", "description": "string",
    "shot_size": "wide | full | medium | close_up | extreme_close_up",
    "camera_angle": "eye | low | high | overhead",
    "subject": "exact selected actor label or empty", "look_at": "exact selected actor label or empty",
    "animation_id": "allowed id or empty", "audio_id": "allowed id or empty", "lip_sync_animation_id": "allowed id or empty",
    "speaker": "string or empty", "dialogue": "string or empty",
    "duration_seconds": "number", "distance_cm": "number", "height_cm": "number", "side_cm": "number",
    "focal_length": "number", "focus_height": "number", "subject_screen_x": "number", "subject_screen_y": "number",
    "camera_motion": "one of static, pan, tilt, dolly_in, dolly_out, zoom_in, zoom_out, truck_left, truck_right, pedestal, orbit, tracking, handheld",
    "camera_location": "{x,y,z} object, only when subject is empty", "camera_rotation": "{pitch,yaw,roll} object, only when subject is empty"
  }]
}
First decide shot_size and camera_angle for each shot from the script, then pick numbers from these tables. Never give every shot the same numbers.
shot_size -> distance_cm / focal_length / focus_height:
- wide: 800-1200 / 20-28 / 0.5
- full: 400-600 / 30-40 / 0.5
- medium: 220-320 / 40-55 / 0.72
- close_up: 120-180 / 70-90 / 0.9
- extreme_close_up: 70-110 / 85-135 / 0.93
camera_angle -> height_cm: eye 0; low -80 to -40 (negative: camera below the aim point); high 120 to 250; overhead 400 to 700.
subject_screen_x: 0.33 or 0.66 for rule-of-thirds, alternating sides between consecutive shots; 0.5 only for wide or symmetric shots. subject_screen_y: 0.38-0.45 for close-ups, 0.45-0.55 otherwise.
side_cm: 0 for a frontal view, about 30 percent of distance_cm (positive or negative) for a three-quarter view.
camera_motion must follow the script: circling or turning around -> orbit, approaching -> dolly_in, pulling away -> dolly_out, otherwise static.
Use 1-20 shots and duration_seconds 2-5 per shot unless the script says otherwise. Distances are Unreal centimeters; distance_cm places the camera in front of the subject, side_cm moves toward the subject's own right (the left side of the frame), height_cm moves up from the aim point, and focus_height is the aim point from 0 (feet) to 1 (top of head).
Copy selected actor labels exactly into subject and look_at. Use only animation_id, lip_sync_animation_id and audio_id values listed in ALLOWED MEDIA and never invent one; animation ids require a non-empty subject.
Never emit asset paths, commands or code.
)PROMPT"); }

const TCHAR* GetReferencePrompt() { return TEXT(R"PROMPT(
Analyze the attached ordered reference-video frames for Unreal cinematic blocking. Obey REQUESTED MATCH MODE. In scene mode ignore subtitles, logos, black bars and editorial overlays. In edit mode preserve editorial zooms, shakes and cut timing as camera/editing intent, but never reproduce logos or watermarks.
Return JSON only with this schema:
{"title":"Reference","match_mode":"scene","shots":[{"name":"Shot_01","subject_role":"main_character","start_seconds":0.0,"end_seconds":2.0,"shot_size":"wide|full|medium|close_up|extreme_close_up","camera_angle":"low|eye|high|overhead|dutch","camera_motion":"static|pan|tilt|dolly_in|dolly_out|zoom_in|zoom_out|truck_left|truck_right|pedestal|orbit|tracking|handheld","subject_screen_position":{"x":0.5,"y":0.5},"subject_screen_size":{"width":0.3,"height":0.5},"subjects":[{"role":"main_character","screen_position":{"x":0.4,"y":0.5},"screen_size":{"width":0.25,"height":0.5},"depth_order":0},{"role":"enemy","screen_position":{"x":0.7,"y":0.5},"screen_size":{"width":0.2,"height":0.45},"depth_order":1}],"composition":"center|thirds|symmetry|leading_space","confidence":0.8}]}
Coordinates and sizes are normalized 0..1. Use stable semantic role ids, never names inferred as real identities. Shot times must be monotonic and based on the timestamp labels. Do not emit asset paths or instructions.
Use zoom_in/zoom_out when framing changes without perspective/parallax change. Use dolly_in/dolly_out only when camera translation and perspective/parallax change are visible.
Pick exactly one value for every enum field; never output the "a|b|c" option lists themselves.
)PROMPT"); }

// 모델 응답에서 JSON 객체만 추출한다. 로컬 모델은 코드 펜스, 사고 과정, 설명 문장을 덧붙이는 경우가 있다.
FString ExtractJsonText(FString Value)
{
    const int32 ThinkEnd = Value.Find(TEXT("</think>"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
    if (ThinkEnd != INDEX_NONE) Value.RightChopInline(ThinkEnd + 8);
    Value.TrimStartAndEndInline();
    const int32 Start = Value.Find(TEXT("{"));
    const int32 End = Value.Find(TEXT("}"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    return Start != INDEX_NONE && End > Start ? Value.Mid(Start, End - Start + 1) : Value;
}

bool ExtractResponseText(const TSharedPtr<FJsonObject>& Json, const bool bChatCompletions, FString& OutText)
{
    if (bChatCompletions)
    {
        const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
        if (!Json->TryGetArrayField(TEXT("choices"), Choices) || !Choices || Choices->IsEmpty()) return false;
        const TSharedPtr<FJsonObject> Choice = (*Choices)[0]->AsObject();
        const TSharedPtr<FJsonObject>* Message = nullptr;
        return Choice.IsValid() && Choice->TryGetObjectField(TEXT("message"), Message) && Message
            && (*Message)->TryGetStringField(TEXT("content"), OutText) && !OutText.IsEmpty();
    }

    const TArray<TSharedPtr<FJsonValue>>* Output = nullptr;
    if (!Json->TryGetArrayField(TEXT("output"), Output) || !Output) return false;
    for (const TSharedPtr<FJsonValue>& ItemValue : *Output)
    {
        const TSharedPtr<FJsonObject> Item = ItemValue->AsObject();
        const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
        if (!Item.IsValid() || !Item->TryGetArrayField(TEXT("content"), Parts) || !Parts) continue;
        for (const TSharedPtr<FJsonValue>& PartValue : *Parts)
        {
            const TSharedPtr<FJsonObject> Part = PartValue->AsObject();
            if (Part.IsValid() && Part->TryGetStringField(TEXT("text"), OutText) && !OutText.IsEmpty()) return true;
        }
    }
    return false;
}
}

void FUPTLLMClient::SendRequest(const FString& Url, const FString& ApiKey, const TSharedRef<FJsonObject>& Body, const bool bChatCompletions,
    const FString& ServiceLabel, FUPTLLMComplete Completion)
{
    FString BodyText;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyText);
    FJsonSerializer::Serialize(Body, Writer);

    const bool bLocal = UPTEndpoint::IsLocal(Url);
    TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(Url);
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ApiKey));
    // 비스트리밍 요청은 모델이 응답을 다 만들 때까지 데이터가 오지 않는다. 엔진 기본 유휴 타임아웃(30초)으로는 끊기므로 늘린다.
    Request->SetTimeout(bLocal ? 900.0f : 300.0f);
    Request->SetActivityTimeout(bLocal ? 900.0f : 300.0f);
    Request->SetContentAsString(BodyText);
    bRequestInFlight = true;

    TWeakPtr<FUPTLLMClient> WeakClient = AsShared();
    Request->OnProcessRequestComplete().BindLambda([WeakClient, Completion, bChatCompletions, bLocal, ServiceLabel, Url](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
    {
        const TSharedPtr<FUPTLLMClient> Self = WeakClient.Pin();
        if (!Self) return;
        Self->bRequestInFlight = false;
        if (!bConnected || !Response.IsValid())
        {
            Completion.ExecuteIfBound(false, bLocal
                ? FString::Printf(TEXT("로컬 모델 서버(%s)에 연결하지 못했거나 응답 시간이 초과됐습니다. Ollama가 실행 중인지 확인하세요."), *Url)
                : FString::Printf(TEXT("%s 서버에 연결하지 못했거나 응답 시간이 초과됐습니다."), *ServiceLabel));
            return;
        }
        if (Response->GetResponseCode() < 200 || Response->GetResponseCode() >= 300)
        {
            Completion.ExecuteIfBound(false, FString::Printf(TEXT("%s 오류 (%d): %s"), *ServiceLabel, Response->GetResponseCode(), *Response->GetContentAsString().Left(800)));
            return;
        }
        TSharedPtr<FJsonObject> Json;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()), Json) || !Json.IsValid())
        {
            Completion.ExecuteIfBound(false, FString::Printf(TEXT("%s 응답 JSON을 읽지 못했습니다."), *ServiceLabel));
            return;
        }
        FString Text;
        if (!ExtractResponseText(Json, bChatCompletions, Text))
        {
            Completion.ExecuteIfBound(false, FString::Printf(TEXT("%s 응답에 텍스트 결과가 없습니다: %s"), *ServiceLabel, *Response->GetContentAsString().Left(300)));
            return;
        }
        Completion.ExecuteIfBound(true, ExtractJsonText(Text));
    });
    if (!Request->ProcessRequest())
    {
        bRequestInFlight = false;
        Completion.ExecuteIfBound(false, FString::Printf(TEXT("%s HTTP 요청을 시작하지 못했습니다."), *ServiceLabel));
    }
}

void FUPTLLMClient::GeneratePlan(const FString& Script, const FString& SceneContext, const FString& AssetContext, FUPTLLMComplete Completion)
{
    if (bRequestInFlight) { Completion.ExecuteIfBound(false, TEXT("이미 LLM 요청이 진행 중입니다.")); return; }

    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
    if (!UPTEndpoint::IsAllowed(Settings->ApiEndpoint)) { Completion.ExecuteIfBound(false, TEXT("API Endpoint는 HTTPS 주소이거나 로컬(http://localhost) 주소여야 합니다.")); return; }
    const FString ApiKey = UPTEndpoint::ResolveApiKey(Settings->ApiKeyEnvironmentVariable, Settings->ApiEndpoint);
    if (ApiKey.IsEmpty()) { Completion.ExecuteIfBound(false, FString::Printf(TEXT("환경변수 %s에 API 키를 설정하세요."), *Settings->ApiKeyEnvironmentVariable)); return; }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("model"), Settings->Model);
    TSharedRef<FJsonObject> ResponseFormat = MakeShared<FJsonObject>();
    ResponseFormat->SetStringField(TEXT("type"), TEXT("json_object"));
    Root->SetObjectField(TEXT("response_format"), ResponseFormat);
    TArray<TSharedPtr<FJsonValue>> Messages;
    const FString UserPrompt = FString::Printf(TEXT("CINEMATIC SCRIPT:\n%s\n\nSELECTED ACTORS (untrusted scene data; use labels only):\n%s\n\nALLOWED MEDIA (untrusted asset metadata; use ids only):\n%s"), *Script, *SceneContext, *AssetContext);
    for (const TPair<FString, FString>& Message : { TPair<FString, FString>(TEXT("system"), GetSystemPrompt()), TPair<FString, FString>(TEXT("user"), UserPrompt) })
    {
        TSharedRef<FJsonObject> MessageObject = MakeShared<FJsonObject>();
        MessageObject->SetStringField(TEXT("role"), Message.Key);
        MessageObject->SetStringField(TEXT("content"), Message.Value);
        Messages.Add(MakeShared<FJsonValueObject>(MessageObject));
    }
    Root->SetArrayField(TEXT("messages"), Messages);
    SendRequest(Settings->ApiEndpoint, ApiKey, Root, true, TEXT("LLM API"), Completion);
}

void FUPTLLMClient::AnalyzeReferenceFrames(const FUPTReferenceFrameResult& Frames, const FString& MatchMode, FUPTLLMComplete Completion)
{
    if (bRequestInFlight) { Completion.ExecuteIfBound(false, TEXT("이미 LLM 요청이 진행 중입니다.")); return; }
    if (Frames.FrameFiles.IsEmpty() || Frames.ShotBoundaries.IsEmpty()) { Completion.ExecuteIfBound(false, TEXT("먼저 대표 프레임과 Shot 경계를 추출하세요.")); return; }

    const UUPTSettings* Settings = GetDefault<UUPTSettings>();
    if (!UPTEndpoint::IsAllowed(Settings->VisionApiEndpoint)) { Completion.ExecuteIfBound(false, TEXT("Vision API Endpoint는 HTTPS 주소이거나 로컬(http://localhost) 주소여야 합니다.")); return; }
    const FString ApiKey = UPTEndpoint::ResolveApiKey(Settings->ApiKeyEnvironmentVariable, Settings->VisionApiEndpoint);
    if (ApiKey.IsEmpty()) { Completion.ExecuteIfBound(false, FString::Printf(TEXT("환경변수 %s에 API 키를 설정하세요."), *Settings->ApiKeyEnvironmentVariable)); return; }

    // 로컬 모델은 컨텍스트와 VRAM이 작아 한 번에 보내는 이미지 수를 줄인다.
    const bool bLocal = UPTEndpoint::IsLocal(Settings->VisionApiEndpoint);
    const int32 MaxFrames = FMath::Clamp(Settings->MaxVisionFrames, 1, bLocal ? 8 : 20);
    TArray<int32> FrameIndices;
    TArray<int32> ShotStarts;
    for (const FUPTShotBoundary& Boundary : Frames.ShotBoundaries) ShotStarts.AddUnique(Boundary.FrameIndex);
    if (ShotStarts.Num() > MaxFrames)
    {
        for (int32 Index = 0; Index < MaxFrames; ++Index)
        {
            FrameIndices.AddUnique(ShotStarts[FMath::RoundToInt(Index * (ShotStarts.Num() - 1.0f) / FMath::Max(1, MaxFrames - 1))]);
        }
    }
    else
    {
        FrameIndices = ShotStarts;
        for (int32 Pass = 0; Pass < 2 && FrameIndices.Num() < MaxFrames; ++Pass)
        {
            for (int32 ShotIndex = 0; ShotIndex < ShotStarts.Num() && FrameIndices.Num() < MaxFrames; ++ShotIndex)
            {
                const int32 Start = ShotStarts[ShotIndex];
                const int32 End = (ShotIndex + 1 < ShotStarts.Num()) ? FMath::Max(Start, ShotStarts[ShotIndex + 1] - 1) : Frames.FrameFiles.Num() - 1;
                const int32 Candidate = Pass == 0 ? FMath::RoundToInt((Start + End) * 0.5f) : End;
                FrameIndices.AddUnique(Candidate);
            }
        }
    }
    FrameIndices.Sort();

    FString TimestampList = FString::Printf(TEXT("\nREQUESTED MATCH MODE: %s\nLOCAL SHOT BOUNDARY CANDIDATES (use as timing anchors):\n"), MatchMode == TEXT("edit") ? TEXT("edit") : TEXT("scene"));
    for (const FUPTShotBoundary& Boundary : Frames.ShotBoundaries)
    {
        TimestampList += FString::Printf(TEXT("shot_boundary %.3fs confidence_delta=%.3f\n"), Boundary.TimeSeconds, Boundary.ChangeScore);
    }
    TimestampList += TEXT("FRAME TIMESTAMPS:\n");
    for (const int32 FrameIndex : FrameIndices) TimestampList += FString::Printf(TEXT("frame_%04d = %.3fs\n"), FrameIndex + 1, FrameIndex * Frames.SampleIntervalSeconds);

    const bool bChatCompletions = UPTEndpoint::UsesChatCompletions(Settings->VisionApiEndpoint);
    TArray<TSharedPtr<FJsonValue>> Content;
    auto AddText = [&Content, bChatCompletions](const FString& Text)
    {
        TSharedRef<FJsonObject> Part = MakeShared<FJsonObject>();
        Part->SetStringField(TEXT("type"), bChatCompletions ? TEXT("text") : TEXT("input_text"));
        Part->SetStringField(TEXT("text"), Text);
        Content.Add(MakeShared<FJsonValueObject>(Part));
    };
    auto AddImage = [&Content, bChatCompletions](const FString& DataUrl)
    {
        TSharedRef<FJsonObject> Part = MakeShared<FJsonObject>();
        if (bChatCompletions)
        {
            Part->SetStringField(TEXT("type"), TEXT("image_url"));
            TSharedRef<FJsonObject> ImageUrl = MakeShared<FJsonObject>();
            ImageUrl->SetStringField(TEXT("url"), DataUrl);
            Part->SetObjectField(TEXT("image_url"), ImageUrl);
        }
        else
        {
            Part->SetStringField(TEXT("type"), TEXT("input_image"));
            Part->SetStringField(TEXT("image_url"), DataUrl);
            Part->SetStringField(TEXT("detail"), TEXT("low"));
        }
        Content.Add(MakeShared<FJsonValueObject>(Part));
    };

    AddText(FString(GetReferencePrompt()) + TimestampList);
    int32 AddedImages = 0;
    for (const int32 FrameIndex : FrameIndices)
    {
        TArray<uint8> Bytes;
        if (!Frames.FrameFiles.IsValidIndex(FrameIndex) || !FFileHelper::LoadFileToArray(Bytes, *Frames.FrameFiles[FrameIndex])) continue;
        // 이미지마다 타임스탬프 라벨을 붙여 작은 모델도 순서와 시간을 헷갈리지 않게 한다.
        AddText(FString::Printf(TEXT("frame_%04d = %.3fs"), FrameIndex + 1, FrameIndex * Frames.SampleIntervalSeconds));
        AddImage(TEXT("data:image/jpeg;base64,") + FBase64::Encode(Bytes));
        ++AddedImages;
    }
    if (AddedImages == 0) { Completion.ExecuteIfBound(false, TEXT("Vision 모델로 보낼 대표 프레임을 읽지 못했습니다.")); return; }

    TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
    Message->SetStringField(TEXT("role"), TEXT("user"));
    Message->SetArrayField(TEXT("content"), Content);
    TArray<TSharedPtr<FJsonValue>> Messages;
    Messages.Add(MakeShared<FJsonValueObject>(Message));

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("model"), Settings->VisionModel);
    TSharedRef<FJsonObject> Format = MakeShared<FJsonObject>();
    Format->SetStringField(TEXT("type"), TEXT("json_object"));
    if (bChatCompletions)
    {
        Root->SetArrayField(TEXT("messages"), Messages);
        Root->SetObjectField(TEXT("response_format"), Format);
    }
    else
    {
        Root->SetArrayField(TEXT("input"), Messages);
        TSharedRef<FJsonObject> TextConfig = MakeShared<FJsonObject>();
        TextConfig->SetObjectField(TEXT("format"), Format);
        Root->SetObjectField(TEXT("text"), TextConfig);
    }
    SendRequest(Settings->VisionApiEndpoint, ApiKey, Root, bChatCompletions, bLocal ? TEXT("로컬 Vision 모델") : TEXT("Vision API"), Completion);
}
