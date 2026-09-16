// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for actor-identity handling: display label vs internal object name.
//
// The defect these pin: an actor's display label (GetActorLabel, what the World Outliner
// shows) is NOT unique, but McpActorUtils::FindActorByName used to break out of its scan on
// the first actor matching label OR name OR path. Two actors sharing a label therefore
// resolved to whichever the level iterated first, and every mutating verb built on that
// resolver would silently act on an actor the caller had not named while reporting success.
//
// Every spawning test uses FScopedEditorWorldActorGuard so actors it places into the open
// map are destroyed and the level's dirty flag restored on scope exit.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Utils/ActorUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

namespace
{
    // Uniquely named to avoid an ODR clash with sibling test files' helpers under a
    // Unity merge. The trailing 'X' is load-bearing: FActorLabelUtilities::SplitActorLabel
    // strips a trailing NUMBER off a label before uniquifying it, and a bare hex GUID often
    // ends in digits — so "PWUniq_...C143" would uniquify to "PWUniq_...C144", which does
    // not start with the requested label. Ending on a letter keeps the suffix appended
    // rather than the tail incremented.
    FString MakeLabelResolutionLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%sX"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UWorld* LabelResolutionWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // Handler-path tests must NOT use TestWorldUtils::SpawnTransientCubeActor. The verbs
    // resolve through UEditorActorSubsystem::GetAllLevelActors when no explicit world is
    // passed, and that skips RF_Transient actors outright (UE 5.8
    // EditorActorSubsystem.cpp:386 `!Actor->HasAnyFlags(RF_Transient)`), so a transient
    // probe is invisible to every actor.* verb and the test would measure the wrong thing.
    // A normal level actor is visible; the enclosing FScopedEditorWorldActorGuard destroys
    // it and restores the level's dirty flag on scope exit.
    AStaticMeshActor* SpawnLabelProbeActor(UWorld* World, const FString& Label, const FVector& Location)
    {
        if (!World)
        {
            return nullptr;
        }
        UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
        if (!CubeMesh)
        {
            return nullptr;
        }
        AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
        Actor->SetActorLabel(Label);
        return Actor;
    }
}

// ============================================================================
// Engine contract: SetActorLabel does NOT uniquify
// ============================================================================

// The premise of every ambiguity test below, and a belief this repo previously had
// backwards (Tests/TestUtils.h once asserted SetActorLabel "may append _N"). It does not:
// AActor::SetActorLabel (Engine/Private/ActorEditor.cpp:1291) validates the string, compares
// it to the current label and assigns. Only FActorLabelUtilities::SetActorLabelUnique
// (EditorEngine.cpp:6579) appends a suffix. So two actors CAN carry one label, which is what
// makes label-keyed resolution ambiguous in the first place.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetActorLabelDoesNotUniquifyTest,
    "PinWright.actor.labels.SetActorLabelDoesNotUniquify",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetActorLabelDoesNotUniquifyTest::RunTest(const FString& Parameters)
{
    UWorld* World = LabelResolutionWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping SetActorLabelDoesNotUniquify."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString SharedLabel = MakeLabelResolutionLabel(TEXT("PWDup"));

    AActor* First = SpawnLabelProbeActor(World, SharedLabel, FVector(0, 0, 0));
    AActor* Second = SpawnLabelProbeActor(World, SharedLabel, FVector(200, 0, 0));
    if (!First || !Second)
    {
        AddError(TEXT("Failed to spawn the two probe actors."));
        return false;
    }

    TestEqual(TEXT("first actor keeps the requested label"), First->GetActorLabel(), SharedLabel);
    TestEqual(TEXT("second actor gets the SAME label - SetActorLabel does not uniquify"),
        Second->GetActorLabel(), SharedLabel);
    TestNotEqual(TEXT("their internal object names still differ"),
        First->GetName(), Second->GetName());
    return true;
}

