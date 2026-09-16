// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Setup/AgentMcpConfigurator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Interfaces/IPluginManager.h"
#include "IPythonScriptPlugin.h"
#include "PinWrightSettings.h"
#include "PinWrightSubsystem.h"
#include "Utils/AtomicFileWriter.h"
#include "Utils/GatewayAuthToken.h"
#include "Utils/GatewayPortFile.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Named (not anonymous) namespace: unity builds merge TUs and anonymous-namespace
// helpers with common names collide across files.
namespace AgentMcpConfiguratorPrivate
{

constexpr const TCHAR* ServerName = TEXT("pinwright");

// --- Server launch resolution: bundled-Python stdio proxy, or direct-HTTP fallback ---
//
// The agent configs default to launching a tiny stdio proxy (Content/Python/mcp_proxy.py)
// on the engine's bundled Python, in front of the editor's HTTP endpoint. The proxy keeps
// the client's MCP connection alive across editor launch/kill/relaunch. If the bundled
// Python or the script can't be resolved (or the setting is off), we write the legacy
// direct-HTTP config instead, so setup never breaks.

struct FServerLaunch
{
    bool bStdio = false;   // true: launch the stdio proxy; false: direct HTTP
    FString Url;           // editor MCP endpoint (always set)
    FString Command;       // stdio: absolute path to the python interpreter
    TArray<FString> Args;  // stdio: { proxy.py, ["--token-file", tok,] "--port-file", pf }
    FString TokenFilePath; // stdio: absolute, forward-slashed; empty = no token plumbing
    FString PortFilePath;  // stdio: absolute, forward-slashed
    FString TokenValue;    // http: literal token; empty = no token plumbing
    // Non-empty only when the stdio proxy was requested (bUseStdioProxy on) but a prerequisite
    // could not be resolved, so the legacy direct-HTTP config was written instead. Describes
    // what was missing; surfaced to the user as an error after the fallback write.
    FString FallbackReason;
};

// Absolute path to the engine's Python interpreter, or empty if absent. The path comes from
// the engine's Python plugin (IPythonScriptPlugin), so it also honors custom UE_PYTHON_DIR
// builds; a disabled/absent Python plugin returns empty, which lands in the existing
// direct-HTTP fallback (FallbackReason machinery).
FString ResolveEnginePython()
{
    if (IPythonScriptPlugin* Python = IPythonScriptPlugin::Get())
    {
        // The reported path can be engine-relative; make it absolute BEFORE FileExists,
        // which otherwise resolves against the editor's CWD and spuriously fails.
        const FString Candidate = FPaths::ConvertRelativePathToFull(Python->GetInterpreterExecutablePath());
        if (!Candidate.IsEmpty() && FPaths::FileExists(Candidate))
        {
            return Candidate;
        }
    }
    return FString();
}

// Absolute path to the shipped proxy script, or empty if not found.
FString ResolveProxyScript()
{
    if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright")))
    {
        // GetBaseDir() is relative when the plugin lives under the project; make it absolute
        // BEFORE FileExists (same CWD pitfall as ResolveEnginePython).
        const FString Path =
            FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Content/Python/mcp_proxy.py"));
        if (FPaths::FileExists(Path))
        {
            return Path;
        }
    }
    return FString();
}

FServerLaunch ResolveServerLaunch(const FString& EndpointUrl)
{
    FServerLaunch Launch;
    Launch.Url = EndpointUrl;

    const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();
    const bool bWantProxy = (Settings == nullptr) || Settings->bUseStdioProxy;

    // Resolve the bearer token only when auth is enabled. When auth is off (or token creation
    // failed) both fields stay empty and no token plumbing is written into any agent config.
    if (Settings && Settings->bRequireAuthToken)
    {
        Launch.TokenValue = GatewayAuthToken::GetOrCreateToken();
        if (!Launch.TokenValue.IsEmpty())
        {
            Launch.TokenFilePath = GatewayAuthToken::GetTokenFilePath();
            Launch.TokenFilePath.ReplaceInline(TEXT("\\"), TEXT("/"));   // forward-slash like Command/Args
        }
    }

    FString Python, Script;
    if (bWantProxy)
    {
        Python = ResolveEnginePython();
        Script = ResolveProxyScript();
        if (!Python.IsEmpty() && !Script.IsEmpty())
        {
            // Forward slashes: accepted by the OS process APIs on all platforms and avoid
            // backslash-escaping surprises in the generated JSON/TOML.
            Python.ReplaceInline(TEXT("\\"), TEXT("/"));
            Script.ReplaceInline(TEXT("\\"), TEXT("/"));
            Launch.bStdio = true;
            Launch.Command = Python;
            Launch.Args = { Script };
            if (!Launch.TokenFilePath.IsEmpty())
            {
                Launch.Args.Append({ TEXT("--token-file"), Launch.TokenFilePath });
            }
            // No --url is baked: the proxy re-reads the port file before every call, and a
            // generated config can only exist after onboarding ran inside a live editor,
            // which had already published the file. (--url stays supported by the proxy for
            // hand-written configs.)
            Launch.PortFilePath = GatewayPortFile::GetPortFilePath();
            Launch.PortFilePath.ReplaceInline(TEXT("\\"), TEXT("/"));   // forward-slash like Command/Args
            Launch.Args.Append({ TEXT("--port-file"), Launch.PortFilePath });
        }
        else if (Python.IsEmpty() && Script.IsEmpty())
        {
            Launch.FallbackReason = TEXT("neither the engine's bundled Python interpreter nor the "
                                         "proxy script (Content/Python/mcp_proxy.py) could be found");
        }
        else if (Python.IsEmpty())
        {
            Launch.FallbackReason = TEXT("the engine's bundled Python interpreter could not be found");
        }
        else
        {
            Launch.FallbackReason =
                TEXT("the proxy script (Content/Python/mcp_proxy.py) could not be found");
        }
    }

    // Diagnostic: at Warning when the proxy was wanted but we fell back to HTTP (the case worth
    // seeing), otherwise Verbose so routine Detect refreshes don't spam the log.
    if (!Launch.FallbackReason.IsEmpty())
    {
        UE_LOG(LogPinWrightSubsystem, Warning,
               TEXT("[AgentMcp] stdio proxy requested but falling back to direct HTTP: %s "
                    "(python='%s' script='%s')"),
               *Launch.FallbackReason, *Python, *Script);
    }
    else
    {
        UE_LOG(LogPinWrightSubsystem, Verbose,
               TEXT("[AgentMcp] bUseStdioProxy=%d python='%s' script='%s' -> stdio=%d"),
               bWantProxy ? 1 : 0, *Python, *Script, Launch.bStdio ? 1 : 0);
    }

    return Launch;  // bStdio == false -> direct-HTTP fallback
}

// --- JSON-config agents (Claude Code, Gemini CLI, VS Code Copilot) ---

struct FJsonAgentFile
{
    const TCHAR* RelativePath;  // relative to the project root
    const TCHAR* TopLevelKey;   // "mcpServers" or "servers"
    const TCHAR* UrlKey;        // "url" or "httpUrl" (Gemini needs httpUrl; "url" would mean SSE)
    bool bWriteTypeKey;         // whether the entry carries "type": "http"
};

FJsonAgentFile GetJsonSpec(EAgentTool Agent)
{
    switch (Agent)
    {
    case EAgentTool::GeminiCli:
        return { TEXT(".gemini/settings.json"), TEXT("mcpServers"), TEXT("httpUrl"), false };
    case EAgentTool::VsCodeCopilot:
        return { TEXT(".vscode/mcp.json"), TEXT("servers"), TEXT("url"), true };
    case EAgentTool::ClaudeCode:
    default:
        return { TEXT(".mcp.json"), TEXT("mcpServers"), TEXT("url"), true };
    }
}

bool LoadJsonRoot(const FString& FilePath, TSharedPtr<FJsonObject>& OutRoot)
{
    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *FilePath))
    {
        return false;
    }
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Content);
    return FJsonSerializer::Deserialize(Reader, OutRoot) && OutRoot.IsValid();
}

