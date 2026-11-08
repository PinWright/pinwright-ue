// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"

// Write a console variable WITHOUT changing the priority it already carries.
//
// THE DEFECT THIS EXISTS FOR (board B-performance-typed-verbs-pin-scalability-cvars). The
// templated helper `IConsoleVariable::Set(T Value, EConsoleVariableFlags Flags = ECVF_SetByCode,
// FName Tag = NAME_None)` (IConsoleManager.h:766) defaults its priority to
// ECVF_SetByCode = 0x0E000000 (IConsoleManager.h:183), so a bare `CVar->Set(x)` stamps the
// variable at Code priority — six levels above the ECVF_SetByScalability = 0x02000000
// (IConsoleManager.h:159) that Scalability::SetQualityLevels, and therefore the editor's own
// Settings > Engine Scalability Settings panel, writes at (Scalability.cpp:907). Because
// FConsoleVariableBase::CanChange is `NewPri >= OldPri` (ConsoleManager.cpp:275-311, compare at
// :280), every later change the HUMAN USER makes to the owning group is discarded for the rest
// of the editor session, logged as a LogConsoleManager Warning by the else branch at :305-309.
// A tuning verb should never spend the user's Scalability panel for the session.
//
// WHY THE CURRENT PRIORITY AND NOT A FIXED ECVF_SetByScalability. Writing everything at
// Scalability priority looks like the symmetric fix and is wrong in both directions:
//
//   - it would be DISCARDED whenever the variable already sits higher. r.VSync is written at
//     ECVF_SetByGameSetting = 0x03000000 by UGameUserSettings::ApplyNonResolutionSettings
//     (GameUserSettings.cpp:529), so a Scalability-priority write to it silently does nothing
//     and the verb quietly stops working;
//   - ECVF_SetByScalability is reserved for scalability-flagged variables. The engine
//     ensureMsgf's on a Scalability-priority write to a cvar carrying neither ECVF_Scalability
//     nor ECVF_ScalabilityGroup (ConfigUtilities.cpp:337-346), and these verbs also write plain
//     cvars such as r.TextureStreaming and r.MeshDrawCommands.*.
//
// Writing at the variable's OWN current priority is accepted unconditionally (CanChange is >=,
// not >), never raises the priority, and never lowers it — so the write always lands and never
// outranks anything it did not already outrank. The pattern is not new here: FScopedViewDistanceScale
// in Handlers/Render/PreviewViewportCaptureUtils.cpp:161-162 (restore :174, rationale :139-142)
// already does exactly this for r.ViewDistanceScale. This header lifts it out so it is not
// copied a third time.
//
// NOT a substitute for reading the value back. The write landing does not mean the engine kept
// the number — clamps and OnChanged sinks still apply — so a verb that REPORTS what it applied
// must re-read the cvar (`CVar->GetInt()`) after the set rather than echoing the request.
namespace CVarPriorityPreservingSet
{
    // The variable's current SetBy priority, i.e. the value to write back at.
    inline EConsoleVariableFlags ReadCurrentSetByPriority(const IConsoleVariable* CVar)
    {
        return CVar ? static_cast<EConsoleVariableFlags>(CVar->GetFlags() & ECVF_SetByMask)
                    : ECVF_SetByConstructor;
    }

    inline void SetPreservingPriority(IConsoleVariable* CVar, int32 Value)
    {
        if (CVar)
        {
            CVar->Set(Value, ReadCurrentSetByPriority(CVar));
        }
    }

    inline void SetPreservingPriority(IConsoleVariable* CVar, float Value)
    {
        if (CVar)
        {
            CVar->Set(Value, ReadCurrentSetByPriority(CVar));
        }
    }
}
