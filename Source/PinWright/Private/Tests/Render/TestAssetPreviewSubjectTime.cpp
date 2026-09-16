// Copyright (c) 2026 Alexander Penkin. MIT License.

// The `time` / `times` arguments on render.capture_asset_preview: the cell of the convergence
// plan's own acceptance matrix (§1a "Time series" x Niagara) that never landed.
//
// WHAT THE GAP WAS. C5 shipped a complete, tested Niagara time driver -- SetAgeUpdateMode ->
// ResetSystem -> AdvanceSimulation -> read the achieved age -> hold at DesiredAge -- and the
// provider publishes `timeSupported: true`. No verb called it. render.capture_animation_preview is
// Persona-gated and refuses `niagara` (plan §1c resolution 2), camera.animation_shots' time axis is
// a Level Sequence driving a PLACED actor, and render.capture_asset_preview took the resolver's
// time setter into a variable literally named UnusedTimeSetter. So `subject.timeSupported`
// advertised an axis no caller could reach.
//
// WHY THIS VERB AND NOT A niagara BRANCH ON capture_animation_preview. This is the only production
// call site that reaches the kind registry for an asset subject, so it is the only place a
// provider's setter can surface at all. The sibling verb's whole payload counts in FRAMES at an
// animation's sampling rate, its toolkit gate is Persona, and it already carries a typed refusal
// pointing `niagara` HERE -- a branch there would make the verb name, the refusal, and the plan all
// disagree.
//
// WHY THESE NEED NO VIEWPORT. Every refusal below is reached above the resolver call: the asset is
// loaded (so the class gate passes) but nothing is opened and nothing is drawn. That matters
// because a test needing a live preview viewport takes a conditional-skip path on a busy machine
// and reports success without running its assertions -- board ticket
// B-test-skips-assertions-silently, which fired on exactly this cluster of verbs. The one test at
// the bottom that must open an editor says out loud, as a WARNING carrying a skip marker, when it
// measured nothing.
//
// WHAT IS NOT ASSERTED HERE. That the crossing marks one pose per instant, and that the primitive
// then drives the subject once per instant, live in Tests/Render/TestSubjectTimeSeriesLayout.cpp,
// where they run against no editor at all.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"

#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"

#include "Dispatch/RpcDispatcher.h"
#include "Handlers/ParamSpec.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

namespace
{
    // Distinctly prefixed: anonymous namespaces merge inside one Unity translation unit, so a bare
    // name a sibling capture suite also uses is a latent ODR clash rather than a compile error.
    const TCHAR* PWSubjTimeMethod = TEXT("render.capture_asset_preview");
    // A Static Mesh: loadable, served by the class gate, and carrying NO time axis. Every
    // argument-shape refusal below is reached with it, which is what keeps them viewport-free.
    const TCHAR* PWSubjTimeCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

    TSharedPtr<FJsonObject> PWSubjTimeCubePayload()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PWSubjTimeCubePath);
        return Payload;
    }

    TSharedPtr<FJsonValue> PWSubjTimeNumber(double Value)
    {
        return MakeShared<FJsonValueNumber>(Value);
    }

    // Direct handler invocation, which is the path that reaches the argument parsing without the
    // dispatcher's param gate in front of it.
    bool PWSubjTimeInvoke(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Payload,
        FTestResponseCapture& OutCapture)
    {
        return Test.TestTrue(TEXT("render.capture_asset_preview handler found"),
            InvokeHandlerWithCapture(PWSubjTimeMethod, Payload, OutCapture));
    }

    void PWSubjTimeCloseEditorFor(const TCHAR* AssetPath)
    {
        if (!GEditor)
        {
            return;
        }
        if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
        {
            if (UObject* Asset = LoadObject<UObject>(nullptr, AssetPath))
            {
                Subsystem->CloseAllEditorsForAsset(Asset);
            }
        }
    }
}