// SetActorLabelUnique is the entry point that DOES disambiguate, and it is what
// actor.set_label's unique=true routes to. Pinning both halves keeps the distinction from
// drifting back into folklore.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetActorLabelUniqueDoesUniquifyTest,
    "PinWright.actor.labels.SetActorLabelUniqueDoesUniquify",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetActorLabelUniqueDoesUniquifyTest::RunTest(const FString& Parameters)
{
    UWorld* World = LabelResolutionWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping SetActorLabelUniqueDoesUniquify."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString SharedLabel = MakeLabelResolutionLabel(TEXT("PWUniq"));

    AActor* First = SpawnLabelProbeActor(World, SharedLabel, FVector(0, 0, 0));
    AActor* Second = SpawnLabelProbeActor(World, TEXT("PWUniqOther"), FVector(200, 0, 0));
    if (!First || !Second)
    {
        AddError(TEXT("Failed to spawn the two probe actors."));
        return false;
    }

    FActorLabelUtilities::SetActorLabelUnique(Second, SharedLabel);
    TestNotEqual(TEXT("SetActorLabelUnique refuses to duplicate the existing label"),
        Second->GetActorLabel(), SharedLabel);
    TestTrue(TEXT("the uniquified label still starts with what was requested"),
        Second->GetActorLabel().StartsWith(SharedLabel));
    return true;
}

// ============================================================================
// ResolveActor precedence and ambiguity
// ============================================================================

// The core fix: an exact label matching two actors is Ambiguous, and BOTH are reported as
// candidates so the caller can pick by internal object name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorResolveAmbiguousLabelTest,
    "PinWright.actor.labels.ResolveAmbiguousLabelReportsCandidates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorResolveAmbiguousLabelTest::RunTest(const FString& Parameters)
{
    UWorld* World = LabelResolutionWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping ResolveAmbiguousLabelReportsCandidates."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString SharedLabel = MakeLabelResolutionLabel(TEXT("PWAmb"));

    AActor* First = SpawnLabelProbeActor(World, SharedLabel, FVector(0, 0, 0));
    AActor* Second = SpawnLabelProbeActor(World, SharedLabel, FVector(200, 0, 0));
    if (!First || !Second)
    {
        AddError(TEXT("Failed to spawn the two probe actors."));
        return false;
    }

    const McpActorUtils::FActorResolution Resolution = McpActorUtils::ResolveActor(World, SharedLabel);
    TestTrue(TEXT("a label matching two actors is Ambiguous"), Resolution.IsAmbiguous());
    TestFalse(TEXT("an ambiguous resolution is not Resolved"), Resolution.IsResolved());
    TestNull(TEXT("an ambiguous resolution carries no chosen actor"), Resolution.Actor);
    TestEqual(TEXT("both matching actors are reported as candidates"),
        Resolution.Candidates.Num(), 2);
    TestTrue(TEXT("candidates contain the first actor"), Resolution.Candidates.Contains(First));
    TestTrue(TEXT("candidates contain the second actor"), Resolution.Candidates.Contains(Second));
    TestTrue(TEXT("the ambiguity is attributed to the label tier"),
        Resolution.MatchedBy == McpActorUtils::EActorMatchKind::Label);
    return true;
}

// The failure direction of the same fix: FindActorByName, which ~100 call sites use and
// which cannot report a structured error, must return nullptr rather than a guess.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByNameRefusesAmbiguityTest,
    "PinWright.actor.labels.FindActorByNameRefusesAmbiguousLabel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByNameRefusesAmbiguityTest::RunTest(const FString& Parameters)
{
    UWorld* World = LabelResolutionWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping FindActorByNameRefusesAmbiguousLabel."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString SharedLabel = MakeLabelResolutionLabel(TEXT("PWRefuse"));

    AActor* First = SpawnLabelProbeActor(World, SharedLabel, FVector(0, 0, 0));
    AActor* Second = SpawnLabelProbeActor(World, SharedLabel, FVector(200, 0, 0));
    if (!First || !Second)
    {
        AddError(TEXT("Failed to spawn the two probe actors."));
        return false;
    }

    TestNull(TEXT("an ambiguous label resolves to nothing rather than to an arbitrary match"),
        McpActorUtils::FindActorByName(World, SharedLabel));

    // The unique internal object name still resolves each actor exactly, so the refusal
    // above is a refusal to guess, not a loss of addressability.
    TestTrue(TEXT("the first actor is still reachable by its internal object name"),
        McpActorUtils::FindActorByName(World, First->GetName()) == First);
    TestTrue(TEXT("the second actor is still reachable by its internal object name"),
        McpActorUtils::FindActorByName(World, Second->GetName()) == Second);
    return true;
}

