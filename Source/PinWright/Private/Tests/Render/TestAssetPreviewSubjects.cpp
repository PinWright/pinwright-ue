// Copyright (c) 2026 Alexander Penkin. MIT License.

// render.capture_asset_preview and render.capture_open_level after the capture-subject
// convergence: the subject argument, the shot-plan arguments, and the three parameters the verb
// used to read without declaring.
//
// WHY A NEW FILE. Tests/EditorOps/TestRenderHandlers.cpp, Tests/Render/TestCaptureExposurePin.cpp,
// TestCaptureBlankCriterion.cpp, TestCaptureCameraAim.cpp, TestCaptureViewModeOverride.cpp and
// TestAnimationCaptureHandlers.cpp are the regression floor for these two verbs and must pass
// UNMODIFIED, so a new assertion goes in a new file rather than being appended to one of them.
//
// WHAT MOST OF THESE ASSERT, and why that is deliberate. Every argument-shape case below is
// reached WITHOUT opening an asset editor and without drawing a frame: the refusals it pins all
// happen above the resolver call. That matters because a test that needs a live preview viewport
// is a test that takes a conditional-skip path on a busy machine and reports success without
// running its assertions -- board ticket B-test-skips-assertions-silently, which fired on exactly
// this cluster of verbs. The two live tests at the bottom are guarded and say out loud, as a
// WARNING carrying a skip marker, when they measured nothing.
//
// TWO CALL PATHS, AND THE DIFFERENCE IS LOAD-BEARING HERE. InvokeHandlerWithCapture calls the
// handler body directly and never runs the dispatcher's required-param / unknown-param gates;
// DispatcherTestHelpers::Dispatch runs the whole pipeline. The three undeclared parameters this
// chunk resolves are only visible through the second path -- on the wire they were REFUSED with
// UNKNOWN_PARAMS before the handler ever ran, which is the opposite of the "accepts three
// undeclared parameters" the plan recorded.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"

#include "Dispatch/RpcDispatcher.h"
#include "Handlers/ParamSpec.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

namespace
{
    // Distinctly prefixed, like the sibling capture suites, so anonymous-namespace symbols cannot
    // ODR-collide when Unity merges these translation units.
    const TCHAR* PWAssetSubjEngineCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");
    const TCHAR* PWAssetSubjEngineMaterialPath =
        TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial");
    const TCHAR* PWAssetSubjSkeletalCubePath = TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube");
    // The stock template system, used unmodified because it ships with baked scripts and can
    // actually render something. The same fixture the Niagara subject suite uses.
    const TCHAR* PWAssetSubjNiagaraSystemPath =
        TEXT("/Niagara/DefaultAssets/Templates/Systems/SimpleExplosion.SimpleExplosion");

    // Failure codes that mean "this host could not draw a preview frame", as opposed to "the verb
    // is broken". An empty or unlisted code still fails the assertion, so "the capture silently
    // did nothing" cannot pass as a skip.
    bool PWAssetSubjIsTypedCaptureFailure(const FString& Code)
    {
        return Code == TEXT("OPEN_FAILED") ||
            Code == TEXT("PREVIEW_VIEWPORT_NOT_FOUND") ||
            Code == TEXT("UNSUPPORTED_ASSET_EDITOR") ||
            Code == TEXT("CAPTURE_FAILED") ||
            Code == TEXT("ENCODE_FAILED") ||
            Code == TEXT("SAVE_FAILED") ||
            Code == TEXT("BLANK_CAPTURE") ||
            Code == TEXT("EDITOR_NOT_AVAILABLE") ||
            Code == TEXT("SUBSYSTEM_MISSING") ||
            // Legitimate only when the stock asset has a measured compile error or a rendered
            // unassigned slot; incomplete shader state now remains a successful warned capture.
            Code == TEXT("MATERIAL_FALLBACK");
    }

    // Close whatever this test opened. An asset editor still open when the editor exits faults in
    // ~FStaticMeshEditor during shutdown (UE 5.8 StaticMeshEditor.cpp:271), so a test that leaves
    // one behind can kill a later suite rather than its own.
    void PWAssetSubjCloseEditorFor(const TCHAR* AssetPath)
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

