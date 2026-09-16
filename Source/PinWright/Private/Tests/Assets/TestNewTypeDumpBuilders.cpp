// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shape + content coverage for the six "new type" asset-dump sidecar builders.
//
// Every fixture below is POPULATED before the builder runs. That is load-bearing, not
// tidiness: each builder emits its arrays unconditionally
// (e.g. SkeletonDumpBuilder.cpp `Root->SetArrayField(TEXT("bones"), BonesArr);`,
// PhysicsAssetDumpBuilder.cpp `Root->SetArrayField(TEXT("bodies"), BodiesArr);`), so a
// default-constructed transient asset yields an EMPTY array and the per-element emission
// loop never executes. A test that only asserts `HasTypedField<EJson::Array>` over such a
// fixture passes with the entire loop body deleted — it cannot see the regression it
// exists for. The value assertions here run the loops and pin the emitted fields.

#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"

#include "Tests/TestUtils.h"

#include "Handlers/Asset/PhysicsAssetDumpBuilder.h"
#include "Handlers/Asset/SkeletonDumpBuilder.h"
#include "Handlers/Asset/AnimMontageDumpBuilder.h"
#include "Handlers/Asset/BlendSpaceDumpBuilder.h"
#include "Handlers/Asset/LandscapeGrassTypeDumpBuilder.h"
#include "Handlers/Asset/SubsurfaceProfileDumpBuilder.h"

