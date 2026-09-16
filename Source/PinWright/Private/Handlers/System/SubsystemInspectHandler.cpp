// Copyright (c) 2026 Alexander Penkin. MIT License.

// SubsystemInspectHandler.cpp
// Enumerates live UEngine/UEditor/UGameInstance/UWorld/ULocalPlayer subsystem
// instances and returns their class name / object path / owner path so that
// downstream MCP callers can resolve a subsystem ref without needing to know
// the leaf instance suffix or outer chain.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/HandlerContext.h"

#include "Utils/ActorUtils.h"
#include "Utils/JsonUtils.h"

#include "Dom/JsonObject.h"
#include "Compat/EngineVersionCompat.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "EditorSubsystem.h"

// ---- system.inspect.list_subsystems ----
REGISTER_RPC_HANDLER("system.inspect.list_subsystems", "system.inspect",
    "Enumerate live UEngine/UEditor/UGameInstance/UWorld/ULocalPlayer subsystem instances.",
    RPC_PARAMS(
        RPC_PARAM_OPT("scope", "string", "Optional filter: Engine | Editor | GameInstance | World | LocalPlayer. Omit for all scopes.")
    ))
{
    const FString Scope = Ctx.GetString(TEXT("scope"));

    // Validate scope if provided.
    if (!Scope.IsEmpty()
        && Scope != TEXT("Engine")
        && Scope != TEXT("Editor")
        && Scope != TEXT("GameInstance")
        && Scope != TEXT("World")
        && Scope != TEXT("LocalPlayer"))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("Invalid scope '%s'. Valid: Engine|Editor|GameInstance|World|LocalPlayer (omit for all)."), *Scope));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Subsystems;

    // Build one entry per non-null subsystem.
    auto Emit = [&Subsystems](const TCHAR* ScopeName, USubsystem* S, UObject* Owner)
    {
        if (!S || !Owner) return;
        TSharedPtr<FJsonObject> Entry = EmitObjectRef(S);
        Entry->SetStringField(TEXT("scope"), ScopeName);
        Entry->SetStringField(TEXT("ownerPath"), Owner->GetPathName());
        Subsystems.Add(MakeShared<FJsonValueObject>(Entry));
    };

    // ---- Engine ----
    if (Scope.IsEmpty() || Scope == TEXT("Engine"))
    {
        if (GEngine)
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            // GetEngineSubsystemArrayCopy added in UE 5.5; use the reference-returning
            // GetEngineSubsystemArray on 5.4 (unsafe for re-entrancy but acceptable here
            // since we only read the list and do not trigger subsystem changes)
            for (UEngineSubsystem* S : GEngine->GetEngineSubsystemArrayCopy<UEngineSubsystem>())
#else
            for (UEngineSubsystem* S : GEngine->GetEngineSubsystemArray<UEngineSubsystem>())
#endif
            {
                Emit(TEXT("Engine"), S, GEngine);
            }
        }
    }

    // ---- Editor ----
    if (Scope.IsEmpty() || Scope == TEXT("Editor"))
    {
        if (GEditor)
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            // GetEditorSubsystemArrayCopy added in UE 5.5
            for (UEditorSubsystem* S : GEditor->GetEditorSubsystemArrayCopy<UEditorSubsystem>())
#else
            for (UEditorSubsystem* S : GEditor->GetEditorSubsystemArray<UEditorSubsystem>())
#endif
            {
                Emit(TEXT("Editor"), S, GEditor);
            }
        }
    }

    // ---- GameInstance / World / LocalPlayer: share PIE-first world resolution.
    auto ResolveWorld = []() -> UWorld*
    {
        FString UnusedMode;
        return McpActorUtils::ResolveQueryWorld(TEXT(""), UnusedMode);
    };

    // ---- GameInstance ----
    if (Scope.IsEmpty() || Scope == TEXT("GameInstance"))
    {
        UWorld* World = ResolveWorld();
        UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
        if (GI)
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            // GetSubsystemArrayCopy added in UE 5.5
            for (UGameInstanceSubsystem* S : GI->GetSubsystemArrayCopy<UGameInstanceSubsystem>())
#else
            for (UGameInstanceSubsystem* S : GI->GetSubsystemArray<UGameInstanceSubsystem>())
#endif
            {
                Emit(TEXT("GameInstance"), S, GI);
            }
        }
    }

    // ---- World ----
    if (Scope.IsEmpty() || Scope == TEXT("World"))
    {
        UWorld* World = ResolveWorld();
        if (World)
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            // GetSubsystemArrayCopy added in UE 5.5
            for (UWorldSubsystem* S : World->GetSubsystemArrayCopy<UWorldSubsystem>())
#else
            for (UWorldSubsystem* S : World->GetSubsystemArray<UWorldSubsystem>())
#endif
            {
                Emit(TEXT("World"), S, World);
            }
        }
    }

    // ---- LocalPlayer ----
    if (Scope.IsEmpty() || Scope == TEXT("LocalPlayer"))
    {
        UWorld* World = ResolveWorld();
        UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
        if (GI)
        {
            for (ULocalPlayer* LP : GI->GetLocalPlayers())
            {
                if (!LP) continue;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
                // GetSubsystemArrayCopy added in UE 5.5
                for (ULocalPlayerSubsystem* S : LP->GetSubsystemArrayCopy<ULocalPlayerSubsystem>())
#else
                for (ULocalPlayerSubsystem* S : LP->GetSubsystemArray<ULocalPlayerSubsystem>())
#endif
                {
                    Emit(TEXT("LocalPlayer"), S, LP);
                }
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("subsystems"), Subsystems);
    Ctx.SendSuccess(Result);
    return true;
}
