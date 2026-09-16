// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for FPwCandidateRegistry (Private/AudioGen) and the two verbs over it,
// audio.synth.list_candidates / audio.synth.discard.
//
// Every assertion here is written so it breaks if the registry goes back to the behaviour it
// was built to avoid (rule §12): an evicted id answering "not found", an empty registry
// answering "bad id", a read failing to protect a candidate from eviction, and a
// selector-less discard quietly discarding everything.
//
// LRU order is driven explicitly through Get() rather than by wall-clock sleeps - the
// registry orders on a monotonic touch counter precisely so a test never has to wait.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"

#include "AudioGen/PwAudioAudition.h"
#include "AudioGen/PwCandidateRegistry.h"
#include "Handlers/ErrorCodes.h"
#include "State/PluginState.h"

namespace
{
    // 100 frames of stereo float = 800 bytes, so a budget can be expressed in whole
    // candidates without allocating anything meaningful. Names carry a Pw prefix because
    // Unity merges translation units and anonymous namespaces do not keep them apart.
    constexpr int32 PwTestFrames = 100;
    constexpr int64 PwTestCandidateBytes =
        static_cast<int64>(PwTestFrames) * 2 * static_cast<int64>(sizeof(float));

    // Fills the channel arrays directly rather than through SetNumFrames, so the byte figure
    // the budget tests rely on is exactly PwTestCandidateBytes regardless of how the buffer
    // helper chooses to size things.
    FPwCandidate MakePwTestCandidate(int32 NumFrames = PwTestFrames)
    {
        FPwCandidate Candidate;
        Candidate.Buffer.SampleRate = 48000;
        Candidate.Buffer.Left.SetNumZeroed(NumFrames);
        Candidate.Buffer.Right.SetNumZeroed(NumFrames);
        return Candidate;
    }

    // Adds a candidate to the process-wide registry and returns its id. Used only by the
    // handler tests, which must go through FPluginState the way the verbs do.
    FString AddPwCandidateToPluginState()
    {
        return FPluginState::Get().GetCandidateRegistry().Add(MakePwTestCandidate());
    }
}

// =========================================================================
// A. Add/Get round-trip: the id resolves, the audio survives, and the byte
//    accounting is a measurement of the buffer rather than a guess.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCandidateRegistryRoundTripTest,
    "PinWright.audio.synth.candidates.AddGetRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCandidateRegistryRoundTripTest::RunTest(const FString& Parameters)
{
    FPwCandidateRegistry Registry;

    const FString IdA = Registry.Add(MakePwTestCandidate());
    const FString IdB = Registry.Add(MakePwTestCandidate());

    TestFalse(TEXT("Add returns a non-empty id"), IdA.IsEmpty());
    TestNotEqual(TEXT("Two adds get different ids"), IdA, IdB);
    TestTrue(TEXT("Id is short enough to repeat in a prompt (< 16 chars)"), IdA.Len() < 16);

    const FPwCandidateLookupResult Hit = Registry.Get(IdA);
    TestTrue(TEXT("Get resolves the id it just issued"), Hit.IsHit());
    if (!Hit.IsHit())
    {
        return false;
    }

    TestEqual(TEXT("Candidate carries the id it was issued"), Hit.Candidate->CandidateId, IdA);
    TestEqual(TEXT("Left channel survived the move into the registry"),
        Hit.Candidate->Buffer.Left.Num(), PwTestFrames);
    TestEqual(TEXT("Right channel survived the move into the registry"),
        Hit.Candidate->Buffer.Right.Num(), PwTestFrames);
    TestEqual(TEXT("ApproxBytes is measured off the buffer"),
        Hit.Candidate->ApproxBytes, PwTestCandidateBytes);
    TestTrue(TEXT("CreatedAt was stamped by the registry"),
        Hit.Candidate->CreatedAt != FDateTime());
    TestFalse(TEXT("A fresh candidate carries no analysis"), Hit.Candidate->Analysis.IsValid());
    TestTrue(TEXT("A fresh candidate is not exported"),
        Hit.Candidate->ExportedAssetPath.IsEmpty());

    const FPwCandidateUsage Usage = Registry.Usage();
    TestEqual(TEXT("Usage counts both candidates"), Usage.NumCandidates, 2);
    TestEqual(TEXT("Usage sums both candidates' bytes"),
        Usage.ApproxBytes, PwTestCandidateBytes * 2);
    TestFalse(TEXT("Two tiny candidates are not over the default budget"), Usage.bOverBudget);

    return true;
}