// ============================================================================
// The arguments exist on the wire.
//
// This is not ceremony. `allowBlank` shipped on this verb READ by the shared parser and ABSENT from
// RPC_PARAMS, so the dispatcher's unknown-param gate refused every wire call carrying it and the
// branch that consumed it was dead. A time argument the schema does not declare would be the same
// defect: the handler code below would be unreachable from any real caller and every test in this
// file would still pass, because they all take the direct-invocation path.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewTimeArgsAreDeclaredTest,
    "PinWright.render.capture_asset_preview.TimeArgumentsReachTheHandlerOnTheWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewTimeArgsAreDeclaredTest::RunTest(const FString& Parameters)
{
    // UNABLE TO FAIL IF only the schema half ran: RPC_PARAMS is a declaration, and a declaration
    // proves what someone typed, not what the dispatcher does with it. The behaviour half below is
    // what makes this test mean "a caller can send this".
    TestNotNull(TEXT("'time' is declared, so a wire caller is not refused with UNKNOWN_PARAMS"),
        GetRegisteredParamSpec(PWSubjTimeMethod, TEXT("time")));
    TestNotNull(TEXT("'times' is declared"),
        GetRegisteredParamSpec(PWSubjTimeMethod, TEXT("times")));

    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // A path that cannot load takes the handler to ASSET_NOT_FOUND. Reaching that code proves the
    // param gate PASSED -- nothing was opened and nothing was drawn, and the answer is neither
    // UNKNOWN_PARAMS (the gate refused) nor a success (something got captured).
    //
    // UNABLE TO FAIL IF the assertion were merely `!bSuccess`: UNKNOWN_PARAMS is also a failure, and
    // it is exactly the failure this test exists to rule out. The code is compared, not the flag.
    for (const TCHAR* Key : {TEXT("time"), TEXT("times")})
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"),
            FString::Printf(TEXT("/Game/MCP_SubjTimeAbsent/Mesh_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
        if (FCString::Strcmp(Key, TEXT("times")) == 0)
        {
            Payload->SetArrayField(TEXT("times"), {PWSubjTimeNumber(0.5)});
        }
        else
        {
            Payload->SetNumberField(TEXT("time"), 0.5);
        }

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, PWSubjTimeMethod,
            TEXT("time-declared"), Payload, bSuccess, ErrorCode);
        TestFalse(*FString::Printf(TEXT("a missing asset is still an error with '%s'"), Key), bSuccess);
        TestEqual(*FString::Printf(
                      TEXT("'%s' passes the param gate and the handler reaches its own asset lookup"),
                      Key),
            ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    }
    return true;
}

// ============================================================================
// Two spellings of one axis.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewTimeAndTimesRefusedTest,
    "PinWright.render.capture_asset_preview.TimeAndTimesTogetherAreRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewTimeAndTimesRefusedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = PWSubjTimeCubePayload();
    Payload->SetNumberField(TEXT("time"), 0.25);
    Payload->SetArrayField(TEXT("times"), {PWSubjTimeNumber(1.0)});

    FTestResponseCapture Capture;
    if (!PWSubjTimeInvoke(*this, Payload, Capture))
    {
        return false;
    }

    // UNABLE TO FAIL IF the payload named only one instant in each key with the SAME value: the two
    // keys carry 0.25 and 1.0, so a handler that silently ranked one over the other would capture a
    // frame and this would read bSuccess=true. The values differ on purpose.
    TestFalse(TEXT("writing both spellings is refused, not ranked: a caller who wrote both believes "
                   "one of them is in force"),
        Capture.bSuccess);
    TestEqual(TEXT("and it is INVALID_ARGUMENT -- caller-fixable, not an unsupported-kind dead end"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    // UNABLE TO FAIL IF only the code were checked: a refusal that names neither key leaves the
    // caller guessing which of the two to delete.
    TestTrue(TEXT("the refusal names both keys so the caller can drop one"),
        Capture.Message.Contains(TEXT("time")) && Capture.Message.Contains(TEXT("times")));
    return true;
}

// ============================================================================
// Malformed instants, refused before anything is opened.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewMalformedInstantsTest,
    "PinWright.render.capture_asset_preview.MalformedInstantsAreRefusedBeforeAnythingOpens",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewMalformedInstantsTest::RunTest(const FString& Parameters)
{
    struct FCase
    {
        const TCHAR* What;
        TFunction<void(const TSharedPtr<FJsonObject>&)> Apply;
        // A fragment the message must carry. Checking it is what stops a generic catch-all refusal
        // from passing every row of this table.
        const TCHAR* MessageFragment;
    };

    const TArray<FCase> Cases = {
        {TEXT("times is not an array"),
         [](const TSharedPtr<FJsonObject>& P) { P->SetNumberField(TEXT("times"), 1.0); },
         TEXT("array")},
        {TEXT("times is empty"),
         [](const TSharedPtr<FJsonObject>& P)
         { P->SetArrayField(TEXT("times"), TArray<TSharedPtr<FJsonValue>>()); },
         TEXT("empty")},
        {TEXT("an entry is not a number"),
         [](const TSharedPtr<FJsonObject>& P)
         {
             P->SetArrayField(TEXT("times"),
                 {PWSubjTimeNumber(0.0), MakeShared<FJsonValueString>(TEXT("half"))});
         },
         TEXT("times[1]")},
        {TEXT("an entry is before the subject's own start"),
         [](const TSharedPtr<FJsonObject>& P)
         { P->SetArrayField(TEXT("times"), {PWSubjTimeNumber(0.0), PWSubjTimeNumber(-1.0)}); },
         TEXT("times[1]")},
        {TEXT("a single negative time"),
         [](const TSharedPtr<FJsonObject>& P) { P->SetNumberField(TEXT("time"), -0.5); },
         TEXT("time")},
    };

    for (const FCase& Case : Cases)
    {
        TSharedPtr<FJsonObject> Payload = PWSubjTimeCubePayload();
        Case.Apply(Payload);

        FTestResponseCapture Capture;
        if (!PWSubjTimeInvoke(*this, Payload, Capture))
        {
            return false;
        }

        // UNABLE TO FAIL IF the fixture asset did not exist or were of an unserved class: the
        // handler would then answer ASSET_NOT_FOUND or UNSUPPORTED_ASSET_EDITOR for every row and
        // the whole table would pass while proving nothing about instant parsing. The engine cube
        // loads and is served, so INVALID_ARGUMENT can only come from the time parsing itself.
        TestFalse(*FString::Printf(TEXT("%s is refused"), Case.What), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s is INVALID_ARGUMENT, not a capture of an arbitrary "
                                        "moment"), Case.What),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        // UNABLE TO FAIL IF only the code were compared: all five rows share it, so one generic
        // message would satisfy the whole table and a caller would not learn WHICH entry was bad.
        TestTrue(*FString::Printf(TEXT("the refusal for '%s' names '%s'"),
                     Case.What, Case.MessageFragment),
            Capture.Message.Contains(Case.MessageFragment));
    }
    return true;
}

// ============================================================================
// The ceiling counts the PRODUCT.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewInstantsMultiplyTheCeilingTest,
    "PinWright.render.capture_asset_preview.InstantsMultiplyTheShotCeiling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewInstantsMultiplyTheCeilingTest::RunTest(const FString& Parameters)
{
    // Five instants x the six axis-aligned views = 30, over the 24-shot ceiling. Neither factor
    // exceeds it alone: `views:"sides"` is 6 and five instants is 5, so this can only be refused by
    // a check that multiplies.
    //
    // UNABLE TO FAIL IF either factor were itself over the ceiling -- the pre-existing
    // Plan.Num() > GMaxOrbitShots check would then refuse it and the test would pass without the
    // multiplication existing at all. 5 and 6 are both well under 24 on purpose.
    TSharedPtr<FJsonObject> Payload = PWSubjTimeCubePayload();
    Payload->SetStringField(TEXT("views"), TEXT("sides"));
    Payload->SetArrayField(TEXT("times"), {
        PWSubjTimeNumber(0.0), PWSubjTimeNumber(0.25), PWSubjTimeNumber(0.5),
        PWSubjTimeNumber(0.75), PWSubjTimeNumber(1.0)});

    FTestResponseCapture Capture;
    if (!PWSubjTimeInvoke(*this, Payload, Capture))
    {
        return false;
    }

    TestFalse(TEXT("30 shots is refused rather than truncated to 24: a truncated set reads as a "
                   "complete set of the wrong thing"),
        Capture.bSuccess);
    TestEqual(TEXT("and it is the existing TOO_MANY_SHOTS code, not a new one"),
        Capture.ErrorCode, FString(TEXT("TOO_MANY_SHOTS")));
    // UNABLE TO FAIL IF the message were not inspected: "Requested 30 shots exceeds 24" is true but
    // leaves the caller unable to see WHICH factor to cut. Both factors have to appear.
    TestTrue(TEXT("the refusal names the product and both factors, so the caller knows what to drop"),
        Capture.Message.Contains(TEXT("30")) &&
        Capture.Message.Contains(TEXT("5 instants")) &&
        Capture.Message.Contains(TEXT("6 cameras")));

    // The counterpart, and the reason the check uses `>` on the product rather than on either
    // factor: 4 x 6 = 24 is exactly the ceiling and must NOT be refused. Reaching a resolve/capture
    // outcome (whatever this host can do) rather than TOO_MANY_SHOTS is the assertion.
    //
    // UNABLE TO FAIL IF this asserted success: on a host that cannot open a Static Mesh editor the
    // call legitimately fails for an unrelated reason, and demanding success would make the test
    // report a host problem as a ceiling defect. Only the specific wrong answer is excluded.
    TSharedPtr<FJsonObject> AtCeiling = PWSubjTimeCubePayload();
    AtCeiling->SetStringField(TEXT("views"), TEXT("sides"));
    AtCeiling->SetArrayField(TEXT("times"), {
        PWSubjTimeNumber(0.0), PWSubjTimeNumber(0.25), PWSubjTimeNumber(0.5), PWSubjTimeNumber(0.75)});

    FTestResponseCapture CeilingCapture;
    if (!PWSubjTimeInvoke(*this, AtCeiling, CeilingCapture))
    {
        return false;
    }
    PWSubjTimeCloseEditorFor(PWSubjTimeCubePath);
    TestNotEqual(TEXT("4 instants x 6 cameras is exactly 24 and is NOT refused by the ceiling"),
        CeilingCapture.ErrorCode, FString(TEXT("TOO_MANY_SHOTS")));
    return true;
}

// ============================================================================
// A kind with no time axis: the typed refusal, and it is the PROVIDER'S OWN.
//
// This one has to resolve the subject, so it opens a Static Mesh editor. It is the only test in
// this file that can be skipped by the host, and it says so when it is.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewNoTimeAxisRefusalTest,
    "PinWright.render.capture_asset_preview.ATimeOnASubjectWithNoAxisIsATypedRefusal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewNoTimeAxisRefusalTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = PWSubjTimeCubePayload();
    Payload->SetNumberField(TEXT("time"), 0.5);

    FTestResponseCapture Capture;
    if (!PWSubjTimeInvoke(*this, Payload, Capture))
    {
        return false;
    }
    PWSubjTimeCloseEditorFor(PWSubjTimeCubePath);

    // A Static Mesh has no time axis, so the ONLY correct outcome is a refusal. A success would
    // mean the instant was silently dropped and a frame of an unknown moment returned, which is
    // indistinguishable from a frame of the right one.
    if (!TestFalse(TEXT("asking a Static Mesh for an instant is refused, never silently dropped"),
            Capture.bSuccess))
    {
        return false;
    }

    // The host may not be able to open a Static Mesh editor at all, in which case the resolve fails
    // before the time refusal is reached. That is a skip, and it is announced -- the totals cannot
    // tell a test that ran its assertions from one that did not.
    //
    // UNABLE TO FAIL IF the skip branch accepted any error code: PREVIEW_VIEWPORT_NOT_FOUND and
    // OPEN_FAILED are host problems, but ASSET_NOT_FOUND or INVALID_ARGUMENT here would mean the
    // fixture or the payload is wrong and must fail rather than skip.
    if (Capture.ErrorCode == TEXT("PREVIEW_VIEWPORT_NOT_FOUND") ||
        Capture.ErrorCode == TEXT("OPEN_FAILED") ||
        Capture.ErrorCode == TEXT("EDITOR_NOT_AVAILABLE"))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-static-mesh-editor"),
            FString::Printf(TEXT("resolve failed with %s before the no-time-axis refusal could be "
                                 "reached: %s"), *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    TestEqual(TEXT("the refusal is UNSUPPORTED_ASSET_EDITOR -- the kind has no axis, which is not "
                   "caller-fixable by changing an argument value"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ASSET_EDITOR")));
    // UNABLE TO FAIL IF only the code were checked. The whole point of harvesting the PROVIDER'S
    // refusal instead of writing a fresh one here is that it names the kind and what would supply
    // an axis; a message that said only "unsupported" would satisfy a code check and teach nothing.
    TestTrue(TEXT("the provider's own wording survives: it names the Static Mesh and says a time "
                  "series over one is not a thing that exists"),
        Capture.Message.Contains(TEXT("Static Mesh")));
    // UNABLE TO FAIL IF the pointer sentence were absent: this verb serves several kinds and the
    // caller has to learn which verb DOES own a frame burst over an animation.
    TestTrue(TEXT("and the refusal names the verbs that do serve a time axis"),
        Capture.Message.Contains(TEXT("render.capture_animation_preview")) &&
        Capture.Message.Contains(TEXT("camera.animation_shots")));
    // UNABLE TO FAIL IF this were left out: the plan's standing rule is that no new error code is
    // minted for a kind a verb cannot serve, and a bespoke code here would read as a new one.
    TestFalse(TEXT("no new error code was minted for this refusal"),
        Capture.ErrorCode.Contains(TEXT("TIME")));
    return true;
}
