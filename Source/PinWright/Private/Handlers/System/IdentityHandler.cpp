// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "PinWrightSettings.h"
#include "PinWrightSubsystem.h"
#include "Transport/EditorIdentity.h"
#include "Dom/JsonObject.h"

// ---- system.identity ----
//
// The handshake half of the editor-identity fix. Publishing the identity is what makes an
// assertion possible: a caller reads this once from the editor it launched, then pins the fields
// it cares about on every later call by putting `_expect_editor` inside that call's `args` - the
// same place `_format` and `wait` live - which the dispatcher checks BEFORE any handler runs
// (RpcDispatcher::ProcessRequest).
//
// This verb takes no parameters. `_expect_editor` is not one of them - the dispatcher strips it
// before validation - so `system.identity` carrying an assertion both verifies the target and
// returns what actually answered, which is the one call worth making first.
REGISTER_RPC_HANDLER("system.identity", "system",
    "Identity of the editor PROCESS answering this call - pid, a per-boot instance GUID, project "
    "file, engine version, plugin build stamp, executable and command line - plus the port it is "
    "serving on. Two editors of the same project derive the SAME MCP port, so read this from the "
    "editor you launched and then pass the fields you pinned as `_expect_editor` on later calls; "
    "a mismatch is refused instead of silently executed on the other editor",
    RPC_NO_PARAMS)
{
    const PinWrightEditorIdentity::FIdentity& Identity = PinWrightEditorIdentity::Measured();
    TSharedRef<FJsonObject> Result = PinWrightEditorIdentity::ToJson(Identity);

    // The port this editor WANTED (a pure function of the project path, so every editor of this
    // project computes the same number) is reported separately from the port it actually holds.
    // They disagree in exactly the case this whole feature exists for.
    const int32 ConfiguredPort = UPinWrightSettings::ResolveHttpPort(GetDefault<UPinWrightSettings>());
    Result->SetNumberField(TEXT("configured_http_port"), ConfiguredPort);

    if (UPinWrightSubsystem* Subsystem = Ctx.GetSubsystem())
    {
        const int32 BoundPort = Subsystem->GetBoundHttpPort();
        const int32 ContestedPort = Subsystem->GetContestedHttpPort();
        const int32 BindAttempts = Subsystem->GetBindAttemptCount();

        Result->SetBoolField(TEXT("serving"), BoundPort != 0);
        if (BoundPort != 0)
        {
            // Measured, not assumed: omitted rather than reported as 0 when nothing is bound.
            Result->SetNumberField(TEXT("bound_http_port"), BoundPort);
        }
        if (ContestedPort != 0)
        {
            // A bind-failed editor answers no HTTP at all, so over the socket this branch is
            // unreachable by construction. It is here for in-process callers (Python, the editor
            // UI), which are the only ones that can ask the orphaned editor anything - and the
            // question they most need answered is "why is nobody reaching me".
            Result->SetNumberField(TEXT("contested_http_port"), ContestedPort);
            Result->SetNumberField(TEXT("bind_attempts"), BindAttempts);
            Result->SetBoolField(TEXT("bind_retry_exhausted"), Subsystem->IsBindRetryExhausted());
        }
        if (BoundPort != 0 && BoundPort != ConfiguredPort)
        {
            Result->SetStringField(TEXT("warning"), FString::Printf(
                TEXT("This editor is serving on port %d but the configured port is %d; the "
                     "settings changed after the bind. Callers resolving the endpoint from "
                     "settings will miss this process."),
                BoundPort, ConfiguredPort));
        }
    }

    Ctx.SendSuccess(Result);
    return true;
}
