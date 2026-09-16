// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests for what render.capture_animation_preview gained when it moved onto the shared
// capture-subject / pose-list layer: the `subject` argument spelling, the pose-distribution
// controls, and the two response guarantees the convergence work is most likely to break.
//
// A NEW FILE rather than assertions appended to TestAnimationCaptureHandlers.cpp, deliberately.
// That file is the regression FLOOR for both animated-capture verbs and has to keep passing
// unmodified; a chunk that edits the floor it is being measured against is measuring nothing.
//
// What each group pins, and why deleting one stops this file being evidence:
//
//  * The ARGUMENT REJECTIONS are fully deterministic - no world, no RHI, no asset editor. Every
//    one of them is reached before LoadObject, which is itself the assertion: a malformed request
//    must not leave a Persona tab open on its way to being refused. Each asserts the MESSAGE, not
//    just the failure, because a typo in a path fails too and would satisfy a bare "it errored".
//  * The RESPONSE-SHAPE tests need a real Persona preview and are guarded. A guard that turns a
//    non-measurement into a silent pass is the exact defect board ticket
//    B-test-skips-assertions-silently names, so a run that could not measure emits the
//    `PINWRIGHT_ASSERTIONS_SKIPPED:` marker `Content/Python/check_suite_log.py` greps - the run is
//    then not COMPLETED_CLEAN, rather than green-and-empty. A failure with an EMPTY or UNKNOWN
//    error code still fails the assertion, so "the capture silently did nothing" cannot pass.
//
// KNOWN LIMIT, stated rather than hidden: the only skinned fixture that exists in every engine
// install is /Engine/EngineMeshes/SkeletalCube, and it has no animation. A bind-pose burst
// collapses to ONE instant, so these tests cannot separate `count` (shots) from `frameCount`
// (instants) by value - they separate `count` from `viewCount` and assert `frames` is one entry.
// Pinning count-vs-frameCount needs a project fixture with an AnimSequence bound to it.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

// File-unique named namespace: the test module is a Unity build, so an anonymous namespace here
// would collide with the same-shaped helpers in sibling Tests/Render/*.cpp files.
namespace PinWrightAnimPreviewSubjectTests
{
    const TCHAR* const Verb = TEXT("render.capture_animation_preview");

    // The only skinned asset that ships with every engine install. Opens the SkeletalMeshEditor,
    // which is one of the four Persona-family editors this verb accepts.
    const TCHAR* const EngineSkeletalMeshPath = TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube");

    // A non-skinned asset, used only where a SECOND real asset path is needed to make two
    // different-but-valid paths collide.
    const TCHAR* const EngineCubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // A well-formed path that resolves to nothing, for the rejections that must fire before any
    // asset is loaded. Unique per call so a stray asset of that name cannot exist.
    FString AbsentPath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/MCP_AnimPreviewAbsent/%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Error codes a live preview capture can legitimately return on a host that cannot render one.
    // An empty code, or one outside this list, is a DEFECT and fails the calling test - which is
    // what stops "the verb quietly did nothing" from reading as an environment problem.
    bool IsTypedLivePathFailure(const FString& Code)
    {
        static const TCHAR* const Codes[] = {
            TEXT("PREVIEW_VIEWPORT_NOT_FOUND"), TEXT("PREVIEW_NOT_FOUND"),
            TEXT("SKELETAL_MESH_NOT_FOUND"),    TEXT("UNSUPPORTED_ASSET_EDITOR"),
            TEXT("OPEN_FAILED"),                TEXT("SUBSYSTEM_MISSING"),
            TEXT("EDITOR_NOT_AVAILABLE"),       TEXT("CAPTURE_FAILED"),
            TEXT("ENCODE_FAILED"),              TEXT("SAVE_FAILED"),
            TEXT("BLANK_CAPTURE"),              TEXT("EXPOSURE_PIN_FAILED"),
        };
        for (const TCHAR* Known : Codes)
        {
            if (Code == Known)
            {
                return true;
            }
        }
        return false;
    }

