// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintPathLoad.h - the single body behind the "load a UBlueprint from a caller-supplied
// path" helper that GameFrameworkHandler.cpp and NetworkingHandler.cpp each used to carry a
// verbatim copy of (GameFrameworkHelpers::LoadBlueprintFromPath and
// NetworkingHelpersNew::LoadBlueprintFromPath, 29 call sites between them).
//
// WHY IT LIVES HERE. What it loads is a Blueprint, so it belongs to the Blueprint handler
// cluster; putting it there also means neither of the two consumers takes a dependency on the
// other's domain header, which is what a "pick one of the two files" merge would have forced.
// It is header-inline rather than a new .cpp because the body is fifteen lines and a second
// translation unit would buy nothing. Both consumers keep their existing namespace-qualified
// spelling through a using-declaration, so all 29 call sites are untouched.
#pragma once

#include "CoreMinimal.h"
#include "Engine/Blueprint.h"
#include "PinWrightSubsystem.h"
#include "UObject/UObjectGlobals.h"
#include "Utils/PathUtils.h"

namespace PinWrightBlueprintPathLoad
{
    // Returns nullptr for a "_C" generated-class path, for a path containing "//", and for a path
    // that resolves to no Blueprint. Callers get one failure shape and must not tell these apart.
    //
    // THE "//" REFUSAL IS WHY THIS FUNCTION IS SHARED RATHER THAN DUPLICATED. Both
    // StaticLoadObject calls below reach CreatePackage's Fatal (StaticLoadObjectInternal ->
    // ResolveName2(..., Create=true) -> CreatePackage on the partial name), and Fatal is not
    // compiled out in any configuration: it ends the PROCESS and every unsaved package in it, so
    // no caller can defend itself with a null check - nothing after the call is reached. A
    // duplicated body is a guard that has to be remembered twice; one body is a guard the 29 call
    // sites inherit. Board B-createpackage-unvalidated-paths-plugin-wide.
    //
    // Warning, never Error: a malformed argument is a refusal, not a plugin fault, and
    // bElevateLogWarningsToErrors turns a logged Error into a test failure. Neither consumer has
    // an error channel here (both return a bare pointer), so the log is the whole record; giving
    // 29 call sites an out-error is a separate ticket.
    inline UBlueprint* LoadBlueprintFromPath(const FString& BlueprintPath)
    {
        if (CanReachCreatePackageFatal(BlueprintPath))
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("LoadBlueprintFromPath refused '%s': a blueprint path may not contain '//'."),
                *BlueprintPath);
            return nullptr;
        }

        FString CleanPath = BlueprintPath;
        if (!CleanPath.EndsWith(TEXT("_C")))
        {
            UBlueprint* BP = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *CleanPath));
            if (BP) return BP;
            if (CleanPath.EndsWith(TEXT(".uasset")))
            {
                CleanPath = CleanPath.LeftChop(7);
                BP = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *CleanPath));
            }
            return BP;
        }
        return nullptr;
    }
}
