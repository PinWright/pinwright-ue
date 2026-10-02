// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for F-sequencer-control-keys-batch (sequencer.set_control_keys / get_control_values) and
// F-sequencer-pin-controls (sequencer.pin_controls), Handlers/Sequencer/ControlRigSequencerHandler.cpp.
//
// THE FIXTURE IS HOST-INDEPENDENT. A transient Control Rig bound to a mesh-less skeletal-mesh actor
// (FControlRigObjectBinding only binds skeletal-mesh / Control Rig components), with a hand-built chain: null "Base" at (100,25,0) -> control "Root" -> "Mid" (offset: yaw 30, z 50) -> "Tip"
// (offset: pitch 10, (20,0,-10)), every control an Euler transform. The bound actor sits at a
// non-identity world transform, so world space differs from rig space. No control hangs off a bone,
// so the handlers never run this bare rig's forward solve (which could reset a runtime-built
// hierarchy; see the space-bake tests).
//
// WHAT FAILS IF THE FEATURE IS REVERTED. Without the verbs the registration asserts fail. Without the
// rollback the mismatch/tolerance tests see the corrupted keys survive. Without the component world
// transform or the parent chain in the world solve, WorldSpaceRoundTrip's independently derived local
// channel values disagree. The clip-edit workflow test is host-dependent (mannequin content).
#include "Misc/AutomationTest.h"
#include "Tests/AutomationSuiteMaintenance.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Utils/AssetUtils.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/Sequencer/ControlRigKeyTestHooks.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "LevelSequence.h"
#include "LevelSequenceEditorBlueprintLibrary.h"
#include "MovieScene.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "UObject/Package.h"

#include "ControlRig.h"
#include "ControlRigObjectBinding.h"
#include "Rigs/AdditiveControlRig.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"

namespace
{
    // Distinct CRKeys* names: unity builds merge this TU with the sibling sequencer tests.
    const FTransform CRKeysActorWorld(FRotator(0.0, 40.0, 0.0), FVector(300.0, -50.0, 20.0));
    const FTransform CRKeysBaseGlobal(FVector(100.0, 25.0, 0.0));
    const FTransform CRKeysMidOffset(FRotator(0.0, 30.0, 0.0), FVector(0.0, 0.0, 50.0));
    const FTransform CRKeysTipOffset(FRotator(10.0, 0.0, 0.0), FVector(20.0, 0.0, -10.0));

    struct FCRKeysFixture
    {
        FString SeqPath;
        ULevelSequence* Sequence = nullptr;
        UMovieScene* MovieScene = nullptr;
        FGuid Binding;
        FString BindingId;
        UMovieSceneControlRigParameterSection* Section = nullptr;
    };

    UClass* CRKeysRigClass()
    {
        // See TestSequencerBakeControlRig.cpp: UControlRig is abstract through 5.7.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        return UControlRig::StaticClass();
#else
        return UAdditiveControlRig::StaticClass();
#endif
    }

