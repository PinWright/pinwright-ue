// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for B-capture-verbs-silent-default-material-fallback.
//
// Counterfactual: remove either capture handler's ApplyCaptureFallbackPolicy call and its
// omitted-allowFallback invocation returns success instead of MATERIAL_FALLBACK; remove the
// shared serializer and the materialReadiness assertions fail on both verbs.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Compat/EngineVersionCompat.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "HAL/FileManager.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Material/MaterialShaderState.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tests/Material/MaterialShaderStateTestFixtures.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace
{
    bool PWMtlFallbackRequireBrokenMaterial(FAutomationTestBase& Test, const TCHAR* Stem,
        FString& OutAssetPath, UMaterial*& OutMaterial)
    {
        OutMaterial = PinWrightMaterialShaderStateTestFixtures::MakeBrokenHlslMaterial(
            Stem, OutAssetPath);
        if (!Test.TestNotNull(TEXT("broken material created"), OutMaterial))
        {
            CleanupTestAsset(OutAssetPath);
            return false;
        }

        const PinWright::MaterialShaderState::FState State =
            PinWright::MaterialShaderState::ProbeAndWait(OutMaterial);
        if (State.Status != PinWright::MaterialShaderState::EStatus::Failed ||
            !State.bRendersDefaultMaterial)
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("shader-compile-unavailable"),
                FString::Printf(TEXT("The deliberate compile-error fixture reported '%s' and "
                                     "usingDefaultMaterial=%s, so this host did not expose the "
                                     "fallback state the capture contract must exercise."),
                    PinWright::MaterialShaderState::ToWire(State.Status),
                    State.bRendersDefaultMaterial ? TEXT("true") : TEXT("false")));
            CleanupTestAsset(OutAssetPath);
            OutMaterial = nullptr;
            return false;
        }
        return true;
    }

    bool PWMtlFallbackMakeBrokenInstance(FAutomationTestBase& Test,
        FString& OutMasterPath, FString& OutInstancePath,
        UMaterial*& OutMaster, UMaterialInstanceConstant*& OutInstance)
    {
        OutMaster = PinWrightMaterialShaderStateTestFixtures::MakeBrokenHlslMaterial(
            TEXT("PWThumbFallbackMaster"), OutMasterPath);
        if (!Test.TestNotNull(TEXT("broken master created"), OutMaster))
        {
            return false;
        }

        OutInstancePath = FString::Printf(TEXT("/Game/__PW_GatewayTests/PWThumbFallbackMI_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        UPackage* InstancePackage = CreatePackage(*OutInstancePath);
        UMaterialInstanceConstantFactoryNew* Factory =
            NewObject<UMaterialInstanceConstantFactoryNew>();
        Factory->InitialParent = OutMaster;
        OutInstance = InstancePackage
            ? Cast<UMaterialInstanceConstant>(Factory->FactoryCreateNew(
                UMaterialInstanceConstant::StaticClass(), InstancePackage,
                FName(*FPackageName::GetLongPackageAssetName(OutInstancePath)),
                RF_Public | RF_Standalone, nullptr, GWarn))
            : nullptr;
        if (!Test.TestNotNull(TEXT("broken material instance created"), OutInstance))
        {
            return false;
        }

        OutInstance->SetParentEditorOnly(OutMaster);
        OutInstance->SetStaticSwitchParameterValueEditorOnly(
            FMaterialParameterInfo(TEXT("UseBrokenHlsl")), true);
        OutInstance->PostEditChange();
        FAssetRegistryModule::AssetCreated(OutInstance);
        // UMaterialInstance::ForceRecompileForRendering gained its EMaterialShaderPrecompileMode
        // parameter in UE 5.4; on 5.3 it takes none and always compiles the way the engine's
        // default does.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        OutInstance->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
#else
        OutInstance->ForceRecompileForRendering();
#endif

        const FMaterialResource* MasterResource =
            PinWright::MaterialShaderState::ResolveMaterialResource(OutMaster);
        const FMaterialResource* InstanceResource =
            PinWright::MaterialShaderState::ResolveMaterialResource(OutInstance);
        return Test.TestTrue(TEXT("fixture owns an actual instance static-permutation resource"),
            InstanceResource != nullptr && InstanceResource != MasterResource);
    }

    bool PWMtlFallbackRequireCleanMaterial(FAutomationTestBase& Test, const TCHAR* Stem,
        FString& OutAssetPath, UMaterial*& OutMaterial)
    {
        OutMaterial = PinWrightMaterialShaderStateTestFixtures::MakeCleanMaterial(
            Stem, OutAssetPath);
        if (!Test.TestNotNull(TEXT("clean material created"), OutMaterial))
        {
            CleanupTestAsset(OutAssetPath);
            return false;
        }

        const PinWright::MaterialShaderState::FState State =
            PinWright::MaterialShaderState::ProbeAndWait(OutMaterial);
        if (State.Status != PinWright::MaterialShaderState::EStatus::Completed)
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("shader-compile-unavailable"),
                FString::Printf(TEXT("The clean material fixture reported '%s', so the clean "
                                     "capture control could not be measured."),
                    PinWright::MaterialShaderState::ToWire(State.Status)));
            CleanupTestAsset(OutAssetPath);
            OutMaterial = nullptr;
            return false;
        }
        return true;
    }

    bool PWMtlFallbackIsEnvironmentCaptureFailure(const FString& ErrorCode)
    {
        return ErrorCode == ErrorCodes::ERR_CAPTURE_FAILED ||
            ErrorCode == ErrorCodes::ERR_THUMBNAIL_GENERATION_FAILED ||
            ErrorCode == ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND ||
            ErrorCode == ErrorCodes::ERR_EDITOR_NOT_AVAILABLE ||
            ErrorCode == ErrorCodes::ERR_RENDER_TARGET_CREATE_FAILED ||
            ErrorCode == ErrorCodes::ERR_SCENE_CAPTURE_FAILED ||
            ErrorCode == ErrorCodes::ERR_READ_PIXELS_FAILED;
    }

    bool PWMtlFallbackInvoke(FAutomationTestBase& Test, const TCHAR* Label,
        const TCHAR* Method, const TSharedPtr<FJsonObject>& Payload,
        FTestResponseCapture& Capture)
    {
        const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
        Test.TestTrue(*FString::Printf(TEXT("%s handler found"), Label), bFound);
        if (!bFound)
        {
            return false;
        }
        Test.TestTrue(*FString::Printf(TEXT("%s response callback ran"), Label),
            Capture.bWasCalled);
        return Capture.bWasCalled;
    }

    bool PWMtlFallbackSkipEnvironmentFailure(FAutomationTestBase& Test,
        const FTestResponseCapture& Capture, const TCHAR* ReasonSlug, const TCHAR* Context)
    {
        if (Capture.bSuccess || !PWMtlFallbackIsEnvironmentCaptureFailure(Capture.ErrorCode))
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(Test, ReasonSlug,
            FString::Printf(TEXT("%s returned the known environment failure '%s'."),
                Context, *Capture.ErrorCode));
        return true;
    }

    bool PWMtlFallbackAssertReadiness(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result, const FString& MaterialPath)
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        if (!Test.TestTrue(TEXT("response carries materialReadiness"),
                Result.IsValid() &&
                Result->TryGetObjectField(TEXT("materialReadiness"), Block)) ||
            !Block)
        {
            return false;
        }

        Test.TestTrue(TEXT("fallback occurred"),
            (*Block)->GetBoolField(TEXT("fallbackOccurred")));
        Test.TestTrue(TEXT("failed is distinct from compiling"),
            (*Block)->GetBoolField(TEXT("failed")) &&
            !(*Block)->GetBoolField(TEXT("compiling")));
        Test.TestTrue(TEXT("engine Default Material use is explicit"),
            (*Block)->GetBoolField(TEXT("usingDefaultMaterial")));
        Test.TestEqual(TEXT("fallback reason"),
            (*Block)->GetStringField(TEXT("reason")), FString(TEXT("shaderMapFailed")));

        const TArray<TSharedPtr<FJsonValue>>* Subjects = nullptr;
        if (!Test.TestTrue(TEXT("material subjects are present"),
                (*Block)->TryGetArrayField(TEXT("subjects"), Subjects)) ||
            !Subjects || !Test.TestTrue(TEXT("a material subject was measured"), Subjects->Num() > 0))
        {
            return false;
        }

        bool bFoundMaterial = false;
        for (const TSharedPtr<FJsonValue>& Value : *Subjects)
        {
            const TSharedPtr<FJsonObject>* Subject = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(Subject) || !Subject)
            {
                continue;
            }
            if ((*Subject)->GetStringField(TEXT("materialPath")) == MaterialPath)
            {
                bFoundMaterial = (*Subject)->GetStringField(TEXT("status")) == TEXT("failed") &&
                    (*Subject)->GetBoolField(TEXT("usingDefaultMaterial")) &&
                    (*Subject)->GetArrayField(TEXT("errors")).Num() > 0;
                break;
            }
        }
        Test.TestTrue(TEXT("the broken material and its compile errors are named"), bFoundMaterial);
        return bFoundMaterial;
    }

    bool PWMtlFallbackAssertCleanReadiness(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result, const FString& MaterialPath)
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        if (!Test.TestTrue(TEXT("clean response carries materialReadiness"),
                Result.IsValid() &&
                Result->TryGetObjectField(TEXT("materialReadiness"), Block)) ||
            !Block)
        {
            return false;
        }

        Test.TestTrue(TEXT("clean material readiness was measured"),
            (*Block)->GetBoolField(TEXT("measured")));
        Test.TestTrue(TEXT("clean material is compiled"),
            (*Block)->GetBoolField(TEXT("compiled")));
        Test.TestFalse(TEXT("clean material is not compiling"),
            (*Block)->GetBoolField(TEXT("compiling")));
        Test.TestFalse(TEXT("clean material is not failed"),
            (*Block)->GetBoolField(TEXT("failed")));
        Test.TestFalse(TEXT("clean material is not using Default Material"),
            (*Block)->GetBoolField(TEXT("usingDefaultMaterial")));
        Test.TestFalse(TEXT("clean material has no fallback"),
            (*Block)->GetBoolField(TEXT("fallbackOccurred")));

        const TArray<TSharedPtr<FJsonValue>>* Subjects = nullptr;
        bool bFoundMaterial = false;
        if ((*Block)->TryGetArrayField(TEXT("subjects"), Subjects) && Subjects)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Subjects)
            {
                const TSharedPtr<FJsonObject>* Subject = nullptr;
                if (Value.IsValid() && Value->TryGetObject(Subject) && Subject &&
                    (*Subject)->GetStringField(TEXT("materialPath")) == MaterialPath)
                {
                    bFoundMaterial = (*Subject)->GetStringField(TEXT("status")) ==
                        TEXT("completed");
                    break;
                }
            }
        }
        Test.TestTrue(TEXT("clean material subject is named and completed"), bFoundMaterial);
        return bFoundMaterial;
    }

    bool PWMtlFallbackHasFallbackWarning(const TSharedPtr<FJsonObject>& Result)
    {
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (!Result.IsValid() ||
            !Result->TryGetArrayField(TEXT("warnings"), Warnings) || !Warnings)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Warning : *Warnings)
        {
            FString Text;
            if (Warning.IsValid() && Warning->TryGetString(Text) &&
                Text.Contains(TEXT("Default Material")) &&
                Text.Contains(TEXT("allowFallback")))
            {
                return true;
            }
        }
        return false;
    }

    void PWMtlFallbackDeleteCapture(const FTestResponseCapture& Capture)
    {
        if (Capture.Result.IsValid())
        {
            FString Path;
            if (Capture.Result->TryGetStringField(TEXT("path"), Path) ||
                Capture.Result->TryGetStringField(TEXT("outputPath"), Path))
            {
                IFileManager::Get().Delete(*Path, false, true, true);
            }
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateThumbnailMaterialFallbackPolicyTest,
    "PinWright.asset.generate_thumbnail.MaterialFallbackRequiresOptIn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateThumbnailMaterialFallbackPolicyTest::RunTest(const FString& Parameters)
{
    FString MaterialPath;
    UMaterial* Material = nullptr;
    if (!PWMtlFallbackRequireBrokenMaterial(*this, TEXT("PWThumbFallback"),
            MaterialPath, Material))
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(MaterialPath); };
    const FString MaterialObjectPath = Material->GetPathName();

    FString CleanMaterialPath;
    UMaterial* CleanMaterial = nullptr;
    if (!PWMtlFallbackRequireCleanMaterial(*this, TEXT("PWThumbClean"),
            CleanMaterialPath, CleanMaterial))
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(CleanMaterialPath); };
    const FString CleanMaterialObjectPath = CleanMaterial->GetPathName();

    const FString OutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PinWright"),
        TEXT("Tests"), FString::Printf(TEXT("GenerateThumbnailMaterialFallback_%s.png"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*OutputPath, false, true, true); };
    TestTrue(TEXT("thumbnail output directory exists"),
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true) ||
        IFileManager::Get().DirectoryExists(*FPaths::GetPath(OutputPath)));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), MaterialPath);
    Payload->SetNumberField(TEXT("width"), 64);
    Payload->SetNumberField(TEXT("height"), 64);

    FTestResponseCapture Rejected;
    if (!PWMtlFallbackInvoke(*this, TEXT("default thumbnail"),
            TEXT("asset.generate_thumbnail"), Payload, Rejected))
    {
        return false;
    }
    if (PWMtlFallbackSkipEnvironmentFailure(*this, Rejected,
            TEXT("thumbnail-capture-unavailable"), TEXT("Default thumbnail capture")))
    {
        return true;
    }
    TestFalse(TEXT("fallback is not a successful capture by default"), Rejected.bSuccess);
    TestEqual(TEXT("default rejection has the dedicated code"), Rejected.ErrorCode,
        FString(ErrorCodes::ERR_MATERIAL_FALLBACK));
    if (Rejected.bSuccess || Rejected.ErrorCode != ErrorCodes::ERR_MATERIAL_FALLBACK)
    {
        return false;
    }
    PWMtlFallbackAssertReadiness(*this, Rejected.Result, MaterialObjectPath);
    TestFalse(TEXT("default rejection did not create the opt-in output file"),
        IFileManager::Get().FileExists(*OutputPath));

    Payload->SetBoolField(TEXT("allowFallback"), true);
    Payload->SetStringField(TEXT("outputPath"), OutputPath);
    FTestResponseCapture Allowed;
    if (!PWMtlFallbackInvoke(*this, TEXT("opt-in thumbnail"),
            TEXT("asset.generate_thumbnail"), Payload, Allowed))
    {
        return false;
    }
    if (PWMtlFallbackSkipEnvironmentFailure(*this, Allowed,
            TEXT("thumbnail-capture-unavailable"), TEXT("Opt-in thumbnail capture")))
    {
        return true;
    }
    if (!TestTrue(TEXT("allowFallback retains successful thumbnail output"), Allowed.bSuccess))
    {
        return false;
    }
    TestTrue(TEXT("allowFallback retains image statistics"),
        Allowed.Result.IsValid() && Allowed.Result->HasField(TEXT("imageStats")));
    FString ReturnedOutputPath;
    TestTrue(TEXT("allowFallback retains the requested outputPath"),
        Allowed.Result.IsValid() &&
        Allowed.Result->TryGetStringField(TEXT("outputPath"), ReturnedOutputPath));
    TestEqual(TEXT("retained outputPath is the requested file"), ReturnedOutputPath, OutputPath);
    TestTrue(TEXT("allowFallback writes the retained thumbnail file"),
        IFileManager::Get().FileExists(*OutputPath));
    TestTrue(TEXT("allowFallback retains an explicit fallback warning"),
        PWMtlFallbackHasFallbackWarning(Allowed.Result));
    PWMtlFallbackAssertReadiness(*this, Allowed.Result, MaterialObjectPath);

    Payload->SetStringField(TEXT("assetPath"), CleanMaterialPath);
    Payload->RemoveField(TEXT("allowFallback"));
    Payload->RemoveField(TEXT("outputPath"));
    FTestResponseCapture Clean;
    if (!PWMtlFallbackInvoke(*this, TEXT("clean thumbnail"),
            TEXT("asset.generate_thumbnail"), Payload, Clean))
    {
        return false;
    }
    if (PWMtlFallbackSkipEnvironmentFailure(*this, Clean,
            TEXT("thumbnail-capture-unavailable"), TEXT("Clean thumbnail capture")))
    {
        return true;
    }
    if (!TestTrue(TEXT("clean material thumbnail succeeds without allowFallback"), Clean.bSuccess))
    {
        return false;
    }
    PWMtlFallbackAssertCleanReadiness(*this, Clean.Result, CleanMaterialObjectPath);
    TestFalse(TEXT("clean thumbnail has no fallback warning"),
        PWMtlFallbackHasFallbackWarning(Clean.Result));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGenerateThumbnailMaterialInstanceFallbackPolicyTest,
    "PinWright.asset.generate_thumbnail.MaterialInstanceFallbackRequiresOptIn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGenerateThumbnailMaterialInstanceFallbackPolicyTest::RunTest(const FString& Parameters)
{
    FString MasterPath;
    FString InstancePath;
    UMaterial* Master = nullptr;
    UMaterialInstanceConstant* Instance = nullptr;
    const bool bFixtureReady = PWMtlFallbackMakeBrokenInstance(
        *this, MasterPath, InstancePath, Master, Instance);
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(InstancePath);
        CleanupTestAsset(MasterPath);
    };
    if (!bFixtureReady)
    {
        return true;
    }

    const PinWright::MaterialShaderState::FState InstanceState =
        PinWright::MaterialShaderState::Probe(Instance);
    if (InstanceState.Status != PinWright::MaterialShaderState::EStatus::Failed)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("shader-compile-unavailable"),
            FString::Printf(TEXT("The broken instance permutation reported '%s'."),
                PinWright::MaterialShaderState::ToWire(InstanceState.Status)));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), InstancePath);
    Payload->SetNumberField(TEXT("width"), 64);
    Payload->SetNumberField(TEXT("height"), 64);
    FTestResponseCapture Capture;
    if (!PWMtlFallbackInvoke(*this, TEXT("material-instance thumbnail"),
            TEXT("asset.generate_thumbnail"), Payload, Capture))
    {
        return false;
    }
    if (PWMtlFallbackSkipEnvironmentFailure(*this, Capture,
            TEXT("thumbnail-capture-unavailable"), TEXT("Material-instance thumbnail capture")))
    {
        return true;
    }
    TestFalse(TEXT("broken instance thumbnail is rejected"), Capture.bSuccess);
    TestEqual(TEXT("broken instance uses the dedicated error"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_MATERIAL_FALLBACK));
    PWMtlFallbackAssertReadiness(*this, Capture.Result, Instance->GetPathName());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderCaptureAssetPreviewMaterialFallbackPolicyTest,
    "PinWright.render.capture_asset_preview.MaterialFallbackRequiresOptIn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRenderCaptureAssetPreviewMaterialFallbackPolicyTest::RunTest(const FString& Parameters)
{
    FString MaterialPath;
    UMaterial* Material = nullptr;
    if (!PWMtlFallbackRequireBrokenMaterial(*this, TEXT("PWPreviewFallback"),
            MaterialPath, Material))
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(MaterialPath); };
    const FString MaterialObjectPath = Material->GetPathName();

    FString CleanMaterialPath;
    UMaterial* CleanMaterial = nullptr;
    if (!PWMtlFallbackRequireCleanMaterial(*this, TEXT("PWPreviewClean"),
            CleanMaterialPath, CleanMaterial))
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(CleanMaterialPath); };
    const FString CleanMaterialObjectPath = CleanMaterial->GetPathName();

    UStaticMesh* SourceMesh = LoadObject<UStaticMesh>(nullptr,
        TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!SourceMesh)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("/Engine/BasicShapes/Cube.Cube is unavailable."));
        return true;
    }

    const FString MeshPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/PWPreviewMesh_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* MeshPackage = CreatePackage(*MeshPath);
    UStaticMesh* Mesh = MeshPackage
        ? DuplicateObject<UStaticMesh>(SourceMesh, MeshPackage,
            FName(*FPackageName::GetLongPackageAssetName(MeshPath)))
        : nullptr;
    if (!TestNotNull(TEXT("preview mesh fixture created"), Mesh))
    {
        CleanupTestAsset(MeshPath);
        return true;
    }
    ON_SCOPE_EXIT
    {
        if (GEditor)
        {
            if (UAssetEditorSubsystem* Editors =
                    GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                Editors->CloseAllEditorsForAsset(Mesh);
            }
        }
        CleanupTestAsset(MeshPath);
    };

    if (Mesh->GetStaticMaterials().Num() == 0)
    {
        Mesh->GetStaticMaterials().Add(FStaticMaterial(Material));
    }
    else
    {
        Mesh->GetStaticMaterials()[0].MaterialInterface = Material;
    }
    Mesh->PostEditChange();
    FAssetRegistryModule::AssetCreated(Mesh);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), MeshPath);
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 256);
    // Keep closure synchronous and fixture-owned; the verb's default deferred close can otherwise
    // outlive the transient mesh this test tears down.
    Payload->SetBoolField(TEXT("closeAfterCapture"), false);
    Payload->SetBoolField(TEXT("measureCoverage"), false);

    FTestResponseCapture Rejected;
    if (!PWMtlFallbackInvoke(*this, TEXT("default asset preview"),
            TEXT("render.capture_asset_preview"), Payload, Rejected))
    {
        return false;
    }
    ON_SCOPE_EXIT { PWMtlFallbackDeleteCapture(Rejected); };
    if (PWMtlFallbackSkipEnvironmentFailure(*this, Rejected,
            TEXT("asset-preview-capture-unavailable"), TEXT("Default asset preview")))
    {
        return true;
    }
    TestFalse(TEXT("fallback is not a successful capture by default"), Rejected.bSuccess);
    TestEqual(TEXT("default rejection has the dedicated code"), Rejected.ErrorCode,
        FString(ErrorCodes::ERR_MATERIAL_FALLBACK));
    if (Rejected.bSuccess || Rejected.ErrorCode != ErrorCodes::ERR_MATERIAL_FALLBACK)
    {
        return false;
    }
    PWMtlFallbackAssertReadiness(*this, Rejected.Result, MaterialObjectPath);

    Payload->SetBoolField(TEXT("allowFallback"), true);
    FTestResponseCapture Allowed;
    if (!PWMtlFallbackInvoke(*this, TEXT("opt-in asset preview"),
            TEXT("render.capture_asset_preview"), Payload, Allowed))
    {
        return false;
    }
    ON_SCOPE_EXIT { PWMtlFallbackDeleteCapture(Allowed); };
    if (PWMtlFallbackSkipEnvironmentFailure(*this, Allowed,
            TEXT("asset-preview-capture-unavailable"), TEXT("Opt-in asset preview")))
    {
        return true;
    }
    if (!TestTrue(TEXT("allowFallback retains a successful asset preview"), Allowed.bSuccess))
    {
        return false;
    }
    TestTrue(TEXT("allowFallback retains the captured image"),
        Allowed.Result.IsValid() && Allowed.Result->HasField(TEXT("path")) &&
        Allowed.Result->HasField(TEXT("imageStats")));
    TestTrue(TEXT("allowFallback retains an explicit fallback warning"),
        PWMtlFallbackHasFallbackWarning(Allowed.Result));
    PWMtlFallbackAssertReadiness(*this, Allowed.Result, MaterialObjectPath);

    if (GEditor)
    {
        if (UAssetEditorSubsystem* Editors =
                GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
        {
            Editors->CloseAllEditorsForAsset(Mesh);
        }
    }
    Mesh->GetStaticMaterials()[0].MaterialInterface = CleanMaterial;
    Mesh->PostEditChange();
    Payload->RemoveField(TEXT("allowFallback"));

    FTestResponseCapture Clean;
    if (!PWMtlFallbackInvoke(*this, TEXT("clean asset preview"),
            TEXT("render.capture_asset_preview"), Payload, Clean))
    {
        return false;
    }
    ON_SCOPE_EXIT { PWMtlFallbackDeleteCapture(Clean); };
    if (PWMtlFallbackSkipEnvironmentFailure(*this, Clean,
            TEXT("asset-preview-capture-unavailable"), TEXT("Clean asset preview")))
    {
        return true;
    }
    if (!TestTrue(TEXT("clean asset preview succeeds without allowFallback"), Clean.bSuccess))
    {
        return false;
    }
    PWMtlFallbackAssertCleanReadiness(*this, Clean.Result, CleanMaterialObjectPath);
    TestFalse(TEXT("clean asset preview has no fallback warning"),
        PWMtlFallbackHasFallbackWarning(Clean.Result));
    return true;
}
