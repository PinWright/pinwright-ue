// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Transport/McpRequestCore.h"
#include "Catalog/WikiDiskGenerator.h"
#include "Catalog/WikiHandler.h"
#include "Compat/JsonKeyCompat.h"
#include "JsonRpc.h"
#include "Utils/HttpResponseSpill.h"
#include "Utils/GatewayAuthToken.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightSettings.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{

// Wrap a successful tool result for MCP `tools/call`: text block + structured copy.
TSharedPtr<FJsonObject> MakeToolCallSuccess(const TSharedPtr<FJsonObject>& Result)
{
    TSharedPtr<FJsonObject> ResultObj = Result.IsValid() ? Result : MakeShared<FJsonObject>();

    const FString StructuredText = JsonRpc::Serialize(ResultObj);

    TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
    TextBlock->SetStringField(TEXT("type"), TEXT("text"));
    TextBlock->SetStringField(TEXT("text"), StructuredText);

    TArray<TSharedPtr<FJsonValue>> Content;
    Content.Add(MakeShared<FJsonValueObject>(TextBlock));

    TSharedPtr<FJsonObject> ToolResult = MakeShared<FJsonObject>();
    ToolResult->SetArrayField(TEXT("content"), Content);
    ToolResult->SetObjectField(TEXT("structuredContent"), ResultObj);
    ToolResult->SetBoolField(TEXT("isError"), false);
    return ToolResult;
}

// Wrap a text-only tool result (wiki render path — no structuredContent).
TSharedPtr<FJsonObject> MakeToolCallText(const FString& Text)
{
    TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
    TextBlock->SetStringField(TEXT("type"), TEXT("text"));
    TextBlock->SetStringField(TEXT("text"), Text);

    TArray<TSharedPtr<FJsonValue>> Content;
    Content.Add(MakeShared<FJsonValueObject>(TextBlock));

    TSharedPtr<FJsonObject> ToolResult = MakeShared<FJsonObject>();
    ToolResult->SetArrayField(TEXT("content"), Content);
    ToolResult->SetBoolField(TEXT("isError"), false);
    return ToolResult;
}

// Hint attached to every wiki-discovery reference: steer agents to read/grep the
// on-disk tree with filesystem tools instead of repeating call() doc requests.
const TCHAR* const GWikiDocHint =
    TEXT("Prefer filesystem tools (Read/Grep/Glob) on the wiki folder over call() doc requests; ")
    TEXT("every wiki page is on disk in the 'wiki' directory, flat (no subfolders): root index 'index.md', ")
    TEXT("namespace pages '<namespace>.md', method pages '<namespace.method>.md' (dotted filenames verbatim). ")
    TEXT("Use call(method, args) only to execute an RPC.");

// Accepted top-level field names for the `call` tool's tools/call arguments.
// 'path' is an alias for 'method'; any other field is rejected outright.
const TCHAR* const GCallArgFields[] = { TEXT("method"), TEXT("args"), TEXT("path") };

// Shared description of the valid `call` argument fields, reused across the
// unknown-field and missing-method rejections so the two never drift.
const TCHAR* const GCallValidFieldsText =
    TEXT("'method' (string, dotted RPC name; 'path' is accepted as an alias) and 'args' (object)");

// Approved MCP initialize text from mcp-instructions.md. Keep this template
// byte-identical with MCP_INSTRUCTIONS_TEMPLATE in Content/Python/mcp_proxy.py.
const TCHAR* const GMcpInitializeInstructionsTemplate =
    TEXT("PinWright generates and refreshes its on-disk wiki when the Unreal Editor starts. Read `{{PINWRIGHT_WIKI_DIRECTORY}}/index.md`, then use filesystem search and file-reading tools in `{{PINWRIGHT_WIKI_DIRECTORY}}/` as the default discovery workflow.\n")
    TEXT("\n")
    TEXT("The wiki is flat:\n")
    TEXT("\n")
    TEXT("- `index.md` is the root page.\n")
    TEXT("- `<namespace>.md` documents a namespace.\n")
    TEXT("- `<namespace.method>.md` documents a method.\n")
    TEXT("\n")
    TEXT("Invocation modes:\n")
    TEXT("\n")
    TEXT("- `call({method: \"<namespace-or-method>\"})` without `args` returns documentation, but direct filesystem search and file reading in the generated wiki are preferred over fetching documentation through MCP.\n")
    TEXT("- `call({method: \"<namespace.method>\", args: {...}})` executes the RPC. Methods with no parameters still require `args: {}` to execute.\n")
    TEXT("\n")
    TEXT("Read the exact method page before execution.\n");

FString BuildMcpInitializeInstructions()
{
    FString WikiDirectory = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir() / TEXT("PinWright") / TEXT("wiki"));
    FPaths::NormalizeDirectoryName(WikiDirectory);
    WikiDirectory.ReplaceInline(TEXT("\\"), TEXT("/"));
    while (WikiDirectory.EndsWith(TEXT("/")))
    {
        WikiDirectory.LeftChopInline(1);
    }

    FString Instructions = GMcpInitializeInstructionsTemplate;
    Instructions.ReplaceInline(TEXT("{{PINWRIGHT_WIKI_DIRECTORY}}"), *WikiDirectory);
    return Instructions;
}

