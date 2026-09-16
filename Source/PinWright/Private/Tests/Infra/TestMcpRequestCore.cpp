// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for McpRequestCore: transport-agnostic JSON-RPC 2.0 / MCP protocol
// handling shared by every HTTP transport behind POST /mcp.
//
// Ported from the deleted FMcpTransport suite (TestMcpTransport.cpp): instead of
// standing up a live IHttpRouter listener and issuing loopback POSTs, each test
// feeds the same JSON body straight into McpRequestCore::ProcessRequestBody with
// an inline FRequestConfig (explicit auth token / body cap — no settings or
// global mutation) and asserts on the FRequestDecision: ImmediateBody + HttpCode
// for protocol-level responses, or the DispatchRpc fields plus WrapToolResult
// output for the tools/call execution path. Wire-level concerns (headers,
// status-line emission, keep-alive, listener lifecycle) are covered by the
// socket transport suite in Tests/Transport/.
#include "Misc/AutomationTest.h"
#include "Transport/McpRequestCore.h"
#include "Catalog/WikiHandler.h"
#include "Catalog/WikiDiskGenerator.h"
#include "JsonRpc.h"
#include "Utils/HttpResponseSpill.h"
#include "Tests/TestSkipReporting.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named namespace (not anonymous): the plugin's tests share a single module with
// Unity builds, and same-name anonymous-namespace helpers across .cpp files
// produce ODR / redefinition errors when Unity merges them into one TU.
namespace McpRequestCoreTest
{
    // Config with auth disabled and an ample body cap; tests override fields inline.
    inline McpRequestCore::FRequestConfig MakeConfig(
        const FString& AuthToken = FString(), int32 MaxBodyBytes = 1024 * 1024)
    {
        McpRequestCore::FRequestConfig Config;
        Config.AuthToken = AuthToken;
        Config.MaxBodyBytes = MaxBodyBytes;
        Config.bEditorReady = true;
        Config.bEditorLoadingPackage = false;
        Config.bEditorSelectionSetAvailable = true;
        return Config;
    }

    // Wrapping threshold high enough that no test result ever spills.
    constexpr int32 NoSpillThreshold = 1024 * 1024;

    // Build a JSON-RPC 2.0 envelope with a string method and optional params.
    inline FString BuildEnvelope(int32 Id, const FString& Method, const TSharedPtr<FJsonObject>& Params)
    {
        TSharedPtr<FJsonObject> Env = MakeShared<FJsonObject>();
        Env->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
        Env->SetNumberField(TEXT("id"), Id);
        Env->SetStringField(TEXT("method"), Method);
        if (Params.IsValid())
        {
            Env->SetObjectField(TEXT("params"), Params);
        }
        return JsonRpc::Serialize(Env);
    }

    // Build a notification envelope (no id).
    inline FString BuildNotification(const FString& Method)
    {
        TSharedPtr<FJsonObject> Env = MakeShared<FJsonObject>();
        Env->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
        Env->SetStringField(TEXT("method"), Method);
        return JsonRpc::Serialize(Env);
    }

    // Build a tools/call envelope targeting the 'call' tool with the given arguments object.
    inline FString BuildToolsCallEnvelope(int32 Id, const TSharedPtr<FJsonObject>& Arguments,
                                          const FString& ToolName = TEXT("call"))
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), ToolName);
        if (Arguments.IsValid())
        {
            Params->SetObjectField(TEXT("arguments"), Arguments);
        }
        return BuildEnvelope(Id, TEXT("tools/call"), Params);
    }

    // Build a tools/call envelope whose outer arguments value can deliberately
    // use a non-object JSON shape.
    inline FString BuildToolsCallEnvelopeValue(int32 Id, const TSharedPtr<FJsonValue>& Arguments,
                                               const FString& ToolName = TEXT("call"))
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), ToolName);
        Params->SetField(TEXT("arguments"), Arguments);
        return BuildEnvelope(Id, TEXT("tools/call"), Params);
    }

    // Pull `result` as an object from a successful JSON-RPC envelope.
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

    // Pull `error` as an object from a JSON-RPC error envelope.
    inline TSharedPtr<FJsonObject> ErrorObject(const TSharedPtr<FJsonObject>& Envelope)
    {
        if (!Envelope.IsValid()) return nullptr;
        const TSharedPtr<FJsonObject>* ErrPtr = nullptr;
        if (Envelope->TryGetObjectField(TEXT("error"), ErrPtr) && ErrPtr)
        {
            return *ErrPtr;
        }
        return nullptr;
    }

    // Fixed 64-hex token used by the auth tests below.
    inline const TCHAR* const TestAuthToken =
        TEXT("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
}

// ============================================================================
// initialize — returns protocolVersion/capabilities/serverInfo/instructions.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreInitializeTest,
    "PinWright.infra.request_core.Initialize",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreInitializeTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildEnvelope(1, TEXT("initialize"), nullptr), FString(), Out, MakeConfig()));

    TestTrue(TEXT("Immediate response"),
        Out.Kind == McpRequestCore::FRequestDecision::EKind::ImmediateResponse);
    TestEqual(TEXT("HTTP 200"), Out.HttpCode, 200);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TestEqual(TEXT("jsonrpc is 2.0"),
        Out.ImmediateBody->GetStringField(TEXT("jsonrpc")), FString(TEXT("2.0")));

    TSharedPtr<FJsonObject> Result = ResultObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("result object present"), Result.IsValid())) return true;

    TestEqual(TEXT("protocolVersion is 2025-06-18"),
        Result->GetStringField(TEXT("protocolVersion")), FString(TEXT("2025-06-18")));

    FString WikiDirectory = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir() / TEXT("PinWright") / TEXT("wiki"));
    FPaths::NormalizeDirectoryName(WikiDirectory);
    WikiDirectory.ReplaceInline(TEXT("\\"), TEXT("/"));
    while (WikiDirectory.EndsWith(TEXT("/")))
    {
        WikiDirectory.LeftChopInline(1);
    }

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!TestTrue(TEXT("PinWright plugin is available"), Plugin.IsValid())) return true;

    const FString CanonicalPath = Plugin->GetBaseDir() / TEXT("mcp-instructions.md");
    FString ExpectedInstructions;
    if (!TestTrue(TEXT("canonical instructions source loads"),
            FFileHelper::LoadFileToString(ExpectedInstructions, *CanonicalPath))) return true;
    ExpectedInstructions.ReplaceInline(
        TEXT("{{PINWRIGHT_WIKI_DIRECTORY}}"), *WikiDirectory);
    FString Instructions;
    TestTrue(TEXT("instructions present"),
        Result->TryGetStringField(TEXT("instructions"), Instructions));
    TestEqual(TEXT("instructions exactly match the approved rendered message"),
        Instructions, ExpectedInstructions);
    TestFalse(TEXT("wiki directory is absolute"), FPaths::IsRelative(WikiDirectory));
    TestFalse(TEXT("wiki directory uses forward slashes"), WikiDirectory.Contains(TEXT("\\")));
    TestFalse(TEXT("wiki directory has no trailing slash"), WikiDirectory.EndsWith(TEXT("/")));
    TestTrue(TEXT("instructions name flat method-page layout"),
        Instructions.Contains(TEXT("<namespace.method>.md")));
    TestTrue(TEXT("instructions prefer direct filesystem discovery"),
        Instructions.Contains(TEXT("direct filesystem search and file reading")));
    TestTrue(TEXT("instructions distinguish documentation calls"),
        Instructions.Contains(TEXT("without `args` returns documentation")));
    TestTrue(TEXT("instructions distinguish execution calls"),
        Instructions.Contains(TEXT("args: {...}})` executes the RPC")));
    TestTrue(TEXT("instructions require explicit empty args"),
        Instructions.Contains(TEXT("still require `args: {}` to execute")));

    const TSharedPtr<FJsonObject>* CapsPtr = nullptr;
    TestTrue(TEXT("capabilities present"),
        Result->TryGetObjectField(TEXT("capabilities"), CapsPtr) && CapsPtr);
    if (CapsPtr)
    {
        const TSharedPtr<FJsonObject>* ToolsPtr = nullptr;
        TestTrue(TEXT("capabilities.tools present"),
            (*CapsPtr)->TryGetObjectField(TEXT("tools"), ToolsPtr) && ToolsPtr);
        if (ToolsPtr)
        {
            bool bListChanged = true;
            (*ToolsPtr)->TryGetBoolField(TEXT("listChanged"), bListChanged);
            TestFalse(TEXT("listChanged is false"), bListChanged);
        }
    }

    const TSharedPtr<FJsonObject>* InfoPtr = nullptr;
    if (TestTrue(TEXT("serverInfo present"),
            Result->TryGetObjectField(TEXT("serverInfo"), InfoPtr) && InfoPtr))
    {
        TestEqual(TEXT("serverInfo.name"),
            (*InfoPtr)->GetStringField(TEXT("name")),
            FString(TEXT("PinWright")));
        FString Version;
        TestTrue(TEXT("serverInfo.version is a non-empty string"),
            (*InfoPtr)->TryGetStringField(TEXT("version"), Version) && !Version.IsEmpty());
    }

    return true;
}

