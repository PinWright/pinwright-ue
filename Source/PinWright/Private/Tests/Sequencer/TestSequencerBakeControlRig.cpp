// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for F-sequencer-bake-controlrig: sequencer.bake_to_controlrig,
// sequencer.bake_control_space and sequencer.export_anim_sequence
// (Handlers/Sequencer/SequencerBakeHandler.cpp).
//
// THE DEFECT CLASS BEING GUARDED. A bake verb can call the engine, receive true, write
// nothing, and still report success. The headless-capable bake/export handlers therefore
// measure what landed. Space baking has a different boundary: its engine API requires an
// interactive Sequencer, so unattended coverage asserts typed editor refusal and exact
// no-mutation behavior instead of pretending a transient runtime hierarchy proves the bake.
//
// LOG ERRORS ARE SUPPRESSED, DELIBERATELY. UControlRigSequencerEditorLibrary::BakeToControlRig
// asks GetSequencerFromAsset() for an open Level Sequence editor before choosing its
// player, and that helper logs `LogControlRig Error: Can not open Sequencer for the
// LevelSequence None` whenever none is open - which is always, under automation. The bake
// then takes the transient-ULevelSequencePlayer path and succeeds. AddExpectedError cannot
// express "zero or more" before UE 5.6 (see TestRemoveTrackCameraCut.cpp), so the two
// headless tests use the suite's other established idiom, bSuppressLogErrors. The closed-
// editor space-bake tests expect a log-clean refusal and suppress nothing.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Compat/EngineVersionCompat.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"
#include "Sequencer/MovieSceneControlRigSpaceChannel.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "Utils/AssetUtils.h"

#include "ControlRig.h"
#include "ControlRigObjectBinding.h"
#include "Rigs/AdditiveControlRig.h"

namespace
{
    // UControlRig carries UCLASS(Abstract) through UE 5.7; 5.8 dropped the specifier. Allocating
    // the abstract base there yields a rig with no RigVM, which dereferences null on the first
    // evaluation, so these fixtures instantiate the concrete additive rig instead. It executes its
    // own units rather than a VM and creates no elements for a hierarchy with no skeletal mesh, so
    // every element these tests assert on is still the one they author by hand.
    UClass* FixtureControlRigClass()
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        return UControlRig::StaticClass();
#else
        return UAdditiveControlRig::StaticClass();
#endif
    }
}

namespace
{
    // Distinctly named (BakeCR* prefix) so a unity build merging this TU with its sibling
    // sequencer tests cannot collide on an anonymous-namespace symbol - the same reason
    // TestSequencerControlRigTrack.cpp names its fixture CreateControlRigTrackSequence.
    // Creates a real /Game LevelSequence through the registered sequencer.create handler so
    // the bake handlers' asset load resolves it. Empty path + nullptr on failure.
    ULevelSequence* BakeCRCreateProbeSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_CRBakeSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_CRBakeProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

        if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
            !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            Test.AddInfo(TEXT("FIXTURE-SKIP: could not create a probe sequence (factory unavailable "
                              "in this host); the registration assertions above still stand"));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // Read a number field, reporting a hard failure when it is absent. A missing measured
    // field is a contract regression, not a zero.
    double BakeCRRequireNumber(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Result,
        const TCHAR* Field)
    {
        double Value = 0.0;
        if (!Result.IsValid() || !Result->TryGetNumberField(Field, Value))
        {
            Test.AddError(FString::Printf(
                TEXT("PINWRIGHT-BAKE-FIELD-MISSING: response carries no numeric '%s'; the measured "
                     "report contract changed and the assertion below is unreachable."), Field));
        }
        return Value;
    }

    const TArray<FString>& BakeCRMannequinCandidates()
    {
        static const TArray<FString> Candidates = {
            TEXT("/Game/Characters/Heroes/Mannequin/Meshes/SKM_Manny.SKM_Manny"),
            TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny"),
            TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"),
            TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin"),
        };
        return Candidates;
    }

    struct FBakeCRFloatChannelState
    {
        TArray<FFrameNumber> Times;
        TArray<FMovieSceneFloatValue> Values;
    };

    struct FBakeCRControlChannelState
    {
        TArray<FBakeCRFloatChannelState> TransformChannels;
        bool bHasSpaceChannel = false;
        TArray<FFrameNumber> SpaceTimes;
        TArray<FMovieSceneControlRigSpaceBaseKey> SpaceValues;
    };