// Wrap a wiki-discovery reference (on-disk path pointer — no rendered markdown).
// The text block carries the JSON-stringified reference; the same object rides
// as structuredContent.
TSharedPtr<FJsonObject> MakeWikiReference(const TSharedPtr<FJsonObject>& Reference)
{
    TSharedPtr<FJsonObject> ReferenceObj = Reference.IsValid() ? Reference : MakeShared<FJsonObject>();

    const FString ReferenceText = JsonRpc::Serialize(ReferenceObj);

    TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
    TextBlock->SetStringField(TEXT("type"), TEXT("text"));
    TextBlock->SetStringField(TEXT("text"), ReferenceText);
    TextBlock->SetStringField(TEXT("mimeType"), TEXT("application/json"));

    TArray<TSharedPtr<FJsonValue>> Content;
    Content.Add(MakeShared<FJsonValueObject>(TextBlock));

    TSharedPtr<FJsonObject> ToolResult = MakeShared<FJsonObject>();
    ToolResult->SetArrayField(TEXT("content"), Content);
    ToolResult->SetObjectField(TEXT("structuredContent"), ReferenceObj);
    ToolResult->SetBoolField(TEXT("isError"), false);
    return ToolResult;
}

// Resolve the on-disk wiki page documenting Method: exact page first, then
// parent namespaces by stripping trailing '.segment', finally the root index.
// Returns empty when nothing exists on disk (cold start / no wiki output).
FString ResolveDocPageForMethod(const FString& Method, FString& OutWikiDir)
{
    OutWikiDir = WikiDiskGenerator::OutputDirectory();
    if (OutWikiDir.IsEmpty() || Method.IsEmpty())
    {
        return FString();
    }

    FString Current = Method;
    for (;;)
    {
        const FString Path = WikiDiskGenerator::PagePath(WikiHandler::NormalizeSlug(Current));
        if (!Path.IsEmpty() && IFileManager::Get().FileExists(*Path))
        {
            return Path;
        }

        int32 DotIndex = INDEX_NONE;
        if (Current.FindLastChar(TEXT('.'), DotIndex))
        {
            Current = Current.Left(DotIndex);
            continue;
        }

        // No dot remains and the last namespace candidate missed: fall back to
        // the root index when it too is present on disk.
        const FString RootPath = WikiDiskGenerator::PagePath(WikiDiskGenerator::RootIndexSlug());
        if (!RootPath.IsEmpty() && IFileManager::Get().FileExists(*RootPath))
        {
            return RootPath;
        }
        return FString();
    }
}

// Pointer text appended to caller-facing tools/call errors: name the on-disk root
// index page when it exists, otherwise steer to the no-argument call() index.
FString RootIndexPointerText()
{
    const FString RootPath = WikiDiskGenerator::PagePath(WikiDiskGenerator::RootIndexSlug());
    if (!RootPath.IsEmpty() && IFileManager::Get().FileExists(*RootPath))
    {
        return FString::Printf(TEXT("Wiki root index: %s."), *RootPath);
    }
    return TEXT("Call with no arguments for the root namespace index.");
}

// Wrap an in-band tool-call failure. Per MCP spec these are NOT JSON-RPC errors —
// they ride as a successful JSON-RPC response with `isError: true` inside the result.
// A handler-attached error payload (3-arg SendError) is folded in: appended to the
// text block as serialized JSON and carried as structuredContent.
// ReportHint, when non-empty, is appended as a trailing paragraph and mirrored into a
// `report` structured field; callers pass it only for internal/unexpected failures.
// DocsPage, when non-empty, appends a trailing "Docs: <DocsPage>" line to the text and
// sets a top-level `docs` object {page: DocsPage, wiki: DocsWikiDir} on the result;
// structuredContent stays exclusively handler-owned and is never touched here.
TSharedPtr<FJsonObject> MakeToolCallError(const FString& Message,
                                          const TSharedPtr<FJsonObject>& Result = nullptr,
                                          const FString& ReportHint = FString(),
                                          const FString& DocsPage = FString(),
                                          const FString& DocsWikiDir = FString())
{
    FString Text = Message;

    TSharedPtr<FJsonObject> ToolResult = MakeShared<FJsonObject>();
    if (Result.IsValid())
    {
        Text += TEXT("\n");
        Text += JsonRpc::Serialize(Result);
        ToolResult->SetObjectField(TEXT("structuredContent"), Result);
    }

    if (!ReportHint.IsEmpty())
    {
        Text += TEXT("\n\n");
        Text += ReportHint;
        ToolResult->SetStringField(TEXT("report"), ReportHint);
    }

    if (!DocsPage.IsEmpty())
    {
        Text += TEXT("\nDocs: ");
        Text += DocsPage;
        // The oversize-spill rewrite (MarkOversizedToolResult) replaces only
        // content and structuredContent, so this top-level `docs` field survives
        // a spill even though the "Docs:" text line goes to the spill file.
        TSharedPtr<FJsonObject> Docs = MakeShared<FJsonObject>();
        Docs->SetStringField(TEXT("page"), DocsPage);
        Docs->SetStringField(TEXT("wiki"), DocsWikiDir);
        ToolResult->SetObjectField(TEXT("docs"), Docs);
    }

    TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
    TextBlock->SetStringField(TEXT("type"), TEXT("text"));
    TextBlock->SetStringField(TEXT("text"), Text);

    TArray<TSharedPtr<FJsonValue>> Content;
    Content.Add(MakeShared<FJsonValueObject>(TextBlock));

    ToolResult->SetArrayField(TEXT("content"), Content);
    ToolResult->SetBoolField(TEXT("isError"), true);
    return ToolResult;
}

