// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for F-sequencer-camera-rig-possessable-unbound.
//
// sequencer.add_camera_rig_rail / add_camera_rig_crane share AddCameraRigTrackInternal
// (SequencerHandler.cpp). Its actorPath branch loads the rig actor and calls
// UMovieScene::AddPossessable(Label, Class) to mint a possessable, returning success:true
// + a bindingGuid — but it NEVER calls ULevelSequence::BindPossessableObject, so no
// FLevelSequenceBindingReference is written for that GUID. The possessable is object-
// UNBOUND: the reported bindingGuid resolves to no object, so sequencer playback drives
// nothing and the editor shows a needs-rebinding track. (The else/spawnable branch uses
// AddSpawnable, carries its own template, and is correctly unaffected.)
//
// This test spawns a real ACameraRig_Rail in the editor world, confirms the actor lives in
// GEditor->GetEditorWorldContext().World() (the world the bind context must match — the one
// thing that differs from B's FindActorByName/spawn path), drives the PRODUCTION
// sequencer.add_camera_rig_rail handler through its actorPath branch, then asserts the
// CORRECT behavior via the reverse lookup (ULevelSequence::FindBindingFromObject): the
// returned bindingGuid must resolve back to the rig actor it named.
//
// Differential property: pre-fix no binding reference exists, so FindBindingFromObject
// returns an invalid GUID that neither is valid nor equals the reported bindingGuid — the
// two correct-behavior assertions FAIL, reproducing the object-unbound-possessable defect.
// With the fix (BindPossessableObject(BindingGuid, *RigActor, World) added after
// AddPossessable in the actorPath branch) the reverse lookup returns the GUID and the
// assertions flip green. The asserted success:true / mode=="possessed" are correct both
// pre- and post-fix: they document the false-success context and prove the actorPath
// (possessable) branch was exercised — the binding-resolution assertions are what
// distinguish fixed from broken.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "CameraRig_Rail.h"
#include "Engine/World.h"

