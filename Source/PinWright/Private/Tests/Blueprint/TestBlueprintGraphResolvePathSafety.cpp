// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBlueprintGraphResolvePathSafety.cpp - regression coverage for the blueprint.graph.* chokepoint
// of B-createpackage-unvalidated-paths-plugin-wide.
//
// WHAT WAS WRONG. BlueprintGraphHelpers::ResolveBlueprintAndGraph is the single entry every one of
// the 24 blueprint.graph.* call sites goes through, and it handed the caller's path straight to
// LoadObject<UBlueprint>. A load is the same door to the same Fatal that CreatePackage is:
// StaticLoadObjectInternal calls ResolveName2(..., Create=true) (UObjectGlobals.cpp:1427), which
// calls CreatePackage on the partial name (:1310), and CreatePackage logs a package name containing
// "//" at **Fatal** (:1094-1096) - a verbosity that is not compiled out in any configuration. So
// `path: "/A//B"` did not fail the call: it ended the editor PROCESS and every unsaved package in
// it, and the `if (!OutBlueprint)` below the load could never fire because nothing after the load
// was reached.
//
// NOTHING ABOVE THE LOAD STOPPED IT, and the reason is worth stating because it looks like it
// should have. BlueprintHandlerUtils::ResolveBlueprintPath runs first, and it neither loads nor
// sanitizes: per alias field it calls FindBlueprintNormalizedPath (Utils/AssetUtils.cpp), which
// prepends "/Game" to anything IsValidMountPoint rejects and then asks
// UEditorAssetLibrary::DoesAssetExist about the result. That probe COLLAPSES the duplicate slash
// internally, so it answers about a different, clean path - and on a miss ResolveBlueprintPath
// assigns `ResolvedPath = Req`, the caller's RAW string. The existence probe finding nothing is
// what let the lethal string through, not what caught it.
//
// WHY THIS TEST CALLS THE RESOLVER DIRECTLY AND NOT A VERB. The guard is exactly Contains("//") -
// nothing wider is correct over a resolver that legitimately accepts a bare name, a package path,
// an object path and a "_C" path (see CanReachCreatePackageFatal's contract in Utils/PathUtils.h).
// There is consequently NO non-lethal payload that a build with the guard answers differently from
// one without it, so this asserts the post-fix CONTRACT and is not a reproduction: on a reverted
// build the "//" case ends the process instead of going red. Driving it through a blueprint.graph.*
// verb would add the dispatcher and the verb's own unguarded arguments to the crash surface without
// adding any coverage, so it is not done.
//
// EVERY REFUSAL CASE IS PAIRED WITH A CONTROL, and here the control is the load-bearing half: the
// refusal returns false, and so does an ordinary not-found, so without a case that gets PAST the
// guard a resolver hard-wired to refuse everything would satisfy the rest of the file. The control
// asserts a well-formed path reaches the load and is answered ASSET_NOT_FOUND - a different error
// code, from below the guard - which is what proves the guard did not widen into a general refusal.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Handlers/Blueprint/BlueprintGraphHelpers.h"
#include "Handlers/HandlerContext.h"

class UBlueprint;
class UEdGraph;

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace BlueprintGraphResolvePathSafetyHelpers
{
    // Drives the resolver once and reports what it answered. bGraphRequired is false throughout:
    // every case here is decided by the path, above any graph lookup.
    struct FResolveOutcome
    {
        bool bOk = false;
        UBlueprint* Blueprint = nullptr;
        FResponseCapture Capture;
    };

    inline void Resolve(FResolveOutcome& Out, const TCHAR* RequestId, const TCHAR* Path)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), Path);

        FHandlerContext Ctx = FHandlerContext::MakeTestContextWithCapture(
            RequestId, TEXT("blueprint.graph.get_nodes"), Payload, &Out.Capture);

        UEdGraph* Graph = nullptr;
        Out.bOk = BlueprintGraphHelpers::ResolveBlueprintAndGraph(
            Ctx, Out.Blueprint, Graph, /*bGraphRequired=*/false);
    }
}

