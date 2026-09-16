// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the actor.* search-verb alias sets declared in
// Handlers/Actor/ActorQueryParamUtils.h (SearchFragmentKeys / FindClassNameKeys).
//
// Two defects, both of the "schema and parser disagree" family that commit 644db343 named:
//
//  - actor.find_by_name declared a bare required `name` and read only `name`, while its own
//    summary, param help and wiki page all call the VALUE a substring. Every key a caller
//    reaches for instead — `pattern`, `filter`, `query`, `substring`, `search` — hard-failed
//    MISSING_REQUIRED_PARAM 'name'. `query` is the sharpest instance: the verb's RESPONSE
//    echoes the fragment back under exactly that key, so one verb disagreed with itself.
//  - actor.find_by_class already read Ctx.GetStringFirstOf({className, class}) and its param
//    help advertised the `class` alias, but the FParamSpec carried no Aliases — so the
//    dispatcher's UNKNOWN_PARAMS gate refused a spelling documented as accepted.
//
// Because both gates live in the dispatcher, the acceptance half MUST route through a real
// FRpcDispatcher: Tests/TestUtils.h's InvokeHandler calls the registered function directly
// and never runs ValidateHandlerParams, so it cannot observe either bug at all.
//
// Two layers, mirroring TestActorSpawnParamAliases.cpp:
//  - Declaration: each slot's production FParamSpec carries the canonical Name and the EXACT
//    expected alias set, so a future edit that drops a spelling (the original defect) or
//    silently widens a slot fails loudly here.
//  - Acceptance: dispatch each spelling through the real dispatcher against a live, uniquely
//    labelled probe actor and check GROUND TRUTH off the response rows — the probe's label
//    must appear, and the alias call must return the SAME count as the canonical call. That
//    proves the alias reached the handler's matching loop, not merely that the payload passed
//    the gate.
// Counterfactual: reverting the Aliases wiring makes every acceptance dispatch fail with
// MISSING_REQUIRED_PARAM/UNKNOWN_PARAMS, and the declaration checks fail on an empty array.
//
// The probe is a NORMAL level actor, never RF_Transient: actor.find_by_name resolves through
// UEditorActorSubsystem::GetAllLevelActors, which filters transient actors out entirely
// (EditorActorSubsystem.cpp:386), so a transient probe would be invisible to the verb under
// test and the test would measure nothing. FScopedEditorWorldActorGuard destroys it and
// restores the level's dirty flag on scope exit. Same rule as SpawnLabelProbeActor in
// Tests/Actor/TestActorLabelResolution.cpp.
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"

#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