// Single `call` tool descriptor returned by `tools/list`.
TSharedPtr<FJsonObject> BuildCallToolDescriptor()
{
    TSharedPtr<FJsonObject> MethodSchema = MakeShared<FJsonObject>();
    MethodSchema->SetStringField(TEXT("type"), TEXT("string"));
    MethodSchema->SetStringField(TEXT("description"),
        TEXT("Dotted RPC method name (e.g. 'asset.dump_folder'). Empty or omitted returns the root namespace index."));

    TSharedPtr<FJsonObject> ArgsSchema = MakeShared<FJsonObject>();
    ArgsSchema->SetStringField(TEXT("type"), TEXT("object"));
    ArgsSchema->SetStringField(TEXT("description"),
        TEXT("Arguments object for the method. Omit to fetch the wiki page instead of executing the method."));

    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
    Properties->SetObjectField(TEXT("method"), MethodSchema);
    Properties->SetObjectField(TEXT("args"), ArgsSchema);

    TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
    InputSchema->SetStringField(TEXT("type"), TEXT("object"));
    InputSchema->SetObjectField(TEXT("properties"), Properties);

    TSharedPtr<FJsonObject> Tool = MakeShared<FJsonObject>();
    Tool->SetStringField(TEXT("name"), TEXT("call"));
    Tool->SetStringField(TEXT("description"),
        TEXT("Invoke a PinWright RPC. Pass method='<namespace.verb>' and args={...} to execute; omit args to fetch the wiki page for the method; omit both to fetch the root namespace index. ")
        TEXT("'path' is accepted as an alias for 'method'; any other argument field is rejected."));
    Tool->SetObjectField(TEXT("inputSchema"), InputSchema);
    return Tool;
}

FString GetPluginVersionString()
{
    if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright")))
    {
        const FString& Ver = Plugin->GetDescriptor().VersionName;
        if (!Ver.IsEmpty())
        {
            return Ver;
        }
    }
    return TEXT("0.1.1");
}

TSharedPtr<FJsonObject> BuildInitializeResult()
{
    TSharedPtr<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
    ToolsCap->SetBoolField(TEXT("listChanged"), false);

    TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
    Capabilities->SetObjectField(TEXT("tools"), ToolsCap);

    TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
    ServerInfo->SetStringField(TEXT("name"), TEXT("PinWright"));
    ServerInfo->SetStringField(TEXT("version"), GetPluginVersionString());

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("protocolVersion"), TEXT("2025-06-18"));
    Result->SetObjectField(TEXT("capabilities"), Capabilities);
    Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
    Result->SetStringField(TEXT("instructions"), BuildMcpInitializeInstructions());
    return Result;
}

bool IsEditorOperational(const McpRequestCore::FRequestConfig& Config)
{
    return Config.bEditorReady && !Config.bEditorLoadingPackage &&
           Config.bEditorSelectionSetAvailable;
}

FString EditorNotReadyMessage(const McpRequestCore::FRequestConfig& Config)
{
    TArray<FString> Reasons;
    if (!Config.bEditorReady)
    {
        Reasons.Add(TEXT("editor startup has not completed"));
    }
    if (Config.bEditorLoadingPackage)
    {
        Reasons.Add(TEXT("packages are still loading"));
    }
    if (!Config.bEditorSelectionSetAvailable)
    {
        Reasons.Add(TEXT("the editor selection set is not initialized"));
    }

    const FString Detail = Reasons.Num() > 0
        ? FString::Join(Reasons, TEXT("; "))
        : TEXT("the editor has not reached operational readiness");
    return FString::Printf(
        TEXT("The Unreal editor is not ready for PinWright operations: %s. "
             "Retry after startup completes."),
        *Detail);
}

FString BlockedOnModalMessage(const McpRequestCore::FRequestConfig& Config)
{
    const FString TitleClause = Config.ModalTitle.IsEmpty()
        ? FString(TEXT("a modal dialog"))
        : FString::Printf(TEXT("a modal dialog titled \"%s\""), *Config.ModalTitle);
    return FString::Printf(
        TEXT("The Unreal editor is blocked on %s and has been for %.0f s. PinWright RPCs "
             "execute on the game thread, which the dialog owns, so no RPC can dismiss it "
             "and retrying will not help. A human must dismiss the dialog in the editor "
             "window, or the process must be killed. Relaunch with "
             "-AutoDeclinePackageRecovery to prevent the auto-save recovery prompt "
             "specifically; see call(\"unattended\")."),
        *TitleClause, Config.BlockedOnModalSeconds);
}

