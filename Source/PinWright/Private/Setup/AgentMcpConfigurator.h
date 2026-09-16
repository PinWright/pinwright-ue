// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Detection result for an agent's "pinwright" MCP server entry.
enum class EAgentConfigState : uint8
{
    Configured,     // entry exists and its URL matches the current endpoint
    Outdated,       // entry exists but its URL differs from the current endpoint
    NotConfigured,  // no entry for this server
    Unknown         // cannot detect (user-global config, e.g. Cursor)
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
// preserved and only our entry is added or replaced. Cursor is the exception - it has
// no project-local config file and is driven through an install deeplink instead.
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

    // Returns true on success. OutMessage is user-facing and names the file written
    // (or describes the deeplink action for Cursor).
    bool Apply(EAgentTool Agent, const FString& EndpointUrl, FString& OutMessage);
}
