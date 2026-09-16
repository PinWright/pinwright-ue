// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Transport-agnostic MCP request processing used by the raw-socket streaming
// transport (SocketHttpServer). Free functions only — no UObject, no
// HttpServer types: callers hand in the decoded body string plus the
// Authorization header value they extracted, and get back either a
// ready-to-serialize JSON body or a dispatch instruction for FRpcDispatcher.
namespace McpRequestCore
{
    // Per-call inputs that vary by transport. FromSettings() derives the values
    // from UPinWrightSettings (clamping MaxBodyBytes to >= 1024); it reads the
    // gateway-token file when auth is required, so transports that process many
    // requests should build one config at startup and reuse it rather than
    // paying that disk read per request.
    struct FRequestConfig
    {
        // Enforced bearer token; empty disables the auth gate.
        FString AuthToken;

        // Maximum accepted request body size in bytes (callers pass an
        // already-clamped value; FromSettings clamps to >= 1024).
        int32 MaxBodyBytes = 1024;

        // Raw on-wire byte count of the request body, when the transport knows
        // it (UE's HttpServer hands over the exact byte array). -1 = measure
        // the decoded Body string as UTF-8 instead.
        int32 BodyBytes = -1;

        // Readiness snapshot published by the game-thread subsystem. The
        // Fail closed until a caller publishes an explicit readiness snapshot.
        // The live socket server overwrites these fields before each request;
        // tests that model a ready editor must opt in explicitly. Every tools/call
        // is gated from this snapshot before wiki lookup or handler dispatch.
        bool bEditorReady = false;
        bool bEditorLoadingPackage = true;
        bool bEditorSelectionSetAvailable = false;

        // Modal-block snapshot from ModalStateProbe, already threshold-gated by the
        // transport (UPinWrightSettings::ModalBlockedReportSeconds) so this layer
        // stays pure. When false every response is byte-identical to before these
        // fields existed; when true the readiness snapshot above is stale-but-
        // positive and must not be trusted, because every writer of it runs on the
        // blocked game thread.
        bool bBlockedOnModal = false;
        double BlockedOnModalSeconds = 0.0;
        // Empty when the active modal window could not be read. Omitted from
        // responses rather than emitted as "".
        FString ModalTitle;

        // Game-thread staleness snapshot from ModalStateProbe's heartbeat, already
        // threshold-gated by the transport (UPinWrightSettings::GameThreadStallReportSeconds).
        // Distinct from bBlockedOnModal in cause and in consequence: a modal is
        // NON-retryable and gates tools/call, a stall is RETRYABLE and does NOT -
        // the wedged handler may still return, and the queued request will run when
        // it does. Only `ping` reads these. When false every response is
        // byte-identical to before these fields existed.
        bool bGameThreadStalled = false;
        double GameThreadStalledSeconds = 0.0;

        // The RPC the game thread was inside when the snapshot was taken, empty
        // when none. A duration alone cannot tell an agent whether its own request
        // caused the wedge; the method name can. Omitted from responses rather than
        // emitted as "".
        FString InFlightMethod;
        FString InFlightRequestId;
        double InFlightSeconds = 0.0;

        PINWRIGHT_API static FRequestConfig FromSettings();
    };

    // Outcome of processing one request body.
    struct FRequestDecision
    {
        enum class EKind
        {
            // Transport serializes ImmediateBody (HTTP status = HttpCode) and
            // is done. A null ImmediateBody with HttpCode 202 is the
            // notification ack: empty HTTP body, no JSON-RPC reply per spec.
            ImmediateResponse,
            // tools/call execute path: transport registers a completion and
            // hands Method/Args to the dispatcher, then wraps the eventual
            // handler result via WrapToolResult.
            DispatchRpc,
        };
        EKind Kind = EKind::ImmediateResponse;

        // ImmediateResponse payload.
        TSharedPtr<FJsonObject> ImmediateBody;
        int32 HttpCode = 200;

        // DispatchRpc payload: dotted RPC method + params object.
        FString Method;
        TSharedPtr<FJsonObject> Args;

        // JSON-RPC envelope id (null until the envelope parses, and for
        // notifications). DispatchRpc callers feed this to WrapToolResult.
        TSharedPtr<FJsonValue> EnvelopeId;

        // Streaming inputs surfaced on the DispatchRpc path so the SSE-capable
        // transport can decide whether to upgrade the response stream.
        bool bHasProgressToken = false;                // params._meta.progressToken present
        TSharedPtr<FJsonValue> ProgressToken;
        // True unless the caller passed args.wait == false — block-and-stream
        // is the default for streaming-capable requests.
        bool bWaitAccepted = true;
    };

    // Runs the full transport-agnostic pipeline on one request: bearer-auth
    // gate, body-size gate, batch-array rejection, JSON-RPC envelope parse,
    // protocol methods (initialize / ping / tools/list / notifications) and
    // tools/call argument-shape routing (wiki root / wiki page / execute).
    //
    // AuthorizationHeader is the raw header VALUE (scheme + credentials).
    // Multiple occurrences of the header (proxies can fold duplicates) are
    // passed newline-joined; any matching value authorizes the request.
    //
    // Returns false when the transport must reject before dispatch: Out
    // carries the error body and HTTP status (401 unauthorized, 400 for
    // body/parse/envelope failures). On 401 the transport must additionally
    // send a `WWW-Authenticate: Bearer` header.
    // Returns true otherwise; branch on Out.Kind.
    PINWRIGHT_API bool ProcessRequestBody(const FString& Body,
                                          const FString& AuthorizationHeader,
                                          FRequestDecision& Out,
                                          const FRequestConfig& Config = FRequestConfig::FromSettings());

    // Wraps a dispatcher completion into the full JSON-RPC response object for
    // a tools/call request: MCP tool-result shape (success text+structured
    // copy, or isError:true with the report hint for internal failures),
    // oversized-result spill via HttpResponseSpill::MarkOversizedToolResult,
    // then the {jsonrpc,id,result} envelope. Always HTTP 200 — handler errors
    // travel in-band, never as JSON-RPC envelope errors.
    //
    // Method is the dotted RPC name of the call. On error results (bSuccess
    // false) with a non-empty Method, a doc reference - the on-disk wiki page
    // resolved by walking up the namespace with a root-index fallback - is
    // attached as a `Docs:` text line plus a top-level `docs: {page, wiki}`
    // field. An empty Method or a success result attaches no doc-ref, keeping
    // byte-identical legacy behavior.
    PINWRIGHT_API TSharedPtr<FJsonObject> WrapToolResult(bool bSuccess, const FString& Message,
                                                         const TSharedPtr<FJsonObject>& Result,
                                                         const FString& ErrorCode,
                                                         const TSharedPtr<FJsonValue>& EnvelopeId,
                                                         int32 SpillThreshold,
                                                         const FString& Method = FString());
}