FString GameThreadStalledMessage(const McpRequestCore::FRequestConfig& Config)
{
    // Naming the in-flight verb is the difference between "something is wrong" and
    // "your python.execute is the thing that is wrong". When nothing is in flight
    // the stall is engine-internal, which is a different instruction to the agent:
    // wait, do not go looking for a PinWright bug.
    const FString CauseClause = Config.InFlightMethod.IsEmpty()
        ? FString(TEXT("No PinWright RPC is in flight, so the stall is engine-internal work "
                       "(asset compile, map load, package save) rather than a PinWright call."))
        : FString::Printf(
            TEXT("The game thread is inside PinWright RPC '%s' (request %s), which has been "
                 "running for %.0f s."),
            *Config.InFlightMethod, *Config.InFlightRequestId, Config.InFlightSeconds);

    return FString::Printf(
        TEXT("The Unreal editor's game thread has not completed a tick for %.0f s. Every "
             "PinWright RPC runs on that thread, so nothing can execute until it returns - "
             "this reply comes from the socket I/O thread, which is the only part still "
             "alive. %s PinWright cannot interrupt a wedged game thread, so this is a "
             "diagnosis, not a recovery: wait if the operation is expected to be this long, "
             "otherwise kill and restart the editor (unsaved work is lost). Retrying is "
             "harmless - the condition clears by itself if the thread drains."),
        Config.GameThreadStalledSeconds, *CauseClause);
}

TSharedPtr<FJsonObject> BuildPingResult(const McpRequestCore::FRequestConfig& Config)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    // A stalled game thread makes every field below it stale-but-positive, exactly
    // as a modal does - the readiness snapshot is written by the thread that is not
    // running. Reporting editorReady:true through a wedge is the defect this probe
    // exists to close, so the stall must fold into bReady.
    const bool bReady = IsEditorOperational(Config) && !Config.bBlockedOnModal
                        && !Config.bGameThreadStalled;
    Result->SetBoolField(TEXT("editorReady"), bReady);
    // A modal block is the one not-ready condition that polling cannot clear, so it
    // must break the retry loop rather than feed it. EDITOR_NOT_READY is documented
    // retryable and a well-behaved client polls it - which is exactly what burns a
    // 600 s watchdog while a human never sees the dialog.
    Result->SetBoolField(TEXT("retryable"), !bReady && !Config.bBlockedOnModal);
    Result->SetBoolField(TEXT("editorLoadingPackage"), Config.bEditorLoadingPackage);
    Result->SetBoolField(TEXT("editorSelectionSetAvailable"),
                         Config.bEditorSelectionSetAvailable);
    if (Config.bBlockedOnModal)
    {
        Result->SetStringField(TEXT("error"), ErrorCodes::ERR_EDITOR_BLOCKED_ON_MODAL);
        Result->SetStringField(TEXT("message"), BlockedOnModalMessage(Config));
        Result->SetBoolField(TEXT("blockedOnModal"), true);
        Result->SetNumberField(TEXT("blockedSeconds"), Config.BlockedOnModalSeconds);
        // Omitted rather than guessed when GetActiveModalWindow() returned null: a
        // modal cancelled under GIsRunningUnattendedScript leaves no window to name.
        if (!Config.ModalTitle.IsEmpty())
        {
            Result->SetStringField(TEXT("modalTitle"), Config.ModalTitle);
        }
    }
    else if (Config.bGameThreadStalled)
    {
        // Ranked below the modal branch on purpose. A modal stops the core ticker
        // too, so past 90 s both probes are true and both are correct - but the
        // modal is the more specific explanation of the same physical fact AND the
        // non-retryable one, so it must win. `retryable` needs no special case:
        // !bReady && !bBlockedOnModal is already true here and false there.
        Result->SetStringField(TEXT("error"), ErrorCodes::ERR_EDITOR_GAME_THREAD_STALLED);
        Result->SetStringField(TEXT("message"), GameThreadStalledMessage(Config));
        Result->SetBoolField(TEXT("gameThreadStalled"), true);
        Result->SetNumberField(TEXT("stalledSeconds"), Config.GameThreadStalledSeconds);
        // Omitted rather than emitted empty when the thread stalled outside any
        // dispatch - "" would read as an unnamed RPC instead of no RPC.
        if (!Config.InFlightMethod.IsEmpty())
        {
            Result->SetStringField(TEXT("inFlightMethod"), Config.InFlightMethod);
            Result->SetNumberField(TEXT("inFlightSeconds"), Config.InFlightSeconds);
            if (!Config.InFlightRequestId.IsEmpty())
            {
                Result->SetStringField(TEXT("inFlightRequestId"), Config.InFlightRequestId);
            }
        }
    }
    else if (!bReady)
    {
        Result->SetStringField(TEXT("error"), ErrorCodes::ERR_EDITOR_NOT_READY);
        Result->SetStringField(TEXT("message"), EditorNotReadyMessage(Config));
    }
    return Result;
}