EAgentConfigState DetectJsonAgent(const FJsonAgentFile& Spec, const FServerLaunch& Launch)
{
    const FString FilePath = FPaths::ProjectDir() / Spec.RelativePath;
    TSharedPtr<FJsonObject> Root;
    if (!FPaths::FileExists(FilePath) || !LoadJsonRoot(FilePath, Root))
    {
        return EAgentConfigState::NotConfigured;
    }

    const TSharedPtr<FJsonObject>* Servers = nullptr;
    if (!Root->TryGetObjectField(FString(Spec.TopLevelKey), Servers))
    {
        return EAgentConfigState::NotConfigured;
    }

    const TSharedPtr<FJsonObject>* Entry = nullptr;
    if (!(*Servers)->TryGetObjectField(FString(ServerName), Entry))
    {
        return EAgentConfigState::NotConfigured;
    }

    if (Launch.bStdio)
    {
        // Configured when the proxy args carry the port-file path and, when auth is on, the
        // token-file path. The proxy follows the live port from the port file, so any leftover
        // --url from an older config is harmless and ignored here. A legacy config missing
        // --port-file reads Outdated so the Install button becomes the migration path
        // (same precedent as the token-file migration).
        const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
        if ((*Entry)->TryGetArrayField(TEXT("args"), ArgsArray))
        {
            bool bHasPortFile = Launch.PortFilePath.IsEmpty();
            bool bHasToken = Launch.TokenFilePath.IsEmpty();
            for (const TSharedPtr<FJsonValue>& Arg : *ArgsArray)
            {
                if (!Arg.IsValid())
                {
                    continue;
                }
                const FString ArgStr = Arg->AsString();
                if (!Launch.PortFilePath.IsEmpty() && ArgStr == Launch.PortFilePath)
                {
                    bHasPortFile = true;
                }
                if (!Launch.TokenFilePath.IsEmpty() && ArgStr == Launch.TokenFilePath)
                {
                    bHasToken = true;
                }
            }
            if (bHasPortFile && bHasToken)
            {
                return EAgentConfigState::Configured;
            }
        }
        return EAgentConfigState::Outdated;
    }

    FString Url;
    if ((*Entry)->TryGetStringField(FString(Spec.UrlKey), Url) && Url == Launch.Url)
    {
        if (Launch.TokenValue.IsEmpty())
        {
            return EAgentConfigState::Configured;
        }
        // Auth on: the entry must also carry the matching bearer header, else it's a pre-auth
        // config that Install should migrate.
        const TSharedPtr<FJsonObject>* HeadersObj = nullptr;
        FString AuthHeader;
        if ((*Entry)->TryGetObjectField(TEXT("headers"), HeadersObj)
            && (*HeadersObj)->TryGetStringField(TEXT("Authorization"), AuthHeader)
            && AuthHeader == TEXT("Bearer ") + Launch.TokenValue)
        {
            return EAgentConfigState::Configured;
        }
    }
    return EAgentConfigState::Outdated;
}

