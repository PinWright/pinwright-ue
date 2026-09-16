// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board E-material-verbs-have-no-shader-compile-signal.
//
// THE DEFECT. Exactly one verb (material.authoring.compile_material) asked the engine whether a
// material's SHADER compiles. Every other material verb returned a payload describing the GRAPH
// write - blocksCompiled, expressionsCreated, nodeId, "Nodes connected." - so a material with
// malformed Custom HLSL wrote a valid .uasset, passed asset.save, read back with the right domain
// and wired mainInputs, and rendered nothing. Nothing in any response said so, and nothing routed
// the author to the one verb that would have.
//
// WHAT IS ASSERTED HERE, in three directions:
//   1. A material whose HLSL cannot compile reports status "failed" WITH the error text.
//   2. A material that compiles reports status "completed" with no errors and no default-material
//      fallback - so (1) is discriminating rather than a constant.
//   3. A production graph-write verb that never mentioned shaders publishes the block at all. This
//      is the wiring half: (1) and (2) exercise the helper, (3) exercises the funnel every material
//      verb now ends on.
//
// COUNTERFACTUAL. Revert MaterialShaderState.h and "shaderCompile block is present" fails in (3)
// and the status assertions fail in (1) and (2), the field not existing. Revert only the
// AddMaterialVerification funnel in MaterialGraphHandler.cpp/MaterialAuthoringHandler.cpp and (3)
// alone fails, which is the distinction that names which half broke.
//
// VACUITY. In a headless editor the permutation compile jobs are deferred until a material is first
// DRAWN, so nothing here can be measured off a passive read: both helper tests go through
// ProbeAndWait, which submits synchronously. A host that cannot compile shaders at all (a
// SkipShaderCompilation build, AllowShaderCompiling false) yields "notCompiled" for the broken and
// the valid material alike; that is reported through PinWrightTestSkip rather than asserted past.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Material/MaterialShaderStateTestFixtures.h"
#include "Handlers/Material/MaterialShaderState.h"

namespace
{
    // The `shaderCompile` block as a verb would publish it, read back off the JSON rather than off
    // the struct: the wire shape is what the defect was about.
    const TSharedPtr<FJsonObject>* PublishAndReadBlock(
        const PinWright::MaterialShaderState::FState& State,
        TSharedPtr<FJsonObject>& OutResult)
    {
        OutResult = MakeShared<FJsonObject>();
        PinWright::MaterialShaderState::AddReport(OutResult, State);

        const TSharedPtr<FJsonObject>* Block = nullptr;
        return OutResult->TryGetObjectField(TEXT("shaderCompile"), Block) ? Block : nullptr;
    }
}