TSharedPtr<FJsonObject> BuildEditorNotReadyToolResult(
    const McpRequestCore::FRequestConfig& Config)
{
    TSharedPtr<FJsonObject> Details = BuildPingResult(Config);
    if (Config.bBlockedOnModal)
    {
        return MakeToolCallError(
            FString::Printf(TEXT("[%s] %s"), ErrorCodes::ERR_EDITOR_BLOCKED_ON_MODAL,
                            *BlockedOnModalMessage(Config)),
            Details);
    }
    // A stall does NOT open this gate - reaching here means the readiness snapshot
    // was already negative on its own. But that snapshot is written by the thread
    // that stopped, so when both are true the stall is the honest explanation, and
    // the visible text must agree with the `error` field BuildPingResult just put
    // in Details. Same branch order as BuildPingResult, for exactly that reason.
    if (Config.bGameThreadStalled)
    {
        return MakeToolCallError(
            FString::Printf(TEXT("[%s] %s"), ErrorCodes::ERR_EDITOR_GAME_THREAD_STALLED,
                            *GameThreadStalledMessage(Config)),
            Details);
    }
    return MakeToolCallError(
        FString::Printf(TEXT("[%s] %s"), ErrorCodes::ERR_EDITOR_NOT_READY,
                        *EditorNotReadyMessage(Config)),
        Details);
}

// Fills the ImmediateResponse fields; returns Accepted so call sites can
// `return Immediate(...)` in one statement.
bool Immediate(McpRequestCore::FRequestDecision& Out, const TSharedPtr<FJsonObject>& Body,
               int32 HttpCode, bool bAccepted)
{
    Out.Kind = McpRequestCore::FRequestDecision::EKind::ImmediateResponse;
    Out.ImmediateBody = Body;
    Out.HttpCode = HttpCode;
    return bAccepted;
}

} // namespace

McpRequestCore::FRequestConfig McpRequestCore::FRequestConfig::FromSettings()
{
    FRequestConfig Config;
    const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();
    // Floor the configured body cap at 1 KiB so a misconfigured setting can't
    // reject every request.
    Config.MaxBodyBytes = FMath::Max(1024, Settings->HttpMaxRequestBodyBytes);
    if (Settings->bRequireAuthToken)
    {
        Config.AuthToken = GatewayAuthToken::GetOrCreateToken();
    }
    return Config;
}