// Precedence: the internal object name is unique, so it must win over an actor whose LABEL
// happens to spell the same string. Under the old first-match-wins scan the winner depended
// on level iteration order.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorObjectNameOutranksCollidingLabelTest,
    "PinWright.actor.labels.ObjectNameOutranksCollidingLabel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorObjectNameOutranksCollidingLabelTest::RunTest(const FString& Parameters)
{
    UWorld* World = LabelResolutionWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping ObjectNameOutranksCollidingLabel."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;

    AActor* Target = SpawnLabelProbeActor(World, TEXT("PWPrecedenceTarget"), FVector(0, 0, 0));
    AActor* Decoy = SpawnLabelProbeActor(World, TEXT("PWPrecedenceDecoy"), FVector(200, 0, 0));
    if (!Target || !Decoy)
    {
        AddError(TEXT("Failed to spawn the two probe actors."));
        return false;
    }

    // Give the decoy a LABEL equal to the target's internal object NAME.
    const FString TargetObjectName = Target->GetName();
    Decoy->SetActorLabel(TargetObjectName);
    TestEqual(TEXT("the decoy now carries the target's object name as its label"),
        Decoy->GetActorLabel(), TargetObjectName);

    const McpActorUtils::FActorResolution Resolution = McpActorUtils::ResolveActor(World, TargetObjectName);
    TestTrue(TEXT("the identifier resolves rather than reporting ambiguity"), Resolution.IsResolved());
    TestTrue(TEXT("the object-name tier wins over the colliding label"), Resolution.Actor == Target);
    TestTrue(TEXT("the match is attributed to the object-name tier"),
        Resolution.MatchedBy == McpActorUtils::EActorMatchKind::ObjectName);
    return true;
}

// The object path is unique by construction and is the most specific tier.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorObjectPathResolvesTest,
    "PinWright.actor.labels.ObjectPathResolvesExactly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorObjectPathResolvesTest::RunTest(const FString& Parameters)
{
    UWorld* World = LabelResolutionWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping ObjectPathResolvesExactly."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString SharedLabel = MakeLabelResolutionLabel(TEXT("PWPath"));

    AActor* First = SpawnLabelProbeActor(World, SharedLabel, FVector(0, 0, 0));
    AActor* Second = SpawnLabelProbeActor(World, SharedLabel, FVector(200, 0, 0));
    if (!First || !Second)
    {
        AddError(TEXT("Failed to spawn the two probe actors."));
        return false;
    }

    // Even though the label is ambiguous, each actor's path picks it out exactly.
    const McpActorUtils::FActorResolution Resolution = McpActorUtils::ResolveActor(World, Second->GetPathName());
    TestTrue(TEXT("an object path resolves"), Resolution.IsResolved());
    TestTrue(TEXT("the object path selects the actor it names"), Resolution.Actor == Second);
    TestTrue(TEXT("the match is attributed to the object-path tier"),
        Resolution.MatchedBy == McpActorUtils::EActorMatchKind::ObjectPath);
    return true;
}

// An identifier matching nothing is NotFound, not Ambiguous - the two have different
// recoveries and must not score alike.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorResolveMissTest,
    "PinWright.actor.labels.UnmatchedIdentifierIsNotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorResolveMissTest::RunTest(const FString& Parameters)
{
    UWorld* World = LabelResolutionWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping UnmatchedIdentifierIsNotFound."));
        return true;
    }

    const McpActorUtils::FActorResolution Resolution =
        McpActorUtils::ResolveActor(World, MakeLabelResolutionLabel(TEXT("PWNoSuchActor")));
    TestFalse(TEXT("a name matching nothing is not Resolved"), Resolution.IsResolved());
    TestFalse(TEXT("a name matching nothing is not Ambiguous"), Resolution.IsAmbiguous());
    TestTrue(TEXT("a default-constructed resolution reports NotFound"),
        Resolution.Status == McpActorUtils::EActorResolveStatus::NotFound);
    TestEqual(TEXT("no candidates are reported for a miss"), Resolution.Candidates.Num(), 0);
    return true;
}

