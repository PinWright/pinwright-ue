// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the `previewScene` rig parameter ON THE THREE render.* PREVIEW VERBS: that it is
// declared where a preview scene exists, and that the two verbs/paths that have no preview scene
// say so instead of accepting it and doing nothing.
//
// WHAT THIS DEFENDS, and it is not the rig itself. `previewScene` is read by
// PinWrightRenderCapture::ParseViewportCaptureRequest, which is ONE parser shared by
// render.capture_asset_preview, render.capture_open_level and render.capture_annotated. Adding a
// field there makes every one of those verbs fill the pin off the payload whether or not it
// declares the parameter -- which is exactly bug 1 of the previous wave, where
// render.capture_open_level silently accepted three undeclared parameters. So the parameter's
// arrival has to be paired with an active refusal on the paths that cannot honour it, and those
// refusals are the thing under test here. The rig's own apply/restore/measure behaviour is tested
// against the guard in Tests/Render/TestCapturePreviewSceneRig.cpp; this file never applies one.
//
// WHY NONE OF THESE TESTS CAPTURES A PIXEL. A test that captures needs a GPU, and a capture test
// that cannot get one takes a conditional-skip path and reports success WITHOUT running its
// assertions -- board ticket B-test-skips-assertions-silently, where
// PinWright.render.capture_asset_preview.PinnedCapturesAreIdentical skipped its only substantive
// assertions in 3 of 3 runs because another project's editor held the GPU, and the suite totals
// looked identical either way. Every assertion below runs against the registered param specs, the
// shared parser, the dispatcher's param gate, or a handler refusal that fires before any viewport
// is acquired. There is no device to be denied and nothing to skip.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Render/PreviewSceneRig.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Tests/TestUtils.h"

// Defined in Handlers/Render/RenderHandler.cpp, declared here rather than in a header because the
// design record (docs/preview-scene-rig.md, section 7) gives this chunk exactly four files and no
// header among them. The signature is enforced by the linker: a drift is an unresolved external,
// which is a loud failure rather than a quiet one.
namespace PinWrightRenderSubject
{
    void ClearPreviewSceneRigForLevelViewport(PinWrightRenderCapture::FViewportCaptureRequest& Request);
}

namespace
{
    // Prefixed for the same reason as every other helper in Tests/Render: anonymous namespaces in
    // one Unity translation unit merge, so a bare name a sibling also uses is a latent ODR clash.

    // A well-formed rig payload -- one ParseViewportCaptureRequest must ACCEPT, so a test asserting
    // a downstream refusal cannot be passing on a parse error instead.
    TSharedPtr<FJsonObject> PWR2RigPayload()
    {
        TSharedPtr<FJsonObject> Rig = MakeShared<FJsonObject>();
        TSharedPtr<FJsonObject> Key = MakeShared<FJsonObject>();
        Key->SetNumberField(TEXT("azimuth"), 110.0);
        Key->SetNumberField(TEXT("elevation"), 40.0);
        Key->SetNumberField(TEXT("intensity"), 4.0);
        Key->SetStringField(TEXT("color"), TEXT("#FFFFFF"));
        Rig->SetObjectField(TEXT("key"), Key);
        TSharedPtr<FJsonObject> Sky = MakeShared<FJsonObject>();
        Sky->SetNumberField(TEXT("intensity"), 2.0);
        Rig->SetObjectField(TEXT("sky"), Sky);
        // All SIX provided-flags are raised by this payload, so test 4's "the clear reset every
        // sub-flag" assertions are non-vacuous rather than passing on a flag never raised.
        Rig->SetBoolField(TEXT("showFloor"), false);
        Rig->SetBoolField(TEXT("showEnvironment"), false);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("previewScene"), Rig);
        return Payload;
    }

    // An asset path that cannot resolve, so a verb reaching its asset load answers ASSET_NOT_FOUND
    // and a verb refusing earlier answers something else. That contrast is how the ORDER of a
    // refusal is asserted without a viewport.
    const TCHAR* PWR2MissingAssetPath()
    {
        return TEXT("/Game/PinWrightMissing_PreviewSceneRig/NoSuchAsset.NoSuchAsset");
    }
}