// ============================================================================
// notifications/initialized — HTTP 202 with a null body: no JSON-RPC response.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreNotificationsInitializedTest,
    "PinWright.infra.request_core.NotificationsInitialized",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreNotificationsInitializedTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildNotification(TEXT("notifications/initialized")), FString(), Out, MakeConfig()));

    TestTrue(TEXT("Immediate response"),
        Out.Kind == McpRequestCore::FRequestDecision::EKind::ImmediateResponse);
    TestEqual(TEXT("HTTP 202 Accepted"), Out.HttpCode, 202);
    TestFalse(TEXT("No JSON-RPC response body (empty HTTP body per spec)"),
        Out.ImmediateBody.IsValid());

    return true;
}

// ============================================================================
// ping — returns the bootstrap liveness + editorReady state with the id echoed.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCorePingTest,
    "PinWright.infra.request_core.Ping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCorePingTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildEnvelope(2, TEXT("ping"), nullptr), FString(), Out, MakeConfig()));

    TestEqual(TEXT("HTTP 200"), Out.HttpCode, 200);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TSharedPtr<FJsonObject> Result = ResultObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("result is an object"), Result.IsValid())) return true;
    bool bEditorReady = false;
    TestTrue(TEXT("ping reports editorReady"),
        Result->TryGetBoolField(TEXT("editorReady"), bEditorReady));
    TestTrue(TEXT("default test config is operational"), bEditorReady);
    bool bRetryable = true;
    TestTrue(TEXT("ping reports retryable"),
        Result->TryGetBoolField(TEXT("retryable"), bRetryable));
    TestFalse(TEXT("ready ping is not retryable"), bRetryable);

    int32 EchoedId = 0;
    TestTrue(TEXT("id echoed as a number"),
        Out.ImmediateBody->TryGetNumberField(TEXT("id"), EchoedId));
    TestEqual(TEXT("id echoed matches request"), EchoedId, 2);

    return true;
}

// ============================================================================
// tools/call — every public operation is blocked before wiki lookup or handler
// dispatch while the editor is still in the cold-start unsafe state.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreEditorNotReadyGateTest,
    "PinWright.infra.request_core.ToolsCall.EditorNotReadyGate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreEditorNotReadyGateTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    // A caller that forgets to publish a readiness snapshot must fail closed.
    McpRequestCore::FRequestConfig Config;

    TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
    Arguments->SetStringField(TEXT("method"), TEXT("system.status"));
    Arguments->SetObjectField(TEXT("args"), MakeShared<FJsonObject>());

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("request accepted as in-band tool error"),
        McpRequestCore::ProcessRequestBody(
            BuildToolsCallEnvelope(20, Arguments), FString(), Out, Config));
    TestEqual(TEXT("gate resolves immediately"),
        Out.Kind, McpRequestCore::FRequestDecision::EKind::ImmediateResponse);
    TestEqual(TEXT("gate keeps MCP HTTP 200"), Out.HttpCode, 200);

    TSharedPtr<FJsonObject> ToolResult = ResultObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("tool result present"), ToolResult.IsValid()))
    {
        return true;
    }
    bool bIsError = false;
    TestTrue(TEXT("tool result isError is true"),
        ToolResult->TryGetBoolField(TEXT("isError"), bIsError) && bIsError);
    const TSharedPtr<FJsonObject>* Details = nullptr;
    if (TestTrue(TEXT("structured readiness details present"),
            ToolResult->TryGetObjectField(TEXT("structuredContent"), Details) &&
            Details && (*Details).IsValid()))
    {
        TestEqual(TEXT("error code is EDITOR_NOT_READY"),
            (*Details)->GetStringField(TEXT("error")),
            FString(TEXT("EDITOR_NOT_READY")));
        bool bRetryable = false;
        TestTrue(TEXT("readiness error is retryable"),
            (*Details)->TryGetBoolField(TEXT("retryable"), bRetryable) && bRetryable);
        bool bLoading = false;
        TestTrue(TEXT("loading-package state is reported"),
            (*Details)->TryGetBoolField(TEXT("editorLoadingPackage"), bLoading) && bLoading);
        bool bSelectionSet = true;
        TestTrue(TEXT("selection-set state is reported"),
            (*Details)->TryGetBoolField(TEXT("editorSelectionSetAvailable"), bSelectionSet) &&
            !bSelectionSet);
    }

    return true;
}

// ============================================================================
// tools/list — exactly one tool named 'call' with the documented schema.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsListTest,
    "PinWright.infra.request_core.ToolsList",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsListTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildEnvelope(3, TEXT("tools/list"), nullptr), FString(), Out, MakeConfig()));

    TestEqual(TEXT("HTTP 200"), Out.HttpCode, 200);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TSharedPtr<FJsonObject> Result = ResultObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("result present"), Result.IsValid())) return true;

    const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
    if (!TestTrue(TEXT("tools array present"),
            Result->TryGetArrayField(TEXT("tools"), Tools) && Tools)) return true;

    TestEqual(TEXT("exactly one tool"), Tools->Num(), 1);
    if (Tools->Num() < 1) return true;

    const TSharedPtr<FJsonObject> Tool = (*Tools)[0]->AsObject();
    if (!TestTrue(TEXT("tool descriptor is an object"), Tool.IsValid())) return true;

    TestEqual(TEXT("tool.name is 'call'"),
        Tool->GetStringField(TEXT("name")), FString(TEXT("call")));

    FString Desc;
    TestTrue(TEXT("description is a non-empty string"),
        Tool->TryGetStringField(TEXT("description"), Desc) && !Desc.IsEmpty());

    const TSharedPtr<FJsonObject>* Schema = nullptr;
    if (!TestTrue(TEXT("inputSchema present"),
            Tool->TryGetObjectField(TEXT("inputSchema"), Schema) && Schema)) return true;

    TestEqual(TEXT("inputSchema.type is object"),
        (*Schema)->GetStringField(TEXT("type")), FString(TEXT("object")));

    const TSharedPtr<FJsonObject>* Props = nullptr;
    if (!TestTrue(TEXT("inputSchema.properties present"),
            (*Schema)->TryGetObjectField(TEXT("properties"), Props) && Props)) return true;

    const TSharedPtr<FJsonObject>* MethodProp = nullptr;
    if (TestTrue(TEXT("properties.method present"),
            (*Props)->TryGetObjectField(TEXT("method"), MethodProp) && MethodProp))
    {
        TestEqual(TEXT("properties.method.type is string"),
            (*MethodProp)->GetStringField(TEXT("type")), FString(TEXT("string")));
    }

    const TSharedPtr<FJsonObject>* ArgsProp = nullptr;
    if (TestTrue(TEXT("properties.args present"),
            (*Props)->TryGetObjectField(TEXT("args"), ArgsProp) && ArgsProp))
    {
        TestEqual(TEXT("properties.args.type is object"),
            (*ArgsProp)->GetStringField(TEXT("type")), FString(TEXT("object")));
    }

    return true;
}

// ============================================================================
// tools/call — execution path. ProcessRequestBody yields DispatchRpc with the
// dotted method + args; wrapping a successful handler completion via
// WrapToolResult produces content[0].text == JSON(result), structuredContent
// == result, isError == false.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallExecutionTest,
    "PinWright.infra.request_core.ToolsCall.Execution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallExecutionTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    // arguments = { method: "any.method", args: {} } drives the execution path.
    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("method"), TEXT("any.method"));
    Args->SetObjectField(TEXT("args"), MakeShared<FJsonObject>());

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildToolsCallEnvelope(4, Args), FString(), Out, MakeConfig()));

    if (!TestTrue(TEXT("Execution path selected (DispatchRpc)"),
            Out.Kind == McpRequestCore::FRequestDecision::EKind::DispatchRpc)) return true;
    TestEqual(TEXT("Dispatch method is the dotted RPC name"),
        Out.Method, FString(TEXT("any.method")));
    TestTrue(TEXT("Dispatch args object present"), Out.Args.IsValid());
    TestTrue(TEXT("Envelope id captured for the completion"), Out.EnvelopeId.IsValid());

    // Canned handler completion, wrapped the way a transport would.
    TSharedPtr<FJsonObject> CannedResult = MakeShared<FJsonObject>();
    CannedResult->SetStringField(TEXT("echo"), TEXT("hello"));
    CannedResult->SetNumberField(TEXT("count"), 42);

    TSharedPtr<FJsonObject> Response = McpRequestCore::WrapToolResult(
        /*bSuccess=*/true, TEXT("OK"), CannedResult, TEXT(""), Out.EnvelopeId, NoSpillThreshold);
    if (!TestTrue(TEXT("Wrapped response present"), Response.IsValid())) return true;

    int32 EchoedId = 0;
    TestTrue(TEXT("id echoed as a number"), Response->TryGetNumberField(TEXT("id"), EchoedId));
    TestEqual(TEXT("id echoed matches request"), EchoedId, 4);

    TSharedPtr<FJsonObject> Result = ResultObject(Response);
    if (!TestTrue(TEXT("result present"), Result.IsValid())) return true;

    bool bIsError = true;
    Result->TryGetBoolField(TEXT("isError"), bIsError);
    TestFalse(TEXT("isError is false"), bIsError);

    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    if (TestTrue(TEXT("content array present"),
            Result->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num() > 0))
    {
        const TSharedPtr<FJsonObject> Block = (*Content)[0]->AsObject();
        if (TestTrue(TEXT("content[0] is an object"), Block.IsValid()))
        {
            TestEqual(TEXT("content[0].type is text"),
                Block->GetStringField(TEXT("type")), FString(TEXT("text")));
            const FString Text = Block->GetStringField(TEXT("text"));
            TestTrue(TEXT("content[0].text contains echo value"),
                Text.Contains(TEXT("\"echo\"")) && Text.Contains(TEXT("hello")));
            TestTrue(TEXT("content[0].text contains count value"),
                Text.Contains(TEXT("\"count\"")) && Text.Contains(TEXT("42")));
        }
    }

    const TSharedPtr<FJsonObject>* Structured = nullptr;
    if (TestTrue(TEXT("structuredContent present"),
            Result->TryGetObjectField(TEXT("structuredContent"), Structured) && Structured))
    {
        TestEqual(TEXT("structuredContent.echo matches"),
            (*Structured)->GetStringField(TEXT("echo")), FString(TEXT("hello")));
        TestEqual(TEXT("structuredContent.count matches"),
            (*Structured)->GetNumberField(TEXT("count")), 42.0);
    }

    return true;
}

