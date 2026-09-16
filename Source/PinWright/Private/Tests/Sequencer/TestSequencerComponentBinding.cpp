// Copyright (c) 2026 Alexander Penkin. MIT License.

// Component bindings on sequencer.add_actor / add_actors, and the parent link that makes them work.
//
// A component binding in Sequencer is a possessable whose PARENT is the owning actor's binding.
// MovieSceneHelpers::GetResolutionContext (MovieSceneCommonHelpers.cpp:1272-1297) substitutes the
// resolved parent object as the locator's resolution context only when
// `Possessable->GetParent().IsValid() && AreParentContextsSignificant()`. A component possessable
// missing that link therefore resolves against the WORLD, finds nothing, and every track keyed to it
// drives nothing - with no error anywhere. That is the failure these tests exist to make visible, so
// the parent is asserted from the movie scene rather than from the handler's own response, and the
// binding is resolved through the runtime path rather than trusted because the write returned true.
//
// No test here takes a conditional-skip path. The only fixture is /Engine/BasicShapes/Cube.Cube
// (engine content, present in every host), and a missing editor world / subsystem / probe actor is a
// hard AddError - a suite entry that reports green by declining to measure is the defect
// B-test-skips-assertions-silently, not a robustness feature.
//
// Every spawning test wraps its probes in FScopedEditorWorldActorGuard, and the probe actor is a
// NORMAL level actor: UEditorActorSubsystem::GetAllLevelActors filters RF_Transient
// (EditorActorSubsystem.cpp:386), so a transient probe is invisible to the verb under test and the
// failure would look like a verb defect.

#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "LevelSequencePlayer.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Sequencer/SequencerBindingUtils.h"
#include "PinWrightSubsystem.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Utils/AssetUtils.h"

namespace
{
    // Fixed so the assertions can name it. Unique within one actor, which is all a component name
    // has to be - unlike an actor display label (rpc-design.md Sec.15).
    const TCHAR* const PWCompBindWheelName = TEXT("PWWheelProbe");

    // Distinctly named (like CreateAddActorBindingSequence / CreateControlRigTrackSequence in the
    // sibling sequencer test TUs) so anonymous-namespace symbols cannot ODR-collide when Unity
    // merges these translation units.
    ULevelSequence* CreateCompBindSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_CompBindSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_CompBindProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

