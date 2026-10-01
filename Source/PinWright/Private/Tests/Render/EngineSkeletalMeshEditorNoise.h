// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// UE 5.7's SkeletalMeshModelingTools editor mode binds a USkeletonModifier to every Skeletal Mesh
// Editor it opens (SkeletalMeshModelingToolsEditorMode.cpp, SetSkeletalMesh), and SetSkeletalMesh
// refuses any /Engine/ mesh with one LogAnimation Error, "Cannot modify built-in engine asset"
// (SkeletonModifier.cpp). 5.8 builds a read-only reader from the reference skeleton instead, and
// 5.6 and earlier bind no modifier, so neither logs anything. A capture that opens the editor for
// the engine's SkeletalCube fixture therefore logs that Error exactly once on 5.7, and only when
// the editor was not already open. Declared as exactly that, so no test needs a blanket
// bSuppressLogErrors.

#include "CoreMinimal.h"
#include "Editor.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Subsystems/AssetEditorSubsystem.h"

#include "Compat/EngineVersionCompat.h"

inline void ExpectEngineSkeletalMeshEditorOpenError(FAutomationTestBase& Test, const TCHAR* AssetPath)
{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 7
    if (!GEditor || !FString(AssetPath).StartsWith(TEXT("/Engine/"))
        || !FModuleManager::Get().IsModuleLoaded(TEXT("SkeletalMeshModelingTools")))
    {
        return;
    }
    UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    UObject* Asset = LoadObject<UObject>(nullptr, AssetPath);
    if (Subsystem && Asset && Subsystem->FindEditorForAsset(Asset, false) == nullptr)
    {
        Test.AddExpectedErrorPlain(TEXT("Skeleton Modifier: Cannot modify built-in engine asset."),
            EAutomationExpectedErrorFlags::Contains, 1);
    }
#endif
}