    // Delete every PNG a multi-shot response names, plus the top-level one.
    void PWAssetSubjDeleteCaptureFiles(const FTestResponseCapture& Capture)
    {
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return;
        }
        FString Path;
        if (Capture.Result->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, false, true, true);
        }
        const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("shots"), Shots) && Shots)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Shots)
            {
                const TSharedPtr<FJsonObject>* Shot = nullptr;
                FString ShotPath;
                if (Value.IsValid() && Value->TryGetObject(Shot) && Shot && (*Shot).IsValid() &&
                    (*Shot)->TryGetStringField(TEXT("path"), ShotPath) && !ShotPath.IsEmpty())
                {
                    IFileManager::Get().Delete(*ShotPath, false, true, true);
                }
            }
        }
    }

    FString PWAssetSubjMissingPath()
    {
        return FString::Printf(TEXT("/Game/MCP_AssetSubjAbsent/Mesh_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }
}

// ============================================================================
// Bug 1: the three parameters the shared parser read and the schema did not declare.
//
// The plan recorded this as "render.capture_asset_preview ACCEPTS three undeclared parameters".
// It did not: the dispatcher rejects any field absent from RPC_PARAMS
// (RpcDispatcher.cpp:125-160), so every wire call carrying one was refused with UNKNOWN_PARAMS
// and the handler's own `allowBlank` branch was dead on the wire. Only a direct handler
// invocation -- i.e. an automation test -- ever reached the parser.
//
// Resolved deliberately, one decision per parameter, and asserted in BOTH directions here:
//   allowBlank         DECLARED. The handler already carries a documented refusal that names it
//                      by hand ("rejectBlank:true and allowBlank:true contradict each other"), and
//                      a message can only be reached by a caller if the parameter is declared.
//   viewDistanceScale  REFUSED. It forces the GLOBAL r.ViewDistanceScale so distant LEVEL content
//                      is not culled; a preview scene has none to un-cull.
//   hideEditorSprites  REFUSED. EngineShowFlags.BillboardSprites gates exactly UArrowComponent,
//                      UBillboardComponent and UMaterialBillboardComponent, and the preview scenes
//                      this verb captures contain none of the three (see the evidence block in
//                      PreviewViewportCaptureUtils.h).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewUndeclaredParamsResolvedTest,
    "PinWright.render.capture_asset_preview.UndeclaredParamsAreResolved",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewUndeclaredParamsResolvedTest::RunTest(const FString& Parameters)
{
    const TCHAR* Method = TEXT("render.capture_asset_preview");

    // --- the schema half ---
    TestNotNull(TEXT("allowBlank is declared, so the rejectBlank contradiction refusal is reachable"),
        GetRegisteredParamSpec(Method, TEXT("allowBlank")));
    TestNull(TEXT("viewDistanceScale is NOT declared: it forces a global cvar for level content a "
                  "preview scene does not have"),
        GetRegisteredParamSpec(Method, TEXT("viewDistanceScale")));
    TestNull(TEXT("hideEditorSprites is NOT declared: no preview scene this verb captures holds an "
                  "Arrow/Billboard/MaterialBillboard component for the flag to act on"),
        GetRegisteredParamSpec(Method, TEXT("hideEditorSprites")));
    // The new surface, asserted here so a silent drop of any of them fails.
    for (const TCHAR* Declared : {TEXT("subject"), TEXT("count"), TEXT("views"),
                                  TEXT("distribution"), TEXT("seed"), TEXT("elevation")})
    {
        TestNotNull(*FString::Printf(TEXT("%s declares '%s'"), Method, Declared),
            GetRegisteredParamSpec(Method, Declared));
    }

    // --- the behaviour half, through the real dispatcher ---
    // Without this the schema half proves only what someone typed in RPC_PARAMS. Both payloads
    // below are chosen so nothing is ever opened or drawn: the refused one stops at the param
    // gate, the accepted one stops at ASSET_NOT_FOUND.
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    for (const TCHAR* Refused : {TEXT("viewDistanceScale"), TEXT("hideEditorSprites")})
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PWAssetSubjEngineCubePath);
        Payload->SetNumberField(Refused, 1.0);

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method, TEXT("undeclared-refused"),
            Payload, bSuccess, ErrorCode);
        TestFalse(*FString::Printf(TEXT("'%s' is refused, not quietly parsed"), Refused), bSuccess);
        TestEqual(*FString::Printf(TEXT("'%s' is refused with a TYPED code"), Refused),
            ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
        TestTrue(*FString::Printf(TEXT("the refusal names '%s' so the caller can self-correct"), Refused),
            Sink->Message.Contains(Refused));
    }

    {
        // allowBlank must now survive the gate. A path that cannot load takes the handler to
        // ASSET_NOT_FOUND, which proves the gate passed without opening anything.
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PWAssetSubjMissingPath());
        Payload->SetBoolField(TEXT("allowBlank"), true);

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method, TEXT("undeclared-accepted"),
            Payload, bSuccess, ErrorCode);
        TestFalse(TEXT("a missing asset is still an error"), bSuccess);
        TestEqual(TEXT("allowBlank passes the param gate, so the handler reaches its own lookup"),
            ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    }
    return true;
}