        if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
            !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            Test.AddError(TEXT("Could not create a probe LevelSequence via sequencer.create - the "
                               "component-binding cases cannot be exercised without one."));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // A normal (non-transient) level actor carrying a second, NON-ROOT static mesh component named
    // PWWheelProbe - the multi-part-object shape this feature exists for (a siege engine whose wheels
    // must spin independently of the body). Returns the actor; OutWheel is the component to bind.
    AStaticMeshActor* SpawnCompBindProbeActor(const FString& Label, UStaticMeshComponent*& OutWheel)
    {
        OutWheel = nullptr;
        UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!CubeMesh)
        {
            return nullptr;
        }
        AStaticMeshActor* Actor = SpawnActorInActiveWorld<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Label);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
        // AStaticMeshActor is Static by default, and a Static component refuses a runtime
        // SetRelativeTransform with only a log warning. Sequencer's transform tracks would then
        // move nothing and the evaluated test below would report a live binding as dead.
        Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);

        UStaticMeshComponent* Wheel = NewObject<UStaticMeshComponent>(
            Actor, UStaticMeshComponent::StaticClass(), FName(PWCompBindWheelName));
        if (!Wheel)
        {
            return Actor;
        }
        Wheel->SetStaticMesh(CubeMesh);
        Wheel->SetMobility(EComponentMobility::Movable);
        Wheel->SetupAttachment(Actor->GetRootComponent());
        Wheel->RegisterComponent();
        Actor->AddInstanceComponent(Wheel);
        OutWheel = Wheel;
        return Actor;
    }

    // Two Location keys on a binding's transform track: Axis 0/1/2 = X/Y/Z on the section's
    // double-channel proxy. Written directly rather than through sequence.add_keyframe because the
    // subject under test is the binding, not the keyframe verb - a failure here must not be
    // ambiguous between the two.
    bool AuthorCompBindLocationRamp(UMovieScene* MovieScene, const FGuid& BindingGuid,
        int32 Axis, double StartValue, double EndValue, FFrameNumber EndTick)
    {
        UMovieScene3DTransformTrack* Track =
            MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingGuid);
        if (!Track)
        {
            return false;
        }
        UMovieScene3DTransformSection* Section = nullptr;
        for (UMovieSceneSection* Candidate : Track->GetAllSections())
        {
            Section = Cast<UMovieScene3DTransformSection>(Candidate);
            if (Section)
            {
                break;
            }
        }
        if (!Section)
        {
            return false;
        }
        Section->SetRange(TRange<FFrameNumber>::All());
        TArrayView<FMovieSceneDoubleChannel*> Channels =
            Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
        if (!Channels.IsValidIndex(Axis) || !Channels[Axis])
        {
            return false;
        }
        Channels[Axis]->GetData().UpdateOrAddKey(FFrameNumber(0), FMovieSceneDoubleValue(StartValue));
        Channels[Axis]->GetData().UpdateOrAddKey(EndTick, FMovieSceneDoubleValue(EndValue));
        return true;
    }

    // Add a transform track to a binding through the production verb.
    bool AddCompBindTransformTrack(const FString& SeqPath, const FGuid& BindingGuid)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("sequencePath"), SeqPath);
        Payload->SetStringField(TEXT("bindingGuid"), BindingGuid.ToString());
        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("sequencer.add_transform_track"), Payload, Capture);
        return Capture.bWasCalled && Capture.bSuccess;
    }

    // sequencer.add_actor dereferences Ctx.GetSubsystem()->FindActorByName, which the null-subsystem
    // InvokeHandlerWithCapture cannot serve; MakeContextWithCapture carries the real subsystem AND
    // captures the response. Returns true iff the handler was found.
    bool InvokeCompBindHandler(const FString& MethodName, const TSharedPtr<FJsonObject>& Payload,
        UPinWrightSubsystem* Subsystem, FTestResponseCapture& Capture)
    {
        Capture.Reset();
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName == MethodName)
            {
                FHandlerContext Ctx = FHandlerContext::MakeContextWithCapture(
                    TEXT("test-id"), MethodName, Payload, Subsystem, &Capture);
                Reg.Func(Ctx);
                return true;
            }
        }
        return false;
    }

    // results[0] of an add_actor response, or an invalid ptr.
    TSharedPtr<FJsonObject> FirstResultRow(const FTestResponseCapture& Capture)
    {
        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        if (!Capture.Result.IsValid() ||
            !Capture.Result->TryGetArrayField(TEXT("results"), Results) ||
            !Results || Results->Num() == 0 || !(*Results)[0].IsValid())
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!(*Results)[0]->TryGetObject(Obj) || !Obj)
        {
            return nullptr;
        }
        return *Obj;
    }

    FGuid GuidField(const TSharedPtr<FJsonObject>& Row, const TCHAR* Field)
    {
        FString Str;
        FGuid Parsed;
        if (Row.IsValid() && Row->TryGetStringField(Field, Str))
        {
            FGuid::Parse(Str, Parsed);
        }
        return Parsed;
    }

    // The declared param spec for a registered method, or nullptr.
    const TArray<FParamSpec>* FindParamSpec(const FString& MethodName)
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName == MethodName)
            {
                return &Reg.Params;
            }
        }
        return nullptr;
    }

    bool SpecDeclaresName(const TArray<FParamSpec>* Params, const FString& Name)
    {
        if (!Params)
        {
            return false;
        }
        for (const FParamSpec& Spec : *Params)
        {
            if (Spec.Name == Name || Spec.Aliases.Contains(Name))
            {
                return true;
            }
            for (const FParamAliasSpec& Alias : Spec.TypedAliases)
            {
                if (Alias.Name == Name)
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Shared preamble: editor + subsystem + world, all hard failures. Returns false when the test
    // cannot run, having already logged the reason as an ERROR (never a skip).
    bool GetCompBindEnvironment(FAutomationTestBase& Test, UPinWrightSubsystem*& OutSubsystem,
        UWorld*& OutWorld)
    {
        OutSubsystem = nullptr;
        OutWorld = nullptr;
        if (!GEditor)
        {
            Test.AddError(TEXT("GEditor unavailable - cannot exercise the sequencer binding verbs."));
            return false;
        }
        OutSubsystem = GEditor->GetEditorSubsystem<UPinWrightSubsystem>();
        if (!OutSubsystem)
        {
            Test.AddError(TEXT("PinWright subsystem unavailable - sequencer.add_actor resolves its "
                               "actor through it, so the case cannot be exercised."));
            return false;
        }
        OutWorld = GEditor->GetEditorWorldContext().World();
        if (!OutWorld)
        {
            Test.AddError(TEXT("No editor world - the probe actor has nowhere to live."));
            return false;
        }
        return true;
    }
}

// ============================================================================
// The component binding is created, and its parent is the actor's binding
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerComponentBindingParentedTest,
    "PinWright.Sequencer.ComponentBinding.ComponentBindingIsParentedToTheActorBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerComponentBindingParentedTest::RunTest(const FString& Parameters)
{
    UPinWrightSubsystem* Subsystem = nullptr;
    UWorld* World = nullptr;
    if (!GetCompBindEnvironment(*this, Subsystem, World))
    {
        return false;
    }

    FScopedEditorWorldActorGuard Guard;

    FString SeqPath;
    ULevelSequence* Sequence = CreateCompBindSequence(*this, SeqPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("probe sequence has a MovieScene"), MovieScene))
    {
        return false;
    }

    const FString ActorLabel = FString::Printf(TEXT("PWCompBindSiege_%sX"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UStaticMeshComponent* Wheel = nullptr;
    AStaticMeshActor* Actor = SpawnCompBindProbeActor(ActorLabel, Wheel);
    if (!TestNotNull(TEXT("probe actor spawned into the editor world"), Actor) ||
        !TestNotNull(TEXT("probe wheel component created on the actor"), Wheel))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), SeqPath);
    Payload->SetStringField(TEXT("actorName"), ActorLabel);
    Payload->SetStringField(TEXT("componentName"), PWCompBindWheelName);
    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("sequencer.add_actor is registered and was invoked"),
            InvokeCompBindHandler(TEXT("sequencer.add_actor"), Payload, Subsystem, Capture)))
    {
        return false;
    }
    if (!TestTrue(FString::Printf(TEXT("sequencer.add_actor succeeded with a componentName (code=%s msg=%s)"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Row = FirstResultRow(Capture);
    if (!TestTrue(TEXT("response carries a results[0] object"), Row.IsValid()))
    {
        return false;
    }

    bool bRowSuccess = false;
    Row->TryGetBoolField(TEXT("success"), bRowSuccess);
    TestTrue(TEXT("results[0].success is true"), bRowSuccess);

    FString ReportedKind;
    Row->TryGetStringField(TEXT("bindingKind"), ReportedKind);
    TestEqual(TEXT("the row declares itself a component binding"), ReportedKind, FString(TEXT("component")));

    FString ReportedComponent;
    Row->TryGetStringField(TEXT("componentName"), ReportedComponent);
    TestEqual(TEXT("the row names the component that was resolved"),
        ReportedComponent, FString(PWCompBindWheelName));

    const FGuid ChildGuid = GuidField(Row, TEXT("bindingGuid"));
    const FGuid ParentGuid = GuidField(Row, TEXT("parentBindingGuid"));
    if (!TestTrue(TEXT("the reported component bindingGuid is a valid GUID"), ChildGuid.IsValid()))
    {
        return false;
    }
    if (!TestTrue(TEXT("the reported parentBindingGuid is a valid GUID - a component binding without "
                       "a parent resolves to nothing at playback"), ParentGuid.IsValid()))
    {
        return false;
    }
    TestNotEqual(TEXT("the component binding and its parent are different bindings"),
        ChildGuid, ParentGuid);

    // The response is only a claim. Read the parent back off the SEQUENCE and require the two to
    // agree - this is what distinguishes a real SetParent from an echoed request.
    FMovieScenePossessable* Child = MovieScene->FindPossessable(ChildGuid);
    if (!TestNotNull(TEXT("the reported component binding exists on the movie scene"), Child))
    {
        return false;
    }
    TestEqual(TEXT("FMovieScenePossessable::GetParent() on the sequence equals the reported parent"),
        Child->GetParent(), ParentGuid);

    FMovieScenePossessable* Parent = MovieScene->FindPossessable(ParentGuid);
    if (!TestNotNull(TEXT("the parent binding exists on the movie scene"), Parent))
    {
        return false;
    }
    TestFalse(TEXT("the parent (actor) binding is itself top-level"), Parent->GetParent().IsValid());

    // Two bindings, not one: the actor's, minted on the way, plus the component's.
    TestEqual(TEXT("binding the component produced exactly two possessables (actor + component)"),
        MovieScene->GetPossessableCount(), 2);

    // The parent binding is the ACTOR's, measured by resolving it rather than by its name.
    const TArray<UObject*> ParentObjects =
        SequencerBindingUtils::ResolveBoundObjects(Sequence, ParentGuid);
    TestTrue(TEXT("the parent binding resolves to the probe actor"),
        ParentObjects.Contains(static_cast<UObject*>(Actor)));

    return true;
}

// ============================================================================
// get_bindings publishes the hierarchy, so a caller can see what it just made
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerComponentBindingGetBindingsParentTest,
    "PinWright.Sequencer.ComponentBinding.GetBindingsReportsTheParentLink",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerComponentBindingGetBindingsParentTest::RunTest(const FString& Parameters)
{
    UPinWrightSubsystem* Subsystem = nullptr;
    UWorld* World = nullptr;
    if (!GetCompBindEnvironment(*this, Subsystem, World))
    {
        return false;
    }

    FScopedEditorWorldActorGuard Guard;

    FString SeqPath;
    ULevelSequence* Sequence = CreateCompBindSequence(*this, SeqPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };

    const FString ActorLabel = FString::Printf(TEXT("PWCompBindRows_%sX"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UStaticMeshComponent* Wheel = nullptr;
    AStaticMeshActor* Actor = SpawnCompBindProbeActor(ActorLabel, Wheel);
    if (!TestNotNull(TEXT("probe actor spawned"), Actor) ||
        !TestNotNull(TEXT("probe wheel component created"), Wheel))
    {
        return false;
    }

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("path"), SeqPath);
    AddPayload->SetStringField(TEXT("actorName"), ActorLabel);
    AddPayload->SetStringField(TEXT("componentName"), PWCompBindWheelName);
    FTestResponseCapture AddCapture;
    InvokeCompBindHandler(TEXT("sequencer.add_actor"), AddPayload, Subsystem, AddCapture);
    if (!TestTrue(FString::Printf(TEXT("add_actor with componentName succeeded (code=%s msg=%s)"),
            *AddCapture.ErrorCode, *AddCapture.Message), AddCapture.bSuccess))
    {
        return false;
    }
    TSharedPtr<FJsonObject> AddRow = FirstResultRow(AddCapture);
    const FGuid ChildGuid = GuidField(AddRow, TEXT("bindingGuid"));
    const FGuid ParentGuid = GuidField(AddRow, TEXT("parentBindingGuid"));
    if (!TestTrue(TEXT("add_actor returned both a component and a parent GUID"),
            ChildGuid.IsValid() && ParentGuid.IsValid()))
    {
        return false;
    }

    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("path"), SeqPath);
    FTestResponseCapture ListCapture;
    if (!TestTrue(TEXT("sequencer.get_bindings is registered and was invoked"),
            InvokeHandlerWithCapture(TEXT("sequencer.get_bindings"), ListPayload, ListCapture)))
    {
        return false;
    }
    if (!TestTrue(TEXT("sequencer.get_bindings succeeded"), ListCapture.bSuccess))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    if (!TestTrue(TEXT("get_bindings returned a bindings array"),
            ListCapture.Result.IsValid() &&
            ListCapture.Result->TryGetArrayField(TEXT("bindings"), Rows) && Rows))
    {
        return false;
    }
    TestEqual(TEXT("get_bindings lists both the actor and the component binding"), Rows->Num(), 2);

    bool bSawChild = false;
    bool bSawParent = false;
    FString ChildParentId;
    FString ChildKind;
    FString ParentParentId;
    for (const TSharedPtr<FJsonValue>& Value : *Rows)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj)
        {
            continue;
        }
        FString Id;
        (*Obj)->TryGetStringField(TEXT("id"), Id);
        if (Id == ChildGuid.ToString())
        {
            bSawChild = true;
            (*Obj)->TryGetStringField(TEXT("parentId"), ChildParentId);
            (*Obj)->TryGetStringField(TEXT("kind"), ChildKind);
        }
        else if (Id == ParentGuid.ToString())
        {
            bSawParent = true;
            (*Obj)->TryGetStringField(TEXT("parentId"), ParentParentId);
        }
    }

    if (!TestTrue(TEXT("get_bindings lists the component binding"), bSawChild) ||
        !TestTrue(TEXT("get_bindings lists the parent actor binding"), bSawParent))
    {
        return false;
    }

    // The failure direction: an empty parentId here is exactly the state in which the component
    // binding silently animates nothing, and is indistinguishable from an actor binding in the
    // output. Assert it is populated, and that it names the right binding.
    TestFalse(TEXT("the component row's parentId is not empty"), ChildParentId.IsEmpty());
    TestEqual(TEXT("the component row's parentId names the actor binding"),
        ChildParentId, ParentGuid.ToString());
    TestEqual(TEXT("the component row is reported as a possessable"),
        ChildKind, FString(TEXT("possessable")));
    TestTrue(TEXT("the actor row is reported as top-level (empty parentId)"),
        ParentParentId.IsEmpty());

    return true;
}

// ============================================================================
// Omitting componentName is unchanged: one actor binding, no hierarchy
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerComponentBindingOmittedIsUnchangedTest,
    "PinWright.Sequencer.ComponentBinding.OmittedComponentNameBindsTheActorUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerComponentBindingOmittedIsUnchangedTest::RunTest(const FString& Parameters)
{
    UPinWrightSubsystem* Subsystem = nullptr;
    UWorld* World = nullptr;
    if (!GetCompBindEnvironment(*this, Subsystem, World))
    {
        return false;
    }

    FScopedEditorWorldActorGuard Guard;

    FString SeqPath;
    ULevelSequence* Sequence = CreateCompBindSequence(*this, SeqPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("probe sequence has a MovieScene"), MovieScene))
    {
        return false;
    }

    const FString ActorLabel = FString::Printf(TEXT("PWCompBindPlain_%sX"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UStaticMeshComponent* Wheel = nullptr;
    AStaticMeshActor* Actor = SpawnCompBindProbeActor(ActorLabel, Wheel);
    if (!TestNotNull(TEXT("probe actor spawned"), Actor) ||
        !TestNotNull(TEXT("probe wheel component created (present but deliberately NOT named)"), Wheel))
    {
        return false;
    }

    // No componentName field at all - the pre-existing call shape.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), SeqPath);
    Payload->SetStringField(TEXT("actorName"), ActorLabel);
    FTestResponseCapture Capture;
    InvokeCompBindHandler(TEXT("sequencer.add_actor"), Payload, Subsystem, Capture);
    if (!TestTrue(TEXT("sequencer.add_actor without componentName succeeded"), Capture.bSuccess))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Row = FirstResultRow(Capture);
    if (!TestTrue(TEXT("response carries a results[0] object"), Row.IsValid()))
    {
        return false;
    }
    bool bRowSuccess = false;
    Row->TryGetBoolField(TEXT("success"), bRowSuccess);
    TestTrue(TEXT("results[0].success is true"), bRowSuccess);

    const FGuid ActorGuid = GuidField(Row, TEXT("bindingGuid"));
    TestTrue(TEXT("the actor path still returns a valid bindingGuid"), ActorGuid.IsValid());

    // The component-path fields must not appear on a call that did not ask for a component.
    TestFalse(TEXT("no componentName field on the actor-only response"),
        Row->HasField(TEXT("componentName")));
    TestFalse(TEXT("no parentBindingGuid field on the actor-only response"),
        Row->HasField(TEXT("parentBindingGuid")));
    TestFalse(TEXT("no bindingKind field on the actor-only response"),
        Row->HasField(TEXT("bindingKind")));

    // One binding, and it is top-level.
    TestEqual(TEXT("exactly one possessable was created"), MovieScene->GetPossessableCount(), 1);
    FMovieScenePossessable* Possessable = MovieScene->FindPossessable(ActorGuid);
    if (!TestNotNull(TEXT("the returned binding exists on the movie scene"), Possessable))
    {
        return false;
    }
    TestFalse(TEXT("an actor binding has no parent"), Possessable->GetParent().IsValid());

    // And it is the ACTOR that is bound, not any component of it.
    const TArray<UObject*> Bound = SequencerBindingUtils::ResolveBoundObjects(Sequence, ActorGuid);
    TestTrue(TEXT("the binding resolves to the actor"),
        Bound.Contains(static_cast<UObject*>(Actor)));
    TestFalse(TEXT("the binding does not resolve to the wheel component"),
        Bound.Contains(static_cast<UObject*>(Wheel)));

    return true;
}

// ============================================================================
// A component that does not exist is refused, and nothing is bound
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerComponentBindingMissingComponentTest,
    "PinWright.Sequencer.ComponentBinding.MissingComponentIsRefusedAndBindsNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerComponentBindingMissingComponentTest::RunTest(const FString& Parameters)
{
    UPinWrightSubsystem* Subsystem = nullptr;
    UWorld* World = nullptr;
    if (!GetCompBindEnvironment(*this, Subsystem, World))
    {
        return false;
    }

    FScopedEditorWorldActorGuard Guard;

    FString SeqPath;
    ULevelSequence* Sequence = CreateCompBindSequence(*this, SeqPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("probe sequence has a MovieScene"), MovieScene))
    {
        return false;
    }

    const FString ActorLabel = FString::Printf(TEXT("PWCompBindMiss_%sX"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UStaticMeshComponent* Wheel = nullptr;
    AStaticMeshActor* Actor = SpawnCompBindProbeActor(ActorLabel, Wheel);
    if (!TestNotNull(TEXT("probe actor spawned"), Actor) ||
        !TestNotNull(TEXT("probe wheel component created"), Wheel))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("path"), SeqPath);
    Payload->SetStringField(TEXT("actorName"), ActorLabel);
    Payload->SetStringField(TEXT("componentName"), TEXT("PWNoSuchComponent"));
    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("sequencer.add_actor is registered and was invoked"),
            InvokeCompBindHandler(TEXT("sequencer.add_actor"), Payload, Subsystem, Capture)))
    {
        return false;
    }

    TestFalse(TEXT("a component that is not on the actor is refused, not bound to something else"),
        Capture.bSuccess);
    TestEqual(TEXT("the refusal is typed COMPONENT_NOT_FOUND"),
        Capture.ErrorCode, FString(ErrorCodes::ERR_COMPONENT_NOT_FOUND));

    // The way out has to be in the structured payload, not only in the prose.
    if (TestTrue(TEXT("the refusal carries a structured payload"), Capture.Result.IsValid()))
    {
        TestTrue(TEXT("availableComponents names the wheel the caller could have asked for"),
            JsonStringArrayContains(Capture.Result, TEXT("availableComponents"),
                FString(PWCompBindWheelName)));
        TestTrue(TEXT("availableComponents names the actor's root static mesh component"),
            JsonStringArrayContains(Capture.Result, TEXT("availableComponents"),
                Actor->GetStaticMeshComponent()->GetName()));
    }

    // Never bind a placeholder: the refusal must leave the sequence exactly as it found it, with no
    // actor binding minted "on the way" to a component that does not exist.
    TestEqual(TEXT("no possessable was created by the refused call"),
        MovieScene->GetPossessableCount(), 0);
    TestEqual(TEXT("no binding was created by the refused call"),
        const_cast<const UMovieScene*>(MovieScene)->GetBindings().Num(), 0);

    return true;
}

