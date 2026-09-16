// Copyright (c) 2026 Alexander Penkin. MIT License.

// Handler-level tests for blueprint.undo_last_bpir.
// Covers the UNDO_NOT_REVERSIBLE refusal when the previous compile ran Phase 0 sweeps
// (B-undo-last-bpir-doesnt-restore-phase0-sweeps) and the positive path where undo
// pops the stack and deletes the created nodes when Phase 0 did not run.

#include "Misc/AutomationTest.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "State/PluginState.h"
#include "UObject/Package.h"


namespace
{
    // Resets both undo stacks together. The two stacks are a coupled invariant: undo_last_bpir
    // pops them in lock-step, so every test must reset them in lock-step too. Keeping the pair
    // in one helper means a future third stack or ordering requirement is fixed in one place.
    void ResetBpirUndoStacks()
    {
        FPluginState::Get().NodeCreationStack().Empty();
        FPluginState::Get().Phase0RanStack().Empty();
    }

    // A package-backed transactional Actor blueprint plus its event graph, built for the
    // undo/compile_bpir tests below. These handlers take an assetPath payload, so they need a
    // named /Game package (CompilerTestUtils::CreateTransientTestBP uses GetTransientPackage()
    // and is not a drop-in). BP is null if construction failed; AssetPath is always populated
    // so the caller's ON_SCOPE_EXIT teardown can run regardless.
    struct FUndoTestBlueprint
    {
        FString AssetPath;
        UBlueprint* BP = nullptr;
        UEdGraph* EventGraph = nullptr;
    };

    // Builds the named, never-saved /Game transactional blueprint the three undo tests share: a
    // GUID-suffixed package under /Game/__PW_GatewayTests/<Slug>_, an AActor blueprint flagged
    // RF_Transactional, and its event graph (also RF_Transactional, required for undo/rollback).
    // Emits the same TestNotNull checkpoints the tests previously inlined. On failure, returns a
    // result whose BP/EventGraph are null but whose AssetPath is set so the caller still cleans up.
    // The caller owns scope lifetime: its ON_SCOPE_EXIT clears the dirty flag, then the safe
    // discard helper renames the asset into the transient package and collects it without saving.
    FUndoTestBlueprint CreateUndoTestBlueprint(FAutomationTestBase& Test, const TCHAR* Slug)
    {
        FUndoTestBlueprint Result;
        Result.AssetPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/%s_%s"),
            Slug,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UPackage* Package = CreatePackage(*Result.AssetPath);
        if (!Test.TestNotNull(TEXT("Package created"), Package))
        {
            return Result;
        }

        UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            Package,
            FName(*FPackageName::GetLongPackageAssetName(Result.AssetPath)),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());

        if (!Test.TestNotNull(TEXT("Blueprint created"), BP))
        {
            return Result;
        }

        BP->SetFlags(RF_Transactional);
        UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
        if (!Test.TestNotNull(TEXT("Event graph exists"), EventGraph))
        {
            // BP is live; surface it so the caller's teardown clears the dirty flag and deletes it.
            Result.BP = BP;
            return Result;
        }
        EventGraph->SetFlags(RF_Transactional);

        Result.BP = BP;
        Result.EventGraph = EventGraph;
        return Result;
    }
} // namespace