// =========================================================================
// A2. RecipeDigest is a FUNCTION OF THE RECIPE, not decoration.
//
// The listing test only ever sees default-constructed recipes, so every digest
// it inspects is the same string and its "non-empty, under 120 chars"
// assertions hold for a digest that ignored its argument entirely. Two
// properties are asserted here instead, both of which a constant fails:
//   - determinism: the same recipe digests identically across calls, which is
//     what makes the digest usable as a fingerprint at all; and
//   - discrimination: recipes differing in ANY of the published scalars, and
//     recipes differing ONLY outside them, all digest differently. The last
//     case is the one that pins the CRC over the compact serialization -
//     fadeInMs is not one of the four scalars the digest spells out, so
//     dropping the hash and keeping only the scalars fails exactly there.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCandidateRecipeDigestDiscriminatesTest,
    "PinWright.audio.synth.candidates.RecipeDigestDiscriminatesRecipes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCandidateRecipeDigestDiscriminatesTest::RunTest(const FString& Parameters)
{
    FPwSynthRecipe Base;
    Base.Seed = 1234;
    Base.SampleRate = 48000;
    Base.DurationMs = 500.0;

    const FString BaseDigest = FPwCandidateRegistry::RecipeDigest(Base);
    TestFalse(TEXT("A recipe digests to something"), BaseDigest.IsEmpty());
    TestEqual(TEXT("The same recipe digests identically twice"),
        FPwCandidateRegistry::RecipeDigest(Base), BaseDigest);

    // A separately built copy, not the same object: the digest must depend on the
    // VALUES rather than on identity or on call order.
    {
        FPwSynthRecipe Same;
        Same.Seed = 1234;
        Same.SampleRate = 48000;
        Same.DurationMs = 500.0;
        TestEqual(TEXT("An equal recipe built separately digests identically"),
            FPwCandidateRegistry::RecipeDigest(Same), BaseDigest);
    }

    {
        FPwSynthRecipe DifferentSeed = Base;
        DifferentSeed.Seed = 1235;
        TestNotEqual(TEXT("A different seed digests differently"),
            FPwCandidateRegistry::RecipeDigest(DifferentSeed), BaseDigest);
    }
    {
        FPwSynthRecipe DifferentDuration = Base;
        DifferentDuration.DurationMs = 750.0;
        TestNotEqual(TEXT("A different duration digests differently"),
            FPwCandidateRegistry::RecipeDigest(DifferentDuration), BaseDigest);
    }
    {
        FPwSynthRecipe DifferentRate = Base;
        DifferentRate.SampleRate = 44100;
        TestNotEqual(TEXT("A different sample rate digests differently"),
            FPwCandidateRegistry::RecipeDigest(DifferentRate), BaseDigest);
    }
    {
        // None of the four scalars the digest spells out moves here, so only the
        // hash over the serialized recipe can separate these two.
        FPwSynthRecipe DifferentFade = Base;
        DifferentFade.Master.FadeInMs = Base.Master.FadeInMs + 7.0;
        TestNotEqual(TEXT("A change outside the spelled-out scalars still digests differently"),
            FPwCandidateRegistry::RecipeDigest(DifferentFade), BaseDigest);
    }

    return true;
}

// =========================================================================
// B. LRU order counts reads. A candidate the agent keeps comparing against
//    must outlive an untouched one, so the counterfactual assertion is that
//    the READ candidate is still resident and the unread one is gone.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCandidateRegistryLruTest,
    "PinWright.audio.synth.candidates.EvictsLeastRecentlyTouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCandidateRegistryLruTest::RunTest(const FString& Parameters)
{
    // Budget holds two candidates, not three.
    FPwCandidateRegistry Registry(PwTestCandidateBytes * 2, 100);

    const FString IdA = Registry.Add(MakePwTestCandidate());
    const FString IdB = Registry.Add(MakePwTestCandidate());

    // The touch that matters: a plain read of A, with B left alone. Insertion order alone
    // would make A the eviction victim.
    TestTrue(TEXT("Reading A hits"), Registry.Get(IdA).IsHit());

    const FString IdC = Registry.Add(MakePwTestCandidate());

    TestTrue(TEXT("The read candidate A survived"), Registry.Get(IdA).IsHit());
    TestTrue(TEXT("The newest candidate C survived"), Registry.Get(IdC).IsHit());

    const FPwCandidateLookupResult Gone = Registry.Get(IdB);
    TestFalse(TEXT("The unread candidate B was the one evicted"), Gone.IsHit());
    TestTrue(TEXT("B reports as evicted"), Gone.Status == EPwCandidateLookup::Evicted);
    TestEqual(TEXT("Only one candidate was evicted"), Registry.Usage().NumEvicted, (int64)1);
    TestEqual(TEXT("Two candidates remain resident"), Registry.Usage().NumCandidates, 2);

    return true;
}