// ============================================================================
// The one that matters: a track on the component binding drives the COMPONENT
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerComponentBindingAnimatesComponentTest,
    "PinWright.Sequencer.ComponentBinding.BoundComponentAnimatesNotTheActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerComponentBindingAnimatesComponentTest::RunTest(const FString& Parameters)
{
    UPinWrightSubsystem* Subsystem = nullptr;
    UWorld* World = nullptr;
    if (!GetCompBindEnvironment(*this, Subsystem, World))
    {
        return false;
    }

    FScopedEditorWorldActorGuard Guard;

    FString SeqPath;
    ULevelSequence* Sequence = CreateCompBindSequence(*this, SeqPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("probe sequence has a MovieScene"), MovieScene))
    {
        return false;
    }

    const FString ActorLabel = FString::Printf(TEXT("PWCompBindSpin_%sX"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UStaticMeshComponent* Wheel = nullptr;
    AStaticMeshActor* Actor = SpawnCompBindProbeActor(ActorLabel, Wheel);
    if (!TestNotNull(TEXT("probe actor spawned"), Actor) ||
        !TestNotNull(TEXT("probe wheel component created"), Wheel))
    {
        return false;
    }

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("path"), SeqPath);
    AddPayload->SetStringField(TEXT("actorName"), ActorLabel);
    AddPayload->SetStringField(TEXT("componentName"), PWCompBindWheelName);
    FTestResponseCapture AddCapture;
    InvokeCompBindHandler(TEXT("sequencer.add_actor"), AddPayload, Subsystem, AddCapture);
    if (!TestTrue(FString::Printf(TEXT("add_actor bound the component (code=%s msg=%s)"),
            *AddCapture.ErrorCode, *AddCapture.Message), AddCapture.bSuccess))
    {
        return false;
    }
    TSharedPtr<FJsonObject> Row = FirstResultRow(AddCapture);
    const FGuid ChildGuid = GuidField(Row, TEXT("bindingGuid"));
    const FGuid ParentGuid = GuidField(Row, TEXT("parentBindingGuid"));
    if (!TestTrue(TEXT("both GUIDs came back valid"), ChildGuid.IsValid() && ParentGuid.IsValid()))
    {
        return false;
    }

    // Author a real transform track on the component binding, through the PRODUCTION verb - the same
    // call a caller would make to spin a wheel. add_transform_track needs no change for this to work;
    // if it did, this assertion is where that would surface.
    TSharedPtr<FJsonObject> TrackPayload = MakeShared<FJsonObject>();
    TrackPayload->SetStringField(TEXT("sequencePath"), SeqPath);
    TrackPayload->SetStringField(TEXT("bindingGuid"), ChildGuid.ToString());
    FTestResponseCapture TrackCapture;
    if (!TestTrue(TEXT("sequencer.add_transform_track is registered and was invoked"),
            InvokeHandlerWithCapture(TEXT("sequencer.add_transform_track"), TrackPayload, TrackCapture)))
    {
        return false;
    }
    if (!TestTrue(FString::Printf(TEXT("add_transform_track accepted the component binding (code=%s msg=%s)"),
            *TrackCapture.ErrorCode, *TrackCapture.Message), TrackCapture.bSuccess))
    {
        return false;
    }

    // The track landed on the COMPONENT binding, and only there.
    UMovieScene3DTransformTrack* ComponentTrack =
        MovieScene->FindTrack<UMovieScene3DTransformTrack>(ChildGuid);
    TestNotNull(TEXT("the transform track is attached to the component binding"), ComponentTrack);
    TestNull(TEXT("no transform track was attached to the actor binding"),
        MovieScene->FindTrack<UMovieScene3DTransformTrack>(ParentGuid));

    // And the component binding resolves, through the runtime resolution path, to the COMPONENT.
    // This is the whole point: with the parent link missing the same call chain would have returned
    // an empty set here while everything above still looked correct.
    const TArray<UObject*> ChildObjects =
        SequencerBindingUtils::ResolveBoundObjects(Sequence, ChildGuid);
    TestEqual(TEXT("the component binding resolves to exactly one object"), ChildObjects.Num(), 1);
    TestTrue(TEXT("the component binding resolves to the wheel component"),
        ChildObjects.Contains(static_cast<UObject*>(Wheel)));
    TestFalse(TEXT("the component binding does NOT resolve to the owning actor"),
        ChildObjects.Contains(static_cast<UObject*>(Actor)));
    if (ChildObjects.Num() == 1 && ChildObjects[0])
    {
        UActorComponent* ResolvedComponent = Cast<UActorComponent>(ChildObjects[0]);
        if (TestNotNull(TEXT("the resolved object is a UActorComponent, not an AActor"), ResolvedComponent))
        {
            TestEqual(TEXT("the resolved component is owned by the probe actor"),
                ResolvedComponent->GetOwner(), static_cast<AActor*>(Actor));
            TestEqual(TEXT("the resolved component carries the requested name"),
                ResolvedComponent->GetName(), FString(PWCompBindWheelName));
        }
    }

    // The other direction: the parent binding still means the actor, not the component.
    const TArray<UObject*> ParentObjects =
        SequencerBindingUtils::ResolveBoundObjects(Sequence, ParentGuid);
    TestTrue(TEXT("the parent binding resolves to the actor"),
        ParentObjects.Contains(static_cast<UObject*>(Actor)));
    TestFalse(TEXT("the parent binding does not resolve to the wheel component"),
        ParentObjects.Contains(static_cast<UObject*>(Wheel)));

    return true;
}

