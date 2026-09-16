// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for E-material-create-combined-assetpath-split.
//
// Every *operate* verb in material.authoring takes a single combined `assetPath`
// (MaterialHandlerUtils: assetPath / materialPath / path), but the create_* family split the
// destination into two slots — `name` (leaf, required) + `path` (folder, optional) — with no
// way to pass the one fully-qualified asset path the rest of the namespace accepts. A caller
// priming on "every material verb takes one assetPath" guessed `assetPath` on the create verb
// and ate a hard MISSING_REQUIRED_PARAM 'name' round-trip.
//
// The fix (MaterialCreatePathParamUtils) makes the create_* verbs additionally accept a
// combined `assetPath` (and the `assetName` casing synonym) and split it server-side into leaf
// name + parent folder, WITHOUT changing the meaning of the existing folder `path` slot.
//
// Two layers of coverage, mirroring TestGeometryCreateNameParamAlias / TestMaterialDispatcherAliases:
//  - Static registration check: every material.authoring.create_* verb's `name` spec carries
//    the `assetPath` (and `assetName`) alias, so ValidateHandlerParams accepts a
//    combined-`assetPath`-only call instead of rejecting it as MISSING_REQUIRED_PARAM.
//  - End-to-end dispatch: route `{assetPath:"/Game/.../M_Foo"}` (no `name`) through the real
//    dispatcher and assert the material is created at the split name + folder.
// Counterfactual: reverting the fix makes the static alias check fail and the dispatch fail
// with MISSING_REQUIRED_PARAM 'name' before the body runs.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Material/MaterialCreatePathParamUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

namespace
{
    // The whole material.authoring.create_* family whose `name` slot now also accepts a
    // combined `assetPath` (split server-side) and the `assetName` synonym.
    const TArray<FString>& MaterialCreateVerbs()
    {
        static const TArray<FString> Verbs = {
            TEXT("material.authoring.create_material"),
            TEXT("material.authoring.create_material_function"),
            TEXT("material.authoring.create_material_instance"),
            TEXT("material.authoring.create_landscape_material"),
            TEXT("material.authoring.create_decal_material"),
            TEXT("material.authoring.create_post_process_material"),
            TEXT("material.authoring.create_material_layer"),
            TEXT("material.authoring.create_material_layer_blend")
        };
        return Verbs;
    }

    const FParamSpec* FindParamSpec(const FString& Method, const FString& ParamName)
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (!Reg.MethodName.Equals(Method))
            {
                continue;
            }
            for (const FParamSpec& Spec : Reg.Params)
            {
                if (Spec.Name.Equals(ParamName))
                {
                    return &Spec;
                }
            }
        }
        return nullptr;
    }

    // Delete the asset created by the dispatch test so nothing persists on the fuzzing host.
    void DeleteAssetAtPath(const FString& PackagePath)
    {
        UPackage* Pkg = FindPackage(nullptr, *PackagePath);
        if (!Pkg)
        {
            return;
        }
        for (TObjectIterator<UObject> It; It; ++It)
        {
            if (It->GetPackage() == Pkg && It->IsAsset())
            {
                It->ClearFlags(RF_Public | RF_Standalone);
                It->MarkAsGarbage();
            }
        }
    }
}

// 1. Static: every material.authoring.create_* verb registers its `name` slot with the
//    combined-`assetPath` alias (plus the `assetName` casing synonym) so a caller passing one
//    full asset path is no longer rejected at the wire level.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCreateVerbsDeclareAssetPathAliasTest,
    "PinWright.material.aliases.CreateVerbsDeclareAssetPathAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialCreateVerbsDeclareAssetPathAliasTest::RunTest(const FString& Parameters)
{
    for (const FString& Verb : MaterialCreateVerbs())
    {
        const FParamSpec* Spec = FindParamSpec(Verb, TEXT("name"));
        if (!TestNotNull(*FString::Printf(TEXT("%s declares a 'name' param"), *Verb), Spec))
        {
            continue;
        }
        TestTrue(*FString::Printf(TEXT("%s 'name' carries the 'assetPath' alias"), *Verb),
            Spec->Aliases.Contains(TEXT("assetPath")));
        TestTrue(*FString::Printf(TEXT("%s 'name' carries the 'assetName' alias"), *Verb),
            Spec->Aliases.Contains(TEXT("assetName")));
    }
    return true;
}

// 2. Unit: the split helper derives leaf name + parent folder from a combined path
//    (both package-path and object-path forms), and reports a bare leaf as un-splittable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCreateSplitCombinedAssetPathTest,
    "PinWright.material.aliases.SplitCombinedAssetPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialCreateSplitCombinedAssetPathTest::RunTest(const FString& Parameters)
{
    FString Name, Folder;

    TestTrue(TEXT("package-path form splits"),
        MaterialCreatePathParamUtils::SplitCombinedAssetPath(
            TEXT("/Game/Pickups/Materials/M_StylizedLeaf"), Name, Folder));
    TestEqual(TEXT("leaf name from package path"), Name, FString(TEXT("M_StylizedLeaf")));
    TestEqual(TEXT("parent folder from package path"), Folder, FString(TEXT("/Game/Pickups/Materials")));

    TestTrue(TEXT("object-path form splits"),
        MaterialCreatePathParamUtils::SplitCombinedAssetPath(
            TEXT("/Game/Pickups/Materials/M_StylizedLeaf.M_StylizedLeaf"), Name, Folder));
    TestEqual(TEXT("leaf name from object path"), Name, FString(TEXT("M_StylizedLeaf")));
    TestEqual(TEXT("parent folder from object path"), Folder, FString(TEXT("/Game/Pickups/Materials")));

    // A bare leaf with no folder component is not a usable combined path.
    TestFalse(TEXT("bare leaf does not split"),
        MaterialCreatePathParamUtils::SplitCombinedAssetPath(TEXT("M_Loose"), Name, Folder));
    return true;
}

// 3. End-to-end: create_material accepts the combined-`assetPath` shape (no separate `name`)
//    the operate verbs already take, and the asset lands at the split name + folder.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCreateAcceptsCombinedAssetPathOnWireTest,
    "PinWright.material.aliases.CreateAcceptsCombinedAssetPathOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMaterialCreateAcceptsCombinedAssetPathOnWireTest::RunTest(const FString& Parameters)
{
    const FString Leaf = FString::Printf(TEXT("M_EACombinedPath_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Folder = TEXT("/Game/__PW_GatewayTests/CombinedPath");
    const FString CombinedPath = Folder / Leaf;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // The repro key: one fully-qualified asset path in `assetPath`, no separate `name`.
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("assetPath"), CombinedPath);
    Params->SetBoolField(TEXT("save"), false);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("material.authoring.create_material"),
        TEXT("req-mat-combined-assetpath"), Params, bSuccess, Result, ErrorCode);

    // Counterfactual: without the fix this fails at validation with MISSING_REQUIRED_PARAM 'name'.
    TestNotEqual(TEXT("create_material does not reject combined assetPath as MISSING_REQUIRED_PARAM"),
        ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    TestTrue(TEXT("create_material succeeds with a combined assetPath"), bSuccess);
    if (bSuccess && Result.IsValid())
    {
        TestEqual(TEXT("created asset name is the split leaf"),
            Result->GetStringField(TEXT("assetName")), Leaf);
        TestEqual(TEXT("created asset landed at the split package path"),
            Result->GetStringField(TEXT("assetPath")), CombinedPath);
    }

    DeleteAssetAtPath(CombinedPath);
    return true;
}