// =========================================================================
// C. Eviction at the byte budget is observable: the id answers "evicted",
//    NOT "not found", and the payload carries the reason plus the budget.
//    A bare not-found is what makes an agent conclude it passed a bad id.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCandidateRegistryEvictedIsDistinctTest,
    "PinWright.audio.synth.candidates.EvictedIdReportsEvictedNotNotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCandidateRegistryEvictedIsDistinctTest::RunTest(const FString& Parameters)
{
    // Room for exactly one candidate's bytes.
    FPwCandidateRegistry Registry(PwTestCandidateBytes, 100);

    const FString IdA = Registry.Add(MakePwTestCandidate());
    const FString IdB = Registry.Add(MakePwTestCandidate());

    const FPwCandidateLookupResult Miss = Registry.Get(IdA);
    TestFalse(TEXT("A was evicted by the byte budget"), Miss.IsHit());
    TestTrue(TEXT("Status is Evicted"), Miss.Status == EPwCandidateLookup::Evicted);
    // The failure direction: the whole point is that this is NOT the answer a bad id gets.
    TestFalse(TEXT("Status is not NeverExisted"),
        Miss.Status == EPwCandidateLookup::NeverExisted);
    TestFalse(TEXT("Status is not RegistryEmpty (B is still resident)"),
        Miss.Status == EPwCandidateLookup::RegistryEmpty);

    // The ERROR CODE is the first thing error handling branches on, so the distinction has to
    // live there and not only in the payload's status field. Asserting the status alone would
    // not catch a regression that collapsed the two codes back into one.
    const FString EvictedCode(FPwCandidateRegistry::MissErrorCode(Miss.Status));
    TestEqual(TEXT("Error code is CANDIDATE_EVICTED"), EvictedCode,
        FString(ErrorCodes::ERR_CANDIDATE_EVICTED));
    TestNotEqual(TEXT("Error code is NOT the wrong-id code"), EvictedCode,
        FString(ErrorCodes::ERR_CANDIDATE_NOT_FOUND));

    // Same registry, same call, an id it never issued: the two must not answer alike.
    const FPwCandidateLookupResult Unknown = Registry.Get(TEXT("c4242_dead"));
    TestTrue(TEXT("An id this registry never issued reports NeverExisted"),
        Unknown.Status == EPwCandidateLookup::NeverExisted);
    TestNotEqual(TEXT("An evicted id and an unknown id do not share an error code"),
        EvictedCode, FString(FPwCandidateRegistry::MissErrorCode(Unknown.Status)));

    TestTrue(TEXT("The miss carries a removal record"), Miss.Removal.IsSet());
    if (Miss.Removal.IsSet())
    {
        TestTrue(TEXT("Removal reason is the byte budget, not the count cap"),
            Miss.Removal->Reason == EPwCandidateRemoval::ByteBudget);
        TestEqual(TEXT("Removal record remembers what the candidate cost"),
            Miss.Removal->ApproxBytes, PwTestCandidateBytes);
    }

    // Recovery information travels in the structured payload, which is what survives the
    // oversize-spill rewrite.
    const TSharedPtr<FJsonObject> Payload = Registry.BuildMissPayload(IdA, Miss);
    TestTrue(TEXT("Miss payload was built"), Payload.IsValid());
    if (!Payload.IsValid())
    {
        return false;
    }
    FString Status;
    TestTrue(TEXT("Payload carries a status discriminator"),
        Payload->TryGetStringField(TEXT("status"), Status));
    TestEqual(TEXT("Payload status is 'evicted'"), Status, FString(TEXT("evicted")));

    const TSharedPtr<FJsonObject>* Removal = nullptr;
    TestTrue(TEXT("Payload carries the removal block"),
        Payload->TryGetObjectField(TEXT("removal"), Removal));
    if (Removal != nullptr && (*Removal).IsValid())
    {
        FString Reason;
        (*Removal)->TryGetStringField(TEXT("reason"), Reason);
        TestEqual(TEXT("Removal reason is byte_budget"), Reason, FString(TEXT("byte_budget")));
    }

    // The budget rides along so the caller can act in the same round trip.
    const TSharedPtr<FJsonObject>* RegistryBlock = nullptr;
    TestTrue(TEXT("Payload carries the live budget"),
        Payload->TryGetObjectField(TEXT("registry"), RegistryBlock));
    if (RegistryBlock != nullptr && (*RegistryBlock).IsValid())
    {
        double MaxBytes = 0.0;
        (*RegistryBlock)->TryGetNumberField(TEXT("maxBytes"), MaxBytes);
        TestEqual(TEXT("Budget in the payload is the registry's real budget"),
            static_cast<int64>(MaxBytes), PwTestCandidateBytes);
    }

    FString Recovery;
    TestTrue(TEXT("Payload names a way out"),
        Payload->TryGetStringField(TEXT("recovery"), Recovery) && !Recovery.IsEmpty());
    // The message must never call an evicted candidate unknown.
    const FString Message = FPwCandidateRegistry::MakeMissMessage(IdA, Miss);
    TestTrue(TEXT("Message says the candidate existed"), Message.Contains(TEXT("existed")));

    TestTrue(TEXT("B, the survivor, still resolves"), Registry.Get(IdB).IsHit());
    return true;
}

