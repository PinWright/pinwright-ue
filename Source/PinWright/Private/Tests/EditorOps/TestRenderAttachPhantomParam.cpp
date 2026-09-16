// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red / regression tests for board ticket B-attach-render-target-phantom-param.
//
// render.attach_render_target_to_volume must NOT report attached:true when
// `parameterName` does not name a real texture parameter on the base material.
// Pre-fix the handler (RenderHandler.cpp:444-456) creates a MID, calls
// SetTextureParameterValue(FName(parameterName), RT) blindly, adds the MID as a
// WeightedBlendable, and returns attached:true unconditionally — so a phantom
// parameter name yields an inert binding the shader never samples yet the caller
// is told attached:true (silent false-success). The correct behavior is to reject
// the phantom parameter with error PARAMETER_NOT_FOUND rather than claim it was
// attached.
//
// Two complementary cases:
//   1. PhantomTextureParameterRejected — the differential (RED pre-fix, GREEN
//      post-fix): a parameterName absent from the material must produce an error,
//      not attached:true. It also pins the error code + rejection payload so a
//      degenerate "success with attached:false that still binds the phantom MID"
//      regression cannot pass.
//   2. RealTextureParameterAccepted — the over-rejection guard: a parameterName
//      that DOES name a real texture parameter must still bind (attached:true).
//      Without it, inverting the validation gate / breaking the name comparison
//      would leave case 1 green while silently rejecting every legitimate
//      parameter. This case is green pre-fix and post-fix; its job is to fail if a
//      future change over-rejects.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
// FMaterialParameterInfo et al. moved from the top-level MaterialTypes.h into
// Materials/MaterialParameters.h in UE 5.7; on 5.6 and earlier they live in
// MaterialTypes.h, which has no MaterialParameters.h (and is deprecated on 5.8).
#if __has_include("Materials/MaterialParameters.h")
#include "Materials/MaterialParameters.h"
#else
#include "MaterialTypes.h"
#endif
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Math/Transform.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

namespace
{
    // Creates a real render target through the production handler so its path resolves via
    // LoadObject exactly as the handler under test resolves it. Fills OutTargetPath (the
    // resolvable object path) and OutPackagePath (the /Game package to clean up, set up
    // front so the caller's cleanup fires even on partial failure). Returns true on success.
    // (Uniquely named to avoid Unity/ODR collisions with other test TUs' fixture helpers.)
    bool RenderAttachParam_CreateRenderTargetFixture(FAutomationTestBase& Test,
        FString& OutTargetPath, FString& OutPackagePath)
    {
        const FString RtName = FString::Printf(TEXT("RT_AttachParamTest_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        OutPackagePath = FString::Printf(TEXT("/Game/RenderTargets/%s"), *RtName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), RtName);
        CreatePayload->SetNumberField(TEXT("width"), 64);
        CreatePayload->SetNumberField(TEXT("height"), 64);
        CreatePayload->SetStringField(TEXT("packagePath"), TEXT("/Game/RenderTargets"));

        FTestResponseCapture CreateCapture;
        Test.TestTrue(TEXT("render.create_render_target handler found"),
            InvokeHandlerWithCapture(TEXT("render.create_render_target"), CreatePayload, CreateCapture));
        Test.TestTrue(TEXT("render target fixture created"), CreateCapture.bSuccess);
        if (CreateCapture.bSuccess && CreateCapture.Result.IsValid())
        {
            CreateCapture.Result->TryGetStringField(TEXT("assetPath"), OutTargetPath);
        }
        return !OutTargetPath.IsEmpty();
    }

    // Builds a real /Game UMaterial exposing exactly one texture parameter named ParamName,
    // mirroring the proven CreatePackage + NewObject + PostEditChange fixture pattern from
    // TestMaterialInfoParameterDefaults. PostEditChange rebuilds the material's cached
    // expression data (ground truth), so the handler's validation source —
    // GetAllParameterInfoOfType(Texture) — returns this parameter by name, and the handler's
    // LoadObject<UMaterialInterface> resolves the in-memory package by path. Returns nullptr
    // on failure (the TestNotNull calls record the failure). Fills OutMaterialObjectPath (the
    // path to pass as materialPath) and OutPackagePath (the package to clean up).
    UMaterialInterface* RenderAttachParam_CreateMaterialWithTextureParam(FAutomationTestBase& Test,
        const FString& ParamName, FString& OutMaterialObjectPath, FString& OutPackagePath)
    {
        OutPackagePath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/AttachParamMat_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        UPackage* Pkg = CreatePackage(*OutPackagePath);
        if (!Test.TestNotNull(TEXT("material fixture package created"), Pkg))
        {
            return nullptr;
        }

        UMaterial* Material = NewObject<UMaterial>(
            Pkg,
            FName(*FPackageName::GetLongPackageAssetName(OutPackagePath)),
            RF_Public | RF_Standalone);
        if (!Test.TestNotNull(TEXT("material fixture created"), Material))
        {
            CleanupTestAsset(OutPackagePath);
            return nullptr;
        }

        UMaterialExpressionTextureSampleParameter2D* TexParam =
            NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
        TexParam->ParameterName = FName(*ParamName);
        TexParam->MaterialExpressionGuid = FGuid::NewGuid();
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(TexParam);

        Material->PostEditChange();
        Material->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Material);

        OutMaterialObjectPath = ToObjectPath(OutPackagePath);
        return Material;
    }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderAttachRenderTargetPhantomParamRejectedTest,
    "PinWright.render.attach_render_target_to_volume.PhantomTextureParameterRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderAttachRenderTargetPhantomParamRejectedTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        // A required fixture (the editor world we spawn the volume into) is a test
        // FAILURE, never a silent skip.
        AddError(TEXT("No editor world available to spawn the post process volume fixture."));
        return false;
    }