#include "PhysicsEngine/PhysicsAsset.h"
#if __has_include("PhysicsEngine/SkeletalBodySetup.h")
#include "PhysicsEngine/SkeletalBodySetup.h"
#endif
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/BoxElem.h"
#include "BodySetupEnums.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/BlendSpace.h"
#include "ReferenceSkeleton.h"
#include "LandscapeGrassType.h"
#include "Engine/SubsurfaceProfile.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhysicsAssetDumpBuilderEmitsTest,
    "PinWright.AssetDump.PhysicsAssetBuilderEmits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPhysicsAssetDumpBuilderEmitsTest::RunTest(const FString& Parameters)
{
    UPhysicsAsset* Asset = NewObject<UPhysicsAsset>(GetTransientPackage());
    TestNotNull(TEXT("Transient PhysicsAsset created"), Asset);
    if (!Asset)
    {
        return false;
    }

    // One body and one constraint, so both emission loops actually run.
    USkeletalBodySetup* BodySetup = NewObject<USkeletalBodySetup>(Asset, TEXT("PelvisBody"));
    BodySetup->BoneName = FName(TEXT("pelvis"));
    BodySetup->PhysicsType = PhysType_Kinematic;
    BodySetup->AggGeom.BoxElems.Add(FKBoxElem(10.0f, 20.0f, 30.0f));
    Asset->SkeletalBodySetups.Add(BodySetup);

    UPhysicsConstraintTemplate* ConstraintTemplate =
        NewObject<UPhysicsConstraintTemplate>(Asset, TEXT("PelvisSpineConstraint"));
    ConstraintTemplate->DefaultInstance.JointName = FName(TEXT("pelvis_spine"));
    ConstraintTemplate->DefaultInstance.ConstraintBone1 = FName(TEXT("spine_01"));
    ConstraintTemplate->DefaultInstance.ConstraintBone2 = FName(TEXT("pelvis"));
    Asset->ConstraintSetup.Add(ConstraintTemplate);

    TSharedPtr<FJsonObject> Json = PhysicsAssetDumpBuilder::BuildPhysicsAssetJson(Asset);
    TestTrue(TEXT("Result is valid for non-null asset"), Json.IsValid());
    if (!Json.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("previewMesh present"), Json->HasTypedField<EJson::String>(TEXT("previewMesh")));

    const TArray<TSharedPtr<FJsonValue>>* Bodies = nullptr;
    TestTrue(TEXT("bodies present"), Json->TryGetArrayField(TEXT("bodies"), Bodies));
    TestEqual(TEXT("one body emitted"), Bodies ? Bodies->Num() : 0, 1);

    const TSharedPtr<FJsonObject> BodyJson =
        JsonArrayFindObjectByStringField(Json, TEXT("bodies"), TEXT("bone"), TEXT("pelvis"));
    if (TestTrue(TEXT("the pelvis body is emitted by bone name"), BodyJson.IsValid()))
    {
        TestEqual(TEXT("body name emitted"),
            BodyJson->GetStringField(TEXT("name")), FString(TEXT("PelvisBody")));
        TestTrue(TEXT("body physicsType names the kinematic value"),
            BodyJson->GetStringField(TEXT("physicsType")).Contains(TEXT("Kinematic")));

        const TSharedPtr<FJsonObject>* Elements = nullptr;
        if (TestTrue(TEXT("body carries an elements object"),
                BodyJson->TryGetObjectField(TEXT("elements"), Elements) && Elements))
        {
            TestEqual(TEXT("elements.box counts the authored box"),
                static_cast<int32>((*Elements)->GetNumberField(TEXT("box"))), 1);
            TestEqual(TEXT("elements.sphere counts none"),
                static_cast<int32>((*Elements)->GetNumberField(TEXT("sphere"))), 0);
            TestEqual(TEXT("elements.convex counts none"),
                static_cast<int32>((*Elements)->GetNumberField(TEXT("convex"))), 0);
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Constraints = nullptr;
    TestTrue(TEXT("constraints present"), Json->TryGetArrayField(TEXT("constraints"), Constraints));
    TestEqual(TEXT("one constraint emitted"), Constraints ? Constraints->Num() : 0, 1);

    const TSharedPtr<FJsonObject> ConstraintJson = JsonArrayFindObjectByStringField(
        Json, TEXT("constraints"), TEXT("name"), TEXT("pelvis_spine"));
    if (TestTrue(TEXT("the constraint is emitted by joint name"), ConstraintJson.IsValid()))
    {
        // bone1/bone2 are directional: swapping them in the builder must fail the test.
        TestEqual(TEXT("constraint bone1 emitted"),
            ConstraintJson->GetStringField(TEXT("bone1")), FString(TEXT("spine_01")));
        TestEqual(TEXT("constraint bone2 emitted"),
            ConstraintJson->GetStringField(TEXT("bone2")), FString(TEXT("pelvis")));
        TestTrue(TEXT("constraint carries linearLimit"),
            ConstraintJson->HasTypedField<EJson::Object>(TEXT("linearLimit")));
        TestTrue(TEXT("constraint carries angularLimit"),
            ConstraintJson->HasTypedField<EJson::Object>(TEXT("angularLimit")));
        TestTrue(TEXT("constraint carries bBreakable"),
            ConstraintJson->HasTypedField<EJson::Boolean>(TEXT("bBreakable")));
    }

    TestFalse(TEXT("Null asset returns invalid pointer"),
        PhysicsAssetDumpBuilder::BuildPhysicsAssetJson(nullptr).IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonDumpBuilderEmitsTest,
    "PinWright.AssetDump.SkeletonBuilderEmits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSkeletonDumpBuilderEmitsTest::RunTest(const FString& Parameters)
{
    USkeleton* Asset = NewObject<USkeleton>(GetTransientPackage());
    TestNotNull(TEXT("Transient Skeleton created"), Asset);
    if (!Asset)
    {
        return false;
    }

    // Two bones so the bones[] walk over GetRawRefBoneInfo/GetRawRefBonePose runs.
    // (sockets[]/virtualBones[] delegate to SkeletonDumpBuilder::BuildSocketsJson /
    // BuildVirtualBonesJson, which are content-covered by
    // FSkeletalMeshDumpBuilderEmitsActiveSocketsAndVirtualBonesTest.)
    {
        FReferenceSkeletonModifier Modifier(Asset);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE),
            FTransform::Identity, true);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("pelvis")), TEXT("pelvis"), 0),
            FTransform(FVector(0.0, 0.0, 90.0)));
    }

    TSharedPtr<FJsonObject> Json = SkeletonDumpBuilder::BuildSkeletonJson(Asset);
    TestTrue(TEXT("Result is valid for non-null asset"), Json.IsValid());
    if (!Json.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
    TestTrue(TEXT("bones present"), Json->TryGetArrayField(TEXT("bones"), Bones));
    TestEqual(TEXT("both authored bones emitted"), Bones ? Bones->Num() : 0, 2);

    const TSharedPtr<FJsonObject> RootBone =
        JsonArrayFindObjectByStringField(Json, TEXT("bones"), TEXT("name"), TEXT("root"));
    if (TestTrue(TEXT("root bone emitted by name"), RootBone.IsValid()))
    {
        TestEqual(TEXT("root bone has no parent"),
            static_cast<int32>(RootBone->GetNumberField(TEXT("parentIndex"))), INDEX_NONE);
        TestTrue(TEXT("root bone carries refPose"),
            RootBone->HasTypedField<EJson::Object>(TEXT("refPose")));
    }

    const TSharedPtr<FJsonObject> PelvisBone =
        JsonArrayFindObjectByStringField(Json, TEXT("bones"), TEXT("name"), TEXT("pelvis"));
    if (TestTrue(TEXT("pelvis bone emitted by name"), PelvisBone.IsValid()))
    {
        // The hierarchy is the whole point of bones[]: a builder that dropped
        // parentIndex would flatten every skeleton in every dump. Assert the key exists
        // first — the expected value here is 0, which is also what a missing number field
        // reads back as.
        TestTrue(TEXT("pelvis bone carries parentIndex"),
            PelvisBone->HasTypedField<EJson::Number>(TEXT("parentIndex")));
        TestEqual(TEXT("pelvis bone parents to root (index 0)"),
            static_cast<int32>(PelvisBone->GetNumberField(TEXT("parentIndex"))), 0);
    }

    TestTrue(TEXT("virtualBones present"), Json->HasTypedField<EJson::Array>(TEXT("virtualBones")));
    TestTrue(TEXT("sockets present"),      Json->HasTypedField<EJson::Array>(TEXT("sockets")));
    TestTrue(TEXT("previewMesh present"),  Json->HasTypedField<EJson::String>(TEXT("previewMesh")));

    TestFalse(TEXT("Null skeleton returns invalid pointer"),
        SkeletonDumpBuilder::BuildSkeletonJson(nullptr).IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimMontageDumpBuilderEmitsTest,
    "PinWright.AssetDump.AnimMontageBuilderEmits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimMontageDumpBuilderEmitsTest::RunTest(const FString& Parameters)
{
    UAnimMontage* Asset = NewObject<UAnimMontage>(GetTransientPackage());
    TestNotNull(TEXT("Transient AnimMontage created"), Asset);
    if (!Asset)
    {
        return false;
    }

    // One named section and one named slot so both emission loops run. The section's
    // start time is left at its default: FAnimLinkableElement::LinkValue is protected and
    // setting it needs a linked montage segment, so startTime/linkValue remain
    // presence-only here (see the report note on TestNewTypeDumpBuilders).
    FCompositeSection& Section = Asset->CompositeSections.AddDefaulted_GetRef();
    Section.SectionName = FName(TEXT("Intro"));
    Section.NextSectionName = FName(TEXT("Loop"));

    FSlotAnimationTrack& Slot = Asset->SlotAnimTracks.AddDefaulted_GetRef();
    Slot.SlotName = FName(TEXT("PwTestSlot"));

    TSharedPtr<FJsonObject> Json = AnimMontageDumpBuilder::BuildAnimMontageJson(Asset);
    TestTrue(TEXT("Result is valid for non-null asset"), Json.IsValid());
    if (!Json.IsValid())
    {
        return false;
    }

    const TSharedPtr<FJsonObject> SectionJson =
        JsonArrayFindObjectByStringField(Json, TEXT("sections"), TEXT("sectionName"), TEXT("Intro"));
    if (TestTrue(TEXT("the authored section is emitted by name"), SectionJson.IsValid()))
    {
        // nextSectionName is the montage's flow graph — dropping it silently strips the
        // section ordering out of every montage dump.
        TestEqual(TEXT("section nextSectionName emitted"),
            SectionJson->GetStringField(TEXT("nextSectionName")), FString(TEXT("Loop")));
        TestTrue(TEXT("section carries startTime"),
            SectionJson->HasTypedField<EJson::Number>(TEXT("startTime")));
        TestTrue(TEXT("section carries linkValue"),
            SectionJson->HasTypedField<EJson::Number>(TEXT("linkValue")));
    }

    const TSharedPtr<FJsonObject> SlotJson =
        JsonArrayFindObjectByStringField(Json, TEXT("slots"), TEXT("slotName"), TEXT("PwTestSlot"));
    if (TestTrue(TEXT("the authored slot is emitted by name"), SlotJson.IsValid()))
    {
        TestTrue(TEXT("slot carries a segments array"),
            SlotJson->HasTypedField<EJson::Array>(TEXT("segments")));
    }

    TestTrue(TEXT("notifies present"), Json->HasTypedField<EJson::Array>(TEXT("notifies")));
    TestTrue(TEXT("blendIn present"),  Json->HasTypedField<EJson::Object>(TEXT("blendIn")));
    TestTrue(TEXT("blendOut present"), Json->HasTypedField<EJson::Object>(TEXT("blendOut")));
    TestTrue(TEXT("bEnableAutoBlendOut present"),
        Json->HasTypedField<EJson::Boolean>(TEXT("bEnableAutoBlendOut")));

    TestFalse(TEXT("Null montage returns invalid pointer"),
        AnimMontageDumpBuilder::BuildAnimMontageJson(nullptr).IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlendSpaceDumpBuilderEmitsTest,
    "PinWright.AssetDump.BlendSpaceBuilderEmits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlendSpaceDumpBuilderEmitsTest::RunTest(const FString& Parameters)
{
    UBlendSpace* Asset = NewObject<UBlendSpace>(GetTransientPackage());
    TestNotNull(TEXT("Transient BlendSpace created"), Asset);
    if (!Asset)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Json = BlendSpaceDumpBuilder::BuildBlendSpaceJson(Asset);
    TestTrue(TEXT("Result is valid for non-null asset"), Json.IsValid());
    if (!Json.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("class names the concrete blend-space class"),
        Json->GetStringField(TEXT("class")), FString(TEXT("BlendSpace")));

    // axes[] and interpolation[] are fixed-arity (3) walks over GetBlendParameter /
    // InterpolationParam, so they run on an unpopulated asset. Assert the arity and the
    // per-entry values against the accessors directly, not just field presence: an entry
    // emitted as {} satisfies HasTypedField<Array> but carries nothing.
    const TArray<TSharedPtr<FJsonValue>>* Axes = nullptr;
    TestTrue(TEXT("axes present"), Json->TryGetArrayField(TEXT("axes"), Axes));
    TestEqual(TEXT("axes has one entry per blend parameter"), Axes ? Axes->Num() : 0, 3);
    if (Axes && Axes->Num() == 3)
    {
        for (int32 i = 0; i < 3; ++i)
        {
            const TSharedPtr<FJsonObject> Axis = (*Axes)[i]->AsObject();
            if (!Axis.IsValid())
            {
                AddError(FString::Printf(TEXT("axes[%d] is not an object"), i));
                continue;
            }
            const FBlendParameter& Param = Asset->GetBlendParameter(i);
            TestEqual(FString::Printf(TEXT("axes[%d].displayName"), i),
                Axis->GetStringField(TEXT("displayName")), Param.DisplayName);
            TestEqual(FString::Printf(TEXT("axes[%d].min"), i),
                static_cast<float>(Axis->GetNumberField(TEXT("min"))), Param.Min);
            TestEqual(FString::Printf(TEXT("axes[%d].max"), i),
                static_cast<float>(Axis->GetNumberField(TEXT("max"))), Param.Max);
            TestEqual(FString::Printf(TEXT("axes[%d].gridNum"), i),
                static_cast<int32>(Axis->GetNumberField(TEXT("gridNum"))), Param.GridNum);
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Interpolation = nullptr;
    TestTrue(TEXT("interpolation present"),
        Json->TryGetArrayField(TEXT("interpolation"), Interpolation));
    TestEqual(TEXT("interpolation has one entry per axis"),
        Interpolation ? Interpolation->Num() : 0, 3);
    if (Interpolation && Interpolation->Num() == 3)
    {
        for (int32 i = 0; i < 3; ++i)
        {
            const TSharedPtr<FJsonObject> Entry = (*Interpolation)[i]->AsObject();
            if (!Entry.IsValid())
            {
                AddError(FString::Printf(TEXT("interpolation[%d] is not an object"), i));
                continue;
            }
            TestEqual(FString::Printf(TEXT("interpolation[%d].interpolationTime"), i),
                static_cast<float>(Entry->GetNumberField(TEXT("interpolationTime"))),
                Asset->InterpolationParam[i].InterpolationTime);
            TestFalse(FString::Printf(TEXT("interpolation[%d].interpolationType is named"), i),
                Entry->GetStringField(TEXT("interpolationType")).IsEmpty());
        }
    }

    // samples[] stays empty: UBlendSpace::AddSample validates against a bound skeleton and
    // a real UAnimSequence, which a transient fixture cannot supply. The sample loop is
    // therefore NOT covered here — see the report note on TestNewTypeDumpBuilders.
    TestTrue(TEXT("samples present"), Json->HasTypedField<EJson::Array>(TEXT("samples")));

    TestFalse(TEXT("Null blend space returns invalid pointer"),
        BlendSpaceDumpBuilder::BuildBlendSpaceJson(nullptr).IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeGrassTypeDumpBuilderEmitsTest,
    "PinWright.AssetDump.LandscapeGrassTypeBuilderEmits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeGrassTypeDumpBuilderEmitsTest::RunTest(const FString& Parameters)
{
    ULandscapeGrassType* Asset = NewObject<ULandscapeGrassType>(GetTransientPackage());
    TestNotNull(TEXT("Transient LandscapeGrassType created"), Asset);
    if (!Asset)
    {
        return false;
    }

    // One variety with every asserted field set away from its default, so the emission
    // loop runs and a dropped/misrouted field cannot coincide with the default.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    FGrassVariety& Variety = Asset->GrassVarieties.AddDefaulted_GetRef();
#else
    // FGrassVariety is declared without LANDSCAPE_API before UE 5.4, so its out-of-line default
    // constructor is not exported and AddDefaulted_GetRef leaves an unresolved external in this
    // DLL. AddZeroed is the same construction the production create path already uses
    // (LandscapeHandler.cpp), every field asserted below is written explicitly, and the one
    // default-dependent assertion - that `scaling` names an enum member - holds because 0 is
    // EGrassScaling's first enumerator.
    FGrassVariety& Variety = Asset->GrassVarieties[Asset->GrassVarieties.AddZeroed()];
#endif
    Variety.GrassDensity = 275.0f;
    Variety.PlacementJitter = 0.25f;
    Variety.StartCullDistance = 1500;
    Variety.EndCullDistance = 4500;
    Variety.MinLOD = 3;
    Variety.ScaleX = FFloatInterval(0.5f, 2.5f);
    Variety.RandomRotation = false;
    Variety.AlignToSurface = true;

    TSharedPtr<FJsonObject> Json = LandscapeGrassTypeDumpBuilder::BuildLandscapeGrassTypeJson(Asset);
    TestTrue(TEXT("Result is valid for non-null asset"), Json.IsValid());
    if (!Json.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Varieties = nullptr;
    TestTrue(TEXT("varieties present"), Json->TryGetArrayField(TEXT("varieties"), Varieties));
    TestEqual(TEXT("the authored variety is emitted"), Varieties ? Varieties->Num() : 0, 1);
    if (!Varieties || Varieties->Num() != 1)
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Entry = (*Varieties)[0]->AsObject();
    if (!TestTrue(TEXT("varieties[0] is an object"), Entry.IsValid()))
    {
        return false;
    }

    // The FPerPlatform* fields are the ones a naive builder gets wrong (emitting the
    // wrapper rather than .Default), so they are pinned by value.
    TestEqual(TEXT("grassDensity reads the per-platform default"),
        static_cast<float>(Entry->GetNumberField(TEXT("grassDensity"))), 275.0f);
    TestEqual(TEXT("startCullDistance reads the per-platform default"),
        static_cast<int32>(Entry->GetNumberField(TEXT("startCullDistance"))), 1500);
    TestEqual(TEXT("endCullDistance reads the per-platform default"),
        static_cast<int32>(Entry->GetNumberField(TEXT("endCullDistance"))), 4500);
    TestEqual(TEXT("placementJitter emitted"),
        static_cast<float>(Entry->GetNumberField(TEXT("placementJitter"))), 0.25f);
    TestEqual(TEXT("minLOD emitted"),
        static_cast<int32>(Entry->GetNumberField(TEXT("minLOD"))), 3);
    // A missing bool field reads back as false, so presence is asserted before the
    // expected-false check — otherwise dropping the field would pass.
    TestTrue(TEXT("randomRotation key present"),
        Entry->HasTypedField<EJson::Boolean>(TEXT("randomRotation")));
    TestFalse(TEXT("randomRotation emitted (set false, default true)"),
        Entry->GetBoolField(TEXT("randomRotation")));
    TestTrue(TEXT("alignToSurface emitted (set true, default false)"),
        Entry->GetBoolField(TEXT("alignToSurface")));
    TestFalse(TEXT("scaling names an enum member"),
        Entry->GetStringField(TEXT("scaling")).IsEmpty());
    TestTrue(TEXT("grassMesh key present"), Entry->HasTypedField<EJson::String>(TEXT("grassMesh")));

    const TSharedPtr<FJsonObject>* ScaleX = nullptr;
    if (TestTrue(TEXT("scaleX is an interval object"),
            Entry->TryGetObjectField(TEXT("scaleX"), ScaleX) && ScaleX))
    {
        // min/max are directional: a builder that swapped them would pass a
        // presence-only check.
        TestEqual(TEXT("scaleX.min"), static_cast<float>((*ScaleX)->GetNumberField(TEXT("min"))), 0.5f);
        TestEqual(TEXT("scaleX.max"), static_cast<float>((*ScaleX)->GetNumberField(TEXT("max"))), 2.5f);
    }
    TestTrue(TEXT("scaleY present"), Entry->HasTypedField<EJson::Object>(TEXT("scaleY")));
    TestTrue(TEXT("scaleZ present"), Entry->HasTypedField<EJson::Object>(TEXT("scaleZ")));

    const TSharedPtr<FJsonObject>* LightingChannels = nullptr;
    if (TestTrue(TEXT("lightingChannels emitted"),
            Entry->TryGetObjectField(TEXT("lightingChannels"), LightingChannels) && LightingChannels))
    {
        TestTrue(TEXT("lightingChannels carries bChannel0"),
            (*LightingChannels)->HasTypedField<EJson::Boolean>(TEXT("bChannel0")));
        TestTrue(TEXT("lightingChannels carries bChannel2"),
            (*LightingChannels)->HasTypedField<EJson::Boolean>(TEXT("bChannel2")));
    }

    TestFalse(TEXT("Null grass type returns invalid pointer"),
        LandscapeGrassTypeDumpBuilder::BuildLandscapeGrassTypeJson(nullptr).IsValid());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSubsurfaceProfileDumpBuilderEmitsTest,
    "PinWright.AssetDump.SubsurfaceProfileBuilderEmits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSubsurfaceProfileDumpBuilderEmitsTest::RunTest(const FString& Parameters)
{
    USubsurfaceProfile* Asset = NewObject<USubsurfaceProfile>(GetTransientPackage());
    TestNotNull(TEXT("Transient SubsurfaceProfile created"), Asset);
    if (!Asset)
    {
        return false;
    }

    // This builder is a flat 21-field property mirror with no loops, so presence of one
    // field proves nothing about the other twenty. Every scalar is set to a distinct
    // non-default value and read back, which also catches two fields wired to the same
    // source (the failure mode a per-field presence check cannot see).
    FSubsurfaceProfileStruct& S = Asset->Settings;
    S.SurfaceAlbedo = FLinearColor(0.11f, 0.22f, 0.33f, 1.0f);
    S.MeanFreePathDistance = 12.5f;
    S.WorldUnitScale = 0.375f;
    S.bEnableBurley = false;
    S.bEnableMeanFreePath = true;
    S.ScatterRadius = 3.25f;
    S.ExtinctionScale = 0.625f;
    S.NormalScale = 0.125f;
    S.ScatteringDistribution = 0.75f;
    S.IOR = 1.75f;
    S.Roughness0 = 0.4375f;
    S.Roughness1 = 0.8125f;
    S.LobeMix = 0.5625f;

    TSharedPtr<FJsonObject> Json = SubsurfaceProfileDumpBuilder::BuildSubsurfaceProfileJson(Asset);
    TestTrue(TEXT("Result is valid for non-null asset"), Json.IsValid());
    if (!Json.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("meanFreePathDistance"),
        static_cast<float>(Json->GetNumberField(TEXT("meanFreePathDistance"))), 12.5f);
    TestEqual(TEXT("worldUnitScale"),
        static_cast<float>(Json->GetNumberField(TEXT("worldUnitScale"))), 0.375f);
    TestEqual(TEXT("scatterRadius"),
        static_cast<float>(Json->GetNumberField(TEXT("scatterRadius"))), 3.25f);
    TestEqual(TEXT("extinctionScale"),
        static_cast<float>(Json->GetNumberField(TEXT("extinctionScale"))), 0.625f);
    TestEqual(TEXT("normalScale"),
        static_cast<float>(Json->GetNumberField(TEXT("normalScale"))), 0.125f);
    TestEqual(TEXT("scatteringDistribution"),
        static_cast<float>(Json->GetNumberField(TEXT("scatteringDistribution"))), 0.75f);
    TestEqual(TEXT("ior"), static_cast<float>(Json->GetNumberField(TEXT("ior"))), 1.75f);
    TestEqual(TEXT("roughness0"),
        static_cast<float>(Json->GetNumberField(TEXT("roughness0"))), 0.4375f);
    TestEqual(TEXT("roughness1"),
        static_cast<float>(Json->GetNumberField(TEXT("roughness1"))), 0.8125f);
    TestEqual(TEXT("lobeMix"), static_cast<float>(Json->GetNumberField(TEXT("lobeMix"))), 0.5625f);
    // Presence first: a missing bool field reads back as false, which is the value under
    // test here.
    TestTrue(TEXT("bEnableBurley key present"),
        Json->HasTypedField<EJson::Boolean>(TEXT("bEnableBurley")));
    TestFalse(TEXT("bEnableBurley reads the set value"), Json->GetBoolField(TEXT("bEnableBurley")));
    TestTrue(TEXT("bEnableMeanFreePath reads the set value"),
        Json->GetBoolField(TEXT("bEnableMeanFreePath")));

    const TSharedPtr<FJsonObject>* Albedo = nullptr;
    if (TestTrue(TEXT("surfaceAlbedo is a color object"),
            Json->TryGetObjectField(TEXT("surfaceAlbedo"), Albedo) && Albedo))
    {
        // Channel order is directional: r/b transposed would pass a presence check.
        TestEqual(TEXT("surfaceAlbedo.r"),
            static_cast<float>((*Albedo)->GetNumberField(TEXT("r"))), 0.11f);
        TestEqual(TEXT("surfaceAlbedo.g"),
            static_cast<float>((*Albedo)->GetNumberField(TEXT("g"))), 0.22f);
        TestEqual(TEXT("surfaceAlbedo.b"),
            static_cast<float>((*Albedo)->GetNumberField(TEXT("b"))), 0.33f);
    }

    // The remaining colour mirrors are shape-only: they carry engine defaults that a
    // transient fixture cannot make distinct without duplicating the struct here.
    TestTrue(TEXT("meanFreePathColor present"),
        Json->HasTypedField<EJson::Object>(TEXT("meanFreePathColor")));
    TestTrue(TEXT("tint present"), Json->HasTypedField<EJson::Object>(TEXT("tint")));
    TestTrue(TEXT("subsurfaceColor present"),
        Json->HasTypedField<EJson::Object>(TEXT("subsurfaceColor")));
    TestTrue(TEXT("falloffColor present"),
        Json->HasTypedField<EJson::Object>(TEXT("falloffColor")));
    TestTrue(TEXT("boundaryColorBleed present"),
        Json->HasTypedField<EJson::Object>(TEXT("boundaryColorBleed")));
    TestTrue(TEXT("transmissionTintColor present"),
        Json->HasTypedField<EJson::Object>(TEXT("transmissionTintColor")));
    TestTrue(TEXT("implementation present"),
        Json->HasTypedField<EJson::String>(TEXT("implementation")));

    TestFalse(TEXT("Null profile returns invalid pointer"),
        SubsurfaceProfileDumpBuilder::BuildSubsurfaceProfileJson(nullptr).IsValid());
    return true;
}