// Test 1: append-mode compile (default RPC mode → Replace = Phase 0 sweeps) followed by
// undo_last_bpir must return UNDO_NOT_REVERSIBLE without deleting any nodes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirUndoLastBpirRefusesAfterReplaceTest,
    "PinWright.blueprint.undo_last_bpir.RefusesAfterReplace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirUndoLastBpirRefusesAfterReplaceTest::RunTest(const FString& Parameters)
{
    // Reset undo stacks so the test sees a deterministic baseline.
    ResetBpirUndoStacks();

    FUndoTestBlueprint Fixture = CreateUndoTestBlueprint(*this, TEXT("BpirUndoReplace"));
    const FString& AssetPath = Fixture.AssetPath;

    ON_SCOPE_EXIT
    {
        if (Fixture.BP)
        {
            if (UPackage* BlueprintPackage = Fixture.BP->GetOutermost())
            {
                BlueprintPackage->SetDirtyFlag(false);
            }
        }
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
    };

    if (!Fixture.BP || !Fixture.EventGraph)
    {
        return true;
    }
    UEdGraph* EventGraph = Fixture.EventGraph;

    // First compile in default (Replace) mode — Phase 0 sweeps run.
    const FString BpirCode = TEXT(
        "entry event BeginPlay() {\n"
        "    call PrintString(InString: \"hello\")\n"
        "}\n");

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("code"), BpirCode);
        // mode omitted → defaults to append (Replace).

        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.compile_bpir handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture));
        TestTrue(TEXT("compile_bpir succeeded"), Capture.bSuccess);
    }

    // Confirm bookkeeping: Phase0RanStack top is true.
    TestEqual(TEXT("Phase0RanStack has one entry"),
        FPluginState::Get().Phase0RanStack().Num(), 1);
    TestTrue(TEXT("Phase0RanStack top is true (Replace mode ran Phase 0)"),
        FPluginState::Get().Phase0RanStack().Last());

    const int32 NodesBeforeUndo = EventGraph->Nodes.Num();

    // Now attempt undo — must refuse with UNDO_NOT_REVERSIBLE.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);

        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.undo_last_bpir handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.undo_last_bpir"), Payload, Capture));
        TestTrue(TEXT("Response was sent"), Capture.bWasCalled);
        TestFalse(TEXT("undo_last_bpir returned a structured error"), Capture.bSuccess);
        TestEqual(TEXT("Error code is UNDO_NOT_REVERSIBLE"),
            Capture.ErrorCode,
            FString(TEXT("UNDO_NOT_REVERSIBLE")));
    }

    // Stacks must remain intact — refusal does not pop.
    TestEqual(TEXT("NodeCreationStack still has the compile entry"),
        FPluginState::Get().NodeCreationStack().Num(), 1);
    TestEqual(TEXT("Phase0RanStack still has the compile entry"),
        FPluginState::Get().Phase0RanStack().Num(), 1);

    // Node graph unchanged.
    TestEqual(TEXT("Event graph node count unchanged after refused undo"),
        EventGraph->Nodes.Num(), NodesBeforeUndo);

    // Clean up so the next test starts with empty stacks.
    ResetBpirUndoStacks();
    return true;
}


// Test 2: positive path — a compile that did NOT run Phase 0 (e.g. InsertCodeAfterNode)
// pushes false on Phase0RanStack and undo_last_bpir pops both stacks successfully.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirUndoLastBpirSucceedsWhenNoPhase0Test,
    "PinWright.blueprint.undo_last_bpir.SucceedsWhenNoPhase0",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirUndoLastBpirSucceedsWhenNoPhase0Test::RunTest(const FString& Parameters)
{
    // Reset undo stacks so the test sees a deterministic baseline.
    ResetBpirUndoStacks();

    FUndoTestBlueprint Fixture = CreateUndoTestBlueprint(*this, TEXT("BpirUndoNoPhase0"));
    const FString& AssetPath = Fixture.AssetPath;

    ON_SCOPE_EXIT
    {
        if (Fixture.BP)
        {
            if (UPackage* BlueprintPackage = Fixture.BP->GetOutermost())
            {
                BlueprintPackage->SetDirtyFlag(false);
            }
        }
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
    };

    if (!Fixture.BP || !Fixture.EventGraph)
    {
        return true;
    }

    // Simulate a no-Phase-0 compile by pushing directly onto the stacks. This is the
    // bookkeeping pattern that InsertCodeAfterNode uses (and the path the handler
    // forwards to for some entry kinds). The positive-undo path is identical regardless
    // of how the false bit got onto the stack.
    TArray<FGuid> FakeCreatedGuids;
    FakeCreatedGuids.Add(FGuid::NewGuid());
    FPluginState::Get().NodeCreationStack().Add(FakeCreatedGuids);
    FPluginState::Get().Phase0RanStack().Add(false);

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);

        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.undo_last_bpir handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.undo_last_bpir"), Payload, Capture));
        TestTrue(TEXT("Response was sent"), Capture.bWasCalled);
        TestTrue(TEXT("undo_last_bpir returned success"), Capture.bSuccess);
    }

    // Both stacks must be popped together.
    TestEqual(TEXT("NodeCreationStack popped"),
        FPluginState::Get().NodeCreationStack().Num(), 0);
    TestEqual(TEXT("Phase0RanStack popped in lock-step"),
        FPluginState::Get().Phase0RanStack().Num(), 0);

    return true;
}