// ============================================================================
// tools/call — the args.wait opt-out flag. The stream gate is opt-out, so
// FRequestDecision::bWaitAccepted is true unless args.wait is explicitly
// false: absent → true, true → true, false → false. (Whether the transport
// actually upgrades to SSE additionally needs the progressToken + Accept
// header; that wire-level selection is covered by the socket suite in
// Tests/Transport/TestStreamGate.cpp.)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallWaitAcceptedTest,
    "PinWright.infra.request_core.ToolsCall.WaitAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallWaitAcceptedTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    // Dispatches one tools/call with the given args object and asserts the
    // DispatchRpc decision carries the expected bWaitAccepted.
    auto RunWaitShape = [this](int32 Id, const TSharedPtr<FJsonObject>& InnerArgs,
        bool bExpectedWaitAccepted, const FString& Label)
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("method"), TEXT("any.method"));
        Args->SetObjectField(TEXT("args"), InnerArgs);

        McpRequestCore::FRequestDecision Out;
        TestTrue(Label + TEXT(": request accepted"), McpRequestCore::ProcessRequestBody(
            BuildToolsCallEnvelope(Id, Args), FString(), Out, MakeConfig()));
        if (!TestTrue(Label + TEXT(": execution path selected (DispatchRpc)"),
                Out.Kind == McpRequestCore::FRequestDecision::EKind::DispatchRpc)) return;
        TestEqual(Label + TEXT(": bWaitAccepted"), Out.bWaitAccepted, bExpectedWaitAccepted);
    };

    // args.wait absent → accepted (opt-out default).
    RunWaitShape(30, MakeShared<FJsonObject>(), /*bExpectedWaitAccepted=*/true,
        TEXT("wait absent"));

    // args.wait = true → accepted.
    TSharedPtr<FJsonObject> WaitTrue = MakeShared<FJsonObject>();
    WaitTrue->SetBoolField(TEXT("wait"), true);
    RunWaitShape(31, WaitTrue, /*bExpectedWaitAccepted=*/true, TEXT("wait true"));

    // args.wait = false → the explicit opt-out; NOT accepted.
    TSharedPtr<FJsonObject> WaitFalse = MakeShared<FJsonObject>();
    WaitFalse->SetBoolField(TEXT("wait"), false);
    RunWaitShape(32, WaitFalse, /*bExpectedWaitAccepted=*/false, TEXT("wait false"));

    return true;
}

// ============================================================================
// tools/call — wiki root index (arguments={}). When the wiki tree exists on
// disk (generated at editor launch), the handler returns a discovery REFERENCE
// — a small JSON pointer ({wiki, root, hint}) steering the caller to read the
// on-disk markdown — rather than the inline-rendered page. The pointer rides as
// both structuredContent and a JSON-stringified content[0].text block. Only the
// cold-start fallback (disk tree absent) renders inline markdown.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallWikiRootTest,
    "PinWright.infra.request_core.ToolsCall.WikiRoot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallWikiRootTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    const FString WikiDir = WikiDiskGenerator::OutputDirectory();
    const FString RootPath = WikiDiskGenerator::PagePath(WikiDiskGenerator::RootIndexSlug());
    if (WikiDir.IsEmpty() || RootPath.IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("wiki-output-directory-unresolved"),
            TEXT("Wiki disk generator unavailable (plugin unresolved); skipping."));
        return true;
    }
    const bool bDiskPagePresent = IFileManager::Get().FileExists(*RootPath);

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildToolsCallEnvelope(5, MakeShared<FJsonObject>()), FString(), Out, MakeConfig()));

    TestEqual(TEXT("HTTP 200"), Out.HttpCode, 200);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TSharedPtr<FJsonObject> Result = ResultObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("result present"), Result.IsValid())) return true;

    bool bIsError = true;
    Result->TryGetBoolField(TEXT("isError"), bIsError);
    TestFalse(TEXT("isError is false"), bIsError);

    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    if (!TestTrue(TEXT("content array present"),
            Result->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num() > 0))
    {
        return true;
    }
    const TSharedPtr<FJsonObject> Block = (*Content)[0]->AsObject();
    if (!TestTrue(TEXT("content[0] is an object"), Block.IsValid())) return true;

    if (bDiskPagePresent)
    {
        // Reference-pointer contract: structuredContent carries {wiki, root, hint}
        // and content[0] is the JSON-stringified pointer with an application/json
        // mimeType — no inline-rendered markdown.
        TestEqual(TEXT("content[0].mimeType is application/json"),
            Block->GetStringField(TEXT("mimeType")), FString(TEXT("application/json")));

        const TSharedPtr<FJsonObject>* Structured = nullptr;
        if (TestTrue(TEXT("structuredContent present"),
                Result->TryGetObjectField(TEXT("structuredContent"), Structured) && Structured && Structured->IsValid()))
        {
            TestEqual(TEXT("structuredContent.wiki points at the wiki output dir"),
                (*Structured)->GetStringField(TEXT("wiki")), WikiDir);
            TestEqual(TEXT("structuredContent.root points at the root index page"),
                (*Structured)->GetStringField(TEXT("root")), RootPath);
            TestFalse(TEXT("structuredContent.hint is non-empty"),
                (*Structured)->GetStringField(TEXT("hint")).IsEmpty());
        }

        // content[0].text is the same pointer object, JSON-stringified.
        TSharedPtr<FJsonObject> ParsedText;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Block->GetStringField(TEXT("text")));
        if (TestTrue(TEXT("content[0].text parses as JSON"),
                FJsonSerializer::Deserialize(Reader, ParsedText) && ParsedText.IsValid()))
        {
            TestEqual(TEXT("content[0].text.root mirrors the pointer"),
                ParsedText->GetStringField(TEXT("root")), RootPath);
        }
    }
    else
    {
        // Cold-start fallback: disk tree not yet present — inline-rendered markdown.
        FString ExpectedMarkdown;
        if (WikiHandler::RenderPage(TEXT(""), ExpectedMarkdown))
        {
            TestEqual(TEXT("content[0].text is the rendered wiki root"),
                Block->GetStringField(TEXT("text")), ExpectedMarkdown);
        }
    }

    return true;
}

// ============================================================================
// tools/call — wiki method page. arguments={method:"actor"} (no `args` field).
// When the page exists on disk, the handler returns a discovery REFERENCE
// pointer ({page, wiki, hint}) pointing at <wiki>/<slug>.md instead of the
// inline-rendered markdown; the pointer rides as both structuredContent and a
// JSON-stringified content[0].text. Cold-start fallback renders inline.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallWikiMethodTest,
    "PinWright.infra.request_core.ToolsCall.WikiMethod",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallWikiMethodTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    const FString WikiPath = TEXT("actor");

    const FString WikiDir = WikiDiskGenerator::OutputDirectory();
    const FString PagePath = WikiDiskGenerator::PagePath(WikiHandler::NormalizeSlug(WikiPath));
    if (WikiDir.IsEmpty() || PagePath.IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("wiki-output-directory-unresolved"),
            TEXT("Wiki disk generator unavailable (plugin unresolved); skipping."));
        return true;
    }
    const bool bDiskPagePresent = IFileManager::Get().FileExists(*PagePath);

    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("method"), WikiPath);
    // Intentionally no `args` field — that's what triggers wiki rendering.

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildToolsCallEnvelope(6, Args), FString(), Out, MakeConfig()));

    TestEqual(TEXT("HTTP 200"), Out.HttpCode, 200);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TSharedPtr<FJsonObject> Result = ResultObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("result present"), Result.IsValid())) return true;

    bool bIsError = true;
    Result->TryGetBoolField(TEXT("isError"), bIsError);
    TestFalse(TEXT("isError is false"), bIsError);

    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    if (!TestTrue(TEXT("content array present"),
            Result->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num() > 0))
    {
        return true;
    }
    const TSharedPtr<FJsonObject> Block = (*Content)[0]->AsObject();
    if (!TestTrue(TEXT("content[0] is an object"), Block.IsValid())) return true;

    if (bDiskPagePresent)
    {
        // Reference-pointer contract: structuredContent carries {page, wiki, hint}
        // and content[0] is the JSON-stringified pointer (application/json mime).
        TestEqual(TEXT("content[0].mimeType is application/json"),
            Block->GetStringField(TEXT("mimeType")), FString(TEXT("application/json")));

        const TSharedPtr<FJsonObject>* Structured = nullptr;
        if (TestTrue(TEXT("structuredContent present"),
                Result->TryGetObjectField(TEXT("structuredContent"), Structured) && Structured && Structured->IsValid()))
        {
            TestEqual(TEXT("structuredContent.page points at the actor wiki page"),
                (*Structured)->GetStringField(TEXT("page")), PagePath);
            TestEqual(TEXT("structuredContent.wiki points at the wiki output dir"),
                (*Structured)->GetStringField(TEXT("wiki")), WikiDir);
            TestFalse(TEXT("structuredContent.hint is non-empty"),
                (*Structured)->GetStringField(TEXT("hint")).IsEmpty());
        }

        // content[0].text is the same pointer object, JSON-stringified.
        TSharedPtr<FJsonObject> ParsedText;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Block->GetStringField(TEXT("text")));
        if (TestTrue(TEXT("content[0].text parses as JSON"),
                FJsonSerializer::Deserialize(Reader, ParsedText) && ParsedText.IsValid()))
        {
            TestEqual(TEXT("content[0].text.page mirrors the pointer"),
                ParsedText->GetStringField(TEXT("page")), PagePath);
        }
    }
    else
    {
        // Cold-start fallback: page not yet on disk — inline-rendered markdown.
        FString ExpectedMarkdown;
        if (WikiHandler::RenderPage(WikiPath, ExpectedMarkdown))
        {
            TestEqual(TEXT("content[0].text matches WikiHandler::RenderPage('actor')"),
                Block->GetStringField(TEXT("text")), ExpectedMarkdown);
        }
    }

    return true;
}

