// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

// Shared guard for the two verbs that run a raw console line — system.console_command and
// editor.console_command — against the line shapes whose side effect outlives the call:
// `sg.<Group> N`, and a set of ANY console variable carrying ECVF_Scalability /
// ECVF_ScalabilityGroup.
//
// A console set writes at ECVF_SetByConsole (IConsoleManager.h:187), the highest of the
// fifteen priorities, and that priority never decays for the life of the process. The
// editor's own Settings > Engine Scalability Settings panel writes through
// Scalability::SetQualityLevels at ECVF_SetByScalability (Scalability.cpp:907), the
// second-lowest, so FConsoleVariableBase::CanChange (ConsoleManager.cpp:277-280) discards
// every later change the HUMAN USER makes to that group until the editor restarts. Measured
// once already (board B-console-command-sg-cvar-pin-freezes-scalability): seven groups
// pinned by an agent, five untouched, and the mixed result read to the user as the editor
// force-resetting quality on its own.
//
// performance.set_scalability drives the same groups through SetQualityLevels — the panel's
// own priority — and therefore cannot create the pin, which is why the refusal names it.
//
// A SCALABILITY GROUP IS NOT A SPECIAL KIND OF VARIABLE. It is a name for a list of ordinary
// cvars in a `[<Group>@N]` section of BaseScalability.ini, and the panel reaches those members
// one hop after the group cvar — Scalability.cpp:446 hands the whole section to
// ApplyCVarSettingsFromIni at ECVF_SetByScalability, which writes each member at
// ConfigUtilities.cpp:365. `r.ViewDistanceScale 0.6` therefore pins ViewDistanceQuality against
// the user's panel exactly as `sg.ViewDistanceQuality 1` does, through the identical
// ConsoleManager.cpp:3228 `CVar->Set(*Param2, ECVF_SetByConsole)`. Board
// B-console-member-cvar-pin-freezes-scalability measured two such members pinned in a live
// editor by lines the sg.-prefix rule allowed, so the rule below tests the FLAG on the resolved
// console object rather than the spelling of the name. Refusing on a flag rather than on an
// ini-derived name list is load-bearing: r.ScreenPercentage is a ResolutionQuality member with
// no ini row at all (Scalability.cpp:551), and r.VSync carries ECVF_Scalability with no ini row
// (ConsoleManager.cpp:4330, flags :4334), so any list built by scanning BaseScalability.ini
// misses both.
namespace ScalabilityConsoleGuard
{
    // True when the command line's FIRST token names an `sg.*` console variable,
    // case-insensitively. Because `sg.` contains no whitespace, a prefix test on the
    // left-trimmed line IS the first-token test. Deliberately narrow in two directions:
    //
    //   - first token only, so `r.Foo sg.Bar` — the prefix appearing in a value or inside a
    //     quoted string — is not a scalability set and is not refused;
    //   - the `sg.` prefix only, so the AGGREGATE `scalability N` is NOT matched. That form
    //     routes through Scalability::ProcessCommand -> SetQualityLevels at
    //     ECVF_SetByScalability (Scalability.cpp:827-834) and is harmless on this axis.
    //
    // A bare `sg.<Group>` read is matched too. Over-refusing a read costs one `force:true`
    // round trip; under-refusing a set costs the user their Scalability panel for the
    // session, so the rule stays the simple prefix test the board ticket asked for.
    inline bool IsScalabilityGroupLine(const FString& CommandLine)
    {
        return CommandLine.TrimStart().StartsWith(TEXT("sg."), ESearchCase::IgnoreCase);
    }

    // Splits the left-trimmed line the way FConsoleManager::GetTextSection does
    // (ConsoleManager.cpp:3409-3429, called at :3079 to produce Param1): the first token runs
    // to the first whitespace, the remainder is everything after it. No quote handling, because
    // the engine's own splitter has none either — a quoted first token is not a cvar name.
    inline void SplitFirstConsoleToken(const FString& CommandLine, FString& OutToken, FString& OutRemainder)
    {
        const FString Trimmed = CommandLine.TrimStart();
        int32 TokenEnd = 0;
        while (TokenEnd < Trimmed.Len() && !FChar::IsWhitespace(Trimmed[TokenEnd]))
        {
            ++TokenEnd;
        }
        OutToken = Trimmed.Left(TokenEnd);
        OutRemainder = Trimmed.Mid(TokenEnd).TrimStartAndEnd();
    }