// ============================================================================
// actor.set_label
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetLabelRegisteredTest,
    "PinWright.actor.set_label.Registered",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetLabelRegisteredTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("actor.set_label is registered"), IsRegistered(TEXT("actor.set_label")));
    return true;
}

// The rename itself, verified against the actor rather than against the response: the test
// reads GetActorLabel() off the world, which is the value the World Outliner renders.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetLabelRenamesTest,
    "PinWright.actor.set_label.RenamesAndReadsBack",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetLabelRenamesTest::RunTest(const FString& Parameters)
{
    UWorld* World = LabelResolutionWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.set_label.RenamesAndReadsBack."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString StartLabel = MakeLabelResolutionLabel(TEXT("PWBefore"));
    const FString EndLabel = MakeLabelResolutionLabel(TEXT("PWAfter"));

    AActor* Actor = SpawnLabelProbeActor(World, StartLabel, FVector(0, 0, 0));
    if (!Actor)
    {
        AddError(TEXT("Failed to spawn the probe actor."));
        return false;
    }
    const FString ObjectNameBefore = Actor->GetName();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), StartLabel);
    Payload->SetStringField(TEXT("label"), EndLabel);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"),
        InvokeHandlerWithCapture(TEXT("actor.set_label"), Payload, Capture));
    TestTrue(TEXT("rename succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("actor.set_label failed: %s"), *Capture.Message));
        return false;
    }

    // Independent of the response: ask the actor what the outliner will show.
    TestEqual(TEXT("the actor itself now reports the new label"), Actor->GetActorLabel(), EndLabel);
    TestEqual(TEXT("the internal object name is untouched by a label rename"),
        Actor->GetName(), ObjectNameBefore);

    TestEqual(TEXT("response echoes the previous label"),
        Capture.Result->GetStringField(TEXT("previousLabel")), StartLabel);
    TestEqual(TEXT("response reports the applied label"),
        Capture.Result->GetStringField(TEXT("label")), EndLabel);
    TestEqual(TEXT("response reports the unchanged internal object name"),
        Capture.Result->GetStringField(TEXT("actorObjectName")), ObjectNameBefore);
    TestTrue(TEXT("response reports applied=true"),
        Capture.Result->GetBoolField(TEXT("applied")));
    TestTrue(TEXT("response reports changed=true"),
        Capture.Result->GetBoolField(TEXT("changed")));

    // And the renamed actor is reachable under its new label.
    TestTrue(TEXT("the new label resolves to the same actor"),
        McpActorUtils::FindActorByName(World, EndLabel) == Actor);
    return true;
}

// unique=true routes to SetActorLabelUnique, and the verb reports the substitution rather
// than echoing the label it was asked for.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetLabelUniqueTest,
    "PinWright.actor.set_label.UniqueAvoidsCollision",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetLabelUniqueTest::RunTest(const FString& Parameters)
{
    UWorld* World = LabelResolutionWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.set_label.UniqueAvoidsCollision."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString TakenLabel = MakeLabelResolutionLabel(TEXT("PWTaken"));
    const FString MoverLabel = MakeLabelResolutionLabel(TEXT("PWMover"));

    AActor* Occupant = SpawnLabelProbeActor(World, TakenLabel, FVector(0, 0, 0));
    AActor* Mover = SpawnLabelProbeActor(World, MoverLabel, FVector(200, 0, 0));
    if (!Occupant || !Mover)
    {
        AddError(TEXT("Failed to spawn the two probe actors."));
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), MoverLabel);
    Payload->SetStringField(TEXT("label"), TakenLabel);
    Payload->SetBoolField(TEXT("unique"), true);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"),
        InvokeHandlerWithCapture(TEXT("actor.set_label"), Payload, Capture));
    TestTrue(TEXT("unique rename succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("actor.set_label unique failed: %s"), *Capture.Message));
        return false;
    }

    TestNotEqual(TEXT("the mover did NOT take the occupied label verbatim"),
        Mover->GetActorLabel(), TakenLabel);
    TestEqual(TEXT("the occupant keeps its label"), Occupant->GetActorLabel(), TakenLabel);
    TestTrue(TEXT("response reports the label was uniquified"),
        Capture.Result->GetBoolField(TEXT("labelWasUniquified")));
    TestFalse(TEXT("response does not claim the requested label was applied verbatim"),
        Capture.Result->GetBoolField(TEXT("applied")));
    TestEqual(TEXT("response reports the label the actor actually carries"),
        Capture.Result->GetStringField(TEXT("label")), Mover->GetActorLabel());
    return true;
}