    FBakeCRControlChannelState BakeCRCaptureControlChannelState(
        UMovieSceneControlRigParameterSection* Section,
        FName ControlName)
    {
        FBakeCRControlChannelState State;
        if (!Section)
        {
            return State;
        }

        for (FMovieSceneFloatChannel* Channel :
                 Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>())
        {
            FBakeCRFloatChannelState& ChannelState = State.TransformChannels.AddDefaulted_GetRef();
            const auto Data = static_cast<const FMovieSceneFloatChannel*>(Channel)->GetData();
            const TArrayView<const FFrameNumber> Times = Data.GetTimes();
            const TArrayView<const FMovieSceneFloatValue> Values = Data.GetValues();
            ChannelState.Times.Append(Times.GetData(), Times.Num());
            ChannelState.Values.Append(Values.GetData(), Values.Num());
        }

        if (const FSpaceControlNameAndChannel* Space = Section->GetSpaceChannel(ControlName))
        {
            State.bHasSpaceChannel = true;
            const auto Data = static_cast<const FMovieSceneControlRigSpaceChannel&>(
                Space->SpaceCurve).GetData();
            const TArrayView<const FFrameNumber> Times = Data.GetTimes();
            const TArrayView<const FMovieSceneControlRigSpaceBaseKey> Values = Data.GetValues();
            State.SpaceTimes.Append(Times.GetData(), Times.Num());
            State.SpaceValues.Append(Values.GetData(), Values.Num());
        }
        return State;
    }

    bool BakeCRControlChannelStatesEqual(
        const FBakeCRControlChannelState& A,
        const FBakeCRControlChannelState& B)
    {
        if (A.bHasSpaceChannel != B.bHasSpaceChannel ||
            A.SpaceTimes != B.SpaceTimes || A.SpaceValues != B.SpaceValues ||
            A.TransformChannels.Num() != B.TransformChannels.Num())
        {
            return false;
        }
        for (int32 Index = 0; Index < A.TransformChannels.Num(); ++Index)
        {
            const FBakeCRFloatChannelState& AChannel = A.TransformChannels[Index];
            const FBakeCRFloatChannelState& BChannel = B.TransformChannels[Index];
            if (AChannel.Times != BChannel.Times ||
                AChannel.Values.Num() != BChannel.Values.Num())
            {
                return false;
            }
            for (int32 ValueIndex = 0; ValueIndex < AChannel.Values.Num(); ++ValueIndex)
            {
                const FMovieSceneFloatValue& AValue = AChannel.Values[ValueIndex];
                const FMovieSceneFloatValue& BValue = BChannel.Values[ValueIndex];
                if (AValue.Value != BValue.Value ||
                    AValue.Tangent.ArriveTangent != BValue.Tangent.ArriveTangent ||
                    AValue.Tangent.LeaveTangent != BValue.Tangent.LeaveTangent ||
                    AValue.Tangent.ArriveTangentWeight != BValue.Tangent.ArriveTangentWeight ||
                    AValue.Tangent.LeaveTangentWeight != BValue.Tangent.LeaveTangentWeight ||
                    AValue.Tangent.TangentWeightMode != BValue.Tangent.TangentWeightMode ||
                    AValue.InterpMode != BValue.InterpMode ||
                    AValue.TangentMode != BValue.TangentMode)
                {
                    return false;
                }
            }
        }
        return true;
    }

}