// ---------------------------------------------------------------------------------------------
// 1. Broken HLSL -> failed, with the error text
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialShaderStateBrokenHlslTest,
    "PinWright.material.shader_state.BrokenHlslReportsFailedWithTheErrorText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialShaderStateBrokenHlslTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterial* Material =
        PinWrightMaterialShaderStateTestFixtures::MakeBrokenHlslMaterial(
            TEXT("ShaderStateBroken"), AssetPath);
    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    const PinWright::MaterialShaderState::FState State =
        PinWright::MaterialShaderState::ProbeAndWait(Material);

    if (State.Status != PinWright::MaterialShaderState::EStatus::Failed)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("shader-compile-unavailable"),
            FString::Printf(TEXT("The broken material reported '%s' rather than 'failed', so this "
                "host did not run the platform shader compiler and the failure this test asserts "
                "on could not be produced."),
                PinWright::MaterialShaderState::ToWire(State.Status)));
        CleanupTestAsset(AssetPath);
        return true;
    }

    TestTrue(TEXT("a failed compile is not a success"), !State.Succeeded());
    TestTrue(TEXT("a failed compile is flagged failed"), State.Failed());
    TestTrue(TEXT("a failed compile carries at least one HLSL error"), State.Errors.Num() > 0);
    TestTrue(TEXT("a failed compile means the Default Material is what renders"),
        State.bRendersDefaultMaterial);

    TSharedPtr<FJsonObject> Result;
    const TSharedPtr<FJsonObject>* Block = PublishAndReadBlock(State, Result);
    if (!TestTrue(TEXT("response carries the shaderCompile block"), Block != nullptr))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    FString Status;
    if (TestTrue(TEXT("shaderCompile carries status"),
            (*Block)->TryGetStringField(TEXT("status"), Status)))
    {
        TestEqual(TEXT("status is failed"), Status, FString(TEXT("failed")));
    }

    bool bSucceeded = true;
    (*Block)->TryGetBoolField(TEXT("succeeded"), bSucceeded);
    TestFalse(TEXT("succeeded is false"), bSucceeded);

    bool bFailed = false;
    (*Block)->TryGetBoolField(TEXT("failed"), bFailed);
    TestTrue(TEXT("failed is true"), bFailed);

    bool bRendersDefault = false;
    (*Block)->TryGetBoolField(TEXT("rendersDefaultMaterial"), bRendersDefault);
    TestTrue(TEXT("rendersDefaultMaterial is true"), bRendersDefault);

    // The error TEXT, not merely a count: a verdict a caller cannot act on is the defect one layer
    // down. This is the same signature TestCompileMaterialShaderErrors matches.
    const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
    if (TestTrue(TEXT("shaderCompile carries an errors array"),
            (*Block)->TryGetArrayField(TEXT("errors"), Errors)) && Errors)
    {
        TestTrue(TEXT("errors is non-empty"), Errors->Num() > 0);

        bool bMatched = false;
        for (const TSharedPtr<FJsonValue>& Value : *Errors)
        {
            FString ErrorText;
            if (Value->TryGetString(ErrorText) &&
                (ErrorText.Contains(TEXT("subscript")) ||
                 ErrorText.Contains(TEXT("FDFMatrix")) ||
                 ErrorText.Contains(TEXT("operator")) ||
                 ErrorText.Contains(TEXT("LocalToWorld"))))
            {
                bMatched = true;
                break;
            }
        }
        TestTrue(TEXT("an error matches the invalid-HLSL signature"), bMatched);
    }

    // A failed status must name the remedy, or the caller is back to guessing which verb to reach
    // for - the half of this defect that is about routing rather than measurement.
    FString Hint;
    TestTrue(TEXT("a failed compile carries a hint"),
        (*Block)->TryGetStringField(TEXT("hint"), Hint) && !Hint.IsEmpty());

    // The capture contract consumes the same measured state. This is not a synthetic fixture:
    // ProbeAndWait above produced the compile errors from the real material resource.
    PinWright::MaterialShaderState::FCaptureReadiness CaptureReadiness;
    CaptureReadiness.AddSubject(AssetPath, State);
    TSharedPtr<FJsonObject> CaptureResult = MakeShared<FJsonObject>();
    TestFalse(TEXT("a measured shader failure is rejected by the default capture policy"),
        PinWright::MaterialShaderState::ApplyCaptureFallbackPolicy(
            CaptureResult, CaptureReadiness, false));

    const TSharedPtr<FJsonObject>* MaterialReadiness = nullptr;
    if (TestTrue(TEXT("the failed fixture publishes materialReadiness"),
            CaptureResult->TryGetObjectField(TEXT("materialReadiness"), MaterialReadiness)) &&
        MaterialReadiness)
    {
        bool bFallbackOccurred = false;
        (*MaterialReadiness)->TryGetBoolField(TEXT("fallbackOccurred"), bFallbackOccurred);
        TestTrue(TEXT("the failed fixture is marked as a Default Material fallback"),
            bFallbackOccurred);
    }

    CleanupTestAsset(AssetPath);
    return true;
}

