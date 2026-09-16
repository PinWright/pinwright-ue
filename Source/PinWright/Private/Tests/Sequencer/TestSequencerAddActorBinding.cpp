// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for B-sequencer-add-actor-unbound-possessable.
//
// sequencer.add_actor (and add_actors / add_camera) resolve the target actor and call
// UMovieScene::AddPossessable(Label, Class) to mint a possessable, returning
// success:true + a bindingGuid — but they NEVER call ULevelSequence::BindPossessableObject,
// so no FLevelSequenceBindingReference is written for that GUID. The possessable is
// object-UNBOUND: the reported bindingGuid resolves to no object, which is why the
// documented add_actor -> add_controlrig_track (FK) path fails BINDING_NOT_SKELETAL even
// though the skeletal-mesh actor is present in the editor world.
//
// This test spawns a real ASkeletalMeshActor in the editor world, drives the PRODUCTION
// sequencer.add_actor handler through a live UPinWrightSubsystem (add_actor dereferences
// Ctx.GetSubsystem()->FindActorByName, so MakeContextWithCapture with a real subsystem is
// required — the null-subsystem InvokeHandlerWithCapture would crash it), then asserts the
// CORRECT behavior via the SAME reverse lookup the FK Control Rig resolver uses
// (ULevelSequence::FindBindingFromObject, ControlRigSequencerHandler.cpp:116): the returned
// bindingGuid must resolve back to the actor it named.
//
// Differential property: pre-fix no binding reference exists, so FindBindingFromObject
// returns an invalid GUID that neither is valid nor equals the reported bindingGuid — the
// two correct-behavior assertions FAIL, reproducing the object-unbound-possessable defect
// exactly. With the fix (BindPossessableObject(BindingGuid, *Found, World) added after
// AddPossessable) the reverse lookup returns the GUID and the assertions flip green. The
// asserted success:true is correct both pre- and post-fix (the handler always reports
// success); it documents the false-success context, not the bug — the binding-resolution
// assertions are what distinguish fixed from broken.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"

