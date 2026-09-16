// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared PIE world-selector helpers for the Editor handler cluster.
// editor.console_command uses Parse + GatherPieContexts + ResolveSelector to route a
// console command into a specific PIE world ("server" / "client" / "client:N" / "pie:N"),
// and editor.pie_status uses GatherPieContexts + the classification helpers to enumerate
// live PIE contexts. Parse / ClassifyNetMode / ResolveSelector are pure (no engine
// globals), so the selector grammar and server/client classification are unit-testable
// without a running PIE session; only GatherPieContexts touches GEngine. Named-namespace
// inline functions, so the header is Unity-merge safe (same pattern as PieControlUtils.h).
#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"

namespace PieWorldSelector
{

// What a parsed `world` selector string asks for.
enum class ESelectorKind : uint8
{
    Editor,       // "editor" or empty — the editor world (not a PIE target)
    Server,       // "server" — first PIE context whose world has authority
    Client,       // "client" / "client:N" — Index holds the 1-based client ordinal
    PieInstance,  // "pie:N" — Index holds the raw FWorldContext::PIEInstance
    Invalid       // unparseable — Error holds the caller-facing message
};

struct FParsedSelector
{
    ESelectorKind Kind = ESelectorKind::Editor;
    int32 Index = 0;
    FString Error;
};

// One live PIE world context, reduced to what selection/classification needs.
// World stays null in unit tests — ResolveSelector only reads PieInstance/NetMode.
struct FPieContextInfo
{
    int32 PieInstance = INDEX_NONE;
    ENetMode NetMode = NM_Standalone;
    UWorld* World = nullptr;
};

// Strict digit-only parse (no sign, no decimal, no trailing junk — FCString::IsNumeric
// is too permissive for selector indices). Returns false on empty/non-digit input.
inline bool ParseNonNegativeInt(const FString& Text, int32& OutValue)
{
    if (Text.IsEmpty())
    {
        return false;
    }
    for (int32 CharIndex = 0; CharIndex < Text.Len(); ++CharIndex)
    {
        if (!FChar::IsDigit(Text[CharIndex]))
        {
            return false;
        }
    }
    OutValue = FCString::Atoi(*Text);
    return true;
}

// Parses a `world` selector string. Case-insensitive, whitespace-trimmed.
// Grammar: "" | "editor" | "server" | "client" | "client:N" (N >= 1, 1-based) |
// "pie:N" (N >= 0, raw PIEInstance). Anything else yields Kind == Invalid with a
// caller-facing Error naming the valid forms.
inline FParsedSelector Parse(const FString& Selector)
{
    FParsedSelector Result;

    const FString Trimmed = Selector.TrimStartAndEnd();
    if (Trimmed.IsEmpty() || Trimmed.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
    {
        Result.Kind = ESelectorKind::Editor;
        return Result;
    }
    if (Trimmed.Equals(TEXT("server"), ESearchCase::IgnoreCase))
    {
        Result.Kind = ESelectorKind::Server;
        return Result;
    }
    if (Trimmed.Equals(TEXT("client"), ESearchCase::IgnoreCase))
    {
        Result.Kind = ESelectorKind::Client;
        Result.Index = 1;
        return Result;
    }

    FString Prefix;
    FString IndexText;
    if (Trimmed.Split(TEXT(":"), &Prefix, &IndexText))
    {
        Prefix.TrimStartAndEndInline();
        IndexText.TrimStartAndEndInline();
        int32 ParsedIndex = 0;
        if (Prefix.Equals(TEXT("client"), ESearchCase::IgnoreCase))
        {
            if (ParseNonNegativeInt(IndexText, ParsedIndex) && ParsedIndex >= 1)
            {
                Result.Kind = ESelectorKind::Client;
                Result.Index = ParsedIndex;
                return Result;
            }
            Result.Kind = ESelectorKind::Invalid;
            Result.Error = FString::Printf(
                TEXT("Invalid client selector '%s': 'client:N' needs a 1-based client number (client:1 is the first client)."),
                *Selector);
            return Result;
        }
        if (Prefix.Equals(TEXT("pie"), ESearchCase::IgnoreCase))
        {
            if (ParseNonNegativeInt(IndexText, ParsedIndex))
            {
                Result.Kind = ESelectorKind::PieInstance;
                Result.Index = ParsedIndex;
                return Result;
            }
            Result.Kind = ESelectorKind::Invalid;
            Result.Error = FString::Printf(
                TEXT("Invalid PIE selector '%s': 'pie:N' needs a non-negative PIEInstance number (e.g. pie:0)."),
                *Selector);
            return Result;
        }
    }

    Result.Kind = ESelectorKind::Invalid;
    Result.Error = FString::Printf(
        TEXT("Unknown world selector '%s'. Valid: 'editor' (default), 'server', 'client', 'client:N' (N-th client, 1-based), 'pie:N' (raw PIEInstance)."),
        *Selector);
    return Result;
}

// Classifies a PIE world by its net mode for the `kind` field / selector matching.
// UWorld::GetNetMode already resolves PIE dedicated servers (FWorldContext::RunAsDedicated)
// to NM_DedicatedServer, so net mode alone is sufficient.
inline const TCHAR* ClassifyNetMode(ENetMode NetMode)
{
    switch (NetMode)
    {
    case NM_DedicatedServer:
    case NM_ListenServer:
        return TEXT("server");
    case NM_Client:
        return TEXT("client");
    default:
        return TEXT("standalone");
    }
}

// Exact ENetMode enum name for diagnostics (the `netMode` field).
inline const TCHAR* NetModeToString(ENetMode NetMode)
{
    switch (NetMode)
    {
    case NM_Standalone:
        return TEXT("Standalone");
    case NM_DedicatedServer:
        return TEXT("DedicatedServer");
    case NM_ListenServer:
        return TEXT("ListenServer");
    case NM_Client:
        return TEXT("Client");
    default:
        return TEXT("Unknown");
    }
}

// Snapshots every live PIE world context (world resolved and non-null), in
// GEngine->GetWorldContexts() order. Empty when PIE is not running (or no GEngine).
inline TArray<FPieContextInfo> GatherPieContexts()
{
    TArray<FPieContextInfo> Contexts;
    if (!GEngine)
    {
        return Contexts;
    }
    for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
    {
        if (WorldContext.WorldType != EWorldType::PIE)
        {
            continue;
        }
        UWorld* World = WorldContext.World();
        if (!World)
        {
            continue;
        }
        FPieContextInfo Info;
        Info.PieInstance = WorldContext.PIEInstance;
        Info.NetMode = World->GetNetMode();
        Info.World = World;
        Contexts.Add(Info);
    }
    return Contexts;
}

// Resolves a parsed PIE-targeting selector against gathered contexts. Returns the index
// into Contexts, or INDEX_NONE when nothing matches. Editor/Invalid selectors are the
// caller's responsibility (handled before resolution) and always return INDEX_NONE here.
// "server" matches the first context whose world has authority (listen or dedicated
// server — and the single instance of a standalone PIE session, which is its own
// authority). "client"/"client:N" counts NM_Client contexts in gather order, 1-based.
inline int32 ResolveSelector(const FParsedSelector& Selector, const TArray<FPieContextInfo>& Contexts)
{
    switch (Selector.Kind)
    {
    case ESelectorKind::Server:
        for (int32 Index = 0; Index < Contexts.Num(); ++Index)
        {
            if (Contexts[Index].NetMode != NM_Client)
            {
                return Index;
            }
        }
        return INDEX_NONE;

    case ESelectorKind::Client:
    {
        int32 ClientOrdinal = 0;
        for (int32 Index = 0; Index < Contexts.Num(); ++Index)
        {
            if (Contexts[Index].NetMode == NM_Client && ++ClientOrdinal == Selector.Index)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    case ESelectorKind::PieInstance:
        for (int32 Index = 0; Index < Contexts.Num(); ++Index)
        {
            if (Contexts[Index].PieInstance == Selector.Index)
            {
                return Index;
            }
        }
        return INDEX_NONE;

    default:
        return INDEX_NONE;
    }
}

// Human-readable summary of the available PIE contexts for WORLD_NOT_FOUND messages,
// e.g. "pie:0 server [MyMap], pie:1 client (client:1) [MyMap]".
inline FString DescribeContexts(const TArray<FPieContextInfo>& Contexts)
{
    if (Contexts.Num() == 0)
    {
        return TEXT("none (PIE is not running)");
    }
    TArray<FString> Parts;
    Parts.Reserve(Contexts.Num());
    int32 ClientOrdinal = 0;
    for (const FPieContextInfo& Info : Contexts)
    {
        FString Part = FString::Printf(TEXT("pie:%d %s"), Info.PieInstance, ClassifyNetMode(Info.NetMode));
        if (Info.NetMode == NM_Client)
        {
            ++ClientOrdinal;
            Part += FString::Printf(TEXT(" (client:%d)"), ClientOrdinal);
        }
        if (Info.World)
        {
            Part += FString::Printf(TEXT(" [%s]"), *UWorld::RemovePIEPrefix(Info.World->GetMapName()));
        }
        Parts.Add(Part);
    }
    return FString::Join(Parts, TEXT(", "));
}

} // namespace PieWorldSelector
