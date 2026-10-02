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

// Same gate as PoseSearchHandler.cpp: schema + database headers only, because the concrete
// channel headers are Private before UE 5.6 (B-pose-search-gate-private-header).
#if __has_include("PoseSearch/PoseSearchSchema.h") && __has_include("PoseSearch/PoseSearchDatabase.h")
#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BoneContainer.h"
#include "Modules/ModuleManager.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchFeatureChannel.h"
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
    TestEqual(TEXT("schema channel is Position"),
        GetNameSafe(Schema->GetChannels()[0] ? Schema->GetChannels()[0]->GetClass() : nullptr),
        FString(TEXT("PoseSearchFeatureChannel_Position")));

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

namespace PoseSearchChannelKindsTestHelpers
{
// Channels are read by reflection so the test stays portable to engines whose concrete channel
// headers are Private (UE 5.3-5.5).
const UPoseSearchFeatureChannel* FindChannelOfClass(const UPoseSearchSchema* Schema, const TCHAR* ClassName)
{
    for (const TObjectPtr<UPoseSearchFeatureChannel>& Channel : Schema->GetChannels())
    {
        if (Channel && Channel->GetClass()->GetName() == ClassName)
        {
            return Channel.Get();
        }
    }
    return nullptr;
}

float ReadFloat(const void* Container, const UStruct* Struct, const TCHAR* PropertyName)
{
    const FFloatProperty* Property = FindFProperty<FFloatProperty>(Struct, PropertyName);
    return Property ? Property->GetPropertyValue_InContainer(Container) : -999.f;
}

FName ReadBoneName(const UObject* Channel, const TCHAR* PropertyName)
{
    const FStructProperty* Property = FindFProperty<FStructProperty>(Channel->GetClass(), PropertyName);
    return Property ? Property->ContainerPtrToValuePtr<FBoneReference>(Channel)->BoneName : NAME_None;
}

TSharedPtr<FJsonObject> MakeChannel(const TCHAR* Kind)
{
    TSharedPtr<FJsonObject> Channel = MakeShared<FJsonObject>();
    Channel->SetStringField(TEXT("type"), Kind);
    return Channel;
}

TSharedPtr<FJsonObject> MakeSchemaPayload(const FString& SchemaPackagePath, const FString& SkeletonObjectPath,
    const TArray<TSharedPtr<FJsonObject>>& ChannelSpecs)
{
    TArray<TSharedPtr<FJsonValue>> Channels;
    for (const TSharedPtr<FJsonObject>& Spec : ChannelSpecs)
    {
        Channels.Add(MakeShared<FJsonValueObject>(Spec));
    }
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), SchemaPackagePath);
    Payload->SetStringField(TEXT("skeleton"), SkeletonObjectPath);
    Payload->SetArrayField(TEXT("channels"), Channels);
    Payload->SetBoolField(TEXT("save"), false);
    return Payload;
}
} // namespace PoseSearchChannelKindsTestHelpers