    // True when the line's first token resolves to a registered console VARIABLE carrying
    // ECVF_Scalability or ECVF_ScalabilityGroup (IConsoleManager.h:103, :106) AND a value token
    // follows it — i.e. exactly the lines that reach ConsoleManager.cpp:3228 and pin the
    // variable at ECVF_SetByConsole.
    //
    // Four things this deliberately does NOT refuse, each because the engine performs no Set on
    // that path and a false refusal costs a caller a round trip for nothing:
    //
    //   - a BARE READ (`r.ViewDistanceScale` with no value): ProcessUserConsoleInput takes the
    //     bShowCurrentState branch at ConsoleManager.cpp:3189 and never calls Set. The `sg.`
    //     rule above over-refuses reads on purpose and says so; this rule can afford to be
    //     exact, and being exact matters more here because reading a member cvar is normal and
    //     frequent;
    //   - the HELP form (`r.Foo ?`), ConsoleManager.cpp:3213 — also no Set;
    //   - an UNRESOLVABLE token: a typo, or an exec command owned by a module. `scalability N`
    //     lands here — it is not a console object at all but a UEngine::Exec branch
    //     (UnrealEngine.cpp:5717-5721) routing to Scalability::ProcessCommand ->
    //     SetQualityLevels at the panel's own priority — so the aggregate form stays allowed,
    //     as it was under the `sg.` rule;
    //   - an ECVF_ReadOnly variable, which the console refuses to set itself at
    //     ConsoleManager.cpp:3223 before reaching the Set. This clause can only ever bite a
    //     ReadOnly ECVF_ScalabilityGroup cvar: FConsoleManager::AddConsoleObject:3269-3275
    //     check()s that an ECVF_Scalability variable is neither Cheat nor ReadOnly, precisely so
    //     the options menu can always drive it.
    //
    // A trailing `?` is stripped from the token because ConsoleManager.cpp:3085-3090 strips one
    // before the registry lookup, and `r.Foo? 0.5` still reaches the Set at :3228 — without the
    // strip that spelling would be a free bypass.
    //
    // A `<cvar>@<platform>` token is left unresolved on purpose: the engine sets the per-platform
    // copy, never the live variable, and logs "Unable to set a value for %s another platform!"
    // instead, so that form cannot pin anything.
    inline bool IsScalabilityCVarSetLine(const FString& CommandLine)
    {
        FString Token;
        FString Remainder;
        SplitFirstConsoleToken(CommandLine, Token, Remainder);

        if (Token.EndsWith(TEXT("?"), ESearchCase::CaseSensitive))
        {
            Token = Token.LeftChop(1);
        }
        if (Token.IsEmpty() || Remainder.IsEmpty() || Remainder == TEXT("?"))
        {
            return false;
        }

        // bTrackFrequentCalls=false: this runs on every console line as a guard, not as a real
        // cvar read, and must not distort the engine's frequent-call tracking.
        IConsoleObject* Object =
            IConsoleManager::Get().FindConsoleObject(*Token, /*bTrackFrequentCalls=*/false);
        if (!Object || !Object->AsVariable())
        {
            return false;
        }
        if (Object->TestFlags(ECVF_ReadOnly))
        {
            return false;
        }
        return Object->TestFlags(ECVF_Scalability) || Object->TestFlags(ECVF_ScalabilityGroup);
    }

    // The predicate the two console verbs gate on. The `sg.` spelling test is kept as a fallback
    // rather than replaced, so a group cvar that is not registered at guard time — an
    // unregistered `sg.*` on a stripped host — still refuses, and every line the old rule
    // refused is still refused. Current behaviour is a strict subset of this one.
    inline bool IsScalabilityPinningLine(const FString& CommandLine)
    {
        return IsScalabilityGroupLine(CommandLine) || IsScalabilityCVarSetLine(CommandLine);
    }

    // The refusal text, shared so the two verbs cannot drift apart on the one sentence that
    // has to explain a durable side effect the caller did not ask for.
    inline FString MakeScalabilityTypedVerbRefusal(const FString& CommandLine)
    {
        return FString::Printf(
            TEXT("Refusing '%s': setting a scalability CVar from the console — an sg.* group, or "
                 "any ECVF_Scalability member of one such as r.ViewDistanceScale or "
                 "r.Streaming.PoolSize — pins it at ECVF_SetByConsole, the highest CVar priority, "
                 "which outranks the ECVF_SetByScalability priority the editor's own Settings > "
                 "Engine Scalability Settings panel writes at, for the remainder of the editor "
                 "session — so every later change the user makes to the owning group is silently "
                 "discarded until the editor is restarted. Use performance.set_scalability "
                 "instead: it writes the sg.* groups through Scalability::SetQualityLevels at the "
                 "same priority the panel uses, which also re-applies every member cvar of the "
                 "groups it touches, and cannot create the pin. Reading a scalability CVar (the "
                 "name with no value) is not refused. Pass force:true to run this line anyway and "
                 "accept the pin."),
            *CommandLine);
    }
}