// =========================================================================
// D. The count cap evicts too, and reports its own reason - a count-capped
//    eviction scored as memory pressure would send the caller to the wrong
//    knob.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCandidateRegistryCountBudgetTest,
    "PinWright.audio.synth.candidates.EvictsAtCountBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCandidateRegistryCountBudgetTest::RunTest(const FString& Parameters)
{
    // Bytes are effectively unlimited; only the count cap can bite.
    FPwCandidateRegistry Registry(FPwCandidateRegistry::DefaultMaxBytes, 2);

    const FString IdA = Registry.Add(MakePwTestCandidate());
    Registry.Add(MakePwTestCandidate());
    Registry.Add(MakePwTestCandidate());

    const FPwCandidateLookupResult Miss = Registry.Get(IdA);
    TestFalse(TEXT("The oldest candidate was evicted at the count cap"), Miss.IsHit());
    TestTrue(TEXT("Status is Evicted"), Miss.Status == EPwCandidateLookup::Evicted);
    TestTrue(TEXT("The removal record exists"), Miss.Removal.IsSet());
    if (Miss.Removal.IsSet())
    {
        TestTrue(TEXT("Reason is the count cap, not the byte budget"),
            Miss.Removal->Reason == EPwCandidateRemoval::CountBudget);
    }
    TestEqual(TEXT("The registry holds exactly the cap"), Registry.Usage().NumCandidates, 2);
    return true;
}

// =========================================================================
// E. An empty registry answers the empty case, not "bad id". Those point the
//    caller at different fixes: render something vs. check the id.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCandidateRegistryEmptyLookupTest,
    "PinWright.audio.synth.candidates.EmptyRegistryLookupIsDistinct",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCandidateRegistryEmptyLookupTest::RunTest(const FString& Parameters)
{
    FPwCandidateRegistry Registry;

    const FPwCandidateLookupResult Empty = Registry.Get(TEXT("c1_beef"));
    TestFalse(TEXT("Nothing resolves in an empty registry"), Empty.IsHit());
    TestTrue(TEXT("Status is RegistryEmpty"),
        Empty.Status == EPwCandidateLookup::RegistryEmpty);
    // The failure direction: reporting a bad id here is reporting zero as a small number.
    TestFalse(TEXT("Status is not NeverExisted"),
        Empty.Status == EPwCandidateLookup::NeverExisted);
    TestTrue(TEXT("Message says no candidates exist"),
        FPwCandidateRegistry::MakeMissMessage(TEXT("c1_beef"), Empty)
            .Contains(TEXT("No candidates exist")));

    // Again, the code and not only the status: an agent branches on the code first, and
    // "generate something" is a different remedy from "check your id".
    const FString EmptyCode(FPwCandidateRegistry::MissErrorCode(Empty.Status));
    TestEqual(TEXT("Error code is NO_CANDIDATES"), EmptyCode,
        FString(ErrorCodes::ERR_NO_CANDIDATES));
    TestNotEqual(TEXT("Error code is NOT the wrong-id code"), EmptyCode,
        FString(ErrorCodes::ERR_CANDIDATE_NOT_FOUND));

    // Once something IS resident, the same unknown id becomes a genuine unknown id.
    Registry.Add(MakePwTestCandidate());
    const FPwCandidateLookupResult Unknown = Registry.Get(TEXT("c99_beef"));
    TestTrue(TEXT("A non-empty registry reports an unknown id as never existing"),
        Unknown.Status == EPwCandidateLookup::NeverExisted);
    TestFalse(TEXT("...and no longer as an empty registry"),
        Unknown.Status == EPwCandidateLookup::RegistryEmpty);

    const FString UnknownCode(FPwCandidateRegistry::MissErrorCode(Unknown.Status));
    TestEqual(TEXT("...and now carries the wrong-id code"), UnknownCode,
        FString(ErrorCodes::ERR_CANDIDATE_NOT_FOUND));
    TestNotEqual(TEXT("The empty registry and the wrong id do not share a code"),
        EmptyCode, UnknownCode);

    return true;
}