    // Delete every PNG a live burst wrote, so the suite does not accumulate one set per run.
    void DeleteShotFiles(const TSharedPtr<FJsonObject>& Result)
    {
        const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("shots"), Shots) || !Shots)
        {
            return;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Shots)
        {
            const TSharedPtr<FJsonObject>* ShotObj = nullptr;
            FString Path;
            if (Value.IsValid() && Value->TryGetObject(ShotObj) && ShotObj && (*ShotObj).IsValid() &&
                (*ShotObj)->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
            {
                IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true,
                    /*Quiet=*/true);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Deterministic argument rejections. No world, no RHI, no asset editor.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimPreviewSubjectParamSurfaceTest,
    "PinWright.render.capture_animation_preview.SubjectParamSurfaceIsDeclared",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimPreviewSubjectParamSurfaceTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAnimPreviewSubjectTests;

    // A parameter the handler READS but does not DECLARE is invisible to every discovery surface
    // and to the wiki generator - the defect this verb's sibling shipped with three times over.
    const TCHAR* const Declared[] = {
        TEXT("subject"), TEXT("viewMode"), TEXT("distribution"), TEXT("seed"),
        // Still declared after the demotion below: `assetPath` is now optional in the SCHEMA
        // (subject.path can carry it) while remaining required in EFFECT, refused by the handler.
        TEXT("assetPath"),
    };
    for (const TCHAR* ParamName : Declared)
    {
        TestNotNull(*FString::Printf(TEXT("%s declares a '%s' param"), Verb, ParamName),
            GetRegisteredParamSpec(Verb, ParamName));
    }

    // The demotion is asserted directly, not inferred: `assetPath` must NOT be marked required, or
    // the dispatcher's own gate refuses a subject-only call before the handler ever sees it.
    if (const FParamSpec* AssetPathSpec = GetRegisteredParamSpec(Verb, TEXT("assetPath")))
    {
        TestFalse(TEXT("assetPath is optional in the schema so subject.path can carry it instead"),
            AssetPathSpec->bRequired);
    }

    // ABSENCE, asserted on purpose and with its reason, so the next agent reading the capability
    // matrix does not "restore" it. `hideEditorSprites` promises to hide light bulbs, audio icons,
    // player start and note icons; a Persona preview world contains none of them. The one thing in
    // that scene the BillboardSprites flag can reach is a user-toggled wind gizmo
    // (AWindDirectionalSource, AnimationEditorPreviewScene.cpp:1121) - so the parameter is not
    // inert here, it is MISDESCRIBED here, which is worse. Undeclared means the dispatcher's
    // unknown-param gate names it in the refusal instead of the handler reading it and doing
    // nothing. render.capture_asset_preview makes the identical call for the same reason.
    TestNull(TEXT("hideEditorSprites is NOT declared on a preview verb"),
        GetRegisteredParamSpec(Verb, TEXT("hideEditorSprites")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimPreviewMissingSubjectTest,
    "PinWright.render.capture_animation_preview.MissingSubjectAndAssetPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimPreviewMissingSubjectTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAnimPreviewSubjectTests;

    // `assetPath` stopped being a REQUIRED param spec so that `subject.path` could carry it. That
    // demotion must not become a loosening: with neither present the verb still refuses, and it
    // refuses with the same typed code the dispatcher's gate used to produce.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("handler is registered and invoked"),
            InvokeHandlerWithCapture(Verb, Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("no subject at all is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("no subject is reported as MISSING_REQUIRED_PARAM"),
        Capture.ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    // Asserted on the message, not only the code: the message is the only place the caller learns
    // that `subject.path` is the second spelling.
    TestTrue(TEXT("the refusal names assetPath"), Capture.Message.Contains(TEXT("assetPath")));
    TestTrue(TEXT("the refusal names subject.path as the alternative"),
        Capture.Message.Contains(TEXT("subject.path")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimPreviewUnknownDistributionTest,
    "PinWright.render.capture_animation_preview.UnknownDistributionIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimPreviewUnknownDistributionTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAnimPreviewSubjectTests;

    // A typo that quietly becomes the default is indistinguishable from the default having been
    // asked for. Reached with an assetPath that resolves to nothing, which also pins that the
    // distribution vocabulary is checked BEFORE the asset is loaded and the editor opened.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), AbsentPath(TEXT("Dist")));
    Payload->SetStringField(TEXT("distribution"), TEXT("spiral"));

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("handler is registered and invoked"),
            InvokeHandlerWithCapture(Verb, Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("an unknown distribution is an error, not a success"), Capture.bSuccess);
    // INVALID_ARGUMENT and not ASSET_NOT_FOUND is the whole point: it proves the refusal happened
    // above the asset load rather than the missing asset masking a silently defaulted argument.
    TestEqual(TEXT("an unknown distribution is reported as INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestTrue(TEXT("the refusal quotes the offending value"),
        Capture.Message.Contains(TEXT("spiral")));
    TestTrue(TEXT("the refusal lists the valid values"),
        Capture.Message.Contains(TEXT("ring")) && Capture.Message.Contains(TEXT("sphere")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimPreviewSubjectBesideAssetPathTest,
    "PinWright.render.capture_animation_preview.SubjectBesideAssetPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimPreviewSubjectBesideAssetPathTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAnimPreviewSubjectTests;

    // A `subject` object does NOT absorb the legacy key beside it. Presented together they are
    // refused naming BOTH KEYS - not ranked, and not refused only when the values disagree.
    // Ranking is how a caller ends up reviewing an asset they did not name and never learns it,
    // and a disagreement-only rule leaves the same-value case silently picking one of two
    // normalisers. Both paths are REAL assets so the refusal cannot be a lookup failure in
    // disguise; they are DIFFERENT assets so a rule that only fired on presence and a rule that
    // only fired on disagreement would both catch this one.
    if (!UEditorAssetLibrary::DoesAssetExist(EngineSkeletalMeshPath) ||
        !UEditorAssetLibrary::DoesAssetExist(EngineCubePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Skipped: engine skeletal cube or basic cube unavailable."));
        return true;
    }

    TSharedPtr<FJsonObject> SubjectObj = MakeShared<FJsonObject>();
    SubjectObj->SetStringField(TEXT("path"), EngineCubePath);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), EngineSkeletalMeshPath);
    Payload->SetObjectField(TEXT("subject"), SubjectObj);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("handler is registered and invoked"),
            InvokeHandlerWithCapture(Verb, Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("a subject beside a legacy target key is an error, not a success"),
        Capture.bSuccess);
    TestEqual(TEXT("the ambiguity is reported as INVALID_ARGUMENT"),
        Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    // BOTH key names. Asserting only that it failed would be satisfied by a typo in either path,
    // which is the failure mode this pair of assertions exists to exclude.
    TestTrue(TEXT("the refusal names the legacy key"),
        Capture.Message.Contains(TEXT("assetPath")));
    TestTrue(TEXT("the refusal names the subject key"),
        Capture.Message.Contains(TEXT("subject")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimPreviewSubjectKindNotServedTest,
    "PinWright.render.capture_animation_preview.SubjectKindNotServedIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimPreviewSubjectKindNotServedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAnimPreviewSubjectTests;

    // An explicit kind this verb cannot serve is a TYPED refusal that names the verb which can -
    // the same two-way pointer the static-mesh rejection already carries. Without it an agent that
    // reaches for the shared `subject` spelling on the wrong verb learns only "no".
    if (!UEditorAssetLibrary::DoesAssetExist(EngineCubePath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Skipped: engine basic cube unavailable."));
        return true;
    }

    TSharedPtr<FJsonObject> SubjectObj = MakeShared<FJsonObject>();
    SubjectObj->SetStringField(TEXT("kind"), TEXT("staticMesh"));
    SubjectObj->SetStringField(TEXT("path"), EngineCubePath);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetObjectField(TEXT("subject"), SubjectObj);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("handler is registered and invoked"),
            InvokeHandlerWithCapture(Verb, Payload, Capture)))
    {
        return false;
    }
    TestFalse(TEXT("an unservable subject kind is an error, not a success"), Capture.bSuccess);
    TestEqual(TEXT("an unservable subject kind is reported as UNSUPPORTED_ASSET_EDITOR"),
        Capture.ErrorCode, FString(TEXT("UNSUPPORTED_ASSET_EDITOR")));
    TestTrue(TEXT("the refusal names the kind that was asked for"),
        Capture.Message.Contains(TEXT("staticMesh")));
    TestTrue(TEXT("the refusal points at render.capture_asset_preview"),
        Capture.Message.Contains(TEXT("render.capture_asset_preview")));
    return true;
}

// ---------------------------------------------------------------------------
// Live preview path. Guarded, and a guard that fires is REPORTED as unmeasured.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimPreviewColdFirstFrameTest,
    "PinWright.render.capture_animation_preview.ColdFirstFrameIsDiscarded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimPreviewColdFirstFrameTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAnimPreviewSubjectTests;

    // The first capture into a freshly opened preview window is measurably dark - ~0.9 stop in the
    // measured case - and viewport.warmup.settled does NOT catch it, because the frame can stop
    // changing while the ambient contribution is still missing. This verb OPENS the window and
    // captures in the same call, so it is the verb that most needs the throwaway frame. Two halves,
    // and the second is the one a reported-but-not-performed warm-up would fail: the file must be
    // gone from disk, checked against the directory the real shots were written to.
    if (!UEditorAssetLibrary::DoesAssetExist(EngineSkeletalMeshPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Skipped: /Engine/EngineMeshes/SkeletalCube unavailable."));
        return true;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), EngineSkeletalMeshPath);
    Payload->SetNumberField(TEXT("count"), 1);
    Payload->SetNumberField(TEXT("width"), 192);
    Payload->SetNumberField(TEXT("height"), 192);
    // Explicit true, which is also the default. An asset editor left open when the editor exits
    // faults in ~FStaticMeshEditor and its Persona siblings during shutdown, so a test that opens
    // one must close it rather than rely on a default staying put.
    Payload->SetBoolField(TEXT("closeAfterCapture"), true);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("handler is registered and invoked"),
            InvokeHandlerWithCapture(Verb, Payload, Capture)))
    {
        return false;
    }

    if (!Capture.bSuccess)
    {
        // A failure is only acceptable when it is TYPED. An empty or unrecognised code means the
        // capture path did something this test cannot account for, and that is a real failure.
        if (!TestTrue(FString::Printf(
                TEXT("a failed preview capture carries a known typed code (got '%s': %s)"),
                *Capture.ErrorCode, *Capture.Message),
                IsTypedLivePathFailure(Capture.ErrorCode)))
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*this, TEXT("persona-preview-unavailable"),
            FString::Printf(TEXT("%s: %s"), *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    if (!TestTrue(TEXT("a successful burst returns a result object"), Capture.Result.IsValid()))
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* PoseSet = nullptr;
    if (!TestTrue(TEXT("a burst publishes the poseSet block"),
            Capture.Result->TryGetObjectField(TEXT("poseSet"), PoseSet) && PoseSet))
    {
        return false;
    }
    bool bWarmupTaken = false;
    TestTrue(TEXT("poseSet.warmupShotTaken is present"),
        (*PoseSet)->TryGetBoolField(TEXT("warmupShotTaken"), bWarmupTaken));
    TestTrue(TEXT("the throwaway warm-up frame was taken"), bWarmupTaken);
    bool bWarmupDiscarded = false;
    TestTrue(TEXT("poseSet.warmupShotDiscarded is present"),
        (*PoseSet)->TryGetBoolField(TEXT("warmupShotDiscarded"), bWarmupDiscarded));
    TestTrue(TEXT("the throwaway warm-up frame's file was deleted"), bWarmupDiscarded);
    // Absence assertion: the leftover-file warning fires only when a PNG the caller did not ask for
    // is still on disk. Present here means the flag above lied.
    FString LeftoverWarning;
    TestFalse(TEXT("no leftover-warm-up-file warning is emitted"),
        (*PoseSet)->TryGetStringField(TEXT("warmupFileNotDeletedWarning"), LeftoverWarning));

    // The disk check, which is what stops a reported-but-not-performed delete passing. The
    // directory comes from a real shot's own path rather than from a rule this test invents.
    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    FString FirstShotPath;
    if (Capture.Result->TryGetArrayField(TEXT("shots"), Shots) && Shots && Shots->Num() > 0)
    {
        const TSharedPtr<FJsonObject>* ShotObj = nullptr;
        if ((*Shots)[0].IsValid() && (*Shots)[0]->TryGetObject(ShotObj) && ShotObj &&
            (*ShotObj).IsValid())
        {
            (*ShotObj)->TryGetStringField(TEXT("path"), FirstShotPath);
        }
    }
    if (TestFalse(TEXT("a captured shot reports the file it wrote"), FirstShotPath.IsEmpty()))
    {
        const FString OutputDir = FPaths::GetPath(FirstShotPath);
        TArray<FString> Leftovers;
        IFileManager::Get().FindFiles(Leftovers, *(OutputDir / TEXT("*_warmup_*.png")),
            /*Files=*/true, /*Directories=*/false);
        TestEqual(FString::Printf(TEXT("no *_warmup_*.png survives in %s"), *OutputDir),
            Leftovers.Num(), 0);
    }

    DeleteShotFiles(Capture.Result);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimPreviewAnglesAndViewsTest,
    "PinWright.render.capture_animation_preview.AnglesAndViewsAreNotInvented",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimPreviewAnglesAndViewsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightAnimPreviewSubjectTests;

    // `angles` and `views` are ARGUMENTS of this verb and have never been RESPONSE keys. A refactor
    // that "restores" them would be inventing wire contract, and a plan brief already claimed they
    // existed. The `angles` half is the load-bearing one: the argument IS supplied below, so an
    // implementation that echoed its arguments back would fail here rather than pass by omission.
    //
    // The three keys that DO exist keep their meanings: `count` is the number of SHOTS, `viewCount`
    // is the size of the VIEW PLAN, and `frames` is one entry per sampled instant. They are
    // asserted against each other, not against literals, so the test survives a fixture change.
    if (!UEditorAssetLibrary::DoesAssetExist(EngineSkeletalMeshPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("engine-cube-unavailable"),
            TEXT("Skipped: /Engine/EngineMeshes/SkeletalCube unavailable."));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Angles;
    for (const float Azimuth : {0.0f, 90.0f})
    {
        TSharedPtr<FJsonObject> Angle = MakeShared<FJsonObject>();
        Angle->SetNumberField(TEXT("azimuth"), Azimuth);
        Angle->SetNumberField(TEXT("elevation"), 15.0);
        Angles.Add(MakeShared<FJsonValueObject>(Angle));
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), EngineSkeletalMeshPath);
    Payload->SetArrayField(TEXT("angles"), Angles);
    Payload->SetNumberField(TEXT("width"), 192);
    Payload->SetNumberField(TEXT("height"), 192);
    Payload->SetBoolField(TEXT("closeAfterCapture"), true);

    FTestResponseCapture Capture;
    if (!TestTrue(TEXT("handler is registered and invoked"),
            InvokeHandlerWithCapture(Verb, Payload, Capture)))
    {
        return false;
    }

    if (!Capture.bSuccess)
    {
        if (!TestTrue(FString::Printf(
                TEXT("a failed preview capture carries a known typed code (got '%s': %s)"),
                *Capture.ErrorCode, *Capture.Message),
                IsTypedLivePathFailure(Capture.ErrorCode)))
        {
            return false;
        }
        PinWrightTestSkip::SkipAssertions(*this, TEXT("persona-preview-unavailable"),
            FString::Printf(TEXT("%s: %s"), *Capture.ErrorCode, *Capture.Message));
        return true;
    }

    if (!TestTrue(TEXT("a successful burst returns a result object"), Capture.Result.IsValid()))
    {
        return false;
    }

    // The absence half, both directions asserted in the same run.
    TestFalse(TEXT("the response carries no 'angles' key -- it never has"),
        Capture.Result->HasField(TEXT("angles")));
    TestFalse(TEXT("the response carries no 'views' key -- it never has"),
        Capture.Result->HasField(TEXT("views")));

    // The presence half.
    const TArray<TSharedPtr<FJsonValue>>* Shots = nullptr;
    if (!TestTrue(TEXT("the response carries shots[]"),
            Capture.Result->TryGetArrayField(TEXT("shots"), Shots) && Shots))
    {
        return false;
    }
    double Count = -1.0;
    TestTrue(TEXT("the response carries 'count'"),
        Capture.Result->TryGetNumberField(TEXT("count"), Count));
    TestEqual(TEXT("'count' is the number of SHOTS captured"),
        static_cast<int32>(Count), Shots->Num());

    double ViewCount = -1.0;
    TestTrue(TEXT("the response carries 'viewCount'"),
        Capture.Result->TryGetNumberField(TEXT("viewCount"), ViewCount));
    TestEqual(TEXT("'viewCount' is the size of the VIEW PLAN, which was two angles"),
        static_cast<int32>(ViewCount), 2);

    const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
    if (TestTrue(TEXT("the response carries frames[]"),
            Capture.Result->TryGetArrayField(TEXT("frames"), Frames) && Frames))
    {
        double FrameCount = -1.0;
        TestTrue(TEXT("the response carries 'frameCount'"),
            Capture.Result->TryGetNumberField(TEXT("frameCount"), FrameCount));
        TestEqual(TEXT("'frames' has one entry per sampled instant"),
            Frames->Num(), static_cast<int32>(FrameCount));
        TestEqual(TEXT("count == frameCount * viewCount"),
            static_cast<int32>(Count),
            static_cast<int32>(FrameCount) * static_cast<int32>(ViewCount));
    }

    // The blocks this verb gained by moving onto the shared layer. Asserted here rather than in
    // their own test because they need the same expensive live burst, and a second one doubles the
    // cost of the slowest test in this file.
    TestTrue(TEXT("a burst publishes shotDistribution"),
        Capture.Result->HasTypedField<EJson::Object>(TEXT("shotDistribution")));
    TestTrue(TEXT("a burst publishes poseSet"),
        Capture.Result->HasTypedField<EJson::Object>(TEXT("poseSet")));
    TestTrue(TEXT("a burst publishes subject"),
        Capture.Result->HasTypedField<EJson::Object>(TEXT("subject")));

    // resolutionSource speaks the ONE shared vocabulary now. "caller" because this request passed
    // width and height; the string that moved in this wave is the burst default, "burstBudget" ->
    // "budget", and asserting "caller" here pins that the classifier is in use at all.
    FString ResolutionSource;
    TestTrue(TEXT("the response carries resolutionSource"),
        Capture.Result->TryGetStringField(TEXT("resolutionSource"), ResolutionSource));
    TestEqual(TEXT("a caller-sized burst reports resolutionSource 'caller'"),
        ResolutionSource, FString(TEXT("caller")));

    // Per-shot `viewport`, which used to exist only once at the top level from the LAST capture -
    // so a burst whose third shot was the mis-aimed one published an aim verdict belonging to
    // shot N. Checked on every shot, because "the first one has it" is not the claim.
    for (int32 Index = 0; Index < Shots->Num(); ++Index)
    {
        const TSharedPtr<FJsonObject>* ShotObj = nullptr;
        if ((*Shots)[Index].IsValid() && (*Shots)[Index]->TryGetObject(ShotObj) && ShotObj &&
            (*ShotObj).IsValid())
        {
            TestTrue(*FString::Printf(TEXT("shot %d carries its own viewport block"), Index),
                (*ShotObj)->HasTypedField<EJson::Object>(TEXT("viewport")));
        }
    }

    DeleteShotFiles(Capture.Result);
    return true;
}