// ============================================================================
// The `subject` argument on render.capture_asset_preview.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewNoTargetIsInvalidArgumentTest,
    "PinWright.render.capture_asset_preview.NoTargetIsInvalidArgumentOnTheWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewNoTargetIsInvalidArgumentTest::RunTest(const FString& Parameters)
{
    // `assetPath` moved from RPC_PARAM_REQ to RPC_PARAM_OPT because `subject` can now stand in for
    // it, and the dispatcher's required-param gate can only speak for ONE slot. The refusal moved
    // into the handler body, which also CLOSES a drift: the direct-invocation test
    // (…MissingAssetPath) has always asserted INVALID_ARGUMENT while the wire answered
    // MISSING_REQUIRED_PARAM. Both now say INVALID_ARGUMENT.
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    bool bSuccess = false;
    FString ErrorCode;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("render.capture_asset_preview"),
        TEXT("no-target"), MakeShared<FJsonObject>(), bSuccess, ErrorCode);

    TestFalse(TEXT("naming no asset at all is an error"), bSuccess);
    TestEqual(TEXT("and it is the handler's INVALID_ARGUMENT, the same code the direct-invocation "
                   "test has always asserted"),
        ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the refusal names both spellings, so a caller who tried one learns the other"),
        Sink->Message.Contains(TEXT("assetPath")) && Sink->Message.Contains(TEXT("subject")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewSubjectPathReplacesAssetPathTest,
    "PinWright.render.capture_asset_preview.SubjectPathReplacesAssetPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewSubjectPathReplacesAssetPathTest::RunTest(const FString& Parameters)
{
    // The failure direction is the point. If `subject.path` were ignored the verb would answer
    // INVALID_ARGUMENT ("assetPath required"); reaching ASSET_NOT_FOUND proves the path was read
    // out of the subject object and carried into the asset lookup.
    TSharedPtr<FJsonObject> Subject = MakeShared<FJsonObject>();
    Subject->SetStringField(TEXT("path"), PWAssetSubjMissingPath());
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("subject"), Subject);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_asset_preview handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("a subject naming a missing asset is an error"), Capture.bSuccess);
    TestEqual(TEXT("subject.path is read as the asset path, so the lookup is what fails"),
        Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewLevelSubjectKindsRefusedTest,
    "PinWright.render.capture_asset_preview.LevelSubjectKindsAreRefusedHere",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewLevelSubjectKindsRefusedTest::RunTest(const FString& Parameters)
{
    // Plan decision 6: a capability a verb cannot serve is a TYPED refusal that names where the
    // capability lives, never silence and never a generic argument error. subject:{kind:"actor"}
    // carries no path, so the refusal has to beat the missing-path check to be useful at all.
    for (const TCHAR* Kind : {TEXT("actor"), TEXT("world")})
    {
        TSharedPtr<FJsonObject> Subject = MakeShared<FJsonObject>();
        Subject->SetStringField(TEXT("kind"), Kind);
        Subject->SetStringField(TEXT("name"), TEXT("SomeActorThatNeedNotExist"));
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("subject"), Subject);

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("render.capture_asset_preview handler found"),
                InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture)))
        {
            return false;
        }
        TestFalse(*FString::Printf(TEXT("subject.kind '%s' is an error here"), Kind), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("subject.kind '%s' is UNSUPPORTED_ASSET_EDITOR, not a bare "
                                        "argument error"), Kind),
            Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ASSET_EDITOR")));
        TestTrue(*FString::Printf(TEXT("the '%s' refusal names the verbs that do serve it"), Kind),
            Capture.Message.Contains(TEXT("render.capture_open_level")) &&
            Capture.Message.Contains(TEXT("camera.frame_actor")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewUnservedClassNamesSiblingsTest,
    "PinWright.render.capture_asset_preview.UnservedAssetClassNamesTheSiblingVerbs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewUnservedClassNamesSiblingsTest::RunTest(const FString& Parameters)
{
    // The verb is no longer Static-Mesh-only, so its rejection message had to be rewritten. This
    // pins the half of that message that is load-bearing: the pointer at
    // render.capture_animation_preview, which is the only discovery path between the two capture
    // surfaces and which TestAnimationCaptureHandlers.cpp also asserts from the other side.
    if (!UEditorAssetLibrary::DoesAssetExist(PWAssetSubjEngineMaterialPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("the engine default material is unavailable, so the unserved-class refusal was "
                 "not exercised"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), PWAssetSubjEngineMaterialPath);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_asset_preview handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("a Material is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("an unserved asset class is UNSUPPORTED_ASSET_EDITOR"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ASSET_EDITOR")));
    TestTrue(TEXT("the refusal still names render.capture_animation_preview"),
        Capture.Message.Contains(TEXT("render.capture_animation_preview")));
    TestTrue(TEXT("and it lists the kinds this verb DOES serve, so the caller learns the new reach "
                  "rather than the old 'Static Mesh only'"),
        Capture.Message.Contains(TEXT("Skeletal Mesh")) && Capture.Message.Contains(TEXT("Niagara")));
    return true;
}

// ============================================================================
// The shot-plan arguments. Every case here is refused ABOVE the resolver, so none of them opens
// an asset editor or draws a frame.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewShotPlanRefusalsTest,
    "PinWright.render.capture_asset_preview.ShotPlanArgumentsAreRefusedNotRanked",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewShotPlanRefusalsTest::RunTest(const FString& Parameters)
{
    if (!UEditorAssetLibrary::DoesAssetExist(PWAssetSubjEngineCubePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("the engine cube is unavailable, so no shot-plan refusal was exercised"));
        return true;
    }

    struct FCase
    {
        const TCHAR* Name;
        TFunction<void(const TSharedPtr<FJsonObject>&)> Fill;
        const TCHAR* ExpectedCode;
        const TCHAR* MessageContains;
    };

    const TArray<FCase> Cases = {
        // A free camera and a shot set are two different answers to "which poses". Ranking one
        // over the other is how a verb ends up with parameters that contradict each other
        // depending on a mode flag.
        {TEXT("count with an explicit location"),
         [](const TSharedPtr<FJsonObject>& P)
         {
             P->SetNumberField(TEXT("count"), 4);
             TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
             Loc->SetNumberField(TEXT("x"), -300.0);
             P->SetObjectField(TEXT("location"), Loc);
         },
         TEXT("INVALID_ARGUMENT"), TEXT("count/views")},
        {TEXT("views combined with count"),
         [](const TSharedPtr<FJsonObject>& P)
         {
             P->SetStringField(TEXT("views"), TEXT("sides"));
             P->SetNumberField(TEXT("count"), 3);
         },
         TEXT("INVALID_ARGUMENT"), TEXT("one shot plan")},
        {TEXT("an unknown views plan"),
         [](const TSharedPtr<FJsonObject>& P) { P->SetStringField(TEXT("views"), TEXT("corners")); },
         TEXT("INVALID_ARGUMENT"), TEXT("sides")},
        {TEXT("an unknown distribution"),
         [](const TSharedPtr<FJsonObject>& P)
         {
             P->SetNumberField(TEXT("count"), 2);
             P->SetStringField(TEXT("distribution"), TEXT("random"));
         },
         TEXT("INVALID_ARGUMENT"), TEXT("sphere")},
        {TEXT("count below one"),
         [](const TSharedPtr<FJsonObject>& P) { P->SetNumberField(TEXT("count"), 0); },
         TEXT("INVALID_ARGUMENT"), TEXT("at least 1")},
        // Refused rather than truncated, matching camera.orbit_shots. The primitive would happily
        // truncate and report it, but a caller who asked for 40 shots wants to know before the
        // editor spends a minute on 24 of them.
        {TEXT("more shots than the ceiling"),
         [](const TSharedPtr<FJsonObject>& P) { P->SetNumberField(TEXT("count"), 25); },
         TEXT("TOO_MANY_SHOTS"), TEXT("maximum")},
    };

    for (const FCase& Case : Cases)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PWAssetSubjEngineCubePath);
        Case.Fill(Payload);

        FTestResponseCapture Capture;
        if (!TestTrue(TEXT("render.capture_asset_preview handler found"),
                InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture)))
        {
            return false;
        }
        TestFalse(*FString::Printf(TEXT("%s is refused"), Case.Name), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s is refused with the typed code"), Case.Name),
            Capture.ErrorCode, FString(Case.ExpectedCode));
        TestTrue(*FString::Printf(TEXT("the '%s' refusal says what to do instead (looking for '%s' "
                                       "in: %s)"), Case.Name, Case.MessageContains, *Capture.Message),
            Capture.Message.Contains(Case.MessageContains));
    }
    return true;
}