bool ApplyJsonAgent(const FJsonAgentFile& Spec, const FServerLaunch& Launch, FString& OutMessage)
{
    const FString FilePath = FPaths::ProjectDir() / Spec.RelativePath;

    TSharedPtr<FJsonObject> Root;
    if (FPaths::FileExists(FilePath))
    {
        if (!LoadJsonRoot(FilePath, Root))
        {
            OutMessage = FString::Printf(
                TEXT("%s exists but could not be parsed as JSON; fix or remove it first."), Spec.RelativePath);
            return false;
        }
    }
    else
    {
        Root = MakeShared<FJsonObject>();
    }

    TSharedPtr<FJsonObject> Servers;
    if (Root->HasTypedField<EJson::Object>(FString(Spec.TopLevelKey)))
    {
        Servers = Root->GetObjectField(FString(Spec.TopLevelKey));
    }
    else
    {
        Servers = MakeShared<FJsonObject>();
        Root->SetObjectField(FString(Spec.TopLevelKey), Servers);
    }

    const TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    if (Launch.bStdio)
    {
        Entry->SetStringField(TEXT("type"), TEXT("stdio"));
        Entry->SetStringField(TEXT("command"), Launch.Command);
        TArray<TSharedPtr<FJsonValue>> ArgsArray;
        for (const FString& Arg : Launch.Args)
        {
            ArgsArray.Add(MakeShared<FJsonValueString>(Arg));
        }
        Entry->SetArrayField(TEXT("args"), ArgsArray);
    }
    else
    {
        if (Spec.bWriteTypeKey)
        {
            Entry->SetStringField(FString(TEXT("type")), TEXT("http"));
        }
        Entry->SetStringField(FString(Spec.UrlKey), Launch.Url);
        if (!Launch.TokenValue.IsEmpty())
        {
            TSharedPtr<FJsonObject> HeadersObj = MakeShared<FJsonObject>();
            HeadersObj->SetStringField(TEXT("Authorization"), TEXT("Bearer ") + Launch.TokenValue);
            Entry->SetObjectField(TEXT("headers"), HeadersObj);
        }
    }
    Servers->SetObjectField(FString(ServerName), Entry);

    FString Output;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
    FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

    if (!AtomicFileWriter::WriteUtf8(FilePath, Output,
            AtomicFileWriter::EExistingFilePolicy::ReplaceExisting).IsSuccess())
    {
        OutMessage = FString::Printf(TEXT("Failed to write %s."), Spec.RelativePath);
        return false;
    }

    OutMessage = FString::Printf(TEXT("MCP server '%s' written to %s."), ServerName, Spec.RelativePath);
    return true;
}