// ============================================================================
// tools/call — wrong tool name. Returns an in-band error (JSON-RPC success
// envelope, result.isError=true, message references unknown tool name).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallWrongToolTest,
    "PinWright.infra.request_core.ToolsCall.WrongToolName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallWrongToolTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildToolsCallEnvelope(7, MakeShared<FJsonObject>(), TEXT("not_call")),
        FString(), Out, MakeConfig()));

    TestEqual(TEXT("HTTP 200"), Out.HttpCode, 200);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    // No JSON-RPC error envelope: the error is in-band per MCP spec.
    TestFalse(TEXT("No top-level error field"),
        Out.ImmediateBody->HasField(TEXT("error")));

    TSharedPtr<FJsonObject> Result = ResultObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("result present"), Result.IsValid())) return true;

    bool bIsError = false;
    Result->TryGetBoolField(TEXT("isError"), bIsError);
    TestTrue(TEXT("isError is true"), bIsError);

    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    if (TestTrue(TEXT("content array present"),
            Result->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num() > 0))
    {
        const TSharedPtr<FJsonObject> Block = (*Content)[0]->AsObject();
        if (TestTrue(TEXT("content[0] is an object"), Block.IsValid()))
        {
            const FString Text = Block->GetStringField(TEXT("text"));
            TestTrue(TEXT("error text mentions unknown tool name"),
                Text.Contains(TEXT("not_call")));
        }
    }

    return true;
}

// ============================================================================
// tools/call — invalid args shape (args present but method missing).
// Returns a JSON-RPC error envelope with code -32602.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallInvalidArgsTest,
    "PinWright.infra.request_core.ToolsCall.InvalidArgs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallInvalidArgsTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    // 'method' deliberately omitted; only 'args' present.
    Args->SetObjectField(TEXT("args"), MakeShared<FJsonObject>());

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildToolsCallEnvelope(8, Args), FString(), Out, MakeConfig()));

    TestEqual(TEXT("HTTP 200"), Out.HttpCode, 200);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TSharedPtr<FJsonObject> Err = ErrorObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("error object present"), Err.IsValid())) return true;

    TestEqual(TEXT("error.code is kInvalidParams"),
        static_cast<int32>(Err->GetNumberField(TEXT("code"))),
        JsonRpc::kInvalidParams);

    // args-without-method now names the offending shape and the valid fields.
    const FString Message = Err->GetStringField(TEXT("message"));
    TestTrue(TEXT("message flags 'args' without a 'method'"),
        Message.Contains(TEXT("got 'args' without a 'method'")));
    TestTrue(TEXT("message names the 'method' field"), Message.Contains(TEXT("'method'")));
    TestTrue(TEXT("message names the 'args' field"), Message.Contains(TEXT("'args'")));
    TestTrue(TEXT("message names the 'path' alias"), Message.Contains(TEXT("'path'")));

    // Index page path is appended only when the generated index.md exists on disk.
    const FString IndexPath = WikiDiskGenerator::PagePath(TEXT("index"));
    if (!IndexPath.IsEmpty() && IFileManager::Get().FileExists(*IndexPath))
    {
        TestTrue(TEXT("message includes the index.md path when present"),
            Message.Contains(IndexPath));
    }

    return true;
}

// ============================================================================
// tools/call — handler error. Wrapping a failed completion (bSuccess=false)
// yields an in-band tool error (success envelope, isError=true, code+message
// in the text body).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallHandlerErrorTest,
    "PinWright.infra.request_core.ToolsCall.HandlerError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallHandlerErrorTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("method"), TEXT("any.method"));
    Args->SetObjectField(TEXT("args"), MakeShared<FJsonObject>());

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildToolsCallEnvelope(9, Args), FString(), Out, MakeConfig()));
    if (!TestTrue(TEXT("Execution path selected (DispatchRpc)"),
            Out.Kind == McpRequestCore::FRequestDecision::EKind::DispatchRpc)) return true;

    TSharedPtr<FJsonObject> Response = McpRequestCore::WrapToolResult(
        /*bSuccess=*/false, TEXT("Some failure"), nullptr, TEXT("INTERNAL_ERROR"),
        Out.EnvelopeId, NoSpillThreshold);
    if (!TestTrue(TEXT("Wrapped response present"), Response.IsValid())) return true;

    TestFalse(TEXT("No JSON-RPC envelope error"), Response->HasField(TEXT("error")));

    TSharedPtr<FJsonObject> Result = ResultObject(Response);
    if (!TestTrue(TEXT("result present"), Result.IsValid())) return true;

    bool bIsError = false;
    Result->TryGetBoolField(TEXT("isError"), bIsError);
    TestTrue(TEXT("isError is true"), bIsError);

    // Legacy 6-arg wrap (no Method): the error result must carry no docs field.
    TestFalse(TEXT("no docs field on a 6-arg (Method-less) error wrap"),
        Result->HasField(TEXT("docs")));

    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    if (TestTrue(TEXT("content array present"),
            Result->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num() > 0))
    {
        const TSharedPtr<FJsonObject> Block = (*Content)[0]->AsObject();
        if (TestTrue(TEXT("content[0] is an object"), Block.IsValid()))
        {
            const FString Text = Block->GetStringField(TEXT("text"));
            TestTrue(TEXT("text contains error code"),
                Text.Contains(TEXT("INTERNAL_ERROR")));
            TestTrue(TEXT("text contains failure message"),
                Text.Contains(TEXT("Some failure")));
        }
    }

    return true;
}

// ============================================================================
// tools/call — handler error WITH a structured payload (3-arg SendError).
// The wrapper must fold the Result into the tool error: structuredContent
// carries the payload and the text block appends the serialized JSON after
// the [CODE] Message line.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallHandlerErrorPayloadTest,
    "PinWright.infra.request_core.ToolsCall.HandlerErrorPayload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallHandlerErrorPayloadTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    const FString Sentinel = TEXT("AttributeError: 'dict' object has no attribute 'label'");

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("error"), TEXT("runtime_error"));
    TArray<TSharedPtr<FJsonValue>> Diagnostics;
    Diagnostics.Add(MakeShared<FJsonValueString>(Sentinel));
    Payload->SetArrayField(TEXT("diagnostics"), Diagnostics);

    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("method"), TEXT("any.method"));
    Args->SetObjectField(TEXT("args"), MakeShared<FJsonObject>());

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildToolsCallEnvelope(12, Args), FString(), Out, MakeConfig()));
    if (!TestTrue(TEXT("Execution path selected (DispatchRpc)"),
            Out.Kind == McpRequestCore::FRequestDecision::EKind::DispatchRpc)) return true;

    TSharedPtr<FJsonObject> Response = McpRequestCore::WrapToolResult(
        /*bSuccess=*/false, TEXT("Python snippet raised an exception."), Payload,
        TEXT("QUERY_FAILED"), Out.EnvelopeId, NoSpillThreshold);
    if (!TestTrue(TEXT("Wrapped response present"), Response.IsValid())) return true;

    TestFalse(TEXT("No JSON-RPC envelope error"), Response->HasField(TEXT("error")));

    TSharedPtr<FJsonObject> Result = ResultObject(Response);
    if (!TestTrue(TEXT("result present"), Result.IsValid())) return true;

    bool bIsError = false;
    Result->TryGetBoolField(TEXT("isError"), bIsError);
    TestTrue(TEXT("isError is true"), bIsError);

    // Legacy 6-arg wrap (no Method): the error result must carry no docs field.
    TestFalse(TEXT("no docs field on a 6-arg (Method-less) error wrap"),
        Result->HasField(TEXT("docs")));

    const TSharedPtr<FJsonObject>* Structured = nullptr;
    if (TestTrue(TEXT("structuredContent present"),
            Result->TryGetObjectField(TEXT("structuredContent"), Structured) && Structured))
    {
        const TArray<TSharedPtr<FJsonValue>>* Diags = nullptr;
        if (TestTrue(TEXT("structuredContent.diagnostics present"),
                (*Structured)->TryGetArrayField(TEXT("diagnostics"), Diags) && Diags && Diags->Num() > 0))
        {
            TestEqual(TEXT("structuredContent.diagnostics[0] carries the sentinel"),
                (*Diags)[0]->AsString(), Sentinel);
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    if (TestTrue(TEXT("content array present"),
            Result->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num() > 0))
    {
        const TSharedPtr<FJsonObject> Block = (*Content)[0]->AsObject();
        if (TestTrue(TEXT("content[0] is an object"), Block.IsValid()))
        {
            const FString Text = Block->GetStringField(TEXT("text"));
            TestTrue(TEXT("text contains the [QUERY_FAILED] message"),
                Text.Contains(TEXT("[QUERY_FAILED] Python snippet raised an exception.")));
            TestTrue(TEXT("text contains the payload sentinel"),
                Text.Contains(Sentinel));
        }
    }

    return true;
}

// ============================================================================
// Unknown protocol method — JSON-RPC error envelope with code -32601.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreUnknownMethodTest,
    "PinWright.infra.request_core.UnknownMethod",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreUnknownMethodTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildEnvelope(10, TEXT("totally_made_up"), nullptr), FString(), Out, MakeConfig()));

    TestEqual(TEXT("HTTP 200"), Out.HttpCode, 200);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TSharedPtr<FJsonObject> Err = ErrorObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("error object present"), Err.IsValid())) return true;

    TestEqual(TEXT("error.code is kMethodNotFound"),
        static_cast<int32>(Err->GetNumberField(TEXT("code"))),
        JsonRpc::kMethodNotFound);

    return true;
}

