// Copyright (c) 2026 Alexander Penkin. MIT License.

// Test-only fixture for the mismatched-inner-name asset-dump regressions
// (plan "Fix asset-dump cache staleness loop", Fix 2). Engine "Bake Out
// Materials" writes packages named M_<mesh>_<mat>_<GUID> whose inner asset is
// MI_M_... — the inner object name NEVER equals the package tail, so a
// pending-queue entry carrying only the bare package name fails LoadObject and
// the sweep writes a skip stub forever. This fixture saves a real on-disk
// package with that shape: package tail "PkgTail_<guid>", single inner
// UMaterialInstanceConstant named "MI_Inner_<guid>".
//
// UMaterialInstanceConstant is used because the dump pipeline handles it
// generically (MaterialInstanceDumpBuilder tolerates a null Parent), it
// matches the real bake-out payload, and SavePackage accepts it headlessly.
//
// Headers under Tests/ are unity-merged, so everything here is inline in a
// named namespace to avoid ODR collisions (convention of the sibling helper
// headers in this directory).

#pragma once

#include "CoreMinimal.h"
#include "Tests/TestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace AssetDumpMismatchedNameFixture
{
    // Temporarily disables the editor's validate-on-save, reached via the
    // UDataValidationSettings CDO by REFLECTION so the plugin takes no hard
    // dependency on the DataValidation module. Cloned from the pattern in
    // Tests/World/GameFeaturesTestFixture.h (PinWrightGF namespace) — that
    // header is #if-gated on the GameFeatures module being available, so its
    // copy cannot be reused unconditionally from Utility tests. Without this,
    // SavePackage can schedule the deferred asset-validation pass whose
    // ensures the automation framework counts as test failures.
    struct FScopedDisableValidateOnSaveReflect
    {
        FBoolProperty* Prop = nullptr;
        UObject* CDO = nullptr;
        bool bPrev = false;

        FScopedDisableValidateOnSaveReflect()
        {
            if (UClass* Cls = FindObject<UClass>(nullptr, TEXT("/Script/DataValidation.DataValidationSettings")))
            {
                CDO = Cls->GetDefaultObject();
                Prop = FindFProperty<FBoolProperty>(Cls, TEXT("bValidateOnSave"));
                if (Prop && CDO)
                {
                    bPrev = Prop->GetPropertyValue_InContainer(CDO);
                    Prop->SetPropertyValue_InContainer(CDO, false);
                }
            }
        }

        ~FScopedDisableValidateOnSaveReflect()
        {
            if (Prop && CDO)
            {
                Prop->SetPropertyValue_InContainer(CDO, bPrev);
            }
        }
    };

    // Identity + on-disk location of one generated mismatched-name asset.
    // FolderPath is GUID-isolated so a non-recursive folder sweep of it sees
    // exactly this one package.
    struct FMismatchedNameAsset
    {
        FString FolderPath;      // /Game/__PW_DumpTests/<guid>
        FString PackagePath;     // <FolderPath>/PkgTail_<guid>
        FString InnerName;       // MI_Inner_<guid>
        FString ObjectPath;      // <PackagePath>.<InnerName>
        FString PackageFilename; // absolute .uasset path
        UMaterialInstanceConstant* Instance = nullptr;
        bool bValid = false;
    };

    // Saves the fixture package to disk (validation-on-save bracketed off) and
    // synchronously rescans the file so the asset registry carries the row +
    // package data (saved hash) before a sweep starts.
    inline bool SaveAndRescan(FMismatchedNameAsset& Asset, FString& OutError)
    {
        if (!Asset.Instance)
        {
            OutError = TEXT("fixture instance is null");
            return false;
        }

        UPackage* Package = Asset.Instance->GetOutermost();
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Public | RF_Standalone;
        Args.SaveFlags = SAVE_NoError;

        bool bSaved = false;
        {
            FScopedDisableValidateOnSaveReflect DisableValidation;
            bSaved = UPackage::SavePackage(Package, Asset.Instance, *Asset.PackageFilename, Args);
        }
        if (!bSaved)
        {
            OutError = FString::Printf(TEXT("SavePackage failed for %s"), *Asset.PackageFilename);
            return false;
        }

        IAssetRegistry& Registry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        Registry.ScanFilesSynchronous({Asset.PackageFilename}, /*bForceRescan=*/true);
        return true;
    }

    // Creates the on-disk fixture: CreatePackage(PkgTail_<guid>) + a single
    // inner MI named MI_Inner_<guid> (mismatched by construction), saved +
    // registry-scanned. Caller must call Cleanup() during teardown.
    inline bool Create(FMismatchedNameAsset& Out, FString& OutError)
    {
        Out = FMismatchedNameAsset();

        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Out.FolderPath = FString::Printf(TEXT("/Game/__PW_DumpTests/%s"), *Suffix);
        Out.PackagePath = FString::Printf(TEXT("%s/PkgTail_%s"), *Out.FolderPath, *Suffix);
        Out.InnerName = FString::Printf(TEXT("MI_Inner_%s"), *Suffix);
        Out.ObjectPath = FString::Printf(TEXT("%s.%s"), *Out.PackagePath, *Out.InnerName);

        if (!FPackageName::TryConvertLongPackageNameToFilename(
                Out.PackagePath, Out.PackageFilename, FPackageName::GetAssetPackageExtension()))
        {
            OutError = FString::Printf(TEXT("could not resolve filename for %s"), *Out.PackagePath);
            return false;
        }

        UPackage* Package = CreatePackage(*Out.PackagePath);
        if (!Package)
        {
            OutError = FString::Printf(TEXT("CreatePackage failed for %s"), *Out.PackagePath);
            return false;
        }

        // No Parent on purpose: keeps the fixture self-contained and exercises
        // the dump builder's null-parent tolerance.
        Out.Instance = NewObject<UMaterialInstanceConstant>(
            Package, FName(*Out.InnerName), RF_Public | RF_Standalone);
        if (!Out.Instance)
        {
            OutError = FString::Printf(TEXT("NewObject<UMaterialInstanceConstant> failed for %s"), *Out.ObjectPath);
            return false;
        }

        if (!SaveAndRescan(Out, OutError))
        {
            return false;
        }

        Out.bValid = true;
        return true;
    }

    // Mutates the instance (adds a scalar parameter override) and re-saves +
    // rescans, so the package's registry saved-hash genuinely changes — the
    // "source file changed on disk" simulation for cache re-queue tests. A
    // plain mtime touch would not change a PackageSavedHash-kind fingerprint.
    inline bool ResaveWithContentChange(FMismatchedNameAsset& Asset, FString& OutError)
    {
        if (!Asset.Instance)
        {
            OutError = TEXT("fixture instance is null");
            return false;
        }

        FScalarParameterValue Param;
        Param.ParameterInfo = FMaterialParameterInfo(
            FName(*FString::Printf(TEXT("PW_Requeue_%d"), Asset.Instance->ScalarParameterValues.Num())));
        Param.ParameterValue = 1.0f + Asset.Instance->ScalarParameterValues.Num();
        Asset.Instance->ScalarParameterValues.Add(Param);
        Asset.Instance->MarkPackageDirty();

        return SaveAndRescan(Asset, OutError);
    }

    // Deletes the asset (in-memory object + on-disk .uasset via
    // CleanupTestAsset) and removes the GUID-isolated content folder.
    inline void Cleanup(FMismatchedNameAsset& Asset)
    {
        if (!Asset.ObjectPath.IsEmpty())
        {
            CleanupTestAsset(Asset.ObjectPath);
        }
        Asset.Instance = nullptr;

        FString FolderFilename;
        if (!Asset.FolderPath.IsEmpty()
            && FPackageName::TryConvertLongPackageNameToFilename(Asset.FolderPath, FolderFilename, TEXT("")))
        {
            IFileManager::Get().DeleteDirectory(*FolderFilename, /*RequireExists=*/false, /*Tree=*/true);
        }
    }
}
