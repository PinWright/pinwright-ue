// Copyright (c) 2026 Alexander Penkin. MIT License.

// ConsoleSearchHandler.cpp
// system.console.search — substring search across the live IConsoleManager
// registry. Closes the discovery gap for the `editor.console_command` /
// `system.console_command` escape hatch (which are execute-only).

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "HAL/IConsoleManager.h"

namespace
{
    // Maps the agent-relevant ECVF_* bits (single-bit flags only) onto stable
    // string tags. ECVF_SetByMask spans multiple bits and is intentionally
    // omitted per the MVP scope on this ticket.
    TArray<TSharedPtr<FJsonValue>> FlagsToStrings(const IConsoleObject* Obj)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        if (!Obj)
        {
            return Out;
        }
        // Use IConsoleObject::TestFlags to keep the IConsoleManager abstraction
        // intact instead of round-tripping through GetFlags() + bit math.
        auto Push = [&Out, Obj](EConsoleVariableFlags Bit, const TCHAR* Name)
        {
            if (Obj->TestFlags(Bit))
            {
                Out.Add(MakeShared<FJsonValueString>(Name));
            }
        };
        Push(ECVF_Cheat,             TEXT("Cheat"));
        Push(ECVF_ReadOnly,          TEXT("ReadOnly"));
        Push(ECVF_RenderThreadSafe,  TEXT("RenderThreadSafe"));
        Push(ECVF_Scalability,       TEXT("Scalability"));
        Push(ECVF_ScalabilityGroup,  TEXT("ScalabilityGroup"));
        Push(ECVF_Preview,           TEXT("Preview"));
        Push(ECVF_ExcludeFromPreview,TEXT("ExcludeFromPreview"));
        Push(ECVF_Unregistered,      TEXT("Unregistered"));
        return Out;
    }
}

