// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirCompilePreexistingErrorsRepair.cpp
//
// Regression test for E-compile-bpir-preexisting-errors-block-repair.
//
// compile_bpir runs a whole-Blueprint compile after a successful placement and, by
// default, rolls the placement back when the BP does not compile — even when every
// failing error lives in OTHER graphs the call never touched (an already-broken BP).
// That discards a good edit and blocks incremental repair. The opt-in
// `allowPreexistingErrors` flag keeps a valid placement when it introduced no NEW
// compile error versus a pre-placement baseline, while still honestly reporting
// compiled:false (so it never regresses B-false-compile-success).
//
// Fixture (built in-code, uses no external example/Lyra content): an AActor Blueprint
// whose ReceiveBeginPlay event drives a call node that has real exec pins (built
// resolved to PrintString, then wired) whose FunctionReference is swapped to an
// unresolvable name WITHOUT ReconstructNode — the pins/wiring survive, but at compile
// time GetTargetFunction() returns null → a deterministic whole-BP BS_Error in a graph
// the repair call never touches.
//
// Part A (default, no flag): a valid `entry function RepairFunc()` placement returns
//   BLUEPRINT_COMPILE_FAILED and is rolled back — today's behavior, unchanged.
// Part B (allowPreexistingErrors:true): the SAME placement is KEPT — success:true,
//   compiled:false, preexistingErrorsOnly:true, and the created nodes still resolve in
//   the Blueprint (proving no rollback).
//
// Counterfactual: revert the handler change and Part B's placement rolls back too, so
// the bSuccess / preexistingErrorsOnly / node-persistence assertions fail.

#include "Misc/AutomationTest.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"
#include "CompilerTestUtils.h"
#include "BpirGraphTestHelpers.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirCompilePreexistingErrorsRepairTest,
    "PinWright.blueprint.compile_bpir.PreexistingErrorsRepair",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

namespace
{
    // The valid, self-contained placement used by both parts. It lands in a NEW
    // function graph and never touches the deliberately-broken BeginPlay chain, so it
    // compiles clean on its own — the only reason the whole BP still fails is the
    // pre-existing error.
    const TCHAR* const GRepairBpir =
        TEXT("entry function RepairFunc() {\n")
        TEXT("    call PrintString(InString: \"repaired\")\n")
        TEXT("}\n");
}

