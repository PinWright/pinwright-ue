// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared scaffold for CRIR round-trip automation tests.
// One canonical model-controller lookup and one CRLF-normalization helper,
// pulled out of the per-file anonymous namespaces that previously declared
// near-identical `GetFirstModelController_<Suffix>` duplicates.
#pragma once

#include "CoreMinimal.h"
#include "CRIR/CRIRCompiler.h"
#include "CRIR/CRIRDecompiler.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Utils/AssetUtils.h"
#include "Utils/ControlRigBlueprintCompat.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"

/// Returns the controller for the first user-facing RigVM model on the BP,
/// or nullptr if the BP, its client, or its model list is unavailable.
inline URigVMController* GetFirstModelController(UControlRigBlueprint* BP, URigVMGraph*& OutModel)
{
    OutModel = nullptr;
    if (!BP) { return nullptr; }
    FRigVMClient* Client = GetControlRigRigVMClient(BP);
    if (!Client) { return nullptr; }
    TArray<URigVMGraph*> Models = Client->GetAllModels(/*bIncludeFunctionLibrary*/ false, /*bRecursive*/ false);
    if (Models.Num() == 0) { return nullptr; }
    OutModel = Models[0];
    return Client->GetController(OutModel);
}

/// Strips CRLF line endings to LF so two CRIR texts emitted on platforms with
/// differing line-ending conventions compare byte-equal.
inline FString NormalizeCRIRLineEndings(const FString& Text)
{
    return Text.Replace(TEXT("\r\n"), TEXT("\n"));
}

/// Drives the canonical CRIR graph round-trip used by the node-graph round-trip
/// tests: create two empty Control Rig BPs, build a ground-truth source graph
/// via `BuildSourceGraph`, decompile #1, compile that text into the fresh
/// target, decompile #2, and assert the two CRIR texts are byte-equal.
///
/// `Tag` distinguishes the asset names and assertion messages between tests.
/// `BuildSourceGraph` receives the source model's controller (and model) and is
/// where each test installs its distinguishing fixture (nodes, wires, etc.).
///
/// Returns false on any failure — asset creation, controller lookup, decompile
/// or compile. Asset creation used to be treated as a skip (AddInfo + true);
/// see the comment at the creation call below for why that is wrong here.
inline bool RunCRIRRoundTrip(
    FAutomationTestBase& Test,
    const FString& Tag,
    TFunctionRef<void(URigVMController*, URigVMGraph*)> BuildSourceGraph)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SrcName = FString::Printf(TEXT("CR_%s_Src_%s"), *Tag, *Guid);
    const FString TgtName = FString::Printf(TEXT("CR_%s_Tgt_%s"), *Tag, *Guid);
    const FString PackageDir = TEXT("/Game/PinWrightTests");
    const FString SourcePath = FString::Printf(TEXT("%s/%s"), *PackageDir, *SrcName);
    const FString TargetPath = FString::Printf(TEXT("%s/%s"), *PackageDir, *TgtName);
    const FString TargetObject = FString::Printf(TEXT("%s.%s"), *TargetPath, *TgtName);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    // Hard failure, not a skip. `McpCreateControlRigBlueprint` is PinWright's own
    // production helper (Utils/AssetUtils.cpp) and it depends on nothing optional:
    // ControlRig / ControlRigDeveloper / RigVM are unconditional dependencies of
    // this module (PinWright.Build.cs), so a null return is a regression in the
    // plugin, never a host-content difference. The repo's skip convention —
    // PINWRIGHT_SKIP_IF_FIXTURE_MISSING in Tests/TestUtils.h — is reserved for a
    // missing Lyra/host asset, and it announces itself with a FIXTURE-SKIP note.
    // Returning true here instead turned a broken creation helper into a silent
    // pass for every round-trip this scaffold drives.
    FString CreateError;
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(McpCreateControlRigBlueprint(
        SrcName, PackageDir, /*TargetSkeleton*/ nullptr, CreateError));
    if (!SourceBP)
    {
        Test.AddError(FString::Printf(
            TEXT("CRIR %s round-trip: McpCreateControlRigBlueprint failed for the source CR BP (%s)."),
            *Tag, *CreateError));
        return false;
    }

    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(McpCreateControlRigBlueprint(
        TgtName, PackageDir, /*TargetSkeleton*/ nullptr, CreateError));
    if (!TargetBP)
    {
        Test.AddError(FString::Printf(
            TEXT("CRIR %s round-trip: McpCreateControlRigBlueprint failed for the target CR BP (%s)."),
            *Tag, *CreateError));
        return false;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    Test.TestNotNull(TEXT("source model controller available"), SourceController);
    if (!SourceController || !SourceModel)
    {
        return false;
    }

    BuildSourceGraph(SourceController, SourceModel);

    // Decompile #1: source ground truth.
    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    Test.TestTrue(FString::Printf(TEXT("decompile #1 succeeds (errorCode='%s')"), *Result1.ErrorCode),
        Result1.bSuccess);
    if (!Result1.bSuccess)
    {
        return false;
    }

    // Compile into the (already-empty) fresh target.
    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    Test.TestTrue(FString::Printf(TEXT("compile to target succeeds (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        return false;
    }

    // Decompile #2: round-tripped target.
    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    Test.TestTrue(FString::Printf(TEXT("decompile #2 succeeds (errorCode='%s')"), *Result2.ErrorCode),
        Result2.bSuccess);
    if (!Result2.bSuccess)
    {
        return false;
    }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    Test.TestEqual(TEXT("CRIR text round-trips byte-equal"), Normalized2, Normalized1);
    return true;
}
