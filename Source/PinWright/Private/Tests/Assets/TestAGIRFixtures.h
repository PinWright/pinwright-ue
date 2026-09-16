// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once


#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Tests/IrCore/IrTestFixture.h"

namespace AGIRTestFixtures
{
// The development host's own Mannequin locomotion AnimBP;
// carries a locomotion state machine and its TargetSkeleton is SK_Mannequin.
// (Name kept as LyraMannequinAnimBPPath so callers in other files still compile.)
inline const TCHAR* LyraMannequinAnimBPPath =
    TEXT("/Game/Characters/Mannequins/Animations/ABP_Manny.ABP_Manny");

inline UAnimBlueprint* LoadLyraMannequinAnimBP()
{
    return LoadObject<UAnimBlueprint>(nullptr, LyraMannequinAnimBPPath);
}

inline UAnimBlueprint* CreateFreshAnimBlueprint(const FString& PackagePath, USkeleton* Skeleton)
{
    UAnimBlueprint* NewBP = IrTest::CreateFactoryAssetAtPath<UAnimBlueprint, UAnimBlueprintFactory>(
        PackagePath,
        [Skeleton](UAnimBlueprintFactory& Factory)
        {
            Factory.TargetSkeleton = Skeleton;
            Factory.ParentClass = UAnimInstance::StaticClass();
        });
    if (!NewBP)
    {
        return nullptr;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBP);
    return NewBP;
}
} // namespace AGIRTestFixtures
