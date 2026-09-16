// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

// Identity of the editor PROCESS that answers an RPC, and the caller-side assertion that pins it.
//
// THE DEFECT THIS EXISTS FOR. UPinWrightSettings::DerivePortFromPath
// (PinWrightSettings.cpp:80) hashes the PROJECT DIRECTORY, so the MCP port is a function of the
// project, not of the editor instance: two editors opened on the same project derive the SAME
// port. Exactly one of them wins the bind - FSocketHttpServer::Start binds without
// SO_REUSEADDR on purpose - and every RPC on that port goes to that one. Nothing in a response
// named the process that ran it, so a client that launched its own editor could be driving a
// different, already-running editor with no error and no way to tell. Measured consequence on
// the board ticket: a misrouted blueprint.set_default reinstanced live drones in the user's
// interactive editor and crashed it.
//
// EVERYTHING HERE IS MEASURED FROM THE RUNNING PROCESS. Nothing is echoed back from settings,
// from the .uplugin, or from a launch argument, because a second editor started from the same
// shortcut carries identical configuration and would answer an assertion identically - which is
// precisely the case this is supposed to catch. That rules out FApp::GetInstanceId() as the
// instance token as well: it is a fresh GUID per process (App.cpp:31) UNTIL `-InstanceId=` is
// passed on the command line (App.cpp:182), at which point two editors launched from the same
// script share it. The GUID below is minted in-process and cannot be supplied from outside.
namespace PinWrightEditorIdentity
{
    // One measurement of this process. Every field is stable for the process lifetime, which is
    // why Measured() may cache it.
    struct FIdentity
    {
        // Unforgeable and externally observable (Task Manager, the editor's own log banner), so
        // it is the field a caller can pin without a prior handshake.
        uint32 ProcessId = 0;
        // Per-boot GUID minted in this process. Distinguishes two editors that share a pid
        // namespace across a restart - a recycled pid answers with a different InstanceId.
        FString InstanceId;
        // Absolute path of the loaded .uproject. Two editors of the SAME project agree here;
        // that is the point - it separates a same-project misroute from a wrong-project one.
        FString ProjectFilePath;
        FString ProjectName;
        FString EngineVersion;
        // Compile stamp of this translation unit. Separates an editor running a freshly built
        // plugin binary from one still holding an older DLL, which no other field here can.
        // Under a Unity build this is the date the containing blob was compiled, so treat it as
        // "not older than", not as an exact build time.
        FString PluginBuild;
        FString ExecutablePath;
        FString CommandLine;
    };

    // Measured once on first call and cached. Safe from any thread after that first call.
    const FIdentity& Measured();

    // Full identity as the wire object. Field names match the assertion vocabulary below, so a
    // caller can copy a subset of a response straight back into an assertion.
    TSharedRef<FJsonObject> ToJson(const FIdentity& Identity);

    // The reserved request field a caller states its expectation in. It travels INSIDE the call's
    // `args` object, beside `_format` and `wait`, and is stripped from the payload before handler
    // validation, so it is invisible to every handler's param spec. It cannot be a sibling of
    // `args`: McpRequestCore rejects any tools/call argument other than method/args/path.
    const FString& AssertionParamName();

    // The field names an assertion may carry. An assertion naming anything else is REFUSED
    // rather than partially applied: a typo in an assertion that silently passes is worse than
    // no assertion at all, because the caller then believes it verified the target.
    const TArray<FString>& AssertableFields();

    struct FAssertionVerdict
    {
        bool bAccepted = true;
        // Empty when accepted.
        FString ErrorCode;
        FString Message;
        // The asserted fields as the caller sent them, and this process's measured counterparts
        // for exactly those fields. Both sides are published so a refusal names what was
        // expected AND what answered; null when accepted or when the assertion was malformed
        // before any comparison could be made.
        TSharedPtr<FJsonObject> Expected;
        TSharedPtr<FJsonObject> Actual;
    };

    // Compares one assertion object against a measured identity. Pure: no engine state, no
    // globals, so a test can drive it with a synthetic FIdentity.
    FAssertionVerdict CheckAssertion(const FIdentity& Actual,
                                     const TSharedPtr<FJsonValue>& Assertion);

    // Pulls AssertionParamName() out of a request payload and checks it against Measured().
    // A payload that does not carry the field is ACCEPTED - asserting is opt-in, so every
    // existing caller keeps working unchanged.
    FAssertionVerdict CheckRequestAssertion(const TSharedPtr<FJsonObject>& Params);
}
