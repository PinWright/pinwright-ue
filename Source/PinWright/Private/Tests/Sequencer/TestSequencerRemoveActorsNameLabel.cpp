// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red test for E-sequencer-add-remove-actors-name-label-mismatch.
//
// sequencer.add_actors resolves each actorNames entry by the INTERNAL object name
// (UPinWrightSubsystem::FindActorByName matches GetName() among label/name/path,
// SequenceHandler.cpp:922) but stores the binding under the actor's DISPLAY LABEL
// (UMovieScene::AddPossessable(Found->GetActorLabel(), Found->GetClass()),
// SequenceHandler.cpp:936-937). sequencer.remove_actors matches actorNames ONLY
// against the stored binding name (= that label; Possessable->GetName(),
// SequenceHandler.cpp:1126-1134) with no name-or-label resolver, so the exact name
// string add_actors just accepted is rejected with "Actor not found in sequence
// bindings" (SequenceHandler.cpp:1151) and bindingsProcessed:0.
//
// This test spawns a real actor whose internal name (StaticMeshActor_N) differs
// from its display label (SetActorLabel changes only the label; GetName() keeps the
// spawn-assigned object name), binds it with sequencer.add_actors using the INTERNAL
// name (accepted), then calls sequencer.remove_actors with the SAME internal-name
// string and asserts the CORRECT behavior: the identifier that binds an actor also
// unbinds it (removedActors[0].success == true, bindingsProcessed == 1).
//
// Differential property: pre-fix remove_actors matches only the stored label, so the
// internal-name arg finds no binding — the round-trip assertions FAIL, reproducing the
// split-identity defect. With the ticket's fix (route each actorNames entry through the
// same name-or-label resolution add_actors already uses) the internal name resolves to
// the bound actor and the removal succeeds, flipping the assertions green.
//
// add_actors dereferences Ctx.GetSubsystem()->FindActorByName, so both handlers are
// driven through a LIVE UPinWrightSubsystem via MakeContextWithCapture (the
// null-subsystem InvokeHandlerWithCapture would crash add_actors).
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Editor.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneSpawnable.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"