// --- Codex CLI (line-based TOML merge; UE has no TOML parser) ---

constexpr const TCHAR* CodexRelativePath = TEXT(".codex/config.toml");
constexpr const TCHAR* CodexTableHeader = TEXT("[mcp_servers.pinwright]");

bool LoadLines(const FString& FilePath, TArray<FString>& OutLines)
{
    FString Content;
    if (!FFileHelper::LoadFileToString(Content, *FilePath))
    {
        return false;
    }
    Content.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
    Content.ParseIntoArray(OutLines, TEXT("\n"), /*InCullEmpty=*/false);
    return true;
}

// Locates the [mcp_servers.pinwright] table. OutEndIdx is the exclusive end
// of the table body (next "[" header line or EOF).
void FindCodexTableRange(const TArray<FString>& Lines, int32& OutHeaderIdx, int32& OutEndIdx)
{
    OutHeaderIdx = INDEX_NONE;
    OutEndIdx = Lines.Num();
    for (int32 i = 0; i < Lines.Num(); ++i)
    {
        if (Lines[i].TrimStartAndEnd() == CodexTableHeader)
        {
            OutHeaderIdx = i;
            for (int32 j = i + 1; j < Lines.Num(); ++j)
            {
                if (Lines[j].TrimStartAndEnd().StartsWith(TEXT("[")))
                {
                    OutEndIdx = j;
                    break;
                }
            }
            return;
        }
    }
}

bool IsTomlKeyLine(const FString& Line, const TCHAR* Key)
{
    const FString Trimmed = Line.TrimStartAndEnd();
    if (!Trimmed.StartsWith(Key))
    {
        return false;
    }
    return Trimmed.RightChop(FCString::Strlen(Key)).TrimStart().StartsWith(TEXT("="));
}