bool FBpirCompilePreexistingErrorsRepairTest::RunTest(const FString& Parameters)
{
    // Builds an on-disk AActor Blueprint carrying a deterministic pre-existing whole-BP
    // compile error. Returns nullptr (after recording the failing sub-assertion) if any
    // step fails — a missing fixture is a FAILURE, never a silent skip.
    auto BuildBrokenActorBP = [this](const FString& AssetPath) -> UBlueprint*
    {
        UPackage* Pkg = CreatePackage(*AssetPath);
        if (!TestNotNull(TEXT("Package created"), Pkg))
        {
            return nullptr;
        }

        UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Pkg,
            FName(*FPackageName::GetLongPackageAssetName(AssetPath)),
            BPTYPE_Normal, UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        if (!TestNotNull(TEXT("Blueprint created"), BP))
        {
            return nullptr;
        }
        BP->SetFlags(RF_Transactional);

        UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(BP);
        if (!TestNotNull(TEXT("EventGraph exists"), EventGraph))
        {
            return nullptr;
        }
        EventGraph->SetFlags(RF_Transactional);

        // Reachable-from-BeginPlay call node with real exec pins, then poisoned to an
        // unresolvable function reference (kept un-reconstructed so pins/wiring survive).
        UK2Node_Event* BeginPlay = BpirGraphTestHelpers::EnsureBeginPlayNode(EventGraph);
        UK2Node_CallFunction* BadCall = CompilerTestUtils::SpawnPrintStringCall(EventGraph, 320, 0);
        if (!TestNotNull(TEXT("BeginPlay event node created"), BeginPlay)
            || !TestNotNull(TEXT("Reachable call node created"), BadCall))
        {
            return nullptr;
        }
        BpirGraphTestHelpers::WireExec(BeginPlay, BadCall);
        BadCall->FunctionReference.SetExternalMember(
            FName(TEXT("PW_NoSuchFunction_Preexisting_ZZZ")), AActor::StaticClass());

        // Guard: the fixture must genuinely fail the whole-BP compile, else the test
        // below would prove nothing. A clean compile here is a fixture FAILURE.
        const BlueprintHandlerUtils::FBlueprintCompileDiagnostics Baseline =
            BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
        TestFalse(TEXT("fixture: Blueprint carries a pre-existing whole-BP compile error"),
            Baseline.bCompiled);
        TestTrue(TEXT("fixture: baseline compile reported at least one error"),
            Baseline.Errors.Num() > 0);

        return BP;
    };

    // ---- Part A: default (no flag) rolls the valid placement back ----
    {
        const FString AssetPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/BpirPreexistingErrors_Default_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UBlueprint* BP = BuildBrokenActorBP(AssetPath);
        ON_SCOPE_EXIT
        {
            if (BP)
            {
                if (UPackage* P = BP->GetOutermost()) { P->SetDirtyFlag(false); }
            }
            PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        };
        if (!BP)
        {
            return true;
        }

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("code"), GRepairBpir);
        // allowPreexistingErrors omitted → default false.

        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.compile_bpir handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture));
        TestTrue(TEXT("Default: response was sent"), Capture.bWasCalled);
        TestFalse(TEXT("Default: valid placement is rolled back (structured error)"),
            Capture.bSuccess);
        // The valid RepairFunc emits fine, so the failure is the whole-BP compile
        // rollback path — BLUEPRINT_COMPILE_FAILED, not the emit-time COMPILE_FAILED.
        TestEqual(TEXT("Default: error code is BLUEPRINT_COMPILE_FAILED"),
            Capture.ErrorCode, FString(TEXT("BLUEPRINT_COMPILE_FAILED")));
        if (TestNotNull(TEXT("Default: compile failure retains its diagnostics payload"),
                Capture.Result.Get()))
        {
            bool bCompiled = true;
            TestTrue(TEXT("Default: failure payload contains compiled"),
                Capture.Result->TryGetBoolField(TEXT("compiled"), bCompiled));
            TestFalse(TEXT("Default: failure payload reports compiled:false"), bCompiled);
            TestTrue(TEXT("Default: failure payload contains compile status"),
                Capture.Result->HasField(TEXT("status")));
            TestTrue(TEXT("Default: failure payload contains compile errors"),
                Capture.Result->HasField(TEXT("errors")));
            TestTrue(TEXT("Default: failure payload contains compile warnings"),
                Capture.Result->HasField(TEXT("warnings")));
        }
    }

    // ---- Part B: allowPreexistingErrors:true keeps the valid placement ----
    {
        const FString AssetPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/BpirPreexistingErrors_Allowed_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UBlueprint* BP = BuildBrokenActorBP(AssetPath);
        ON_SCOPE_EXIT
        {
            if (BP)
            {
                if (UPackage* P = BP->GetOutermost()) { P->SetDirtyFlag(false); }
            }
            PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(AssetPath));
        };
        if (!BP)
        {
            return true;
        }

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        Payload->SetStringField(TEXT("code"), GRepairBpir);
        Payload->SetBoolField(TEXT("allowPreexistingErrors"), true);

        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.compile_bpir handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.compile_bpir"), Payload, Capture));
        TestTrue(TEXT("Allowed: response was sent"), Capture.bWasCalled);
        TestTrue(TEXT("Allowed: valid placement is KEPT (success)"), Capture.bSuccess);

        if (Capture.bSuccess && TestNotNull(TEXT("Allowed: success payload present"), Capture.Result.Get()))
        {
            // Honest reporting: the BP still does not compile — never a false success.
            bool bCompiled = true;
            Capture.Result->TryGetBoolField(TEXT("compiled"), bCompiled);
            TestFalse(TEXT("Allowed: still reports compiled:false (BP not yet compilable)"), bCompiled);

            bool bPreexistingOnly = false;
            Capture.Result->TryGetBoolField(TEXT("preexistingErrorsOnly"), bPreexistingOnly);
            TestTrue(TEXT("Allowed: reports preexistingErrorsOnly:true"), bPreexistingOnly);

            // Prove the placement PERSISTED (was not rolled back): a created node GUID
            // still resolves in the Blueprint. After a rollback it would not.
            const TArray<TSharedPtr<FJsonValue>>* Created = nullptr;
            TestTrue(TEXT("Allowed: createdNodes present"),
                Capture.Result->TryGetArrayField(TEXT("createdNodes"), Created));
            if (Created && Created->Num() > 0)
            {
                FGuid FirstGuid;
                const bool bParsed = FGuid::Parse((*Created)[0]->AsString(), FirstGuid);
                TestTrue(TEXT("Allowed: first created node id parses"), bParsed);
                if (bParsed)
                {
                    TestNotNull(TEXT("Allowed: placed node still present (not rolled back)"),
                        FBlueprintEditorUtils::GetNodeByGUID(BP, FirstGuid));
                }
            }
            else
            {
                AddError(TEXT("Allowed: expected at least one created node from RepairFunc placement"));
            }
        }
    }

    return true;
}