    // Caller owns cleanup: CleanupTestAsset(Out.SeqPath) and an FScopedEditorWorldActorGuard.
    bool CRKeysBuildFixture(FAutomationTestBase& Test, FCRKeysFixture& Out)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_CRKeysSeq_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString Folder = FString(PinWrightSuiteMaintenance::ScratchRootPackagePath()) / TEXT("MCP_CRKeysProbe");
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), Folder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);
        Out.SeqPath = Folder / SeqName;
        Out.Sequence = CreateCapture.bSuccess ? Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(Out.SeqPath)) : nullptr;
        Out.MovieScene = Out.Sequence ? Out.Sequence->GetMovieScene() : nullptr;
        UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!Test.TestNotNull(TEXT("fixture sequence created"), Out.MovieScene) ||
            !Test.TestNotNull(TEXT("editor world present"), EditorWorld))
        {
            return false;
        }
        Out.MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
        Out.MovieScene->SetDisplayRate(FFrameRate(30, 1));

        const FString Label = FString::Printf(TEXT("MCP_CRKeysActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        ASkeletalMeshActor* Actor = SpawnActorInActiveWorld<ASkeletalMeshActor>(ASkeletalMeshActor::StaticClass(),
            CRKeysActorWorld.GetLocation(), CRKeysActorWorld.Rotator(), Label);
        if (!Test.TestNotNull(TEXT("fixture actor spawned"), Actor))
        {
            return false;
        }
        Out.Binding = Out.MovieScene->AddPossessable(Label, ASkeletalMeshActor::StaticClass());
        Out.Sequence->BindPossessableObject(Out.Binding, *Actor, EditorWorld);
        Out.BindingId = Out.Binding.ToString(EGuidFormats::Digits);

        UMovieSceneControlRigParameterTrack* Track = Out.MovieScene->AddTrack<UMovieSceneControlRigParameterTrack>(Out.Binding);
        UControlRig* Rig = Track ? NewObject<UControlRig>(Track, CRKeysRigClass(), NAME_None, RF_Transactional) : nullptr;
        if (!Test.TestNotNull(TEXT("fixture rig created"), Rig))
        {
            return false;
        }
        Rig->SetObjectBinding(MakeShared<FControlRigObjectBinding>());
        Rig->GetObjectBinding()->BindToObject(Actor->GetSkeletalMeshComponent());
        Rig->Initialize();
        Rig->Evaluate_AnyThread(); // finish construction before the runtime-built hierarchy (see the bake tests)
        URigHierarchy* Hierarchy = Rig->GetHierarchy();
        URigHierarchyController* Controller = Hierarchy ? Hierarchy->GetController(true) : nullptr;
        if (!Test.TestNotNull(TEXT("fixture hierarchy controller"), Controller))
        {
            return false;
        }
        const FRigElementKey Base = Controller->AddNull(TEXT("Base"), FRigElementKey(), CRKeysBaseGlobal,
            /*bTransformInGlobal=*/true, /*bSetupUndo=*/false, /*bPrintPythonCommand=*/false);
        FRigControlSettings Settings;
        Settings.AnimationType = ERigControlAnimationType::AnimationControl;
        Settings.ControlType = ERigControlType::EulerTransform;
        auto AddControl = [&](const TCHAR* Name, const FRigElementKey& Parent, const FTransform& Offset)
        {
            const FRigElementKey Key = Controller->AddControl(Name, Parent, Settings,
                FRigControlValue::Make<FRigControlValue::FEulerTransform_Float>(FEulerTransform::Identity),
                Offset, FTransform::Identity, /*bSetupUndo=*/false, /*bPrintPythonCommand=*/false);
            Hierarchy->SetInitialLocalTransform(Key, FTransform::Identity, true, false, false);
            Hierarchy->SetLocalTransform(Key, FTransform::Identity, false, true, false, false);
            Hierarchy->GetGlobalTransform(Key);
            return Key;
        };
        Hierarchy->SetInitialGlobalTransform(Base, CRKeysBaseGlobal, true, false);
        Hierarchy->SetGlobalTransform(Base, CRKeysBaseGlobal, false, true, false, false);
        const FRigElementKey Root = AddControl(TEXT("Root"), Base, FTransform::Identity);
        const FRigElementKey Mid = AddControl(TEXT("Mid"), Root, CRKeysMidOffset);
        const FRigElementKey Tip = AddControl(TEXT("Tip"), Mid, CRKeysTipOffset);
        if (!Test.TestTrue(TEXT("fixture controls created"), Root.IsValid() && Mid.IsValid() && Tip.IsValid()))
        {
            return false;
        }
        Out.Section = Cast<UMovieSceneControlRigParameterSection>(Track->CreateControlRigSection(0, Rig, /*bOwnsControlRig=*/true));
        if (!Test.TestNotNull(TEXT("fixture section created"), Out.Section))
        {
            return false;
        }
        Out.Section->RecreateWithThisControlRig(Rig, /*bSetDefault=*/true);
        Out.Section->ReconstructChannelProxy();
        return Test.TestTrue(TEXT("fixture section exposes 27 float channels (3 Euler transforms)"),
            Out.Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>().Num() >= 27);
    }

    TSharedPtr<FJsonObject> CRKeysPayload(const FCRKeysFixture& F)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("sequence"), F.SeqPath);
        Payload->SetStringField(TEXT("binding"), F.BindingId);
        return Payload;
    }

    TSharedPtr<FJsonValue> CRKeysNumbers(std::initializer_list<double> Values)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const double V : Values)
        {
            Arr.Add(MakeShared<FJsonValueNumber>(V));
        }
        return MakeShared<FJsonValueArray>(Arr);
    }

    TSharedPtr<FJsonValue> CRKeysTransformJson(const FTransform& X)
    {
        const FVector T = X.GetLocation();
        const FRotator R = X.Rotator();
        return CRKeysNumbers({T.X, T.Y, T.Z, R.Roll, R.Pitch, R.Yaw});
    }

    FTransform CRKeysTransformFromJson(const TSharedPtr<FJsonValue>& Value)
    {
        TArray<double> V;
        for (const TSharedPtr<FJsonValue>& E : Value->AsArray())
        {
            V.Add(E->AsNumber());
        }
        return V.Num() >= 6 ? FTransform(FRotator(V[4], V[5], V[3]), FVector(V[0], V[1], V[2])) : FTransform::Identity;
    }

    TSharedPtr<FJsonValue> CRKeysEntry(const FString& Control, const TArray<int32>& Frames, const TArray<TSharedPtr<FJsonValue>>& Values)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("control"), Control);
        TArray<TSharedPtr<FJsonValue>> FramesJson;
        for (const int32 Frame : Frames)
        {
            FramesJson.Add(MakeShared<FJsonValueNumber>(Frame));
        }
        Entry->SetArrayField(TEXT("frames"), FramesJson);
        Entry->SetArrayField(TEXT("values"), Values);
        return MakeShared<FJsonValueObject>(Entry);
    }

    // Local keys for 3 controls x 20 frames: channel c of control k at frame f = k*10 + f*0.5 + c*0.25.
    TArray<TSharedPtr<FJsonValue>> CRKeysLocalBatch(TArray<int32>& OutFrames)
    {
        OutFrames.Reset();
        for (int32 Frame = 0; Frame < 20; ++Frame)
        {
            OutFrames.Add(Frame);
        }
        TArray<TSharedPtr<FJsonValue>> Keys;
        const TCHAR* Names[] = {TEXT("Root"), TEXT("Mid"), TEXT("Tip")};
        for (int32 K = 0; K < 3; ++K)
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (const int32 Frame : OutFrames)
            {
                TArray<TSharedPtr<FJsonValue>> Channels;
                for (int32 C = 0; C < 9; ++C)
                {
                    Channels.Add(MakeShared<FJsonValueNumber>(K * 10.0 + Frame * 0.5 + C * 0.25));
                }
                Values.Add(MakeShared<FJsonValueArray>(Channels));
            }
            Keys.Add(CRKeysEntry(Names[K], OutFrames, Values));
        }
        return Keys;
    }

    // A control's first float channel (translation X for a transform), resolved from engine metadata.
    FMovieSceneFloatChannel* CRKeysFirstChannelOf(UMovieSceneControlRigParameterSection* Section, FName Control)
    {
        TArrayView<FMovieSceneFloatChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        for (FMovieSceneFloatChannel* Channel : Channels)
        {
            const UE::MovieScene::FControlRigChannelMetaData Meta = Section->GetChannelMetaData(Channel);
            if (static_cast<bool>(Meta) && Meta.GetControlName() == Control && Meta.GetChannelIndex() == 0)
            {
                return Channel;
            }
        }
        return nullptr;
#else
        const FChannelMapInfo* Info = Section->ControlChannelMap.Find(Control);
        return Info && Channels.IsValidIndex(Info->ChannelIndex) ? Channels[Info->ChannelIndex] : nullptr;
#endif
    }

    // Every float channel's keys, for exact before/after comparison.
    TArray<TPair<TArray<FFrameNumber>, TArray<FMovieSceneFloatValue>>> CRKeysCaptureAll(UMovieSceneControlRigParameterSection* Section)
    {
        TArray<TPair<TArray<FFrameNumber>, TArray<FMovieSceneFloatValue>>> State;
        for (FMovieSceneFloatChannel* Channel : Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>())
        {
            const auto Data = static_cast<const FMovieSceneFloatChannel*>(Channel)->GetData();
            TPair<TArray<FFrameNumber>, TArray<FMovieSceneFloatValue>>& Entry = State.AddDefaulted_GetRef();
            Entry.Key.Append(Data.GetTimes().GetData(), Data.GetTimes().Num());
            Entry.Value.Append(Data.GetValues().GetData(), Data.GetValues().Num());
        }
        return State;
    }

    bool CRKeysStatesEqual(const TArray<TPair<TArray<FFrameNumber>, TArray<FMovieSceneFloatValue>>>& A,
        const TArray<TPair<TArray<FFrameNumber>, TArray<FMovieSceneFloatValue>>>& B)
    {
        if (A.Num() != B.Num())
        {
            return false;
        }
        for (int32 I = 0; I < A.Num(); ++I)
        {
            if (A[I].Key != B[I].Key || A[I].Value != B[I].Value)
            {
                return false;
            }
        }
        return true;
    }

    // get_control_values -> per control, per frame value arrays. Empty on failure (asserted).
    TArray<TArray<TArray<double>>> CRKeysRead(FAutomationTestBase& Test, const FCRKeysFixture& F,
        const TArray<FString>& Controls, const TArray<int32>& Frames, const TCHAR* Space)
    {
        TSharedPtr<FJsonObject> Payload = CRKeysPayload(F);
        TArray<TSharedPtr<FJsonValue>> ControlsJson, FramesJson;
        for (const FString& C : Controls) { ControlsJson.Add(MakeShared<FJsonValueString>(C)); }
        for (const int32 Frame : Frames) { FramesJson.Add(MakeShared<FJsonValueNumber>(Frame)); }
        Payload->SetArrayField(TEXT("controls"), ControlsJson);
        Payload->SetArrayField(TEXT("frames"), FramesJson);
        Payload->SetStringField(TEXT("space"), Space);
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("sequencer.get_control_values"), Payload, Capture);
        TArray<TArray<TArray<double>>> Out;
        const TArray<TSharedPtr<FJsonValue>>* ControlsOut = nullptr;
        if (!Test.TestTrue(FString::Printf(TEXT("get_control_values (%s) succeeded: %s %s"), Space, *Capture.ErrorCode, *Capture.Message),
                Capture.bSuccess && Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("controls"), ControlsOut)))
        {
            return Out;
        }
        for (const TSharedPtr<FJsonValue>& ControlJson : *ControlsOut)
        {
            TArray<TArray<double>>& PerFrame = Out.AddDefaulted_GetRef();
            for (const TSharedPtr<FJsonValue>& FrameJson : ControlJson->AsObject()->GetArrayField(TEXT("values")))
            {
                TArray<double>& Values = PerFrame.AddDefaulted_GetRef();
                for (const TSharedPtr<FJsonValue>& V : FrameJson->AsArray())
                {
                    Values.Add(V->Type == EJson::Number ? V->AsNumber() : TNumericLimits<double>::Max());
                }
            }
        }
        return Out;
    }
}