FString ExtractTomlStringValue(const FString& Line)
{
    int32 FirstQuote = INDEX_NONE;
    if (!Line.FindChar(TEXT('"'), FirstQuote))
    {
        return FString();
    }
    const int32 SecondQuote =
        Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstQuote + 1);
    if (SecondQuote == INDEX_NONE)
    {
        return FString();
    }
    return Line.Mid(FirstQuote + 1, SecondQuote - FirstQuote - 1);
}

EAgentConfigState DetectCodex(const FServerLaunch& Launch)
{
    const FString FilePath = FPaths::ProjectDir() / CodexRelativePath;
    TArray<FString> Lines;
    if (!FPaths::FileExists(FilePath) || !LoadLines(FilePath, Lines))
    {
        return EAgentConfigState::NotConfigured;
    }

    int32 HeaderIdx, EndIdx;
    FindCodexTableRange(Lines, HeaderIdx, EndIdx);
    if (HeaderIdx == INDEX_NONE)
    {
        return EAgentConfigState::NotConfigured;
    }

    for (int32 i = HeaderIdx + 1; i < EndIdx; ++i)
    {
        if (Launch.bStdio)
        {
            // The args line must carry the port-file path (the proxy follows the live port from
            // it; any leftover --url from an older config is harmless) and, when auth is on,
            // the token-file path; a legacy config missing either reads Outdated so Install
            // migrates it.
            if (IsTomlKeyLine(Lines[i], TEXT("args"))
                && (Launch.PortFilePath.IsEmpty() || Lines[i].Contains(Launch.PortFilePath))
                && (Launch.TokenFilePath.IsEmpty() || Lines[i].Contains(Launch.TokenFilePath)))
            {
                return EAgentConfigState::Configured;
            }
        }
        else if (IsTomlKeyLine(Lines[i], TEXT("url")))
        {
            return ExtractTomlStringValue(Lines[i]) == Launch.Url
                       ? EAgentConfigState::Configured
                       : EAgentConfigState::Outdated;
        }
    }
    // Table exists but doesn't match the desired transport - offer an update.
    return EAgentConfigState::Outdated;
}

bool ApplyCodex(const FServerLaunch& Launch, FString& OutMessage)
{
    const FString FilePath = FPaths::ProjectDir() / CodexRelativePath;

    // Build the desired table body (everything after the [table] header).
    TArray<FString> Body;
    if (Launch.bStdio)
    {
        Body.Add(FString::Printf(TEXT("command = \"%s\""), *Launch.Command));
        FString ArgsJoined;
        for (int32 i = 0; i < Launch.Args.Num(); ++i)
        {
            ArgsJoined += FString::Printf(TEXT("%s\"%s\""), i == 0 ? TEXT("") : TEXT(", "), *Launch.Args[i]);
        }
        Body.Add(FString::Printf(TEXT("args = [%s]"), *ArgsJoined));
    }
    else
    {
        Body.Add(FString::Printf(TEXT("url = \"%s\""), *Launch.Url));
        // TODO: Codex TOML has no confirmed per-server header field for the http fallback; the stdio proxy path (default) carries the token via --token-file.
    }
    Body.Add(TEXT("startup_timeout_sec = 5.0"));
    Body.Add(TEXT("tool_timeout_sec = 300.0"));

    TArray<FString> Lines;
    if (FPaths::FileExists(FilePath) && !LoadLines(FilePath, Lines))
    {
        OutMessage = FString::Printf(TEXT("Could not read %s."), CodexRelativePath);
        return false;
    }

    int32 HeaderIdx, EndIdx;
    FindCodexTableRange(Lines, HeaderIdx, EndIdx);

    if (HeaderIdx == INDEX_NONE)
    {
        if (Lines.Num() > 0 && !Lines.Last().TrimStartAndEnd().IsEmpty())
        {
            Lines.Add(FString());
        }
        Lines.Add(CodexTableHeader);
        Lines.Append(Body);
    }
    else
    {
        // Replace the existing table body [HeaderIdx+1, EndIdx) with the rebuilt body
        // (the table is plugin-owned, so we don't try to preserve custom keys in it).
        TArray<FString> Rebuilt;
        for (int32 i = 0; i <= HeaderIdx; ++i) { Rebuilt.Add(Lines[i]); }
        Rebuilt.Append(Body);
        for (int32 i = EndIdx; i < Lines.Num(); ++i) { Rebuilt.Add(Lines[i]); }
        Lines = MoveTemp(Rebuilt);
    }

    FString Output = FString::Join(Lines, TEXT("\n"));
    if (!Output.EndsWith(TEXT("\n")))
    {
        Output += TEXT("\n");
    }

    if (!AtomicFileWriter::WriteUtf8(FilePath, Output,
            AtomicFileWriter::EExistingFilePolicy::ReplaceExisting).IsSuccess())
    {
        OutMessage = FString::Printf(TEXT("Failed to write %s."), CodexRelativePath);
        return false;
    }

    OutMessage = FString::Printf(TEXT("MCP server '%s' written to %s."), ServerName, CodexRelativePath);
    return true;
}

