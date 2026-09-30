// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Detection result for an agent's "pinwright" MCP server entry.
enum class EAgentConfigState : uint8
{
    Configured,     // entry exists and equals the entry Install would write now
    Outdated,       // entry exists but differs from it in any field
    NotConfigured   // no entry for this server
};

// AI agent clients the setup screen can configure.
enum class EAgentTool : uint8
{
    ClaudeCode,
    CodexCli,
    Cursor,
    GeminiCli,
    VsCodeCopilot
};

// Upserts the "pinwright" MCP server entry into per-agent config files in the
// host project (FPaths::ProjectDir()). All file writes are merges: existing content is
// preserved and only our entry is added or replaced.
namespace AgentMcpConfigurator
{
    // Endpoint URL ("http://127.0.0.1:<port>/mcp") using the live bound HTTP port;
    // falls back to the configured HttpPort when the transport is not active.
    FString GetEndpointUrl();

    // Stdio proxy launch used in generated configs (command + args). Returns false when
    // the bundled Python or the proxy script can't be resolved (or the proxy is disabled
    // in settings) - agents must then connect over direct HTTP.
    bool GetStdioProxyLaunch(FString& OutCommand, TArray<FString>& OutArgs);

    EAgentConfigState Detect(EAgentTool Agent, const FString& EndpointUrl);

    // Returns true on success. OutMessage is user-facing and names the file written.
    bool Apply(EAgentTool Agent, const FString& EndpointUrl, FString& OutMessage);

    // Tests only: read and write the agent config files under Dir instead of the project
    // directory; an empty Dir restores the default.
    void SetProjectDirOverrideForTests(const FString& Dir);

    // Detect's comparators, exposed for tests. True when Existing has exactly Desired's
    // fields with equal values; "command" and each "args" element compare as paths (slash
    // direction and, on Windows, case do not matter).
    bool ServerEntryMatches(const FJsonObject& Existing, const FJsonObject& Desired);
    // The same comparison over two Codex TOML table bodies (the lines after the header).
    bool CodexTableBodyMatches(const TArray<FString>& ExistingBody, const TArray<FString>& DesiredBody);
}