// The ambiguity policy at the wire level: a label naming two actors is refused with
// AMBIGUOUS_ACTOR_NAME, and the payload lists each candidate's unique internal name so the
// caller can disambiguate without a second query.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetLabelAmbiguousTargetTest,
    "PinWright.actor.set_label.AmbiguousTargetIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetLabelAmbiguousTargetTest::RunTest(const FString& Parameters)
{
    UWorld* World = LabelResolutionWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.set_label.AmbiguousTargetIsRefused."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString SharedLabel = MakeLabelResolutionLabel(TEXT("PWAmbSet"));

    AActor* First = SpawnLabelProbeActor(World, SharedLabel, FVector(0, 0, 0));
    AActor* Second = SpawnLabelProbeActor(World, SharedLabel, FVector(200, 0, 0));
    if (!First || !Second)
    {
        AddError(TEXT("Failed to spawn the two probe actors."));
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), SharedLabel);
    Payload->SetStringField(TEXT("label"), TEXT("PWShouldNeverBeApplied"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"),
        InvokeHandlerWithCapture(TEXT("actor.set_label"), Payload, Capture));
    TestFalse(TEXT("an ambiguous target is refused, not silently applied to one actor"),
        Capture.bSuccess);
    TestEqual(TEXT("the refusal uses AMBIGUOUS_ACTOR_NAME"),
        Capture.ErrorCode, FString(TEXT("AMBIGUOUS_ACTOR_NAME")));

    // The refusal must be total: neither actor may have been touched.
    TestEqual(TEXT("the first actor keeps its label"), First->GetActorLabel(), SharedLabel);
    TestEqual(TEXT("the second actor keeps its label"), Second->GetActorLabel(), SharedLabel);

    if (!Capture.Result.IsValid())
    {
        AddError(TEXT("AMBIGUOUS_ACTOR_NAME carried no structured payload."));
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
    if (!Capture.Result->TryGetArrayField(TEXT("candidates"), Candidates) || !Candidates)
    {
        AddError(TEXT("AMBIGUOUS_ACTOR_NAME payload has no candidates array."));
        return false;
    }
    TestEqual(TEXT("both colliding actors are offered as candidates"), Candidates->Num(), 2);

    // Each candidate must carry the unique internal object name - without it the caller
    // cannot act on the error.
    TArray<FString> CandidateObjectNames;
    for (const TSharedPtr<FJsonValue>& Entry : *Candidates)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (Entry.IsValid() && Entry->TryGetObject(Obj) && Obj)
        {
            FString ObjectName;
            if ((*Obj)->TryGetStringField(TEXT("name"), ObjectName))
            {
                CandidateObjectNames.Add(ObjectName);
            }
        }
    }
    TestTrue(TEXT("candidates name the first actor's internal object name"),
        CandidateObjectNames.Contains(First->GetName()));
    TestTrue(TEXT("candidates name the second actor's internal object name"),
        CandidateObjectNames.Contains(Second->GetName()));
    return true;
}

// ============================================================================
// actor.set_folder - the batch-verb form of the same policy
// ============================================================================