// ============================================================================
// A "//" path is refused before the load, with a code and a message that name the rule
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphResolveRefusesDoubleSlashPathTest,
    "PinWright.blueprint.graph.resolve_path.RefusesDoubleSlash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphResolveRefusesDoubleSlashPathTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintGraphResolvePathSafetyHelpers;

    // Every shape the resolver accepts, each carrying the one lethal property. The bare-name case
    // has no leading slash and no dot and is still a kill: ResolveName2 returns early with no
    // delimiter, and StaticLoadObjectInternal re-enters itself with InName + "." +
    // GetShortName(InName), so the second pass reaches CreatePackage.
    const TCHAR* LethalPaths[] = {
        TEXT("/Game//PinWrightMissing/BP_GraphPathSafety"),
        TEXT("/Game/PinWrightMissing//BP_GraphPathSafety"),
        TEXT("/Game//PinWrightMissing/BP_GraphPathSafety.BP_GraphPathSafety"),
        TEXT("PinWrightMissing//BP_GraphPathSafety"),
    };

    for (const TCHAR* LethalPath : LethalPaths)
    {
        FResolveOutcome Outcome;
        Resolve(Outcome, TEXT("req-bp-graph-resolve-double-slash"), LethalPath);

        TestFalse(*FString::Printf(TEXT("'%s' is refused"), LethalPath), Outcome.bOk);
        TestNull(*FString::Printf(TEXT("'%s' resolves no blueprint"), LethalPath),
            Outcome.Blueprint);
        TestTrue(*FString::Printf(TEXT("'%s': the refusal was actually dispatched"), LethalPath),
            Outcome.Capture.bWasCalled);
        TestFalse(*FString::Printf(TEXT("'%s': the response is an error, not a fake success"),
            LethalPath), Outcome.Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("'%s' is refused as a malformed path argument"),
            LethalPath), Outcome.Capture.ErrorCode, FString(TEXT("INVALID_BLUEPRINT_PATH")));
        // The message must name the rule, not merely fail: this is what lets support tell a
        // refusal apart from an ordinary not-found in a log.
        TestTrue(*FString::Printf(TEXT("'%s': the refusal names the '//' rule"), LethalPath),
            Outcome.Capture.Message.Contains(TEXT("//")));
    }

    return true;
}

// ============================================================================
// The guard widened the existing empty-path condition rather than replacing it, and did not
// widen into a general refusal
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintGraphResolveStillAcceptsWellFormedPathsTest,
    "PinWright.blueprint.graph.resolve_path.StillAcceptsWellFormedPaths",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintGraphResolveStillAcceptsWellFormedPathsTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintGraphResolvePathSafetyHelpers;

    // CONTROL. A well-formed path that names no asset must get PAST the guard and be answered by
    // the load below it. ASSET_NOT_FOUND - a different code, emitted from below the guard - is what
    // distinguishes "the guard let this through" from "the guard refuses everything", which the
    // bare false return cannot. Safe on any build: no "//", so nothing here reaches the Fatal.
    {
        FResolveOutcome Outcome;
        Resolve(Outcome, TEXT("req-bp-graph-resolve-absent"),
            TEXT("/Game/PinWrightMissing/BP_AbsentGraphProbe"));

        TestFalse(TEXT("an absent blueprint still fails"), Outcome.bOk);
        TestEqual(TEXT("an absent blueprint fails BELOW the guard, as ASSET_NOT_FOUND"),
            Outcome.Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    }

    // ORDERING WITNESS. The "//" half was added to the existing IsEmpty condition; a missing path
    // must still be refused, and still with its own message rather than the "//" one.
    {
        FResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

        FHandlerContext Ctx = FHandlerContext::MakeTestContextWithCapture(
            TEXT("req-bp-graph-resolve-empty"), TEXT("blueprint.graph.get_nodes"),
            Payload, &Capture);

        UBlueprint* Blueprint = nullptr;
        UEdGraph* Graph = nullptr;
        const bool bOk = BlueprintGraphHelpers::ResolveBlueprintAndGraph(
            Ctx, Blueprint, Graph, /*bGraphRequired=*/false);

        TestFalse(TEXT("a missing path is still refused"), bOk);
        TestEqual(TEXT("a missing path still answers INVALID_BLUEPRINT_PATH"),
            Capture.ErrorCode, FString(TEXT("INVALID_BLUEPRINT_PATH")));
        TestFalse(TEXT("a missing path is not reported as a '//' violation"),
            Capture.Message.Contains(TEXT("//")));
    }

    return true;
}