// =========================================================================
// F. A discarded candidate is not an evicted one - the caller asked for this,
//    and the two need different recoveries. Also covers the registry-level
//    idempotence the batch verb depends on.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCandidateRegistryDiscardStatusTest,
    "PinWright.audio.synth.candidates.DiscardedIsDistinctFromEvicted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCandidateRegistryDiscardStatusTest::RunTest(const FString& Parameters)
{
    FPwCandidateRegistry Registry;
    const FString Id = Registry.Add(MakePwTestCandidate());

    TestTrue(TEXT("First discard removes a resident candidate"),
        Registry.Discard(Id) == EPwCandidateLookup::Found);
    // Idempotence at the registry level: the repeat converges instead of failing.
    TestTrue(TEXT("Second discard reports it was already discarded"),
        Registry.Discard(Id) == EPwCandidateLookup::Discarded);
    TestFalse(TEXT("A repeat discard is not reported as a fresh removal"),
        Registry.Discard(Id) == EPwCandidateLookup::Found);

    const FPwCandidateLookupResult Miss = Registry.Get(Id);
    TestTrue(TEXT("Lookup reports Discarded, not Evicted"),
        Miss.Status == EPwCandidateLookup::Discarded);
    TestEqual(TEXT("Wire spelling is 'already_discarded'"),
        FString(FPwCandidateRegistry::LexDiscardOutcome(Miss.Status)),
        FString(TEXT("already_discarded")));

    // Deliberate code choice, pinned here so it is a decision rather than an accident: a
    // self-discarded candidate reuses CANDIDATE_NOT_FOUND because "you removed it" is a
    // caller-side fact whose remedy matches a wrong id. It must still never be reported as a
    // budget eviction, which is a SYSTEM-side fact the caller did not cause.
    const FString DiscardedCode(FPwCandidateRegistry::MissErrorCode(Miss.Status));
    TestEqual(TEXT("A self-discarded candidate reuses the wrong-id code by design"),
        DiscardedCode, FString(ErrorCodes::ERR_CANDIDATE_NOT_FOUND));
    TestNotEqual(TEXT("...and is never reported as a budget eviction"),
        DiscardedCode, FString(ErrorCodes::ERR_CANDIDATE_EVICTED));
    TestEqual(TEXT("The status field keeps the distinction the code drops"),
        FString(FPwCandidateRegistry::LexLookupStatus(Miss.Status)),
        FString(TEXT("discarded")));

    TestEqual(TEXT("Byte accounting released the discarded candidate"),
        Registry.Usage().ApproxBytes, (int64)0);

    return true;
}

// =========================================================================
// G. audio.synth.discard with neither id nor all is an error - and, the point
//    of the test, it discards NOTHING. A silent discard-everything would pass
//    a "did it error?" check alone, so the survivor is asserted too.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCandidateDiscardNoSelectorTest,
    "PinWright.audio.synth.candidates.DiscardWithoutSelectorErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCandidateDiscardNoSelectorTest::RunTest(const FString& Parameters)
{
    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();
    Registry.DiscardAll();
    const FString Id = AddPwCandidateToPluginState();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("audio.synth.discard"), Payload, Capture);

    TestTrue(TEXT("Handler is registered"), bFound);
    TestTrue(TEXT("Handler responded"), Capture.bWasCalled);
    TestFalse(TEXT("A selector-less discard is an error, not a no-op success"),
        Capture.bSuccess);
    TestEqual(TEXT("ErrorCode is INVALID_PARAMS"), Capture.ErrorCode,
        FString(ErrorCodes::ERR_INVALID_PARAMS));
    // The assertion that matters: nothing was discarded.
    TestTrue(TEXT("The resident candidate survived the rejected call"),
        Registry.Get(Id).IsHit());

    // An explicitly empty ids array is a selector that matched nothing - also an error.
    TSharedPtr<FJsonObject> EmptyArrayPayload = MakeShared<FJsonObject>();
    EmptyArrayPayload->SetArrayField(TEXT("ids"), TArray<TSharedPtr<FJsonValue>>());
    FTestResponseCapture EmptyCapture;
    InvokeHandlerWithCapture(TEXT("audio.synth.discard"), EmptyArrayPayload, EmptyCapture);
    TestFalse(TEXT("An empty ids array is an error, not a zero-item success"),
        EmptyCapture.bSuccess);
    TestTrue(TEXT("The resident candidate survived that one too"), Registry.Get(Id).IsHit());

    // all:true beside explicit ids is contradictory and must not be guessed at.
    TSharedPtr<FJsonObject> BothPayload = MakeShared<FJsonObject>();
    BothPayload->SetBoolField(TEXT("all"), true);
    BothPayload->SetStringField(TEXT("id"), Id);
    FTestResponseCapture BothCapture;
    InvokeHandlerWithCapture(TEXT("audio.synth.discard"), BothPayload, BothCapture);
    TestFalse(TEXT("'all' together with ids is rejected"), BothCapture.bSuccess);
    TestTrue(TEXT("...and discarded nothing"), Registry.Get(Id).IsHit());

    Registry.DiscardAll();
    return true;
}