    // Fixture 1: a real base material that has NO texture parameter named our phantom
    // name. Engine materials are always present and a MID can be created from any of
    // them; this reaches the phantom-parameter code path without depending on host
    // content having a specific parameter layout.
    UMaterialInterface* BaseMat = nullptr;
    FString MaterialPath;
    const TCHAR* MaterialCandidates[] = {
        TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"),
        TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"),
    };
    for (const TCHAR* Candidate : MaterialCandidates)
    {
        if (UMaterialInterface* Loaded = LoadObject<UMaterialInterface>(nullptr, Candidate))
        {
            BaseMat = Loaded;
            MaterialPath = Candidate;
            break;
        }
    }
    if (!BaseMat)
    {
        AddError(TEXT("No engine base-material fixture could be loaded."));
        return false;
    }

    // Fixture 2: a real render target asset, created through the production handler so
    // its path resolves via LoadObject exactly as the handler under test resolves it.
    FString TargetPath;
    FString RtPackagePath;
    const bool bRtCreated = RenderAttachParam_CreateRenderTargetFixture(*this, TargetPath, RtPackagePath);
    // The RT lives in a real /Game package once created — clean it up unconditionally.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(RtPackagePath);
    };
    if (!bRtCreated)
    {
        AddError(TEXT("render.create_render_target did not yield a resolvable render target asset path."));
        return false;
    }

    // Fixture 3: a fresh post process volume spawned into the editor world. Its full
    // object path round-trips through the handler's FindObject<AActor>(nullptr, path).
    APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>(
        APostProcessVolume::StaticClass(), FTransform::Identity);
    if (!Volume)
    {
        AddError(TEXT("Failed to spawn the APostProcessVolume fixture into the editor world."));
        return false;
    }
    ON_SCOPE_EXIT
    {
        if (Volume)
        {
            Volume->Destroy();
        }
    };
    const FString VolumePath = Volume->GetPathName();

    // The defect trigger: a parameterName that names no texture parameter on BaseMat.
    const FString PhantomParam = TEXT("PW_PhantomParam_DoesNotExist_9Z7K");

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumePath"), VolumePath);
    Payload->SetStringField(TEXT("targetPath"), TargetPath);
    Payload->SetStringField(TEXT("materialPath"), MaterialPath);
    Payload->SetStringField(TEXT("parameterName"), PhantomParam);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.attach_render_target_to_volume handler found"),
        InvokeHandlerWithCapture(TEXT("render.attach_render_target_to_volume"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    // Correct behavior: a parameterName that names no texture parameter on the base
    // material must be rejected — the handler must not report attached:true for a
    // phantom binding the shader never samples. Pre-fix the handler returns
    // {attached:true} here, so bClaimsAttached is true and this assertion fails
    // (the reproduction). A fix that returns an error (PARAMETER_NOT_FOUND) or reports
    // attached:false flips it green.
    bool bAttached = false;
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        Capture.Result->TryGetBoolField(TEXT("attached"), bAttached);
    }
    const bool bClaimsAttached = Capture.bSuccess && bAttached;
    TestFalse(
        TEXT("nonexistent texture parameter must not be reported as attached:true"),
        bClaimsAttached);

    // Pin the repair, not just the attached boolean: the handler must actively REJECT
    // (an error response), not merely flip attached:false while still creating the MID
    // and adding the inert phantom blendable. Assert the error path + its registered code
    // and the caller-guiding payload (validTextureParameters + echoed parameterName).
    TestFalse(TEXT("handler rejected with an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("rejection uses the PARAMETER_NOT_FOUND error code"),
        Capture.ErrorCode, FString(TEXT("PARAMETER_NOT_FOUND")));
    if (!Capture.bSuccess && Capture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* ValidParams = nullptr;
        TestTrue(TEXT("rejection payload lists validTextureParameters"),
            Capture.Result->TryGetArrayField(TEXT("validTextureParameters"), ValidParams));
        FString EchoedParam;
        Capture.Result->TryGetStringField(TEXT("parameterName"), EchoedParam);
        TestEqual(TEXT("rejection echoes the offending parameterName"), EchoedParam, PhantomParam);
    }

    return true;
}

// Over-rejection guard: a parameterName that DOES name a real texture parameter on the
// base material must still bind (attached:true). The phantom-rejection differential above
// only proves a bogus name is refused; it stays green even if a regression inverts the
// validation gate (RenderHandler.cpp:471), breaks the name comparison (:465), or otherwise
// rejects everything. This case fails in exactly those regressions: it builds a material
// the engine reports as exposing the texture parameter, then asserts the handler accepts it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderAttachRenderTargetRealParamAcceptedTest,
    "PinWright.render.attach_render_target_to_volume.RealTextureParameterAccepted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderAttachRenderTargetRealParamAcceptedTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        AddError(TEXT("No editor world available to spawn the post process volume fixture."));
        return false;
    }

    // Fixture 1: a real /Game base material exposing exactly one texture parameter.
    const FString KnownParam = TEXT("PW_AttachTexParam");
    FString MaterialPath;
    FString MatPackagePath;
    UMaterialInterface* BaseMat = RenderAttachParam_CreateMaterialWithTextureParam(
        *this, KnownParam, MaterialPath, MatPackagePath);
    if (!BaseMat)
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(MatPackagePath);
    };

    // Precondition: the engine must actually report the texture parameter on this material
    // through the same API the handler validates against. If it does not (unexpected in a
    // compiled editor), skip the acceptance assertion rather than emit a false regression —
    // the over-rejection guard is only meaningful once the fixture genuinely exposes the
    // parameter. The presence check is by our own case-insensitive name match, independent of
    // the handler's comparison, so a broken handler comparison cannot mask a valid fixture.
    TArray<FMaterialParameterInfo> TextureParamInfos;
    TArray<FGuid> TextureParamGuids;
    BaseMat->GetAllParameterInfoOfType(EMaterialParameterType::Texture, TextureParamInfos, TextureParamGuids);
    bool bFixtureExposesParam = false;
    for (const FMaterialParameterInfo& Info : TextureParamInfos)
    {
        if (Info.Name.ToString().Equals(KnownParam, ESearchCase::IgnoreCase))
        {
            bFixtureExposesParam = true;
            break;
        }
    }
    if (!bFixtureExposesParam)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-did-not-populate"),
            TEXT("Material fixture did not expose the texture parameter via "
                "GetAllParameterInfoOfType; skipping the accept-case assertion."));
        return true;
    }

    // Fixture 2: a real render target (same production-handler path as the reject case).
    FString TargetPath;
    FString RtPackagePath;
    const bool bRtCreated = RenderAttachParam_CreateRenderTargetFixture(*this, TargetPath, RtPackagePath);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(RtPackagePath);
    };
    if (!bRtCreated)
    {
        AddError(TEXT("render.create_render_target did not yield a resolvable render target asset path."));
        return false;
    }

    // Fixture 3: a fresh post process volume. Destroyed first (declared last) so its MID —
    // which references BaseMat and the render target — is dropped before those assets are
    // deleted.
    APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>(
        APostProcessVolume::StaticClass(), FTransform::Identity);
    if (!Volume)
    {
        AddError(TEXT("Failed to spawn the APostProcessVolume fixture into the editor world."));
        return false;
    }
    ON_SCOPE_EXIT
    {
        if (Volume)
        {
            Volume->Destroy();
        }
    };
    const FString VolumePath = Volume->GetPathName();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("volumePath"), VolumePath);
    Payload->SetStringField(TEXT("targetPath"), TargetPath);
    Payload->SetStringField(TEXT("materialPath"), MaterialPath);
    Payload->SetStringField(TEXT("parameterName"), KnownParam);

    FTestResponseCapture Capture;
    TestTrue(TEXT("render.attach_render_target_to_volume handler found"),
        InvokeHandlerWithCapture(TEXT("render.attach_render_target_to_volume"), Payload, Capture));
    TestTrue(TEXT("handler sent a response"), Capture.bWasCalled);

    // A parameterName that names a real texture parameter must bind, not be over-rejected.
    TestTrue(TEXT("valid texture parameter is accepted (success)"), Capture.bSuccess);
    bool bAttached = false;
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        Capture.Result->TryGetBoolField(TEXT("attached"), bAttached);
    }
    TestTrue(TEXT("valid texture parameter is reported as attached:true"), bAttached);

    return true;
}
