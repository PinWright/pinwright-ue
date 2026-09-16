// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
// UE 5.8 moved the sampler-type derivation to the MaterialExpressionUtils namespace and
// deprecated the UMaterialExpressionTextureBase static (C4996); the header only exists on 5.8+.
#if __has_include("Materials/MaterialExpressionUtils.h")
#include "Materials/MaterialExpressionUtils.h"
#endif
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

// Regression coverage for material.authoring.set_texture_sample_texture
// (E-material-set-texture-on-existing-sample): there was no typed verb to assign or
// change the texture on an EXISTING TextureSample node — callers had to property.set the
// raw `Texture` UPROPERTY and then hand-pick a samplerType that matched the texture's
// derived sampler class. The new verb assigns the texture and, when samplerType is
// omitted, auto-derives the sampler so the pair is consistent by construction.
//
// These tests fail if the verb is reverted: InvokeHandlerWithCapture would not find the
// handler (bFound == false) and the node's Texture would never be set.

namespace
{
    // Builds a transient-package material carrying a single, deliberately UNASSIGNED
    // TextureSample node (mirroring the "leave texturePath unset, point it later" intent
    // that motivated the ticket). Returns the material and the sample node, or nullptr
    // after emitting the failing assertion + cleaning up.
    UMaterial* CreateUnassignedTextureSampleMaterial(
        FAutomationTestBase& Test,
        const TCHAR* AssetPathPrefix,
        FString& OutAssetPath,
        UMaterialExpressionTextureSample*& OutSample)
    {
        OutSample = nullptr;
        OutAssetPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/%s_%s"),
            AssetPathPrefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UPackage* Pkg = CreatePackage(*OutAssetPath);
        if (!Test.TestNotNull(TEXT("Package created"), Pkg))
            return nullptr;

        UMaterial* Material = NewObject<UMaterial>(
            Pkg,
            FName(*FPackageName::GetLongPackageAssetName(OutAssetPath)),
            RF_Public | RF_Standalone);
        if (!Test.TestNotNull(TEXT("Material created"), Material))
        {
            CleanupTestAsset(OutAssetPath);
            return nullptr;
        }

        UMaterialExpressionTextureSample* Sample = NewObject<UMaterialExpressionTextureSample>(Material);
        if (!Test.TestNotNull(TEXT("TextureSample created"), Sample))
        {
            CleanupTestAsset(OutAssetPath);
            return nullptr;
        }
        if (!Sample->MaterialExpressionGuid.IsValid())
        {
            Sample->MaterialExpressionGuid = FGuid::NewGuid();
        }
        // The node starts with no texture — exactly the state with no typed write path before the fix.
        Sample->Texture = nullptr;

        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Sample);
        Material->PostEditChange();
        Material->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Material);

        OutSample = Sample;
        return Material;
    }

    // Builds a transient-package material carrying TWO TextureSampleParameter2D nodes that share
    // one parameter name — the normal shape of a triplanar / multi-band material, where several
    // sample nodes read one atlas on different projection planes and share a single value at the
    // material-instance level. Both nodes start unassigned so a partial write is visible.
    UMaterial* CreateSharedParameterNameMaterial(
        FAutomationTestBase& Test,
        const TCHAR* SharedParameterName,
        FString& OutAssetPath,
        UMaterialExpressionTextureSampleParameter2D*& OutFirst,
        UMaterialExpressionTextureSampleParameter2D*& OutSecond)
    {
        OutFirst = nullptr;
        OutSecond = nullptr;
        OutAssetPath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/SetTexSampleAmbiguous_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UPackage* Pkg = CreatePackage(*OutAssetPath);
        if (!Test.TestNotNull(TEXT("Package created"), Pkg))
            return nullptr;

        UMaterial* Material = NewObject<UMaterial>(
            Pkg,
            FName(*FPackageName::GetLongPackageAssetName(OutAssetPath)),
            RF_Public | RF_Standalone);
        if (!Test.TestNotNull(TEXT("Material created"), Material))
        {
            CleanupTestAsset(OutAssetPath);
            return nullptr;
        }

        UMaterialExpressionTextureSampleParameter2D* Created[2] = { nullptr, nullptr };
        for (int32 Index = 0; Index < 2; ++Index)
        {
            UMaterialExpressionTextureSampleParameter2D* Node =
                NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
            if (!Test.TestNotNull(TEXT("TextureSampleParameter2D created"), Node))
            {
                CleanupTestAsset(OutAssetPath);
                return nullptr;
            }
            Node->MaterialExpressionGuid = FGuid::NewGuid();
            Node->ParameterName = FName(SharedParameterName);
            Node->Texture = nullptr;
            Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Node);
            Created[Index] = Node;
        }

        Material->PostEditChange();
        Material->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Material);

        OutFirst = Created[0];
        OutSecond = Created[1];
        return Material;
    }
}