// Uniquely named namespace: Unity merges test TUs into one translation unit, so an
// anonymous-namespace helper here would ODR-clash with the identically-shaped helpers in
// TestActorSpawnParamAliases.cpp / TestActorLabelResolution.cpp.
namespace ActorQueryParamAliasTestLocal
{
    // GUID-suffixed so the fragment matches exactly one actor in a populated level, and so
    // the count comparison between the canonical call and the alias call is not perturbed by
    // pre-existing actors. Trailing 'X' for the same reason TestActorLabelResolution gives:
    // FActorLabelUtilities::SplitActorLabel strips a trailing NUMBER before uniquifying, and a
    // bare hex GUID often ends in digits. No '/', '\' or '..' — the verb rejects those as a
    // path-traversal guard.
    FString QueryProbeLabel()
    {
        return FString::Printf(TEXT("PWFindAlias_%sX"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    UWorld* QueryTestWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // A normal (non-transient) level actor — see the file header for why that is load-bearing.
    AStaticMeshActor* SpawnQueryProbeActor(UWorld* World, const FString& Label)
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
            AStaticMeshActor::StaticClass(), FVector(0, 0, 0), FRotator::ZeroRotator);
        if (!Actor)
        {
            return nullptr;
        }
        Actor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
        Actor->SetActorLabel(Label);
        return Actor;
    }

    // One expected slot declaration: the verb, the canonical wire name that must remain
    // FParamSpec.Name, and the exact alias set MakeAliasParamSpec should have produced.
    struct FQueryAliasSlot
    {
        const TCHAR* Method;
        const TCHAR* Canonical;
        // The declared type token. Path-shaped slots carry `path`/`classref`, which is what
        // makes the dispatcher refuse a doubled slash before the handler loads anything.
        const TCHAR* ExpectedType;
        TArray<FString> Aliases;
    };

    // The full declared contract of ActorQueryParamUtils across its two wired verbs.
    const TArray<FQueryAliasSlot>& ExpectedQueryAliasSlots()
    {
        static const TArray<FQueryAliasSlot> Slots = {
            { TEXT("actor.find_by_name"), TEXT("name"), TEXT("string"),
              { TEXT("pattern"), TEXT("filter"), TEXT("query"), TEXT("substring"), TEXT("search") } },
            { TEXT("actor.find_by_class"), TEXT("className"), TEXT("classref"),
              { TEXT("class") } }
        };
        return Slots;
    }

    // One dispatched actor.find_by_name call, reduced to what the assertions need.
    struct FFindByNameOutcome
    {
        bool bSuccess = false;
        FString ErrorCode;
        int32 Count = -1;
        FString EchoedQuery;
        bool bContainsProbe = false;
    };

    // Dispatch actor.find_by_name with the fragment under FragmentKey through the REAL
    // dispatcher (both gates live there, not in the handler body) and read the response rows.
    FFindByNameOutcome RunFindByName(const TCHAR* FragmentKey, const FString& Fragment,
        const FString& ProbeLabel)
    {
        FFindByNameOutcome Outcome;

        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(FragmentKey, Fragment);

        TSharedPtr<FJsonObject> Result;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("actor.find_by_name"),
            FString::Printf(TEXT("req-find-by-name-%s"), FragmentKey),
            Params, Outcome.bSuccess, Result, Outcome.ErrorCode);

        if (!Result.IsValid())
        {
            return Outcome;
        }

        double CountValue = 0.0;
        if (Result->TryGetNumberField(TEXT("count"), CountValue))
        {
            Outcome.Count = static_cast<int32>(CountValue);
        }
        Result->TryGetStringField(TEXT("query"), Outcome.EchoedQuery);

        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        if (Result->TryGetArrayField(TEXT("actors"), Rows) && Rows)
        {
            for (const TSharedPtr<FJsonValue>& Row : *Rows)
            {
                const TSharedPtr<FJsonObject>* RowObj = nullptr;
                if (!Row.IsValid() || !Row->TryGetObject(RowObj) || !RowObj)
                {
                    continue;
                }
                FString RowLabel;
                if ((*RowObj)->TryGetStringField(TEXT("label"), RowLabel)
                    && RowLabel.Equals(ProbeLabel))
                {
                    Outcome.bContainsProbe = true;
                }
            }
        }
        return Outcome;
    }

