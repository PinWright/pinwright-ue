// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red/regression test for B-open-asset-world-map-load-crash:
//   editor.open_asset on a World hard-crashes the editor.
//
// Pre-fix, editor.open_asset (EditorCommandHandler.cpp) routes EVERY asset type
// through UAssetEditorSubsystem::OpenEditorForAsset. For a UWorld that dispatches to
// UAssetDefinition_World::OpenAssets -> UEditorEngine::Map_Load, which tears down and
// GCs the currently-loaded editor world. Re-opening the ALREADY-ACTIVE map is the
// acute case: level.load / editor.open_level no-op when the requested map is already
// current (LevelHandler.cpp: DoesRequestedLevelMatchCurrentWorld -> alreadyLoaded),
// but editor.open_asset has no such guard, so it Map_Loads over the live world and
// trips
//   Assertion failed: !LevelList.Contains(TickTaskLevel)  (TickTaskManager.cpp)
// in FTickTaskManager::FreeTickTaskLevel during the outgoing world's ~ULevel GC.
//
// Correct behavior (ticket "What should happen"): open_asset on a World must NOT
// hard-crash and must NOT destroy-and-reload the live editor world — reopening the
// already-active map is a no-op. The fix detects a UWorld target and cross-dispatches
// to editor.open_level (-> level.load), which no-ops on the already-active world.
//
// That cross-dispatch runs through the live UPinWrightSubsystem (open_asset ->
// editor.open_level -> level.load, each a Ctx.GetSubsystem()->DispatchMethod hop), so
// this test drives the PRODUCTION editor.open_asset handler through the real subsystem
// (GEditor->GetEditorSubsystem, whose DispatchMethod wires the live subsystem into the
// handler context) — the same pattern the level.spawn_light cross-dispatch test uses.
// The subsystem-less context InvokeHandler builds would short-circuit the World branch
// at SUBSYSTEM_NOT_FOUND and never exercise the real map-load route.
//
// The test targets the live editor world's own object path (the acute cold-boot
// already-active-map scenario). PRE-FIX the invoke reaches OpenEditorForAsset ->
// Map_Load and tears down + GCs the original world (and can hard-crash on the
// !LevelList assertion — the run dies here); either way the world-identity assertions
// below fail. POST-FIX open_asset no-ops via editor.open_level and the live world is
// preserved, flipping both assertions green.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "UObject/WeakObjectPtr.h"
#include "Misc/PackageName.h"
#include "EditorAssetLibrary.h"
#include "PinWrightSubsystem.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

