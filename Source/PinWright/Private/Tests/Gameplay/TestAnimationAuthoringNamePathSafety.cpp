// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestAnimationAuthoringNamePathSafety.cpp - regression coverage for the animation.authoring
// cluster of board B-createpackage-unvalidated-paths-plugin-wide.
//
// WHAT WAS WRONG. Nine handlers across AnimationAuthoringHandler_{Sequence,BlendSpace,
// AnimBlueprint}.cpp composed "<path>/<name>" from two raw caller strings and handed the result
// straight to CreatePackage with no validation of any kind - verified by direct read, not taken
// from the ticket: the three files carried no SanitizeProjectRelativePath, IsValidAssetPath,
// ValidateAssetCreationPath, SanitizePackageName or IsValid*LongPackageName call anywhere.
// CreatePackage has TWO Fatals, and both were reachable. It logs a name containing "//" at
// **Fatal** (UObjectGlobals.cpp:1094-1096) and a name that resolves to EMPTY through ResolveName2
// at Fatal too (:1118) - a verbosity that is not compiled out in any configuration. So the call
// did not fail: the editor PROCESS died, taking every unsaved package in it, in an editor shared
// between agents. `name: "a//b"` is a one-argument kill through the first; `name: ".."` is a
// one-argument kill through the second. The handlers' own `if (!Package)` branches could never
// fire, because nothing after CreatePackage was ever reached. Measured once, on a different verb,
// in B-foliage-add-type-name-with-slash-kills-the-editor (editor pid 7856).
//
// BOTH ARGUMENTS, NOT JUST `name`. The ticket lists these sites under "no guard at all" and
// discusses `name`, but `path` is caller text too, and the transform applied to it was a
// NORMALIZER, not a validator: the file-local AnimationAuthoringHelpers::NormalizeAnimPath mapped
// /Content to /Game, converted backslashes and stripped TRAILING slashes, but did not collapse an
// interior "//" and did not reject "..". `path: "/Game//Animations"` therefore reached the same
// Fatal with a perfectly bare `name`. ExpectDoubleSlashFolderRefused covers that half.
//
// WHERE THE `path` REFUSAL COMES FROM NOW, which is NOT this handler. NormalizeAnimPath is gone,
// replaced by the shared NormalizeContentAssetPath (Utils/PathUtils.h), which front-loads
// SanitizeProjectRelativePath and therefore COLLAPSES an interior "//" rather than refusing it.
// The refusal moved one layer up: `path` is declared type "path" in the ParamSpec, and the
// dispatcher's declared-type gate (Handlers/ParamTypeCheck.h) refuses a "path"-typed value
// containing "//" with INVALID_ARGUMENT before any handler runs. ExpectDoubleSlashFolderRefused
// still asserts refusal + INVALID_ARGUMENT + the offending value quoted, so it now pins the gate
// rather than the handler; either layer going missing is still a red. That last assertion REQUIRES
// the gate's refusal message to quote the received value.
//
// AND WHAT MUST STILL WORK. PinWrightComposeAssetPackagePath composes with Printf("%s/%s"), which
// WOULD double a separator if handed a folder ending in "/" - unlike FString::operator/
// (PathAppend, Core/Private/Containers/String.cpp.inl:855-885), which does not. That is a real
// hazard for sites whose folder is raw caller text, but not for these nine: the normalizer's
// trailing-separator trim runs on `path` at every one of them (NormalizeContentAssetPath keeps the
// `while (EndsWith("/")) LeftChopInline(1)` loop verbatim), so a trailing slash provably cannot
// reach the helper and no trim was added. ExpectTrailingSlashFolderStillAccepted pins that, so a
// future edit that drops the normalizer turns into a red test rather than into callers being
// refused a folder that works today.
//
// WHY THIS TEST DOES NOT DRIVE THE CRASH, AND CANNOT ACCIDENTALLY DRIVE IT. A Fatal takes the test
// host down with it, so a test that reproduced the defect would abort the whole suite rather than
// report a red - and a suite that dies mid-queue is not a failure signal, it is an absence of one
// (the DID_NOT_COMPLETE state in the plugin's testing notes). The assertion is therefore on the
// post-fix CONTRACT - the refusal - and the fixture is built so that a build WITHOUT the fix takes
// a different, harmless path instead of the fatal one:
//
//   every refusal case pairs its bad `name` with a `skeletonPath` that is a well-formed long
//   package name naming NO asset (a fresh GUID). All three verbs driven here resolve their
//   skeleton ABOVE the composition on a pre-fix build, so a reverted build is refused
//   SKELETON_NOT_FOUND at that load and the TestEqual on INVALID_ARGUMENT below goes red while the
//   process lives. On a fixed build the composition check sits ABOVE the skeleton load, so the
//   same payload is refused INVALID_ARGUMENT. CreatePackage is unreachable on both builds.
//
// That property depends on the fix keeping its name/path check ABOVE the skeleton resolution, and
// each handler carries a comment saying so. Do NOT "improve" these cases by supplying a skeleton
// that exists, and do NOT rewrite them into a crash expectation: either change hands a live editor
// a string that ends it.
//
// WHY THESE THREE VERBS. The nine sites are one idiom - `Path / Name` off Ctx.GetString, composed
// inline - so one verb per handler FILE is what this covers: create_animation_sequence
// (Sequence.cpp, shared with create_montage and create_composite), create_blend_space_1d
// (BlendSpace.cpp, shared with create_blend_space_2d and create_aim_offset) and
// create_pose_library (AnimBlueprint.cpp).
//
// WHAT IS DELIBERATELY NOT DRIVEN, AND WHY. AnimBlueprint.cpp's other two sites -
// create_ik_retargeter and the UE 5.1-5.4 create_control_rig fallback branch - resolve NOTHING
// before their composition: their IK Rig / skeletal mesh arguments are optional and unread at that
// point. There is therefore no earlier refusal to catch a reverted build, and driving a bad `name`
// at either of them would reach CreatePackage and kill the suite host on exactly the build this
// test exists to detect. They are fixed the same way and left uncovered rather than covered by a
// test that is only safe while the fix is present.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses, which also validates each payload
// against the declared ParamSpec.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