    // Assert one fragment spelling behaves identically to the canonical `name` spelling.
    void CheckFragmentAliasMatchesCanonical(FAutomationTestBase& Test, const TCHAR* FragmentKey,
        const FString& ProbeLabel, const FFindByNameOutcome& Canonical)
    {
        const FFindByNameOutcome Aliased = RunFindByName(FragmentKey, ProbeLabel, ProbeLabel);

        Test.TestNotEqual(*FString::Printf(
                TEXT("actor.find_by_name does not reject '%s' as UNKNOWN_PARAMS"), FragmentKey),
            Aliased.ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
        Test.TestNotEqual(*FString::Printf(
                TEXT("actor.find_by_name does not reject '%s' as MISSING_REQUIRED_PARAM"), FragmentKey),
            Aliased.ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
        Test.TestTrue(*FString::Printf(TEXT("actor.find_by_name succeeds with '%s'"), FragmentKey),
            Aliased.bSuccess);

        // GROUND TRUTH: the fragment reached the matching loop, not just the param gate.
        Test.TestTrue(*FString::Printf(
            TEXT("'%s' found the probe actor by label"), FragmentKey), Aliased.bContainsProbe);
        Test.TestEqual(*FString::Printf(
            TEXT("'%s' returns the same match count as the canonical 'name'"), FragmentKey),
            Aliased.Count, Canonical.Count);
        Test.TestEqual(*FString::Printf(
            TEXT("'%s' echoes the fragment back under 'query'"), FragmentKey),
            Aliased.EchoedQuery, ProbeLabel);
    }
}

// ============================================================================
// 1. Declaration: both wired slots register the canonical name plus EXACTLY the alias set
//    ActorQueryParamUtils promises. Without these the dispatcher's known-param set omits the
//    spelling and the payload is rejected before the handler body reads it; a silently
//    widened set changes which payloads the dispatcher admits without anyone noticing.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorQueryVerbsDeclareQueryParamAliasesTest,
    "PinWright.actor.aliases.QueryVerbsDeclareQueryParamAliases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorQueryVerbsDeclareQueryParamAliasesTest::RunTest(const FString& Parameters)
{
    using ActorQueryParamAliasTestLocal::FQueryAliasSlot;

    for (const FQueryAliasSlot& Slot : ActorQueryParamAliasTestLocal::ExpectedQueryAliasSlots())
    {
        const FParamSpec* Spec = ParamSpecTestHelpers::FindParamSpec(Slot.Method, Slot.Canonical);
        if (!TestNotNull(*FString::Printf(TEXT("%s declares the '%s' slot"),
                Slot.Method, Slot.Canonical), Spec))
        {
            continue;
        }

        TestEqual(*FString::Printf(TEXT("%s '%s' keeps its canonical name"),
            Slot.Method, Slot.Canonical), Spec->Name, FString(Slot.Canonical));
        TestEqual(*FString::Printf(TEXT("%s '%s' declares type '%s'"),
            Slot.Method, Slot.Canonical, Slot.ExpectedType), Spec->Type, FString(Slot.ExpectedType));
        TestTrue(*FString::Printf(TEXT("%s '%s' stays required"),
            Slot.Method, Slot.Canonical), Spec->bRequired);

        for (const FString& Alias : Slot.Aliases)
        {
            TestTrue(*FString::Printf(TEXT("%s '%s' accepts the '%s' alias"),
                Slot.Method, Slot.Canonical, *Alias), Spec->Aliases.Contains(Alias));
        }

        TestEqual(*FString::Printf(TEXT("%s '%s' declares exactly %d aliases"),
            Slot.Method, Slot.Canonical, Slot.Aliases.Num()),
            Spec->Aliases.Num(), Slot.Aliases.Num());

        // Named individually so a widened set reports WHICH spelling appeared.
        for (const FString& Declared : Spec->Aliases)
        {
            TestTrue(*FString::Printf(TEXT("%s '%s' alias '%s' is part of the declared contract"),
                Slot.Method, Slot.Canonical, *Declared), Slot.Aliases.Contains(Declared));
        }

        // The canonical name must never be duplicated into its own alias list: the dispatcher
        // unions Name + Aliases, and a duplicate would mask a canonical rename.
        TestFalse(*FString::Printf(TEXT("%s '%s' does not repeat itself in Aliases"),
            Slot.Method, Slot.Canonical), Spec->Aliases.Contains(FString(Slot.Canonical)));
    }

    // The identity keys stay OUT of the search-fragment slot. `actorName`/`actorPath` mean
    // "exactly one actor, ambiguity refused" on the actor.* readers; find_by_name's contract
    // is a fragment matching many. See the ActorQueryParamUtils.h header note.
    if (const FParamSpec* FragmentSpec =
            ParamSpecTestHelpers::FindParamSpec(TEXT("actor.find_by_name"), TEXT("name")))
    {
        TestFalse(TEXT("actor.find_by_name 'name' does not claim the identity key actorName"),
            FragmentSpec->Aliases.Contains(FString(TEXT("actorName"))));
        TestFalse(TEXT("actor.find_by_name 'name' does not claim the identity key actorPath"),
            FragmentSpec->Aliases.Contains(FString(TEXT("actorPath"))));
    }
    return true;
}

// ============================================================================
// 2. Acceptance (actor.find_by_name): `pattern` — the reported defect, and the natural
//    spelling for a substring query — reaches the handler and returns the same result set as
//    the canonical `name`. Ground truth is the probe actor's label appearing in the rows.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByNameAcceptsPatternOnWireTest,
    "PinWright.actor.aliases.FindByNameAcceptsPatternOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByNameAcceptsPatternOnWireTest::RunTest(const FString& Parameters)
{
    UWorld* World = ActorQueryParamAliasTestLocal::QueryTestWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.find_by_name pattern alias test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString ProbeLabel = ActorQueryParamAliasTestLocal::QueryProbeLabel();
    if (!ActorQueryParamAliasTestLocal::SpawnQueryProbeActor(World, ProbeLabel))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("Could not spawn the probe actor; skipping pattern alias test."));
        return true;
    }

    // Canonical baseline first: everything the alias call is compared against.
    const ActorQueryParamAliasTestLocal::FFindByNameOutcome Canonical =
        ActorQueryParamAliasTestLocal::RunFindByName(TEXT("name"), ProbeLabel, ProbeLabel);
    TestTrue(TEXT("canonical actor.find_by_name {name} succeeds"), Canonical.bSuccess);
    TestTrue(TEXT("canonical actor.find_by_name {name} finds the probe actor"),
        Canonical.bContainsProbe);

    ActorQueryParamAliasTestLocal::CheckFragmentAliasMatchesCanonical(*this, TEXT("pattern"),
        ProbeLabel, Canonical);
    return true;
}

