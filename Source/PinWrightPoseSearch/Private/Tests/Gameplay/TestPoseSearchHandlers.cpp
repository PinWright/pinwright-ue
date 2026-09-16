// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/HandlerContext.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#if __has_include("PoseSearch/PoseSearchSchema.h") && __has_include("PoseSearch/PoseSearchDatabase.h") && __has_include("PoseSearch/PoseSearchFeatureChannel_Position.h")
#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchFeatureChannel_Position.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#define MCP_TEST_HAS_POSESEARCH 1
#else
#define MCP_TEST_HAS_POSESEARCH 0
#endif

#if MCP_TEST_HAS_POSESEARCH

namespace
{
FString MakePoseSearchTestPackagePath(const TCHAR* Prefix)
{
    return FString::Printf(
        TEXT("/Game/PinWrightTests/%s_%s"),
        Prefix,
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

static FString ToPoseSearchTestObjectPath(const FString& PackagePath)
{
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    return FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
}

USkeleton* CreatePoseSearchTestSkeleton(const FString& PackagePath)
{
    UPackage* Package = CreatePackage(*PackagePath);
    USkeleton* Skeleton = NewObject<USkeleton>(
        Package,
        *FPackageName::GetLongPackageAssetName(PackagePath),
        RF_Public | RF_Standalone | RF_Transient);
    if (!Skeleton)
    {
        return nullptr;
    }

    {
        FReferenceSkeletonModifier Modifier(Skeleton);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity);
    }

    Skeleton->AddToRoot();
    FAssetRegistryModule::AssetCreated(Skeleton);
    return Skeleton;
}

UAnimSequence* CreatePoseSearchTestSequence(const FString& PackagePath, USkeleton* Skeleton)
{
    UPackage* Package = CreatePackage(*PackagePath);
    UAnimSequence* Sequence = NewObject<UAnimSequence>(
        Package,
        *FPackageName::GetLongPackageAssetName(PackagePath),
        RF_Public | RF_Standalone | RF_Transient);
    if (!Sequence)
    {
        return nullptr;
    }

    Sequence->SetSkeleton(Skeleton);

    // Author one keyed "root" bone track so the sequence has real, hashable compressed data.
    // A pose_search database bound to this sequence gets an async index build scheduled off its
    // edit-modification delegates (FAsyncPoseSearchDatabasesManagement, editor-tickable). That
    // build samples every bound animation and hard-check()s that a non-cooked UAnimSequence has a
    // non-zero PlatformHash — i.e. built compressed data (PoseSearchAssetSampler.cpp:438). A bare
    // NewObject sequence has no data model ("No Movie Scene found for SequencerDataModel"), so its
    // hash is Zero and the background worker fatally asserts mid-suite (the full-suite run runs long
    // enough for the queued task to fire). Closing the controller bracket triggers the synchronous
    // DDC recompression that gives the sequence a valid hash, making it a samplable fixture.
    // (5.5+ only: on 5.3/5.4 this recompression path itself asserts on a minimal transient skeleton.)
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    {
        IAnimationDataController& Controller = Sequence->GetController();
        Controller.OpenBracket(FText::FromString(TEXT("PinWright PoseSearch Fixture")), /*bShouldTransact=*/false);
        Controller.InitializeModel();
        Controller.SetNumberOfFrames(FFrameNumber(30), /*bShouldTransact=*/false);
        Controller.AddBoneCurve(FName(TEXT("root")), /*bShouldTransact=*/false);
        Controller.SetBoneTrackKeys(FName(TEXT("root")), { FVector::ZeroVector }, { FQuat::Identity }, { FVector::OneVector }, /*bShouldTransact=*/false);
        Controller.CloseBracket(/*bShouldTransact=*/false);
    }
#endif

    Sequence->AddToRoot();
    FAssetRegistryModule::AssetCreated(Sequence);
    return Sequence;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseSearchSchemaDatabaseAuthoringPipelineTest,
    "PinWright.pose_search.SchemaDatabaseAuthoringPipeline",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseSearchSchemaDatabaseAuthoringPipelineTest::RunTest(const FString& Parameters)
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("PoseSearch")))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
            TEXT("PoseSearch module is not loaded; skipping gated Pose Search authoring pipeline test."));
        return true;
    }

    const FString SkeletonPackagePath = MakePoseSearchTestPackagePath(TEXT("SK_PoseSearch"));
    const FString SequencePackagePath = MakePoseSearchTestPackagePath(TEXT("AS_PoseSearch"));
    const FString OtherSkeletonPackagePath = MakePoseSearchTestPackagePath(TEXT("SK_PoseSearchOther"));
    const FString OtherSequencePackagePath = MakePoseSearchTestPackagePath(TEXT("AS_PoseSearchOther"));
    const FString SchemaPackagePath = MakePoseSearchTestPackagePath(TEXT("PSSchema"));
    const FString DatabasePackagePath = MakePoseSearchTestPackagePath(TEXT("PSDatabase"));
    const FString InvalidDatabasePackagePath = MakePoseSearchTestPackagePath(TEXT("PSDatabaseInvalid"));

    USkeleton* Skeleton = CreatePoseSearchTestSkeleton(SkeletonPackagePath);
    UAnimSequence* Sequence = CreatePoseSearchTestSequence(SequencePackagePath, Skeleton);
    USkeleton* OtherSkeleton = CreatePoseSearchTestSkeleton(OtherSkeletonPackagePath);
    UAnimSequence* OtherSequence = CreatePoseSearchTestSequence(OtherSequencePackagePath, OtherSkeleton);
    ON_SCOPE_EXIT
    {
        if (OtherSequence)
        {
            OtherSequence->RemoveFromRoot();
        }
        if (OtherSkeleton)
        {
            OtherSkeleton->RemoveFromRoot();
        }
        if (Sequence)
        {
            Sequence->RemoveFromRoot();
        }
        if (Skeleton)
        {
            Skeleton->RemoveFromRoot();
        }
        CleanupTestAsset(InvalidDatabasePackagePath);
        CleanupTestAsset(DatabasePackagePath);
        CleanupTestAsset(SchemaPackagePath);
        CleanupTestAsset(OtherSequencePackagePath);
        CleanupTestAsset(OtherSkeletonPackagePath);
        CleanupTestAsset(SequencePackagePath);
        CleanupTestAsset(SkeletonPackagePath);
    };

    TestNotNull(TEXT("test skeleton created"), Skeleton);
    TestNotNull(TEXT("test sequence created"), Sequence);
    TestNotNull(TEXT("other test skeleton created"), OtherSkeleton);
    TestNotNull(TEXT("other test sequence created"), OtherSequence);
    if (!Skeleton || !Sequence || !OtherSkeleton || !OtherSequence)
    {
        return false;
    }

    TSharedPtr<FJsonObject> PositionChannel = MakeShared<FJsonObject>();
    PositionChannel->SetStringField(TEXT("type"), TEXT("Position"));
    PositionChannel->SetStringField(TEXT("bone"), TEXT("root"));

    TArray<TSharedPtr<FJsonValue>> Channels;
    Channels.Add(MakeShared<FJsonValueObject>(PositionChannel));

    TSharedPtr<FJsonObject> CreateSchemaPayload = MakeShared<FJsonObject>();
    CreateSchemaPayload->SetStringField(TEXT("assetPath"), SchemaPackagePath);
    CreateSchemaPayload->SetStringField(TEXT("skeleton"), ToPoseSearchTestObjectPath(SkeletonPackagePath));
    CreateSchemaPayload->SetArrayField(TEXT("channels"), Channels);
    CreateSchemaPayload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("pose_search.create_schema handler found"),
        InvokeHandlerWithCapture(TEXT("pose_search.create_schema"), CreateSchemaPayload, Capture));
    TestTrue(TEXT("pose_search.create_schema succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("create_schema failed: %s %s"), *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    UPoseSearchSchema* Schema = LoadObject<UPoseSearchSchema>(nullptr, *ToPoseSearchTestObjectPath(SchemaPackagePath));
    TestNotNull(TEXT("schema asset created"), Schema);
    if (!Schema)
    {
        return false;
    }
    TestEqual(TEXT("schema has one skeleton"), Schema->GetRoledSkeletons().Num(), 1);
    TestEqual(TEXT("schema skeleton matches"), Schema->GetRoledSkeletons()[0].Skeleton.Get(), Skeleton);
    TestEqual(TEXT("schema has one finalized channel"), Schema->GetChannels().Num(), 1);
    TestNotNull(TEXT("schema channel is Position"),
        Cast<UPoseSearchFeatureChannel_Position>(Schema->GetChannels()[0].Get()));

    TSharedPtr<FJsonObject> InvalidAnimation = MakeShared<FJsonObject>();
    InvalidAnimation->SetStringField(TEXT("sequencePath"), ToPoseSearchTestObjectPath(OtherSequencePackagePath));

    TArray<TSharedPtr<FJsonValue>> InvalidAnimations;
    InvalidAnimations.Add(MakeShared<FJsonValueObject>(InvalidAnimation));

    TSharedPtr<FJsonObject> InvalidCreateDatabasePayload = MakeShared<FJsonObject>();
    InvalidCreateDatabasePayload->SetStringField(TEXT("assetPath"), InvalidDatabasePackagePath);
    InvalidCreateDatabasePayload->SetStringField(TEXT("schema"), ToPoseSearchTestObjectPath(SchemaPackagePath));
    InvalidCreateDatabasePayload->SetArrayField(TEXT("animations"), InvalidAnimations);
    InvalidCreateDatabasePayload->SetBoolField(TEXT("save"), false);

    const bool bInvalidCreateDatabaseHandlerFound = InvokeHandlerWithCapture(
        TEXT("pose_search.create_database"), InvalidCreateDatabasePayload, Capture);
    TestTrue(TEXT("pose_search.create_database invalid animation handler found"), bInvalidCreateDatabaseHandlerFound);
    TestFalse(TEXT("create_database rejects mismatched supplied animation"), Capture.bSuccess);
    TestEqual(TEXT("create_database mismatched animation error code"), Capture.ErrorCode, FString(TEXT("SKELETON_MISMATCH")));
    TestNull(TEXT("invalid create_database leaves no database object"),
        FindObject<UPoseSearchDatabase>(nullptr, *ToPoseSearchTestObjectPath(InvalidDatabasePackagePath)));
    if (!bInvalidCreateDatabaseHandlerFound || Capture.bSuccess)
    {
        return false;
    }

    TSharedPtr<FJsonObject> CreateDatabasePayload = MakeShared<FJsonObject>();
    CreateDatabasePayload->SetStringField(TEXT("assetPath"), DatabasePackagePath);
    CreateDatabasePayload->SetStringField(TEXT("schema"), ToPoseSearchTestObjectPath(SchemaPackagePath));
    CreateDatabasePayload->SetBoolField(TEXT("save"), false);

    TestTrue(TEXT("pose_search.create_database handler found"),
        InvokeHandlerWithCapture(TEXT("pose_search.create_database"), CreateDatabasePayload, Capture));
    TestTrue(TEXT("pose_search.create_database succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("create_database failed: %s %s"), *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    UPoseSearchDatabase* Database = LoadObject<UPoseSearchDatabase>(nullptr, *ToPoseSearchTestObjectPath(DatabasePackagePath));
    TestNotNull(TEXT("database asset created"), Database);
    if (!Database)
    {
        return false;
    }
    TestTrue(TEXT("database schema matches"), Database->Schema.Get() == Schema);
    TestEqual(TEXT("database starts empty"), Database->GetNumAnimationAssets(), 0);

    TSharedPtr<FJsonObject> MismatchedAnimationPayload = MakeShared<FJsonObject>();
    MismatchedAnimationPayload->SetStringField(TEXT("assetPath"), ToPoseSearchTestObjectPath(DatabasePackagePath));
    MismatchedAnimationPayload->SetStringField(TEXT("sequencePath"), ToPoseSearchTestObjectPath(OtherSequencePackagePath));
    MismatchedAnimationPayload->SetBoolField(TEXT("save"), false);

    const bool bMismatchHandlerFound = InvokeHandlerWithCapture(
        TEXT("pose_search.add_database_animation"), MismatchedAnimationPayload, Capture);
    TestTrue(TEXT("pose_search.add_database_animation mismatch handler found"), bMismatchHandlerFound);
    TestFalse(TEXT("mismatched sequence rejected"), Capture.bSuccess);
    TestEqual(TEXT("mismatched sequence error code"), Capture.ErrorCode, FString(TEXT("SKELETON_MISMATCH")));
    TestEqual(TEXT("database remains empty after mismatch"), Database->GetNumAnimationAssets(), 0);
    if (!bMismatchHandlerFound || Capture.bSuccess)
    {
        return false;
    }

    TSharedPtr<FJsonObject> SamplingRange = MakeShared<FJsonObject>();
    SamplingRange->SetNumberField(TEXT("min"), 0.125);
    SamplingRange->SetNumberField(TEXT("max"), 0.75);

    TSharedPtr<FJsonObject> AddAnimationPayload = MakeShared<FJsonObject>();
    AddAnimationPayload->SetStringField(TEXT("assetPath"), ToPoseSearchTestObjectPath(DatabasePackagePath));
    AddAnimationPayload->SetStringField(TEXT("sequencePath"), ToPoseSearchTestObjectPath(SequencePackagePath));
    AddAnimationPayload->SetObjectField(TEXT("samplingRange"), SamplingRange);
    AddAnimationPayload->SetBoolField(TEXT("save"), false);

    TestTrue(TEXT("pose_search.add_database_animation handler found"),
        InvokeHandlerWithCapture(TEXT("pose_search.add_database_animation"), AddAnimationPayload, Capture));
    TestTrue(TEXT("pose_search.add_database_animation succeeded"), Capture.bSuccess);
    if (!Capture.bSuccess)
    {
        AddError(FString::Printf(TEXT("add_database_animation failed: %s %s"), *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    TestEqual(TEXT("database has one animation"), Database->GetNumAnimationAssets(), 1);
    TestTrue(TEXT("database sequence asset matches"), Database->GetAnimationAsset(0) == Sequence);

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    const FPoseSearchDatabaseAnimationAsset* Entry = Database->GetDatabaseAnimationAsset(0);
#else
    const FPoseSearchDatabaseSequence* Entry = Database->GetDatabaseAnimationAsset<FPoseSearchDatabaseSequence>(0);
#endif
    TestNotNull(TEXT("database entry is a sequence"), Entry);
    if (!Entry)
    {
        return false;
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    TestTrue(TEXT("sequence entry asset matches"), Entry->AnimAsset.Get() == Sequence);
#else
    TestEqual(TEXT("sequence entry asset matches"), Entry->Sequence.Get(), Sequence);
#endif
    TestEqual(TEXT("sampling range min matches"), Entry->SamplingRange.Min, 0.125f);
    TestEqual(TEXT("sampling range max matches"), Entry->SamplingRange.Max, 0.75f);

    return true;
}

#endif // MCP_TEST_HAS_POSESEARCH
