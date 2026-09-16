// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared fixture helpers for WidgetXml/* tests. Extracted from anonymous namespaces
// previously duplicated across TestWidgetEventBinding.cpp, TestWidgetXmlHandlers.cpp,
// TestWidgetAddUserWidgetHandler.cpp and TestWidgetWrapHandler.cpp. Required because
// the plugin's tests now share a single module with Unity builds: anonymous-namespace
// helpers with the same name across .cpp files in the same folder produce ODR /
// redefinition errors when Unity merges them into one translation unit.
//
// Conventions match Tests/Assets/NiagaraEditTestUtils.h and Tests/Widget/WidgetTestFixtures.h:
// named namespace + inline functions, no module API macro.


#include "CoreMinimal.h"
#include "Blueprint/WidgetTree.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Tests/TestUtils.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

namespace WidgetXmlTestHelpers
{
    inline FString MakeXmlTestAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/_Test/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Invokes widget.create_widget_blueprint at AssetPath, returning the handler-found bool.
    // The success flag and any error live in Capture.
    inline bool CreateXmlTestWidget(const FString& AssetPath, FTestResponseCapture& Capture)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
        const FString Folder = AssetPath.Left(AssetPath.Len() - AssetName.Len() - 1);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), AssetName);
        Payload->SetStringField(TEXT("folder"), Folder);
        return InvokeHandlerWithCapture(TEXT("widget.create_widget_blueprint"), Payload, Capture);
    }

    // Releases the package, clears the WidgetTree root to avoid ForceDeleteObjects
    // ensure failures, drops the dirty flag, GCs, and finally deletes the asset.
    inline void CleanupXmlTestAsset(const FString& PackagePath)
    {
        FString ObjectPath = PackagePath;
        FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        if (!AssetName.IsEmpty() && !ObjectPath.Contains(TEXT(".")))
        {
            ObjectPath = PackagePath + TEXT(".") + AssetName;
        }
        if (UWidgetBlueprint* WB = FindObject<UWidgetBlueprint>(nullptr, *ObjectPath))
        {
            if (WB->WidgetTree)
            {
                WB->WidgetTree->RootWidget = nullptr;
            }
        }
        if (UPackage* Pkg = FindPackage(nullptr, *PackagePath))
        {
            Pkg->SetDirtyFlag(false);
        }
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
        CleanupTestAsset(PackagePath);
    }

    // Build an in-memory UWidgetBlueprint at a /Game/_Test/... pathname so the
    // widget-xml handlers' LoadWidgetBlueprint -> FindObject lookup resolves it.
    // Avoids FKismetEditorUtilities::CreateBlueprint (full BP compile), on-disk
    // save, and matching DeleteAsset+GC cleanup — that path adds ~7-8s per WBP
    // versus this transient construction.
    //
    // BS_BeingCreated short-circuits MarkBlueprintAsStructurallyModified's
    // synchronous skeleton recompile (BlueprintEditorUtils.cpp). The import / wrap
    // handlers call MarkBlueprintAsStructurallyModified, and on a stub BP a real
    // recompile is both unnecessary and slow.
    inline UWidgetBlueprint* MakeOnDiskShapedWidgetBlueprint(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        Package->SetFlags(RF_Transient);

        UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
            Package, *AssetName, RF_Transient | RF_Public | RF_Standalone);
        if (!WBP)
        {
            return nullptr;
        }
        WBP->WidgetTree = NewObject<UWidgetTree>(WBP, NAME_None, RF_Transient);
        WBP->Status = BS_BeingCreated;
        return WBP;
    }
}
