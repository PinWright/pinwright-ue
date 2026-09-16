// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Asset/PhysicsAssetDumpBuilder.h"

#include "PhysicsEngine/PhysicsAsset.h"
#if __has_include("PhysicsEngine/SkeletalBodySetup.h")
#include "PhysicsEngine/SkeletalBodySetup.h"
#endif
// USkeletalBodySetup is declared in PhysicsEngine/PhysicsAsset.h on UE versions that lack the above header
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "BodySetupCore.h"
#include "BodySetupEnums.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/Class.h"
#include "Utils/JsonBuilders.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"

namespace
{
    // EBodyCollisionResponse is a namespace-scoped UENUM; LexToString is the canonical
    // PhysicsCore-provided stringifier (StaticEnum<> can't resolve namespaced enums here).
    FString CollisionResponseToString(EBodyCollisionResponse::Type Type)
    {
        return LexToString(Type);
    }
}

TSharedPtr<FJsonObject> PhysicsAssetDumpBuilder::BuildPhysicsAssetJson(const UPhysicsAsset* Asset)
{
    if (!Asset)
    {
        return nullptr;
    }

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    // PreviewMesh: PhysicsAsset stores a TSoftObjectPtr to a SkeletalMesh. The
    // soft-pointer field is editor-only; runtime builds get an empty string.
    FString PreviewMeshPath;
#if WITH_EDITORONLY_DATA
    PreviewMeshPath = Asset->PreviewSkeletalMesh.ToString();
#endif
    Root->SetStringField(TEXT("previewMesh"), PreviewMeshPath);

    TArray<TSharedPtr<FJsonValue>> BodiesArr;
    BodiesArr.Reserve(Asset->SkeletalBodySetups.Num());
    for (const TObjectPtr<USkeletalBodySetup>& BodySetupPtr : Asset->SkeletalBodySetups)
    {
        const USkeletalBodySetup* BodySetup = BodySetupPtr.Get();
        if (!BodySetup)
        {
            continue;
        }

        TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
        Body->SetStringField(TEXT("name"), BodySetup->GetName());
        Body->SetStringField(TEXT("bone"), BodySetup->BoneName.ToString());
        Body->SetStringField(TEXT("physicsType"), JsonBuilders::EnumValueToString<EPhysicsType>(BodySetup->PhysicsType));
        Body->SetStringField(TEXT("collisionResponse"), CollisionResponseToString(BodySetup->CollisionReponse));

        TSharedRef<FJsonObject> Elements = MakeShared<FJsonObject>();
        const FKAggregateGeom& AggGeom = BodySetup->AggGeom;
        Elements->SetNumberField(TEXT("sphere"),        AggGeom.SphereElems.Num());
        Elements->SetNumberField(TEXT("box"),           AggGeom.BoxElems.Num());
        Elements->SetNumberField(TEXT("sphyl"),         AggGeom.SphylElems.Num());
        Elements->SetNumberField(TEXT("convex"),        AggGeom.ConvexElems.Num());
        Elements->SetNumberField(TEXT("taperedCapsule"), AggGeom.TaperedCapsuleElems.Num());
        Body->SetObjectField(TEXT("elements"), Elements);

        BodiesArr.Add(MakeShared<FJsonValueObject>(Body));
    }
    Root->SetArrayField(TEXT("bodies"), BodiesArr);

    TArray<TSharedPtr<FJsonValue>> ConstraintsArr;
    ConstraintsArr.Reserve(Asset->ConstraintSetup.Num());
    for (const TObjectPtr<UPhysicsConstraintTemplate>& TemplatePtr : Asset->ConstraintSetup)
    {
        const UPhysicsConstraintTemplate* Template = TemplatePtr.Get();
        if (!Template)
        {
            continue;
        }

        const FConstraintInstance& Inst = Template->DefaultInstance;

        TSharedRef<FJsonObject> Constraint = MakeShared<FJsonObject>();
        Constraint->SetStringField(TEXT("name"), Inst.JointName.ToString());
        Constraint->SetStringField(TEXT("bone1"), Inst.ConstraintBone1.ToString());
        Constraint->SetStringField(TEXT("bone2"), Inst.ConstraintBone2.ToString());

        TSharedRef<FJsonObject> LinearLimit = MakeShared<FJsonObject>();
        LinearLimit->SetStringField(TEXT("x"), JsonBuilders::EnumValueToString<ELinearConstraintMotion>(Inst.ProfileInstance.LinearLimit.XMotion));
        LinearLimit->SetStringField(TEXT("y"), JsonBuilders::EnumValueToString<ELinearConstraintMotion>(Inst.ProfileInstance.LinearLimit.YMotion));
        LinearLimit->SetStringField(TEXT("z"), JsonBuilders::EnumValueToString<ELinearConstraintMotion>(Inst.ProfileInstance.LinearLimit.ZMotion));
        Constraint->SetObjectField(TEXT("linearLimit"), LinearLimit);

        TSharedRef<FJsonObject> AngularLimit = MakeShared<FJsonObject>();
        AngularLimit->SetStringField(TEXT("twistMotion"),  JsonBuilders::EnumValueToString<EAngularConstraintMotion>(Inst.ProfileInstance.TwistLimit.TwistMotion));
        AngularLimit->SetStringField(TEXT("swing1Motion"), JsonBuilders::EnumValueToString<EAngularConstraintMotion>(Inst.ProfileInstance.ConeLimit.Swing1Motion));
        AngularLimit->SetStringField(TEXT("swing2Motion"), JsonBuilders::EnumValueToString<EAngularConstraintMotion>(Inst.ProfileInstance.ConeLimit.Swing2Motion));
        Constraint->SetObjectField(TEXT("angularLimit"), AngularLimit);

        Constraint->SetBoolField(TEXT("bBreakable"), Inst.ProfileInstance.bLinearBreakable || Inst.ProfileInstance.bAngularBreakable);

        ConstraintsArr.Add(MakeShared<FJsonValueObject>(Constraint));
    }
    Root->SetArrayField(TEXT("constraints"), ConstraintsArr);

    return Root;
}

namespace
{
    UClass* GetPhysicsAssetSidecarClass()
    {
        return UPhysicsAsset::StaticClass();
    }

    TSharedPtr<FJsonObject> BuildPhysicsAssetSidecar(UObject* Asset)
    {
        return PhysicsAssetDumpBuilder::BuildPhysicsAssetJson(Cast<UPhysicsAsset>(Asset));
    }
}

REGISTER_DUMP_JSON_SIDECAR(TEXT("physics_asset"), DumpFileNames::PhysicsAsset,
    &GetPhysicsAssetSidecarClass, &BuildPhysicsAssetSidecar,
    nullptr, nullptr, nullptr, 100);
