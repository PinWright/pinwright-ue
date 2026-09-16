// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-get-components-renders-empty-to-caller.
//
// actor.get_components on an actor with many scene components produced a success
// payload large enough that the MCP client silently dropped the text block — no
// error, no visible output. Two spill concerns cover this:
//   1. HttpResponseSpill::MarkOversizedToolResult rewrites an over-threshold
//      ToolResult in place (content[0].text notice + structuredContent marker),
//      keeping a valid MCP shape — covered by LargePayloadMcpRender below.
//   2. The envelope-level spill must NOT clobber an MCP result in the dead band
//      where the ToolResult is under threshold but the full {jsonrpc,id,result}
//      envelope is over it — covered by DeadBandMcpRender below (the #6
//      regression). McpRequestCore::WrapToolResult deliberately applies ONLY the
//      ToolResult-level spill; the test pins that a dead-band response keeps the
//      valid MCP shape instead of degrading to a bare {outputTooLong,...} object.
//
// Ported off the deleted IHttpRouter transport (FMcpTransport): instead of live
// loopback HTTP through per-port test transports, the tests drive the REAL
// dispatcher (actor.get_components handler) with a capturing response sink and
// wrap the completion through McpRequestCore::WrapToolResult at an explicit
// spill threshold — the same wrap+spill pipeline every transport now uses.
#include "Misc/AutomationTest.h"

#include "Components/SceneComponent.h"
#include "Dispatch/RpcDispatcher.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PinWrightHelpers.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "JsonRpc.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestWorldUtils.h"
#include "Transport/McpRequestCore.h"
#include "Utils/HttpResponseSpill.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named namespace (not anonymous): the plugin's tests share a single module with
// Unity builds, and same-name anonymous-namespace helpers across .cpp files
// produce ODR / redefinition errors when Unity merges them into one TU.
namespace GetComponentsSpillTest
{
    // Small enough that a 24-component get_components payload always exceeds it,
    // so the oversize branch is deterministically exercised. Matches the spill
    // clamp minimum (HttpResponseSpill::ClampThresholdCharacters floors at 1024).
    constexpr int32 TinySpillThreshold = 1024;

    // The probe wrap must never spill, so its threshold is effectively
    // unreachable; it exists only to measure a real response's sizes.
    constexpr int32 NoSpillThreshold = 1024 * 1024;

    // Fixed component count for the dead-band probe. Large enough that the
    // serialized result and full envelope both clear a few KB (so the derived
    // threshold has room) yet cheap to build.
    constexpr int32 DeadBandComponentCount = 24;

    struct FDispatchCapture
    {
        bool bCompletionFired = false;
        bool bSuccess = false;
        FString Message;
        TSharedPtr<FJsonObject> Result;
        FString ErrorCode;
    };