// ============================================================================
// Parse error — malformed JSON body. HTTP 400, error code -32700, id null.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreParseErrorTest,
    "PinWright.infra.request_core.ParseError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreParseErrorTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    McpRequestCore::FRequestDecision Out;
    TestFalse(TEXT("Request rejected before dispatch"), McpRequestCore::ProcessRequestBody(
        TEXT("{not json"), FString(), Out, MakeConfig()));

    TestEqual(TEXT("HTTP 400"), Out.HttpCode, 400);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TSharedPtr<FJsonObject> Err = ErrorObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("error object present"), Err.IsValid())) return true;

    TestEqual(TEXT("error.code is kParseError"),
        static_cast<int32>(Err->GetNumberField(TEXT("code"))),
        JsonRpc::kParseError);

    // Id null when the parse fails before id extraction.
    const TSharedPtr<FJsonValue> IdField = Out.ImmediateBody->TryGetField(TEXT("id"));
    TestTrue(TEXT("envelope id is null"),
        IdField.IsValid() && IdField->Type == EJson::Null);

    return true;
}

// ============================================================================
// Batch rejection — JSON arrays. HTTP 400, error code -32600.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreBatchRejectionTest,
    "PinWright.infra.request_core.BatchRejection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreBatchRejectionTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    const FString BatchBody =
        TEXT("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"},")
        TEXT("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}]");

    McpRequestCore::FRequestDecision Out;
    TestFalse(TEXT("Request rejected before dispatch"), McpRequestCore::ProcessRequestBody(
        BatchBody, FString(), Out, MakeConfig()));

    TestEqual(TEXT("HTTP 400"), Out.HttpCode, 400);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TSharedPtr<FJsonObject> Err = ErrorObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("error object present"), Err.IsValid())) return true;

    TestEqual(TEXT("error.code is kInvalidRequest"),
        static_cast<int32>(Err->GetNumberField(TEXT("code"))),
        JsonRpc::kInvalidRequest);

    const FString Message = Err->GetStringField(TEXT("message"));
    TestTrue(TEXT("message mentions batch"),
        Message.Contains(TEXT("Batch")) || Message.Contains(TEXT("batch")));

    const TSharedPtr<FJsonValue> IdField = Out.ImmediateBody->TryGetField(TEXT("id"));
    TestTrue(TEXT("envelope id is null"),
        IdField.IsValid() && IdField->Type == EJson::Null);

    return true;
}

// ============================================================================
// Body too large — a body bigger than the config's MaxBodyBytes. HTTP 400,
// -32600. The cap is per-config here, so a small cap + a normal ping envelope
// exercises the branch without megabyte padding.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreBodyTooLargeTest,
    "PinWright.infra.request_core.BodyTooLarge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreBodyTooLargeTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    const FString Body = BuildEnvelope(11, TEXT("ping"), nullptr);
    // Cap strictly below the body's UTF-8 size so the size gate trips.
    const int32 TinyCap = FTCHARToUTF8(*Body).Length() - 1;

    McpRequestCore::FRequestDecision Out;
    TestFalse(TEXT("Request rejected before dispatch"), McpRequestCore::ProcessRequestBody(
        Body, FString(), Out, MakeConfig(FString(), TinyCap)));

    TestEqual(TEXT("HTTP 400"), Out.HttpCode, 400);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TSharedPtr<FJsonObject> Err = ErrorObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("error object present"), Err.IsValid())) return true;

    TestEqual(TEXT("error.code is kInvalidRequest"),
        static_cast<int32>(Err->GetNumberField(TEXT("code"))),
        JsonRpc::kInvalidRequest);

    return true;
}

// ============================================================================
// Auth: token set, no Authorization header — rejected with HTTP 401 and a body
// naming the gateway-token file. (The WWW-Authenticate: Bearer header is the
// transport's job on 401; wire-level 401 emission is covered by the socket
// suite: PinWright.transport.socket_http.Parse.AuthMissing401.)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreAuthMissingHeaderTest,
    "PinWright.infra.request_core.Auth.MissingHeader",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreAuthMissingHeaderTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    McpRequestCore::FRequestDecision Out;
    TestFalse(TEXT("Request rejected before dispatch"), McpRequestCore::ProcessRequestBody(
        BuildEnvelope(20, TEXT("ping"), nullptr), FString(), Out, MakeConfig(TestAuthToken)));

    TestEqual(TEXT("HTTP 401"), Out.HttpCode, 401);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TSharedPtr<FJsonObject> Err = ErrorObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("error object present"), Err.IsValid())) return true;

    TestTrue(TEXT("Body names the gateway-token file"),
        Err->GetStringField(TEXT("message")).Contains(TEXT("gateway-token")));

    return true;
}

// ============================================================================
// Auth: token set, wrong bearer credentials — HTTP 401.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreAuthWrongTokenTest,
    "PinWright.infra.request_core.Auth.WrongToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreAuthWrongTokenTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    McpRequestCore::FRequestDecision Out;
    TestFalse(TEXT("Request rejected before dispatch"), McpRequestCore::ProcessRequestBody(
        BuildEnvelope(21, TEXT("ping"), nullptr),
        TEXT("Bearer deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef"),
        Out, MakeConfig(TestAuthToken)));

    TestEqual(TEXT("HTTP 401"), Out.HttpCode, 401);

    return true;
}

// ============================================================================
// Auth: token set, correct bearer credentials — HTTP 200 with a normal
// JSON-RPC ping result (id echoed, no error). ping is protocol-local and
// carries the bootstrap editorReady state.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreAuthCorrectTokenTest,
    "PinWright.infra.request_core.Auth.CorrectToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreAuthCorrectTokenTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildEnvelope(22, TEXT("ping"), nullptr),
        FString(TEXT("Bearer ")) + TestAuthToken,
        Out, MakeConfig(TestAuthToken)));

    TestEqual(TEXT("HTTP 200"), Out.HttpCode, 200);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TestFalse(TEXT("No JSON-RPC error field"), Out.ImmediateBody->HasField(TEXT("error")));

    TSharedPtr<FJsonObject> Result = ResultObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("result is an object"), Result.IsValid())) return true;
    bool bEditorReady = false;
    TestTrue(TEXT("ping reports editorReady"),
        Result->TryGetBoolField(TEXT("editorReady"), bEditorReady));
    TestTrue(TEXT("correct-token test config is operational"), bEditorReady);
    bool bRetryable = true;
    TestTrue(TEXT("ping reports retryable"),
        Result->TryGetBoolField(TEXT("retryable"), bRetryable));
    TestFalse(TEXT("ready ping is not retryable"), bRetryable);

    int32 EchoedId = 0;
    TestTrue(TEXT("id echoed as a number"),
        Out.ImmediateBody->TryGetNumberField(TEXT("id"), EchoedId));
    TestEqual(TEXT("id echoed matches request"), EchoedId, 22);

    return true;
}

// ============================================================================
// Auth: a notification (no id) with a bad/absent token is still rejected with
// HTTP 401 — NOT the 202 notification ack. This locks in the pre-parse gate
// placement: unauthenticated requests never reach notification handling.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreAuthNotificationWithBadTokenTest,
    "PinWright.infra.request_core.Auth.NotificationWithBadToken",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreAuthNotificationWithBadTokenTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    // No Authorization header at all on a notification envelope.
    McpRequestCore::FRequestDecision Out;
    TestFalse(TEXT("Request rejected before dispatch"), McpRequestCore::ProcessRequestBody(
        BuildNotification(TEXT("notifications/initialized")), FString(), Out,
        MakeConfig(TestAuthToken)));

    TestEqual(TEXT("HTTP 401 (not the 202 notification ack)"), Out.HttpCode, 401);

    return true;
}

