// Copyright (c) 2026 Alexander Penkin. MIT License.

// End-to-end coverage for sequencer.repoint_actor. The fixtures deliberately use normal editor
// world actors and the runtime binding resolver: a green response without the locator readback is
// not proof that the replacement reached playback.

#include "Misc/AutomationTest.h"

#include "Compat/EngineVersionCompat.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Camera/CameraActor.h"
#include "Misc/ScopeExit.h"

// Custom bindings (UMovieSceneCustomBinding and the replaceable-actor binding built on it)
// arrived in UE 5.5 together with FMovieSceneBindingReference::CustomBinding. On 5.4 no
// binding reference can carry one, so the fixture, the snapshot field and the refusal case
// below are all 5.5+.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
#include "Bindings/MovieSceneReplaceableActorBinding.h"
#endif
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSequence.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Sequencer/SequencerBindingUtils.h"
#include "PinWrightSubsystem.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/Sequencer/SequencerTestFixtures.h"
#include "Utils/AssetUtils.h"

// sequencer.repoint_actor exists only on UE 5.4+: it rewrites the FUniversalObjectLocator on a
// possessable's binding reference, and 5.3 predates the locator binding model entirely (see
// SequencerBindingUtils.h, which compiles the machinery out on the same gate, and
// SequenceHandler.cpp, which refuses the verb by name there). With nothing to exercise, the whole
// fixture and every case in this file compile out rather than assert against an absent capability.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)

#include "MovieSceneBindingReferences.h"

namespace PinWrightRepointTests
{
    struct FRepointFixture
    {
        ULevelSequence* Sequence = nullptr;
        FString SequencePath;
        UWorld* World = nullptr;
        UPinWrightSubsystem* Subsystem = nullptr;
        AActor* SourceActor = nullptr;
        AActor* TargetActor = nullptr;
        FString SourceLabel;
        FString TargetLabel;
        FGuid BindingGuid;

        // The sequence is RF_Standalone, so the periodic suite GC keeps it alive; detach it from
        // its /Engine/Transient package at end of test. The dtor would otherwise suppress the
        // implicit moves and turn `OutFixture = FRepointFixture();` into a copy, which would leave
        // the reassigned fixture's first path uncleaned; restore them and ban copying instead.
        // A moved-from FString is empty, so the source's dtor is then a no-op.
        FRepointFixture() = default;
        FRepointFixture(FRepointFixture&&) = default;
        FRepointFixture& operator=(FRepointFixture&&) = default;
        FRepointFixture(const FRepointFixture&) = delete;
        FRepointFixture& operator=(const FRepointFixture&) = delete;
        ~FRepointFixture()
        {
            CleanupTestAsset(SequencePath);
        }
    };

    struct FBindingStateSnapshot
    {
        FGuid BindingGuid;
        bool bHasPossessable = false;
        bool bHasSpawnable = false;
        FString BindingName;
        FString AuthoredClassPath;
        FGuid ParentGuid;
        int32 LocatorCount = 0;
        TArray<FUniversalObjectLocator> Locators;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        TArray<UMovieSceneCustomBinding*> CustomBindings;
#endif
        FString TrackSignature;
    };