// ---------------------------------------------------------------------------
// 3 controls x 20 frames in ONE call; readback through get_control_values and the section itself.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerControlRigKeysLocalBatchTest,
    "PinWright.Sequencer.ControlRigKeys.LocalThreeControlsTwentyFrames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerControlRigKeysLocalBatchTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("sequencer.set_control_keys registered"), IsHandlerRegistered(TEXT("sequencer.set_control_keys")));
    TestTrue(TEXT("sequencer.get_control_values registered"), IsHandlerRegistered(TEXT("sequencer.get_control_values")));
    FScopedEditorWorldActorGuard WorldGuard;
    FCRKeysFixture F;
    ON_SCOPE_EXIT { CleanupTestAsset(F.SeqPath); };
    if (!CRKeysBuildFixture(*this, F))
    {
        return false;
    }

    TArray<int32> Frames;
    TSharedPtr<FJsonObject> Payload = CRKeysPayload(F);
    Payload->SetStringField(TEXT("space"), TEXT("local"));
    Payload->SetArrayField(TEXT("keys"), CRKeysLocalBatch(Frames));
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("sequencer.set_control_keys"), Payload, Capture);
    if (!TestTrue(FString::Printf(TEXT("set_control_keys succeeded: %s %s"), *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return false;
    }
    TestEqual(TEXT("keysWritten counts 3 controls x 20 frames"), Capture.Result->GetIntegerField(TEXT("keysWritten")), 60);
    const TArray<TSharedPtr<FJsonValue>> Stats = Capture.Result->GetArrayField(TEXT("controls"));
    TestEqual(TEXT("one stats row per control"), Stats.Num(), 3);
    for (const TSharedPtr<FJsonValue>& Row : Stats)
    {
        TestEqual(TEXT("each control wrote 20 keys"), Row->AsObject()->GetIntegerField(TEXT("keysWritten")), 20);
        TestTrue(TEXT("readback error is float rounding only"), Row->AsObject()->GetNumberField(TEXT("maxReadbackError")) < 1.0e-4);
    }

    // Independent of the verb: every one of the 27 channels now carries exactly 20 keys.
    int32 ChannelsWithTwentyKeys = 0;
    for (FMovieSceneFloatChannel* Channel : F.Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>())
    {
        ChannelsWithTwentyKeys += Channel->GetData().GetTimes().Num() == 20 ? 1 : 0;
    }
    TestEqual(TEXT("27 channels carry 20 keys each"), ChannelsWithTwentyKeys, 27);

    // Batch readback: three controls at three frames, each channel the value keyed.
    F.Sequence->GetOutermost()->SetDirtyFlag(false);
    const TArray<int32> ReadFrames = {0, 7, 19};
    const TArray<TArray<TArray<double>>> Read = CRKeysRead(*this, F, {TEXT("Root"), TEXT("Mid"), TEXT("Tip")}, ReadFrames, TEXT("local"));
    TestFalse(TEXT("get_control_values left the package clean"), F.Sequence->GetOutermost()->IsDirty());
    if (TestEqual(TEXT("three controls read back"), Read.Num(), 3))
    {
        for (int32 K = 0; K < 3; ++K)
        {
            for (int32 FI = 0; FI < ReadFrames.Num(); ++FI)
            {
                for (int32 C = 0; C < 9 && C < Read[K][FI].Num(); ++C)
                {
                    const double Want = K * 10.0 + ReadFrames[FI] * 0.5 + C * 0.25;
                    TestTrue(FString::Printf(TEXT("control %d frame %d channel %d reads %g (wrote %g)"), K, ReadFrames[FI], C, Read[K][FI][C], Want),
                        FMath::IsNearlyEqual(Read[K][FI][C], Want, 1.0e-3));
                }
            }
        }
    }

    // A repeated (control, frame) pair is refused before anything is written.
    const auto Before = CRKeysCaptureAll(F.Section);
    TSharedPtr<FJsonObject> Dup = CRKeysPayload(F);
    Dup->SetStringField(TEXT("space"), TEXT("local"));
    Dup->SetArrayField(TEXT("keys"), {CRKeysEntry(TEXT("Root"), {3, 3}, {CRKeysNumbers({1.0}), CRKeysNumbers({2.0})})});
    FTestResponseCapture DupCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.set_control_keys"), Dup, DupCapture);
    TestEqual(TEXT("duplicate frame refused as INVALID_ARGUMENT"), DupCapture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("refused batch wrote nothing"), CRKeysStatesEqual(Before, CRKeysCaptureAll(F.Section)));
    return true;
}

// ---------------------------------------------------------------------------
// An injected readback mismatch undoes EVERY key of the call, not only the bad one.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerControlRigKeysMismatchRollbackTest,
    "PinWright.Sequencer.ControlRigKeys.ReadbackMismatchUndoesEveryKey",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerControlRigKeysMismatchRollbackTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    FCRKeysFixture F;
    ON_SCOPE_EXIT
    {
        PinWrightControlRigKeyTestHooks::PostWriteHook() = nullptr;
        CleanupTestAsset(F.SeqPath);
    };
    if (!CRKeysBuildFixture(*this, F))
    {
        return false;
    }
    // A pre-existing key that must survive the rollback untouched.
    CRKeysFirstChannelOf(F.Section, TEXT("Root"))->GetData().AddKey(FFrameNumber(0), FMovieSceneFloatValue(3.0f));
    const auto Before = CRKeysCaptureAll(F.Section);
    F.Sequence->GetOutermost()->SetDirtyFlag(false);

    // Corrupt one written key (Tip, first channel, frame 5) between the write and the readback.
    const FFrameNumber CorruptTick = FFrameRate::TransformTime(FFrameTime(FFrameNumber(5)),
        F.MovieScene->GetDisplayRate(), F.MovieScene->GetTickResolution()).RoundToFrame();
    int32 HookCalls = 0;
    PinWrightControlRigKeyTestHooks::PostWriteHook() = [&HookCalls, CorruptTick](UMovieSceneControlRigParameterSection* Section)
    {
        ++HookCalls;
        CRKeysFirstChannelOf(Section, TEXT("Tip"))->GetData().UpdateOrAddKey(CorruptTick, FMovieSceneFloatValue(999.0f));
    };

    TArray<int32> Frames;
    TSharedPtr<FJsonObject> Payload = CRKeysPayload(F);
    Payload->SetStringField(TEXT("space"), TEXT("local"));
    Payload->SetArrayField(TEXT("keys"), CRKeysLocalBatch(Frames));
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("sequencer.set_control_keys"), Payload, Capture);

    TestEqual(TEXT("the hook ran once, between write and readback"), HookCalls, 1);
    TestFalse(TEXT("a corrupted readback fails the call"), Capture.bSuccess);
    TestEqual(TEXT("the failure is CONTROL_KEY_READBACK_MISMATCH"), Capture.ErrorCode, FString(TEXT("CONTROL_KEY_READBACK_MISMATCH")));
    if (TestTrue(TEXT("the failure carries a result"), Capture.Result.IsValid()))
    {
        TestTrue(TEXT("rolledBack reports a verified restore"), Capture.Result->GetBoolField(TEXT("rolledBack")));
        const TArray<TSharedPtr<FJsonValue>> Mismatches = Capture.Result->GetArrayField(TEXT("mismatches"));
        TestTrue(TEXT("the mismatch names Tip at frame 5"), Mismatches.Num() >= 1 &&
            Mismatches[0]->AsObject()->GetStringField(TEXT("control")) == TEXT("Tip") &&
            Mismatches[0]->AsObject()->GetIntegerField(TEXT("frame")) == 5);
    }
    TestTrue(TEXT("every channel is back to its exact pre-call keys (all 60 writes undone)"),
        CRKeysStatesEqual(Before, CRKeysCaptureAll(F.Section)));
    TestFalse(TEXT("the package's dirty flag was restored"), F.Sequence->GetOutermost()->IsDirty());
    return true;
}