// ============================================================================
// End to end: the sequence is evaluated and the COMPONENT is what moves
// ============================================================================

// The strongest claim this feature makes, and the only one a dead binding cannot satisfy.
// Everything cheaper passes on the failure being guarded against: the binding exists, its parent is
// set, get_bindings reports the hierarchy, and sequencer.get_binding_transform reports a moving
// transform — because it interrogates the AUTHORED track and never asks what the GUID resolves to.
//
// So this test evaluates the sequence onto the world and reads the objects back, with three
// assertions that fail in three distinguishable ways:
//   - a CONTROL actor with its own actor-level transform track must move. If it does not, the
//     evaluation machinery did not run and the other two assertions mean nothing — this is what
//     stops an environment failure from being read as a binding defect.
//   - the bound WHEEL COMPONENT's relative transform must change. This is the claim.
//   - the wheel's OWNING ACTOR must not move. Its binding exists (it is the wheel's parent) and
//     carries no track, so a component track leaking onto the actor would show up here.
//
// ALevelSequenceActor::InitializePlayer() no-ops outside a game world (LevelSequenceActor.cpp:386
// gates on IsGameWorld), so the player is initialized explicitly against the persistent level.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerComponentBindingEvaluatesOntoComponentTest,
    "PinWright.Sequencer.ComponentBinding.EvaluatedSequenceMovesTheComponentNotItsActor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerComponentBindingEvaluatesOntoComponentTest::RunTest(const FString& Parameters)
{
    UPinWrightSubsystem* Subsystem = nullptr;
    UWorld* World = nullptr;
    if (!GetCompBindEnvironment(*this, Subsystem, World))
    {
        return false;
    }
    ULevel* PersistentLevel = World->PersistentLevel;
    if (!TestNotNull(TEXT("editor world has a persistent level to play into"), PersistentLevel))
    {
        return false;
    }

    FScopedEditorWorldActorGuard Guard;

    FString SeqPath;
    ULevelSequence* Sequence = CreateCompBindSequence(*this, SeqPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(SeqPath); };
    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("probe sequence has a MovieScene"), MovieScene))
    {
        return false;
    }

    // A 30-display-frame range, converted to the movie scene's own tick resolution rather than
    // assumed — a bare tick number would silently key 0.00125 s of animation at 24000 ticks/s.
    const int32 EndDisplayFrame = 30;
    const FFrameNumber EndTick = FFrameRate::TransformTime(
        FFrameTime(FFrameNumber(EndDisplayFrame)),
        MovieScene->GetDisplayRate(), MovieScene->GetTickResolution()).FloorToFrame();
    MovieScene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), EndTick));

    const FVector ActorAStart(1000.0, 0.0, 0.0);
    const FString ActorALabel = FString::Printf(TEXT("PWCompBindEvalRig_%sX"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString ActorBLabel = FString::Printf(TEXT("PWCompBindEvalCtl_%sX"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    UStaticMeshComponent* Wheel = nullptr;
    AStaticMeshActor* ActorA = SpawnCompBindProbeActor(ActorALabel, Wheel);
    UStaticMeshComponent* UnusedWheel = nullptr;
    AStaticMeshActor* ActorB = SpawnCompBindProbeActor(ActorBLabel, UnusedWheel);
    if (!TestNotNull(TEXT("rig probe actor spawned"), ActorA) ||
        !TestNotNull(TEXT("rig probe wheel component created"), Wheel) ||
        !TestNotNull(TEXT("control probe actor spawned"), ActorB))
    {
        return false;
    }
    ActorA->SetActorLocation(ActorAStart);
    ActorB->SetActorLocation(FVector::ZeroVector);

    // Bind the wheel COMPONENT of A, and the whole of B as the evaluation control.
    TSharedPtr<FJsonObject> WheelPayload = MakeShared<FJsonObject>();
    WheelPayload->SetStringField(TEXT("path"), SeqPath);
    WheelPayload->SetStringField(TEXT("actorName"), ActorALabel);
    WheelPayload->SetStringField(TEXT("componentName"), PWCompBindWheelName);
    FTestResponseCapture WheelCapture;
    InvokeCompBindHandler(TEXT("sequencer.add_actor"), WheelPayload, Subsystem, WheelCapture);
    if (!TestTrue(FString::Printf(TEXT("the wheel component bound (code=%s msg=%s)"),
            *WheelCapture.ErrorCode, *WheelCapture.Message), WheelCapture.bSuccess))
    {
        return false;
    }
    TSharedPtr<FJsonObject> WheelRow = FirstResultRow(WheelCapture);
    const FGuid WheelGuid = GuidField(WheelRow, TEXT("bindingGuid"));
    const FGuid ActorAGuid = GuidField(WheelRow, TEXT("parentBindingGuid"));

    TSharedPtr<FJsonObject> ControlPayload = MakeShared<FJsonObject>();
    ControlPayload->SetStringField(TEXT("path"), SeqPath);
    ControlPayload->SetStringField(TEXT("actorName"), ActorBLabel);
    FTestResponseCapture ControlCapture;
    InvokeCompBindHandler(TEXT("sequencer.add_actor"), ControlPayload, Subsystem, ControlCapture);
    const FGuid ControlGuid = GuidField(FirstResultRow(ControlCapture), TEXT("bindingGuid"));
    if (!TestTrue(TEXT("all three bindings are valid (wheel, its parent actor, control actor)"),
            WheelGuid.IsValid() && ActorAGuid.IsValid() && ControlGuid.IsValid()))
    {
        return false;
    }

    // Tracks: X on the wheel component, Y on the control actor, nothing on the wheel's owner.
    if (!TestTrue(TEXT("a transform track was added to the component binding"),
            AddCompBindTransformTrack(SeqPath, WheelGuid)) ||
        !TestTrue(TEXT("a transform track was added to the control actor binding"),
            AddCompBindTransformTrack(SeqPath, ControlGuid)))
    {
        return false;
    }
    const double WheelTravelX = 500.0;
    const double ControlTravelY = 700.0;
    if (!TestTrue(TEXT("the component track carries a Location.X ramp"),
            AuthorCompBindLocationRamp(MovieScene, WheelGuid, 0, 0.0, WheelTravelX, EndTick)) ||
        !TestTrue(TEXT("the control track carries a Location.Y ramp"),
            AuthorCompBindLocationRamp(MovieScene, ControlGuid, 1, 0.0, ControlTravelY, EndTick)))
    {
        return false;
    }
    TestNull(TEXT("the wheel's owning actor binding deliberately carries no transform track"),
        MovieScene->FindTrack<UMovieScene3DTransformTrack>(ActorAGuid));

    ALevelSequenceActor* PlayerActor = nullptr;
    ULevelSequencePlayer* Player = ULevelSequencePlayer::CreateLevelSequencePlayer(
        World, Sequence, FMovieSceneSequencePlaybackSettings(), PlayerActor);
    if (!TestNotNull(TEXT("a ULevelSequencePlayer was created for the editor world"), Player))
    {
        return false;
    }
    Player->Initialize(Sequence, PersistentLevel, FLevelSequenceCameraSettings());
    ON_SCOPE_EXIT
    {
        if (Player) { Player->Stop(); }
        if (IsValid(PlayerActor)) { PlayerActor->Destroy(); }
    };

    Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(
        FFrameTime(FFrameNumber(0)), EUpdatePositionMethod::Jump));
    const FVector WheelAtStart = Wheel->GetRelativeLocation();
    const FVector ActorAAtStart = ActorA->GetActorLocation();
    const FVector ControlAtStart = ActorB->GetActorLocation();

    Player->SetPlaybackPosition(FMovieSceneSequencePlaybackParams(
        FFrameTime(FFrameNumber(EndDisplayFrame)), EUpdatePositionMethod::Jump));
    const FVector WheelAtEnd = Wheel->GetRelativeLocation();
    const FVector ActorAAtEnd = ActorA->GetActorLocation();
    const FVector ControlAtEnd = ActorB->GetActorLocation();

    // CONTROL first. If the evaluation machinery did not run at all, this is the assertion that
    // says so, and the two below are then uninformative rather than evidence of a dead binding.
    TestTrue(FString::Printf(
            TEXT("CONTROL: an actor-bound transform track moved its actor across the range "
                 "(Y %.1f -> %.1f, expected ~%.1f). A failure here means the sequence was not "
                 "evaluated, and the component assertions below prove nothing either way."),
            ControlAtStart.Y, ControlAtEnd.Y, ControlTravelY),
        FMath::IsNearlyEqual(ControlAtEnd.Y - ControlAtStart.Y, ControlTravelY, 1.0));

    // THE CLAIM: the component the caller bound is what the track drives.
    TestTrue(FString::Printf(
            TEXT("the bound COMPONENT moved across the range (relative X %.1f -> %.1f, expected "
                 "~%.1f). A binding that resolves to nothing leaves this at 0."),
            WheelAtStart.X, WheelAtEnd.X, WheelTravelX),
        FMath::IsNearlyEqual(WheelAtEnd.X - WheelAtStart.X, WheelTravelX, 1.0));

    // And the animation stayed on the component: the owning actor, whose binding exists but carries
    // no track, is exactly where it was put.
    TestTrue(FString::Printf(
            TEXT("the wheel's OWNING ACTOR did not move (%s -> %s)"),
            *ActorAAtStart.ToString(), *ActorAAtEnd.ToString()),
        ActorAAtEnd.Equals(ActorAAtStart, 0.5) && ActorAAtEnd.Equals(ActorAStart, 0.5));

    return true;
}

// ============================================================================
// The wire parameter is declared, or the dispatcher rejects every call using it
// ============================================================================

// The handler-level tests above invoke the registration directly and therefore bypass
// FRpcDispatcher::ValidateHandlerParams. That gate rejects any field not present in the method's
// RPC_PARAMS with UNKNOWN_PARAMS, so a componentName that works in-process would still be
// unreachable over the wire if the spec were ever dropped. Pin the declaration itself.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerComponentBindingParamIsDeclaredTest,
    "PinWright.Sequencer.ComponentBinding.ComponentNameParamIsDeclaredOnBothVerbs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerComponentBindingParamIsDeclaredTest::RunTest(const FString& Parameters)
{
    for (const TCHAR* Method : { TEXT("sequencer.add_actor"), TEXT("sequencer.add_actors") })
    {
        const TArray<FParamSpec>* Params = FindParamSpec(Method);
        if (!TestNotNull(*FString::Printf(TEXT("%s is registered"), Method), Params))
        {
            continue;
        }
        TestTrue(*FString::Printf(TEXT("%s declares componentName, so the dispatcher accepts it"), Method),
            SpecDeclaresName(Params, TEXT("componentName")));
        TestTrue(*FString::Printf(TEXT("%s declares the component_name snake_case alias"), Method),
            SpecDeclaresName(Params, TEXT("component_name")));
        // Failure direction: an undeclared name must NOT be reported as declared, or the assertion
        // above would pass against a spec walker that returns true for everything.
        TestFalse(*FString::Printf(TEXT("%s does not declare a bogus parameter"), Method),
            SpecDeclaresName(Params, TEXT("componentNam")));
    }
    return true;
}
