// Copyright (c) 2026 Alexander Penkin. MIT License.

// animation.check_morph_curves on scratch fixtures: a copy of /Engine/EngineMeshes/SkeletalCube
// (on its own copied Skeleton) carrying four one-delta morphs, and a clip carrying four float
// curves that cover every flag/name combination:
//   Jaw   - morph on mesh, flagged on the Skeleton          -> drives, flagSource skeleton
//   Brow  - morph on mesh, flagged only on the MESH metadata -> drives, flagSource mesh
//   Blink - morph on mesh, NO flag anywhere                 -> does NOT drive (the name-only trap)
//   Ghost - flagged on the Skeleton, no such morph          -> does not drive, warning
//   Smile - morph with no curve                             -> morphsWithoutCurve

#include "Misc/AutomationTest.h"

#include "Animation/AnimCurveMetadata.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace PwCheckMorphCurvesTests
{
    const TCHAR* const Verb = TEXT("animation.check_morph_curves");
    const TCHAR* const EngineMesh = TEXT("/Engine/EngineMeshes/SkeletalCube");
    const TCHAR* const EngineSkeleton = TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton");

    void AddMorph(USkeletalMesh* Mesh, const TCHAR* Name)
    {
        UMorphTarget* Morph = NewObject<UMorphTarget>(Mesh, FName(Name));
        TArray<FMorphTargetDelta> Deltas;
        FMorphTargetDelta& Delta = Deltas.AddDefaulted_GetRef();
        Delta.SourceIdx = 0;
        Delta.PositionDelta = FVector3f(1.0f, 0.0f, 0.0f);
        TArray<FSkelMeshSection> NoSections;
        Morph->PopulateDeltas(Deltas, 0, NoSections, false, false);
        // No render-data invalidation: that path runs PostEditChange, whose editor rebuild
        // regenerates morphs from the imported model (SkeletalCube has none) and drops these.
        Mesh->RegisterMorphTarget(Morph, /*bInvalidateRenderData=*/false);
    }

    void SetFlag(UAnimCurveMetaData* MetaData, const TCHAR* Curve, bool bMorph)
    {
        MetaData->AddCurveMetaData(FName(Curve), /*bInTransact=*/false);
        MetaData->SetCurveMetaDataMorphTarget(FName(Curve), bMorph);
    }

    TArray<TSharedPtr<FJsonValue>> Paths(std::initializer_list<FString> Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FString& Value : Values)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    }

    TSharedPtr<FJsonObject> CurveRow(const TSharedPtr<FJsonObject>& AssetRow, const FString& Curve)
    {
        const TArray<TSharedPtr<FJsonValue>>* Curves = nullptr;
        if (AssetRow.IsValid() && AssetRow->TryGetArrayField(TEXT("curves"), Curves))
        {
            for (const TSharedPtr<FJsonValue>& Value : *Curves)
            {
                const TSharedPtr<FJsonObject> Row = Value->AsObject();
                if (Row.IsValid() && Row->GetStringField(TEXT("curve")) == Curve)
                {
                    return Row;
                }
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCheckMorphCurvesFlagAndNameTest,
    "PinWright.animation.check_morph_curves.FlagAndNameDecideDrives",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCheckMorphCurvesFlagAndNameTest::RunTest(const FString& Parameters)
{
    using namespace PwCheckMorphCurvesTests;

    if (!UEditorAssetLibrary::DoesAssetExist(EngineMesh))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("Skipped: /Engine/EngineMeshes/SkeletalCube unavailable."));
        return true;
    }

    const FString Folder = FString::Printf(TEXT("/Game/PinWrightTests/PWMorphCurves_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString SkeletonPath = Folder / TEXT("SK_Face");
    const FString MeshPath = Folder / TEXT("SKM_Face");
    const FString ClipPath = Folder / TEXT("A_Face");
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ClipPath);
        CleanupTestAsset(MeshPath);
        CleanupTestAsset(SkeletonPath);
    };

    USkeleton* Skeleton = Cast<USkeleton>(UEditorAssetLibrary::DuplicateAsset(EngineSkeleton, SkeletonPath));
    USkeletalMesh* Mesh = Cast<USkeletalMesh>(UEditorAssetLibrary::DuplicateAsset(EngineMesh, MeshPath));
    if (!TestNotNull(TEXT("skeleton copied"), Skeleton) || !TestNotNull(TEXT("mesh copied"), Mesh))
    {
        return false;
    }
    Mesh->SetSkeleton(Skeleton);

    for (const TCHAR* Morph : {TEXT("Jaw"), TEXT("Brow"), TEXT("Blink"), TEXT("Smile")})
    {
        AddMorph(Mesh, Morph);
    }
    Mesh->InitMorphTargets(); // rebuild the name->index map FindMorphTarget reads
    if (!TestEqual(TEXT("four fixture morphs registered"), Mesh->GetMorphTargets().Num(), 4))
    {
        return false;
    }

    // Clip with the four curves. Built before the metadata so the controller cannot reset flags.
    UPackage* ClipPackage = CreatePackage(*ClipPath);
    UAnimSequence* Clip = NewObject<UAnimSequence>(ClipPackage, TEXT("A_Face"), RF_Public | RF_Standalone);
    Clip->SetSkeleton(Skeleton);
    IAnimationDataController& Controller = Clip->GetController();
    Controller.InitializeModel();
    Controller.SetFrameRate(FFrameRate(30, 1));
    Controller.SetNumberOfFrames(FFrameNumber(10));
    for (const TCHAR* Curve : {TEXT("Jaw"), TEXT("Brow"), TEXT("Blink"), TEXT("Ghost")})
    {
        Controller.AddCurve(FAnimationCurveIdentifier(FName(Curve), ERawCurveTrackTypes::RCT_Float));
    }
    Controller.NotifyPopulated();
    FAssetRegistryModule::AssetCreated(Clip);

    // Skeleton metadata: Jaw and Ghost flagged, Blink present but unflagged.
    MCP_ADD_CURVE_META_DATA(Skeleton, FName(TEXT("Jaw")));
    UAnimCurveMetaData* SkeletonMeta = Skeleton->GetAssetUserData<UAnimCurveMetaData>();
    UAnimCurveMetaData* MeshMeta = Mesh->GetAssetUserData<UAnimCurveMetaData>();
    if (!MeshMeta)
    {
        MeshMeta = NewObject<UAnimCurveMetaData>(Mesh);
        Mesh->AddAssetUserData(MeshMeta);
    }
    if (!TestNotNull(TEXT("skeleton curve metadata"), SkeletonMeta))
    {
        return false;
    }
    SetFlag(SkeletonMeta, TEXT("Jaw"), true);
    SetFlag(SkeletonMeta, TEXT("Ghost"), true);
    SetFlag(SkeletonMeta, TEXT("Blink"), false);
    SetFlag(SkeletonMeta, TEXT("Brow"), false);
    // Mesh metadata flags Brow only.
    SetFlag(MeshMeta, TEXT("Brow"), true);

    const bool bMeshDirty = Mesh->GetPackage()->IsDirty();
    const bool bSkeletonDirty = Skeleton->GetPackage()->IsDirty();
    const bool bClipDirty = ClipPackage->IsDirty();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("assetPaths"), Paths({ClipPath}));
    Payload->SetStringField(TEXT("skeletalMeshPath"), MeshPath);
    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"), InvokeHandlerWithCapture(Verb, Payload, Capture));
    if (!TestTrue(FString::Printf(TEXT("call succeeds (%s %s)"), *Capture.ErrorCode, *Capture.Message), Capture.bSuccess)
        || !Capture.Result.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>& Assets = Capture.Result->GetArrayField(TEXT("assets"));
    if (!TestEqual(TEXT("one asset row"), Assets.Num(), 1))
    {
        return false;
    }
    const TSharedPtr<FJsonObject> Row = Assets[0]->AsObject();

    struct FExpect { const TCHAR* Curve; bool bOnMesh; const TCHAR* Source; bool bDrives; };
    for (const FExpect& Expect : {
        FExpect{TEXT("Jaw"), true, TEXT("skeleton"), true},
        FExpect{TEXT("Brow"), true, TEXT("mesh"), true},
        FExpect{TEXT("Blink"), true, TEXT("none"), false},
        FExpect{TEXT("Ghost"), false, TEXT("skeleton"), false}})
    {
        const TSharedPtr<FJsonObject> Curve = CurveRow(Row, Expect.Curve);
        if (!TestNotNull(*FString::Printf(TEXT("row for %s"), Expect.Curve), Curve.Get()))
        {
            continue;
        }
        TestEqual(*FString::Printf(TEXT("%s morphOnMesh"), Expect.Curve), Curve->GetBoolField(TEXT("morphOnMesh")), Expect.bOnMesh);
        TestEqual(*FString::Printf(TEXT("%s flagSource"), Expect.Curve), Curve->GetStringField(TEXT("flagSource")), FString(Expect.Source));
        TestEqual(*FString::Printf(TEXT("%s drives"), Expect.Curve), Curve->GetBoolField(TEXT("drives")), Expect.bDrives);
    }

    TArray<FString> Undriven;
    Row->TryGetStringArrayField(TEXT("morphsWithoutCurve"), Undriven);
    TestEqual(TEXT("morphsWithoutCurve lists the unflagged and the curveless morph"),
        FString::Join(Undriven, TEXT(",")), FString(TEXT("Blink,Smile")));

    const TSharedPtr<FJsonObject> Summary = Capture.Result->GetObjectField(TEXT("summary"));
    TestEqual(TEXT("one error (Blink)"), static_cast<int32>(Summary->GetIntegerField(TEXT("errors"))), 1);
    TestEqual(TEXT("one warning (Ghost)"), static_cast<int32>(Summary->GetIntegerField(TEXT("warnings"))), 1);
    TestFalse(TEXT("an unflagged matching curve fails the default failOn"), Capture.Result->GetBoolField(TEXT("pass")));

    TestEqual(TEXT("mesh package dirty flag unchanged"), Mesh->GetPackage()->IsDirty(), bMeshDirty);
    TestEqual(TEXT("skeleton package dirty flag unchanged"), Skeleton->GetPackage()->IsDirty(), bSkeletonDirty);
    TestEqual(TEXT("clip package dirty flag unchanged"), ClipPackage->IsDirty(), bClipDirty);

    // failOn:none clears the severity bar, but an unmeasurable asset still fails pass.
    Payload->SetStringField(TEXT("failOn"), TEXT("none"));
    InvokeHandlerWithCapture(Verb, Payload, Capture);
    TestTrue(TEXT("failOn none passes a fully measured sweep"), Capture.bSuccess && Capture.Result->GetBoolField(TEXT("pass")));

    Payload->SetArrayField(TEXT("assetPaths"), Paths({ClipPath, Folder / TEXT("A_Missing")}));
    InvokeHandlerWithCapture(Verb, Payload, Capture);
    if (TestTrue(TEXT("missing asset is a finding, not an RPC error"), Capture.bSuccess))
    {
        TestEqual(TEXT("missing asset is unrunnable"),
            static_cast<int32>(Capture.Result->GetObjectField(TEXT("summary"))->GetIntegerField(TEXT("unrunnable"))), 1);
        TestFalse(TEXT("unrunnable fails pass even under failOn none"), Capture.Result->GetBoolField(TEXT("pass")));
    }
    return true;
}