// ---------------------------------------------------------------------------
// World space: write world targets, check the channels against an independent derivation from the
// fixture's known chain, and read them back as world.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerControlRigKeysWorldRoundTripTest,
    "PinWright.Sequencer.ControlRigKeys.WorldSpaceRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerControlRigKeysWorldRoundTripTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    FCRKeysFixture F;
    ON_SCOPE_EXIT { CleanupTestAsset(F.SeqPath); };
    if (!CRKeysBuildFixture(*this, F))
    {
        return false;
    }

    const TArray<int32> Frames = {0, 10, 20};
    const TCHAR* Names[] = {TEXT("Root"), TEXT("Mid"), TEXT("Tip")};
    TArray<TArray<FTransform>> Targets; // [control][frame]
    TArray<TSharedPtr<FJsonValue>> Keys;
    for (int32 K = 0; K < 3; ++K)
    {
        TArray<FTransform>& PerFrame = Targets.AddDefaulted_GetRef();
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const int32 Frame : Frames)
        {
            const FTransform Target(FRotator(Frame * 1.5, 20.0 + K * 10.0, K * 5.0), FVector(350.0 + Frame, -40.0 + K * 7.0, 100.0 + K * 30.0));
            PerFrame.Add(Target);
            Values.Add(CRKeysTransformJson(Target));
        }
        Keys.Add(CRKeysEntry(Names[K], Frames, Values));
    }
    TSharedPtr<FJsonObject> Payload = CRKeysPayload(F);
    Payload->SetStringField(TEXT("space"), TEXT("world"));
    Payload->SetArrayField(TEXT("keys"), Keys);
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("sequencer.set_control_keys"), Payload, Capture);
    if (!TestTrue(FString::Printf(TEXT("world set_control_keys succeeded: %s %s"), *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Row : Capture.Result->GetArrayField(TEXT("controls")))
    {
        TestTrue(TEXT("world readback within 0.01 cm"), Row->AsObject()->GetNumberField(TEXT("maxErrorCm")) <= 0.01);
        TestTrue(TEXT("world readback within 0.01 deg"), Row->AsObject()->GetNumberField(TEXT("maxErrorDeg")) <= 0.01);
    }

    // Independent derivation: world = value * offset * parentWorld, parentWorld(Root) = Base * actor.
    // So the stored channel translation must equal each target relative to its parent's world.
    const TArray<TArray<TArray<double>>> Local = CRKeysRead(*this, F, {TEXT("Root"), TEXT("Mid"), TEXT("Tip")}, Frames, TEXT("local"));
    if (TestEqual(TEXT("local readback has three controls"), Local.Num(), 3))
    {
        for (int32 FI = 0; FI < Frames.Num(); ++FI)
        {
            const FTransform Parents[] = {
                CRKeysBaseGlobal * CRKeysActorWorld,
                CRKeysMidOffset * Targets[0][FI],
                CRKeysTipOffset * Targets[1][FI]};
            for (int32 K = 0; K < 3; ++K)
            {
                const FTransform Expected = Targets[K][FI].GetRelativeTransform(K == 0 ? Parents[0] : Parents[K]);
                const FVector Got(Local[K][FI][0], Local[K][FI][1], Local[K][FI][2]);
                TestTrue(FString::Printf(TEXT("%s frame %d local translation %s matches derived %s"),
                        Names[K], Frames[FI], *Got.ToString(), *Expected.GetLocation().ToString()),
                    Got.Equals(Expected.GetLocation(), 0.01));
                const FQuat GotRot = FRotator(Local[K][FI][4], Local[K][FI][5], Local[K][FI][3]).Quaternion();
                TestTrue(FString::Printf(TEXT("%s frame %d local rotation matches derived"), Names[K], Frames[FI]),
                    FMath::RadiansToDegrees(GotRot.AngularDistance(Expected.GetRotation())) <= 0.01);
            }
        }
    }

    // And the world readback verb returns the targets.
    const TArray<TArray<TArray<double>>> WorldRead = CRKeysRead(*this, F, {TEXT("Root"), TEXT("Mid"), TEXT("Tip")}, Frames, TEXT("world"));
    if (TestEqual(TEXT("world readback has three controls"), WorldRead.Num(), 3))
    {
        for (int32 K = 0; K < 3; ++K)
        {
            for (int32 FI = 0; FI < Frames.Num(); ++FI)
            {
                TArray<TSharedPtr<FJsonValue>> Arr;
                for (const double V : WorldRead[K][FI]) { Arr.Add(MakeShared<FJsonValueNumber>(V)); }
                const FTransform Got = CRKeysTransformFromJson(MakeShared<FJsonValueArray>(Arr));
                TestTrue(FString::Printf(TEXT("%s frame %d world readback equals the target"), Names[K], Frames[FI]),
                    Got.GetLocation().Equals(Targets[K][FI].GetLocation(), 0.01) &&
                    FMath::RadiansToDegrees(Got.GetRotation().AngularDistance(Targets[K][FI].GetRotation())) <= 0.01);
            }
        }
    }

    // A binding moved by a transform track cannot be evaluated headless: refused, not answered.
    F.MovieScene->AddTrack<UMovieScene3DTransformTrack>(F.Binding);
    FTestResponseCapture Refused;
    InvokeHandlerWithCapture(TEXT("sequencer.set_control_keys"), Payload, Refused);
    TestEqual(TEXT("world keys on a transform-tracked binding are CONTROL_WORLD_SPACE_UNAVAILABLE"),
        Refused.ErrorCode, FString(TEXT("CONTROL_WORLD_SPACE_UNAVAILABLE")));
    return true;
}

// ---------------------------------------------------------------------------
// pin_controls: a moving "foot" holds within 0.1 cm over 30 frames; blend edges match the
// smoothstep contract.
// ---------------------------------------------------------------------------
namespace
{
    // Root slides +60 cm in X over frames 0..60, so the unpinned Tip moves with it.
    bool CRKeysAnimateRoot(FAutomationTestBase& Test, const FCRKeysFixture& F)
    {
        TSharedPtr<FJsonObject> Payload = CRKeysPayload(F);
        Payload->SetStringField(TEXT("space"), TEXT("local"));
        Payload->SetArrayField(TEXT("keys"), {CRKeysEntry(TEXT("Root"), {0, 60},
            {CRKeysNumbers({0.0, 0.0, 0.0}), CRKeysNumbers({60.0, 0.0, 0.0})})});
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("sequencer.set_control_keys"), Payload, Capture);
        return Test.TestTrue(TEXT("root animated"), Capture.bSuccess);
    }

    TSharedPtr<FJsonObject> CRKeysPinPayload(const FCRKeysFixture& F)
    {
        TSharedPtr<FJsonObject> Payload = CRKeysPayload(F);
        Payload->SetArrayField(TEXT("controls"), {MakeShared<FJsonValueString>(TEXT("Tip"))});
        Payload->SetNumberField(TEXT("startFrame"), 20);
        Payload->SetNumberField(TEXT("endFrame"), 49);
        Payload->SetStringField(TEXT("target"), TEXT("anchorFrame"));
        Payload->SetNumberField(TEXT("anchorFrame"), 20);
        Payload->SetNumberField(TEXT("blendInFrames"), 5);
        Payload->SetNumberField(TEXT("blendOutFrames"), 5);
        Payload->SetNumberField(TEXT("positionToleranceCm"), 0.1);
        Payload->SetNumberField(TEXT("rotationToleranceDeg"), 0.1);
        return Payload;
    }

    FTransform CRKeysWorldOf(const TArray<double>& V)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const double D : V) { Arr.Add(MakeShared<FJsonValueNumber>(D)); }
        return CRKeysTransformFromJson(MakeShared<FJsonValueArray>(Arr));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerControlRigPinHoldTest,
    "PinWright.Sequencer.ControlRigKeys.PinHoldsAndBlends",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerControlRigPinHoldTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("sequencer.pin_controls registered"), IsHandlerRegistered(TEXT("sequencer.pin_controls")));
    FScopedEditorWorldActorGuard WorldGuard;
    FCRKeysFixture F;
    ON_SCOPE_EXIT { CleanupTestAsset(F.SeqPath); };
    if (!CRKeysBuildFixture(*this, F) || !CRKeysAnimateRoot(*this, F))
    {
        return false;
    }

    TArray<int32> Frames;
    for (int32 Frame = 15; Frame <= 54; ++Frame)
    {
        Frames.Add(Frame);
    }
    const TArray<TArray<TArray<double>>> Before = CRKeysRead(*this, F, {TEXT("Tip")}, Frames, TEXT("world"));
    if (!TestEqual(TEXT("original Tip motion read"), Before.Num(), 1))
    {
        return false;
    }
    const FTransform Anchor = CRKeysWorldOf(Before[0][20 - 15]);
    TestTrue(TEXT("precondition: the unpinned Tip moves across the hold"),
        !CRKeysWorldOf(Before[0][49 - 15]).GetLocation().Equals(Anchor.GetLocation(), 1.0));

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("sequencer.pin_controls"), CRKeysPinPayload(F), Capture);
    if (!TestTrue(FString::Printf(TEXT("pin_controls succeeded: %s %s"), *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return false;
    }
    TestEqual(TEXT("keys span blend-in + hold + blend-out (40 frames)"), Capture.Result->GetIntegerField(TEXT("keysWritten")), 40);

    const TArray<TArray<TArray<double>>> After = CRKeysRead(*this, F, {TEXT("Tip")}, Frames, TEXT("world"));
    if (!TestEqual(TEXT("pinned Tip motion read"), After.Num(), 1))
    {
        return false;
    }
    double MaxHoldCm = 0.0;
    for (int32 Frame = 20; Frame <= 49; ++Frame)
    {
        MaxHoldCm = FMath::Max(MaxHoldCm, FVector::Distance(CRKeysWorldOf(After[0][Frame - 15]).GetLocation(), Anchor.GetLocation()));
    }
    TestTrue(FString::Printf(TEXT("Tip holds within 0.1 cm over the 30 hold frames (max %g cm)"), MaxHoldCm), MaxHoldCm <= 0.1);

    // Blend edges: weight 0 at the outer frames (original motion), smoothstep in between.
    auto ExpectBlend = [&](int32 Frame, double T)
    {
        const double W = T * T * (3.0 - 2.0 * T);
        FTransform Expected;
        Expected.Blend(CRKeysWorldOf(Before[0][Frame - 15]), Anchor, static_cast<float>(W));
        const FVector Got = CRKeysWorldOf(After[0][Frame - 15]).GetLocation();
        TestTrue(FString::Printf(TEXT("frame %d sits at blend weight %.3f (got %s, want %s)"), Frame, W, *Got.ToString(), *Expected.GetLocation().ToString()),
            Got.Equals(Expected.GetLocation(), 0.1));
    };
    ExpectBlend(15, 0.0);   // blend-in outer edge: untouched motion
    ExpectBlend(17, 0.4);
    ExpectBlend(52, 0.4);   // blend-out: (54 - 52) / 5
    ExpectBlend(54, 0.0);   // blend-out outer edge
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerControlRigPinToleranceRollbackTest,
    "PinWright.Sequencer.ControlRigKeys.PinToleranceFailureUndoesAll",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerControlRigPinToleranceRollbackTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    FCRKeysFixture F;
    ON_SCOPE_EXIT
    {
        PinWrightControlRigKeyTestHooks::PostWriteHook() = nullptr;
        CleanupTestAsset(F.SeqPath);
    };
    if (!CRKeysBuildFixture(*this, F) || !CRKeysAnimateRoot(*this, F))
    {
        return false;
    }
    const auto Before = CRKeysCaptureAll(F.Section);
    F.Sequence->GetOutermost()->SetDirtyFlag(false);

    // Push Tip's X 5 cm off at frame 30 after the write.
    const FFrameNumber Tick30 = FFrameRate::TransformTime(FFrameTime(FFrameNumber(30)),
        F.MovieScene->GetDisplayRate(), F.MovieScene->GetTickResolution()).RoundToFrame();
    PinWrightControlRigKeyTestHooks::PostWriteHook() = [Tick30](UMovieSceneControlRigParameterSection* Section)
    {
        FMovieSceneFloatChannel* TipX = CRKeysFirstChannelOf(Section, TEXT("Tip"));
        float Current = 0.0f;
        TipX->Evaluate(FFrameTime(Tick30), Current);
        TipX->GetData().UpdateOrAddKey(Tick30, FMovieSceneFloatValue(Current + 5.0f));
    };

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("sequencer.pin_controls"), CRKeysPinPayload(F), Capture);
    TestEqual(TEXT("a pin outside tolerance is CONTACT_TOLERANCE_EXCEEDED"), Capture.ErrorCode, FString(TEXT("CONTACT_TOLERANCE_EXCEEDED")));
    if (TestTrue(TEXT("the failure carries a result"), Capture.Result.IsValid()))
    {
        TestTrue(TEXT("rolledBack reports a verified restore"), Capture.Result->GetBoolField(TEXT("rolledBack")));
        const TArray<TSharedPtr<FJsonValue>> Rows = Capture.Result->GetArrayField(TEXT("controls"));
        TestTrue(TEXT("the reported error reflects the 5 cm push"),
            Rows.Num() == 1 && Rows[0]->AsObject()->GetNumberField(TEXT("maxErrorCm")) > 1.0);
    }
    TestTrue(TEXT("every channel is back to its exact pre-call keys"), CRKeysStatesEqual(Before, CRKeysCaptureAll(F.Section)));
    TestFalse(TEXT("the package's dirty flag was restored"), F.Sequence->GetOutermost()->IsDirty());
    return true;
}