    static bool InvokeRepointHandler(const TSharedPtr<FJsonObject>& Payload,
        UPinWrightSubsystem* Subsystem, FTestResponseCapture& Capture)
    {
        Capture.Reset();
        for (const FHandlerRegistration& Registration : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Registration.MethodName == TEXT("sequencer.repoint_actor"))
            {
                FHandlerContext Context = FHandlerContext::MakeContextWithCapture(
                    TEXT("repoint-test"), TEXT("sequencer.repoint_actor"), Payload, Subsystem,
                    &Capture);
                Registration.Func(Context);
                return true;
            }
        }
        return false;
    }

    static FString BoundToString(const TRangeBound<FFrameNumber>& Bound)
    {
        if (Bound.IsOpen())
        {
            return TEXT("open");
        }
        return FString::Printf(TEXT("%d%s"), Bound.GetValue().Value,
            Bound.IsInclusive() ? TEXT("i") : TEXT("e"));
    }

    static FString RangeToString(const TRange<FFrameNumber>& Range)
    {
        return FString::Printf(TEXT("[%s,%s]"),
            *BoundToString(Range.GetLowerBound()), *BoundToString(Range.GetUpperBound()));
    }

    static void AppendDoubleChannelSignature(FString& Out, const FMovieSceneDoubleChannel* Channel)
    {
        Out += TEXT("D{");
        if (Channel)
        {
            const TArrayView<const FFrameNumber> Times = Channel->GetTimes();
            const TArrayView<const FMovieSceneDoubleValue> Values = Channel->GetValues();
            Out += FString::Printf(TEXT("%d:"), FMath::Min(Times.Num(), Values.Num()));
            for (int32 Index = 0; Index < FMath::Min(Times.Num(), Values.Num()); ++Index)
            {
                Out += FString::Printf(TEXT("%d=%.17g;"), Times[Index].Value, Values[Index].Value);
            }
        }
        Out += TEXT("}");
    }

    static void AppendBoolChannelSignature(FString& Out, const FMovieSceneBoolChannel* Channel)
    {
        Out += TEXT("B{");
        if (Channel)
        {
            const TArrayView<const FFrameNumber> Times = Channel->GetTimes();
            const TArrayView<const bool> Values = Channel->GetValues();
            Out += FString::Printf(TEXT("%d:"), FMath::Min(Times.Num(), Values.Num()));
            for (int32 Index = 0; Index < FMath::Min(Times.Num(), Values.Num()); ++Index)
            {
                Out += FString::Printf(TEXT("%d=%s;"), Times[Index].Value,
                    Values[Index] ? TEXT("true") : TEXT("false"));
            }
        }
        Out += TEXT("}");
    }

    static FString SectionSignature(const UMovieSceneSection* Section)
    {
        FString Signature;
        if (!Section)
        {
            return Signature;
        }

        Signature += Section->GetClass()->GetPathName();
        Signature += TEXT("|");
        Signature += RangeToString(Section->GetRange());
        Signature += TEXT("|");

        const FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
        const TArrayView<FMovieSceneDoubleChannel*> DoubleChannels =
            Proxy.GetChannels<FMovieSceneDoubleChannel>();
        for (const FMovieSceneDoubleChannel* Channel : DoubleChannels)
        {
            AppendDoubleChannelSignature(Signature, Channel);
        }

        const TArrayView<FMovieSceneBoolChannel*> BoolChannels =
            Proxy.GetChannels<FMovieSceneBoolChannel>();
        for (const FMovieSceneBoolChannel* Channel : BoolChannels)
        {
            AppendBoolChannelSignature(Signature, Channel);
        }
        return Signature;
    }

    static FString TrackSignature(const UMovieSceneTrack* Track)
    {
        FString Signature;
        if (!Track)
        {
            return Signature;
        }

        Signature += Track->GetClass()->GetPathName();
        Signature += TEXT("|");
        Signature += Track->GetName();
        Signature += TEXT("|");
        Signature += Track->GetDisplayName().ToString();
        Signature += FString::Printf(TEXT("|sections=%d|"), Track->GetAllSections().Num());
        for (const UMovieSceneSection* Section : Track->GetAllSections())
        {
            Signature += SectionSignature(Section);
            Signature += TEXT(";");
        }
        return Signature;
    }

    static FBindingStateSnapshot SnapshotBinding(ULevelSequence* Sequence, const FGuid& Guid)
    {
        FBindingStateSnapshot Snapshot;
        if (!Sequence || !Guid.IsValid() || !Sequence->GetMovieScene())
        {
            return Snapshot;
        }
        Snapshot.BindingGuid = Guid;

        UMovieScene* MovieScene = Sequence->GetMovieScene();
        if (FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid))
        {
            Snapshot.bHasPossessable = true;
            Snapshot.BindingName = Possessable->GetName();
            Snapshot.AuthoredClassPath = Possessable->GetPossessedObjectClass()
                ? Possessable->GetPossessedObjectClass()->GetPathName()
                : FString();
            Snapshot.ParentGuid = Possessable->GetParent();
        }
        if (FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(Guid))
        {
            Snapshot.bHasSpawnable = true;
            Snapshot.BindingName = Spawnable->GetName();
        }

        if (const FMovieSceneBindingReferences* References =
                static_cast<UMovieSceneSequence*>(Sequence)->GetBindingReferences())
        {
            const TArrayView<const FMovieSceneBindingReference> BindingReferences =
                References->GetReferences(Guid);
            Snapshot.LocatorCount = BindingReferences.Num();
            Snapshot.Locators.Reserve(BindingReferences.Num());
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            Snapshot.CustomBindings.Reserve(BindingReferences.Num());
#endif
            for (const FMovieSceneBindingReference& Reference : BindingReferences)
            {
                Snapshot.Locators.Add(Reference.Locator);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
                Snapshot.CustomBindings.Add(Reference.CustomBinding.Get());
#endif
            }
        }

        if (const FMovieSceneBinding* Binding = MovieScene->FindBinding(Guid))
        {
            for (const UMovieSceneTrack* Track : Binding->GetTracks())
            {
                Snapshot.TrackSignature += TrackSignature(Track);
                Snapshot.TrackSignature += TEXT("#");
            }
        }
        return Snapshot;
    }

    static bool SnapshotEqual(const FBindingStateSnapshot& A, const FBindingStateSnapshot& B)
    {
        if (A.BindingGuid != B.BindingGuid || A.bHasPossessable != B.bHasPossessable
            || A.bHasSpawnable != B.bHasSpawnable
            || A.BindingName != B.BindingName || A.AuthoredClassPath != B.AuthoredClassPath
            || A.ParentGuid != B.ParentGuid || A.LocatorCount != B.LocatorCount
            || A.Locators.Num() != B.Locators.Num()
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            || A.CustomBindings.Num() != B.CustomBindings.Num()
#endif
            || A.TrackSignature != B.TrackSignature)
        {
            return false;
        }
        for (int32 Index = 0; Index < A.Locators.Num(); ++Index)
        {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            if (A.Locators[Index] != B.Locators[Index]
                || A.CustomBindings[Index] != B.CustomBindings[Index])
#else
            if (A.Locators[Index] != B.Locators[Index])
#endif
            {
                return false;
            }
        }
        return true;
    }

    static bool BindingMetadataAndTrackSnapshotEqual(
        const FBindingStateSnapshot& A, const FBindingStateSnapshot& B)
    {
        if (A.BindingGuid != B.BindingGuid || A.bHasPossessable != B.bHasPossessable
            || A.bHasSpawnable != B.bHasSpawnable
            || A.BindingName != B.BindingName || A.AuthoredClassPath != B.AuthoredClassPath
            || A.ParentGuid != B.ParentGuid || A.LocatorCount != B.LocatorCount
            || A.Locators.Num() != B.Locators.Num()
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            || A.CustomBindings.Num() != B.CustomBindings.Num()
#endif
            || A.TrackSignature != B.TrackSignature)
        {
            return false;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        for (int32 Index = 0; Index < A.CustomBindings.Num(); ++Index)
        {
            if (A.CustomBindings[Index] != B.CustomBindings[Index])
            {
                return false;
            }
        }
#endif
        return true;
    }

    static bool SameResolvedObjects(const TArray<UObject*>& A, const TArray<UObject*>& B)
    {
        if (A.Num() != B.Num())
        {
            return false;
        }
        for (UObject* Object : A)
        {
            if (!B.Contains(Object))
            {
                return false;
            }
        }
        return true;
    }

    static void AssertFailureDetails(FAutomationTestBase& Test, const FTestResponseCapture& Capture,
        const FString& ExpectedCode, const FString& ExpectedMessage, const FString& ExpectedGuid,
        const FString& ExpectedOldName, const FString& ExpectedNewName, bool bExpectedReadback,
        int32 ExpectedLocatorCount)
    {
        Test.TestTrue(TEXT("repoint failure response was captured"), Capture.bWasCalled);
        Test.TestFalse(TEXT("repoint failure response is not successful"), Capture.bSuccess);
        Test.TestEqual(TEXT("repoint failure code is exact"), Capture.ErrorCode, ExpectedCode);
        Test.TestEqual(TEXT("repoint failure message is exact"), Capture.Message, ExpectedMessage);
        if (!Test.TestNotNull(TEXT("repoint failure carries details"), Capture.Result.Get()))
        {
            return;
        }

        static const TCHAR* const RequiredKeys[] =
        {
            TEXT("bindingGuid"), TEXT("bindingName"), TEXT("parentBindingGuid"),
            TEXT("locatorCount"), TEXT("oldActorName"), TEXT("newActorName"),
            TEXT("authoredClass"), TEXT("newObjectClass"), TEXT("oldObjectPath"),
            TEXT("resolvedObjectPath"), TEXT("readbackMeasured"), TEXT("readbackPhase"),
            TEXT("resolvesToNewActor"), TEXT("oldObjectUnbound"), TEXT("classMismatch"),
            TEXT("warnings"), TEXT("rollbackAttempted"), TEXT("rollbackSucceeded"),
            TEXT("restoredResolutionStatus"), TEXT("restoredResolvedObjectPath")
        };
        for (const TCHAR* Key : RequiredKeys)
        {
            Test.TestTrue(FString::Printf(TEXT("failure details contain '%s'"), Key),
                Capture.Result->HasField(Key));
        }

        FString StringValue;
        Test.TestTrue(TEXT("failure details bindingGuid is a string"),
            Capture.Result->TryGetStringField(TEXT("bindingGuid"), StringValue));
        Test.TestEqual(TEXT("failure details bindingGuid is measured from the request"),
            StringValue, ExpectedGuid);
        Test.TestTrue(TEXT("failure details oldActorName is a string"),
            Capture.Result->TryGetStringField(TEXT("oldActorName"), StringValue));
        Test.TestEqual(TEXT("failure details oldActorName echoes the request"),
            StringValue, ExpectedOldName);
        Test.TestTrue(TEXT("failure details newActorName is a string"),
            Capture.Result->TryGetStringField(TEXT("newActorName"), StringValue));
        Test.TestEqual(TEXT("failure details newActorName echoes the request"),
            StringValue, ExpectedNewName);

        double LocatorCount = -1.0;
        Test.TestTrue(TEXT("failure details locatorCount is numeric"),
            Capture.Result->TryGetNumberField(TEXT("locatorCount"), LocatorCount));
        Test.TestEqual(TEXT("failure details locatorCount is unchanged"),
            static_cast<int32>(LocatorCount), ExpectedLocatorCount);

        bool bValue = true;
        Test.TestTrue(TEXT("failure details readbackMeasured is boolean"),
            Capture.Result->TryGetBoolField(TEXT("readbackMeasured"), bValue));
        Test.TestEqual(TEXT("failure details readbackMeasured has the expected phase state"),
            bValue, bExpectedReadback);
        Test.TestTrue(TEXT("failure details rollbackAttempted is boolean"),
            Capture.Result->TryGetBoolField(TEXT("rollbackAttempted"), bValue));
        Test.TestFalse(TEXT("pre-write failure never attempts rollback"), bValue);
        Test.TestTrue(TEXT("failure details rollbackSucceeded is boolean"),
            Capture.Result->TryGetBoolField(TEXT("rollbackSucceeded"), bValue));
        Test.TestFalse(TEXT("pre-write failure does not claim rollback success"), bValue);
        Test.TestTrue(TEXT("failure details restoredResolutionStatus is not attempted"),
            Capture.Result->TryGetStringField(TEXT("restoredResolutionStatus"), StringValue));
        Test.TestEqual(TEXT("restoredResolutionStatus is the stable default"),
            StringValue, FString(TEXT("not_attempted")));
        Test.TestTrue(TEXT("failure details restoredResolvedObjectPath is a string"),
            Capture.Result->TryGetStringField(TEXT("restoredResolvedObjectPath"), StringValue));
        Test.TestTrue(TEXT("restoredResolvedObjectPath is empty before a write"), StringValue.IsEmpty());

        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        Test.TestTrue(TEXT("failure details warnings is an array"),
            Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings);
        if (Warnings)
        {
            Test.TestEqual(TEXT("pre-write failures carry no warnings"), Warnings->Num(), 0);
        }
    }

    static bool MakeActorFixture(FAutomationTestBase& Test, FRepointFixture& OutFixture,
        bool bCameraTarget = false)
    {
        OutFixture = FRepointFixture();
        if (!GEditor)
        {
            Test.AddError(TEXT("GEditor unavailable - repoint fixtures require an editor world."));
            return false;
        }

        OutFixture.World = GEditor->GetEditorWorldContext().World();
        OutFixture.Subsystem = GEditor->GetEditorSubsystem<UPinWrightSubsystem>();
        if (!Test.TestNotNull(TEXT("editor world exists for repoint fixture"), OutFixture.World)
            || !Test.TestNotNull(TEXT("PinWright subsystem exists for repoint fixture"), OutFixture.Subsystem))
        {
            return false;
        }

        UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!Test.TestNotNull(TEXT("engine cube fixture exists"), CubeMesh))
        {
            return false;
        }

        const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        OutFixture.SourceLabel = FString::Printf(TEXT("PWRepointSource_%s"), *Suffix);
        OutFixture.TargetLabel = FString::Printf(TEXT("PWRepointTarget_%s"), *Suffix);
        OutFixture.SourceActor = SpawnActorInActiveWorld<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), FVector(0.0, 0.0, 0.0), FRotator::ZeroRotator,
            OutFixture.SourceLabel);
        if (!Test.TestNotNull(TEXT("source actor spawned as a normal editor-world actor"),
                OutFixture.SourceActor))
        {
            return false;
        }

        if (AStaticMeshActor* SourceMeshActor = Cast<AStaticMeshActor>(OutFixture.SourceActor))
        {
            SourceMeshActor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
            SourceMeshActor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
        }

        if (bCameraTarget)
        {
            OutFixture.TargetActor = SpawnActorInActiveWorld<ACameraActor>(
                ACameraActor::StaticClass(), FVector(1000.0, 0.0, 0.0), FRotator::ZeroRotator,
                OutFixture.TargetLabel);
        }
        else
        {
            OutFixture.TargetActor = SpawnActorInActiveWorld<AStaticMeshActor>(
                AStaticMeshActor::StaticClass(), FVector(1000.0, 0.0, 0.0), FRotator::ZeroRotator,
                OutFixture.TargetLabel);
            if (AStaticMeshActor* TargetMeshActor = Cast<AStaticMeshActor>(OutFixture.TargetActor))
            {
                TargetMeshActor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
                TargetMeshActor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
            }
        }
        if (!Test.TestNotNull(TEXT("target actor spawned as a normal editor-world actor"),
                OutFixture.TargetActor))
        {
            return false;
        }

        OutFixture.Sequence = SequencerTestFixtures::MakeTransientSequence(
            TEXT("RepointActor"), OutFixture.SequencePath);
        if (!Test.TestNotNull(TEXT("transient LevelSequence created"), OutFixture.Sequence)
            || !Test.TestNotNull(TEXT("repoint fixture MovieScene exists"),
                OutFixture.Sequence ? OutFixture.Sequence->GetMovieScene() : nullptr))
        {
            return false;
        }

        UMovieScene* MovieScene = OutFixture.Sequence->GetMovieScene();
        OutFixture.BindingGuid = MovieScene->AddPossessable(
            OutFixture.SourceLabel, OutFixture.SourceActor->GetClass());
        if (!Test.TestTrue(TEXT("fixture possessable GUID is valid"), OutFixture.BindingGuid.IsValid()))
        {
            return false;
        }
        OutFixture.Sequence->BindPossessableObject(
            OutFixture.BindingGuid, *OutFixture.SourceActor, OutFixture.World);
        return true;
    }

    static bool AddTransformAndBoolTracks(FAutomationTestBase& Test,
        ULevelSequence* Sequence, const FGuid& BindingGuid, bool bTwoTransformSections = true)
    {
        UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
        if (!Test.TestNotNull(TEXT("MovieScene is available for track authoring"), MovieScene))
        {
            return false;
        }

        UMovieScene3DTransformTrack* TransformTrack =
            MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid);
        if (!Test.TestNotNull(TEXT("transform track authored on the binding"), TransformTrack))
        {
            return false;
        }

        auto AddTransformSection = [&](int32 StartFrame, int32 EndFrame,
            double StartX, double EndX) -> bool
        {
            UMovieScene3DTransformSection* Section =
                Cast<UMovieScene3DTransformSection>(TransformTrack->CreateNewSection());
            if (!Section)
            {
                return false;
            }
            Section->SetRange(TRange<FFrameNumber>(FFrameNumber(StartFrame), FFrameNumber(EndFrame)));
            TransformTrack->AddSection(*Section);
            const TArrayView<FMovieSceneDoubleChannel*> Channels =
                Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
            if (!Channels.IsValidIndex(0) || !Channels[0])
            {
                return false;
            }
            Channels[0]->GetData().UpdateOrAddKey(
                FFrameNumber(StartFrame), FMovieSceneDoubleValue(StartX));
            Channels[0]->GetData().UpdateOrAddKey(
                FFrameNumber(EndFrame), FMovieSceneDoubleValue(EndX));
            return true;
        };

        if (!Test.TestTrue(TEXT("first transform section carries multiple location keys"),
                AddTransformSection(0, 30, 0.0, 100.0)))
        {
            return false;
        }
        if (bTwoTransformSections
            && !Test.TestTrue(TEXT("second transform section carries multiple location keys"),
                AddTransformSection(30, 60, 100.0, 200.0)))
        {
            return false;
        }

        UMovieSceneBoolTrack* BoolTrack = MovieScene->AddTrack<UMovieSceneBoolTrack>(BindingGuid);
        if (!Test.TestNotNull(TEXT("second key-bearing bool property track authored"), BoolTrack))
        {
            return false;
        }
        BoolTrack->SetPropertyNameAndPath(FName(TEXT("bHidden")), TEXT("bHidden"));
        UMovieSceneBoolSection* BoolSection =
            Cast<UMovieSceneBoolSection>(BoolTrack->CreateNewSection());
        if (!Test.TestNotNull(TEXT("bool property section authored"), BoolSection))
        {
            return false;
        }
        BoolSection->SetRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(60)));
        BoolTrack->AddSection(*BoolSection);
        const TArrayView<FMovieSceneBoolChannel*> BoolChannels =
            BoolSection->GetChannelProxy().GetChannels<FMovieSceneBoolChannel>();
        if (!Test.TestTrue(TEXT("bool section exposes a key channel"),
                BoolChannels.IsValidIndex(0) && BoolChannels[0]))
        {
            return false;
        }
        TArray<FFrameNumber> BoolTimes{ FFrameNumber(0), FFrameNumber(30) };
        TArray<bool> BoolValues{ false, true };
        BoolChannels[0]->AddKeys(BoolTimes, BoolValues);
        return true;
    }

    static bool AddTransformRamp(FAutomationTestBase& Test, ULevelSequence* Sequence,
        const FGuid& BindingGuid, int32 EndFrame, double EndX)
    {
        UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
        UMovieScene3DTransformTrack* Track = MovieScene
            ? MovieScene->AddTrack<UMovieScene3DTransformTrack>(BindingGuid)
            : nullptr;
        if (!Test.TestNotNull(TEXT("evaluation transform track authored"), Track))
        {
            return false;
        }
        UMovieScene3DTransformSection* Section =
            Cast<UMovieScene3DTransformSection>(Track->CreateNewSection());
        if (!Test.TestNotNull(TEXT("evaluation transform section authored"), Section))
        {
            return false;
        }
        const FFrameNumber EndTick = FFrameRate::TransformTime(
            FFrameTime(FFrameNumber(EndFrame)),
            MovieScene->GetDisplayRate(), MovieScene->GetTickResolution()).FloorToFrame();
        Section->SetRange(TRange<FFrameNumber>::All());
        // Sequence players evaluate MovieScene ticks after converting the display-frame input.
        // Author the section and playback range in that same tick domain.
        MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), EndTick));
        Track->AddSection(*Section);
        const TArrayView<FMovieSceneDoubleChannel*> Channels =
            Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
        if (!Test.TestTrue(TEXT("evaluation transform exposes location X channel"),
                Channels.IsValidIndex(0) && Channels[0]))
        {
            return false;
        }
        Channels[0]->GetData().UpdateOrAddKey(FFrameNumber(0), FMovieSceneDoubleValue(0.0));
        Channels[0]->GetData().UpdateOrAddKey(
            EndTick, FMovieSceneDoubleValue(EndX));
        return true;
    }

    static TSharedPtr<FJsonObject> MakeRepointPayload(const FRepointFixture& Fixture,
        const FString& OldName, const FString& NewName, const FString& GuidString = FString())
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), Fixture.SequencePath);
        Payload->SetStringField(TEXT("bindingGuid"), GuidString.IsEmpty()
            ? Fixture.BindingGuid.ToString()
            : GuidString);
        Payload->SetStringField(TEXT("oldActorName"), OldName);
        Payload->SetStringField(TEXT("newActorName"), NewName);
        return Payload;
    }

    static bool AssertSuccessEnvelope(FAutomationTestBase& Test,
        const FTestResponseCapture& Capture, const FRepointFixture& Fixture,
        bool bClassMismatch, int32 WarningCount)
    {
        if (!Test.TestTrue(TEXT("repoint success response was captured"), Capture.bWasCalled)
            || !Test.TestTrue(TEXT("repoint success response is successful"), Capture.bSuccess)
            || !Test.TestNotNull(TEXT("repoint success carries a result"), Capture.Result.Get()))
        {
            return false;
        }

        FString StringValue;
        Test.TestTrue(TEXT("success result carries bindingGuid"),
            Capture.Result->TryGetStringField(TEXT("bindingGuid"), StringValue));
        Test.TestEqual(TEXT("success result preserves the binding GUID"),
            StringValue, Fixture.BindingGuid.ToString());
        Test.TestTrue(TEXT("success result carries bindingName"),
            Capture.Result->TryGetStringField(TEXT("bindingName"), StringValue));
        Test.TestEqual(TEXT("success result preserves the authored binding name"),
            StringValue, Fixture.SourceLabel);
        Test.TestTrue(TEXT("success result carries parentBindingGuid"),
            Capture.Result->TryGetStringField(TEXT("parentBindingGuid"), StringValue));
        Test.TestTrue(TEXT("top-level success has no parent GUID"), StringValue.IsEmpty());

        double LocatorCount = 0.0;
        Test.TestTrue(TEXT("success result carries locatorCount"),
            Capture.Result->TryGetNumberField(TEXT("locatorCount"), LocatorCount));
        Test.TestEqual(TEXT("success result proves one locator"), static_cast<int32>(LocatorCount), 1);

        bool bValue = false;
        Test.TestTrue(TEXT("success result carries readbackMeasured"),
            Capture.Result->TryGetBoolField(TEXT("readbackMeasured"), bValue));
        Test.TestTrue(TEXT("success result readbackMeasured is true"), bValue);
        Test.TestTrue(TEXT("success result carries resolvesToNewActor"),
            Capture.Result->TryGetBoolField(TEXT("resolvesToNewActor"), bValue));
        Test.TestTrue(TEXT("success result resolves to the target actor"), bValue);
        Test.TestTrue(TEXT("success result carries oldObjectUnbound"),
            Capture.Result->TryGetBoolField(TEXT("oldObjectUnbound"), bValue));
        Test.TestTrue(TEXT("success result proves the old actor is unbound"), bValue);
        Test.TestTrue(TEXT("success result carries classMismatch"),
            Capture.Result->TryGetBoolField(TEXT("classMismatch"), bValue));
        Test.TestEqual(TEXT("success result classMismatch is measured"), bValue, bClassMismatch);

        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (!Test.TestTrue(TEXT("success result carries warnings array"),
                Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings))
        {
            return false;
        }
        Test.TestEqual(TEXT("success result has the expected warning count"),
            Warnings->Num(), WarningCount);
        return true;
    }

    static void AssertSuccessfulResolution(FAutomationTestBase& Test,
        ULevelSequence* Sequence, const FGuid& Guid, AActor* OldActor, AActor* NewActor)
    {
        const TArray<UObject*> Objects = SequencerBindingUtils::ResolveBoundObjects(Sequence, Guid);
        Test.TestTrue(TEXT("independent movie-scene readback resolves the target actor"),
            Objects.Contains(NewActor));
        Test.TestFalse(TEXT("independent movie-scene readback no longer resolves the old actor"),
            Objects.Contains(OldActor));
    }

    static bool PrepareUnsupportedFixture(FAutomationTestBase& Test, FRepointFixture& Fixture,
        const FString& Kind)
    {
        if (!MakeActorFixture(Test, Fixture))
        {
            return false;
        }
        UMovieScene* MovieScene = Fixture.Sequence->GetMovieScene();
        if (Kind == TEXT("component"))
        {
            UMovieSceneSequence* SequenceBase = Fixture.Sequence;
            Fixture.BindingGuid = SequenceBase->CreatePossessable(
                CastChecked<AStaticMeshActor>(Fixture.SourceActor)->GetStaticMeshComponent());
            if (!Test.TestTrue(TEXT("component child possessable was created"),
                    Fixture.BindingGuid.IsValid()))
            {
                return false;
            }
        }
        else if (Kind == TEXT("spawnable"))
        {
            AStaticMeshActor* Template = NewObject<AStaticMeshActor>(
                MovieScene, NAME_None, RF_Transactional);
            if (!Test.TestNotNull(TEXT("spawnable template was created"), Template))
            {
                return false;
            }
            Fixture.BindingGuid = MovieScene->AddSpawnable(TEXT("PWRepointSpawnable"), *Template);
            if (!Test.TestTrue(TEXT("public AddSpawnable path produced a GUID"),
                    Fixture.BindingGuid.IsValid()))
            {
                return false;
            }
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        else if (Kind == TEXT("custom"))
        {
            Fixture.BindingGuid = MovieScene->AddPossessable(
                Fixture.SourceLabel, Fixture.SourceActor->GetClass());
            Fixture.Sequence->BindPossessableObject(
                Fixture.BindingGuid, *Fixture.SourceActor, Fixture.World);
            UMovieSceneReplaceableActorBinding* CustomBinding = NewObject<
                UMovieSceneReplaceableActorBinding>(MovieScene, NAME_None, RF_Transactional);
            if (!Test.TestNotNull(TEXT("replaceable custom binding was created"), CustomBinding))
            {
                return false;
            }
            FMovieSceneBindingReferences* References =
                static_cast<UMovieSceneSequence*>(Fixture.Sequence)->GetBindingReferences();
            if (!Test.TestNotNull(TEXT("sequence binding references are available"), References))
            {
                return false;
            }
            References->AddOrReplaceBinding(Fixture.BindingGuid, CustomBinding, 0);
        }
#endif
        else if (Kind == TEXT("multi"))
        {
            Fixture.Sequence->BindPossessableObject(
                Fixture.BindingGuid, *Fixture.TargetActor, Fixture.World);
        }
        else
        {
            Test.AddError(TEXT("unknown unsupported repoint fixture kind"));
            return false;
        }

        return AddTransformAndBoolTracks(Test, Fixture.Sequence, Fixture.BindingGuid, false);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerRepointPreservesGuidTracksAndKeysTest,
    "PinWright.Sequencer.RepointActor.PreservesGuidTracksAndKeys",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerRepointPreservesGuidTracksAndKeysTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    PinWrightRepointTests::FRepointFixture Fixture;
    if (!PinWrightRepointTests::MakeActorFixture(*this, Fixture)
        || !PinWrightRepointTests::AddTransformAndBoolTracks(*this, Fixture.Sequence,
            Fixture.BindingGuid))
    {
        return false;
    }

    const PinWrightRepointTests::FBindingStateSnapshot Before =
        PinWrightRepointTests::SnapshotBinding(Fixture.Sequence, Fixture.BindingGuid);
    FTestResponseCapture Capture;
    const bool bFound = PinWrightRepointTests::InvokeRepointHandler(
        PinWrightRepointTests::MakeRepointPayload(
            Fixture, Fixture.SourceLabel, Fixture.TargetLabel), Fixture.Subsystem, Capture);
    TestTrue(TEXT("sequencer.repoint_actor is registered and invoked"), bFound);
    if (!PinWrightRepointTests::AssertSuccessEnvelope(*this, Capture, Fixture, false, 0))
    {
        return false;
    }

    const PinWrightRepointTests::FBindingStateSnapshot After =
        PinWrightRepointTests::SnapshotBinding(Fixture.Sequence, Fixture.BindingGuid);
    TestTrue(TEXT("complete binding track, section, range, key-frame, and value snapshot is unchanged"),
        PinWrightRepointTests::BindingMetadataAndTrackSnapshotEqual(Before, After));
    TestTrue(TEXT("repoint replaces exactly one locator while preserving locator cardinality"),
        Before.LocatorCount == After.LocatorCount
        && Before.Locators.Num() == 1
        && After.Locators.Num() == 1
        && Before.Locators[0] != After.Locators[0]);
    FMovieScenePossessable* Possessable = Fixture.Sequence->GetMovieScene()->FindPossessable(
        Fixture.BindingGuid);
    TestNotNull(TEXT("repointed binding remains a possessable"), Possessable);
    if (Possessable)
    {
        TestTrue(TEXT("authored class metadata remains unchanged"),
            Possessable->GetPossessedObjectClass() == AStaticMeshActor::StaticClass());
    }
    PinWrightRepointTests::AssertSuccessfulResolution(*this, Fixture.Sequence,
        Fixture.BindingGuid, Fixture.SourceActor, Fixture.TargetActor);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerRepointEvaluationResolvesTargetActorTest,
    "PinWright.Sequencer.RepointActor.EvaluationResolvesTargetActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerRepointEvaluationResolvesTargetActorTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    PinWrightRepointTests::FRepointFixture Fixture;
    constexpr int32 EndFrame = 30;
    if (!PinWrightRepointTests::MakeActorFixture(*this, Fixture)
        || !PinWrightRepointTests::AddTransformRamp(*this, Fixture.Sequence,
            Fixture.BindingGuid, EndFrame, 250.0))
    {
        return false;
    }

    const FVector OldBefore = Fixture.SourceActor->GetActorLocation();
    FTestResponseCapture Capture;
    PinWrightRepointTests::InvokeRepointHandler(
        PinWrightRepointTests::MakeRepointPayload(
            Fixture, Fixture.SourceLabel, Fixture.TargetLabel), Fixture.Subsystem, Capture);
    if (!PinWrightRepointTests::AssertSuccessEnvelope(*this, Capture, Fixture, false, 0))
    {
        return false;
    }

    ALevelSequenceActor* PlayerActor = nullptr;
    ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(
        Fixture.World, Fixture.Sequence, FMovieSceneSequencePlaybackSettings(), PlayerActor);
    if (!TestNotNull(TEXT("a level sequence player was created for evaluation"), Player))
    {
        if (IsValid(PlayerActor))
        {
            PlayerActor->Destroy();
        }
        return false;
    }
    Player->Initialize(Fixture.Sequence, Fixture.World->PersistentLevel, FLevelSequenceCameraSettings());
    ON_SCOPE_EXIT
    {
        Player->Stop();
        if (IsValid(PlayerActor))
        {
            PlayerActor->Destroy();
        }
    };

    Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(
        FFrameTime(FFrameNumber(0)), EUpdatePositionMethod::Jump));
    const FVector TargetAtStart = Fixture.TargetActor->GetActorLocation();
    const FVector OldAtStart = Fixture.SourceActor->GetActorLocation();
    Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(
        FFrameTime(FFrameNumber(EndFrame)), EUpdatePositionMethod::Jump));
    const FVector TargetAtEnd = Fixture.TargetActor->GetActorLocation();
    const FVector OldAtEnd = Fixture.SourceActor->GetActorLocation();

    const double TargetDeltaX = TargetAtEnd.X - TargetAtStart.X;
    TestTrue(FString::Printf(
            TEXT("target actor moves by the authored transform ramp after repoint (start %.3f, end %.3f, delta %.3f)"),
            TargetAtStart.X, TargetAtEnd.X, TargetDeltaX),
        FMath::IsNearlyEqual(TargetDeltaX, 250.0, 1.0));
    TestTrue(TEXT("old actor remains unchanged during target evaluation"),
        OldAtStart.Equals(OldBefore, 0.5) && OldAtEnd.Equals(OldBefore, 0.5));
    PinWrightRepointTests::AssertSuccessfulResolution(*this, Fixture.Sequence,
        Fixture.BindingGuid, Fixture.SourceActor, Fixture.TargetActor);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerRepointClassMismatchWarnsTest,
    "PinWright.Sequencer.RepointActor.ClassMismatchWarnsAndPreservesMetadata",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerRepointClassMismatchWarnsTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    PinWrightRepointTests::FRepointFixture Fixture;
    if (!PinWrightRepointTests::MakeActorFixture(*this, Fixture, true)
        || !PinWrightRepointTests::AddTransformAndBoolTracks(*this, Fixture.Sequence,
            Fixture.BindingGuid))
    {
        return false;
    }

    const PinWrightRepointTests::FBindingStateSnapshot Before =
        PinWrightRepointTests::SnapshotBinding(Fixture.Sequence, Fixture.BindingGuid);
    FTestResponseCapture Capture;
    PinWrightRepointTests::InvokeRepointHandler(
        PinWrightRepointTests::MakeRepointPayload(
            Fixture, Fixture.SourceLabel, Fixture.TargetLabel), Fixture.Subsystem, Capture);
    if (!PinWrightRepointTests::AssertSuccessEnvelope(*this, Capture, Fixture, true, 1))
    {
        return false;
    }
    if (!Capture.Result.IsValid())
    {
        return false;
    }

    FString AuthoredClass;
    FString NewObjectClass;
    FString OldObjectPath;
    FString ResolvedObjectPath;
    Capture.Result->TryGetStringField(TEXT("authoredClass"), AuthoredClass);
    Capture.Result->TryGetStringField(TEXT("newObjectClass"), NewObjectClass);
    Capture.Result->TryGetStringField(TEXT("oldObjectPath"), OldObjectPath);
    Capture.Result->TryGetStringField(TEXT("resolvedObjectPath"), ResolvedObjectPath);
    TestEqual(TEXT("success envelope reports authored StaticMeshActor class"),
        AuthoredClass, AStaticMeshActor::StaticClass()->GetPathName());
    TestEqual(TEXT("success envelope reports new CameraActor class"),
        NewObjectClass, ACameraActor::StaticClass()->GetPathName());
    TestEqual(TEXT("success envelope reports old object path"),
        OldObjectPath, Fixture.SourceActor->GetPathName());
    TestEqual(TEXT("success envelope reports resolved target path"),
        ResolvedObjectPath, Fixture.TargetActor->GetPathName());

    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    TestTrue(TEXT("class mismatch warning array is present"),
        Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings
            && Warnings->Num() == 1);
    if (Warnings && Warnings->Num() == 1)
    {
        FString Warning;
        (*Warnings)[0]->TryGetString(Warning);
        TestTrue(TEXT("warning names both classes and metadata retention"),
            Warning.Contains(AStaticMeshActor::StaticClass()->GetPathName())
            && Warning.Contains(ACameraActor::StaticClass()->GetPathName())
            && Warning.Contains(TEXT("authored class metadata retained")));
    }

    const PinWrightRepointTests::FBindingStateSnapshot After =
        PinWrightRepointTests::SnapshotBinding(Fixture.Sequence, Fixture.BindingGuid);
    TestTrue(TEXT("class mismatch preserves the complete track and key snapshot"),
        PinWrightRepointTests::BindingMetadataAndTrackSnapshotEqual(Before, After));
    FMovieScenePossessable* Possessable = Fixture.Sequence->GetMovieScene()->FindPossessable(
        Fixture.BindingGuid);
    if (TestNotNull(TEXT("class mismatch leaves the possessable present"), Possessable))
    {
        TestTrue(TEXT("class mismatch preserves authored possessable class metadata"),
            Possessable->GetPossessedObjectClass() == AStaticMeshActor::StaticClass());
        TestEqual(TEXT("class mismatch preserves authored binding name"),
            Possessable->GetName(), Fixture.SourceLabel);
    }
    PinWrightRepointTests::AssertSuccessfulResolution(*this, Fixture.Sequence,
        Fixture.BindingGuid, Fixture.SourceActor, Fixture.TargetActor);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerRepointNonexistentActorNoMutationTest,
    "PinWright.Sequencer.RepointActor.NonexistentActorFailsWithoutMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerRepointNonexistentActorNoMutationTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    PinWrightRepointTests::FRepointFixture Fixture;
    if (!PinWrightRepointTests::MakeActorFixture(*this, Fixture)
        || !PinWrightRepointTests::AddTransformAndBoolTracks(*this, Fixture.Sequence,
            Fixture.BindingGuid))
    {
        return false;
    }

    const FString MissingOld = FString::Printf(TEXT("PWRepointMissingOld_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString MissingNew = FString::Printf(TEXT("PWRepointMissingNew_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    struct FFailureCase
    {
        FString Label;
        FString OldName;
        FString NewName;
        FString GuidString;
        FString ExpectedCode;
        FString ExpectedMessage;
        bool bExpectedReadback = false;
        FString ExpectedGuid;
        int32 ExpectedLocatorCount = 1;
    };
    const TArray<FFailureCase> Cases =
    {
        {
            TEXT("missing old actor"), MissingOld, Fixture.TargetLabel, FString(),
            ErrorCodes::ERR_ACTOR_NOT_FOUND,
            FString::Printf(TEXT("Old actor not found: %s"), *MissingOld), false,
            Fixture.BindingGuid.ToString(), 1
        },
        {
            TEXT("missing new actor"), Fixture.SourceLabel, MissingNew, FString(),
            ErrorCodes::ERR_ACTOR_NOT_FOUND,
            FString::Printf(TEXT("New actor not found: %s"), *MissingNew), false,
            Fixture.BindingGuid.ToString(), 1
        },
        {
            TEXT("malformed binding GUID"), Fixture.SourceLabel, Fixture.TargetLabel,
            TEXT("not-a-guid"), ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("bindingGuid is not a valid GUID: 'not-a-guid'"), false, FString(), 0
        },
        {
            TEXT("wrong old actor for a valid binding"), Fixture.TargetLabel, Fixture.SourceLabel,
            FString(), ErrorCodes::ERR_BINDING_UNRESOLVED,
            TEXT("binding does not currently resolve to the old actor"), true,
            Fixture.BindingGuid.ToString(), 1
        }
    };

    for (const FFailureCase& FailureCase : Cases)
    {
        const PinWrightRepointTests::FBindingStateSnapshot Before =
            PinWrightRepointTests::SnapshotBinding(Fixture.Sequence, Fixture.BindingGuid);
        const TArray<UObject*> ResolvedBefore =
            SequencerBindingUtils::ResolveBoundObjects(Fixture.Sequence, Fixture.BindingGuid);
        FTestResponseCapture Capture;
        const FString GuidString = FailureCase.GuidString.IsEmpty()
            ? Fixture.BindingGuid.ToString()
            : FailureCase.GuidString;
        const bool bFound = PinWrightRepointTests::InvokeRepointHandler(
            PinWrightRepointTests::MakeRepointPayload(Fixture, FailureCase.OldName,
                FailureCase.NewName, GuidString), Fixture.Subsystem, Capture);
        TestTrue(FString::Printf(TEXT("%s invokes the registered handler"), *FailureCase.Label), bFound);
        PinWrightRepointTests::AssertFailureDetails(*this, Capture,
            FailureCase.ExpectedCode, FailureCase.ExpectedMessage, FailureCase.ExpectedGuid,
            FailureCase.OldName, FailureCase.NewName, FailureCase.bExpectedReadback,
            FailureCase.ExpectedLocatorCount);

        const PinWrightRepointTests::FBindingStateSnapshot After =
            PinWrightRepointTests::SnapshotBinding(Fixture.Sequence, Fixture.BindingGuid);
        TestTrue(FString::Printf(TEXT("%s leaves binding tracks and keys unchanged"),
                *FailureCase.Label),
            PinWrightRepointTests::SnapshotEqual(Before, After));
        const TArray<UObject*> ResolvedAfter =
            SequencerBindingUtils::ResolveBoundObjects(Fixture.Sequence, Fixture.BindingGuid);
        TestTrue(FString::Printf(TEXT("%s leaves locator resolution unchanged"),
                *FailureCase.Label),
            PinWrightRepointTests::SameResolvedObjects(ResolvedBefore, ResolvedAfter));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerRepointUnsupportedBindingsNoMutationTest,
    "PinWright.Sequencer.RepointActor.UnsupportedBindingsAreRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerRepointUnsupportedBindingsNoMutationTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    const TArray<FString> Kinds = { TEXT("component"), TEXT("spawnable"), TEXT("custom"), TEXT("multi") };
#else
    // No "custom": this engine has no custom bindings to refuse. See the include guard.
    const TArray<FString> Kinds = { TEXT("component"), TEXT("spawnable"), TEXT("multi") };
#endif
    for (const FString& Kind : Kinds)
    {
        PinWrightRepointTests::FRepointFixture Fixture;
        if (!PinWrightRepointTests::PrepareUnsupportedFixture(*this, Fixture, Kind))
        {
            return false;
        }
        const PinWrightRepointTests::FBindingStateSnapshot Before =
            PinWrightRepointTests::SnapshotBinding(Fixture.Sequence, Fixture.BindingGuid);
        const TArray<UObject*> ResolvedBefore =
            SequencerBindingUtils::ResolveBoundObjects(Fixture.Sequence, Fixture.BindingGuid);
        FTestResponseCapture Capture;
        PinWrightRepointTests::InvokeRepointHandler(
            PinWrightRepointTests::MakeRepointPayload(
                Fixture, Fixture.SourceLabel, Fixture.TargetLabel), Fixture.Subsystem, Capture);
        PinWrightRepointTests::AssertFailureDetails(*this, Capture,
            ErrorCodes::ERR_UNSUPPORTED_OPERATION,
            Kind == TEXT("component")
                ? TEXT("sequencer.repoint_actor does not support parented or component possessables")
                : Kind == TEXT("spawnable")
                    ? TEXT("sequencer.repoint_actor supports possessable actor bindings only")
                    : Kind == TEXT("custom")
                        ? TEXT("sequencer.repoint_actor requires exactly one non-custom possessable locator")
                        : TEXT("sequencer.repoint_actor requires exactly one non-custom possessable locator"),
            Fixture.BindingGuid.ToString(), Fixture.SourceLabel, Fixture.TargetLabel, false,
            Before.LocatorCount);

        const PinWrightRepointTests::FBindingStateSnapshot After =
            PinWrightRepointTests::SnapshotBinding(Fixture.Sequence, Fixture.BindingGuid);
        TestTrue(FString::Printf(TEXT("%s refusal leaves hierarchy, locators, tracks, and keys unchanged"),
                *Kind),
            PinWrightRepointTests::SnapshotEqual(Before, After));
        const TArray<UObject*> ResolvedAfter =
            SequencerBindingUtils::ResolveBoundObjects(Fixture.Sequence, Fixture.BindingGuid);
        TestTrue(FString::Printf(TEXT("%s refusal leaves resolution unchanged"), *Kind),
            PinWrightRepointTests::SameResolvedObjects(ResolvedBefore, ResolvedAfter));
        if (Kind == TEXT("multi"))
        {
            TestEqual(TEXT("multi-locator fixture still has two locators after refusal"),
                After.LocatorCount, 2);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerRepointReadbackRollbackTest,
    "PinWright.Sequencer.RepointActor.PostWriteReadbackFailureRollsBack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerRepointReadbackRollbackTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    PinWrightRepointTests::FRepointFixture Fixture;
    if (!PinWrightRepointTests::MakeActorFixture(*this, Fixture)
        || !PinWrightRepointTests::AddTransformAndBoolTracks(*this, Fixture.Sequence,
            Fixture.BindingGuid))
    {
        return false;
    }
    const PinWrightRepointTests::FBindingStateSnapshot Before =
        PinWrightRepointTests::SnapshotBinding(Fixture.Sequence, Fixture.BindingGuid);

    const auto Readback = [](ULevelSequence* InSequence, const FGuid& InGuid,
        SequencerBindingUtils::ERepointReadbackPhase Phase) -> TArray<UObject*>
    {
        if (Phase == SequencerBindingUtils::ERepointReadbackPhase::AfterWrite)
        {
            return {};
        }
        return SequencerBindingUtils::ResolveBoundObjects(InSequence, InGuid);
    };
    const SequencerBindingUtils::FRepointOutcome Outcome =
        SequencerBindingUtils::RepointTopLevelPossessableActorForTest(
            Fixture.Sequence, Fixture.BindingGuid, Fixture.SourceActor, Fixture.TargetActor, Readback);

    TestFalse(TEXT("forced post-write readback failure reports failure"), Outcome.bSuccess);
    TestEqual(TEXT("forced post-write failure uses BINDING_UNRESOLVED"),
        Outcome.ErrorCode, FString(ErrorCodes::ERR_BINDING_UNRESOLVED));
    TestEqual(TEXT("forced post-write failure message is exact"), Outcome.ErrorMessage,
        FString(TEXT("post-write readback did not prove the target binding")));
    TestEqual(TEXT("forced post-write failure preserves the GUID"),
        Outcome.BindingGuid, Fixture.BindingGuid);
    TestEqual(TEXT("forced post-write failure preserves authored name"),
        Outcome.BindingName, Fixture.SourceLabel);
    TestEqual(TEXT("forced post-write failure preserves authored class"),
        Outcome.AuthoredClassPath, AStaticMeshActor::StaticClass()->GetPathName());
    TestEqual(TEXT("forced post-write failure records the new class"),
        Outcome.NewObjectClassPath, AStaticMeshActor::StaticClass()->GetPathName());
    TestEqual(TEXT("forced post-write failure records the old object path"),
        Outcome.OldObjectPath, Fixture.SourceActor->GetPathName());
    TestTrue(TEXT("forced post-write failure measured readback"), Outcome.bReadbackMeasured);
    TestEqual(TEXT("forced post-write failure phase is AfterWrite"),
        Outcome.FailureReadbackPhase, SequencerBindingUtils::ERepointReadbackPhase::AfterWrite);
    TestFalse(TEXT("forced post-write failure does not claim target resolution"),
        Outcome.bResolvesToNewActor);
    TestFalse(TEXT("forced post-write failure does not claim old object removal"),
        Outcome.bOldObjectUnbound);
    TestFalse(TEXT("exact-class rollback has no class mismatch warning"), Outcome.bClassMismatch);
    TestEqual(TEXT("exact-class rollback has no warnings"), Outcome.Warnings.Num(), 0);
    TestTrue(TEXT("forced post-write failure attempts rollback"), Outcome.bRollbackAttempted);
    TestTrue(TEXT("forced post-write failure proves rollback succeeded"), Outcome.bRollbackSucceeded);
    TestEqual(TEXT("rollback reports restored old actor"), Outcome.RestoredResolutionStatus,
        FString(TEXT("restored_old_actor")));
    TestEqual(TEXT("rollback reports the old actor path"), Outcome.RestoredResolvedObjectPath,
        Fixture.SourceActor->GetPathName());
    TestEqual(TEXT("rollback failure details report the original locator count"),
        Outcome.LocatorCount, 1);

    const PinWrightRepointTests::FBindingStateSnapshot After =
        PinWrightRepointTests::SnapshotBinding(Fixture.Sequence, Fixture.BindingGuid);
    TestTrue(TEXT("rollback restores the complete track and key snapshot"),
        PinWrightRepointTests::SnapshotEqual(Before, After));
    const TArray<UObject*> Resolved =
        SequencerBindingUtils::ResolveBoundObjects(Fixture.Sequence, Fixture.BindingGuid);
    TestTrue(TEXT("rollback resolves the old actor"), Resolved.Contains(Fixture.SourceActor));
    TestFalse(TEXT("rollback leaves the replacement actor absent"), Resolved.Contains(Fixture.TargetActor));

    // These assertions mirror every field emitted by BuildRepointFailureDetails: the test-only
    // seam cannot be reached through the handler without hiding the forced readback callback.
    TestEqual(TEXT("rollback failure keeps the default parent GUID empty"),
        Outcome.ParentGuid, FGuid());
    TestEqual(TEXT("rollback failure keeps the measured resolved path empty"),
        Outcome.ResolvedObjectPath, FString());
    return true;
}

#endif // UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