// Resolves the currently-active editor world to a real map that has a package on disk.
// Since PinWright.aa_suite_start.OpenBlankTransientWorld the ambient world is ALWAYS an
// untitled /Temp world, so this always takes the second branch: it discovers a real map
// through the shared FindAlternateOnDiskMap registry walk and opens it through the
// CRASH-SAFE editor.open_level path (level.load no-ops when that map is already active),
// dispatched through the live subsystem so the cross-dispatch to level.load actually
// executes. Discovered, never hardcoded — the previous fixed path was a map from another
// host project that does not exist here. Returns the world, or nullptr if a persisted map
// world could not be established.
//
// ANY mount root qualifies, not just /Game. The fixture this test needs is "a live editor
// world whose asset exists on disk", and the registry walk returns whatever the host has
// first — on this host that is the engine's own /Engine/Maps/Entry. A /Game-only
// acceptance check rejected exactly the map the walk had just successfully opened, and the
// resolve reported failure with the correct map loaded. The mount root is also the wrong
// thing to prefer: an /Engine map is read-only content, so using one keeps this test even
// further away from the host's own assets.
static UWorld* ResolveActiveOnDiskMapWorldForOpenAssetTest(UPinWrightSubsystem* Subsystem)
{
    auto IsOnDiskMapWorld = [](UWorld* W) -> bool
    {
        if (!W || !W->GetOutermost())
        {
            return false;
        }
        // GetOutermost()->GetName() is the bare package name (not an object path),
        // which is what FPackageName::DoesPackageExist requires. It is false for the
        // untitled /Temp world the suite runs on, which is the case this must reject.
        return FPackageName::DoesPackageExist(W->GetOutermost()->GetName());
    };

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (IsOnDiskMapWorld(World))
    {
        return World;
    }

    // Ambient world is not a persisted map — establish one via the crash-safe open_level
    // (level.load no-ops if it is already the active world). Empty exclusion: any on-disk
    // map will do, including the one the editor started on before the suite swapped it out.
    const FString MapToOpen = FindAlternateOnDiskMap(FString());
    if (Subsystem && !MapToOpen.IsEmpty())
    {
        TSharedPtr<FJsonObject> OpenPayload = MakeShared<FJsonObject>();
        OpenPayload->SetStringField(TEXT("levelPath"), MapToOpen);
        Subsystem->DispatchMethod(TEXT("editor.open_level"), TEXT("test-id"), OpenPayload);
    }

    World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    return IsOnDiskMapWorld(World) ? World : nullptr;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorOpenAssetWorldNoCrashTest,
    "PinWright.editor.open_asset.WorldDoesNotCrashEditor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorOpenAssetWorldNoCrashTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddError(TEXT("GEditor unavailable — cannot exercise editor.open_asset on a World."));
        return false;
    }

    // The World cross-dispatch (editor.open_asset -> editor.open_level -> level.load)
    // only runs when a real UPinWrightSubsystem is wired into the handler context. In
    // the editor automation run the subsystem is always initialized; its absence is a
    // fixture FAILURE (not a skip) — the fix cannot be exercised without it.
    UPinWrightSubsystem* Subsystem = GEditor->GetEditorSubsystem<UPinWrightSubsystem>();
    if (!Subsystem)
    {
        AddError(TEXT("PinWright subsystem unavailable — cannot exercise the World cross-dispatch."));
        return false;
    }

    // Declared BEFORE the resolve: the resolve opens a real on-disk map over the untitled
    // world the suite runs on, and this guard is what puts a blank world back afterwards
    // so that map is not left ambient for every test that sorts after this one.
    FScopedEditorWorldMapGuard MapGuard;

    UWorld* ActiveWorld = ResolveActiveOnDiskMapWorldForOpenAssetTest(Subsystem);
    if (!ActiveWorld)
    {
        // A real on-disk editor world is a REQUIRED fixture for this repro, not a skip.
        // Both halves are named: "the registry had nothing" and "the open did not take"
        // need different fixes, and a message that cannot tell them apart sent the last
        // failure looking at the registry query when the map had loaded correctly.
        const FString OfferedMap = FindAlternateOnDiskMap(FString());
        UWorld* const FinalWorld = GEditor->GetEditorWorldContext().World();
        const FString FinalPackage = (FinalWorld && FinalWorld->GetOutermost())
            ? FinalWorld->GetOutermost()->GetName()
            : FString(TEXT("<none>"));
        AddError(FString::Printf(
            TEXT("Could not establish a real on-disk editor map world. The registry walk "
                 "offered '%s'; the active world after the open_level dispatch is '%s'."),
            *OfferedMap, *FinalPackage));
        return false;
    }

    // The World asset the handler will (pre-fix) Map_Load over: the object path of the
    // live editor world itself, i.e. re-opening the ALREADY-ACTIVE map.
    const FString WorldAssetPath = ActiveWorld->GetPathName();
    if (!TestTrue(TEXT("active world's asset path exists on disk"),
            UEditorAssetLibrary::DoesAssetExist(WorldAssetPath)))
    {
        return false;
    }

    // Snapshot the live world's identity. A weak ptr goes stale iff the world object
    // is GC'd — which is exactly what Map_Load's EditorDestroyWorld -> Cleanse ->
    // CollectGarbage does to the outgoing world.
    TWeakObjectPtr<UWorld> OriginalWorldWeak(ActiveWorld);

    // Drive the PRODUCTION editor.open_asset handler through the live subsystem with the
    // active World's path. PRE-FIX this reaches UAssetEditorSubsystem::OpenEditorForAsset
    // -> Map_Load and either hard-crashes the editor (!LevelList.Contains(TickTaskLevel))
    // — the run dies here — or tears down + GCs the live world, failing the identity
    // assertions below. POST-FIX open_asset cross-dispatches to editor.open_level ->
    // level.load, which no-ops on the already-active map, preserving the live world.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), WorldAssetPath);
    TestTrue(TEXT("editor.open_asset dispatched without hard-crashing on a World target"),
        Subsystem->DispatchMethod(TEXT("editor.open_asset"), TEXT("test-id"), Payload));

    // Load-bearing correct-behavior contract: opening a World via open_asset must not
    // tear down the live editor world. The original world object must still be alive
    // and remain the active editor world (no destroy-and-reload of the active map).
    TestTrue(TEXT("the live editor world was NOT torn down by open_asset on a World"),
        OriginalWorldWeak.IsValid());
    TestTrue(TEXT("the original world is still the active editor world (no Map_Load reload)"),
        OriginalWorldWeak.Get() == GEditor->GetEditorWorldContext().World());

    return true;
}