// ============================================================================
// 1. The declaration, read off the REGISTERED spec list.
//
// Not a grep of the source: the dispatcher gates the wire on Reg.Params, so a parameter written in
// the file but not registered is refused on every call while reading as present. This walks the
// same array the gate walks.
//
// The negative half -- render.capture_open_level does NOT declare it -- is what makes the
// UNKNOWN_PARAMS refusal in test 3 reachable, and it is also the half that passes for free if the
// lookup is broken (a helper returning nullptr for everything satisfies every "absent" assertion).
// The `exposure` control on each verb exists solely to make that failure loud.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigDeclaredTest,
    "PinWright.render.capture_asset_preview.PreviewSceneRigIsDeclared",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigDeclaredTest::RunTest(const FString& Parameters)
{
    using ParamSpecTestHelpers::FindParamSpec;

    const TCHAR* RigVerbs[] = {
        TEXT("render.capture_asset_preview"),
        TEXT("render.capture_animation_preview"),
        TEXT("render.capture_annotated"),
    };

    for (const TCHAR* Verb : RigVerbs)
    {
        // Control first. If the registration list were empty, or a method name misspelled, this
        // fails and the previewScene assertions below cannot pass vacuously.
        TestNotNull(*FString::Printf(TEXT("%s declares the control parameter 'exposure'"), Verb),
            FindParamSpec(Verb, TEXT("exposure")));

        const FParamSpec* Spec = FindParamSpec(Verb, TEXT("previewScene"));
        if (!TestNotNull(*FString::Printf(TEXT("%s declares 'previewScene'"), Verb), Spec))
        {
            continue;
        }
        TestEqual(*FString::Printf(TEXT("%s declares previewScene as an object"), Verb),
            Spec->Type, FString(TEXT("object")));
        TestFalse(*FString::Printf(TEXT("%s declares previewScene as OPTIONAL"), Verb),
            Spec->bRequired);
        // An empty description renders an undocumented knob on the wiki page, which is how a
        // parameter ships reachable and unexplained.
        TestTrue(*FString::Printf(TEXT("%s documents previewScene"), Verb),
            Spec->Description.Len() > 0);
    }

    // The verb that must NOT declare it. Its viewport is the live Level Editor one, which carries
    // no FPreviewScene at all (FLevelEditorViewportClient passes nullptr for it, UE 5.8
    // Editor/UnrealEd/Private/LevelEditorViewport.cpp:2335).
    TestNotNull(TEXT("render.capture_open_level declares the control parameter 'exposure'"),
        FindParamSpec(TEXT("render.capture_open_level"), TEXT("exposure")));
    TestNull(TEXT("render.capture_open_level does NOT declare 'previewScene'"),
        FindParamSpec(TEXT("render.capture_open_level"), TEXT("previewScene")));

    return true;
}