// Explicit samplerType path: the verb assigns the texture on the existing node and stamps
// the requested sampler.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialSetTextureSampleTextureExplicitTest,
    "PinWright.material.authoring.set_texture_sample_texture.ExplicitSampler",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialSetTextureSampleTextureExplicitTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterialExpressionTextureSample* Sample = nullptr;
    if (!CreateUnassignedTextureSampleMaterial(*this, TEXT("SetTexSampleExplicit"), AssetPath, Sample))
        return true;

    TestNull(TEXT("Sample starts with no texture"), Sample->Texture.Get());

    UTexture* Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
    if (!TestNotNull(TEXT("Engine DefaultTexture loaded"), Texture))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("nodeId"), Sample->MaterialExpressionGuid.ToString());
    Payload->SetStringField(TEXT("texturePath"), TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
    Payload->SetStringField(TEXT("samplerType"), TEXT("LinearColor"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("material.authoring.set_texture_sample_texture"), Payload, Capture);
    TestTrue(TEXT("Handler found for material.authoring.set_texture_sample_texture"), bFound);
    TestTrue(TEXT("set_texture_sample_texture succeeded"), Capture.bSuccess);

    // The core assertion: the texture is now assigned on the EXISTING node (no creation, no property.set).
    TestTrue(TEXT("Existing node's Texture is now the assigned texture"), Sample->Texture.Get() == Texture);
    // The explicit sampler is honored verbatim.
    TestEqual(TEXT("SamplerType stamped to the requested LinearColor"),
        static_cast<int32>(Sample->SamplerType.GetValue()), static_cast<int32>(SAMPLERTYPE_LinearColor));

    CleanupTestAsset(AssetPath);
    return true;
}


// Auto-derive path: omitting samplerType derives the sampler from the texture so the
// sampler/texture pair is consistent by construction (no LinearColor-vs-Color compile trap).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialSetTextureSampleTextureAutoDeriveTest,
    "PinWright.material.authoring.set_texture_sample_texture.AutoDeriveSampler",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialSetTextureSampleTextureAutoDeriveTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterialExpressionTextureSample* Sample = nullptr;
    if (!CreateUnassignedTextureSampleMaterial(*this, TEXT("SetTexSampleAuto"), AssetPath, Sample))
        return true;

    const TCHAR* TexturePath = TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");
    UTexture* Texture = LoadObject<UTexture>(nullptr, TexturePath);
    if (!TestNotNull(TEXT("Engine DefaultTexture loaded"), Texture))
    {
        CleanupTestAsset(AssetPath);
        return true;
    }

    // The sampler the engine itself derives for this texture — what the compiler's
    // VerifySamplerType check enforces. The handler must match this, not whatever the
    // node defaulted to. Pre-seed a deliberately-wrong sampler so a no-op would be caught.
#if __has_include("Materials/MaterialExpressionUtils.h")
    const EMaterialSamplerType Expected =
        MaterialExpressionUtils::GetSamplerTypeForTexture(Texture);
#else
    const EMaterialSamplerType Expected =
        UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Texture);
#endif
    Sample->SamplerType = (Expected == SAMPLERTYPE_LinearColor) ? SAMPLERTYPE_Color : SAMPLERTYPE_LinearColor;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("nodeId"), Sample->MaterialExpressionGuid.ToString());
    Payload->SetStringField(TEXT("texturePath"), TexturePath);
    // No samplerType -> auto-derive.

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("material.authoring.set_texture_sample_texture"), Payload, Capture);
    TestTrue(TEXT("Handler found for material.authoring.set_texture_sample_texture"), bFound);
    TestTrue(TEXT("set_texture_sample_texture (auto-derive) succeeded"), Capture.bSuccess);

    TestTrue(TEXT("Existing node's Texture is now the assigned texture"), Sample->Texture.Get() == Texture);
    // Auto-derived sampler matches the engine's GetSamplerTypeForTexture for this texture.
    TestEqual(TEXT("SamplerType auto-derived to match the texture's engine-derived sampler class"),
        static_cast<int32>(Sample->SamplerType.GetValue()), static_cast<int32>(Expected));

    // The response payload echoes the derived sampler so callers can confirm without a re-read.
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ReportedSampler;
        TestTrue(TEXT("Response carries samplerType"),
            Capture.Result->TryGetStringField(TEXT("samplerType"), ReportedSampler));
    }

    CleanupTestAsset(AssetPath);
    return true;
}


