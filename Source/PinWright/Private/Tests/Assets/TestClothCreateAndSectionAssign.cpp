// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for F-cloth-create-and-section-assign.
//
// Before this fix the skeleton cloth verbs could only bind/list an
// already-existing UClothingAsset, so Chaos Cloth setup was impossible
// end-to-end through the MCP:
//  - there was NO verb that creates a UClothingAsset, and
//  - skeleton.assign_cloth_asset_to_mesh declared ONLY `skeletalMeshPath`, so
//    passing `clothAssetName`/`sectionIndex` was rejected UNKNOWN_PARAMS at the
//    wire level (ValidateHandlerParams) before the body ran — it could attach
//    nothing to any section.
//
// The fix adds skeleton.create_cloth_from_section (the editor's
// UClothingAssetFactoryBase::CreateFromSkeletalMesh / "Create Clothing Data from
// Section" path) and gives assign_cloth_asset_to_mesh real
// clothAssetName + sectionIndex (+ meshLodIndex/assetLodIndex) params that bind
// an existing named asset to a chosen section.
//
// Coverage (exercises the production FParamSpec set + the real dispatcher):
//  - Static: create_cloth_from_section is registered and requires
//    skeletalMeshPath + clothAssetName; assign_cloth_asset_to_mesh declares the
//    clothAssetName + sectionIndex params it previously lacked.
//  - End-to-end dispatch: route {skeletalMeshPath, clothAssetName, sectionIndex}
//    through the real dispatcher to assign_cloth_asset_to_mesh and assert it is
//    NOT rejected UNKNOWN_PARAMS — the params passed validation and the body ran
//    (a bogus mesh path then yields MESH_NOT_FOUND, proving the body was reached).
// Counterfactual: reverting the fix drops the new verb (static check fails),
// drops the assign params (static check fails), and makes the dispatch reject
// {clothAssetName, sectionIndex} with UNKNOWN_PARAMS (dispatch check fails).
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

namespace
{
    const FHandlerRegistration* FindRegistration(const FString& Method)
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName.Equals(Method))
            {
                return &Reg;
            }
        }
        return nullptr;
    }

    const FParamSpec* FindParamSpec(const FHandlerRegistration& Reg, const FString& ParamName)
    {
        for (const FParamSpec& Spec : Reg.Params)
        {
            if (Spec.Name.Equals(ParamName))
            {
                return &Spec;
            }
        }
        return nullptr;
    }
}

// 1. Static: the create-cloth verb exists and declares the params that author a
//    new asset from a section. This is the verb that was entirely absent before.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClothCreateFromSectionVerbRegisteredTest,
    "PinWright.skeleton.cloth.CreateFromSectionVerbRegistered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FClothCreateFromSectionVerbRegisteredTest::RunTest(const FString& Parameters)
{
    const FHandlerRegistration* Reg = FindRegistration(TEXT("skeleton.create_cloth_from_section"));
    if (!TestNotNull(TEXT("skeleton.create_cloth_from_section is registered"), Reg))
    {
        return false;
    }

    const FParamSpec* MeshPath = FindParamSpec(*Reg, TEXT("skeletalMeshPath"));
    const FParamSpec* AssetName = FindParamSpec(*Reg, TEXT("clothAssetName"));
    const FParamSpec* SectionIndex = FindParamSpec(*Reg, TEXT("sectionIndex"));

    TestNotNull(TEXT("create_cloth_from_section declares skeletalMeshPath"), MeshPath);
    TestNotNull(TEXT("create_cloth_from_section declares clothAssetName"), AssetName);
    TestNotNull(TEXT("create_cloth_from_section declares sectionIndex"), SectionIndex);
    if (MeshPath)
    {
        TestTrue(TEXT("skeletalMeshPath is required"), MeshPath->bRequired);
    }
    if (AssetName)
    {
        TestTrue(TEXT("clothAssetName is required (the new asset's name)"), AssetName->bRequired);
    }
    return true;
}

// 2. Static: assign_cloth_asset_to_mesh now declares the clothAssetName +
//    sectionIndex params it previously rejected, so it can attach an existing
//    asset to a specific section rather than only list.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClothAssignDeclaresSectionParamsTest,
    "PinWright.skeleton.cloth.AssignDeclaresSectionParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FClothAssignDeclaresSectionParamsTest::RunTest(const FString& Parameters)
{
    const FHandlerRegistration* Reg = FindRegistration(TEXT("skeleton.assign_cloth_asset_to_mesh"));
    if (!TestNotNull(TEXT("skeleton.assign_cloth_asset_to_mesh is registered"), Reg))
    {
        return false;
    }

    TestNotNull(TEXT("assign_cloth_asset_to_mesh declares clothAssetName"),
        FindParamSpec(*Reg, TEXT("clothAssetName")));
    TestNotNull(TEXT("assign_cloth_asset_to_mesh declares sectionIndex"),
        FindParamSpec(*Reg, TEXT("sectionIndex")));
    return true;
}

// 3. End-to-end: routing {clothAssetName, sectionIndex} through the real
//    dispatcher to assign_cloth_asset_to_mesh is NOT rejected UNKNOWN_PARAMS.
//    The params pass ValidateHandlerParams and the body runs; with a bogus mesh
//    path the body returns MESH_NOT_FOUND, which proves the body was reached
//    rather than the request being rejected at the param-validation wall.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClothAssignAcceptsSectionParamsOnWireTest,
    "PinWright.skeleton.cloth.AssignAcceptsSectionParamsOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FClothAssignAcceptsSectionParamsOnWireTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("skeletalMeshPath"),
        TEXT("/Game/PW_NonexistentClothProbeMesh.PW_NonexistentClothProbeMesh"));
    Params->SetStringField(TEXT("clothAssetName"), TEXT("PW_ClothProbeAsset"));
    Params->SetNumberField(TEXT("sectionIndex"), 1);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("skeleton.assign_cloth_asset_to_mesh"),
        TEXT("req-cloth-assign-section-params"), Params, bSuccess, Result, ErrorCode);

    // The repro before the fix: clothAssetName/sectionIndex were UNKNOWN_PARAMS.
    TestNotEqual(TEXT("assign_cloth_asset_to_mesh does not reject clothAssetName/sectionIndex as UNKNOWN_PARAMS"),
        ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    // The body ran and failed to load the (intentionally bogus) mesh, proving the
    // params reached the handler rather than tripping the validation wall.
    TestEqual(TEXT("assign_cloth_asset_to_mesh reaches the body and reports MESH_NOT_FOUND"),
        ErrorCode, FString(TEXT("MESH_NOT_FOUND")));
    TestFalse(TEXT("assign_cloth_asset_to_mesh fails on the bogus mesh path"), bSuccess);
    return true;
}