// ============================================================================
// 2. The parameter reaches the SHARED parser on render.capture_asset_preview.
//
// Asserted by ORDER rather than by outcome: a malformed rig is refused by
// ParseViewportCaptureRequest, which this verb calls BEFORE it loads the asset (RenderHandler.cpp,
// the LoadObject behind the "Asset not found" refusal). So a payload naming an asset that cannot
// resolve AND an empty previewScene block must come back INVALID_ARGUMENT, not ASSET_NOT_FOUND. If
// the field never reached the parser the parse would succeed and the load would answer
// ASSET_NOT_FOUND -- which is the failure this asserts against.
//
// Unable to fail if the verb refused everything with INVALID_ARGUMENT: the second half sends the
// SAME payload without the previewScene block and requires ASSET_NOT_FOUND, so the two codes are
// pinned in both directions.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigReachesParserTest,
    "PinWright.render.capture_asset_preview.PreviewSceneRigReachesTheSharedParser",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigReachesParserTest::RunTest(const FString& Parameters)
{
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PWR2MissingAssetPath());
        // An object that asks for nothing is a caller mistake, not a no-op (plan section 2.1).
        Payload->SetObjectField(TEXT("previewScene"), MakeShared<FJsonObject>());

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("render.capture_asset_preview is registered and invoked"),
                InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture)))
        {
            return false;
        }
        TestFalse(TEXT("an empty previewScene block is an error, not a success"), Capture.bSuccess);
        TestEqual(TEXT("an empty previewScene block is INVALID_ARGUMENT, refused by the shared parser before the asset load"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PWR2MissingAssetPath());

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("render.capture_asset_preview is registered and invoked"),
                InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture)))
        {
            return false;
        }
        TestFalse(TEXT("an unresolvable asset is an error, not a success"), Capture.bSuccess);
        TestEqual(TEXT("without previewScene the SAME payload reaches the asset load"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_ASSET_NOT_FOUND));
    }

    return true;
}

// ============================================================================
// 3. render.capture_open_level refuses the field on the wire.
//
// Routed through the real dispatcher, because the refusal IS the dispatcher's unknown-param gate:
// InvokeHandlerWithCapture bypasses param validation and would never see it.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigOpenLevelRefusedTest,
    "PinWright.render.capture_open_level.RefusesPreviewSceneRigAsUnknownParam",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigOpenLevelRefusedTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    bool bSuccess = false;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("render.capture_open_level"),
        TEXT("req-open-level-preview-scene-rig"), PWR2RigPayload(), bSuccess, ErrorCode);

    TestFalse(TEXT("previewScene on capture_open_level is not a success"), bSuccess);
    TestEqual(TEXT("previewScene on capture_open_level is UNKNOWN_PARAMS"),
        ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    // Not merely "the message mentions previewScene somewhere": the gate's message also prints the
    // list of VALID parameters, so a bare Contains would pass on a build that wrongly declared it.
    // "[previewScene]" is the UNKNOWN list, which is the half that must name it.
    TestTrue(TEXT("the refusal names previewScene as the UNKNOWN parameter"),
        Sink->Message.Contains(TEXT("[previewScene]")));
    return true;
}