// Test 3: the documented forward path (E-add-event-then-default-compile-bpir-unundoable).
// A real production compile_bpir in mode:"extend" skips the Phase 0 sweep, so it pushes
// false on Phase0RanStack and a following undo_last_bpir SUCCEEDS — proving the
// "use mode:extend to keep undo working" guidance added to blueprint.bpir-gotchas.md.
// This drives the production handler end-to-end (not a simulated stack push), so it fails
// if the bPhase0Ran = (Mode == Replace) contract is ever reverted to sweep in extend mode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirUndoLastBpirSucceedsAfterExtendTest,
    "PinWright.blueprint.undo_last_bpir.SucceedsAfterExtend",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirUndoLastBpirSucceedsAfterExtendTest::RunTest(const FString& Parameters)
{
    // Reset undo stacks so the test sees a deterministic baseline.
    ResetBpirUndoStacks();

    FUndoTestBlueprint Fixture = CreateUndoTestBlueprint(*this, TEXT("BpirUndoExtend"));
    const FString& AssetPath = Fixture.AssetPath;

    ON_SCOPE_EXIT
    {
        if (Fixture.BP)
        {
            if (UPackage* BlueprintPackage = Fixture.BP->GetOutermost())
            {
                BlueprintPackage->SetDirtyFlag(false);
            }
        }
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
    };

    if (!Fixture.BP || !Fixture.EventGraph)
    {
        return true;
    }

    // Step 1: seed an entry plus an initial body via a default (Replace) compile_bpir.
    // Phase 0 runs here, which is what makes the seed itself unundoable; the extend in
    // Step 2 is the part the test proves stays undoable. BeginPlay is a real overridable
    // AActor UFunction (ReceiveBeginPlay) — `entry event <Name>` routes through
    // SetupBuiltinEvent, which refuses any name that is not an existing parent UFunction,
    // so a made-up event name would fail the seed compile and leave the stacks empty.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("code"), TEXT(
            "entry event BeginPlay() {\n"
            "    call PrintString(InString: \"a\")\n"
            "}\n"));
        // mode omitted → default append (Replace).

        FTestResponseCapture Capture;
        TestTrue(TEXT("seed compile_bpir handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture));
        TestTrue(TEXT("seed compile_bpir succeeded"), Capture.bSuccess);
    }

    // Seed pushes exactly one entry, and Replace mode runs Phase 0 → top is true.
    // Guard the stack depth before reading Last(): if the seed compile ever fails to push
    // (e.g. a future regression rejects the event), bail with the failed TestEqual rather
    // than letting TArray::Last() assert on an empty array and take the whole suite down.
    if (!TestEqual(TEXT("Phase0RanStack has one entry (seed)"),
        FPluginState::Get().Phase0RanStack().Num(), 1))
    {
        return true;
    }
    TestTrue(TEXT("seed compile ran Phase 0 (default Replace mode)"),
        FPluginState::Get().Phase0RanStack().Last());

    // Step 2: append more body to the same entry with mode:"extend" — the documented
    // undoable forward path. This targets the SAME BeginPlay entry so extend mode finds
    // its existing exec chain and appends after the terminal pin. This must NOT run Phase 0.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("code"), TEXT(
            "entry event BeginPlay() {\n"
            "    call PrintString(InString: \"b\")\n"
            "}\n"));
        Payload->SetStringField(TEXT("mode"), TEXT("extend"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("extend compile_bpir handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture));
        TestTrue(TEXT("extend compile_bpir succeeded"), Capture.bSuccess);
    }

    // The extend compile pushes a second entry (seed + extend), and Phase 0 is skipped
    // → top is false. Guard the depth before Last() for the same reason as Step 1.
    if (!TestEqual(TEXT("Phase0RanStack has two entries (seed + extend)"),
        FPluginState::Get().Phase0RanStack().Num(), 2))
    {
        return true;
    }
    TestFalse(TEXT("extend compile did NOT run Phase 0 (Phase0RanStack top is false)"),
        FPluginState::Get().Phase0RanStack().Last());

    // Step 3: undo_last_bpir must SUCCEED — the forward path is undoable, unlike the
    // default-mode compile in Test 1 that refuses with UNDO_NOT_REVERSIBLE.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);

        FTestResponseCapture Capture;
        TestTrue(TEXT("undo_last_bpir handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.undo_last_bpir"), Payload, Capture));
        TestTrue(TEXT("Response was sent"), Capture.bWasCalled);
        TestTrue(TEXT("undo_last_bpir succeeded after extend-mode compile"), Capture.bSuccess);
    }

    // Clean up so the next test starts with empty stacks.
    ResetBpirUndoStacks();
    return true;
}