    // Dispatch actor.get_components for the given label through the real
    // dispatcher (production auto-registrations, null subsystem) and capture the
    // handler completion the way a transport's sink would.
    inline FDispatchCapture DispatchGetComponents(const FString& ActorLabel)
    {
        FDispatchCapture Capture;
        FRpcDispatcher Dispatcher;
        // Initialize BEFORE draining so the bridge lambdas capture a live sink.
        Dispatcher.Initialize(FResponseSink(
            [&Capture](const FString&, bool bSuccess, const FString& Message,
                       const TSharedPtr<FJsonObject>& Result, const FString& ErrorCode)
            {
                Capture.bCompletionFired = true;
                Capture.bSuccess = bSuccess;
                Capture.Message = Message;
                Capture.Result = Result;
                Capture.ErrorCode = ErrorCode;
            }));
        Dispatcher.DrainAutoRegistrations(nullptr);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("actorName"), ActorLabel);
        Dispatcher.ProcessRequest(TEXT("req-get-components"),
            TEXT("actor.get_components"), Params);
        return Capture;
    }

    // Wrap a captured completion into the full JSON-RPC response the way a
    // transport would, at the given spill threshold.
    inline TSharedPtr<FJsonObject> WrapAtThreshold(const FDispatchCapture& Capture,
                                                   int32 Id, int32 SpillThreshold)
    {
        return McpRequestCore::WrapToolResult(
            Capture.bSuccess, Capture.Message, Capture.Result, Capture.ErrorCode,
            MakeShared<FJsonValueNumber>(Id), SpillThreshold);
    }

    // Measure via the shared production serializer so the dead-band sizing matches
    // what a transport actually puts on the wire.
    inline int32 SerializedLen(const TSharedPtr<FJsonObject>& Object)
    {
        return Object.IsValid() ? JsonRpc::Serialize(Object).Len() : 0;
    }

    inline TSharedPtr<FJsonObject> ResultObject(const TSharedPtr<FJsonObject>& Envelope)
    {
        if (!Envelope.IsValid()) return nullptr;
        const TSharedPtr<FJsonObject>* ResultPtr = nullptr;
        if (Envelope->TryGetObjectField(TEXT("result"), ResultPtr) && ResultPtr)
        {
            return *ResultPtr;
        }
        return nullptr;
    }

    // Attach Count child scene components (plus a root) to Actor. Returns false if
    // the root couldn't be made. Each child carries a distinct relative transform so
    // the serialized entries don't collapse to identical bytes.
    inline bool BuildSceneComponentTree(AActor* Actor, int32 Count, const TCHAR* RootName)
    {
        USceneComponent* Root = NewObject<USceneComponent>(
            Actor, USceneComponent::StaticClass(), RootName, RF_Transactional);
        if (!Root)
        {
            return false;
        }
        Actor->AddInstanceComponent(Root);
        Actor->SetRootComponent(Root);
        Root->RegisterComponent();

        for (int32 Index = 0; Index < Count; ++Index)
        {
            const FString CompName = FString::Printf(TEXT("SpillChild_%02d"), Index);
            USceneComponent* Child = NewObject<USceneComponent>(
                Actor, USceneComponent::StaticClass(), *CompName, RF_Transactional);
            if (!Child)
            {
                continue;
            }
            Actor->AddInstanceComponent(Child);
            Child->SetupAttachment(Root);
            Child->SetRelativeLocation(FVector(Index * 11.0, Index * 22.0, Index * 33.0));
            Child->RegisterComponent();
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetComponentsLargePayloadMcpRenderTest,
    "PinWright.actor.get_components.LargePayloadMcpRender",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetComponentsLargePayloadMcpRenderTest::RunTest(const FString& Parameters)
{
    using namespace GetComponentsSpillTest;
    FScopedEditorWorldActorGuard WorldGuard;

    // Redirect spill output to a throwaway dir so the test never pollutes Saved/.
    const FString SpillRoot = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectIntermediateDir() /
        TEXT("PinWrightTests/GetComponentsLargePayload") /
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(SpillRoot);
    ON_SCOPE_EXIT
    {
        HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
        IFileManager::Get().DeleteDirectory(*SpillRoot, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString ActorLabel = FString::Printf(TEXT("PW_GetComponentsLarge_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AActor* Actor = SpawnActorInActiveWorld<AActor>(
        AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, ActorLabel);
    if (!TestNotNull(TEXT("actor spawned"), Actor))
    {
        return true;
    }
    // 24 scene components total (root + 23). Each carries relative
    // location/rotation/scale objects, easily clearing the 1024-char threshold.
    if (!TestTrue(TEXT("scene component tree built"),
            BuildSceneComponentTree(Actor, 23, TEXT("LargeRoot"))))
    {
        return true;
    }

    const FDispatchCapture Capture = DispatchGetComponents(ActorLabel);
    if (!TestTrue(TEXT("handler completion fired"), Capture.bCompletionFired))
    {
        return true;
    }
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);

    // Wrap at the tiny threshold: the ToolResult-level spill must fire.
    const TSharedPtr<FJsonObject> Response = WrapAtThreshold(Capture, 1, TinySpillThreshold);
    if (!TestTrue(TEXT("wrapped response present"), Response.IsValid())) return true;

    TSharedPtr<FJsonObject> Result = ResultObject(Response);
    if (!TestTrue(TEXT("result present"), Result.IsValid())) return true;

    // 1. Not an error.
    bool bIsError = true;
    Result->TryGetBoolField(TEXT("isError"), bIsError);
    TestFalse(TEXT("isError is false"), bIsError);

    // 2. content[0].text is never the empty string that the client used to drop.
    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    FString Text;
    if (TestTrue(TEXT("content array present"),
            Result->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num() > 0))
    {
        const TSharedPtr<FJsonObject> Block = (*Content)[0]->AsObject();
        if (TestTrue(TEXT("content[0] is an object"), Block.IsValid()))
        {
            Text = Block->GetStringField(TEXT("text"));
            TestFalse(TEXT("content[0].text is NOT empty"), Text.IsEmpty());
        }
    }

    // 3. structuredContent.outputTooLong is true (payload exceeds the threshold).
    const TSharedPtr<FJsonObject>* Structured = nullptr;
    bool bOutputTooLong = false;
    if (TestTrue(TEXT("structuredContent present"),
            Result->TryGetObjectField(TEXT("structuredContent"), Structured) && Structured && (*Structured).IsValid()))
    {
        (*Structured)->TryGetBoolField(TEXT("outputTooLong"), bOutputTooLong);
        TestTrue(TEXT("structuredContent.outputTooLong is true"), bOutputTooLong);

        // 4 (structured half): a file reference points at the spilled full JSON.
        const TSharedPtr<FJsonObject>* File = nullptr;
        if (TestTrue(TEXT("structuredContent.file present"),
                (*Structured)->TryGetObjectField(TEXT("file"), File) && File && (*File).IsValid()))
        {
            FString SpillPath;
            if (TestTrue(TEXT("file.path present"),
                    (*File)->TryGetStringField(TEXT("path"), SpillPath) && !SpillPath.IsEmpty()))
            {
                TestTrue(TEXT("spill path stays under override root"),
                    SpillPath.StartsWith(SpillRoot + TEXT("/")));
                TestTrue(TEXT("spill file exists on disk"),
                    IFileManager::Get().FileExists(*SpillPath));
            }
        }
    }

    // 4 (text half): the notice explains the truncation rather than rendering nothing.
    TestTrue(TEXT("content[0].text carries the 'Response exceeds' notice"),
        Text.Contains(TEXT("Response exceeds")));

    return true;
}

// Regression for the #6 dead band: an MCP tools/call result whose serialized
// ToolResult is UNDER the spill threshold but whose full {jsonrpc,id,result}
// envelope is OVER it. The ToolResult-level MarkOversizedToolResult does not
// fire (result < threshold), and the envelope-level spill must not fire either:
// pre-fix, the legacy transport's envelope spill REPLACED `result` with a bare
// {outputTooLong,message,file} object — no content[] array, no isError — which
// the MCP client renders as empty output. WrapToolResult now applies only the
// ToolResult-level spill by design; this test pins that a dead-band response
// keeps the valid MCP shape.
//
// Hitting the dead band is delicate: each scene component grows the serialized
// result by far more than the envelope-vs-result gap (the result carries the
// payload twice -- once in structuredContent, once serialized into
// content[0].text -- while the envelope only adds the {jsonrpc,id,result}
// wrapper). A fixed threshold + coarse component-count search overshoots the
// narrow band, so instead
// this PROBES one real wrapped response (huge threshold, no spill) to read its
// exact ResultLen and EnvelopeLen, then DERIVES a threshold strictly inside that
// [ResultLen, EnvelopeLen) gap and re-wraps the same completion at that threshold.
// That reproduces the exact dead band deterministically.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorGetComponentsDeadBandMcpRenderTest,
    "PinWright.actor.get_components.DeadBandMcpRender",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorGetComponentsDeadBandMcpRenderTest::RunTest(const FString& Parameters)
{
    using namespace GetComponentsSpillTest;
    FScopedEditorWorldActorGuard WorldGuard;

    // The dead-band wrap must not spill, but if it ever DID fire it would write a
    // spill file; keep that out of Saved/ either way.
    const FString SpillRoot = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectIntermediateDir() /
        TEXT("PinWrightTests/GetComponentsDeadBand") /
        FGuid::NewGuid().ToString(EGuidFormats::Digits));
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(SpillRoot);
    ON_SCOPE_EXIT
    {
        HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
        IFileManager::Get().DeleteDirectory(*SpillRoot, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString ActorLabel = FString::Printf(TEXT("PW_GetComponentsDeadBand_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AActor* Actor = SpawnActorInActiveWorld<AActor>(
        AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, ActorLabel);
    if (!TestNotNull(TEXT("actor spawned"), Actor))
    {
        return true;
    }
    if (!TestTrue(TEXT("scene component tree built"),
            BuildSceneComponentTree(Actor, DeadBandComponentCount, TEXT("DeadBandRoot"))))
    {
        return true;
    }

    const FDispatchCapture Capture = DispatchGetComponents(ActorLabel);
    if (!TestTrue(TEXT("handler completion fired"), Capture.bCompletionFired))
    {
        return true;
    }
    TestTrue(TEXT("handler reported success"), Capture.bSuccess);

    // Phase 1 — PROBE: wrap with a non-spilling threshold so we learn the actual
    // serialized result and envelope sizes.
    const TSharedPtr<FJsonObject> Probe = WrapAtThreshold(Capture, 1, NoSpillThreshold);
    if (!TestTrue(TEXT("probe response present"), Probe.IsValid()))
    {
        return true;
    }

    const TSharedPtr<FJsonObject> ProbeResult = ResultObject(Probe);
    const int32 ResultLen = SerializedLen(ProbeResult);
    const int32 EnvelopeLen = JsonRpc::Serialize(Probe).Len();

    // The dead band only exists when the envelope is strictly larger than the inner
    // result (it always is — the wrapper + indentation add bytes). Need at least a
    // 2-char gap so a threshold can sit strictly between them.
    if (!TestTrue(TEXT("probe result is non-empty"), ResultLen > 0)
        || !TestTrue(
            FString::Printf(TEXT("envelope (%d) exceeds inner result (%d) by >=2 chars"),
                EnvelopeLen, ResultLen),
            EnvelopeLen >= ResultLen + 2))
    {
        return true;
    }

    // Phase 2 — DERIVE a threshold strictly inside [ResultLen, EnvelopeLen):
    // result <= threshold (so MarkOversizedToolResult stays silent) AND
    // threshold < envelope (so a hypothetical envelope-level spill would fire).
    // Midpoint keeps it clear of both edges.
    //
    // The result-level gate has a MARGIN here, not a coincidence: since
    // 2026-08-23 MarkOversizedToolResult measures ONE condensed copy of the
    // reader-facing payload rather than the serialized wrapper, and the wrapper
    // carries that payload twice. So the number it compares is roughly half
    // ResultLen, comfortably under any threshold at or above ResultLen. The
    // envelope side of the band is unchanged. See HttpResponseSpill.cpp
    // (MeasureReaderFacingCharacters) for why the old measurement was wrong.
    const int32 DerivedThreshold = ResultLen + (EnvelopeLen - ResultLen) / 2;

    // Phase 3 — re-wrap the same completion at the derived threshold: this lands
    // the response squarely in the dead band.
    const TSharedPtr<FJsonObject> Response = WrapAtThreshold(Capture, 2, DerivedThreshold);
    if (!TestTrue(TEXT("dead-band response present"), Response.IsValid()))
    {
        return true;
    }

    const TSharedPtr<FJsonObject> InBandResult = ResultObject(Response);
    if (!TestTrue(TEXT("in-band result present"), InBandResult.IsValid()))
    {
        return true;
    }

    // Confirm we really are in the dead band: inner result within the derived
    // threshold, full envelope over it. (Sizes match the probe — same payload.)
    const int32 InBandResultLen = SerializedLen(InBandResult);
    const int32 InBandEnvelopeLen = JsonRpc::Serialize(Response).Len();
    TestTrue(
        FString::Printf(TEXT("response sits in the dead band (result %d <= threshold %d < envelope %d)"),
            InBandResultLen, DerivedThreshold, InBandEnvelopeLen),
        InBandResultLen <= DerivedThreshold && InBandEnvelopeLen > DerivedThreshold);

    // The load-bearing assertions: in the dead band the response must remain a
    // valid MCP tools/call result. Pre-fix the envelope spill replaced `result`
    // with {outputTooLong,message,file} — no content[], no isError — so these fail.
    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    TestTrue(TEXT("dead-band result keeps a non-empty content[] (valid MCP shape)"),
        InBandResult->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num() > 0);

    bool bHasIsError = false;
    bool bIsError = true;
    if (InBandResult->TryGetBoolField(TEXT("isError"), bIsError))
    {
        bHasIsError = true;
    }
    TestTrue(TEXT("dead-band result carries isError (valid MCP shape)"), bHasIsError);
    TestFalse(TEXT("dead-band result is not an error"), bIsError);

    // The envelope-spill signature is a top-level outputTooLong on `result`; the
    // real success result never has it, so its presence means an envelope spill
    // clobbered the result (the bug).
    bool bEnvelopeSpillSignature = false;
    InBandResult->TryGetBoolField(TEXT("outputTooLong"), bEnvelopeSpillSignature);
    TestFalse(TEXT("dead-band result is NOT the envelope-spill {outputTooLong} shape"),
        bEnvelopeSpillSignature);

    return true;
}

#endif