// =========================================================================
// H. audio.synth.discard is idempotent and reports per id what happened. A
//    client retry after a response-only timeout must converge, not fail.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCandidateDiscardIdempotentTest,
    "PinWright.audio.synth.candidates.DiscardIsIdempotent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCandidateDiscardIdempotentTest::RunTest(const FString& Parameters)
{
    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();
    Registry.DiscardAll();
    const FString Id = AddPwCandidateToPluginState();

    const auto RunDiscard = [](const FString& TargetId, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Ids;
        Ids.Add(MakeShared<FJsonValueString>(TargetId));
        Payload->SetArrayField(TEXT("ids"), Ids);
        return InvokeHandlerWithCapture(TEXT("audio.synth.discard"), Payload, Capture);
    };

    FTestResponseCapture First;
    TestTrue(TEXT("Handler is registered"), RunDiscard(Id, First));
    TestTrue(TEXT("First discard succeeds"), First.bSuccess);
    if (!First.bSuccess || !First.Result.IsValid())
    {
        return false;
    }
    double Discarded = 0.0;
    First.Result->TryGetNumberField(TEXT("discarded"), Discarded);
    TestEqual(TEXT("First call discarded one candidate"), static_cast<int32>(Discarded), 1);

    FTestResponseCapture Second;
    RunDiscard(Id, Second);
    TestTrue(TEXT("The retry converges instead of failing"), Second.bSuccess);
    if (!Second.bSuccess || !Second.Result.IsValid())
    {
        return false;
    }
    double SecondDiscarded = 0.0;
    double SecondAlready = 0.0;
    Second.Result->TryGetNumberField(TEXT("discarded"), SecondDiscarded);
    Second.Result->TryGetNumberField(TEXT("alreadyDiscarded"), SecondAlready);
    // The failure direction: the retry must not claim it removed anything.
    TestEqual(TEXT("The retry discarded nothing"), static_cast<int32>(SecondDiscarded), 0);
    TestEqual(TEXT("The retry reports it was already discarded"),
        static_cast<int32>(SecondAlready), 1);

    const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
    TestTrue(TEXT("Per-id measurements are reported"),
        Second.Result->TryGetArrayField(TEXT("results"), Results));
    if (Results != nullptr && Results->Num() == 1 && (*Results)[0]->Type == EJson::Object)
    {
        FString Outcome;
        (*Results)[0]->AsObject()->TryGetStringField(TEXT("outcome"), Outcome);
        TestEqual(TEXT("Per-id outcome is already_discarded"), Outcome,
            FString(TEXT("already_discarded")));
    }

    // An id this registry never issued is a different measurement again.
    FTestResponseCapture Unknown;
    RunDiscard(TEXT("c99999_dead"), Unknown);
    TestTrue(TEXT("Discarding an unknown id still responds successfully"), Unknown.bSuccess);
    if (Unknown.bSuccess && Unknown.Result.IsValid())
    {
        double NotFound = 0.0;
        Unknown.Result->TryGetNumberField(TEXT("notFound"), NotFound);
        TestEqual(TEXT("The unknown id is counted as not found, not as discarded"),
            static_cast<int32>(NotFound), 1);
    }

    Registry.DiscardAll();
    return true;
}