// --- Cursor (install deeplink into the user-global Cursor config) ---

bool ApplyCursor(const FServerLaunch& Launch, FString& OutMessage)
{
    // Cursor's deeplink takes the server config JSON base64-encoded.
    TSharedPtr<FJsonObject> Config = MakeShared<FJsonObject>();
    if (Launch.bStdio)
    {
        Config->SetStringField(TEXT("type"), TEXT("stdio"));
        Config->SetStringField(TEXT("command"), Launch.Command);
        TArray<TSharedPtr<FJsonValue>> ArgsArray;
        for (const FString& Arg : Launch.Args)
        {
            ArgsArray.Add(MakeShared<FJsonValueString>(Arg));
        }
        Config->SetArrayField(TEXT("args"), ArgsArray);
    }
    else
    {
        Config->SetStringField(TEXT("url"), Launch.Url);
        if (!Launch.TokenValue.IsEmpty())
        {
            TSharedPtr<FJsonObject> HeadersObj = MakeShared<FJsonObject>();
            HeadersObj->SetStringField(TEXT("Authorization"), TEXT("Bearer ") + Launch.TokenValue);
            Config->SetObjectField(TEXT("headers"), HeadersObj);
        }
    }

    FString ConfigJson;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ConfigJson);
    FJsonSerializer::Serialize(Config.ToSharedRef(), Writer);

    const FTCHARToUTF8 Utf8(*ConfigJson);
    const TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());

    FString Encoded = FBase64::Encode(Bytes);
    Encoded.ReplaceInline(TEXT("+"), TEXT("%2B"));
    Encoded.ReplaceInline(TEXT("/"), TEXT("%2F"));
    Encoded.ReplaceInline(TEXT("="), TEXT("%3D"));

    const FString DeepLink = FString::Printf(
        TEXT("cursor://anysphere.cursor-deeplink/mcp/install?name=%s&config=%s"),
        ServerName, *Encoded);

    FString Error;
    FPlatformProcess::LaunchURL(*DeepLink, nullptr, &Error);
    if (!Error.IsEmpty())
    {
        OutMessage = TEXT("Could not open the Cursor install deeplink (is Cursor installed?). "
                          "Use the manual setup instructions instead.");
        return false;
    }

    OutMessage = TEXT("Cursor install deeplink opened - confirm the install dialog in Cursor.");
    return true;
}

} // namespace AgentMcpConfiguratorPrivate