// ============================================================================
// Auth: token empty (auth disabled) — a request with no Authorization header
// is served normally (HTTP 200).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreAuthDisabledAllowsAllTest,
    "PinWright.infra.request_core.Auth.DisabledAllowsAll",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreAuthDisabledAllowsAllTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildEnvelope(23, TEXT("ping"), nullptr), FString(), Out,
        MakeConfig(/*AuthToken=*/FString())));

    TestEqual(TEXT("HTTP 200"), Out.HttpCode, 200);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TSharedPtr<FJsonObject> Result = ResultObject(Out.ImmediateBody);
    TestTrue(TEXT("result is an object"), Result.IsValid());

    return true;
}

// ============================================================================
// tools/call — `path` as an alias for `method` on the wiki-page branch.
// arguments={path:"actor"} (no `args`) routes to the METHOD-PAGE branch, not the
// root reference: the on-disk reference carries {page, wiki, hint} with no
// `root` field. Mirrors WikiMethodTest's cold-start conditional.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallPathAliasWikiTest,
    "PinWright.infra.request_core.ToolsCall.PathAliasWiki",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallPathAliasWikiTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    const FString WikiPath = TEXT("actor");

    const FString WikiDir = WikiDiskGenerator::OutputDirectory();
    const FString PagePath = WikiDiskGenerator::PagePath(WikiHandler::NormalizeSlug(WikiPath));
    if (WikiDir.IsEmpty() || PagePath.IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("wiki-output-directory-unresolved"),
            TEXT("Wiki disk generator unavailable (plugin unresolved); skipping."));
        return true;
    }
    const bool bDiskPagePresent = IFileManager::Get().FileExists(*PagePath);

    // `path` alias in place of `method`; still no `args` field so wiki rendering wins.
    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("path"), WikiPath);

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildToolsCallEnvelope(60, Args), FString(), Out, MakeConfig()));

    TestEqual(TEXT("HTTP 200"), Out.HttpCode, 200);
    if (!TestTrue(TEXT("Response body present"), Out.ImmediateBody.IsValid())) return true;

    TSharedPtr<FJsonObject> Result = ResultObject(Out.ImmediateBody);
    if (!TestTrue(TEXT("result present"), Result.IsValid())) return true;

    bool bIsError = true;
    Result->TryGetBoolField(TEXT("isError"), bIsError);
    TestFalse(TEXT("isError is false"), bIsError);

    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    if (!TestTrue(TEXT("content array present"),
            Result->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num() > 0))
    {
        return true;
    }
    const TSharedPtr<FJsonObject> Block = (*Content)[0]->AsObject();
    if (!TestTrue(TEXT("content[0] is an object"), Block.IsValid())) return true;

    if (bDiskPagePresent)
    {
        // Method-page reference shape ({page, wiki, hint}) — never the root pointer.
        TestEqual(TEXT("content[0].mimeType is application/json"),
            Block->GetStringField(TEXT("mimeType")), FString(TEXT("application/json")));

        const TSharedPtr<FJsonObject>* Structured = nullptr;
        if (TestTrue(TEXT("structuredContent present"),
                Result->TryGetObjectField(TEXT("structuredContent"), Structured) && Structured && Structured->IsValid()))
        {
            TestEqual(TEXT("structuredContent.page points at the actor wiki page"),
                (*Structured)->GetStringField(TEXT("page")), PagePath);
            TestEqual(TEXT("structuredContent.wiki points at the wiki output dir"),
                (*Structured)->GetStringField(TEXT("wiki")), WikiDir);
            TestFalse(TEXT("structuredContent.hint is non-empty"),
                (*Structured)->GetStringField(TEXT("hint")).IsEmpty());
            // The `path` alias must NOT degrade to the root reference.
            TestFalse(TEXT("structuredContent has no root field (method-page, not root)"),
                (*Structured)->HasField(TEXT("root")));
        }
    }
    else
    {
        // Cold-start fallback: page not yet on disk — inline-rendered markdown.
        FString ExpectedMarkdown;
        if (WikiHandler::RenderPage(WikiPath, ExpectedMarkdown))
        {
            TestEqual(TEXT("content[0].text matches WikiHandler::RenderPage('actor')"),
                Block->GetStringField(TEXT("text")), ExpectedMarkdown);
        }
    }

    return true;
}

// ============================================================================
// tools/call — `path` as an alias for `method` on the execution branch.
// arguments={path:"any.method", args:{}} yields a DispatchRpc decision whose
// Method is the dotted name carried by `path`.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallPathAliasExecutionTest,
    "PinWright.infra.request_core.ToolsCall.PathAliasExecution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallPathAliasExecutionTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("path"), TEXT("any.method"));
    Args->SetObjectField(TEXT("args"), MakeShared<FJsonObject>());

    McpRequestCore::FRequestDecision Out;
    TestTrue(TEXT("Request accepted"), McpRequestCore::ProcessRequestBody(
        BuildToolsCallEnvelope(61, Args), FString(), Out, MakeConfig()));

    if (!TestTrue(TEXT("Execution path selected (DispatchRpc)"),
            Out.Kind == McpRequestCore::FRequestDecision::EKind::DispatchRpc)) return true;
    TestEqual(TEXT("Dispatch method comes from the 'path' alias"),
        Out.Method, FString(TEXT("any.method")));

    return true;
}

// ============================================================================
// tools/call — `method` and `path` set together. Different values are a
// conflict (-32602, message names both and the alias rule); equal values
// proceed as a normal dispatch with that method.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallMethodPathConflictTest,
    "PinWright.infra.request_core.ToolsCall.MethodPathConflict",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallMethodPathConflictTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    // Different values: conflict rejected with -32602.
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("method"), TEXT("a.b"));
        Args->SetStringField(TEXT("path"), TEXT("c.d"));
        Args->SetObjectField(TEXT("args"), MakeShared<FJsonObject>());

        McpRequestCore::FRequestDecision Out;
        TestTrue(TEXT("conflict: request accepted"), McpRequestCore::ProcessRequestBody(
            BuildToolsCallEnvelope(62, Args), FString(), Out, MakeConfig()));

        TestEqual(TEXT("conflict: HTTP 200"), Out.HttpCode, 200);
        if (TestTrue(TEXT("conflict: response body present"), Out.ImmediateBody.IsValid()))
        {
            TSharedPtr<FJsonObject> Err = ErrorObject(Out.ImmediateBody);
            if (TestTrue(TEXT("conflict: error object present"), Err.IsValid()))
            {
                TestEqual(TEXT("conflict: error.code is kInvalidParams"),
                    static_cast<int32>(Err->GetNumberField(TEXT("code"))), JsonRpc::kInvalidParams);
                const FString Message = Err->GetStringField(TEXT("message"));
                TestTrue(TEXT("conflict: message contains the method value"),
                    Message.Contains(TEXT("a.b")));
                TestTrue(TEXT("conflict: message contains the path value"),
                    Message.Contains(TEXT("c.d")));
                TestTrue(TEXT("conflict: message explains the alias rule"),
                    Message.Contains(TEXT("'path' is an alias for 'method'")));
            }
        }
    }

    // Equal values: not a conflict — dispatch proceeds with the shared method.
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("method"), TEXT("a.b"));
        Args->SetStringField(TEXT("path"), TEXT("a.b"));
        Args->SetObjectField(TEXT("args"), MakeShared<FJsonObject>());

        McpRequestCore::FRequestDecision Out;
        TestTrue(TEXT("equal: request accepted"), McpRequestCore::ProcessRequestBody(
            BuildToolsCallEnvelope(63, Args), FString(), Out, MakeConfig()));

        if (TestTrue(TEXT("equal: execution path selected (DispatchRpc)"),
                Out.Kind == McpRequestCore::FRequestDecision::EKind::DispatchRpc))
        {
            TestEqual(TEXT("equal: dispatch method is the shared value"),
                Out.Method, FString(TEXT("a.b")));
        }
    }

    return true;
}