// The original live repro: set_folder on an ambiguous label moved exactly one of two actors
// and returned updatedCount=1 as a success. It must now move neither and say why.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorSetFolderAmbiguousTest,
    "PinWright.actor.set_folder.AmbiguousNameMovesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorSetFolderAmbiguousTest::RunTest(const FString& Parameters)
{
    UWorld* World = LabelResolutionWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.set_folder.AmbiguousNameMovesNothing."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString SharedLabel = MakeLabelResolutionLabel(TEXT("PWAmbFolder"));

    AActor* First = SpawnLabelProbeActor(World, SharedLabel, FVector(0, 0, 0));
    AActor* Second = SpawnLabelProbeActor(World, SharedLabel, FVector(200, 0, 0));
    if (!First || !Second)
    {
        AddError(TEXT("Failed to spawn the two probe actors."));
        return false;
    }
    const FName FolderBefore = First->GetFolderPath();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), SharedLabel);
    Payload->SetStringField(TEXT("folderPath"), TEXT("PWProbe/ShouldNotHappen"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"),
        InvokeHandlerWithCapture(TEXT("actor.set_folder"), Payload, Capture));
    TestFalse(TEXT("an ambiguous-only batch is an error, not a partial success"), Capture.bSuccess);
    TestEqual(TEXT("the refusal uses AMBIGUOUS_ACTOR_NAME"),
        Capture.ErrorCode, FString(TEXT("AMBIGUOUS_ACTOR_NAME")));

    // Neither actor moved: this is the defect the fix exists for.
    TestTrue(TEXT("the first actor's folder is unchanged"), First->GetFolderPath() == FolderBefore);
    TestTrue(TEXT("the second actor's folder is unchanged"), Second->GetFolderPath() == FolderBefore);

    if (Capture.Result.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Ambiguous = nullptr;
        TestTrue(TEXT("the response buckets the name under ambiguous[], not missing[]"),
            Capture.Result->TryGetArrayField(TEXT("ambiguous"), Ambiguous) && Ambiguous && Ambiguous->Num() == 1);
    }
    return true;
}

// ============================================================================
// actor.delete - the destructive case
// ============================================================================

// The worst version of the original defect: under first-match-wins, deleting by a colliding
// label destroyed whichever actor the level happened to iterate first, irreversibly, and
// reported success. Nothing may be deleted, and the refusal must say why.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorDeleteAmbiguousTest,
    "PinWright.actor.delete.AmbiguousNameDeletesNothing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorDeleteAmbiguousTest::RunTest(const FString& Parameters)
{
    UWorld* World = LabelResolutionWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.delete.AmbiguousNameDeletesNothing."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString SharedLabel = MakeLabelResolutionLabel(TEXT("PWAmbDelete"));

    AActor* First = SpawnLabelProbeActor(World, SharedLabel, FVector(0, 0, 0));
    AActor* Second = SpawnLabelProbeActor(World, SharedLabel, FVector(200, 0, 0));
    if (!First || !Second)
    {
        AddError(TEXT("Failed to spawn the two probe actors."));
        return false;
    }
    const FString FirstName = First->GetName();
    const FString SecondName = Second->GetName();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), SharedLabel);

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler found"),
        InvokeHandlerWithCapture(TEXT("actor.delete"), Payload, Capture));
    TestFalse(TEXT("deleting by an ambiguous label is refused"), Capture.bSuccess);
    TestEqual(TEXT("the refusal uses AMBIGUOUS_ACTOR_NAME"),
        Capture.ErrorCode, FString(TEXT("AMBIGUOUS_ACTOR_NAME")));

    // The load-bearing assertion: both actors are still alive. Re-probe the world rather
    // than trusting the response, since the response is what the old code got wrong.
    TestTrue(TEXT("the first actor still exists"),
        McpActorUtils::FindActorByName(World, FirstName) != nullptr);
    TestTrue(TEXT("the second actor still exists"),
        McpActorUtils::FindActorByName(World, SecondName) != nullptr);

    // And deleting by the unique internal name still works, so the refusal is a refusal to
    // guess rather than a loss of the ability to delete.
    TSharedPtr<FJsonObject> ByName = MakeShared<FJsonObject>();
    ByName->SetStringField(TEXT("actorName"), FirstName);
    FTestResponseCapture DeleteCapture;
    TestTrue(TEXT("handler found (by internal name)"),
        InvokeHandlerWithCapture(TEXT("actor.delete"), ByName, DeleteCapture));
    TestTrue(TEXT("deleting by the unique internal object name succeeds"), DeleteCapture.bSuccess);
    TestTrue(TEXT("the named actor is gone"),
        McpActorUtils::FindActorByName(World, FirstName) == nullptr);
    TestTrue(TEXT("the other actor survived"),
        McpActorUtils::FindActorByName(World, SecondName) != nullptr);
    return true;
}