// =========================================================================
// I. audio.synth.list_candidates pages, carries a digest instead of the
//    recipe, and publishes the budget so eviction is visible in advance.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCandidateListTest,
    "PinWright.audio.synth.candidates.ListPagesAndReportsBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCandidateListTest::RunTest(const FString& Parameters)
{
    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();
    Registry.DiscardAll();

    // Empty registry first: the listing must say so rather than return a bare empty array.
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("Handler is registered"), InvokeHandlerWithCapture(
            TEXT("audio.synth.list_candidates"), MakeShared<FJsonObject>(), Capture));
        TestTrue(TEXT("Listing an empty registry succeeds"), Capture.bSuccess);
        TestTrue(TEXT("...and carries a result object"), Capture.Result.IsValid());
        if (Capture.Result.IsValid())
        {
            double Total = -1.0;
            Capture.Result->TryGetNumberField(TEXT("total"), Total);
            TestEqual(TEXT("Total is zero"), static_cast<int32>(Total), 0);
        }
        TestTrue(TEXT("Message names the empty case"),
            Capture.Message.Contains(TEXT("No candidates exist")));
    }

    AddPwCandidateToPluginState();
    AddPwCandidateToPluginState();
    const FString Newest = AddPwCandidateToPluginState();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetNumberField(TEXT("limit"), 2);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("audio.synth.list_candidates"), Payload, Capture);
    TestTrue(TEXT("Listing succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    double Total = 0.0;
    double Returned = 0.0;
    Capture.Result->TryGetNumberField(TEXT("total"), Total);
    Capture.Result->TryGetNumberField(TEXT("returned"), Returned);
    TestEqual(TEXT("Total counts every candidate"), static_cast<int32>(Total), 3);
    TestEqual(TEXT("The page honours limit"), static_cast<int32>(Returned), 2);

    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    TestTrue(TEXT("Rows are returned"), Capture.Result->TryGetArrayField(TEXT("candidates"), Rows));
    if (Rows != nullptr && Rows->Num() == 2 && (*Rows)[0]->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
        FString RowId;
        Row->TryGetStringField(TEXT("id"), RowId);
        TestEqual(TEXT("Newest candidate is listed first"), RowId, Newest);

        FString Digest;
        TestTrue(TEXT("Row carries a recipe digest"),
            Row->TryGetStringField(TEXT("recipeDigest"), Digest) && !Digest.IsEmpty());
        TestTrue(TEXT("The digest is short, not a serialized recipe"), Digest.Len() < 120);

        bool bExported = true;
        Row->TryGetBoolField(TEXT("exported"), bExported);
        TestFalse(TEXT("An unexported candidate reports exported=false"), bExported);
        TestFalse(TEXT("...and carries no asset path"),
            Row->HasField(TEXT("exportedAssetPath")));
    }

    const TSharedPtr<FJsonObject>* RegistryBlock = nullptr;
    TestTrue(TEXT("The listing publishes the budget"),
        Capture.Result->TryGetObjectField(TEXT("registry"), RegistryBlock));
    if (RegistryBlock != nullptr && (*RegistryBlock).IsValid())
    {
        double Count = 0.0;
        double MaxBytes = 0.0;
        (*RegistryBlock)->TryGetNumberField(TEXT("count"), Count);
        (*RegistryBlock)->TryGetNumberField(TEXT("maxBytes"), MaxBytes);
        TestEqual(TEXT("Registry count matches the total"), static_cast<int32>(Count), 3);
        TestTrue(TEXT("Registry budget is published"), MaxBytes > 0.0);
    }

    // Paging: the tail page returns the remainder rather than repeating the head.
    TSharedPtr<FJsonObject> TailPayload = MakeShared<FJsonObject>();
    TailPayload->SetNumberField(TEXT("limit"), 2);
    TailPayload->SetNumberField(TEXT("offset"), 2);
    FTestResponseCapture TailCapture;
    // The guard conditions are ASSERTED rather than only branched on: without them a handler
    // that errored on `offset` - or that stopped being registered - would skip the only paging
    // assertion in this file and the test would still pass green.
    TestTrue(TEXT("Handler is registered for the tail page"), InvokeHandlerWithCapture(
        TEXT("audio.synth.list_candidates"), TailPayload, TailCapture));
    TestTrue(TEXT("The tail page listing succeeds"), TailCapture.bSuccess);
    TestTrue(TEXT("The tail page carries a result object"), TailCapture.Result.IsValid());
    if (TailCapture.bSuccess && TailCapture.Result.IsValid())
    {
        double TailReturned = 0.0;
        TailCapture.Result->TryGetNumberField(TEXT("returned"), TailReturned);
        TestEqual(TEXT("The tail page returns the remaining one row"),
            static_cast<int32>(TailReturned), 1);
    }

    Registry.DiscardAll();
    return true;
}

// =========================================================================
// J. audio.synth.discard with all:true empties the registry and stays honest
//    on the repeat: zero discarded, not a fabricated success count.
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCandidateDiscardAllTest,
    "PinWright.audio.synth.candidates.DiscardAllIsMeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCandidateDiscardAllTest::RunTest(const FString& Parameters)
{
    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();
    Registry.DiscardAll();
    const FString Id = AddPwCandidateToPluginState();
    AddPwCandidateToPluginState();

    const auto RunDiscardAll = [](FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetBoolField(TEXT("all"), true);
        InvokeHandlerWithCapture(TEXT("audio.synth.discard"), Payload, Capture);
    };

    FTestResponseCapture First;
    RunDiscardAll(First);
    TestTrue(TEXT("discard all succeeds"), First.bSuccess);
    TestTrue(TEXT("...and carries a result object"), First.Result.IsValid());
    if (First.Result.IsValid())
    {
        double Discarded = 0.0;
        First.Result->TryGetNumberField(TEXT("discarded"), Discarded);
        TestEqual(TEXT("Both resident candidates were discarded"),
            static_cast<int32>(Discarded), 2);
    }
    TestEqual(TEXT("The registry is empty afterwards"), Registry.Usage().NumCandidates, 0);
    TestTrue(TEXT("A discarded id reports discarded, not unknown"),
        Registry.Get(Id).Status == EPwCandidateLookup::Discarded);

    FTestResponseCapture Second;
    RunDiscardAll(Second);
    TestTrue(TEXT("The repeat converges"), Second.bSuccess);
    TestTrue(TEXT("...and carries a result object"), Second.Result.IsValid());
    if (Second.Result.IsValid())
    {
        double Discarded = -1.0;
        Second.Result->TryGetNumberField(TEXT("discarded"), Discarded);
        // The failure direction: a repeat must not report work it did not do.
        TestEqual(TEXT("The repeat discarded nothing"), static_cast<int32>(Discarded), 0);
    }

    return true;
}

// =========================================================================
// K. The audition seam. AudioSynthCandidateHandler.cpp binds
//    PwAuditionCandidateResolver, which is what makes audio.synth.audition
//    accept a candidateId at all - so per the note in
//    TestAudioSynthAudition.cpp case E, the candidateId round-trip is owned
//    here. The assertion that matters is that a miss keeps its STRUCTURE
//    across the seam: an evicted candidate must not arrive at the audition
//    caller as an anonymous "not found".
// =========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwCandidateAuditionSeamTest,
    "PinWright.audio.synth.candidates.AuditionSeamCarriesStructuredMiss",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPwCandidateAuditionSeamTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("The candidate registry is wired into the audition seam"),
        PwAuditionCandidateResolver().IsBound());
    if (!PwAuditionCandidateResolver().IsBound())
    {
        return false;
    }

    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();
    Registry.DiscardAll();
    const FString Id = AddPwCandidateToPluginState();

    // Resolve through the delegate exactly as the audition handler does.
    {
        FPwAudioBuffer Resolved;
        FString ErrorCode;
        FString Error;
        TSharedPtr<FJsonObject> ErrorData;
        TestTrue(TEXT("A resident candidate resolves through the seam"),
            PwAuditionCandidateResolver().Execute(Id, Resolved, ErrorCode, Error, ErrorData));
        TestEqual(TEXT("The seam hands back the rendered frames"),
            Resolved.NumFrames(), PwTestFrames);
        TestTrue(TEXT("A hit carries no error payload"), !ErrorData.IsValid());
    }

    // Now make the same id an evicted-class miss by discarding it, and check the
    // structure survives.
    Registry.Discard(Id);
    {
        FPwAudioBuffer Resolved;
        FString ErrorCode;
        FString Error;
        TSharedPtr<FJsonObject> ErrorData;
        TestFalse(TEXT("A removed candidate does not resolve"),
            PwAuditionCandidateResolver().Execute(Id, Resolved, ErrorCode, Error, ErrorData));
        TestTrue(TEXT("The miss carries the structured payload, not just prose"),
            ErrorData.IsValid());
        if (ErrorData.IsValid())
        {
            FString Status;
            ErrorData->TryGetStringField(TEXT("status"), Status);
            // The failure direction: an id the registry issued must never come back as an
            // anonymous unknown id.
            TestEqual(TEXT("Status names what actually happened"), Status,
                FString(TEXT("discarded")));
            TestNotEqual(TEXT("Status is not the generic never_existed"), Status,
                FString(TEXT("never_existed")));
            TestTrue(TEXT("The budget rides along"), ErrorData->HasField(TEXT("registry")));
        }
        TestTrue(TEXT("The prose message says the candidate existed"),
            Error.Contains(TEXT("existed")));
    }

    Registry.DiscardAll();
    return true;
}
