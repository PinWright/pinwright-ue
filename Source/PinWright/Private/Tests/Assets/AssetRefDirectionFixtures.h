// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared fixture for asset-reference-direction tests: builds a hard package
// dependency A -> B on disk (A references B) and rescans the asset registry so
// both directions are tracked deterministically. Used by
// TestAssetReferenceDirection.cpp and TestAssetReferencersWarning.cpp.
//
// Headers under Tests/ are unity-merged, so the builder is defined inline to
// avoid ODR collisions (matching the convention of the other helper headers in
// this directory).
#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

namespace AssetRefDirectionFixtures
{
    // Builds a hard dependency A -> B (A references B) on disk under
    // /Game/__PW_GatewayTests with GUID-suffixed names derived from PathLabel
    // (e.g. "RefDir" -> RefDirA_<guid>/RefDirB_<guid>), compiles + saves both
    // blueprints, then synchronously rescans the asset registry. On any failure
    // it cleans up whatever was created, reports the failure on Test, and returns
    // false; on success OutPathA/OutPathB carry the package paths.
    inline bool BuildHardDependency(FAutomationTestBase& Test, const TCHAR* PathLabel,
        FString& OutPathA, FString& OutPathB)
    {
        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        OutPathB = FString::Printf(TEXT("/Game/__PW_GatewayTests/%sB_%s"), PathLabel, *Suffix);
        OutPathA = FString::Printf(TEXT("/Game/__PW_GatewayTests/%sA_%s"), PathLabel, *Suffix);

        // 1. Create BP_B (the referenced asset).
        UPackage* PkgB = CreatePackage(*OutPathB);
        if (!Test.TestNotNull(TEXT("PkgB created"), PkgB))
        {
            return false;
        }

        UBlueprint* BPB = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), PkgB,
            FName(*FPackageName::GetLongPackageAssetName(OutPathB)),
            BPTYPE_Normal, UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        if (!Test.TestNotNull(TEXT("BP_B created"), BPB))
        {
            CleanupTestAsset(OutPathA);
            CleanupTestAsset(OutPathB);
            return false;
        }
        FKismetEditorUtilities::CompileBlueprint(BPB);
        UEditorAssetLibrary::SaveAsset(OutPathB, /*bOnlyIfIsDirty=*/false);

        // 2. Create BP_A with a member variable typed as B's generated class —
        //    this creates a hard package-level dependency from A's package to B's.
        UPackage* PkgA = CreatePackage(*OutPathA);
        if (!Test.TestNotNull(TEXT("PkgA created"), PkgA))
        {
            CleanupTestAsset(OutPathB);
            return false;
        }
        UBlueprint* BPA = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), PkgA,
            FName(*FPackageName::GetLongPackageAssetName(OutPathA)),
            BPTYPE_Normal, UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        if (!Test.TestNotNull(TEXT("BP_A created"), BPA))
        {
            CleanupTestAsset(OutPathA);
            CleanupTestAsset(OutPathB);
            return false;
        }
        if (BPB->GeneratedClass)
        {
            FEdGraphPinType PinType;
            PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
            PinType.PinSubCategoryObject = BPB->GeneratedClass;
            FBlueprintEditorUtils::AddMemberVariable(BPA, TEXT("RefToB"), PinType);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BPA);
        }
        FKismetEditorUtilities::CompileBlueprint(BPA);
        UEditorAssetLibrary::SaveAsset(OutPathA, /*bOnlyIfIsDirty=*/false);

        // 3. Force the asset registry to rescan both files so the A -> B
        //    dependency (and the inverse referencer edge) is tracked.
        IAssetRegistry& Registry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        FString FilenameA, FilenameB;
        const bool bGotA = FPackageName::TryConvertLongPackageNameToFilename(
            OutPathA, FilenameA, TEXT(".uasset"));
        const bool bGotB = FPackageName::TryConvertLongPackageNameToFilename(
            OutPathB, FilenameB, TEXT(".uasset"));
        if (bGotA && bGotB)
        {
            Registry.ScanFilesSynchronous({FilenameA, FilenameB}, /*bForceRescan=*/true);
        }
        else
        {
            Registry.ScanPathsSynchronous({TEXT("/Game/__PW_GatewayTests")}, /*bForceRescan=*/true);
        }
        return true;
    }
}