bool McpRequestCore::ProcessRequestBody(const FString& Body,
                                        const FString& AuthorizationHeader,
                                        FRequestDecision& Out,
                                        const FRequestConfig& Config)
{
    Out = FRequestDecision();

    // Bearer-token auth gate. When a token is configured, every request must carry a
    // matching `Authorization: Bearer <token>` header. This runs FIRST — before
    // notification handling, body-size checks, and JSON parse — so an unauthenticated
    // request never reaches the completion registry or the timeout sweeper.
    if (!Config.AuthToken.IsEmpty())
    {
        bool bAuthorized = false;
        // Multiple header occurrences (proxies can fold duplicates) arrive
        // newline-joined; any matching value authorizes.
        TArray<FString> HeaderValues;
        AuthorizationHeader.ParseIntoArray(HeaderValues, TEXT("\n"), /*InCullEmpty=*/true);
        for (const FString& V : HeaderValues)
        {
            FString Scheme, Credentials;
            if (V.TrimStartAndEnd().Split(TEXT(" "), &Scheme, &Credentials)
                && Scheme.Equals(TEXT("Bearer"), ESearchCase::IgnoreCase)
                && GatewayAuthToken::ConstantTimeEquals(Credentials.TrimStartAndEnd(), Config.AuthToken))
            {
                bAuthorized = true;
                break;
            }
        }
        if (!bAuthorized)
        {
            auto ErrResp = JsonRpc::BuildErrorResponse(nullptr, JsonRpc::kInvalidRequest,
                FString::Printf(
                    TEXT("Unauthorized: missing or invalid bearer token. Send 'Authorization: Bearer <token>' ")
                    TEXT("with the token stored at %s, or re-run Install from the PinWright setup screen ")
                    TEXT("(Tools -> PinWright MCP Setup)."),
                    *GatewayAuthToken::GetTokenFilePath()));
            return Immediate(Out, ErrResp, 401, /*bAccepted=*/false);
        }
    }

    // Body validation
    const int32 BodyByteCount =
        Config.BodyBytes >= 0 ? Config.BodyBytes : FTCHARToUTF8(*Body).Length();
    if (BodyByteCount <= 0 || BodyByteCount > Config.MaxBodyBytes)
    {
        auto ErrResp = JsonRpc::BuildErrorResponse(nullptr, JsonRpc::kInvalidRequest,
            BodyByteCount <= 0 ? TEXT("Request body is required.") : TEXT("Request body is too large."));
        return Immediate(Out, ErrResp, 400, /*bAccepted=*/false);
    }

    // Batch arrays are explicitly unsupported in v1.
    const FString Trimmed = Body.TrimStartAndEnd();
    if (Trimmed.StartsWith(TEXT("[")))
    {
        auto ErrResp = JsonRpc::BuildErrorResponse(nullptr, JsonRpc::kInvalidRequest,
            TEXT("Batch requests not supported."));
        return Immediate(Out, ErrResp, 400, /*bAccepted=*/false);
    }

    // Parse JSON-RPC 2.0 envelope
    TSharedPtr<FJsonObject> RootObj;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
    if (!FJsonSerializer::Deserialize(Reader, RootObj) || !RootObj.IsValid())
    {
        auto ErrResp = JsonRpc::BuildErrorResponse(nullptr, JsonRpc::kParseError, TEXT("Failed to parse JSON."));
        return Immediate(Out, ErrResp, 400, /*bAccepted=*/false);
    }

    JsonRpc::FEnvelope Env;
    FString ParseErr;
    if (!JsonRpc::ParseEnvelope(RootObj, Env, ParseErr))
    {
        auto ErrResp = JsonRpc::BuildErrorResponse(nullptr, JsonRpc::kInvalidRequest, ParseErr);
        return Immediate(Out, ErrResp, 400, /*bAccepted=*/false);
    }

    Out.EnvelopeId = Env.Id;

    // Notifications never get a JSON-RPC response per spec; ack with HTTP 202
    // and an empty body (null ImmediateBody).
    if (Env.bIsNotification)
    {
        return Immediate(Out, nullptr, 202, /*bAccepted=*/true);
    }

    // Dispatch by protocol method
    if (Env.Method == TEXT("initialize"))
    {
        return Immediate(Out, JsonRpc::BuildResponse(Env.Id, BuildInitializeResult()), 200, true);
    }

    if (Env.Method == TEXT("ping"))
    {
        return Immediate(Out, JsonRpc::BuildResponse(Env.Id, BuildPingResult(Config)), 200, true);
    }

    if (Env.Method == TEXT("tools/list"))
    {
        TArray<TSharedPtr<FJsonValue>> Tools;
        Tools.Add(MakeShared<FJsonValueObject>(BuildCallToolDescriptor()));
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetArrayField(TEXT("tools"), Tools);
        return Immediate(Out, JsonRpc::BuildResponse(Env.Id, Result), 200, true);
    }

    if (Env.Method == TEXT("tools/call"))
    {
        // Public PinWright calls are unavailable until the editor has completed
        // startup, package loading is idle, and the selection set used by editor
        // tooling exists. This check intentionally precedes tool-name parsing,
        // wiki lookup, and dispatcher registration so no public operation can
        // touch editor state during the cold-start unsafe window.
        //
        // bBlockedOnModal is checked by the same gate (BuildPingResult folds it into
        // both `editorReady` and `retryable`): a request arriving while the game
        // thread is wedged fails in milliseconds with an actionable, NON-retryable
        // message instead of being handed to a dispatcher that cannot run it.
        // Requests already in flight when the modal opened stay stuck - nothing can
        // rescue those, because the completion-timeout sweep runs on the blocked
        // thread too.
        if (!IsEditorOperational(Config) || Config.bBlockedOnModal)
        {
            return Immediate(Out,
                JsonRpc::BuildResponse(Env.Id, BuildEditorNotReadyToolResult(Config)),
                200, true);
        }

        // Read params.name and params.arguments
        FString ToolName;
        Env.Params->TryGetStringField(TEXT("name"), ToolName);

        if (ToolName != TEXT("call"))
        {
            TSharedPtr<FJsonObject> ToolErr = MakeToolCallError(
                FString::Printf(TEXT("Unknown tool name: %s. The only supported tool is 'call'."), *ToolName));
            return Immediate(Out, JsonRpc::BuildResponse(Env.Id, ToolErr), 200, true);
        }

        // Extract arguments
        TSharedPtr<FJsonObject> Arguments;
        const TSharedPtr<FJsonValue>* ArgumentsValuePtr =
            Env.Params->Values.Find(EARGCompat::JsonFieldKey(FString(TEXT("arguments"))));
        const bool bHasArguments = ArgumentsValuePtr != nullptr;
        if (bHasArguments)
        {
            const TSharedPtr<FJsonValue>& ArgumentsValue = *ArgumentsValuePtr;
            if (!ArgumentsValue.IsValid() || ArgumentsValue->Type != EJson::Object)
            {
                auto ErrResp = JsonRpc::BuildErrorResponse(Env.Id, JsonRpc::kInvalidParams,
                    TEXT("tools/call argument 'arguments' must be an object when present."));
                return Immediate(Out, ErrResp, 200, true);
            }
            Arguments = ArgumentsValue->AsObject();
        }
        if (!Arguments.IsValid())
        {
            Arguments = MakeShared<FJsonObject>();
        }

        // Reject unknown top-level argument fields so a mistyped key (e.g. a stray
        // field, or 'path' misspelled) fails loudly with a pointer to the fix rather
        // than silently falling through to the wiki-root branch.
        {
            TArray<FString> UnknownFields;
            // Explicit TPair<FString,...>: on UE 5.8 Values keys are UE::TSharedString,
            // which lacks Equals(); the conversion-copy keeps FString semantics.
            for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : Arguments->Values)
            {
                bool bKnown = false;
                for (const TCHAR* const Field : GCallArgFields)
                {
                    if (Pair.Key.Equals(Field, ESearchCase::CaseSensitive))
                    {
                        bKnown = true;
                        break;
                    }
                }
                if (!bKnown)
                {
                    UnknownFields.Add(FString::Printf(TEXT("'%s'"), *Pair.Key));
                }
            }
            if (UnknownFields.Num() > 0)
            {
                auto ErrResp = JsonRpc::BuildErrorResponse(Env.Id, JsonRpc::kInvalidParams,
                    FString::Printf(
                        TEXT("Unknown tools/call argument field(s) for 'call': %s. Valid fields: %s. %s"),
                        *FString::Join(UnknownFields, TEXT(", ")),
                        GCallValidFieldsText,
                        *RootIndexPointerText()));
                return Immediate(Out, ErrResp, 200, true);
            }
        }

        // 'method' and 'path' must be strings when present.
        const TCHAR* const StringFields[] = { TEXT("method"), TEXT("path") };
        for (const TCHAR* const NameField : StringFields)
        {
            const TSharedPtr<FJsonValue> Val = Arguments->TryGetField(NameField);
            if (Val.IsValid() && Val->Type != EJson::String)
            {
                auto ErrResp = JsonRpc::BuildErrorResponse(Env.Id, JsonRpc::kInvalidParams,
                    FString::Printf(
                        TEXT("tools/call argument '%s' must be a string (dotted RPC method name)."),
                        NameField));
                return Immediate(Out, ErrResp, 200, true);
            }
        }

        // 'path' is an alias for 'method'; empty strings count as absent, so only a
        // non-empty value participates in the fold.
        FString MethodField, PathField;
        Arguments->TryGetStringField(TEXT("method"), MethodField);
        Arguments->TryGetStringField(TEXT("path"), PathField);

        FString MethodArg = MethodField;
        if (MethodField.IsEmpty() && !PathField.IsEmpty())
        {
            MethodArg = PathField;
        }
        else if (!MethodField.IsEmpty() && !PathField.IsEmpty() && !MethodField.Equals(PathField))
        {
            auto ErrResp = JsonRpc::BuildErrorResponse(Env.Id, JsonRpc::kInvalidParams,
                FString::Printf(
                    TEXT("Both 'method' and 'path' were provided with different values ('%s' vs '%s'); ")
                    TEXT("'path' is an alias for 'method' - pass one."),
                    *MethodField, *PathField));
            return Immediate(Out, ErrResp, 200, true);
        }

        const bool bHasMethod = !MethodArg.IsEmpty();
        const TSharedPtr<FJsonValue>* ArgsValuePtr =
            Arguments->Values.Find(EARGCompat::JsonFieldKey(FString(TEXT("args"))));
        const bool bHasArgsField = ArgsValuePtr != nullptr;
        TSharedPtr<FJsonObject> ArgsObj;
        if (bHasArgsField)
        {
            const TSharedPtr<FJsonValue>& ArgsValue = *ArgsValuePtr;
            if (!ArgsValue.IsValid() || ArgsValue->Type != EJson::Object)
            {
                auto ErrResp = JsonRpc::BuildErrorResponse(Env.Id, JsonRpc::kInvalidParams,
                    TEXT("tools/call argument 'args' must be an object when present."));
                return Immediate(Out, ErrResp, 200, true);
            }
            ArgsObj = ArgsValue->AsObject();
        }

        // Wiki root index: no method AND no args.
        if (!bHasMethod && !bHasArgsField)
        {
            // Prefer a reference to the on-disk wiki tree generated at editor launch.
            const FString Dir = WikiDiskGenerator::OutputDirectory();
            const FString RootPath = WikiDiskGenerator::PagePath(WikiDiskGenerator::RootIndexSlug());
            if (!Dir.IsEmpty() && !RootPath.IsEmpty() && IFileManager::Get().FileExists(*RootPath))
            {
                TSharedPtr<FJsonObject> Reference = MakeShared<FJsonObject>();
                Reference->SetStringField(TEXT("wiki"), Dir);
                Reference->SetStringField(TEXT("root"), RootPath);
                Reference->SetStringField(TEXT("hint"), GWikiDocHint);
                TSharedPtr<FJsonObject> ToolResult = MakeWikiReference(Reference);
                return Immediate(Out, JsonRpc::BuildResponse(Env.Id, ToolResult), 200, true);
            }

            // Cold-start fallback: disk tree not yet present — render live.
            FString Markdown;
            if (!WikiHandler::RenderPage(TEXT(""), Markdown))
            {
                auto ErrResp = JsonRpc::BuildErrorResponse(Env.Id, JsonRpc::kInternalError, TEXT("Wiki renderer unavailable."));
                return Immediate(Out, ErrResp, 200, true);
            }
            TSharedPtr<FJsonObject> ToolResult = MakeToolCallText(Markdown);
            return Immediate(Out, JsonRpc::BuildResponse(Env.Id, ToolResult), 200, true);
        }

        // Wiki method-page: method present, args absent.
        if (bHasMethod && !bHasArgsField)
        {
            // Prefer a reference to the on-disk page generated at editor launch.
            const FString Slug = WikiHandler::NormalizeSlug(MethodArg);
            const FString PagePath = WikiDiskGenerator::PagePath(Slug);
            if (!PagePath.IsEmpty() && IFileManager::Get().FileExists(*PagePath))
            {
                TSharedPtr<FJsonObject> Reference = MakeShared<FJsonObject>();
                Reference->SetStringField(TEXT("page"), PagePath);
                Reference->SetStringField(TEXT("wiki"), WikiDiskGenerator::OutputDirectory());
                Reference->SetStringField(TEXT("hint"), GWikiDocHint);
                TSharedPtr<FJsonObject> ToolResult = MakeWikiReference(Reference);
                return Immediate(Out, JsonRpc::BuildResponse(Env.Id, ToolResult), 200, true);
            }

            // Cold-start / unknown-slug fallback: no on-disk page — render live.
            FString Markdown;
            if (!WikiHandler::RenderPage(MethodArg, Markdown))
            {
                auto ErrResp = JsonRpc::BuildErrorResponse(Env.Id, JsonRpc::kInternalError, TEXT("Wiki renderer unavailable."));
                return Immediate(Out, ErrResp, 200, true);
            }
            TSharedPtr<FJsonObject> ToolResult = MakeToolCallText(Markdown);
            return Immediate(Out, JsonRpc::BuildResponse(Env.Id, ToolResult), 200, true);
        }

        // No method but args present is nonsensical.
        if (!bHasMethod && bHasArgsField)
        {
            auto ErrResp = JsonRpc::BuildErrorResponse(Env.Id, JsonRpc::kInvalidParams,
                FString::Printf(
                    TEXT("tools/call to 'call' got 'args' without a 'method'. Valid fields: %s. ")
                    TEXT("Omit 'args' to fetch a wiki page; omit both for the root namespace index. %s"),
                    GCallValidFieldsText, *RootIndexPointerText()));
            return Immediate(Out, ErrResp, 200, true);
        }

        // Execution path: the transport registers a completion and hands
        // Method/Args to FRpcDispatcher.
        if (!ArgsObj.IsValid())
        {
            ArgsObj = MakeShared<FJsonObject>();
        }

        Out.Kind = FRequestDecision::EKind::DispatchRpc;
        Out.Method = MethodArg;
        Out.Args = ArgsObj;

        // Streaming inputs for SSE-capable transports: MCP progress token from
        // params._meta.progressToken, and the tool-level wait flag.
        const TSharedPtr<FJsonObject>* MetaObj = nullptr;
        if (Env.Params->TryGetObjectField(TEXT("_meta"), MetaObj) && MetaObj != nullptr)
        {
            Out.ProgressToken = (*MetaObj)->TryGetField(TEXT("progressToken"));
            Out.bHasProgressToken = Out.ProgressToken.IsValid();
        }
        // Block-and-stream is the default: only an explicit boolean
        // args.wait == false declines it (absent or non-boolean keep true).
        const TSharedPtr<FJsonValue> WaitVal = ArgsObj->TryGetField(TEXT("wait"));
        if (WaitVal.IsValid() && WaitVal->Type == EJson::Boolean && !WaitVal->AsBool())
        {
            Out.bWaitAccepted = false;
        }
        return true;
    }

    // Unknown protocol method.
    auto ErrResp = JsonRpc::BuildErrorResponse(Env.Id, JsonRpc::kMethodNotFound,
        FString::Printf(TEXT("Method not found: %s"), *Env.Method));
    return Immediate(Out, ErrResp, 200, true);
}