#include "PinWrightSubsystem.h"
#include "Utils/AssetUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace
{
    // Distinctly named (avoids anonymous-namespace ODR collisions with the sibling
    // sequencer test .cpp files when Unity merges these TUs): create a real /Game
    // LevelSequence via the registered sequencer.create handler so the add/remove asset
    // loads resolve it. Empty path + nullptr on failure.
    ULevelSequence* CreateNameLabelMismatchSequence(FAutomationTestBase& Test, FString& OutFullPath)
    {
        const FString SeqName = FString::Printf(TEXT("MCP_SeqNameLabelSeq_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString DestFolder = TEXT("/Game/MCP_SeqNameLabelProbe");
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
                               "the add/remove name-label repro cannot be exercised without it."));
            OutFullPath.Reset();
            return nullptr;
        }
        return Cast<ULevelSequence>(UEditorAssetLibrary::LoadAsset(OutFullPath));
    }

    // Drive the named handler through a LIVE subsystem while capturing its response.
    // sequencer.add_actors dereferences Ctx.GetSubsystem()->FindActorByName, which crashes
    // on the null subsystem InvokeHandlerWithCapture wires; MakeContextWithCapture carries the
    // real subsystem AND captures SendSuccess/SendError. Returns true iff the handler exists.
    // (Distinct name from the sibling's InvokeHandlerWithSubsystemCapture to avoid an
    // anonymous-namespace ODR collision when Unity merges these TUs.)
    bool InvokeSeqHandlerViaLiveSubsystem(const FString& MethodName,
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

    // Wraps a single actorNames entry into the array the sequencer verbs require.
    TSharedPtr<FJsonObject> MakeActorNamesPayload(const FString& SeqPath, const FString& ActorName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), SeqPath);
        TArray<TSharedPtr<FJsonValue>> NamesArr;
        NamesArr.Add(MakeShared<FJsonValueString>(ActorName));
        Payload->SetArrayField(TEXT("actorNames"), NamesArr);
        return Payload;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerRemoveActorsMatchesInternalNameTest,
    "PinWright.Sequencer.RemoveActors.RoundTripsInternalNameAddActorsAccepts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerRemoveActorsMatchesInternalNameTest::RunTest(const FString& Parameters)
{
    FScopedEditorWorldActorGuard WorldGuard;
    if (!GEditor)
    {
        AddError(TEXT("GEditor unavailable — cannot exercise sequencer.add_actors/remove_actors."));
        return false;
    }

    // Both verbs are driven through the live subsystem; add_actors dereferences it. In the
    // editor automation run the subsystem is always initialized; its absence is a fixture
    // FAILURE (not a skip) — the defect cannot be exercised without it.
    UPinWrightSubsystem* Subsystem = GEditor->GetEditorSubsystem<UPinWrightSubsystem>();
    if (!Subsystem)
    {
        AddError(TEXT("PinWright subsystem unavailable — cannot drive sequencer.add_actors."));
        return false;
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!TestNotNull(TEXT("editor world present"), World))
    {
        return false;
    }

    // A real /Game LevelSequence for add_actors to bind into and remove_actors to trim.
    FString FullPath;
    ULevelSequence* Sequence = CreateNameLabelMismatchSequence(*this, FullPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    // A live actor whose DISPLAY LABEL is deliberately distinct from its auto-generated
    // INTERNAL object name — SetActorLabel changes only the label, so GetName() keeps the
    // spawn-assigned StaticMeshActor_N. This name-vs-label split is the whole premise of
    // the ticket; without it there is nothing to reproduce.
    const FString DistinctLabel = FString::Printf(TEXT("MCP_SeqNameLabelActor_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AStaticMeshActor* Actor = SpawnActorInActiveWorld<AStaticMeshActor>(
        AStaticMeshActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, DistinctLabel);
    if (!TestNotNull(TEXT("probe actor spawned in the editor world"), Actor))
    {
        return false;
    }

    const FString InternalName = Actor->GetName();
    const FString Label = Actor->GetActorLabel();

    // Precondition: the internal name and the label must differ, else there is no
    // name-vs-label split to exercise (a fixture failure, not the defect).
    if (!TestTrue(TEXT("the probe actor's internal name differs from its display label "
                       "(precondition for the name-vs-label split)"),
            !InternalName.Equals(Label, ESearchCase::IgnoreCase)))
    {
        return false;
    }

    // --- add_actors with the INTERNAL name (the add side resolves it via FindActorByName) ---
    FTestResponseCapture AddCapture;
    if (!TestTrue(TEXT("sequencer.add_actors handler is registered and invoked"),
            InvokeSeqHandlerViaLiveSubsystem(TEXT("sequencer.add_actors"),
                MakeActorNamesPayload(FullPath, InternalName), Subsystem, AddCapture)))
    {
        return false;
    }
    if (!TestTrue(TEXT("sequencer.add_actors reported an overall success"), AddCapture.bSuccess))
    {
        return false;
    }

    // Precondition: add_actors ACCEPTED the internal name (results[0].success == true). This
    // documents that the add side speaks the internal name — the exact argument we replay to
    // remove_actors. A false here is a fixture problem (actor not resolved), not the defect.
    const TArray<TSharedPtr<FJsonValue>>* AddResults = nullptr;
    if (!TestTrue(TEXT("add_actors response carries a non-empty results array"),
            AddCapture.Result.IsValid() &&
            AddCapture.Result->TryGetArrayField(TEXT("results"), AddResults) &&
            AddResults && AddResults->Num() > 0))
    {
        return false;
    }
    const TSharedPtr<FJsonObject>* AddItem = nullptr;
    if (!TestTrue(TEXT("add_actors results[0] is a JSON object"),
            (*AddResults)[0].IsValid() && (*AddResults)[0]->TryGetObject(AddItem) &&
            AddItem && (*AddItem).IsValid()))
    {
        return false;
    }
    bool bAddItemSuccess = false;
    (*AddItem)->TryGetBoolField(TEXT("success"), bAddItemSuccess);
    if (!TestTrue(TEXT("add_actors accepted the internal name and bound the actor "
                       "(results[0].success == true)"), bAddItemSuccess))
    {
        return false;
    }

    // --- remove_actors with the SAME INTERNAL-name string that add_actors just accepted ---
    FTestResponseCapture RemoveCapture;
    if (!TestTrue(TEXT("sequencer.remove_actors handler is registered and invoked"),
            InvokeSeqHandlerViaLiveSubsystem(TEXT("sequencer.remove_actors"),
                MakeActorNamesPayload(FullPath, InternalName), Subsystem, RemoveCapture)))
    {
        return false;
    }
    if (!TestTrue(TEXT("sequencer.remove_actors reported an overall success envelope"),
            RemoveCapture.bSuccess))
    {
        return false;
    }

    // CORRECT-BEHAVIOR ASSERTIONS (the defect surfaces here). The identifier that bound the
    // actor must also unbind it: remove_actors with the SAME internal name must remove the
    // binding. Pre-fix remove_actors matches only the stored LABEL, so the internal-name arg
    // finds no binding — bindingsProcessed stays 0 and removedActors[0].success is false with
    // "Actor not found in sequence bindings". Post-fix (name-or-label resolver) it removes it.
    double BindingsProcessed = -1.0;
    if (RemoveCapture.Result.IsValid())
    {
        RemoveCapture.Result->TryGetNumberField(TEXT("bindingsProcessed"), BindingsProcessed);
    }

    const TArray<TSharedPtr<FJsonValue>>* RemovedArr = nullptr;
    bool bRemovedItemSuccess = false;
    FString RemovedError;
    if (RemoveCapture.Result.IsValid() &&
        RemoveCapture.Result->TryGetArrayField(TEXT("removedActors"), RemovedArr) &&
        RemovedArr && RemovedArr->Num() > 0)
    {
        const TSharedPtr<FJsonObject>* RemItem = nullptr;
        if ((*RemovedArr)[0].IsValid() && (*RemovedArr)[0]->TryGetObject(RemItem) &&
            RemItem && (*RemItem).IsValid())
        {
            (*RemItem)->TryGetBoolField(TEXT("success"), bRemovedItemSuccess);
            (*RemItem)->TryGetStringField(TEXT("error"), RemovedError);
        }
    }

    TestTrue(*FString::Printf(TEXT("remove_actors removed the binding the same internal name bound "
                                   "(removedActors[0].success == true; observed error='%s')"),
                 *RemovedError),
        bRemovedItemSuccess);
    TestEqual(TEXT("remove_actors processed exactly one binding for the internal name "
                   "(bindingsProcessed == 1)"),
        (int32)BindingsProcessed, 1);

    return true;
}

// Regression test for the spawnable-removal no-op fix (B8). remove_actors matches both
// possessable and spawnable bindings by name, but before the fix it only ever called
// RemovePossessable — which returns false for a spawnable GUID (it searches the Possessables
// array only) — so removing a spawnable was a silent no-op still reported as success:true.
// The fix falls through to RemoveSpawnable and reports success only when a binding was really
// removed. This test adds a spawnable directly (the SUT is remove_actors, not the spawnable
// creator), removes it by its stored binding name, and asserts the spawnable is actually gone
// from the MovieScene and the per-actor result reports a real removal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSequencerRemoveActorsRemovesSpawnableTest,
    "PinWright.Sequencer.RemoveActors.RemovesSpawnableBinding",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSequencerRemoveActorsRemovesSpawnableTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        AddError(TEXT("GEditor unavailable — cannot exercise sequencer.remove_actors."));
        return false;
    }

    // A real /Game LevelSequence to add a spawnable into and then remove it from.
    FString FullPath;
    ULevelSequence* Sequence = CreateNameLabelMismatchSequence(*this, FullPath);
    if (!Sequence)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(FullPath); };

    UMovieScene* MovieScene = Sequence->GetMovieScene();
    if (!TestNotNull(TEXT("MovieScene present on the probe sequence"), MovieScene))
    {
        return false;
    }

    // Add a SPAWNABLE binding directly. A spawnable has no live world actor, so remove_actors
    // must match it by its stored binding name and remove it via RemoveSpawnable.
    UObject* Template = AStaticMeshActor::StaticClass()->GetDefaultObject();
    if (!TestNotNull(TEXT("spawnable template CDO available"), Template))
    {
        return false;
    }
    const FGuid SpawnableGuid = MovieScene->AddSpawnable(TEXT("MCP_RemoveSpawnableProbe"), *Template);
    FMovieSceneSpawnable* Spawnable = MovieScene->FindSpawnable(SpawnableGuid);
    if (!TestNotNull(TEXT("spawnable binding created in the probe sequence"), Spawnable))
    {
        return false;
    }
    // Remove by the spawnable's exact stored name — the identity get_bindings reports.
    const FString SpawnableName = Spawnable->GetName();

    // remove_actors guards Ctx.GetSubsystem() before dereferencing it, so the null-subsystem
    // InvokeHandlerWithCapture is safe here (unlike add_actors, which requires a live subsystem).
    FTestResponseCapture RemoveCapture;
    if (!TestTrue(TEXT("sequencer.remove_actors handler is registered and invoked"),
            InvokeHandlerWithCapture(TEXT("sequencer.remove_actors"),
                MakeActorNamesPayload(FullPath, SpawnableName), RemoveCapture)))
    {
        return false;
    }
    if (!TestTrue(TEXT("sequencer.remove_actors reported an overall success envelope"),
            RemoveCapture.bSuccess))
    {
        return false;
    }

    // CORRECT-BEHAVIOR ASSERTIONS: the spawnable must actually be gone from the MovieScene
    // (pre-fix RemovePossessable is a no-op for a spawnable GUID, so it would still be found).
    TestNull(TEXT("spawnable binding removed from the MovieScene (RemoveSpawnable ran)"),
        MovieScene->FindSpawnable(SpawnableGuid));

    double BindingsProcessed = -1.0;
    if (RemoveCapture.Result.IsValid())
    {
        RemoveCapture.Result->TryGetNumberField(TEXT("bindingsProcessed"), BindingsProcessed);
    }
    bool bRemovedItemSuccess = false;
    const TArray<TSharedPtr<FJsonValue>>* RemovedArr = nullptr;
    if (RemoveCapture.Result.IsValid() &&
        RemoveCapture.Result->TryGetArrayField(TEXT("removedActors"), RemovedArr) &&
        RemovedArr && RemovedArr->Num() > 0)
    {
        const TSharedPtr<FJsonObject>* RemItem = nullptr;
        if ((*RemovedArr)[0].IsValid() && (*RemovedArr)[0]->TryGetObject(RemItem) &&
            RemItem && (*RemItem).IsValid())
        {
            (*RemItem)->TryGetBoolField(TEXT("success"), bRemovedItemSuccess);
        }
    }
    TestTrue(TEXT("remove_actors reported the spawnable removal as a real success "
                  "(not a silent no-op)"), bRemovedItemSuccess);
    TestEqual(TEXT("remove_actors processed exactly one binding for the spawnable "
                   "(bindingsProcessed == 1)"),
        (int32)BindingsProcessed, 1);

    return true;
}