#include "Misc/Guid.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace AnimAuthoringNamePathSafetyHelpers
{
    // The folder every verb here defaults to, which no refusal case may write into.
    constexpr const TCHAR* DefaultAnimFolder = TEXT("/Game/Animations");

    // A well-formed long package name that names nothing. This is the load-bearing half of the
    // no-crash guarantee described in the file header: it makes a PRE-FIX handler bail at its
    // skeleton load, above the concatenation, instead of reaching CreatePackage.
    inline FString AnimSafetyAbsentSkeletonPath()
    {
        return FString::Printf(TEXT("/Game/PinWrightMissing/SK_Absent_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString AnimSafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWAnimSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString AnimSafetyObjectPath(const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s.%s"), DefaultAnimFolder, *Name, *Name);
    }

    // Drives one refusal case and reports the three facts that separate the post-fix contract from
    // the pre-fix behaviour: it is refused, it is refused as a CALLER ARGUMENT error rather than as
    // a missing skeleton, and the message names the offending value so the caller can act.
    inline void ExpectAnimNameRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const TCHAR* Method, const FString& BadName,
        const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), BadName);
        Params->SetStringField(TEXT("skeletonPath"), AnimSafetyAbsentSkeletonPath());

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
            TEXT("req-anim-authoring-name-safety"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(TEXT("%s: a %s name ('%s') is refused"),
            Method, Label, *BadName), bSuccess);
        // The discriminator against a reverted fix: pre-fix this same payload answers
        // SKELETON_NOT_FOUND from the skeleton load, never INVALID_ARGUMENT.
        Test.TestEqual(*FString::Printf(
            TEXT("%s: a %s name is refused as a caller argument error, not as a missing skeleton"),
            Method, Label), ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(TEXT("%s: the %s refusal quotes the offending name"),
            Method, Label), Sink->Message.Contains(BadName));
        // A refusal that still left a half-built package behind would be the same data hazard in
        // slower motion - a later editor-wide save-all flushes it into host Content.
        Test.TestTrue(*FString::Printf(TEXT("%s: the %s refusal creates no object"), Method, Label),
            FindObject<UObject>(nullptr, *AnimSafetyObjectPath(BadName)) == nullptr);
    }

    // The whole class in one place, so all three verbs are held to the same vocabulary.
    inline void ExpectEveryBadNameRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const TCHAR* Method)
    {
        // The one-argument kill named in the ticket: FString::operator/ does not double a leading
        // slash, but a name that carries "//" itself composes "//" whatever the folder is.
        ExpectAnimNameRefused(Test, Dispatcher, Sink, Method, TEXT("a//b"), TEXT("embedded '//'"));

        // The shape measured on the foliage ticket: a rooted path in `name`.
        ExpectAnimNameRefused(Test, Dispatcher, Sink, Method,
            TEXT("/Game/PinWrightScratch/PWScratch_AnimSafety"), TEXT("rooted path"));

        // An interior slash composes a nested package rather than "//", so it does not fatal - but
        // it silently writes somewhere the caller did not name, which is the same argument
        // confusion one step short of the crash.
        ExpectAnimNameRefused(Test, Dispatcher, Sink, Method, TEXT("Sub/Leaf"),
            TEXT("interior slash"));

        // A backslash is not in INVALID_OBJECTNAME_CHARACTERS but IS in
        // INVALID_LONGPACKAGE_CHARACTERS, so this case is what proves the composed path is checked
        // against the engine's package rules as well as the name against its object rules.
        ExpectAnimNameRefused(Test, Dispatcher, Sink, Method, TEXT("Sub\\Leaf"), TEXT("backslash"));

        // Traversal, in the argument that has no sanitizer of its own.
        ExpectAnimNameRefused(Test, Dispatcher, Sink, Method, TEXT("../Escape"), TEXT("traversal"));

        // CreatePackage has a SECOND Fatal, and this is the input that reaches it: ".." survives
        // as a package name and then resolves to EMPTY through ResolveName2, which is fatal at
        // UObjectGlobals.cpp:1118, not at the "//" check on :1094-1096. A guard that only rejected
        // slashes would pass it straight through. INVALID_OBJECTNAME_CHARACTERS contains '.' (and
        // ':'), so routing through the engine's own rule covers it and a hand-rolled slash filter
        // would not - which is the reason this file asserts on the shared helper rather than on a
        // character list of the plugin's own devising.
        ExpectAnimNameRefused(Test, Dispatcher, Sink, Method, TEXT(".."), TEXT("dot-dot"));

        // A trailing slash composes "<folder>/Name/", which CreatePackage would resolve to
        // something other than the caller's name.
        ExpectAnimNameRefused(Test, Dispatcher, Sink, Method, TEXT("Trailing/"),
            TEXT("trailing slash"));
    }

    // The OTHER half of the hazard, and the half the ticket's per-site notes do not spell out.
    // `path` is caller text too, and the transform applied to it was a NORMALIZER, not a validator:
    // it mapped /Content to /Game, converted backslashes and stripped TRAILING slashes, but did not
    // collapse an interior "//". So `path: "/Game//Animations"` with a perfectly bare `name`
    // composed "/Game//Animations/<name>" and reached the same Fatal with nothing wrong with `name`
    // at all. Safe to drive for the same reason the name cases are: the skeleton load sits above
    // the concatenation on a pre-fix build, so a reverted build answers SKELETON_NOT_FOUND here too.
    //
    // The refusal now comes from the dispatcher's declared-type gate on the "path"-typed parameter,
    // above the handler entirely - see the file header. Asserted the same way on purpose: what this
    // case is for is that the payload is REFUSED with an actionable message, not which layer said so.
    inline void ExpectDoubleSlashFolderRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const TCHAR* Method)
    {
        const FString BadFolder = TEXT("/Game//Animations");

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), AnimSafetyUniqueName(TEXT("BadFolder")));
        Params->SetStringField(TEXT("path"), BadFolder);
        Params->SetStringField(TEXT("skeletonPath"), AnimSafetyAbsentSkeletonPath());

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
            TEXT("req-anim-authoring-folder-safety"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(TEXT("%s: a path containing '//' is refused"), Method),
            bSuccess);
        Test.TestEqual(*FString::Printf(
            TEXT("%s: a '//' path is refused as a caller argument error, not as a missing skeleton"),
            Method), ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(TEXT("%s: the '//' path refusal quotes the composed path"),
            Method), Sink->Message.Contains(BadFolder));
    }

    // A trailing slash on `path` must still WORK, not start being refused. The normalizer strips it
    // before the composition, so "<folder>/" and "<folder>" are the same request - and they were
    // the same request before this fix too, because FString::operator/ (PathAppend,
    // Core/Private/Containers/String.cpp.inl:855-885) does not double a separator either. The
    // shared helper composes with Printf("%s/%s"), which WOULD double one, so this case pins the
    // fact that the trailing slash never reaches it. Driven with the absent skeleton, so a pass is
    // SKELETON_NOT_FOUND: the folder got through the check, which is the whole point.
    inline void ExpectTrailingSlashFolderStillAccepted(FAutomationTestBase& Test,
        FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink, const TCHAR* Method)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), AnimSafetyUniqueName(TEXT("TrailingFolder")));
        Params->SetStringField(TEXT("path"), FString(DefaultAnimFolder) + TEXT("/"));
        Params->SetStringField(TEXT("skeletonPath"), AnimSafetyAbsentSkeletonPath());

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
            TEXT("req-anim-authoring-trailing-folder"), Params, bSuccess, ErrorCode);

        Test.TestEqual(*FString::Printf(
            TEXT("%s: a trailing slash on path is still accepted - it reaches the skeleton load, "
                 "it is not refused as a bad path"), Method),
            ErrorCode, FString(TEXT("SKELETON_NOT_FOUND")));
    }

    // CONTROL. Without it, a handler that refused every name would satisfy every case above.
    // Driven with the same absent skeleton, so it stops at SKELETON_NOT_FOUND - which is exactly
    // the proof that a bare name got PAST the new check and reached the skeleton resolution.
    inline void ExpectBareNameReachesSkeletonLoad(FAutomationTestBase& Test,
        FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink, const TCHAR* Method)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), AnimSafetyUniqueName(TEXT("Control")));
        Params->SetStringField(TEXT("skeletonPath"), AnimSafetyAbsentSkeletonPath());

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
            TEXT("req-anim-authoring-name-control"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(
            TEXT("%s: the control call still fails - its skeleton does not exist"), Method),
            bSuccess);
        Test.TestEqual(*FString::Printf(
            TEXT("%s: a BARE name passes the name check and is refused on the skeleton instead"),
            Method), ErrorCode, FString(TEXT("SKELETON_NOT_FOUND")));
    }
}