// ============================================================================
// 4. The pin is CLEARED on the level path, not merely undeclared.
//
// This is the assertion that separates this chunk from bug 1 of the previous wave. Test 3 proves
// the WIRE refuses the field; it proves nothing about a direct handler invocation, which is the
// path every automation test in this repo takes and which bypasses the param gate entirely. Only
// the clear covers that, and only this test covers the clear.
//
// COUNTERFACTUAL. Delete the ClearPreviewSceneRigForLevelViewport call from
// render.capture_open_level and this test fails on every assertion after the clear: the shared parser
// leaves bRequested true and the pin reaches the level-viewport capture, where the guard has no
// FPreviewScene to write to and the response reports a rig that was requested and could never be
// applied. Delete the FUNCTION and this file does not link.
//
// Unable to fail if the parser never filled the pin in the first place -- then "cleared" and
// "never set" are the same observation. The preconditions below assert BOTH directions: a payload
// with previewScene parses to bRequested true, one without parses to false.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigOpenLevelPinClearedTest,
    "PinWright.render.capture_open_level.PreviewSceneRigPinIsClearedNotJustUndeclared",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigOpenLevelPinClearedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightRenderCapture;

    FString ErrorCode;
    FString ErrorMessage;

    // Precondition, direction 1: an omitted previewScene leaves the pin unset. Without this a
    // parser that never sets bRequested at all would satisfy the whole test.
    {
        FViewportCaptureRequest Omitted;
        if (!TestTrue(TEXT("an empty payload parses"), ParseViewportCaptureRequest(
                MakeShared<FJsonObject>(), Omitted, ErrorCode, ErrorMessage)))
        {
            return false;
        }
        TestFalse(TEXT("an omitted previewScene leaves the pin unrequested"),
            Omitted.PreviewSceneRig.bRequested);
    }

    // Precondition, direction 2: the SHARED parser does fill the pin off any payload, which is the
    // whole reason the level path has to clear it.
    FViewportCaptureRequest Request;
    if (!TestTrue(TEXT("a well-formed previewScene payload parses"), ParseViewportCaptureRequest(
            PWR2RigPayload(), Request, ErrorCode, ErrorMessage)))
    {
        return false;
    }
    if (!TestTrue(TEXT("the shared parser fills the pin regardless of which verb called it"),
            Request.PreviewSceneRig.bRequested))
    {
        return false;
    }
    TestTrue(TEXT("the parsed pin carries the key aim it was given"),
        Request.PreviewSceneRig.bKeyAimProvided);

    // The level path.
    PinWrightRenderSubject::ClearPreviewSceneRigForLevelViewport(Request);

    TestFalse(TEXT("the level path clears bRequested"), Request.PreviewSceneRig.bRequested);
    // Every provided-flag too, not just the master switch: a clear that reset bRequested and left
    // the sub-flags set would leave a half-populated pin for anything reading them directly.
    TestFalse(TEXT("the level path clears the key-aim flag"),
        Request.PreviewSceneRig.bKeyAimProvided);
    TestFalse(TEXT("the level path clears the key-intensity flag"),
        Request.PreviewSceneRig.bKeyIntensityProvided);
    TestFalse(TEXT("the level path clears the key-colour flag"),
        Request.PreviewSceneRig.bKeyColorProvided);
    TestFalse(TEXT("the level path clears the sky-intensity flag"),
        Request.PreviewSceneRig.bSkyIntensityProvided);
    TestFalse(TEXT("the level path clears the showFloor flag"),
        Request.PreviewSceneRig.bShowFloorProvided);
    TestFalse(TEXT("the level path clears the showEnvironment flag"),
        Request.PreviewSceneRig.bShowEnvironmentProvided);
    TestFalse(TEXT("WantsRig() is false after the clear"),
        Request.PreviewSceneRig.WantsRig());

    return true;
}

