// Copyright (c) 2026 Alexander Penkin. MIT License.

// Setup-screen agent config detection: an installed "pinwright" entry reads Configured only
// when it equals, field for field, the entry Install would write now; any other difference
// reads Outdated. Path-valued fields ignore slash direction and (on Windows) case.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Setup/AgentMcpConfigurator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace PinWrightAgentMcpConfiguratorTest
{
    // Synthetic absolute paths; only their spelling matters to the comparator.
    struct FProbePaths
    {
        FString Python;
        FString OtherPython;
        FString Script;
        FString TokenFile;
        FString PortFile;
    };

    FProbePaths MakeProbePaths()
    {
        FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("AgentMcpProbe"));
        Root.ReplaceInline(TEXT("\\"), TEXT("/"));
        return { Root / TEXT("EngineA/Python/python.exe"), Root / TEXT("EngineB/Python/python.exe"),
                 Root / TEXT("Plugin/Content/Python/mcp_proxy.py"), Root / TEXT("Saved/gateway-token"),
                 Root / TEXT("Saved/gateway-port") };
    }

    TArray<FString> ProxyArgs(const FProbePaths& P)
    {
        return { P.Script, TEXT("--token-file"), P.TokenFile, TEXT("--port-file"), P.PortFile };
    }

    TSharedRef<FJsonObject> MakeStdioEntry(const FString& Command, const TArray<FString>& Args)
    {
        const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("type"), TEXT("stdio"));
        Entry->SetStringField(TEXT("command"), Command);
        TArray<TSharedPtr<FJsonValue>> ArgsArray;
        for (const FString& Arg : Args)
        {
            ArgsArray.Add(MakeShared<FJsonValueString>(Arg));
        }
        Entry->SetArrayField(TEXT("args"), ArgsArray);
        return Entry;
    }

    TSharedRef<FJsonObject> MakeHttpEntry(const FString& Url, const FString& Token)
    {
        const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("type"), TEXT("http"));
        Entry->SetStringField(TEXT("url"), Url);
        const TSharedRef<FJsonObject> Headers = MakeShared<FJsonObject>();
        Headers->SetStringField(TEXT("Authorization"), TEXT("Bearer ") + Token);
        Entry->SetObjectField(TEXT("headers"), Headers);
        return Entry;
    }

    // Each path with every '/' swapped for Separator.
    FString Reslash(const FString& Path, const TCHAR* Separator)
    {
        return Path.Replace(TEXT("/"), Separator, ESearchCase::CaseSensitive);
    }

    TArray<FString> CodexBody(const FString& Command, const TArray<FString>& Args)
    {
        FString Joined;
        for (int32 i = 0; i < Args.Num(); ++i)
        {
            Joined += FString::Printf(TEXT("%s\"%s\""), i == 0 ? TEXT("") : TEXT(", "), *Args[i]);
        }
        return { FString::Printf(TEXT("command = \"%s\""), *Command), FString::Printf(TEXT("args = [%s]"), *Joined),
                 TEXT("startup_timeout_sec = 5.0"), TEXT("tool_timeout_sec = 300.0") };
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentMcpConfigIdenticalEntryIsConfiguredTest,
    "PinWright.infra.agent_mcp_config.IdenticalEntryIsConfigured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAgentMcpConfigIdenticalEntryIsConfiguredTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAgentMcpConfiguratorTest;
    const FProbePaths P = MakeProbePaths();

    TestTrue(TEXT("identical stdio entry matches"), AgentMcpConfigurator::ServerEntryMatches(
        *MakeStdioEntry(P.Python, ProxyArgs(P)), *MakeStdioEntry(P.Python, ProxyArgs(P))));
    TestTrue(TEXT("identical http entry matches"), AgentMcpConfigurator::ServerEntryMatches(
        *MakeHttpEntry(TEXT("http://127.0.0.1:20000/mcp"), TEXT("abc")),
        *MakeHttpEntry(TEXT("http://127.0.0.1:20000/mcp"), TEXT("abc"))));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentMcpConfigAnyFieldDifferenceIsOutdatedTest,
    "PinWright.infra.agent_mcp_config.AnyFieldDifferenceIsOutdated",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAgentMcpConfigAnyFieldDifferenceIsOutdatedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAgentMcpConfiguratorTest;
    const FProbePaths P = MakeProbePaths();
    const TSharedRef<FJsonObject> Desired = MakeStdioEntry(P.Python, ProxyArgs(P));

    auto ExpectOutdated = [this, &Desired](const TCHAR* What, const TSharedRef<FJsonObject>& Existing)
    {
        TestFalse(What, AgentMcpConfigurator::ServerEntryMatches(*Existing, *Desired));
    };

    ExpectOutdated(TEXT("another engine's interpreter"), MakeStdioEntry(P.OtherPython, ProxyArgs(P)));

    TArray<FString> Args = ProxyArgs(P);
    Args[0] = FPaths::GetPath(P.Script) / TEXT("old_proxy.py");
    ExpectOutdated(TEXT("a different proxy script"), MakeStdioEntry(P.Python, Args));

    Args = ProxyArgs(P);
    Args.RemoveAt(0);
    ExpectOutdated(TEXT("the proxy script missing"), MakeStdioEntry(P.Python, Args));

    Args = ProxyArgs(P);
    Args.Append({ TEXT("--url"), TEXT("http://127.0.0.1:20000/mcp") });
    ExpectOutdated(TEXT("an extra leftover arg"), MakeStdioEntry(P.Python, Args));

    Args = ProxyArgs(P);
    Args.Swap(1, 3);
    Args.Swap(2, 4);
    ExpectOutdated(TEXT("the same args in another order"), MakeStdioEntry(P.Python, Args));

    TSharedRef<FJsonObject> WithEnv = MakeStdioEntry(P.Python, ProxyArgs(P));
    WithEnv->SetObjectField(TEXT("env"), MakeShared<FJsonObject>());
    ExpectOutdated(TEXT("a field Install does not write"), WithEnv);

    TSharedRef<FJsonObject> NoType = MakeStdioEntry(P.Python, ProxyArgs(P));
    NoType->RemoveField(TEXT("type"));
    ExpectOutdated(TEXT("a field Install writes missing"), NoType);

    const TSharedRef<FJsonObject> Http = MakeHttpEntry(TEXT("http://127.0.0.1:20000/mcp"), TEXT("abc"));
    TestFalse(TEXT("a different url"), AgentMcpConfigurator::ServerEntryMatches(
        *MakeHttpEntry(TEXT("http://127.0.0.1:20001/mcp"), TEXT("abc")), *Http));
    TestFalse(TEXT("a different bearer header"), AgentMcpConfigurator::ServerEntryMatches(
        *MakeHttpEntry(TEXT("http://127.0.0.1:20000/mcp"), TEXT("xyz")), *Http));
    TSharedRef<FJsonObject> NoHeaders = MakeHttpEntry(TEXT("http://127.0.0.1:20000/mcp"), TEXT("abc"));
    NoHeaders->RemoveField(TEXT("headers"));
    TestFalse(TEXT("a pre-auth entry without headers"), AgentMcpConfigurator::ServerEntryMatches(*NoHeaders, *Http));
    TestFalse(TEXT("an http entry where stdio is wanted"), AgentMcpConfigurator::ServerEntryMatches(*Http, *Desired));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentMcpConfigPathSpellingOnlyDifferenceIsConfiguredTest,
    "PinWright.infra.agent_mcp_config.PathSpellingOnlyDifferenceIsConfigured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAgentMcpConfigPathSpellingOnlyDifferenceIsConfiguredTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAgentMcpConfiguratorTest;
    const FProbePaths P = MakeProbePaths();
    const TSharedRef<FJsonObject> Desired = MakeStdioEntry(P.Python, ProxyArgs(P));

    TArray<FString> Backslashed;
    for (const FString& Arg : ProxyArgs(P))
    {
        Backslashed.Add(Reslash(Arg, TEXT("\\")));
    }
    TestTrue(TEXT("backslashed command and args match"), AgentMcpConfigurator::ServerEntryMatches(
        *MakeStdioEntry(Reslash(P.Python, TEXT("\\")), Backslashed), *Desired));

#if PLATFORM_WINDOWS
    TArray<FString> Upper;
    for (const FString& Arg : ProxyArgs(P))
    {
        Upper.Add(Arg.ToUpper());
    }
    TestTrue(TEXT("case-only differences match on Windows"), AgentMcpConfigurator::ServerEntryMatches(
        *MakeStdioEntry(P.Python.ToUpper(), Upper), *Desired));
#endif
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentMcpConfigCodexBodyComparisonTest,
    "PinWright.infra.agent_mcp_config.CodexBodyComparison",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAgentMcpConfigCodexBodyComparisonTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAgentMcpConfiguratorTest;
    const FProbePaths P = MakeProbePaths();
    const TArray<FString> Desired = CodexBody(P.Python, ProxyArgs(P));

    TArray<FString> Existing = Desired;
    Existing.Append({ TEXT("# kept by hand"), FString() });
    TestTrue(TEXT("identical body plus blank and comment lines matches"),
        AgentMcpConfigurator::CodexTableBodyMatches(Existing, Desired));

    // TOML basic strings escape a backslash as two.
    TestTrue(TEXT("TOML-escaped backslashed command matches"), AgentMcpConfigurator::CodexTableBodyMatches(
        CodexBody(Reslash(P.Python, TEXT("\\\\")), ProxyArgs(P)), Desired));

    TestFalse(TEXT("another engine's interpreter"), AgentMcpConfigurator::CodexTableBodyMatches(
        CodexBody(P.OtherPython, ProxyArgs(P)), Desired));

    TArray<FString> Args = ProxyArgs(P);
    Args.RemoveAt(0);
    TestFalse(TEXT("the proxy script missing"), AgentMcpConfigurator::CodexTableBodyMatches(
        CodexBody(P.Python, Args), Desired));

    Existing = Desired;
    Existing.Add(TEXT("env = { A = \"1\" }"));
    TestFalse(TEXT("a key Install does not write"), AgentMcpConfigurator::CodexTableBodyMatches(Existing, Desired));

    Existing = Desired;
    Existing[3] = TEXT("tool_timeout_sec = 60.0");
    TestFalse(TEXT("a different timeout"), AgentMcpConfigurator::CodexTableBodyMatches(Existing, Desired));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentMcpConfigInstallThenDetectRoundTripsTest,
    "PinWright.infra.agent_mcp_config.InstallThenDetectRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAgentMcpConfigInstallThenDetectRoundTripsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAgentMcpConfiguratorTest;
    const FString Root = FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir() / TEXT("PinWright/TestTemp") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AgentMcpConfigurator::SetProjectDirOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        AgentMcpConfigurator::SetProjectDirOverrideForTests(FString());
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString EndpointUrl = AgentMcpConfigurator::GetEndpointUrl();
    FString Command;
    TArray<FString> Args;
    const bool bStdio = AgentMcpConfigurator::GetStdioProxyLaunch(Command, Args);
    // The one spelling every writer emits verbatim: the interpreter (stdio) or the url (http).
    const FString Needle = bStdio ? Command : EndpointUrl;
    const FString Stale = bStdio ? MakeProbePaths().OtherPython : FString(TEXT("http://127.0.0.1:1/mcp"));

    const struct { EAgentTool Agent; const TCHAR* File; } Agents[] = {
        { EAgentTool::ClaudeCode, TEXT(".mcp.json") },
        { EAgentTool::CodexCli, TEXT(".codex/config.toml") },
        { EAgentTool::Cursor, TEXT(".cursor/mcp.json") },
        { EAgentTool::GeminiCli, TEXT(".gemini/settings.json") },
        { EAgentTool::VsCodeCopilot, TEXT(".vscode/mcp.json") },
    };
    for (const auto& A : Agents)
    {
        const FString FilePath = Root / A.File;
        auto Rewrite = [&FilePath](const FString& From, const FString& To)
        {
            FString Content;
            FFileHelper::LoadFileToString(Content, *FilePath);
            FFileHelper::SaveStringToFile(Content.Replace(*From, *To, ESearchCase::CaseSensitive), *FilePath);
        };

        TestTrue(FString::Printf(TEXT("%s: nothing installed yet"), A.File),
            AgentMcpConfigurator::Detect(A.Agent, EndpointUrl) == EAgentConfigState::NotConfigured);

        // Apply reports false (after writing) when it had to fall back to direct HTTP; the
        // round trip holds either way, so only the file and the detection are asserted.
        FString Message;
        AgentMcpConfigurator::Apply(A.Agent, EndpointUrl, Message);
        FString Content;
        FFileHelper::LoadFileToString(Content, *FilePath);
        if (!TestTrue(FString::Printf(TEXT("%s: written with the launch spelled verbatim"), A.File),
                Content.Contains(Needle, ESearchCase::CaseSensitive)))
        {
            continue;
        }
        TestTrue(FString::Printf(TEXT("%s: fresh install is Configured"), A.File),
            AgentMcpConfigurator::Detect(A.Agent, EndpointUrl) == EAgentConfigState::Configured);

        if (bStdio)
        {
            // JSON and TOML basic strings both escape a backslash as two.
            Rewrite(Needle, Reslash(Needle, TEXT("\\\\")));
            TestTrue(FString::Printf(TEXT("%s: backslashed interpreter is still Configured"), A.File),
                AgentMcpConfigurator::Detect(A.Agent, EndpointUrl) == EAgentConfigState::Configured);
            Rewrite(Reslash(Needle, TEXT("\\\\")), Needle);
        }

        Rewrite(Needle, Stale);
        TestTrue(FString::Printf(TEXT("%s: stale %s is Outdated"), A.File, bStdio ? TEXT("interpreter") : TEXT("url")),
            AgentMcpConfigurator::Detect(A.Agent, EndpointUrl) == EAgentConfigState::Outdated);
    }
    return true;
}

#endif
