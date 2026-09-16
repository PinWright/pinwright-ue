// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioral regression coverage for animation.setup_retargeting. The legacy verb
// duplicates an AnimSequence and swaps its Skeleton pointer; it does not run IK track
// remapping. These tests exercise the registered handler and verify both the wire
// contract and the on-disk destination state.

#include "Misc/AutomationTest.h"

#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Curves/RichCurve.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ReferenceSkeleton.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"
#include "Utils/AssetSaveState.h"
#include "Utils/AssetUtils.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace SetupRetargetingBehaviorTests
{
    static FString ObjectPathForPackage(const FString& PackagePath)
    {
        return FString::Printf(TEXT("%s.%s"), *PackagePath,
            *FPackageName::GetLongPackageAssetName(PackagePath));
    }

    static FString PackageFilename(const FString& PackagePath)
    {
        FString Filename;
        FPackageName::TryConvertLongPackageNameToFilename(
            PackagePath, Filename, FPackageName::GetAssetPackageExtension());
        return Filename;
    }

    static USkeleton* CreateSkeleton(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }

        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        USkeleton* Skeleton = NewObject<USkeleton>(
            Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
        if (!Skeleton)
        {
            return nullptr;
        }

        FReferenceSkeletonModifier Modifier(Skeleton);
        Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE),
            FTransform::Identity, /*bAllowMultipleRoots=*/true);
        FAssetRegistryModule::AssetCreated(Skeleton);
        Skeleton->MarkPackageDirty();
        return Skeleton;
    }

    static UAnimSequence* CreateSequence(const FString& PackagePath,
                                         USkeleton* Skeleton,
                                         int32 NumberOfFrames,
                                         const TCHAR* DistinguishingCurveName = nullptr)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package || !Skeleton)
        {
            return nullptr;
        }

        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UAnimSequence* Sequence = NewObject<UAnimSequence>(
            Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
        if (!Sequence)
        {
            return nullptr;
        }

        Sequence->SetSkeleton(Skeleton);
        IAnimationDataController& Controller = Sequence->GetController();
        Controller.InitializeModel();
        Controller.SetFrameRate(FFrameRate(30, 1));
        Controller.SetNumberOfFrames(FFrameNumber(NumberOfFrames));
        // Through UE 5.5, UAnimDataModel::GenerateGuid hashes only bone tracks, curves and
        // attributes - never the frame count - so two otherwise-empty sequences of different
        // lengths hash identically there. A named curve is content the hash covers on every
        // supported engine, which is what keeps "destination differs from source" a real
        // precondition rather than an engine-version accident.
        if (DistinguishingCurveName)
        {
            const FAnimationCurveIdentifier CurveId(
                FName(DistinguishingCurveName), ERawCurveTrackTypes::RCT_Float);
            Controller.AddCurve(CurveId);
            Controller.SetCurveKey(CurveId, FRichCurveKey(0.0f, 1.0f));
        }
        Controller.NotifyPopulated();
        FAssetRegistryModule::AssetCreated(Sequence);
        Sequence->MarkPackageDirty();
        return Sequence;
    }

    static bool Persist(UObject* Asset)
    {
        EAssetSaveState SaveState = EAssetSaveState::NotRequested;
        return SaveAssetToDiskReportingPresence(
            Asset, /*bForce=*/true, nullptr, nullptr, &SaveState);
    }

    static void DiscardPackage(const FString& PackagePath)
    {
        if (PackagePath.IsEmpty())
        {
            return;
        }

        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(
            ObjectPathForPackage(PackagePath));

        const FString Filename = PackageFilename(PackagePath);
        if (!Filename.IsEmpty())
        {
            FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(
                *Filename, false);
            IFileManager::Get().Delete(
                *Filename, /*RequireExists=*/false, /*EvenReadOnly=*/true,
                /*Quiet=*/true);
        }
    }

    struct FRetargetFixture
    {
        bool Create(const TCHAR* Prefix)
        {
            RootFolder = FString::Printf(TEXT("/Game/PinWrightTests/%s_%s"), Prefix,
                *FGuid::NewGuid().ToString(EGuidFormats::Digits));
            SourceSkeletonPackage = RootFolder / TEXT("SK_Source");
            TargetSkeletonPackage = RootFolder / TEXT("SK_Target");
            SourceSequencePackage = RootFolder / TEXT("AS_Source");
            OutputFolder = RootFolder / TEXT("Output");
            DestinationSequencePackage = OutputFolder / TEXT("AS_Source_Copy");

            SourceSkeleton = CreateSkeleton(SourceSkeletonPackage);
            TargetSkeleton = CreateSkeleton(TargetSkeletonPackage);
            SourceSequence = CreateSequence(SourceSequencePackage, SourceSkeleton, 7);
            DestinationSequence = CreateSequence(
                DestinationSequencePackage, SourceSkeleton, 2,
                TEXT("PWFixtureDestinationMarker"));
            if (!SourceSkeleton || !TargetSkeleton || !SourceSequence ||
                !DestinationSequence)
            {
                return false;
            }

            SourceSkeletonRoot.Reset(SourceSkeleton);
            TargetSkeletonRoot.Reset(TargetSkeleton);
            SourceSequenceRoot.Reset(SourceSequence);

            return Persist(SourceSkeleton) && Persist(TargetSkeleton) &&
                Persist(SourceSequence) && Persist(DestinationSequence);
        }

        void Reset()
        {
            SourceSequenceRoot.Reset();
            TargetSkeletonRoot.Reset();
            SourceSkeletonRoot.Reset();

            DiscardPackage(DestinationSequencePackage);
            DiscardPackage(SourceSequencePackage);
            DiscardPackage(TargetSkeletonPackage);
            DiscardPackage(SourceSkeletonPackage);
        }

        TSharedPtr<FJsonObject> MakePayload() const
        {
            TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
            Payload->SetStringField(TEXT("sourceSkeleton"),
                ObjectPathForPackage(SourceSkeletonPackage));
            Payload->SetStringField(TEXT("targetSkeleton"),
                ObjectPathForPackage(TargetSkeletonPackage));
            Payload->SetStringField(TEXT("savePath"), OutputFolder);
            Payload->SetStringField(TEXT("suffix"), TEXT("_Copy"));
            Payload->SetBoolField(TEXT("overwrite"), true);
            Payload->SetArrayField(TEXT("assets"), {
                MakeShared<FJsonValueString>(
                    ObjectPathForPackage(SourceSequencePackage))});
            return Payload;
        }

        FString DestinationObjectPath() const
        {
            return ObjectPathForPackage(DestinationSequencePackage);
        }

        FString RootFolder;
        FString SourceSkeletonPackage;
        FString TargetSkeletonPackage;
        FString SourceSequencePackage;
        FString OutputFolder;
        FString DestinationSequencePackage;
        USkeleton* SourceSkeleton = nullptr;
        USkeleton* TargetSkeleton = nullptr;
        UAnimSequence* SourceSequence = nullptr;
        UAnimSequence* DestinationSequence = nullptr;
        TStrongObjectPtr<USkeleton> SourceSkeletonRoot;
        TStrongObjectPtr<USkeleton> TargetSkeletonRoot;
        TStrongObjectPtr<UAnimSequence> SourceSequenceRoot;
    };

    static bool TryGetBool(const TSharedPtr<FJsonObject>& Object,
                           const TCHAR* Field, bool& OutValue)
    {
        return Object.IsValid() && Object->TryGetBoolField(Field, OutValue);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetupRetargetingInvalidSavePathTest,
    "PinWright.animation.setup_retargeting.InvalidPresentSavePathIsRejectedUpFront",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetupRetargetingInvalidSavePathTest::RunTest(const FString& /*Parameters*/)
{
    const FString MissingAssetPackage = FString::Printf(
        TEXT("/Game/PinWrightTests/PWRetargetMissing_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString MissingObjectPath =
        SetupRetargetingBehaviorTests::ObjectPathForPackage(MissingAssetPackage);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("sourceSkeleton"), MissingObjectPath);
    Payload->SetStringField(TEXT("targetSkeleton"), MissingObjectPath);
    Payload->SetStringField(TEXT("savePath"), TEXT("/Game/Invalid.Path"));
    Payload->SetStringField(TEXT("suffix"), TEXT("_ShouldNotExist"));
    Payload->SetBoolField(TEXT("overwrite"), true);
    Payload->SetArrayField(TEXT("assets"), {
        MakeShared<FJsonValueString>(MissingObjectPath)});

    FTestResponseCapture Capture;
    TestTrue(TEXT("setup_retargeting handler is registered"),
        InvokeHandlerWithCapture(
            TEXT("animation.setup_retargeting"), Payload, Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("invalid savePath is rejected"), Capture.bSuccess);
    TestEqual(TEXT("invalid path wins before missing-asset validation"),
        Capture.ErrorCode, FString(TEXT("INVALID_PATH")));
    TestNull(TEXT("source asset was not loaded or created"),
        StaticFindObject(UObject::StaticClass(), nullptr, *MissingObjectPath));
    TestFalse(TEXT("no package was written while rejecting savePath"),
        IFileManager::Get().FileExists(
            *SetupRetargetingBehaviorTests::PackageFilename(MissingAssetPackage)));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetupRetargetingOverwriteStagesSourceTest,
    "PinWright.animation.setup_retargeting.OverwriteStagesSourceBeforeReplacement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetupRetargetingOverwriteStagesSourceTest::RunTest(const FString& /*Parameters*/)
{
    using namespace SetupRetargetingBehaviorTests;

    FRetargetFixture Fixture;
    ON_SCOPE_EXIT { Fixture.Reset(); };
    if (!TestTrue(TEXT("durable animation fixture created"),
            Fixture.Create(TEXT("PWRetargetOverwrite"))))
    {
        return false;
    }

    const FGuid SourceDataGuid = Fixture.SourceSequence->GetDataModel()->GenerateGuid();
    const FGuid OriginalDestinationGuid =
        Fixture.DestinationSequence->GetDataModel()->GenerateGuid();
    TestTrue(TEXT("fixture destination differs from source"),
        OriginalDestinationGuid != SourceDataGuid);

    TArray<uint8> OldDestinationBytes;
    const FString DestinationFilename =
        PackageFilename(Fixture.DestinationSequencePackage);
    TestTrue(TEXT("original destination file is readable"),
        FFileHelper::LoadFileToArray(OldDestinationBytes, *DestinationFilename));

    FTestResponseCapture Capture;
    TestTrue(TEXT("setup_retargeting handler is registered"),
        InvokeHandlerWithCapture(TEXT("animation.setup_retargeting"),
            Fixture.MakePayload(), Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestTrue(*FString::Printf(TEXT("overwrite succeeds: %s"), *Capture.Message),
        Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bRetargeted = true;
    bool bSkeletonSwap = false;
    bool bControlRigBindingCleared = false;
    bool bDurable = false;
    bool bSaved = false;
    TestTrue(TEXT("retargeted field is present"),
        TryGetBool(Capture.Result, TEXT("retargeted"), bRetargeted));
    TestFalse(TEXT("handler does not claim IK retargeting"), bRetargeted);
    TestTrue(TEXT("skeleton-swap field is present"),
        TryGetBool(Capture.Result, TEXT("duplicatedWithSkeletonSwap"), bSkeletonSwap));
    TestTrue(TEXT("handler reports the actual skeleton swap"), bSkeletonSwap);
    TestTrue(TEXT("control-rig binding is cleared before skeleton swap"),
        TryGetBool(Capture.Result, TEXT("controlRigBindingCleared"),
            bControlRigBindingCleared));
    TestTrue(TEXT("successful overwrite clears the control-rig binding"),
        bControlRigBindingCleared);
    TestTrue(TEXT("disk persistence field is present"),
        TryGetBool(Capture.Result, TEXT("diskPersistenceGuaranteed"), bDurable));
    TestTrue(TEXT("successful overwrite is durable"), bDurable);
    TestTrue(TEXT("saved field is present"),
        TryGetBool(Capture.Result, TEXT("saved"), bSaved));
    TestTrue(TEXT("successful overwrite reports saved"), bSaved);
    TestTrue(TEXT("result identifies the published destination"),
        JsonStringArrayContains(Capture.Result, TEXT("duplicatedAssets"),
            Fixture.DestinationObjectPath()));

    UAnimSequence* Published = LoadObject<UAnimSequence>(
        nullptr, *Fixture.DestinationObjectPath());
    TestNotNull(TEXT("published destination resolves after overwrite"), Published);
    if (Published)
    {
        TestEqual(TEXT("published data came from the source"),
            Published->GetDataModel()->GenerateGuid(), SourceDataGuid);
        TestEqual(TEXT("published destination uses target skeleton"),
            Published->GetSkeleton(), Fixture.TargetSkeleton);
    }

    TArray<uint8> NewDestinationBytes;
    TestTrue(TEXT("published destination remains on disk"),
        FFileHelper::LoadFileToArray(NewDestinationBytes, *DestinationFilename));
    TestTrue(TEXT("destination file contains a new serialized asset"),
        NewDestinationBytes != OldDestinationBytes);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetupRetargetingPersistFailureRestoresOriginalTest,
    "PinWright.animation.setup_retargeting.PersistFailureRestoresOriginal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSetupRetargetingPersistFailureRestoresOriginalTest::RunTest(
    const FString& /*Parameters*/)
{
    using namespace SetupRetargetingBehaviorTests;

    FRetargetFixture Fixture;
    if (!TestTrue(TEXT("durable animation fixture created"),
            Fixture.Create(TEXT("PWRetargetRollback"))))
    {
        Fixture.Reset();
        return false;
    }

    const FString DestinationFilename =
        PackageFilename(Fixture.DestinationSequencePackage);
    const FGuid OriginalDestinationGuid =
        Fixture.DestinationSequence->GetDataModel()->GenerateGuid();
    TArray<uint8> OriginalBytes;
    TestTrue(TEXT("original destination file is readable"),
        FFileHelper::LoadFileToArray(OriginalBytes, *DestinationFilename));
    TestTrue(TEXT("destination file made read-only"),
        FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(
            *DestinationFilename, true));
    ON_SCOPE_EXIT
    {
        FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(
            *DestinationFilename, false);
        Fixture.Reset();
    };

    AddExpectedError(TEXT("Cannot remove"),
        EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedError(TEXT("Error saving"),
        EAutomationExpectedErrorFlags::Contains, 2);
    FTestResponseCapture Capture;
    TestTrue(TEXT("setup_retargeting handler is registered"),
        InvokeHandlerWithCapture(TEXT("animation.setup_retargeting"),
            Fixture.MakePayload(), Capture));
    TestTrue(TEXT("handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("read-only destination refuses publication"), Capture.bSuccess);
    TestEqual(TEXT("read-only destination fails while saving the staged destination"),
        Capture.ErrorCode, FString(TEXT("SAVE_FAILED")));
    if (Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bOriginalRestored = false;
    bool bOriginalFilePresent = false;
    bool bDurable = false;
    TestTrue(TEXT("originalRestored field is present"),
        TryGetBool(Capture.Result, TEXT("originalRestored"), bOriginalRestored));
    TestTrue(TEXT("original object is restored"), bOriginalRestored);
    TestTrue(TEXT("originalFilePresent field is present"),
        TryGetBool(Capture.Result, TEXT("originalFilePresent"),
            bOriginalFilePresent));
    TestTrue(TEXT("original file remains present"), bOriginalFilePresent);
    TestTrue(TEXT("disk persistence field is present"),
        TryGetBool(Capture.Result, TEXT("diskPersistenceGuaranteed"), bDurable));
    TestTrue(TEXT("failure response guarantees the restored original"), bDurable);

    UAnimSequence* Restored = LoadObject<UAnimSequence>(
        nullptr, *Fixture.DestinationObjectPath());
    TestNotNull(TEXT("original destination still resolves"), Restored);
    if (Restored)
    {
        TestEqual(TEXT("original destination data is unchanged"),
            Restored->GetDataModel()->GenerateGuid(), OriginalDestinationGuid);
        TestEqual(TEXT("original destination skeleton is unchanged"),
            Restored->GetSkeleton(), Fixture.SourceSkeleton);
    }

    TArray<uint8> BytesAfterFailure;
    TestTrue(TEXT("original destination file remains readable"),
        FFileHelper::LoadFileToArray(BytesAfterFailure, *DestinationFilename));
    TestTrue(TEXT("original destination file bytes are unchanged"),
        BytesAfterFailure == OriginalBytes);
    return true;
}