// ============================================================================
// tools/call — unknown top-level argument fields. Any field other than
// `method`, `args`, `path` is rejected first with -32602; the message names
// each offending field, the valid-fields text, and (when present) the index.md
// path. The unknown-field error wins even when method+args are otherwise valid.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallUnknownArgFieldTest,
    "PinWright.infra.request_core.ToolsCall.UnknownArgField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallUnknownArgFieldTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    // Dispatch one tools/call, assert the -32602 envelope error, return its message.
    auto ExpectInvalidParams = [this](int32 Id, const TSharedPtr<FJsonObject>& Args,
        const FString& Label) -> FString
    {
        McpRequestCore::FRequestDecision Out;
        if (!TestTrue(Label + TEXT(": request accepted"), McpRequestCore::ProcessRequestBody(
                BuildToolsCallEnvelope(Id, Args), FString(), Out, MakeConfig())))
        {
            return FString();
        }
        TestEqual(Label + TEXT(": HTTP 200"), Out.HttpCode, 200);
        if (!TestTrue(Label + TEXT(": response body present"), Out.ImmediateBody.IsValid()))
        {
            return FString();
        }
        TSharedPtr<FJsonObject> Err = ErrorObject(Out.ImmediateBody);
        if (!TestTrue(Label + TEXT(": error object present"), Err.IsValid()))
        {
            return FString();
        }
        TestEqual(Label + TEXT(": error.code is kInvalidParams"),
            static_cast<int32>(Err->GetNumberField(TEXT("code"))), JsonRpc::kInvalidParams);
        return Err->GetStringField(TEXT("message"));
    };

    // A single typo'd field name ('methd') is unknown and rejected on its own.
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("methd"), TEXT("actor.spawn"));
        const FString Message = ExpectInvalidParams(64, Args, TEXT("typo field"));
        TestTrue(TEXT("message names the offending 'methd' field"),
            Message.Contains(TEXT("'methd'")));
        TestTrue(TEXT("message names the valid 'method' field"),
            Message.Contains(TEXT("'method'")));
        TestTrue(TEXT("message names the 'path' alias"), Message.Contains(TEXT("'path'")));
        TestTrue(TEXT("message names the 'args' field"), Message.Contains(TEXT("'args'")));

        const FString IndexPath = WikiDiskGenerator::PagePath(TEXT("index"));
        if (!IndexPath.IsEmpty() && IFileManager::Get().FileExists(*IndexPath))
        {
            TestTrue(TEXT("message includes the index.md path when present"),
                Message.Contains(IndexPath));
        }
    }

    // Unknown 'extra' alongside an otherwise-valid method+args is still rejected.
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("method"), TEXT("x"));
        Args->SetObjectField(TEXT("args"), MakeShared<FJsonObject>());
        Args->SetNumberField(TEXT("extra"), 1);
        const FString Message = ExpectInvalidParams(65, Args, TEXT("extra field"));
        TestTrue(TEXT("message names the offending 'extra' field"),
            Message.Contains(TEXT("'extra'")));
    }

    // Two unknown fields — both are named in the message.
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("method"), TEXT("x"));
        Args->SetObjectField(TEXT("args"), MakeShared<FJsonObject>());
        Args->SetNumberField(TEXT("foo"), 1);
        Args->SetNumberField(TEXT("bar"), 2);
        const FString Message = ExpectInvalidParams(66, Args, TEXT("two unknown fields"));
        TestTrue(TEXT("message names the offending 'foo' field"),
            Message.Contains(TEXT("'foo'")));
        TestTrue(TEXT("message names the offending 'bar' field"),
            Message.Contains(TEXT("'bar'")));
    }

    return true;
}

// ============================================================================
// tools/call — non-string `method` / `path`. Present-but-wrong-type is rejected
// with -32602 and a message naming the field and stating it must be a string.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallArgFieldTypeTest,
    "PinWright.infra.request_core.ToolsCall.ArgFieldType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallArgFieldTypeTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    auto ExpectInvalidParams = [this](int32 Id, const TSharedPtr<FJsonObject>& Args,
        const FString& Label) -> FString
    {
        McpRequestCore::FRequestDecision Out;
        if (!TestTrue(Label + TEXT(": request accepted"), McpRequestCore::ProcessRequestBody(
                BuildToolsCallEnvelope(Id, Args), FString(), Out, MakeConfig())))
        {
            return FString();
        }
        TestEqual(Label + TEXT(": HTTP 200"), Out.HttpCode, 200);
        if (!TestTrue(Label + TEXT(": response body present"), Out.ImmediateBody.IsValid()))
        {
            return FString();
        }
        TSharedPtr<FJsonObject> Err = ErrorObject(Out.ImmediateBody);
        if (!TestTrue(Label + TEXT(": error object present"), Err.IsValid()))
        {
            return FString();
        }
        TestEqual(Label + TEXT(": error.code is kInvalidParams"),
            static_cast<int32>(Err->GetNumberField(TEXT("code"))), JsonRpc::kInvalidParams);
        return Err->GetStringField(TEXT("message"));
    };

    // {path:123} — numeric path.
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetNumberField(TEXT("path"), 123);
        const FString Message = ExpectInvalidParams(67, Args, TEXT("numeric path"));
        TestTrue(TEXT("message names the 'path' field"), Message.Contains(TEXT("path")));
        TestTrue(TEXT("message says must be a string"),
            Message.Contains(TEXT("must be a string")));
    }

    // {method:{}} — object method.
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetObjectField(TEXT("method"), MakeShared<FJsonObject>());
        const FString Message = ExpectInvalidParams(68, Args, TEXT("object method"));
        TestTrue(TEXT("message names the 'method' field"), Message.Contains(TEXT("method")));
        TestTrue(TEXT("message says must be a string"),
            Message.Contains(TEXT("must be a string")));
    }

    return true;
}

// ============================================================================
// tools/call — present `arguments` and `args` values must be objects. Wrong
// shapes are JSON-RPC invalid-params errors and never become DispatchRpc.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreToolsCallArgumentObjectTypeTest,
    "PinWright.infra.request_core.ToolsCall.ArgumentObjectType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreToolsCallArgumentObjectTypeTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    auto ExpectInvalidParams = [this](const FString& Body,
        const FString& Field, const FString& Label)
    {
        McpRequestCore::FRequestDecision Out;
        if (!TestTrue(Label + TEXT(": request accepted"), McpRequestCore::ProcessRequestBody(
                Body, FString(), Out, MakeConfig())))
        {
            return;
        }

        TestEqual(Label + TEXT(": HTTP 200"), Out.HttpCode, 200);
        // ProcessRequestBody has no callback parameter; DispatchRpc is the only
        // decision that would invoke SocketHttpServer's capture handler.
        const bool bCaptureHandlerInvoked =
            Out.Kind == McpRequestCore::FRequestDecision::EKind::DispatchRpc;
        TestFalse(Label + TEXT(": capture handler was not invoked"), bCaptureHandlerInvoked);
        if (!TestTrue(Label + TEXT(": immediate response"),
                Out.Kind == McpRequestCore::FRequestDecision::EKind::ImmediateResponse))
        {
            return;
        }
        if (!TestTrue(Label + TEXT(": response body present"), Out.ImmediateBody.IsValid()))
        {
            return;
        }

        TSharedPtr<FJsonObject> Err = ErrorObject(Out.ImmediateBody);
        if (!TestTrue(Label + TEXT(": error object present"), Err.IsValid()))
        {
            return;
        }
        TestEqual(Label + TEXT(": error.code is kInvalidParams"),
            static_cast<int32>(Err->GetNumberField(TEXT("code"))), JsonRpc::kInvalidParams);
        const FString Message = Err->GetStringField(TEXT("message"));
        TestTrue(Label + TEXT(": message names the offending field"), Message.Contains(Field));
        TestTrue(Label + TEXT(": message requires an object"),
            Message.Contains(TEXT("must be an object")));
        TestFalse(Label + TEXT(": no JSON-RPC result"),
            Out.ImmediateBody->HasField(TEXT("result")));
    };

    TArray<TSharedPtr<FJsonValue>> EmptyArray;
    ExpectInvalidParams(
        BuildToolsCallEnvelopeValue(69, MakeShared<FJsonValueString>(TEXT("PinWright.transport"))),
        TEXT("'arguments'"), TEXT("outer string"));
    ExpectInvalidParams(
        BuildToolsCallEnvelopeValue(70, MakeShared<FJsonValueArray>(EmptyArray)),
        TEXT("'arguments'"), TEXT("outer array"));
    ExpectInvalidParams(
        BuildToolsCallEnvelopeValue(71, MakeShared<FJsonValueNull>()),
        TEXT("'arguments'"), TEXT("outer null"));

    auto BuildInnerArgsBody = [](int32 Id, const TSharedPtr<FJsonValue>& InvalidArgs)
    {
        TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
        Arguments->SetStringField(TEXT("method"), TEXT("system.run_tests"));
        Arguments->SetField(TEXT("args"), InvalidArgs);
        return BuildToolsCallEnvelope(Id, Arguments);
    };

    ExpectInvalidParams(
        BuildInnerArgsBody(72, MakeShared<FJsonValueString>(TEXT("PinWright.transport"))),
        TEXT("'args'"), TEXT("inner string"));
    ExpectInvalidParams(
        BuildInnerArgsBody(73, MakeShared<FJsonValueArray>(EmptyArray)),
        TEXT("'args'"), TEXT("inner array"));
    ExpectInvalidParams(
        BuildInnerArgsBody(74, MakeShared<FJsonValueNull>()),
        TEXT("'args'"), TEXT("inner null"));

    return true;
}