// A non-existent node id is rejected with NOT_FOUND, not silently no-op'd.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialSetTextureSampleTextureNotFoundTest,
    "PinWright.material.authoring.set_texture_sample_texture.NodeNotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialSetTextureSampleTextureNotFoundTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    UMaterialExpressionTextureSample* Sample = nullptr;
    if (!CreateUnassignedTextureSampleMaterial(*this, TEXT("SetTexSampleNotFound"), AssetPath, Sample))
        return true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("nodeId"), FGuid::NewGuid().ToString()); // no such node
    Payload->SetStringField(TEXT("texturePath"), TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("material.authoring.set_texture_sample_texture"), Payload, Capture);
    TestTrue(TEXT("Handler found for material.authoring.set_texture_sample_texture"), bFound);
    TestFalse(TEXT("Missing node is rejected"), Capture.bSuccess);
    TestEqual(TEXT("NOT_FOUND error for a missing node"), Capture.ErrorCode, FString(TEXT("NOT_FOUND")));

    CleanupTestAsset(AssetPath);
    return true;
}


// A nodeId that names a parameter shared by several nodes is refused, not applied to whichever
// node iterated first. The silent half-write it replaces was undetectable from the response: the
// graph stays legal with two textures on two same-named parameter nodes, so the payload, a clean
// compile and the matching read verb all agreed with the wrong answer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialSetTextureSampleTextureAmbiguousParameterNameTest,
    "PinWright.material.authoring.set_texture_sample_texture.AmbiguousParameterName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialSetTextureSampleTextureAmbiguousParameterNameTest::RunTest(const FString& Parameters)
{
    const TCHAR* SharedParameterName = TEXT("PwSharedAtlasParam");

    FString AssetPath;
    UMaterialExpressionTextureSampleParameter2D* First = nullptr;
    UMaterialExpressionTextureSampleParameter2D* Second = nullptr;
    if (!CreateSharedParameterNameMaterial(*this, SharedParameterName, AssetPath, First, Second))
        return true;

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AssetPath);
    Payload->SetStringField(TEXT("nodeId"), SharedParameterName); // names BOTH nodes
    Payload->SetStringField(TEXT("texturePath"), TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("material.authoring.set_texture_sample_texture"), Payload, Capture);
    TestTrue(TEXT("Handler found for material.authoring.set_texture_sample_texture"), bFound);
    TestFalse(TEXT("A parameter name shared by two nodes is refused, not applied to one of them"),
        Capture.bSuccess);
    TestEqual(TEXT("The refusal uses AMBIGUOUS_NODE"),
        Capture.ErrorCode, FString(TEXT("AMBIGUOUS_NODE")));

    // The refusal must be total: neither node may have been repointed.
    TestNull(TEXT("The first node keeps its texture"), First->Texture.Get());
    TestNull(TEXT("The sibling node keeps its texture"), Second->Texture.Get());

    // Without the candidate list the caller cannot act on the refusal — the workaround is to
    // re-issue once per node against a unique key, which has to come from somewhere.
    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("AMBIGUOUS_NODE carried no structured payload."));
        CleanupTestAsset(AssetPath);
        return false;
    }

    double CandidateCount = 0.0;
    if (Capture.Result->TryGetNumberField(TEXT("candidateCount"), CandidateCount))
    {
        TestEqual(TEXT("The response says how many nodes the nodeId matched"),
            static_cast<int32>(CandidateCount), 2);
    }
    else
    {
        AddError(TEXT("AMBIGUOUS_NODE payload has no candidateCount."));
    }

    const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("candidates"), Candidates) || !Candidates)
    {
        AddError(TEXT("AMBIGUOUS_NODE payload has no candidates array."));
        CleanupTestAsset(AssetPath);
        return false;
    }
    TestEqual(TEXT("Both colliding nodes are offered as candidates"), Candidates->Num(), 2);

    TSet<FString> CandidateNodeIds;
    for (const TSharedPtr<FJsonValue>& Entry : *Candidates)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (Entry.IsValid() && Entry->TryGetObject(Obj) && Obj)
        {
            FString CandidateNodeId;
            if ((*Obj)->TryGetStringField(TEXT("nodeId"), CandidateNodeId))
            {
                CandidateNodeIds.Add(CandidateNodeId);
            }
        }
    }
    TestTrue(TEXT("The first node's GUID is a candidate"),
        CandidateNodeIds.Contains(First->MaterialExpressionGuid.ToString()));
    TestTrue(TEXT("The sibling node's GUID is a candidate"),
        CandidateNodeIds.Contains(Second->MaterialExpressionGuid.ToString()));

    CleanupTestAsset(AssetPath);
    return true;
}