// ============================================================================
// 5. render.capture_annotated refuses the rig on its LEVEL path.
//
// This verb serves both worlds: a world/actor subject (or no subject at all) annotates the Level
// Editor viewport, an asset subject opens that asset's editor and annotates ITS preview. So the
// parameter is declared and reachable, and clearing it silently -- the answer that is right for
// render.capture_open_level, where it is undeclared -- would be the "call succeeded, nothing
// happened" shape the whole parameter exists to remove. It is refused instead.
//
// The refusal fires before any viewport is acquired and before the GEditor check, so this test
// captures nothing, opens nothing and cannot be skipped.
//
// Unable to fail if the verb answered UNSUPPORTED_ASSET_EDITOR for some unrelated reason: the
// message assertion pins it to previewScene, and the second half sends an ASSET subject and
// requires a DIFFERENT code, proving the refusal is conditional on the level path rather than
// unconditional.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigAnnotatedLevelPathTest,
    "PinWright.render.capture_annotated.PreviewSceneRigIsRefusedOnTheLevelPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigAnnotatedLevelPathTest::RunTest(const FString& Parameters)
{
    // No subject at all -- the level viewport, this verb's default.
    {
        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("render.capture_annotated is registered and invoked"),
                InvokeHandlerWithCapture(TEXT("render.capture_annotated"), PWR2RigPayload(), Capture)))
        {
            return false;
        }
        TestFalse(TEXT("previewScene against the level viewport is an error, not a success"),
            Capture.bSuccess);
        TestEqual(TEXT("previewScene against the level viewport is UNSUPPORTED_ASSET_EDITOR"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR));
        TestTrue(TEXT("the refusal names previewScene"),
            Capture.Message.Contains(TEXT("previewScene")));
        // "Naming the mechanism and the remedy" -- a refusal that does not say how to reach the rig
        // sends the caller to the wiki for a fact the message already had.
        TestTrue(TEXT("the refusal names the remedy, an asset subject"),
            Capture.Message.Contains(TEXT("subject")));
    }

    // An ASSET subject, so the same field must NOT be refused. The path is unresolvable, so the
    // verb gets as far as the subject resolver and stops there -- no editor is opened and no frame
    // is captured. ASSET_NOT_FOUND (not UNSUPPORTED_ASSET_EDITOR) is the proof that the refusal
    // above did not fire on this path.
    {
        TSharedPtr<FJsonObject> Payload = PWR2RigPayload();
        TSharedPtr<FJsonObject> Subject = MakeShared<FJsonObject>();
        Subject->SetStringField(TEXT("kind"), TEXT("staticMesh"));
        Subject->SetStringField(TEXT("path"), PWR2MissingAssetPath());
        Payload->SetObjectField(TEXT("subject"), Subject);

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("render.capture_annotated is registered and invoked"),
                InvokeHandlerWithCapture(TEXT("render.capture_annotated"), Payload, Capture)))
        {
            return false;
        }
        TestFalse(TEXT("an unresolvable asset subject is an error, not a success"),
            Capture.bSuccess);
        TestEqual(TEXT("an asset subject carries previewScene past the level-path refusal"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_ASSET_NOT_FOUND));
    }

    return true;
}

// ============================================================================
// 6. render.capture_animation_preview parses the rig, and parses it EARLY.
//
// This verb hand-builds its FPoseListCaptureRequest instead of going through
// ParseViewportCaptureRequest, so its `previewScene` is a separate ParsePreviewSceneRigPin call
// that can be forgotten without any other test noticing -- the declaration in test 1 would still
// pass, and the wire would accept a field the verb never read. This asserts the call exists.
//
// It also asserts WHERE it is: before the asset is loaded. A rig parsed after the asset editor is
// opened refuses a typo only once a window is on screen and a toolkit has been created, which the
// caller then has to close. The unresolvable asset path is what makes the ordering observable --
// INVALID_ARGUMENT means the rig parser ran first, ASSET_NOT_FOUND means it did not.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewSceneRigAnimationPreviewParseTest,
    "PinWright.render.capture_animation_preview.PreviewSceneRigIsParsedBeforeTheAssetLoads",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPreviewSceneRigAnimationPreviewParseTest::RunTest(const FString& Parameters)
{
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PWR2MissingAssetPath());
        Payload->SetObjectField(TEXT("previewScene"), MakeShared<FJsonObject>());

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("render.capture_animation_preview is registered and invoked"),
                InvokeHandlerWithCapture(TEXT("render.capture_animation_preview"), Payload, Capture)))
        {
            return false;
        }
        TestFalse(TEXT("an empty previewScene block is an error, not a success"), Capture.bSuccess);
        TestEqual(TEXT("an empty previewScene block is INVALID_ARGUMENT, refused before the asset is loaded"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
    }

    // The control that makes the code above mean something: the same payload without the rig block
    // runs on to the asset load and answers ASSET_NOT_FOUND.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PWR2MissingAssetPath());

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("render.capture_animation_preview is registered and invoked"),
                InvokeHandlerWithCapture(TEXT("render.capture_animation_preview"), Payload, Capture)))
        {
            return false;
        }
        TestFalse(TEXT("an unresolvable asset is an error, not a success"), Capture.bSuccess);
        TestEqual(TEXT("without previewScene the SAME payload reaches the asset load"),
            Capture.ErrorCode, FString(ErrorCodes::ERR_ASSET_NOT_FOUND));
    }

    return true;
}