// ---------------------------------------------------------------------------
// Acceptance: AnimSequence <- CR track <- edited key <- exported AnimSequence.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerBakeControlRigRoundTripTest,
    "PinWright.Sequencer.ControlRigBake.AnimSequenceRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerBakeControlRigRoundTripTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    // See the file header: the engine's headless bake logs one benign LogControlRig error.
    bSuppressLogErrors = true;

    // Host-independent half of the capability assertion: both bake verbs must be registered.
    TestTrue(TEXT("sequencer.bake_to_controlrig handler registered"),
        IsHandlerRegistered(TEXT("sequencer.bake_to_controlrig")));
    TestTrue(TEXT("sequencer.export_anim_sequence handler registered"),
        IsHandlerRegistered(TEXT("sequencer.export_anim_sequence")));

    // The round trip evaluates a live skeletal mesh, which many hosts do not ship. Gated
    // before any asset is created so a skip leaves nothing behind.
    PINWRIGHT_SKIP_IF_ALL_FIXTURES_MISSING(BakeCRMannequinCandidates());

    FString SeqPath;
    ULevelSequence* Sequence = BakeCRCreateProbeSequence(*this, SeqPath);
    if (!Sequence)
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        return true;
    }

    // Pin rates and a short playback range BEFORE anything is authored, so the key counts
    // below are host-config independent: 30 fps display over 24000 ticks, 0..30 display
    // frames = one second of animation to bake.
    MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
    MovieScene->SetDisplayRate(FFrameRate(30, 1));
    const FFrameNumber RangeStartTick = FFrameRate::TransformTime(FFrameTime(FFrameNumber(0)),
        MovieScene->GetDisplayRate(), MovieScene->GetTickResolution()).RoundToFrame();
    const FFrameNumber RangeEndTick = FFrameRate::TransformTime(FFrameTime(FFrameNumber(30)),
        MovieScene->GetDisplayRate(), MovieScene->GetTickResolution()).RoundToFrame();
    MovieScene->SetPlaybackRange(TRange<FFrameNumber>(RangeStartTick, RangeEndTick));

    USkeletalMesh* MannyMesh = nullptr;
    for (const FString& Candidate : BakeCRMannequinCandidates())
    {
        if (UEditorAssetLibrary::DoesAssetExist(Candidate))
        {
            MannyMesh = Cast<USkeletalMesh>(UEditorAssetLibrary::LoadAsset(Candidate));
            if (MannyMesh)
            {
                break;
            }
        }
    }
    // A candidate package that exists but will not load is a real regression, not a host
    // difference, so this is a hard failure rather than a second skip.
    if (!TestNotNull(TEXT("mannequin fixture skeletal mesh loaded"), MannyMesh))
    {
        return true;
    }
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("editor world present"), EditorWorld))
    {
        return true;
    }

    const FString MannyLabel = FString::Printf(TEXT("MCP_CRBakeManny_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ASkeletalMeshActor* MannyActor = SpawnActorInActiveWorld<ASkeletalMeshActor>(
        ASkeletalMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, MannyLabel);
    if (!TestNotNull(TEXT("skeletal mesh actor spawned"), MannyActor))
    {
        return true;
    }
    MannyActor->GetSkeletalMeshComponent()->SetSkeletalMeshAsset(MannyMesh);

    const FGuid BindingGuid = MovieScene->AddPossessable(MannyLabel, ASkeletalMeshActor::StaticClass());
    Sequence->BindPossessableObject(BindingGuid, *MannyActor, EditorWorld);
    const FString BindingId = BindingGuid.ToString(EGuidFormats::Digits);
    if (!TestTrue(TEXT("skeletal binding GUID is valid"), BindingGuid.IsValid()))
    {
        return true;
    }

    // --- bake_to_controlrig: evaluated animation -> editable CR track -----------------
    TSharedPtr<FJsonObject> BakePayload = MakeShared<FJsonObject>();
    BakePayload->SetStringField(TEXT("sequence"), SeqPath);
    BakePayload->SetStringField(TEXT("binding"), BindingId);
    FTestResponseCapture BakeCapture;
    TestTrue(TEXT("sequencer.bake_to_controlrig invoked (handler present)"),
        InvokeHandlerWithCapture(TEXT("sequencer.bake_to_controlrig"), BakePayload, BakeCapture));
    if (!TestTrue(TEXT("bake_to_controlrig reported success"), BakeCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("bake_to_controlrig failed: %s - %s"),
            *BakeCapture.ErrorCode, *BakeCapture.Message));
        return true;
    }

    // The measurement, not the call: a bake that wrote no keys must never read as success.
    const double KeysWritten = BakeCRRequireNumber(*this, BakeCapture.Result, TEXT("keysWritten"));
    const double ChannelsWithKeys = BakeCRRequireNumber(*this, BakeCapture.Result, TEXT("channelsWithKeys"));
    const double ControlCount = BakeCRRequireNumber(*this, BakeCapture.Result, TEXT("controlCount"));
    TestTrue(TEXT("bake wrote at least one key to the Control Rig section"), KeysWritten > 0.0);
    TestTrue(TEXT("bake keyed at least one channel"), ChannelsWithKeys > 0.0);
    TestTrue(TEXT("baked rig exposes controls"), ControlCount > 0.0);
    TestTrue(TEXT("bake reports the frame span its keys actually cover"),
        BakeCapture.Result.IsValid() && BakeCapture.Result->HasField(TEXT("keyRange")));

    // Independent confirmation that the track exists on the binding, read off the MovieScene
    // rather than off the response the handler wrote.
    TestTrue(TEXT("a Control Rig track is present on the binding after the bake"),
        MovieScene->FindTracks(UMovieSceneControlRigParameterTrack::StaticClass(),
            BindingGuid, NAME_None).Num() > 0);

    // --- the baked track is EDITABLE: key a control and read it back ------------------
    // This is the middle of the ticket's acceptance round trip. It routes through the
    // existing CR-track verbs, which is the point: a bake that produces a track those verbs
    // cannot drive has not closed the loop.
    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("sequence"), SeqPath);
    ListPayload->SetStringField(TEXT("binding"), BindingId);
    FTestResponseCapture ListCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.list_controls"), ListPayload, ListCapture);
    FString ControlName;
    if (ListCapture.bSuccess && ListCapture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Controls = nullptr;
        if (ListCapture.Result->TryGetArrayField(TEXT("controls"), Controls) && Controls && Controls->Num() > 0)
        {
            const TSharedPtr<FJsonObject>* AsObj = nullptr;
            if ((*Controls)[0].IsValid() && (*Controls)[0]->TryGetObject(AsObj) && AsObj && (*AsObj).IsValid())
            {
                (*AsObj)->TryGetStringField(TEXT("name"), ControlName);
            }
        }
    }
    if (TestFalse(TEXT("list_controls names a control on the baked track"), ControlName.IsEmpty()))
    {
        TSharedPtr<FJsonObject> ControlValues = MakeShared<FJsonObject>();
        ControlValues->SetNumberField(ControlName, 7.5);
        TSharedPtr<FJsonObject> KeyPayload = MakeShared<FJsonObject>();
        KeyPayload->SetStringField(TEXT("sequence"), SeqPath);
        KeyPayload->SetStringField(TEXT("binding"), BindingId);
        KeyPayload->SetNumberField(TEXT("frame"), 10);
        KeyPayload->SetObjectField(TEXT("controls"), ControlValues);
        FTestResponseCapture KeyCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.key_controls"), KeyPayload, KeyCapture);
        TestTrue(TEXT("a control on the baked track accepts a key"), KeyCapture.bSuccess);

        TSharedPtr<FJsonObject> ReadPayload = MakeShared<FJsonObject>();
        ReadPayload->SetStringField(TEXT("sequence"), SeqPath);
        ReadPayload->SetStringField(TEXT("binding"), BindingId);
        ReadPayload->SetStringField(TEXT("control"), ControlName);
        ReadPayload->SetNumberField(TEXT("frame"), 10);
        FTestResponseCapture ReadCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.get_control_value"), ReadPayload, ReadCapture);
        if (TestTrue(TEXT("the edited control reads back"),
                ReadCapture.bSuccess && ReadCapture.Result.IsValid()))
        {
            double ReadValue = 0.0;
            ReadCapture.Result->TryGetNumberField(TEXT("value"), ReadValue);
            TestTrue(FString::Printf(TEXT("edited key survives on the baked track (read %g, wrote 7.5)"), ReadValue),
                FMath::IsNearlyEqual(ReadValue, 7.5, 0.01));
        }
    }

    // --- export_anim_sequence: evaluated performance -> AnimSequence asset ------------
    const FString ExportPath = FString::Printf(TEXT("/Game/MCP_CRBakeProbe/MCP_CRBakeAnim_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ON_SCOPE_EXIT { CleanupTestAsset(ExportPath); };

    TSharedPtr<FJsonObject> ExportPayload = MakeShared<FJsonObject>();
    ExportPayload->SetStringField(TEXT("sequence"), SeqPath);
    ExportPayload->SetStringField(TEXT("binding"), BindingId);
    ExportPayload->SetStringField(TEXT("outAssetPath"), ExportPath);
    ExportPayload->SetBoolField(TEXT("save"), false);
    FTestResponseCapture ExportCapture;
    TestTrue(TEXT("sequencer.export_anim_sequence invoked (handler present)"),
        InvokeHandlerWithCapture(TEXT("sequencer.export_anim_sequence"), ExportPayload, ExportCapture));
    if (!TestTrue(TEXT("export_anim_sequence reported success"), ExportCapture.bSuccess))
    {
        AddError(FString::Printf(TEXT("export_anim_sequence failed: %s - %s"),
            *ExportCapture.ErrorCode, *ExportCapture.Message));
        return true;
    }

    const double BoneTrackCount = BakeCRRequireNumber(*this, ExportCapture.Result, TEXT("boneTrackCount"));
    const double BoneKeysWritten = BakeCRRequireNumber(*this, ExportCapture.Result, TEXT("boneKeysWritten"));
    TestTrue(TEXT("export wrote at least one bone track"), BoneTrackCount > 0.0);
    TestTrue(TEXT("export wrote at least one bone key"), BoneKeysWritten > 0.0);
    bool bReportedCreated = false;
    if (ExportCapture.Result.IsValid())
    {
        ExportCapture.Result->TryGetBoolField(TEXT("created"), bReportedCreated);
    }
    TestTrue(TEXT("export reports it created the asset"), bReportedCreated);
    // The asset the response names must exist; `created:true` on a path with no asset is the
    // same class of lie the key counts guard against.
    TestTrue(TEXT("the exported AnimSequence asset exists at the reported path"),
        UEditorAssetLibrary::DoesAssetExist(ExportPath));

    // A second export to the same path without overwrite must be refused rather than
    // silently rewriting an asset the caller did not ask to replace.
    FTestResponseCapture ReExportCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.export_anim_sequence"), ExportPayload, ReExportCapture);
    TestFalse(TEXT("a second export without overwrite is refused"), ReExportCapture.bSuccess);
    TestEqual(TEXT("the refusal is ASSET_ALREADY_EXISTS"),
        ReExportCapture.ErrorCode, FString(TEXT("ASSET_ALREADY_EXISTS")));

    return true;
}

// ---------------------------------------------------------------------------
// A non-skeletal binding is refused, and refused BEFORE anything is mutated.
// Host-independent: needs no mannequin content.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerBakeControlRigRefusesNonSkeletalTest,
    "PinWright.Sequencer.ControlRigBake.RefusesNonSkeletalBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerBakeControlRigRefusesNonSkeletalTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    bSuppressLogErrors = true;

    FString SeqPath;
    ULevelSequence* Sequence = BakeCRCreateProbeSequence(*this, SeqPath);
    if (!Sequence)
    {
        return true;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        return true;
    }

    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("editor world present"), EditorWorld))
    {
        return true;
    }
    const FString PropLabel = FString::Printf(TEXT("MCP_CRBakeProp_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* PropActor = SpawnActorInActiveWorld<AStaticMeshActor>(
        AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, PropLabel);
    if (!TestNotNull(TEXT("static mesh actor spawned"), PropActor))
    {
        return true;
    }

    const FGuid BindingGuid = MovieScene->AddPossessable(PropLabel, AStaticMeshActor::StaticClass());
    Sequence->BindPossessableObject(BindingGuid, *PropActor, EditorWorld);
    const FString BindingId = BindingGuid.ToString(EGuidFormats::Digits);

    TSharedPtr<FJsonObject> BakePayload = MakeShared<FJsonObject>();
    BakePayload->SetStringField(TEXT("sequence"), SeqPath);
    BakePayload->SetStringField(TEXT("binding"), BindingId);
    FTestResponseCapture BakeCapture;
    TestTrue(TEXT("sequencer.bake_to_controlrig invoked (handler present)"),
        InvokeHandlerWithCapture(TEXT("sequencer.bake_to_controlrig"), BakePayload, BakeCapture));
    TestFalse(TEXT("baking a non-skeletal binding is refused"), BakeCapture.bSuccess);
    TestEqual(TEXT("the refusal is BINDING_NOT_SKELETAL"),
        BakeCapture.ErrorCode, FString(TEXT("BINDING_NOT_SKELETAL")));
    // Refused BEFORE the engine bake ran: no Control Rig track was left on the binding, and
    // the engine's own bake would have removed existing tracks before failing.
    TestEqual(TEXT("no Control Rig track was created by the refused bake"),
        MovieScene->FindTracks(UMovieSceneControlRigParameterTrack::StaticClass(),
            BindingGuid, NAME_None).Num(), 0);

    // The export verb shares the same precondition and must refuse identically, without
    // creating the destination asset.
    const FString ExportPath = FString::Printf(TEXT("/Game/MCP_CRBakeProbe/MCP_CRBakeNoAnim_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedPtr<FJsonObject> ExportPayload = MakeShared<FJsonObject>();
    ExportPayload->SetStringField(TEXT("sequence"), SeqPath);
    ExportPayload->SetStringField(TEXT("binding"), BindingId);
    ExportPayload->SetStringField(TEXT("outAssetPath"), ExportPath);
    FTestResponseCapture ExportCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.export_anim_sequence"), ExportPayload, ExportCapture);
    TestFalse(TEXT("exporting a non-skeletal binding is refused"), ExportCapture.bSuccess);
    TestEqual(TEXT("the export refusal is BINDING_NOT_SKELETAL"),
        ExportCapture.ErrorCode, FString(TEXT("BINDING_NOT_SKELETAL")));
    TestFalse(TEXT("the refused export created no destination asset"),
        UEditorAssetLibrary::DoesAssetExist(ExportPath));

    return true;
}

// ---------------------------------------------------------------------------
// Space baking is intentionally editor-gated. The engine entry point only operates on
// the currently open Sequencer; accepting a closed asset would otherwise report a
// plausible result without evaluating or writing the requested control.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerBakeControlSpaceRequiresOpenEditorTest,
    "PinWright.Sequencer.ControlRigBake.SpaceRequiresOpenSequencer",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerBakeControlSpaceRequiresOpenEditorTest::RunTest(const FString& Parameters)
{
    const FParamSpec* TargetTypeSpec = GetRegisteredParamSpec(
        TEXT("sequencer.bake_control_space"), TEXT("targetSpaceType"));
    if (!TestNotNull(TEXT("space bake declares targetSpaceType"), TargetTypeSpec))
    {
        return false;
    }
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    TestFalse(TEXT("UE 5.3 schema does not advertise unavailable Socket elements"),
        TargetTypeSpec->Description.Contains(TEXT("Socket")));
#else
    TestTrue(TEXT("UE 5.4+ schema advertises supported Socket elements"),
        TargetTypeSpec->Description.Contains(TEXT("Socket")));
#endif
    const FParamSpec* FocusOptInSpec = GetRegisteredParamSpec(
        TEXT("sequencer.bake_control_space"), TEXT("allowFocusedSequenceMismatch"));
    if (!TestNotNull(TEXT("space bake declares focused-sequence opt-in"), FocusOptInSpec))
    {
        return false;
    }
    TestFalse(TEXT("focused-sequence mismatch opt-in is optional"),
        FocusOptInSpec->bRequired);

    TSharedPtr<FJsonObject> BakePayload = MakeShared<FJsonObject>();
    BakePayload->SetStringField(TEXT("control"), TEXT("MissingControl"));
    BakePayload->SetStringField(TEXT("targetSpaceName"), TEXT("WorldSpace"));
    BakePayload->SetStringField(TEXT("targetSpaceType"), TEXT("Reference"));
    BakePayload->SetNumberField(TEXT("startFrame"), 0);
    BakePayload->SetNumberField(TEXT("endFrame"), 10);

    FString SeqPath;
    ULevelSequence* Sequence = BakeCRCreateProbeSequence(*this, SeqPath);
    if (!TestNotNull(TEXT("probe sequence created"), Sequence))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        if (GEditor)
        {
            if (UAssetEditorSubsystem* AssetEditors =
                    GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                AssetEditors->CloseAllEditorsForAsset(Sequence);
            }
        }
        CleanupTestAsset(SeqPath);
    };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene))
    {
        return false;
    }

    const FGuid BindingGuid = MovieScene->AddPossessable(
        TEXT("SpaceBakeProbe"), UObject::StaticClass());
    UMovieSceneControlRigParameterTrack* Track =
        MovieScene->AddTrack<UMovieSceneControlRigParameterTrack>(BindingGuid);
    if (!TestNotNull(TEXT("Control Rig track created"), Track))
    {
        return false;
    }

    UControlRig* Rig = NewObject<UControlRig>(Track, FixtureControlRigClass(),
        NAME_None, RF_Transactional);
    if (!TestNotNull(TEXT("Control Rig instance created"), Rig))
    {
        return false;
    }
    Rig->Initialize();
    UMovieSceneControlRigParameterSection* Section = Cast<UMovieSceneControlRigParameterSection>(
        Track->CreateControlRigSection(0, Rig, /*bOwnsControlRig=*/true));
    if (!TestNotNull(TEXT("Control Rig section created"), Section))
    {
        return false;
    }

    UAssetEditorSubsystem* AssetEditors = GEditor
        ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("asset editor subsystem present"), AssetEditors))
    {
        return false;
    }
    AssetEditors->CloseAllEditorsForAsset(Sequence);
    TestTrue(TEXT("fixture sequence is not open in an asset editor"),
        AssetEditors->FindEditorForAsset(Sequence, /*bFocusIfOpen=*/false) == nullptr);

    const int32 SpaceChannelsBefore = Section->GetSpaceChannels().Num();
    BakePayload->SetStringField(TEXT("sequence"), SeqPath);
    BakePayload->SetStringField(TEXT("binding"),
        BindingGuid.ToString(EGuidFormats::Digits));

    FTestResponseCapture BakeCapture;
    TestTrue(TEXT("sequencer.bake_control_space invoked (handler present)"),
        InvokeHandlerWithCapture(TEXT("sequencer.bake_control_space"), BakePayload, BakeCapture));
    TestFalse(TEXT("space bake is refused while the sequence editor is closed"),
        BakeCapture.bSuccess);
    TestEqual(TEXT("the refusal is EDITOR_NOT_OPEN"),
        BakeCapture.ErrorCode, FString(TEXT("EDITOR_NOT_OPEN")));
    TestEqual(TEXT("the refused bake did not create a space channel"),
        Section->GetSpaceChannels().Num(), SpaceChannelsBefore);

    return true;
}