// ---------------------------------------------------------------------------------------------
// 2. Valid material -> completed, no errors
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialShaderStateValidMaterialTest,
    "PinWright.material.shader_state.ValidMaterialReportsCompleted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialShaderStateValidMaterialTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterial* Material = PinWrightMaterialShaderStateTestFixtures::MakeCleanMaterial(
        TEXT("ShaderStateValid"), AssetPath);
    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    const PinWright::MaterialShaderState::FState State =
        PinWright::MaterialShaderState::ProbeAndWait(Material);

    // The invariant, asserted on every host regardless of what the fixture achieved: a material
    // that compiles cleanly never reports errors and never claims the Default Material renders.
    TestFalse(TEXT("a valid material is never reported as failed"), State.Failed());
    TestEqual(TEXT("a valid material collects no compile errors"), State.Errors.Num(), 0);

    if (State.Status != PinWright::MaterialShaderState::EStatus::Completed)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("shader-compile-unavailable"),
            FString::Printf(TEXT("The valid material reported '%s' rather than 'completed', so "
                "this host did not install a complete shader map and the clean-compile branch "
                "could not be asserted. The no-errors invariant above still ran."),
                PinWright::MaterialShaderState::ToWire(State.Status)));
        CleanupTestAsset(AssetPath);
        return true;
    }

    TestTrue(TEXT("a completed compile is a success"), State.Succeeded());
    TestFalse(TEXT("a completed compile does not fall back to the Default Material"),
        State.bRendersDefaultMaterial);

    TSharedPtr<FJsonObject> Result;
    const TSharedPtr<FJsonObject>* Block = PublishAndReadBlock(State, Result);
    if (!TestTrue(TEXT("response carries the shaderCompile block"), Block != nullptr))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    FString Status;
    if (TestTrue(TEXT("shaderCompile carries status"),
            (*Block)->TryGetStringField(TEXT("status"), Status)))
    {
        TestEqual(TEXT("status is completed"), Status, FString(TEXT("completed")));
    }

    bool bSucceeded = false;
    (*Block)->TryGetBoolField(TEXT("succeeded"), bSucceeded);
    TestTrue(TEXT("succeeded is true"), bSucceeded);

    const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
    if ((*Block)->TryGetArrayField(TEXT("errors"), Errors) && Errors)
    {
        TestEqual(TEXT("errors is empty"), Errors->Num(), 0);
    }

    // No remedy is published for a clean compile: a verb with nothing to warn about says nothing.
    TestFalse(TEXT("a completed compile carries no hint"), (*Block)->HasField(TEXT("hint")));

    CleanupTestAsset(AssetPath);
    return true;
}