#include "PinWrightSubsystem.h"
#include "Utils/AssetUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    // Distinctly named (mirrors CreateControlRigTrackSequence / CreateEvalReadbackSequence in
    // the sibling sequencer test .cpp files) so anonymous-namespace symbols don't ODR-collide
    // when Unity merges these TUs: create a real /Game LevelSequence via the registered
    // sequencer.create handler so the add_actor asset load can resolve it. Empty path +
    // nullptr on failure.
    ULevelSequence* CreateAddActorBindingSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_AddActorBindSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_AddActorBindProbe");
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
                               "the add_actor binding repro cannot be exercised without it."));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // Drive the named handler through a LIVE subsystem while capturing its response.
    // add_actor calls Ctx.GetSubsystem()->FindActorByName, which crashes on the null
    // subsystem InvokeHandlerWithCapture wires; MakeContextWithCapture carries the real
    // subsystem AND captures SendSuccess/SendError. Returns true iff the handler was found.
    bool InvokeHandlerWithSubsystemCapture(const FString& MethodName,
        const TSharedPtr<FJsonObject>& Payload, UPinWrightSubsystem* Subsystem,
        FTestResponseCapture& Capture)
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerAddActorBindsPossessableTest,
    "PinWright.Sequencer.AddActor.BindsPossessableToObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerAddActorBindsPossessableTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    if (!GEditor)
    {
        AddError(TEXT("GEditor unavailable — cannot exercise sequencer.add_actor."));
        return false;
    }

    // add_actor resolves the target actor through the live subsystem. In the editor
    // automation run the subsystem is always initialized; its absence is a fixture
    // FAILURE (not a skip) — the defect cannot be exercised without it.
    UPinWrightSubsystem* Subsystem = GEditor->GetEditorSubsystem<UPinWrightSubsystem>();
    if (!Subsystem)
    {
        AddError(TEXT("PinWright subsystem unavailable — cannot drive sequencer.add_actor."));
        return false;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!TestNotNull(TEXT("editor world present"), World))
    {
        return false;
    }

    // A live skeletal-mesh actor in the editor world — the target add_actor will possess.
    // SKM_Manny is the host's loadable skeletal mesh (SK_Mannequin exists on disk but does
    // not load as a USkeletalMesh here); a small candidates array keeps the fixture robust.
    static const TCHAR* MannyMeshCandidates[] = {
        TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny"),
        TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"),
        TEXT("/Game/Characters/Mannequins/Meshes/SK_Mannequin"),
    };

    // Host-dependent fixture gate (docs/test-organization.md "Host-Dependent Fixtures"):
    // the mannequin mesh is Lyra host content that not every host project ships.
    // PINWRIGHT_SKIP_IF_FIXTURE_MISSING gates a single path, so this multi-candidate
    // variant inlines the same FPackageName::DoesPackageExist check + FIXTURE-SKIP audit
    // token: skip only when NO candidate package exists on this host. A candidate that
    // exists on disk but fails to load stays a hard failure below (regression signal,
    // not a host difference). Gated before the probe-sequence creation so skip hosts
    // don't churn a created-then-force-deleted /Game asset.
    bool bAnyMannyPackageOnDisk = false;
    for (const TCHAR* Candidate : MannyMeshCandidates)
    {
        if (FPackageName::DoesPackageExist(Candidate))
        {
            bAnyMannyPackageOnDisk = true;
            break;
        }
    }
    if (!bAnyMannyPackageOnDisk)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("FIXTURE-SKIP: /Game/Characters/Mannequins/Meshes/SKM_Manny (and fallback "
                         "mannequin mesh candidates) not present in this host project; test requires "
                         "Lyra mannequin content."));
        return true;
    }

    USkeletalMesh* MannyMesh = nullptr;
    for (const TCHAR* Candidate : MannyMeshCandidates)
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
    if (!TestNotNull(TEXT("mannequin fixture skeletal mesh loaded (a candidate package exists "
                          "on this host but none loaded as a USkeletalMesh)"), MannyMesh))
    {
        return false;
    }

    // A real /Game LevelSequence for add_actor to bind into.
    FString FullPath;
    ULevelSequence* Sequence = CreateAddActorBindingSequence(*this, FullPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), Sequence->GetMovieScene()))
    {
        return false;
    }

    const FString MannyLabel = FString::Printf(TEXT("MCP_AddActorBindManny_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    ASkeletalMeshActor* MannyActor = SpawnActorInActiveWorld<ASkeletalMeshActor>(
        ASkeletalMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, MannyLabel);
    if (!TestNotNull(TEXT("skeletal-mesh actor spawned in the editor world"), MannyActor))
    {
        return false;
    }
    MannyActor->GetSkeletalMeshComponent()->SetSkeletalMeshAsset(MannyMesh);

    // Drive the production sequencer.add_actor handler with the sequence path + the actor's
    // label, through the live subsystem so its FindActorByName resolves the spawned actor.
    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("path"), FullPath);
    AddPayload->SetStringField(TEXT("actorName"), MannyLabel);
    FTestResponseCapture AddCapture;
    const bool bFound = InvokeHandlerWithSubsystemCapture(
        TEXT("sequencer.add_actor"), AddPayload, Subsystem, AddCapture);
    if (!TestTrue(TEXT("sequencer.add_actor handler is registered and invoked"), bFound))
    {
        return false;
    }
    if (!TestTrue(TEXT("sequencer.add_actor reported an overall success"), AddCapture.bSuccess))
    {
        return false;
    }

    // Extract results[0]: the per-actor success + the bindingGuid the handler minted.
    // These are preconditions that document the false-success (the handler claims success
    // and hands back a GUID); they hold both pre- and post-fix.
    const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
    if (!TestTrue(TEXT("add_actor response carries a non-empty results array"),
            AddCapture.Result.IsValid() &&
            AddCapture.Result->TryGetArrayField(TEXT("results"), Results) &&
            Results && Results->Num() > 0))
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* FirstObj = nullptr;
    if (!TestTrue(TEXT("results[0] is a JSON object"),
            (*Results)[0].IsValid() && (*Results)[0]->TryGetObject(FirstObj) && FirstObj && (*FirstObj).IsValid()))
    {
        return false;
    }
    bool bItemSuccess = false;
    (*FirstObj)->TryGetBoolField(TEXT("success"), bItemSuccess);
    if (!TestTrue(TEXT("add_actor reported success:true for the actor (actor was resolved)"), bItemSuccess))
    {
        // A false here means the actor was not found — a fixture problem, not the defect.
        return false;
    }
    FString BindingGuidStr;
    (*FirstObj)->TryGetStringField(TEXT("bindingGuid"), BindingGuidStr);
    FGuid ResponseGuid;
    if (!TestTrue(TEXT("add_actor returned a parseable, valid bindingGuid"),
            FGuid::Parse(BindingGuidStr, ResponseGuid) && ResponseGuid.IsValid()))
    {
        return false;
    }

    // CORRECT-BEHAVIOR ASSERTIONS (the defect surfaces here). The possessable the handler
    // just minted must be object-bound to the actor: the SAME reverse lookup the FK Control
    // Rig resolver performs must resolve the actor back to the returned bindingGuid. The
    // 5.7-deprecated (UObject* Context) overload is the only headless-viable path across
    // UE 5.3-5.7 and is exactly what ControlRigSequencerHandler.cpp:116 uses.
    //
    // Pre-fix: AddPossessable wrote no FLevelSequenceBindingReference, so FindBindingFromObject
    // returns an invalid GUID — both assertions FAIL, reproducing the unbound possessable.
    FGuid ResolvedGuid;
    PRAGMA_DISABLE_DEPRECATION_WARNINGS
    ResolvedGuid = Sequence->FindBindingFromObject(MannyActor, World);
    PRAGMA_ENABLE_DEPRECATION_WARNINGS

    TestTrue(TEXT("add_actor bound the possessable to the actor "
                  "(FindBindingFromObject resolves a valid binding reference)"),
        ResolvedGuid.IsValid());
    TestTrue(TEXT("the returned bindingGuid resolves back to the bound actor "
                  "(reverse lookup equals the reported GUID)"),
        ResolvedGuid == ResponseGuid);

    return true;
}
