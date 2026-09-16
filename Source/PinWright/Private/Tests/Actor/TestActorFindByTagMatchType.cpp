// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board ticket B-find-by-tag-matchtype-silent-fallback.
//
// actor.find_by_tag tested `matchType == "contains"` and let EVERY other value fall
// into the else branch, which ran exact FName equality. So a typo, or a token borrowed
// from a sibling verb's vocabulary, produced a well-formed, plausible, silently
// NARROWER result set: no error, no warning, and no echo of the mode actually used.
// The caller reads it as "nothing is tagged that way" rather than "your mode was
// ignored".
//
// Both tests are failure-direction: restoring the silent fallback makes the first one
// see a success where it requires an INVALID_MODE refusal, and removing the echo makes
// the second one find no matchType field to read.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"

namespace FindByTagMatchTypeTestUtils
{
    // A tag no fixture uses. The point of these tests is the ARGUMENT contract, which
    // is decided before the world walk, so the row set is deliberately empty and no
    // level actor has to be spawned.
    const TCHAR* const ProbeTag = TEXT("PinWrightMatchTypeProbeTag");

    TSharedPtr<FJsonObject> MakePayload(const TCHAR* MatchType)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("tag"), ProbeTag);
        if (MatchType)
        {
            Payload->SetStringField(TEXT("matchType"), MatchType);
        }
        // Pin the editor world: 'auto' is PIE-first, and which world answers is not
        // what these tests are about.
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        return Payload;
    }
}

// ============================================================================
// An unrecognised matchType is a caller error, not a mode to guess at.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByTagUnknownMatchTypeTest,
    "PinWright.actor.find_by_tag.UnknownMatchTypeIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByTagUnknownMatchTypeTest::RunTest(const FString& Parameters)
{
    using namespace FindByTagMatchTypeTestUtils;

    // Every one of these used to return a success carrying exact-match rows.
    // "conatins" is the typo from the ticket; "prefix" and "regex" are the shared
    // NameMatch vocabulary a caller learns on actor.list and reasonably tries here;
    // "substring_typo" stands for anything else.
    const TArray<FString> Rejected = {
        TEXT("conatins"), TEXT("prefix"), TEXT("regex"), TEXT("substring_typo")
    };

    for (const FString& Mode : Rejected)
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("actor.find_by_tag handler registered"),
            InvokeHandlerWithCapture(TEXT("actor.find_by_tag"), MakePayload(*Mode), Capture));
        TestFalse(*FString::Printf(TEXT("matchType '%s' is not answered as a success"), *Mode),
            Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("matchType '%s' is refused with INVALID_MODE"), *Mode),
            Capture.ErrorCode, FString(TEXT("INVALID_MODE")));
        // The message has to be actionable on its own: an agent that cannot read the
        // accepted values out of the refusal will guess again.
        TestTrue(*FString::Printf(TEXT("the refusal of '%s' names 'exact'"), *Mode),
            Capture.Message.Contains(TEXT("exact")));
        TestTrue(*FString::Printf(TEXT("the refusal of '%s' names 'contains'"), *Mode),
            Capture.Message.Contains(TEXT("contains")));
        TestTrue(*FString::Printf(TEXT("the refusal of '%s' quotes what was sent"), *Mode),
            Capture.Message.Contains(Mode));
    }
    return true;
}

// ============================================================================
// The accepted values still work, and the response says which one ran.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorFindByTagResolvedMatchTypeEchoTest,
    "PinWright.actor.find_by_tag.ResolvedMatchTypeIsEchoed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorFindByTagResolvedMatchTypeEchoTest::RunTest(const FString& Parameters)
{
    using namespace FindByTagMatchTypeTestUtils;

    // Sent value -> canonical value the response must report. An omitted matchType
    // still has to say which semantics ran, and an alias must echo the canonical
    // spelling rather than the one the caller happened to send.
    const TArray<TPair<const TCHAR*, FString>> Cases = {
        { nullptr,             FString(TEXT("exact")) },
        { TEXT("exact"),       FString(TEXT("exact")) },
        { TEXT("contains"),    FString(TEXT("contains")) },
        { TEXT("substring"),   FString(TEXT("contains")) },
        { TEXT("  Contains "), FString(TEXT("contains")) },
    };

    for (const TPair<const TCHAR*, FString>& Case : Cases)
    {
        const FString Label = Case.Key ? FString(Case.Key) : FString(TEXT("<omitted>"));
        FTestResponseCapture Capture;
        TestTrue(TEXT("actor.find_by_tag handler registered"),
            InvokeHandlerWithCapture(TEXT("actor.find_by_tag"), MakePayload(Case.Key), Capture));
        TestTrue(*FString::Printf(TEXT("matchType '%s' is accepted"), *Label), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            continue;
        }
        FString Echoed;
        TestTrue(*FString::Printf(TEXT("matchType '%s' echoes a resolved mode"), *Label),
            Capture.Result->TryGetStringField(TEXT("matchType"), Echoed));
        TestEqual(*FString::Printf(TEXT("matchType '%s' resolves to the canonical spelling"), *Label),
            Echoed, Case.Value);
    }
    return true;
}
