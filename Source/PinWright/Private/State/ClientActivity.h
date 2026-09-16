// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Who has been driving this editor, and how recently.
//
// One editor process is reachable by several agents at once: they share the
// loopback port and the project-wide bearer token, and nothing else in a request
// identifies its sender (the token is one shared secret, and RequestId is a GUID
// minted per request by the transport). So the editor could not tell its own
// caller's traffic from a stranger's - and `editor.quit`, the one verb that ends
// the process for EVERY client, had no liveness signal at all: it weighed unsaved
// packages and a live PIE session, then exited. An editor that had served
// actor.list, actor.describe and render.capture_asset_preview minutes earlier was
// shut down by a second agent that believed it was orphaned, killing the first
// agent's session mid-work.
//
// This ledger is the missing signal, kept as weak as it can be while still being
// impossible to fake into a false positive: an opaque client id the transport
// lifts off an optional request header, plus the wall clock of the last RPC
// dispatched under it. Recent traffic from a DIFFERENT id is evidence the editor
// is in use. Silence is evidence it is not - and silence is exactly what a
// genuinely abandoned editor produces forever, so a caller reaping abandoned
// editors still succeeds by waiting the window out.
//
// Only a dispatched RPC counts. `ping`, `initialize`, `tools/list` and wiki-page
// lookups are answered inside McpRequestCore and never reach the dispatcher, so
// an idle MCP session that is merely connected - or one reading documentation -
// leaves no trace here. That grain is load-bearing in both directions: an agent
// polling `ping` cannot pin an editor "in use" forever, and an agent that ran a
// real verb registers even if it then went quiet.
//
// A caller that sends no header is tracked as one shared anonymous bucket. That
// makes the guard inert between two anonymous callers, which is honest: they are
// genuinely indistinguishable here. The shipped client (Content/Python/mcp_proxy.py)
// sends a per-process id, so one MCP session never reads as another.
//
// Threading: NoteRequestClient runs on the socket I/O thread, because that is the
// last point at which the header is still in scope - the dispatch delegate carries
// only (RequestId, Method, Args). Everything else runs on the game thread. One
// mutex covers the whole ledger and every critical section is a few FString copies,
// so neither thread can be held up behind the other.
namespace ClientActivity
{
    // The optional request header carrying the caller id. Lowercase: the socket
    // transport lowercases every header key as it parses (SocketHttpServer.cpp).
    inline constexpr TCHAR ClientIdHeader[] = TEXT("x-pinwright-client");

    // Socket I/O thread, immediately before the request is handed to the
    // dispatcher. Parks the caller id against the request id the transport just
    // minted, for the game thread to consume one dispatch later. An empty id (no
    // header) parks nothing: absence IS the anonymous bucket.
    void NoteRequestClient(const FString& RequestId, const FString& ClientId);

    // Game thread, from FRpcDispatcher::ProcessRequest, next to the in-flight
    // stamp. Consumes the parked id, publishes it as this RPC's caller, and
    // stamps that client's last-seen time.
    void NoteDispatch(const FString& RequestId, const FString& Method);

    // The caller id of the RPC being served right now, empty when the caller sent
    // no header. Meaningful inside a handler body: the dispatcher's reentrancy
    // guard means at most one request is ever in flight, and the next dispatch
    // overwrites this rather than clearing it.
    FString GetCallerId();

    // One client's most recent RPC.
    struct FActivity
    {
        FString ClientId;
        FString Method;
        double SecondsAgo = 0.0;
    };

    // The most recent dispatch by a client whose id differs from CallerId.
    // False when no other client has ever been seen - which includes two
    // anonymous callers, since every anonymous caller is the same empty id.
    bool GetMostRecentOtherClient(const FString& CallerId, FActivity& Out);

    // Tests only. The ledger is a process-global.
    void ResetForTests();

    // Tests only. Stamps an activity at an absolute FPlatformTime::Seconds()
    // value so a test can model old traffic without sleeping.
    void NoteDispatchForTests(const FString& ClientId, const FString& Method, double AtSeconds);
}