// ============================================================================
// WrapToolResult — error docs pointer. A failed wrap with a non-empty Method
// appends a `Docs: <page>` line to content[0].text and attaches a top-level
// docs object {page, wiki}. Successes never get docs; the legacy 6-arg wrap
// (empty Method) never gets docs. All page assertions are conditional on the
// generated page existing on disk (cold machine may lack the wiki tree).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreWrapToolResultErrorDocsTest,
    "PinWright.infra.request_core.ToolsCall.ErrorDocs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreWrapToolResultErrorDocsTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    const FString WikiDir = WikiDiskGenerator::OutputDirectory();
    const FString PagePath = WikiDiskGenerator::PagePath(WikiHandler::NormalizeSlug(TEXT("actor")));
    if (WikiDir.IsEmpty() || PagePath.IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("wiki-output-directory-unresolved"),
            TEXT("Wiki disk generator unavailable (plugin unresolved); skipping."));
        return true;
    }
    const bool bDiskPagePresent = IFileManager::Get().FileExists(*PagePath);

    const TSharedPtr<FJsonValue> Id = MakeShared<FJsonValueNumber>(99);

    // Error wrap WITH Method set.
    {
        TSharedPtr<FJsonObject> Response = McpRequestCore::WrapToolResult(
            /*bSuccess=*/false, TEXT("boom"), nullptr, TEXT("UNKNOWN_ACTION"),
            Id, NoSpillThreshold, TEXT("actor"));
        if (TestTrue(TEXT("wrapped response present"), Response.IsValid()))
        {
            TSharedPtr<FJsonObject> Result = ResultObject(Response);
            if (TestTrue(TEXT("result present"), Result.IsValid()))
            {
                if (bDiskPagePresent)
                {
                    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
                    if (TestTrue(TEXT("content array present"),
                            Result->TryGetArrayField(TEXT("content"), Content) && Content && Content->Num() > 0))
                    {
                        const TSharedPtr<FJsonObject> Block = (*Content)[0]->AsObject();
                        if (TestTrue(TEXT("content[0] is an object"), Block.IsValid()))
                        {
                            const FString Text = Block->GetStringField(TEXT("text"));
                            TestTrue(TEXT("content[0].text ends with the Docs: page line"),
                                Text.EndsWith(FString(TEXT("Docs: ")) + PagePath));
                        }
                    }

                    const TSharedPtr<FJsonObject>* Docs = nullptr;
                    if (TestTrue(TEXT("top-level docs object present"),
                            Result->TryGetObjectField(TEXT("docs"), Docs) && Docs && (*Docs).IsValid()))
                    {
                        TestEqual(TEXT("docs.page is the resolved page path"),
                            (*Docs)->GetStringField(TEXT("page")), PagePath);
                        TestEqual(TEXT("docs.wiki is the wiki output dir"),
                            (*Docs)->GetStringField(TEXT("wiki")), WikiDir);
                    }
                }
                else
                {
                    TestFalse(TEXT("no docs field when the page is absent on disk"),
                        Result->HasField(TEXT("docs")));
                }
            }
        }
    }

    // Success wrap WITH Method set — never gets docs.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("ok"), TEXT("yes"));
        TSharedPtr<FJsonObject> Response = McpRequestCore::WrapToolResult(
            /*bSuccess=*/true, TEXT("OK"), Payload, TEXT(""), Id, NoSpillThreshold, TEXT("actor"));
        if (TestTrue(TEXT("success response present"), Response.IsValid()))
        {
            TSharedPtr<FJsonObject> Result = ResultObject(Response);
            if (TestTrue(TEXT("success result present"), Result.IsValid()))
            {
                TestFalse(TEXT("success wrap never carries docs, even with Method set"),
                    Result->HasField(TEXT("docs")));
            }
        }
    }

    // Legacy 6-arg error wrap (empty Method) — no docs.
    {
        TSharedPtr<FJsonObject> Response = McpRequestCore::WrapToolResult(
            /*bSuccess=*/false, TEXT("boom"), nullptr, TEXT("UNKNOWN_ACTION"), Id, NoSpillThreshold);
        if (TestTrue(TEXT("legacy response present"), Response.IsValid()))
        {
            TSharedPtr<FJsonObject> Result = ResultObject(Response);
            if (TestTrue(TEXT("legacy result present"), Result.IsValid()))
            {
                TestFalse(TEXT("legacy 6-arg wrap adds no docs field"),
                    Result->HasField(TEXT("docs")));
            }
        }
    }

    return true;
}

// ============================================================================
// WrapToolResult — docs page walk-up. An unresolved verb strips trailing
// `.segment`s to the nearest existing page (actor.totally_bogus_verb -> actor.md);
// a wholly-unknown namespace falls back to index.md. Conditional on disk.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreWrapToolResultDocsWalkUpTest,
    "PinWright.infra.request_core.ToolsCall.ErrorDocsWalkUp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreWrapToolResultDocsWalkUpTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    const FString WikiDir = WikiDiskGenerator::OutputDirectory();
    if (WikiDir.IsEmpty())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("wiki-output-directory-unresolved"),
            TEXT("Wiki disk generator unavailable (plugin unresolved); skipping."));
        return true;
    }

    const TSharedPtr<FJsonValue> Id = MakeShared<FJsonValueNumber>(101);

    // actor.totally_bogus_verb walks up to actor.md.
    const FString ActorPath = WikiDiskGenerator::PagePath(WikiHandler::NormalizeSlug(TEXT("actor")));
    if (!ActorPath.IsEmpty() && IFileManager::Get().FileExists(*ActorPath))
    {
        TSharedPtr<FJsonObject> Response = McpRequestCore::WrapToolResult(
            /*bSuccess=*/false, TEXT("boom"), nullptr, TEXT("UNKNOWN_ACTION"),
            Id, NoSpillThreshold, TEXT("actor.totally_bogus_verb"));
        if (TestTrue(TEXT("walk-up response present"), Response.IsValid()))
        {
            TSharedPtr<FJsonObject> Result = ResultObject(Response);
            if (TestTrue(TEXT("walk-up result present"), Result.IsValid()))
            {
                const TSharedPtr<FJsonObject>* Docs = nullptr;
                if (TestTrue(TEXT("docs object present"),
                        Result->TryGetObjectField(TEXT("docs"), Docs) && Docs && (*Docs).IsValid()))
                {
                    TestEqual(TEXT("docs.page walks up to actor.md"),
                        (*Docs)->GetStringField(TEXT("page")), ActorPath);
                }
            }
        }
    }

    // A wholly-unknown namespace falls back to index.md.
    const FString IndexPath = WikiDiskGenerator::PagePath(TEXT("index"));
    if (!IndexPath.IsEmpty() && IFileManager::Get().FileExists(*IndexPath))
    {
        TSharedPtr<FJsonObject> Response = McpRequestCore::WrapToolResult(
            /*bSuccess=*/false, TEXT("boom"), nullptr, TEXT("UNKNOWN_ACTION"),
            Id, NoSpillThreshold, TEXT("zznope.zzz"));
        if (TestTrue(TEXT("fallback response present"), Response.IsValid()))
        {
            TSharedPtr<FJsonObject> Result = ResultObject(Response);
            if (TestTrue(TEXT("fallback result present"), Result.IsValid()))
            {
                const TSharedPtr<FJsonObject>* Docs = nullptr;
                if (TestTrue(TEXT("docs object present"),
                        Result->TryGetObjectField(TEXT("docs"), Docs) && Docs && (*Docs).IsValid()))
                {
                    TestEqual(TEXT("docs.page falls back to index.md"),
                        (*Docs)->GetStringField(TEXT("page")), IndexPath);
                }
            }
        }
    }

    return true;
}

// ============================================================================
// WrapToolResult — docs survives an oversize spill. When the wrapped result is
// spilled to a file (message over the spill threshold), content/structuredContent
// are rewritten but the top-level docs field is preserved. Uses the spill-root
// override so the spill output is isolated and cleaned up.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpRequestCoreWrapToolResultDocsSpillSurvivalTest,
    "PinWright.infra.request_core.ToolsCall.ErrorDocsSpillSurvival",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMcpRequestCoreWrapToolResultDocsSpillSurvivalTest::RunTest(const FString& Parameters)
{
    using namespace McpRequestCoreTest;

    // Isolate spill output under a throwaway root so we can wipe it afterward.
    FString Root = FPaths::ProjectIntermediateDir() /
        TEXT("PinWrightTests/McpRequestCoreDocsSpill") /
        FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Root = FPaths::ConvertRelativePathToFull(Root);
    FPaths::NormalizeDirectoryName(Root);
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(Root);

    const FString WikiDir = WikiDiskGenerator::OutputDirectory();
    const FString PagePath = WikiDiskGenerator::PagePath(WikiHandler::NormalizeSlug(TEXT("actor")));
    const bool bDiskPagePresent = !PagePath.IsEmpty() && IFileManager::Get().FileExists(*PagePath);

    const TSharedPtr<FJsonValue> Id = MakeShared<FJsonValueNumber>(202);
    // A message longer than the 1024-char threshold forces the oversize spill.
    const FString BigMessage = FString::ChrN(2048, TEXT('x'));

    TSharedPtr<FJsonObject> Response = McpRequestCore::WrapToolResult(
        /*bSuccess=*/false, BigMessage, nullptr, TEXT("BIG_ERROR"),
        Id, /*SpillThreshold=*/1024, TEXT("actor"));

    if (TestTrue(TEXT("wrapped response present"), Response.IsValid()))
    {
        TSharedPtr<FJsonObject> Result = ResultObject(Response);
        if (TestTrue(TEXT("result present"), Result.IsValid()))
        {
            const TSharedPtr<FJsonObject>* Structured = nullptr;
            if (TestTrue(TEXT("structuredContent present after spill"),
                    Result->TryGetObjectField(TEXT("structuredContent"), Structured) && Structured && (*Structured).IsValid()))
            {
                bool bOutputTooLong = false;
                TestTrue(TEXT("structuredContent.outputTooLong is true"),
                    (*Structured)->TryGetBoolField(TEXT("outputTooLong"), bOutputTooLong) && bOutputTooLong);
            }

            if (bDiskPagePresent)
            {
                const TSharedPtr<FJsonObject>* Docs = nullptr;
                if (TestTrue(TEXT("docs survives the spill rewrite"),
                        Result->TryGetObjectField(TEXT("docs"), Docs) && Docs && (*Docs).IsValid()))
                {
                    TestEqual(TEXT("docs.page still points at the resolved page"),
                        (*Docs)->GetStringField(TEXT("page")), PagePath);
                }
            }
        }
    }

    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
    IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