FString AgentMcpConfigurator::GetEndpointUrl()
{
    int32 Port = 0;
    if (GEditor)
    {
        if (const UPinWrightSubsystem* Subsystem =
                GEditor->GetEditorSubsystem<UPinWrightSubsystem>())
        {
            Port = Subsystem->GetBoundHttpPort();
        }
    }
    if (Port <= 0)
    {
        // No live bound port yet - resolve the per-project derived port (when auto-derive is on)
        // so the baked URL matches what the server will actually listen on.
        Port = UPinWrightSettings::ResolveHttpPort(GetDefault<UPinWrightSettings>());
    }
    return FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), Port);
}

bool AgentMcpConfigurator::GetStdioProxyLaunch(FString& OutCommand, TArray<FString>& OutArgs)
{
    using namespace AgentMcpConfiguratorPrivate;
    const FServerLaunch Launch = ResolveServerLaunch(GetEndpointUrl());
    if (!Launch.bStdio)
    {
        return false;
    }
    OutCommand = Launch.Command;
    OutArgs = Launch.Args;
    return true;
}

EAgentConfigState AgentMcpConfigurator::Detect(EAgentTool Agent, const FString& EndpointUrl)
{
    using namespace AgentMcpConfiguratorPrivate;
    const FServerLaunch Launch = ResolveServerLaunch(EndpointUrl);
    switch (Agent)
    {
    case EAgentTool::ClaudeCode:
    case EAgentTool::GeminiCli:
    case EAgentTool::VsCodeCopilot:
        return DetectJsonAgent(GetJsonSpec(Agent), Launch);
    case EAgentTool::CodexCli:
        return DetectCodex(Launch);
    case EAgentTool::Cursor:
    default:
        return EAgentConfigState::Unknown;
    }
}

bool AgentMcpConfigurator::Apply(EAgentTool Agent, const FString& EndpointUrl, FString& OutMessage)
{
    using namespace AgentMcpConfiguratorPrivate;
    const FServerLaunch Launch = ResolveServerLaunch(EndpointUrl);

    bool bWritten;
    switch (Agent)
    {
    case EAgentTool::ClaudeCode:
    case EAgentTool::GeminiCli:
    case EAgentTool::VsCodeCopilot:
        bWritten = ApplyJsonAgent(GetJsonSpec(Agent), Launch, OutMessage);
        break;
    case EAgentTool::CodexCli:
        bWritten = ApplyCodex(Launch, OutMessage);
        break;
    case EAgentTool::Cursor:
        bWritten = ApplyCursor(Launch, OutMessage);
        break;
    default:
        OutMessage = TEXT("Unknown agent.");
        return false;
    }

    // Visible (Display) once-per-click record of exactly what the writer chose - the definitive
    // signal for why a host got stdio vs. the legacy HTTP config. Apply runs only on the Install
    // button, so this never spams (unlike ResolveServerLaunch, which Detect also calls on refresh).
    UE_LOG(LogPinWrightSubsystem, Display,
           TEXT("[AgentMcp] Apply agent=%d transport=%s written=%d url=%s%s"),
           (int32)Agent, Launch.bStdio ? TEXT("stdio-proxy") : TEXT("direct-http"),
           bWritten ? 1 : 0, *Launch.Url,
           Launch.FallbackReason.IsEmpty()
               ? TEXT("")
               : *FString::Printf(TEXT(" fallback=[%s]"), *Launch.FallbackReason));

    // The stdio proxy was requested but a prerequisite was missing, so the legacy direct-HTTP
    // config was written instead. It works while the editor is running, but the agent's MCP
    // connection drops on every editor restart. Report it as an error so the user sees the
    // degraded result rather than a silent "success".
    if (bWritten && !Launch.FallbackReason.IsEmpty())
    {
        OutMessage = FString::Printf(
            TEXT("Wrote the legacy direct-HTTP MCP config because %s. The agent will lose its MCP "
                 "connection whenever the editor restarts. Fix the engine's bundled Python install "
                 "(or turn off bUseStdioProxy to silence this), then configure again."),
            *Launch.FallbackReason);
        return false;
    }

    return bWritten;
}