// ============================================================================
// 3. Acceptance (actor.find_by_name): the four remaining fragment spellings. `filter` is what
//    the sibling actor.list calls the identical value; `query` is what THIS verb's response
//    echoes it back as; `substring` and `search` are the wrong guesses docs/wiki-src/actor.md
//    already documented callers making. Every one of them used to hard-fail
//    MISSING_REQUIRED_PARAM 'name'.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByNameAcceptsRemainingFragmentSpellingsTest,
    "PinWright.actor.aliases.FindByNameAcceptsRemainingFragmentSpellings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByNameAcceptsRemainingFragmentSpellingsTest::RunTest(const FString& Parameters)
{
    UWorld* World = ActorQueryParamAliasTestLocal::QueryTestWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.find_by_name alias-set test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    const FString ProbeLabel = ActorQueryParamAliasTestLocal::QueryProbeLabel();
    if (!ActorQueryParamAliasTestLocal::SpawnQueryProbeActor(World, ProbeLabel))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("Could not spawn the probe actor; skipping alias-set test."));
        return true;
    }

    const ActorQueryParamAliasTestLocal::FFindByNameOutcome Canonical =
        ActorQueryParamAliasTestLocal::RunFindByName(TEXT("name"), ProbeLabel, ProbeLabel);
    TestTrue(TEXT("canonical actor.find_by_name {name} succeeds"), Canonical.bSuccess);

    for (const TCHAR* FragmentKey : { TEXT("filter"), TEXT("query"), TEXT("substring"), TEXT("search") })
    {
        ActorQueryParamAliasTestLocal::CheckFragmentAliasMatchesCanonical(*this, FragmentKey,
            ProbeLabel, Canonical);
    }
    return true;
}

// ============================================================================
// 4. Acceptance (actor.find_by_class): the `class` spelling the body has always read and the
//    param help has always advertised. This is the 644db343 defect verbatim — declared as
//    accepted, refused by the gate — so the assertion that matters is that the dispatch is
//    not rejected, and that it returns the same count as the canonical `className`.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByClassAcceptsClassOnWireTest,
    "PinWright.actor.aliases.FindByClassAcceptsClassOnWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByClassAcceptsClassOnWireTest::RunTest(const FString& Parameters)
{
    UWorld* World = ActorQueryParamAliasTestLocal::QueryTestWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping actor.find_by_class class alias test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;

    // A StaticMeshActor probe guarantees the class query has at least one row to return, so a
    // count of 0 on both calls cannot masquerade as agreement.
    const FString ProbeLabel = ActorQueryParamAliasTestLocal::QueryProbeLabel();
    if (!ActorQueryParamAliasTestLocal::SpawnQueryProbeActor(World, ProbeLabel))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            TEXT("Could not spawn the probe actor; skipping class alias test."));
        return true;
    }

    auto RunFindByClass = [](const TCHAR* Key, int32& OutCount, bool& bOutSuccess, FString& OutErrorCode)
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(Key, TEXT("StaticMeshActor"));

        TSharedPtr<FJsonObject> Result;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("actor.find_by_class"),
            FString::Printf(TEXT("req-find-by-class-%s"), Key),
            Params, bOutSuccess, Result, OutErrorCode);

        OutCount = -1;
        double CountValue = 0.0;
        if (Result.IsValid() && Result->TryGetNumberField(TEXT("count"), CountValue))
        {
            OutCount = static_cast<int32>(CountValue);
        }
    };

    int32 CanonicalCount = -1;
    bool bCanonicalSuccess = false;
    FString CanonicalErrorCode;
    RunFindByClass(TEXT("className"), CanonicalCount, bCanonicalSuccess, CanonicalErrorCode);
    TestTrue(TEXT("canonical actor.find_by_class {className} succeeds"), bCanonicalSuccess);
    TestTrue(TEXT("the StaticMeshActor probe makes the canonical count non-zero"),
        CanonicalCount > 0);

    int32 AliasCount = -1;
    bool bAliasSuccess = false;
    FString AliasErrorCode;
    RunFindByClass(TEXT("class"), AliasCount, bAliasSuccess, AliasErrorCode);

    TestNotEqual(TEXT("actor.find_by_class does not reject 'class' as UNKNOWN_PARAMS"),
        AliasErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    TestNotEqual(TEXT("actor.find_by_class does not reject 'class' as MISSING_REQUIRED_PARAM"),
        AliasErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    TestTrue(TEXT("actor.find_by_class succeeds with 'class'"), bAliasSuccess);
    TestEqual(TEXT("'class' returns the same count as the canonical 'className'"),
        AliasCount, CanonicalCount);
    return true;
}