// ============================================================================
// render.capture_open_level: the subject is a FRAMING VERDICT, and an asset kind is refused.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOpenLevelAssetSubjectRefusedTest,
    "PinWright.render.capture_open_level.AssetSubjectKindsAreRefusedHere",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FOpenLevelAssetSubjectRefusedTest::RunTest(const FString& Parameters)
{
    // This verb is domain-named: its registration string says it captures the OPEN LEVEL, so
    // serving an asset subject would make the name lie (plan §4.3). The refusal has to happen
    // before the capture, and it has to name render.capture_asset_preview.
    TestNotNull(TEXT("render.capture_open_level declares a 'subject' param"),
        GetRegisteredParamSpec(TEXT("render.capture_open_level"), TEXT("subject")));

    TSharedPtr<FJsonObject> Subject = MakeShared<FJsonObject>();
    Subject->SetStringField(TEXT("kind"), TEXT("staticMesh"));
    Subject->SetStringField(TEXT("path"), PWAssetSubjEngineCubePath);
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("subject"), Subject);
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 256);
    Payload->SetBoolField(TEXT("allowBlank"), true);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_open_level handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_open_level"), Payload, Capture)))
    {
        return false;
    }
    ON_SCOPE_EXIT { PWAssetSubjDeleteCaptureFiles(Capture); };

    TestFalse(TEXT("an asset subject is an error on the level verb"), Capture.bSuccess);
    if (Capture.ErrorCode == TEXT("NO_ACTIVE_LEVEL_VIEWPORT") ||
        Capture.ErrorCode == TEXT("NO_EDITOR_WORLD") ||
        Capture.ErrorCode == TEXT("EDITOR_NOT_AVAILABLE") ||
        Capture.ErrorCode == TEXT("VIEWPORT_WORLD_MISMATCH"))
    {
        // The viewport gate runs first on this verb and is not something this test can arrange.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            FString::Printf(TEXT("the viewport gate answered %s before the subject was parsed, so "
                                 "the asset-kind refusal was not exercised"), *Capture.ErrorCode));
        return true;
    }
    TestEqual(TEXT("an asset subject is UNSUPPORTED_ASSET_EDITOR"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ASSET_EDITOR")));
    TestTrue(TEXT("the refusal names render.capture_asset_preview"),
        Capture.Message.Contains(TEXT("render.capture_asset_preview")));
    return true;
}

// ============================================================================
// The live paths. Guarded, and a skip is reported as a WARNING carrying the skip marker so a
// green total can never be mistaken for a measurement.
//
// NOTE ON PIXELS. The plan's §8 said asset-preview captures render bimodally under the headless
// commandlet -- black on some runs -- and that no pixel assertion is possible. That doctrine was
// measured off three captures that were all writing to ONE file (a one-second timestamp in the
// auto-generated filename); it was corrected in commit bcc334e8, and the same fixture now measures
// meanLuminance 0.3644 in the commandlet and 0.3644 interactively. So an unlit frame here is
// NOT MEASURED and says so; it is never treated as a pass, and no bimodality is assumed.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewSkeletalMeshSixSidesTest,
    "PinWright.render.capture_asset_preview.SkeletalMeshAssetGetsSixSides",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewSkeletalMeshSixSidesTest::RunTest(const FString& Parameters)
{
    // Two things at once, and both used to be impossible on this verb: a SKELETAL MESH is served
    // at all (it used to be a hard UNSUPPORTED_ASSET_EDITOR), and it is served through the shared
    // pose-list primitive, so `views:"sides"` produces the same six axis-aligned poses
    // camera.orbit_shots produces from the same one table.
    if (!UEditorAssetLibrary::DoesAssetExist(PWAssetSubjSkeletalCubePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("/Engine/EngineMeshes/SkeletalCube is unavailable on this host, so nothing was "
                 "captured and no assertion ran"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), PWAssetSubjSkeletalCubePath);
    Payload->SetStringField(TEXT("views"), TEXT("sides"));
    // Fixed, and the same size the sibling render tests use. A capture size that VARIES within an
    // editor session trips FViewport::GetHitProxy's ProxyMap assertion, which has already cost
    // unsaved level state on this project.
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 256);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_asset_preview handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture)))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        PWAssetSubjDeleteCaptureFiles(Capture);
        PWAssetSubjCloseEditorFor(PWAssetSubjSkeletalCubePath);
    };

    if (!Capture.bSuccess)
    {
        TestTrue(*FString::Printf(TEXT("failure is typed, not a crash: %s"), *Capture.ErrorCode),
            PWAssetSubjIsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
            FString::Printf(TEXT("the six-sides capture returned %s, so the shot-count, "
                                 "captureSource and subject assertions were skipped"),
                *Capture.ErrorCode));
        return true;
    }
    if (!TestTrue(TEXT("success result exists"), Capture.Result.IsValid()))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (!TestTrue(TEXT("a views:'sides' call returns a shots array"),
            Capture.Result->TryGetArrayField(TEXT("shots"), Shots) && Shots))
    {
        return false;
    }
    TestEqual(TEXT("'sides' is six shots"), Shots->Num(), 6);
    TestEqual(TEXT("count agrees with the array"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("count"))), Shots->Num());

    // The six azimuth/elevation pairs, written as literals so a silent reordering of the shared
    // sides table fails here as well as in the table's own test.
    const float ExpectedAzimuth[6] = {0.0f, 180.0f, 90.0f, -90.0f, 0.0f, 0.0f};
    const float ExpectedElevation[6] = {0.0f, 0.0f, 0.0f, 0.0f, 90.0f, -90.0f};
    TSet<FString> ShotPaths;
    for (int32 Index = 0; Index < Shots->Num() && Index < 6; ++Index)
    {
        const TSharedPtr<FJsonObject>* Shot = nullptr;
        if (!(*Shots)[Index].IsValid() || !(*Shots)[Index]->TryGetObject(Shot) || !Shot || !(*Shot).IsValid())
        {
            TestTrue(TEXT("every shot entry is an object"), false);
            continue;
        }
        const TSharedPtr<FJsonObject>* Angle = nullptr;
        if ((*Shot)->TryGetObjectField(TEXT("angle"), Angle) && Angle && (*Angle).IsValid())
        {
            // TOLERANCE 0, PASSED EXPLICITLY. TestEqual's float overload defaults to
            // UE_KINDA_SMALL_NUMBER, so an assertion written to mean "this is the table value"
            // silently becomes "this is nearly the table value" -- and it fails quietly, by
            // weakening, not by erroring. Exactness is both intended and reachable here: every
            // entry in the sides table is an exactly representable float that travels to JSON as a
            // double and back without rounding, so a nudged table value must fail.
            TestEqual(*FString::Printf(TEXT("shot %d azimuth is the sides table value exactly"), Index),
                static_cast<float>((*Angle)->GetNumberField(TEXT("requestedAzimuth"))),
                ExpectedAzimuth[Index], 0.0f);
            TestEqual(*FString::Printf(TEXT("shot %d elevation is the sides table value exactly"), Index),
                static_cast<float>((*Angle)->GetNumberField(TEXT("requestedElevation"))),
                ExpectedElevation[Index], 0.0f);
        }
        FString ShotPath;
        if ((*Shot)->TryGetStringField(TEXT("path"), ShotPath))
        {
            ShotPaths.Add(ShotPath);
        }
    }
    // SIX FILES, NOT ONE. The sibling exposure suite reported green for weeks while comparing a
    // file with itself, because an auto-generated filename carried a one-second timestamp and a
    // capture takes ~60 ms. Assert the fixture before trusting anything measured off it.
    TestEqual(TEXT("the six shots wrote six DIFFERENT files"), ShotPaths.Num(), Shots->Num());

    TestEqual(TEXT("captureSource names the Persona preview, measured by the provider rather than "
                   "hardcoded per class"),
        Capture.Result->GetStringField(TEXT("captureSource")), FString(TEXT("personaPreviewViewport")));

    const TSharedPtr<FJsonObject>* Subject = nullptr;
    if (TestTrue(TEXT("the response carries a subject block"),
            Capture.Result->TryGetObjectField(TEXT("subject"), Subject) && Subject && (*Subject).IsValid()))
    {
        TestEqual(TEXT("subject.kind is the skeletal-mesh kind"),
            (*Subject)->GetStringField(TEXT("kind")), FString(TEXT("skeletalMesh")));
        TestEqual(TEXT("subject.path is the asset that was captured"),
            (*Subject)->GetStringField(TEXT("path")), FString(PWAssetSubjSkeletalCubePath));
    }

    const TSharedPtr<FJsonObject>* PoseSet = nullptr;
    if (TestTrue(TEXT("the response carries the poseSet block from the shared primitive"),
            Capture.Result->TryGetObjectField(TEXT("poseSet"), PoseSet) && PoseSet && (*PoseSet).IsValid()))
    {
        // The cold-first-frame mitigation. This is the verb that OPENS the window and captures in
        // the same call, so the throwaway frame matters more here than anywhere else.
        TestTrue(TEXT("a throwaway warm-up frame was taken"),
            (*PoseSet)->GetBoolField(TEXT("warmupShotTaken")));
        TestTrue(TEXT("and its uniquely named warm-up file was deleted"),
            (*PoseSet)->GetBoolField(TEXT("warmupShotDiscarded")));
        TestEqual(TEXT("no pose was dropped by the bound"),
            static_cast<int32>((*PoseSet)->GetNumberField(TEXT("posesTruncated"))), 0);
    }

    // PINWRIGHT_INFO_IS_NOT_A_SKIP: every assertion above already ran; this records the
    // measured shot count and luminance for the next reader.
    AddInfo(FString::Printf(TEXT("Six-sides skeletal capture: %d shots, captureSource '%s', "
                                 "top-level meanLuminance %.4f, blank %s."),
        Shots->Num(), *Capture.Result->GetStringField(TEXT("captureSource")),
        Capture.Result->HasField(TEXT("imageStats"))
            ? Capture.Result->GetObjectField(TEXT("imageStats"))->GetNumberField(TEXT("meanLuminance"))
            : -1.0,
        Capture.Result->GetBoolField(TEXT("blank")) ? TEXT("true") : TEXT("false")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetPreviewNiagaraCapturedTest,
    "PinWright.render.capture_asset_preview.NiagaraAssetIsCaptured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetPreviewNiagaraCapturedTest::RunTest(const FString& Parameters)
{
    // The cheapest win in the convergence and the one that used to look impossible.
    // SNiagaraSystemViewport really does derive from SEditorViewport and publishes
    // GetViewportClient / GetSceneViewport through UnrealEd's public header; what hid it was the
    // widget-walk's `EndsWith("EditorViewport")` suffix test, which "SNiagaraSystemViewport" fails.
    // The exact-name allow-list in the shared walk is what reaches it -- and is also what stops the
    // suffix test being an unchecked downcast on anything else.
    if (!UEditorAssetLibrary::DoesAssetExist(PWAssetSubjNiagaraSystemPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("the stock SimpleExplosion template system is unavailable (Niagara plugin content "
                 "not mounted), so nothing was captured and no assertion ran"));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), PWAssetSubjNiagaraSystemPath);
    Payload->SetNumberField(TEXT("width"), 256);
    Payload->SetNumberField(TEXT("height"), 256);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("render.capture_asset_preview handler found"),
            InvokeHandlerWithCapture(TEXT("render.capture_asset_preview"), Payload, Capture)))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        PWAssetSubjDeleteCaptureFiles(Capture);
        PWAssetSubjCloseEditorFor(PWAssetSubjNiagaraSystemPath);
    };

    if (!Capture.bSuccess)
    {
        TestTrue(*FString::Printf(TEXT("failure is typed, not a crash: %s"), *Capture.ErrorCode),
            PWAssetSubjIsTypedCaptureFailure(Capture.ErrorCode));
        PinWrightTestSkip::SkipAssertions(*this, TEXT("capture-unavailable"),
            FString::Printf(TEXT("the Niagara capture returned %s, so the subject-kind and pixel "
                                 "assertions were skipped"), *Capture.ErrorCode));
        return true;
    }
    if (!TestTrue(TEXT("success result exists"), Capture.Result.IsValid()))
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* Subject = nullptr;
    if (TestTrue(TEXT("the response carries a subject block"),
            Capture.Result->TryGetObjectField(TEXT("subject"), Subject) && Subject && (*Subject).IsValid()))
    {
        TestEqual(TEXT("subject.kind is niagara"),
            (*Subject)->GetStringField(TEXT("kind")), FString(TEXT("niagara")));
    }

    // No reproducibility field, in either direction. Niagara determinism is off at all three
    // scopes by default and is void under a variable tick delta, so nothing here may claim a
    // capture repeats (plan §4.2). This asserts the ABSENCE, which is the part a later refactor
    // would silently break by emitting a zeroed field.
    if (Subject && (*Subject).IsValid())
    {
        TestFalse(TEXT("no `reproducible` field is invented for a particle capture"),
            (*Subject)->HasField(TEXT("reproducible")));
    }

    // The pixels, honestly. An unlit frame is NOT MEASURED and says so rather than passing.
    const TSharedPtr<FJsonObject>* ImageStats = nullptr;
    if (!Capture.Result->TryGetObjectField(TEXT("imageStats"), ImageStats) || !ImageStats ||
        !(*ImageStats).IsValid())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-image-stats"),
            TEXT("the capture reported no imageStats, so nothing about the picture was measured"));
        return true;
    }
    const double Mean = (*ImageStats)->GetNumberField(TEXT("meanLuminance"));
    const double LitFraction = (*ImageStats)->GetNumberField(TEXT("litPixelFraction"));
    AddInfo(FString::Printf(
        TEXT("Niagara preview capture: meanLuminance %.4f, litPixelFraction %.4f, blank %s, "
             "captureSource '%s'."),
        Mean, LitFraction, Capture.Result->GetBoolField(TEXT("blank")) ? TEXT("true") : TEXT("false"),
        *Capture.Result->GetStringField(TEXT("captureSource"))));
    // The same 0.03 gate the exposure suite uses: 12x below what this class of preview scene
    // measures when it really renders, and far above a black frame.
    if (Mean <= 0.03)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-not-lit"),
            FString::Printf(TEXT("the Niagara preview rendered at meanLuminance %.4f, which is not "
                                 "a lit frame, so 'a non-blank frame was produced' was NOT measured"),
                Mean));
        return true;
    }
    TestFalse(TEXT("a lit Niagara preview frame is not blank"),
        Capture.Result->GetBoolField(TEXT("blank")));
    return true;
}