// ---------------------------------------------------------------------------------------------
// 3. Capture fallback policy and wire shape
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialShaderStateCaptureReadinessStructureTest,
    "PinWright.material.shader_state.CaptureReadinessStructure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialShaderStateCaptureReadinessStructureTest::RunTest(const FString& Parameters)
{
    using namespace PinWright::MaterialShaderState;

    FState FailedState;
    FailedState.Status = EStatus::Failed;
    FailedState.Errors.Add(TEXT("deliberate shader error"));
    FailedState.bRendersDefaultMaterial = true;

    FCaptureReadiness FailedReadiness;
    FailedReadiness.AddSubject(TEXT("/Game/Test/M_Broken.M_Broken"), FailedState);

    TSharedPtr<FJsonObject> Rejected = MakeShared<FJsonObject>();
    TestFalse(TEXT("fallback is rejected when allowFallback is false"),
        ApplyCaptureFallbackPolicy(Rejected, FailedReadiness, false));

    const TSharedPtr<FJsonObject>* Block = nullptr;
    if (TestTrue(TEXT("materialReadiness is serialized"),
            Rejected->TryGetObjectField(TEXT("materialReadiness"), Block)) && Block)
    {
        bool bFailed = false;
        bool bCompiling = true;
        bool bUsingDefault = false;
        bool bFallback = false;
        bool bFallbackPossible = true;
        (*Block)->TryGetBoolField(TEXT("failed"), bFailed);
        (*Block)->TryGetBoolField(TEXT("compiling"), bCompiling);
        (*Block)->TryGetBoolField(TEXT("usingDefaultMaterial"), bUsingDefault);
        (*Block)->TryGetBoolField(TEXT("fallbackOccurred"), bFallback);
        (*Block)->TryGetBoolField(TEXT("fallbackPossible"), bFallbackPossible);
        TestTrue(TEXT("aggregate reports failed"), bFailed);
        TestFalse(TEXT("failed is distinct from compiling"), bCompiling);
        TestTrue(TEXT("aggregate reports Default Material use"), bUsingDefault);
        TestTrue(TEXT("aggregate reports fallback"), bFallback);
        TestFalse(TEXT("known failed fallback is not merely possible"), bFallbackPossible);

        FString Reason;
        if (TestTrue(TEXT("fallback reason is present"),
                (*Block)->TryGetStringField(TEXT("reason"), Reason)))
        {
            TestEqual(TEXT("failed shader map has a specific reason"), Reason,
                FString(TEXT("shaderMapFailed")));
        }

        const TArray<TSharedPtr<FJsonValue>>* Subjects = nullptr;
        if (TestTrue(TEXT("subjects are present"),
                (*Block)->TryGetArrayField(TEXT("subjects"), Subjects)) &&
            Subjects && TestEqual(TEXT("one subject is serialized"), Subjects->Num(), 1))
        {
            const TSharedPtr<FJsonObject>* Subject = nullptr;
            if (TestTrue(TEXT("subject is an object"),
                    (*Subjects)[0]->TryGetObject(Subject)) && Subject)
            {
                TestEqual(TEXT("subject path is retained"),
                    (*Subject)->GetStringField(TEXT("materialPath")),
                    FString(TEXT("/Game/Test/M_Broken.M_Broken")));
                TestEqual(TEXT("subject status is failed"),
                    (*Subject)->GetStringField(TEXT("status")), FString(TEXT("failed")));
                TestTrue(TEXT("subject carries compile errors"),
                    (*Subject)->GetArrayField(TEXT("errors")).Num() > 0);
            }
        }
    }

    TSharedPtr<FJsonObject> Allowed = MakeShared<FJsonObject>();
    TestTrue(TEXT("allowFallback opts into the same fallback image"),
        ApplyCaptureFallbackPolicy(Allowed, FailedReadiness, true));
    TestTrue(TEXT("allowFallback keeps an explicit warning"),
        Allowed->GetArrayField(TEXT("warnings")).Num() > 0);
    FString ShaderWarning;
    Allowed->GetArrayField(TEXT("warnings"))[0]->TryGetString(ShaderWarning);
    TestTrue(TEXT("shader fallback warning points to compile errors"),
        ShaderWarning.Contains(TEXT("subjects[].errors")));

    FState CleanState;
    CleanState.Status = EStatus::Completed;
    FCaptureReadiness CleanReadiness;
    CleanReadiness.AddSubject(TEXT("/Game/Test/M_Clean.M_Clean"), CleanState);
    TSharedPtr<FJsonObject> Clean = MakeShared<FJsonObject>();
    TestTrue(TEXT("a clean compiled material is accepted without an opt-in"),
        ApplyCaptureFallbackPolicy(Clean, CleanReadiness, false));
    TestFalse(TEXT("a clean capture has no fallback warning"), Clean->HasField(TEXT("warnings")));
    TestFalse(TEXT("a clean capture has no possible fallback"),
        Clean->GetObjectField(TEXT("materialReadiness"))->GetBoolField(TEXT("fallbackPossible")));

    const EStatus UncertainStatuses[] = {
        EStatus::NotCompiled,
        EStatus::Outstanding,
        EStatus::TimedOut
    };
    for (const EStatus UncertainStatus : UncertainStatuses)
    {
        const FString StatusName(ToWire(UncertainStatus));
        FState UncertainState;
        UncertainState.Status = UncertainStatus;
        UncertainState.bRendersDefaultMaterial = true;
        FCaptureReadiness UncertainReadiness;
        UncertainReadiness.AddSubject(
            TEXT("/Game/Test/M_Uncertain.M_Uncertain"), UncertainState);
        TSharedPtr<FJsonObject> Uncertain = MakeShared<FJsonObject>();
        TestTrue(*FString::Printf(TEXT("%s remains successful without allowFallback"),
                *StatusName),
            ApplyCaptureFallbackPolicy(Uncertain, UncertainReadiness, false));

        const TSharedPtr<FJsonObject>* UncertainBlock = nullptr;
        if (TestTrue(*FString::Printf(TEXT("%s readiness is serialized"), *StatusName),
                Uncertain->TryGetObjectField(TEXT("materialReadiness"), UncertainBlock)) &&
            UncertainBlock)
        {
            TestFalse(*FString::Printf(TEXT("%s is not a known fallback"), *StatusName),
                (*UncertainBlock)->GetBoolField(TEXT("fallbackOccurred")));
            TestTrue(*FString::Printf(TEXT("%s reports possible fallback"), *StatusName),
                (*UncertainBlock)->GetBoolField(TEXT("fallbackPossible")));
            TestFalse(*FString::Printf(TEXT("%s does not claim known Default Material use"),
                    *StatusName),
                (*UncertainBlock)->GetBoolField(TEXT("usingDefaultMaterial")));
            TestFalse(*FString::Printf(TEXT("%s has no known-fallback reason"), *StatusName),
                (*UncertainBlock)->HasField(TEXT("reason")));
            TestEqual(*FString::Printf(TEXT("%s has an uncertainty reason"), *StatusName),
                (*UncertainBlock)->GetStringField(TEXT("possibleReason")),
                FString(TEXT("shaderMapIncomplete")));

            const TArray<TSharedPtr<FJsonValue>>* UncertainSubjects = nullptr;
            if (TestTrue(*FString::Printf(TEXT("%s subject is serialized"), *StatusName),
                    (*UncertainBlock)->TryGetArrayField(
                        TEXT("subjects"), UncertainSubjects) && UncertainSubjects &&
                    UncertainSubjects->Num() == 1))
            {
                const TSharedPtr<FJsonObject>* UncertainSubject = nullptr;
                if ((*UncertainSubjects)[0]->TryGetObject(UncertainSubject) && UncertainSubject)
                {
                    TestEqual(*FString::Printf(TEXT("%s exact status is retained"), *StatusName),
                        (*UncertainSubject)->GetStringField(TEXT("status")), StatusName);
                    TestTrue(*FString::Printf(TEXT("%s subject reports possible fallback"),
                            *StatusName),
                        (*UncertainSubject)->GetBoolField(TEXT("fallbackPossible")));
                }
            }
        }
        const TArray<TSharedPtr<FJsonValue>>* UncertainWarnings = nullptr;
        TestTrue(*FString::Printf(TEXT("%s emits an uncertainty warning"), *StatusName),
            Uncertain->TryGetArrayField(TEXT("warnings"), UncertainWarnings) &&
            UncertainWarnings && UncertainWarnings->Num() > 0);
    }

    FState IntentionalDefaultState;
    IntentionalDefaultState.Status = EStatus::Completed;
    IntentionalDefaultState.bRendersDefaultMaterial = true;
    FCaptureReadiness IntentionalDefaultReadiness;
    IntentionalDefaultReadiness.AddSubject(
        TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"),
        IntentionalDefaultState);
    TSharedPtr<FJsonObject> IntentionalDefault = MakeShared<FJsonObject>();
    TestTrue(TEXT("an intentionally selected Default Material is not a fallback substitution"),
        ApplyCaptureFallbackPolicy(IntentionalDefault, IntentionalDefaultReadiness, false));
    const TSharedPtr<FJsonObject>* IntentionalDefaultBlock = nullptr;
    if (IntentionalDefault->TryGetObjectField(
            TEXT("materialReadiness"), IntentionalDefaultBlock) && IntentionalDefaultBlock)
    {
        TestTrue(TEXT("intentional Default Material use remains visible"),
            (*IntentionalDefaultBlock)->GetBoolField(TEXT("usingDefaultMaterial")));
        TestFalse(TEXT("intentional Default Material use is not labeled fallback"),
            (*IntentionalDefaultBlock)->GetBoolField(TEXT("fallbackOccurred")));
    }

    FCaptureReadiness UnusedFailedReadiness;
    UnusedFailedReadiness.AddSubject(
        TEXT("/Game/Test/M_UnusedBroken.M_UnusedBroken"), FailedState, false);
    TSharedPtr<FJsonObject> UnusedFailed = MakeShared<FJsonObject>();
    TestTrue(TEXT("a failed material in an unused slot does not reject the capture"),
        ApplyCaptureFallbackPolicy(UnusedFailed, UnusedFailedReadiness, false));
    const TSharedPtr<FJsonObject>* UnusedFailedBlock = nullptr;
    if (UnusedFailed->TryGetObjectField(
            TEXT("materialReadiness"), UnusedFailedBlock) && UnusedFailedBlock)
    {
        TestTrue(TEXT("unused slot failure remains readiness evidence"),
            (*UnusedFailedBlock)->GetBoolField(TEXT("failed")));
        TestFalse(TEXT("unused slot failure did not occur in captured pixels"),
            (*UnusedFailedBlock)->GetBoolField(TEXT("fallbackOccurred")));
    }

    FCaptureReadiness UnassignedReadiness;
    UnassignedReadiness.AddUnassignedSubject(TEXT("/Game/Test/SM_Null:materialSlot[0]"), true);
    TSharedPtr<FJsonObject> Unassigned = MakeShared<FJsonObject>();
    TestFalse(TEXT("a drawn unassigned material slot is a fallback substitution"),
        ApplyCaptureFallbackPolicy(Unassigned, UnassignedReadiness, false));
    const TSharedPtr<FJsonObject>* UnassignedBlock = nullptr;
    if (Unassigned->TryGetObjectField(TEXT("materialReadiness"), UnassignedBlock) &&
        UnassignedBlock)
    {
        TestEqual(TEXT("unassigned slot fallback has a specific reason"),
            (*UnassignedBlock)->GetStringField(TEXT("reason")),
            FString(TEXT("unassignedMaterialSlot")));
        const TArray<TSharedPtr<FJsonValue>>* UnassignedSubjects = nullptr;
        if ((*UnassignedBlock)->TryGetArrayField(TEXT("subjects"), UnassignedSubjects) &&
            UnassignedSubjects && UnassignedSubjects->Num() == 1)
        {
            const TSharedPtr<FJsonObject>* UnassignedSubject = nullptr;
            if ((*UnassignedSubjects)[0]->TryGetObject(UnassignedSubject) && UnassignedSubject)
            {
                TestEqual(TEXT("unassigned slot retains its subject identifier"),
                    (*UnassignedSubject)->GetStringField(TEXT("subjectId")),
                    FString(TEXT("/Game/Test/SM_Null:materialSlot[0]")));
            }
        }
    }

    TSharedPtr<FJsonObject> AllowedUnassigned = MakeShared<FJsonObject>();
    TestTrue(TEXT("allowFallback accepts an unassigned-slot fallback"),
        ApplyCaptureFallbackPolicy(AllowedUnassigned, UnassignedReadiness, true));
    FString UnassignedWarning;
    AllowedUnassigned->GetArrayField(TEXT("warnings"))[0]->TryGetString(UnassignedWarning);
    TestTrue(TEXT("unassigned fallback warning points to slot details"),
        UnassignedWarning.Contains(TEXT("unassignedMaterial")) &&
        !UnassignedWarning.Contains(TEXT("compile errors")));

    UStaticMesh* SourceMesh = LoadObject<UStaticMesh>(nullptr,
        TEXT("/Engine/BasicShapes/Cube.Cube"));
    UStaticMesh* MeshWithUnassignedSlot = SourceMesh
        ? DuplicateObject<UStaticMesh>(SourceMesh, GetTransientPackage())
        : nullptr;
    if (!TestNotNull(TEXT("mesh-scope fixture created"), MeshWithUnassignedSlot))
    {
        return false;
    }
    MeshWithUnassignedSlot->GetStaticMaterials().Add(FStaticMaterial(nullptr));
    UStaticMeshComponent* PreviewComponent = NewObject<UStaticMeshComponent>();
    PreviewComponent->SetStaticMesh(MeshWithUnassignedSlot);
    PreviewComponent->SetForcedLodModel(1);
    const FCaptureReadiness PreviewMeshReadiness = ProbeCaptureComponent(PreviewComponent);
    TSharedPtr<FJsonObject> PreviewMesh = MakeShared<FJsonObject>();
    TestTrue(TEXT("asset preview ignores an unassigned slot unused by the captured LOD"),
        ApplyCaptureFallbackPolicy(PreviewMesh, PreviewMeshReadiness, false));
    const TSharedPtr<FJsonObject>* PreviewBlock = nullptr;
    if (PreviewMesh->TryGetObjectField(
            TEXT("materialReadiness"), PreviewBlock) && PreviewBlock)
    {
        TestEqual(TEXT("captured component policy is explicit on the wire"),
            (*PreviewBlock)->GetStringField(TEXT("meshUsagePolicy")),
            FString(TEXT("capturedComponentSections")));
        TestEqual(TEXT("forced preview LOD is explicit on the wire"),
            (*PreviewBlock)->GetIntegerField(TEXT("renderedLodIndex")), 0);
        TestEqual(TEXT("forced preview LOD scope is explicit on the wire"),
            (*PreviewBlock)->GetStringField(TEXT("scope")),
            FString(TEXT("staticMeshForcedLod")));
    }

    const FCaptureReadiness ThumbnailMeshReadiness = ProbeCaptureAsset(
        MeshWithUnassignedSlot, ECaptureMeshUsagePolicy::ThumbnailLod0Sections);
    TSharedPtr<FJsonObject> ThumbnailMesh = MakeShared<FJsonObject>();
    TestTrue(TEXT("thumbnail LOD0 policy leaves an unreferenced slot as readiness only"),
        ApplyCaptureFallbackPolicy(ThumbnailMesh, ThumbnailMeshReadiness, false));
    const TSharedPtr<FJsonObject>* ThumbnailBlock = nullptr;
    if (ThumbnailMesh->TryGetObjectField(TEXT("materialReadiness"), ThumbnailBlock) &&
        ThumbnailBlock)
    {
        TestEqual(TEXT("thumbnail LOD0 policy is explicit on the wire"),
            (*ThumbnailBlock)->GetStringField(TEXT("meshUsagePolicy")),
            FString(TEXT("thumbnailLod0Sections")));
    }

    TSharedPtr<FJsonObject> DebugSubstitution = MakeShared<FJsonObject>();
    TestTrue(TEXT("a debug-material capture does not reject an unused subject material"),
        ApplyCaptureFallbackPolicy(DebugSubstitution, FailedReadiness, false, false));
    const TSharedPtr<FJsonObject>* DebugBlock = nullptr;
    if (DebugSubstitution->TryGetObjectField(TEXT("materialReadiness"), DebugBlock) && DebugBlock)
    {
        TestFalse(TEXT("debug material replacement does not claim fallback pixels"),
            (*DebugBlock)->GetBoolField(TEXT("fallbackOccurred")));
        TestFalse(TEXT("debug material replacement is disclosed"),
            (*DebugBlock)->GetBoolField(TEXT("subjectMaterialsRendered")));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// 4. A production graph-write verb publishes the block
// ---------------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialShaderStateGraphWriteTest,
    "PinWright.material.shader_state.GraphWriteVerbPublishesTheBlock",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialShaderStateGraphWriteTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterial* Material = PinWrightMaterialShaderStateTestFixtures::MakeSandboxMaterial(
        TEXT("ShaderStateGraphWrite"), AssetPath);
    if (!TestNotNull(TEXT("Material created"), Material))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    Material->PostEditChange();
    FAssetRegistryModule::AssetCreated(Material);

    // material.graph.add_node is the plainest write verb in the namespace and used to answer with
    // nodeId and nodeType alone - a payload about the graph that says nothing about the shader.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("nodeType"), TEXT("Constant"));
    Payload->SetNumberField(TEXT("x"), -300.0);
    Payload->SetNumberField(TEXT("y"), 0.0);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("material.graph.add_node"), Payload, Capture);
    TestTrue(TEXT("handler registered"), bFound);
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("response reports success"), Capture.bSuccess) ||
        !TestTrue(TEXT("result is valid"), Capture.Result.IsValid()))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    const TSharedPtr<FJsonObject>* Block = nullptr;
    if (!TestTrue(TEXT("a material write verb publishes shaderCompile"),
            Capture.Result->TryGetObjectField(TEXT("shaderCompile"), Block)) || !Block)
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // The status is whatever this host's engine state produces - the point of this test is that the
    // caller is TOLD, not which value it is told. What must hold on every host is that the block is
    // well formed and that a graph write is never reported as a clean shader compile it did not do:
    // the write path uses the non-blocking probe, which never submits a compile.
    FString Status;
    if (TestTrue(TEXT("shaderCompile carries status"),
            (*Block)->TryGetStringField(TEXT("status"), Status)))
    {
        TestTrue(TEXT("status is one of the documented spellings"),
            Status == TEXT("notCompiled") || Status == TEXT("outstanding") ||
            Status == TEXT("timedOut") || Status == TEXT("failed") ||
            Status == TEXT("completed"));
    }

    TestTrue(TEXT("shaderCompile carries succeeded"), (*Block)->HasField(TEXT("succeeded")));
    TestTrue(TEXT("shaderCompile carries failed"), (*Block)->HasField(TEXT("failed")));
    TestTrue(TEXT("shaderCompile carries errors"), (*Block)->HasField(TEXT("errors")));
    TestTrue(TEXT("shaderCompile carries rendersDefaultMaterial"),
        (*Block)->HasField(TEXT("rendersDefaultMaterial")));

    bool bWaited = true;
    (*Block)->TryGetBoolField(TEXT("waited"), bWaited);
    TestFalse(TEXT("a graph write does not block on shader compilation"), bWaited);

    CleanupTestAsset(AssetPath);
    return true;
}