#include "Utils/AssetUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    // Distinctly named (mirrors CreateAddActorBindingSequence / CreateControlRigTrackSequence
    // in the sibling sequencer test .cpp files) so anonymous-namespace symbols don't ODR-
    // collide when Unity merges these TUs: create a real /Game LevelSequence via the
    // registered sequencer.create handler so the add_camera_rig_rail asset load can resolve
    // it. Empty path + nullptr on failure.
    ULevelSequence* CreateCameraRigBindingSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_CamRigBindSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_CamRigBindProbe");
        OutFullPath = FString::Printf(TEXT("%s/%s"), *DestFolder, *SeqName);

        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), SeqName);
        CreatePayload->SetStringField(TEXT("path"), DestFolder);
        FTestResponseCapture CreateCapture;
        InvokeHandlerWithCapture(TEXT("sequencer.create"), CreatePayload, CreateCapture);

        if (!CreateCapture.bWasCalled || !CreateCapture.bSuccess ||
            !UEditorAssetLibrary::DoesAssetExist(OutFullPath))
        {
            Test.AddError(TEXT("Could not create a probe LevelSequence via sequencer.create — "
                               "the camera-rig binding repro cannot be exercised without it."));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerCameraRigRailBindsPossessableTest,
    "PinWright.Sequencer.CameraRigRail.BindsPossessableToObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerCameraRigRailBindsPossessableTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    if (!GEditor)
    {
        AddError(TEXT("GEditor unavailable — cannot exercise sequencer.add_camera_rig_rail."));
        return false;
    }

    // The bind context world: the actorPath resolver (LoadObject) and the fix's
    // BindPossessableObject must agree on THIS world, so the rig actor must live in it.
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!TestNotNull(TEXT("editor world present"), World))
    {
        return false;
    }

    // A real /Game LevelSequence for add_camera_rig_rail to bind into.
    FString FullPath;
    ULevelSequence* Sequence = CreateCameraRigBindingSequence(*this, FullPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), Sequence->GetMovieScene()))
    {
        return false;
    }

    // A live ACameraRig_Rail in the editor world — the target the actorPath branch possesses.
    // ACameraRig_Rail is exactly the ExpectedClass the rail handler checks with IsA, so the
    // WRONG_ACTOR_CLASS guard passes and the possessable branch runs.
    const FString RigLabel = FString::Printf(TEXT("MCP_CamRigRail_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ACameraRig_Rail* RigActor = SpawnActorInActiveWorld<ACameraRig_Rail>(
        ACameraRig_Rail::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, RigLabel);
    if (!TestNotNull(TEXT("ACameraRig_Rail spawned in the editor world"), RigActor))
    {
        return false;
    }

    // Ticket's world-match requirement: the spawned rig must live in the editor world the
    // fix binds against — otherwise the failure would be a world mismatch, not the defect.
    if (!TestTrue(TEXT("rig actor lives in the editor world context world"),
            RigActor->GetWorld() == World))
    {
        return false;
    }

    // The handler resolves the actorPath via LoadObject<AActor>(nullptr, *ActorPath). Feed it
    // the actor's full object path and confirm — as a fixture precondition — that LoadObject
    // returns THIS exact actor, so a later binding failure is the object-unbound defect and
    // not a path/resolution problem.
    const FString ActorPath = RigActor->GetPathName();
    AActor* Reloaded = LoadObject<AActor>(nullptr, *ActorPath);
    if (!TestTrue(TEXT("rig actorPath resolves back to the spawned actor via LoadObject"),
            Reloaded == RigActor))
    {
        return false;
    }

    // Drive the production sequencer.add_camera_rig_rail handler through its actorPath branch.
    // AddCameraRigTrackInternal loads the actor via LoadObject (not the subsystem), so the
    // null-subsystem capture path is safe here.
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("sequencePath"), FullPath);
    AddPayload->SetStringField(TEXT("actorPath"), ActorPath);
    FTestResponseCapture AddCapture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("sequencer.add_camera_rig_rail"), AddPayload, AddCapture);
    if (!TestTrue(TEXT("sequencer.add_camera_rig_rail handler is registered and invoked"), bFound))
    {
        return false;
    }
    if (!TestTrue(TEXT("sequencer.add_camera_rig_rail reported success"), AddCapture.bSuccess))
    {
        return false;
    }

    // Preconditions documenting the false-success: the handler claims success, reports the
    // possessable branch (mode=="possessed"), and hands back a valid bindingGuid. These hold
    // both pre- and post-fix.
    if (!TestTrue(TEXT("add_camera_rig_rail response is a valid object"), AddCapture.Result.IsValid()))
    {
        return false;
    }
    FString ModeStr;
    AddCapture.Result->TryGetStringField(TEXT("mode"), ModeStr);
    if (!TestEqual(TEXT("handler took the actorPath (possessed) branch, not spawnable"),
            ModeStr, FString(TEXT("possessed"))))
    {
        // A "spawned" mode means the actorPath was ignored — a fixture/wiring problem, not the defect.
        return false;
    }
    FString BindingGuidStr;
    AddCapture.Result->TryGetStringField(TEXT("bindingGuid"), BindingGuidStr);
    FGuid ResponseGuid;
    if (!TestTrue(TEXT("add_camera_rig_rail returned a parseable, valid bindingGuid"),
            FGuid::Parse(BindingGuidStr, ResponseGuid) && ResponseGuid.IsValid()))
    {
        return false;
    }

    // CORRECT-BEHAVIOR ASSERTIONS (the defect surfaces here). The possessable the handler
    // just minted must be object-bound to the rig actor: the reverse lookup must resolve the
    // actor back to the returned bindingGuid. The 5.7-deprecated (UObject* Context) overload
    // is the only headless-viable path across UE 5.3-5.7 (matches B's adopted red test and
    // ControlRigSequencerHandler.cpp).
    //
    // Pre-fix: AddPossessable wrote no FLevelSequenceBindingReference, so FindBindingFromObject
    // returns an invalid GUID — both assertions FAIL, reproducing the unbound possessable.
    FGuid ResolvedGuid;
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    ResolvedGuid = Sequence->FindBindingFromObject(RigActor, World);
    PRAGMA_ENABLE_DEPRECATION_WARNINGS

    TestTrue(TEXT("add_camera_rig_rail bound the possessable to the rig actor "
                  "(FindBindingFromObject resolves a valid binding reference)"),
        ResolvedGuid.IsValid());
    TestTrue(TEXT("the returned bindingGuid resolves back to the bound rig actor "
                  "(reverse lookup equals the reported GUID)"),
        ResolvedGuid == ResponseGuid);

    return true;
}