TSharedPtr<FJsonObject> McpRequestCore::WrapToolResult(bool bSuccess, const FString& Message,
                                                       const TSharedPtr<FJsonObject>& Result,
                                                       const FString& ErrorCode,
                                                       const TSharedPtr<FJsonValue>& EnvelopeId,
                                                       int32 SpillThreshold,
                                                       const FString& Method)
{
    TSharedPtr<FJsonObject> ToolResult;
    if (bSuccess)
    {
        ToolResult = MakeToolCallSuccess(Result);
    }
    else
    {
        // Only nudge a bug report for internal/unexpected failures —
        // never for caller-side param/validation errors, so the agent
        // isn't steered to file issues on its own mistakes.
        const bool bInternalError =
            ErrorCode.IsEmpty()
            || ErrorCode == TEXT("INTERNAL")
            || ErrorCode == ErrorCodes::ERR_AUTOMATION_ERROR;
        const FString ReportHint = bInternalError
            ? FString(TEXT("If this looks like a plugin bug, you can report it: call(\"support\")"))
            : FString();
        // Attach an on-disk doc reference for the failing method: exact page, then
        // parent namespaces, then the root index. A success result never gets one.
        FString DocsWikiDir;
        const FString DocsPage = Method.IsEmpty()
            ? FString()
            : ResolveDocPageForMethod(Method, DocsWikiDir);
        ToolResult = MakeToolCallError(
            FString::Printf(TEXT("[%s] %s"),
                ErrorCode.IsEmpty() ? TEXT("INTERNAL") : *ErrorCode,
                *Message),
            Result,
            ReportHint,
            DocsPage,
            DocsWikiDir);
    }
    // MarkOversizedToolResult is the authoritative (and only) spill for
    // the MCP tools/call path: it measures the wrapped ToolResult and, on
    // overflow, rewrites it in place to a valid MCP shape (content[0].text
    // notice + structuredContent {outputTooLong,...}), keeping content[]/
    // isError. Error payloads are handler-controlled and unbounded too, so
    // mark both branches here. The envelope-level spill is intentionally
    // NOT applied to tools/call: it would clobber a valid result in the
    // dead band where the ToolResult is under threshold but the larger
    // {jsonrpc,id,result} envelope is over it — see the test header in
    // TestGetComponentsLargePayload.cpp for the dead-band rationale.
    if (ToolResult.IsValid())
    {
        HttpResponseSpill::MarkOversizedToolResult(
            ToolResult.ToSharedRef(),
            SpillThreshold);
    }
    // Handler-level errors travel as in-band tool-call results, not JSON-RPC errors.
    return JsonRpc::BuildResponse(EnvelopeId, ToolResult);
}