// ---------------------------------------------------------------------------
// A valid transform-control section must be refused before mutation when its Level Sequence
// is not open. The live engine bake and world-space invariant require an interactive,
// asset-backed Control Rig and are intentionally outside this unattended fixture.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerBakeControlSpaceNoEditorNoMutationTest,
    "PinWright.Sequencer.ControlRigBake.SpaceNoEditorNoMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerBakeControlSpaceNoEditorNoMutationTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;

    TestTrue(TEXT("sequencer.bake_control_space handler registered"),
        IsHandlerRegistered(TEXT("sequencer.bake_control_space")));

    FString SeqPath;
    ULevelSequence* Sequence = BakeCRCreateProbeSequence(*this, SeqPath);
    if (!TestNotNull(TEXT("probe sequence created"), Sequence))
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("MovieScene present"), MovieScene) ||
        !TestNotNull(TEXT("editor world present"), EditorWorld))
    {
        return false;
    }
    MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
    MovieScene->SetDisplayRate(FFrameRate(30, 1));
    const FFrameNumber StartTick = FFrameRate::TransformTime(FFrameTime(FFrameNumber(0)),
        MovieScene->GetDisplayRate(), MovieScene->GetTickResolution()).RoundToFrame();
    const FFrameNumber EndTick = FFrameRate::TransformTime(FFrameTime(FFrameNumber(10)),
        MovieScene->GetDisplayRate(), MovieScene->GetTickResolution()).RoundToFrame();
    MovieScene->SetPlaybackRange(TRange<FFrameNumber>(StartTick, EndTick));

    const FString ActorLabel = FString::Printf(TEXT("MCP_CRSpaceActor_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* BoundActor = SpawnActorInActiveWorld<AStaticMeshActor>(
        AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, ActorLabel);
    if (!TestNotNull(TEXT("space-bake binding actor spawned"), BoundActor))
    {
        return false;
    }
    const FGuid BindingGuid = MovieScene->AddPossessable(ActorLabel, AStaticMeshActor::StaticClass());
    Sequence->BindPossessableObject(BindingGuid, *BoundActor, EditorWorld);
    if (!TestTrue(TEXT("space-bake binding GUID is valid"), BindingGuid.IsValid()))
    {
        return false;
    }
    const FString BindingId = BindingGuid.ToString(EGuidFormats::Digits);

    UMovieSceneControlRigParameterTrack* Track =
        MovieScene->AddTrack<UMovieSceneControlRigParameterTrack>(BindingGuid);
    UControlRig* Rig = Track
        ? NewObject<UControlRig>(Track, FixtureControlRigClass(), NAME_None, RF_Transactional)
        : nullptr;
    if (!TestNotNull(TEXT("Control Rig track created"), Track) ||
        !TestNotNull(TEXT("Control Rig instance created"), Rig))
    {
        return false;
    }
    Rig->SetObjectBinding(MakeShared<FControlRigObjectBinding>());
    Rig->GetObjectBinding()->BindToObject(BoundActor->GetStaticMeshComponent());
    Rig->Initialize();
    // Finish the base rig's pending construction before adding the transient test hierarchy.
    // A later RequestInit/Initialize would copy the empty UControlRig CDO hierarchy back over
    // these runtime elements, leaving the section with no controls to reconstruct.
    Rig->Evaluate_AnyThread();
    URigHierarchy* Hierarchy = Rig->GetHierarchy();
    URigHierarchyController* Controller = Hierarchy ? Hierarchy->GetController(true) : nullptr;
    if (!TestNotNull(TEXT("Control Rig hierarchy present"), Hierarchy) ||
        !TestNotNull(TEXT("Control Rig hierarchy controller present"), Controller))
    {
        return false;
    }

    const FTransform ParentTransform(FVector(100.0, 25.0, 0.0));
    const FRigElementKey ParentKey = Controller->AddNull(
        TEXT("MovingParent"), FRigElementKey(),
        ParentTransform,
        /*bTransformInGlobal=*/true, /*bSetupUndo=*/false, /*bPrintPythonCommand=*/false);
    FRigControlSettings ControlSettings;
    ControlSettings.AnimationType = ERigControlAnimationType::AnimationControl;
    ControlSettings.ControlType = ERigControlType::EulerTransform;
    const FName ControlName(TEXT("SpaceControl"));
    FRigElementKey ControlKey = Controller->AddControl(
        ControlName, ParentKey, ControlSettings,
        FRigControlValue::Make<FRigControlValue::FEulerTransform_Float>(
            FEulerTransform::Identity),
        FTransform::Identity, FTransform::Identity,
        /*bSetupUndo=*/false, /*bPrintPythonCommand=*/false);
    if (!TestTrue(TEXT("parent element created"), ParentKey.IsValid()) ||
        !TestTrue(TEXT("transform control created"), ControlKey.IsValid()))
    {
        return false;
    }
    // Establish both initial and current transform caches through the hierarchy API before
    // the section adopts this dynamically authored rig.
    Hierarchy->SetInitialGlobalTransform(ParentKey, ParentTransform,
        /*bAffectChildren=*/true, /*bSetupUndo=*/false);
    Hierarchy->SetGlobalTransform(ParentKey, ParentTransform,
        /*bInitial=*/false, /*bAffectChildren=*/true,
        /*bSetupUndo=*/false, /*bPrintPythonCommand=*/false);
    Hierarchy->SetInitialLocalTransform(ControlKey, FTransform::Identity,
        /*bAffectChildren=*/true, /*bSetupUndo=*/false, /*bPrintPythonCommands=*/false);
    Hierarchy->SetLocalTransform(ControlKey, FTransform::Identity,
        /*bInitial=*/false, /*bAffectChildren=*/true,
        /*bSetupUndo=*/false, /*bPrintPythonCommands=*/false);
    Hierarchy->GetInitialGlobalTransform(ControlKey);
    Hierarchy->GetGlobalTransform(ControlKey);
    const FRigControlElement* FixtureControl = Rig->FindControl(ControlName);
    if (!TestNotNull(TEXT("transform control remains present before section creation"), FixtureControl))
    {
        return false;
    }
    TestTrue(TEXT("transform control starts parented to the non-world space"),
        Hierarchy->GetFirstParent(ControlKey) == ParentKey);
    TestEqual(TEXT("fixture control remains an Euler transform"),
        static_cast<int32>(FixtureControl->Settings.ControlType),
        static_cast<int32>(ERigControlType::EulerTransform));
    TestTrue(TEXT("fixture control is animatable"), Hierarchy->IsAnimatable(FixtureControl));

    UMovieSceneControlRigParameterSection* Section =
        Cast<UMovieSceneControlRigParameterSection>(
            Track->CreateControlRigSection(0, Rig, /*bOwnsControlRig=*/true));
    if (!TestNotNull(TEXT("Control Rig section created"), Section))
    {
        return false;
    }
    // Force the public Control Rig section rebuild after the transient hierarchy is complete.
    // This is the same reconstruction path used when a rig's controls change in the editor.
    Section->RecreateWithThisControlRig(Rig, /*bSetDefault=*/true);
    Section->ReconstructChannelProxy();
    Rig = Section->GetControlRig();
    Hierarchy = Rig ? Rig->GetHierarchy() : nullptr;
    FixtureControl = Rig ? Rig->FindControl(ControlName) : nullptr;
    if (!TestNotNull(TEXT("section retains its current Control Rig"), Rig) ||
        !TestNotNull(TEXT("section rig retains its hierarchy"), Hierarchy) ||
        !TestNotNull(TEXT("section rig retains the transform control"), FixtureControl))
    {
        return false;
    }
    TestEqual(TEXT("fixture starts without a space channel"),
        Section->GetSpaceChannels().Num(), 0);
    TArrayView<FMovieSceneFloatChannel*> FloatChannels =
        Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
    if (!TestTrue(TEXT("fixture exposes transform float channels"), FloatChannels.Num() >= 9))
    {
        return false;
    }
    FMovieSceneFloatValue SeedValue;
    SeedValue.Value = 3.0f;
    FloatChannels[0]->GetData().AddKey(FFrameNumber(0), SeedValue);

    UAssetEditorSubsystem* AssetEditors = GEditor
        ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    if (!TestNotNull(TEXT("asset editor subsystem present"), AssetEditors))
    {
        return false;
    }
    AssetEditors->CloseAllEditorsForAsset(Sequence);
    TestNull(TEXT("valid fixture sequence is closed before the request"),
        AssetEditors->FindEditorForAsset(Sequence, /*bFocusIfOpen=*/false));

    const FBakeCRControlChannelState StateBefore =
        BakeCRCaptureControlChannelState(Section, ControlName);
    Sequence->GetOutermost()->SetDirtyFlag(false);

    TSharedPtr<FJsonObject> BakePayload = MakeShared<FJsonObject>();
    BakePayload->SetStringField(TEXT("sequence"), SeqPath);
    BakePayload->SetStringField(TEXT("binding"), BindingId);
    BakePayload->SetStringField(TEXT("control"), ControlName.ToString());
    BakePayload->SetStringField(TEXT("targetSpaceName"), TEXT("DefaultParent"));
    BakePayload->SetStringField(TEXT("targetSpaceType"), TEXT("Reference"));
    BakePayload->SetNumberField(TEXT("startFrame"), 0);
    BakePayload->SetNumberField(TEXT("endFrame"), 10);

    FTestResponseCapture BakeCapture;
    TestTrue(TEXT("valid closed-editor space bake request invoked"),
        InvokeHandlerWithCapture(TEXT("sequencer.bake_control_space"), BakePayload, BakeCapture));
    TestFalse(TEXT("valid space bake is refused while its sequence editor is closed"),
        BakeCapture.bSuccess);
    TestEqual(TEXT("valid closed-editor refusal is typed EDITOR_NOT_OPEN"),
        BakeCapture.ErrorCode, FString(TEXT("EDITOR_NOT_OPEN")));
    TestTrue(TEXT("closed-editor refusal preserves exact transform and space channels"),
        BakeCRControlChannelStatesEqual(
            StateBefore, BakeCRCaptureControlChannelState(Section, ControlName)));
    TestNull(TEXT("closed-editor refusal creates no space channel"),
        Section->GetSpaceChannel(ControlName));
    TestFalse(TEXT("closed-editor refusal leaves the package clean"),
        Sequence->GetOutermost()->IsDirty());
    TestNotNull(TEXT("closed-editor refusal retains the transform control"),
        Section->GetControlRig()
            ? Section->GetControlRig()->FindControl(ControlName) : nullptr);

    return true;
}