// ---------------------------------------------------------------------------
// The documented clip-edit workflow end to end on real content: AnimSequence -> skeletal track ->
// bake_to_controlrig -> set_control_keys (local + world on an FK control) -> export_anim_sequence,
// and the source AnimSequence is byte-unchanged on disk and not dirtied.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerControlRigClipEditWorkflowTest,
    "PinWright.Sequencer.ControlRigKeys.ClipEditWorkflowLeavesSourceUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerControlRigClipEditWorkflowTest::RunTest(const FString& Parameters)
{
    const TArray<FString> MeshCandidates = {
        TEXT("/Game/Characters/Heroes/Mannequin/Meshes/SKM_Manny"),
        TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny"),
    };
    const TArray<FString> AnimCandidates = {
        TEXT("/Game/Characters/Heroes/Mannequin/Animations/Locomotion/Rifle/MM_Rifle_Idle_Hipfire"),
        TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Idle"),
    };
    PINWRIGHT_SKIP_IF_ALL_FIXTURES_MISSING(MeshCandidates);
    PINWRIGHT_SKIP_IF_ALL_FIXTURES_MISSING(AnimCandidates);
    USkeletalMesh* Mesh = nullptr;
    for (const FString& Path : MeshCandidates)
    {
        Mesh = Mesh ? Mesh : Cast<USkeletalMesh>(UEditorAssetLibrary::LoadAsset(Path));
    }
    FString AnimPath;
    UAnimSequence* Anim = nullptr;
    for (const FString& Path : AnimCandidates)
    {
        if (!Anim && FPackageName::DoesPackageExist(Path))
        {
            Anim = Cast<UAnimSequence>(UEditorAssetLibrary::LoadAsset(Path));
            AnimPath = Path;
        }
    }
    if (!TestNotNull(TEXT("mannequin mesh loads"), Mesh) || !TestNotNull(TEXT("source AnimSequence loads"), Anim))
    {
        return false;
    }
    const FString AnimFile = FPackageName::LongPackageNameToFilename(AnimPath, FPackageName::GetAssetPackageExtension());
    const FMD5Hash HashBefore = FMD5Hash::HashFile(*AnimFile);
    const bool bAnimWasDirty = Anim->GetOutermost()->IsDirty();

    FScopedEditorWorldActorGuard WorldGuard;
    const FString SeqName = FString::Printf(TEXT("MCP_CRKeysClip_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Folder = FString(PinWrightSuiteMaintenance::ScratchRootPackagePath()) / TEXT("MCP_CRKeysProbe");
    const FString SeqPath = Folder / SeqName;
    const FString ExportPath = Folder / (SeqName + TEXT("_Edited"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(ExportPath);
        CleanupTestAsset(SeqPath);
    };
    TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
    CreatePayload->SetStringField(TEXT("name"), SeqName);
    CreatePayload->SetStringField(TEXT("path"), Folder);
    FTestResponseCapture CreateCapture;
    InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);
    ULevelSequence* Sequence = Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(SeqPath));
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!TestNotNull(TEXT("sequence created"), Sequence) || !TestNotNull(TEXT("editor world"), EditorWorld))
    {
        return false;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    MovieScene->SetTickResolutionDirectly(FFrameRate(24000, 1));
    MovieScene->SetDisplayRate(FFrameRate(30, 1));
    MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), FFrameNumber(24000)));

    const FString Label = FString::Printf(TEXT("MCP_CRKeysManny_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ASkeletalMeshActor* Actor = SpawnActorInActiveWorld<ASkeletalMeshActor>(ASkeletalMeshActor::StaticClass(),
        FVector(0.0, 200.0, 0.0), FRotator(0.0, 90.0, 0.0), Label);
    if (!TestNotNull(TEXT("skeletal actor spawned"), Actor))
    {
        return false;
    }
    Actor->GetSkeletalMeshComponent()->SetSkeletalMeshAsset(Mesh);
    const FGuid Binding = MovieScene->AddPossessable(Label, ASkeletalMeshActor::StaticClass());
    Sequence->BindPossessableObject(Binding, *Actor, EditorWorld);
    const FString BindingId = Binding.ToString(EGuidFormats::Digits);

    auto Call = [&](const TCHAR* Method, const TSharedPtr<FJsonObject>& Payload) -> TSharedPtr<FJsonObject>
    {
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(Method, Payload, Capture);
        TestTrue(FString::Printf(TEXT("%s succeeded: %s %s"), Method, *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);
        return Capture.bSuccess ? Capture.Result : nullptr;
    };

    TSharedPtr<FJsonObject> AnimTrack = MakeShared<FJsonObject>();
    AnimTrack->SetStringField(TEXT("sequencePath"), SeqPath);
    AnimTrack->SetStringField(TEXT("bindingGuid"), BindingId);
    AnimTrack->SetStringField(TEXT("animSequencePath"), AnimPath);
    if (!Call(TEXT("sequencer.add_animation_track"), AnimTrack))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
    Target->SetStringField(TEXT("sequence"), SeqPath);
    Target->SetStringField(TEXT("binding"), BindingId);
    if (!ULevelSequenceEditorBlueprintLibrary::GetCurrentLevelSequence())
    {
        // See TestSequencerBakeControlRig.cpp: the engine bake logs this once with no Sequencer open.
        AddExpectedErrorPlain(TEXT("Can not open Sequencer for the LevelSequence None"), EAutomationExpectedErrorFlags::Contains, 1);
    }
    if (!Call(TEXT("sequencer.bake_to_controlrig"), Target))
    {
        return false;
    }
    const TSharedPtr<FJsonObject> List = Call(TEXT("sequencer.list_controls"), Target);
    TArray<FString> Controls;
    FString WorldControl;
    for (const TSharedPtr<FJsonValue>& Row : List ? List->GetArrayField(TEXT("controls")) : TArray<TSharedPtr<FJsonValue>>())
    {
        const FString Name = Row->AsObject()->GetStringField(TEXT("name"));
        if (Row->AsObject()->GetStringField(TEXT("type")) == TEXT("EulerTransform"))
        {
            if (Controls.Num() < 3)
            {
                Controls.Add(Name);
            }
            if (Name.StartsWith(TEXT("hand_r")))
            {
                WorldControl = Name;
            }
        }
    }
    if (!TestEqual(TEXT("the baked FK rig has three transform controls to edit"), Controls.Num(), 3))
    {
        return false;
    }

    // Local edit: 3 controls x 20 frames, rotation channels nudged.
    TArray<int32> Frames;
    for (int32 Frame = 0; Frame < 20; ++Frame) { Frames.Add(Frame); }
    TArray<TSharedPtr<FJsonValue>> Keys;
    for (int32 K = 0; K < 3; ++K)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const int32 Frame : Frames)
        {
            Values.Add(CRKeysNumbers({0.0, 0.0, 0.0, 1.0 + K, Frame * 0.5, 2.0}));
        }
        Keys.Add(CRKeysEntry(Controls[K], Frames, Values));
    }
    TSharedPtr<FJsonObject> LocalKeys = MakeShared<FJsonObject>(*Target);
    LocalKeys->SetStringField(TEXT("space"), TEXT("local"));
    LocalKeys->SetArrayField(TEXT("keys"), Keys);
    Call(TEXT("sequencer.set_control_keys"), LocalKeys);

    // World edit on an FK control (its parent is a bone, so the rig's forward solve runs).
    if (WorldControl.IsEmpty())
    {
        AddWarning(TEXT("No hand_r FK control on this mesh; the FK world-space step was not exercised."));
    }
    else
    {
        const TArray<TSharedPtr<FJsonValue>> FramesJson = {MakeShared<FJsonValueNumber>(10)};
        TSharedPtr<FJsonObject> Read = MakeShared<FJsonObject>(*Target);
        Read->SetArrayField(TEXT("controls"), {MakeShared<FJsonValueString>(WorldControl)});
        Read->SetArrayField(TEXT("frames"), FramesJson);
        Read->SetStringField(TEXT("space"), TEXT("world"));
        const TSharedPtr<FJsonObject> Now = Call(TEXT("sequencer.get_control_values"), Read);
        if (Now)
        {
            const TArray<TSharedPtr<FJsonValue>> Values = Now->GetArrayField(TEXT("controls"))[0]->AsObject()->GetArrayField(TEXT("values"));
            FTransform Moved = CRKeysTransformFromJson(Values[0]);
            Moved.AddToTranslation(FVector(0.0, 0.0, 5.0));
            TSharedPtr<FJsonObject> WorldKeys = MakeShared<FJsonObject>(*Target);
            WorldKeys->SetStringField(TEXT("space"), TEXT("world"));
            WorldKeys->SetNumberField(TEXT("positionToleranceCm"), 0.1);
            WorldKeys->SetNumberField(TEXT("rotationToleranceDeg"), 0.1);
            WorldKeys->SetArrayField(TEXT("keys"), {CRKeysEntry(WorldControl, {10}, {CRKeysTransformJson(Moved)})});
            Call(TEXT("sequencer.set_control_keys"), WorldKeys);
        }
    }

    TSharedPtr<FJsonObject> Export = MakeShared<FJsonObject>(*Target);
    Export->SetStringField(TEXT("outAssetPath"), ExportPath);
    Export->SetBoolField(TEXT("save"), false);
    const TSharedPtr<FJsonObject> Exported = Call(TEXT("sequencer.export_anim_sequence"), Export);
    TestTrue(TEXT("the edited clip exported with keys"), Exported && Exported->GetNumberField(TEXT("boneKeysWritten")) > 0);

    TestTrue(TEXT("the source AnimSequence file is byte-unchanged"), FMD5Hash::HashFile(*AnimFile) == HashBefore);
    TestEqual(TEXT("the source AnimSequence was not dirtied"), Anim->GetOutermost()->IsDirty(), bAnimWasDirty);
    return true;
}