// F-pose-search-schema-channel-kinds: create_schema builds Trajectory, Velocity, Heading and Pose
// channels (not only Position) with their settings applied, echoes each channel, and refuses an
// unknown kind or setting before any schema asset exists.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPoseSearchSchemaChannelKindsTest,
    "PinWright.pose_search.CreateSchemaChannelKinds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPoseSearchSchemaChannelKindsTest::RunTest(const FString& Parameters)
{
    using namespace PoseSearchChannelKindsTestHelpers;

    if (!FModuleManager::Get().IsModuleLoaded(TEXT("PoseSearch")))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("optional-plugin-not-shipped"),
            TEXT("PoseSearch module is not loaded; skipping gated Pose Search channel-kinds test."));
        return true;
    }

    const FString SkeletonPackagePath = MakePoseSearchTestPackagePath(TEXT("SK_PoseSearchKinds"));
    const FString SchemaPackagePath = MakePoseSearchTestPackagePath(TEXT("PSSchemaKinds"));
    const FString BadKindSchemaPackagePath = MakePoseSearchTestPackagePath(TEXT("PSSchemaBadKind"));
    const FString BadSettingSchemaPackagePath = MakePoseSearchTestPackagePath(TEXT("PSSchemaBadSetting"));

    USkeleton* Skeleton = CreatePoseSearchTestSkeleton(SkeletonPackagePath);
    ON_SCOPE_EXIT
    {
        if (Skeleton)
        {
            Skeleton->RemoveFromRoot();
        }
        CleanupTestAsset(BadSettingSchemaPackagePath);
        CleanupTestAsset(BadKindSchemaPackagePath);
        CleanupTestAsset(SchemaPackagePath);
        CleanupTestAsset(SkeletonPackagePath);
    };
    if (!TestNotNull(TEXT("test skeleton created"), Skeleton))
    {
        return false;
    }
    const FString SkeletonObjectPath = ToPoseSearchTestObjectPath(SkeletonPackagePath);

    // Trajectory with explicit samples (array of structs).
    TSharedPtr<FJsonObject> Trajectory = MakeChannel(TEXT("Trajectory"));
    {
        TArray<TSharedPtr<FJsonValue>> Samples;
        for (const double Offset : {-0.5, 0.5})
        {
            TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
            Sample->SetNumberField(TEXT("offset"), Offset);
            Sample->SetNumberField(TEXT("flags"), 32); // EPoseSearchTrajectoryFlags::PositionXY
            Samples.Add(MakeShared<FJsonValueObject>(Sample));
        }
        Trajectory->SetArrayField(TEXT("samples"), Samples);
    }

    // Velocity via the "kind" key and a lower-case kind.
    TSharedPtr<FJsonObject> Velocity = MakeShared<FJsonObject>();
    Velocity->SetStringField(TEXT("kind"), TEXT("velocity"));
    Velocity->SetStringField(TEXT("bone"), TEXT("root"));
    Velocity->SetNumberField(TEXT("weight"), 2.5);

    TSharedPtr<FJsonObject> Heading = MakeChannel(TEXT("Heading"));
    Heading->SetStringField(TEXT("bone"), TEXT("root"));
    Heading->SetStringField(TEXT("headingAxis"), TEXT("Y"));

    // Position keeps the legacy boneName alias.
    TSharedPtr<FJsonObject> Position = MakeChannel(TEXT("Position"));
    Position->SetStringField(TEXT("boneName"), TEXT("root"));
    Position->SetNumberField(TEXT("sampleTimeOffset"), 0.25);

    TSharedPtr<FJsonObject> Pose = MakeChannel(TEXT("Pose"));
    {
        TSharedPtr<FJsonObject> Reference = MakeShared<FJsonObject>();
        Reference->SetStringField(TEXT("boneName"), TEXT("root"));
        TSharedPtr<FJsonObject> SampledBone = MakeShared<FJsonObject>();
        SampledBone->SetObjectField(TEXT("reference"), Reference);
        SampledBone->SetNumberField(TEXT("flags"), 2); // EPoseSearchBoneFlags::Position
        Pose->SetArrayField(TEXT("sampledBones"), { MakeShared<FJsonValueObject>(SampledBone) });
    }

    FTestResponseCapture Capture;
    TestTrue(TEXT("create_schema handler found"), InvokeHandlerWithCapture(TEXT("pose_search.create_schema"),
        MakeSchemaPayload(SchemaPackagePath, SkeletonObjectPath, {Trajectory, Velocity, Heading, Position, Pose}), Capture));
    if (!TestTrue(TEXT("create_schema with Trajectory/Velocity/Heading/Position/Pose succeeded"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("create_schema failed: %s %s"), *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    // Response echoes each created channel with its resolved settings.
    const TArray<TSharedPtr<FJsonValue>>* Echo = nullptr;
    if (TestTrue(TEXT("response has channels[]"), Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("channels"), Echo)) && Echo)
    {
        const TCHAR* ExpectedKinds[] = {TEXT("Trajectory"), TEXT("Velocity"), TEXT("Heading"), TEXT("Position"), TEXT("Pose")};
        if (TestEqual(TEXT("one echo per requested channel"), Echo->Num(), static_cast<int32>(UE_ARRAY_COUNT(ExpectedKinds))))
        {
            for (int32 Index = 0; Index < Echo->Num(); ++Index)
            {
                const TSharedPtr<FJsonObject> Entry = (*Echo)[Index]->AsObject();
                TestEqual(FString::Printf(TEXT("channels[%d].kind"), Index), Entry->GetStringField(TEXT("kind")), FString(ExpectedKinds[Index]));
                TestEqual(FString::Printf(TEXT("channels[%d].className"), Index), Entry->GetStringField(TEXT("className")),
                    FString(TEXT("PoseSearchFeatureChannel_")) + ExpectedKinds[Index]);
            }
            const TSharedPtr<FJsonObject>* HeadingSettings = nullptr;
            if (TestTrue(TEXT("heading echo has settings"), (*Echo)[2]->AsObject()->TryGetObjectField(TEXT("settings"), HeadingSettings)))
            {
                TestEqual(TEXT("heading echo resolves headingAxis"), (*HeadingSettings)->GetStringField(TEXT("headingAxis")), FString(TEXT("Y")));
            }
        }
    }

    UPoseSearchSchema* Schema = LoadObject<UPoseSearchSchema>(nullptr, *ToPoseSearchTestObjectPath(SchemaPackagePath));
    if (!TestNotNull(TEXT("schema asset created"), Schema))
    {
        return false;
    }

    const UPoseSearchFeatureChannel* TrajectoryChannel = FindChannelOfClass(Schema, TEXT("PoseSearchFeatureChannel_Trajectory"));
    const UPoseSearchFeatureChannel* VelocityChannel = FindChannelOfClass(Schema, TEXT("PoseSearchFeatureChannel_Velocity"));
    const UPoseSearchFeatureChannel* HeadingChannel = FindChannelOfClass(Schema, TEXT("PoseSearchFeatureChannel_Heading"));
    const UPoseSearchFeatureChannel* PositionChannel = FindChannelOfClass(Schema, TEXT("PoseSearchFeatureChannel_Position"));
    const UPoseSearchFeatureChannel* PoseChannel = FindChannelOfClass(Schema, TEXT("PoseSearchFeatureChannel_Pose"));
    TestNotNull(TEXT("schema finalized a Trajectory channel"), TrajectoryChannel);
    TestNotNull(TEXT("schema finalized a Velocity channel"), VelocityChannel);
    TestNotNull(TEXT("schema finalized a Heading channel"), HeadingChannel);
    TestNotNull(TEXT("schema finalized a Position channel"), PositionChannel);
    TestNotNull(TEXT("schema finalized a Pose channel"), PoseChannel);

    if (TrajectoryChannel)
    {
        TestTrue(TEXT("trajectory channel is owned by the schema"), TrajectoryChannel->GetOuter() == Schema);
        const FArrayProperty* SamplesProperty = FindFProperty<FArrayProperty>(TrajectoryChannel->GetClass(), TEXT("Samples"));
        if (TestNotNull(TEXT("trajectory has Samples"), SamplesProperty))
        {
            FScriptArrayHelper Samples(SamplesProperty, SamplesProperty->ContainerPtrToValuePtr<void>(TrajectoryChannel));
            if (TestEqual(TEXT("trajectory samples replaced by the spec"), Samples.Num(), 2))
            {
                const UStruct* SampleStruct = CastFieldChecked<FStructProperty>(SamplesProperty->Inner)->Struct;
                TestEqual(TEXT("trajectory sample[0].Offset"), ReadFloat(Samples.GetRawPtr(0), SampleStruct, TEXT("Offset")), -0.5f);
                TestEqual(TEXT("trajectory sample[1].Offset"), ReadFloat(Samples.GetRawPtr(1), SampleStruct, TEXT("Offset")), 0.5f);
            }
        }
    }
    if (VelocityChannel)
    {
        TestEqual(TEXT("velocity bone"), ReadBoneName(VelocityChannel, TEXT("Bone")), FName(TEXT("root")));
        TestEqual(TEXT("velocity weight"), ReadFloat(VelocityChannel, VelocityChannel->GetClass(), TEXT("Weight")), 2.5f);
    }
    if (PositionChannel)
    {
        TestEqual(TEXT("position boneName alias"), ReadBoneName(PositionChannel, TEXT("Bone")), FName(TEXT("root")));
        TestEqual(TEXT("position sampleTimeOffset"),
            ReadFloat(PositionChannel, PositionChannel->GetClass(), TEXT("SampleTimeOffset")), 0.25f);
    }
    if (PoseChannel)
    {
        const FArrayProperty* BonesProperty = FindFProperty<FArrayProperty>(PoseChannel->GetClass(), TEXT("SampledBones"));
        if (TestNotNull(TEXT("pose has SampledBones"), BonesProperty))
        {
            FScriptArrayHelper Bones(BonesProperty, BonesProperty->ContainerPtrToValuePtr<void>(PoseChannel));
            TestEqual(TEXT("pose sampled one bone"), Bones.Num(), 1);
        }
    }

    // An unknown kind is refused, names the supported kinds, and leaves no schema object.
    TestTrue(TEXT("bad-kind handler found"), InvokeHandlerWithCapture(TEXT("pose_search.create_schema"),
        MakeSchemaPayload(BadKindSchemaPackagePath, SkeletonObjectPath, {MakeChannel(TEXT("Trajectory")), MakeChannel(TEXT("NotAChannel"))}), Capture));
    TestFalse(TEXT("unknown kind refused"), Capture.bSuccess);
    TestEqual(TEXT("unknown kind error code"), Capture.ErrorCode, FString(TEXT("UNSUPPORTED_CHANNEL")));
    TestTrue(TEXT("unknown kind message lists Trajectory"), Capture.Message.Contains(TEXT("Trajectory")));
    TestNull(TEXT("unknown kind leaves no schema object"),
        FindObject<UPoseSearchSchema>(nullptr, *ToPoseSearchTestObjectPath(BadKindSchemaPackagePath)));

    // An unknown setting is refused rather than silently dropped.
    TSharedPtr<FJsonObject> BadSetting = MakeChannel(TEXT("Velocity"));
    BadSetting->SetNumberField(TEXT("notASetting"), 1.0);
    TestTrue(TEXT("bad-setting handler found"), InvokeHandlerWithCapture(TEXT("pose_search.create_schema"),
        MakeSchemaPayload(BadSettingSchemaPackagePath, SkeletonObjectPath, {BadSetting}), Capture));
    TestFalse(TEXT("unknown setting refused"), Capture.bSuccess);
    TestEqual(TEXT("unknown setting error code"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestNull(TEXT("unknown setting leaves no schema object"),
        FindObject<UPoseSearchSchema>(nullptr, *ToPoseSearchTestObjectPath(BadSettingSchemaPackagePath)));

    return true;
}

#endif // MCP_TEST_HAS_POSESEARCH
