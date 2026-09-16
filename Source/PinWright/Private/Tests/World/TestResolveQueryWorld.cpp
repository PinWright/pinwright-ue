// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/ActorUtils.h"

#include "Editor.h"
#include "Engine/World.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveQueryWorldEditorModeReturnsEditorWorld,
    "PinWright.actor_utils.ResolveQueryWorld.EditorMode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolveQueryWorldEditorModeReturnsEditorWorld::RunTest(const FString& Parameters)
{
    // Test assumes no PIE session is active (headless automation run).
    TestNull(TEXT("test assumes no active PIE world"),
        GEditor ? GEditor->PlayWorld : nullptr);

    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;

    // editor: must return the editor world, mode echoed as "editor".
    {
        FString ResolvedMode;
        UWorld* Resolved = McpActorUtils::ResolveQueryWorld(TEXT("editor"), ResolvedMode);
        TestEqual(TEXT("editor mode echoes as 'editor'"), ResolvedMode, FString(TEXT("editor")));
        TestEqual(TEXT("editor mode returns editor world"), Resolved, EditorWorld);
    }

    // auto (no PIE): must fall back to the editor world.
    {
        FString ResolvedMode;
        UWorld* Resolved = McpActorUtils::ResolveQueryWorld(TEXT("auto"), ResolvedMode);
        TestEqual(TEXT("auto mode echoes as 'auto'"), ResolvedMode, FString(TEXT("auto")));
        TestEqual(TEXT("auto mode falls back to editor world"), Resolved, EditorWorld);
    }

    // pie (no PIE active): must return nullptr, mode echoed as "pie".
    {
        FString ResolvedMode;
        UWorld* Resolved = McpActorUtils::ResolveQueryWorld(TEXT("pie"), ResolvedMode);
        TestEqual(TEXT("pie mode echoes as 'pie'"), ResolvedMode, FString(TEXT("pie")));
        TestNull(TEXT("pie mode returns nullptr when no PIE active"), Resolved);
    }

    return true;
}