REGISTER_RPC_HANDLER("system.console.search", "system.console",
    "Substring-search the live IConsoleManager registry for variables/commands matching `query`. Returns name + kind + help + currentValue (variables only) + flags per row. A broad query at the default limit emits full multi-line help text per row and can exceed the inline display budget (spilling to a file); drop the fat help column inline with namesOnly=true (or fields=[...]), narrow the query, or lower limit. Closes the discovery gap for the console_command escape hatch.",
    RPC_PARAMS(
        RPC_PARAM_REQ("query", "string", "Case-insensitive substring matched against console object names. Non-empty after trim."),
        RPC_PARAM_OPT("kind", "string", "Filter by object kind: 'variable' | 'command' | 'any'. Default 'any'."),
        RPC_PARAM_OPT("limit", "number", "Maximum rows in `results` (clamped to [1, 500]). Default 50. totalMatches always reports the full untruncated count so elision stays detectable."),
        RPC_PARAM_OPT_ALIAS("fields", "array|string", "Case-insensitive allow-list of per-row keys to return (valid keys: name, kind, help, currentValue, flags); e.g. [\"name\",\"currentValue\"] to drop the verbose help text. Omit for all keys. A single string is also accepted.", "field"),
        RPC_PARAM_OPT_ALIAS("namesOnly", "bool", "When true, drops the byte-dominating multi-line `help` string from every row (keeps name+kind+currentValue+flags) — shorthand for the common broad-discovery scan that overflows the inline budget. Snake_case names_only accepted. Ignored when fields is supplied.", "names_only")
    ))
{
    FString Query = Ctx.GetString(TEXT("query"));
    Query.TrimStartAndEndInline();
    if (Query.IsEmpty())
    {
        // Rejecting empty queries here prevents accidental full enumeration of
        // the (thousands-strong) registry through a missing/blank param.
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("query must be a non-empty string"));
        return true;
    }

    FString Kind = Ctx.GetString(TEXT("kind"), TEXT("any")).ToLower();
    if (Kind != TEXT("variable") && Kind != TEXT("command") && Kind != TEXT("any"))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("kind must be 'variable', 'command', or 'any' (got '%s')"), *Kind));
        return true;
    }

    int32 Limit = Ctx.GetInt(TEXT("limit"), 50);
    Limit = FMath::Clamp(Limit, 1, 500);

    // Per-row field projection: an explicit fields allow-list (array or bare
    // string) wins; otherwise namesOnly drops the byte-dominating multi-line
    // `help` text (the column that overflows a broad discovery scan into a file).
    // An empty set means "no projection" — the unprojected output is byte-identical
    // to the prior shape. Shared with actor.list via FHandlerContext::ReadFieldProjection;
    // the namesOnly column set (everything except the fat `help`) is the per-handler argument.
    const TSet<FString> Fields = Ctx.ReadFieldProjection(
        {TEXT("name"), TEXT("kind"), TEXT("currentvalue"), TEXT("flags")});
    // Resolve each key to a bool once (the wanted-set never changes per row) instead
    // of probing the TSet — with a throwaway FString — for every emitted row. An empty
    // set means "no projection": want everything. The probe keys are lowercase to match
    // the lowercased set ReadFieldProjection returns.
    const bool bProject = Fields.Num() > 0;
    const auto Wants = [&Fields, bProject](const TCHAR* Key)
    {
        return !bProject || Fields.Contains(FString(Key));
    };
    const bool bWantName = Wants(TEXT("name"));
    const bool bWantKind = Wants(TEXT("kind"));
    const bool bWantHelp = Wants(TEXT("help"));
    const bool bWantCurrentValue = Wants(TEXT("currentvalue"));
    const bool bWantFlags = Wants(TEXT("flags"));

    // Decode the kind filter once outside the closure so the per-row visitor
    // (potentially called thousands of times) tests cheap bools instead of
    // re-comparing strings.
    const bool bWantVariables = (Kind != TEXT("command"));
    const bool bWantCommands  = (Kind != TEXT("variable"));

    int32 TotalMatches = 0;
    // Buffer matched-and-built rows alongside their name so we can apply a
    // relevance sort (exact > prefix > substring, then by length, then lex)
    // before truncating to Limit. Sorting AFTER collection — instead of
    // limiting inside the visitor — is required so that an exact match like
    // `r.ScreenPercentage` is not dropped when it happens to be visited after
    // Limit prefix matches such as `r.ScreenPercentage.Default*`.
    struct FRow
    {
        FString Name;
        TSharedPtr<FJsonObject> Json;
    };
    TArray<FRow> Collected;

    FConsoleObjectVisitor Visitor = FConsoleObjectVisitor::CreateLambda(
        [&Collected, &TotalMatches, bWantVariables, bWantCommands,
         bWantName, bWantKind, bWantHelp, bWantCurrentValue, bWantFlags]
        (const TCHAR* Name, IConsoleObject* Obj)
    {
        if (!Obj)
        {
            return;
        }

        IConsoleVariable* Var = Obj->AsVariable();
        IConsoleCommand* Cmd = Obj->AsCommand();

        const bool bIsVariable = Var != nullptr;
        const bool bIsCommand  = Cmd != nullptr;

        if (bIsVariable && !bWantVariables) return;
        if (bIsCommand  && !bWantCommands)  return;

        ++TotalMatches;

        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        // NameStr is always built — it backs the relevance sort below regardless of
        // whether the `name` column is projected into the emitted row.
        FString NameStr(Name);
        if (bWantName)
        {
            Row->SetStringField(TEXT("name"), NameStr);
        }
        if (bWantKind)
        {
            Row->SetStringField(TEXT("kind"), bIsVariable ? TEXT("variable") : TEXT("command"));
        }

        if (bWantHelp)
        {
            const TCHAR* Help = Obj->GetHelp();
            Row->SetStringField(TEXT("help"), Help ? FString(Help) : FString());
        }

        if (bIsVariable && bWantCurrentValue)
        {
            Row->SetStringField(TEXT("currentValue"), Var->GetString());
        }

        if (bWantFlags)
        {
            Row->SetArrayField(TEXT("flags"), FlagsToStrings(Obj));
        }
        Collected.Add({ MoveTemp(NameStr), MoveTemp(Row) });
    });

    IConsoleManager::Get().ForEachConsoleObjectThatContains(Visitor, *Query);

    // Relevance ranking: tier 0 = exact name match (case-insensitive),
    // tier 1 = name starts with the query, tier 2 = anywhere else. Ties
    // broken by shorter name first, then lexicographically.
    auto RelevanceTier = [&Query](const FString& Name) -> int32
    {
        if (Name.Equals(Query, ESearchCase::IgnoreCase)) return 0;
        if (Name.StartsWith(Query, ESearchCase::IgnoreCase)) return 1;
        return 2;
    };
    Collected.Sort([&RelevanceTier](const FRow& A, const FRow& B)
    {
        const int32 TA = RelevanceTier(A.Name);
        const int32 TB = RelevanceTier(B.Name);
        if (TA != TB) return TA < TB;
        if (A.Name.Len() != B.Name.Len()) return A.Name.Len() < B.Name.Len();
        return A.Name.Compare(B.Name, ESearchCase::IgnoreCase) < 0;
    });

    const int32 Kept = FMath::Min(Collected.Num(), Limit);
    TArray<TSharedPtr<FJsonValue>> Rows;
    Rows.Reserve(Kept);
    for (int32 i = 0; i < Kept; ++i)
    {
        Rows.Add(MakeShared<FJsonValueObject>(Collected[i].Json));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("results"), Rows);
    Result->SetNumberField(TEXT("totalMatches"), TotalMatches);
    Result->SetBoolField(TEXT("truncated"), TotalMatches > Rows.Num());

    Ctx.SendSuccess(Result);
    return true;
}