// Each RunTest below opens the helper namespace inside its own body rather than at file scope: a
// file-scope using-directive would leak into every other test .cpp that Unity merges after this
// one into the same translation unit.

// ============================================================================
// AnimationAuthoringHandler_Sequence.cpp
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringCreateSequenceNamePathRefusedTest,
    "PinWright.animation.authoring.create_animation_sequence.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringCreateSequenceNamePathRefusedTest::RunTest(const FString& Parameters)
{
    using namespace AnimAuthoringNamePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    ExpectEveryBadNameRefused(*this, Dispatcher, Sink,
        TEXT("animation.authoring.create_animation_sequence"));
    ExpectDoubleSlashFolderRefused(*this, Dispatcher, Sink,
        TEXT("animation.authoring.create_animation_sequence"));
    ExpectTrailingSlashFolderStillAccepted(*this, Dispatcher, Sink,
        TEXT("animation.authoring.create_animation_sequence"));
    ExpectBareNameReachesSkeletonLoad(*this, Dispatcher, Sink,
        TEXT("animation.authoring.create_animation_sequence"));

    return true;
}

// ============================================================================
// AnimationAuthoringHandler_BlendSpace.cpp
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringCreateBlendSpace1DNamePathRefusedTest,
    "PinWright.animation.authoring.create_blend_space_1d.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringCreateBlendSpace1DNamePathRefusedTest::RunTest(const FString& Parameters)
{
    using namespace AnimAuthoringNamePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    ExpectEveryBadNameRefused(*this, Dispatcher, Sink,
        TEXT("animation.authoring.create_blend_space_1d"));
    ExpectDoubleSlashFolderRefused(*this, Dispatcher, Sink,
        TEXT("animation.authoring.create_blend_space_1d"));
    ExpectTrailingSlashFolderStillAccepted(*this, Dispatcher, Sink,
        TEXT("animation.authoring.create_blend_space_1d"));
    ExpectBareNameReachesSkeletonLoad(*this, Dispatcher, Sink,
        TEXT("animation.authoring.create_blend_space_1d"));

    return true;
}

// ============================================================================
// AnimationAuthoringHandler_AnimBlueprint.cpp
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAnimAuthoringCreatePoseLibraryNamePathRefusedTest,
    "PinWright.animation.authoring.create_pose_library.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAnimAuthoringCreatePoseLibraryNamePathRefusedTest::RunTest(const FString& Parameters)
{
    using namespace AnimAuthoringNamePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    ExpectEveryBadNameRefused(*this, Dispatcher, Sink,
        TEXT("animation.authoring.create_pose_library"));
    ExpectDoubleSlashFolderRefused(*this, Dispatcher, Sink,
        TEXT("animation.authoring.create_pose_library"));
    ExpectTrailingSlashFolderStillAccepted(*this, Dispatcher, Sink,
        TEXT("animation.authoring.create_pose_library"));
    ExpectBareNameReachesSkeletonLoad(*this, Dispatcher, Sink,
        TEXT("animation.authoring.create_pose_library"));

    return true;
}
