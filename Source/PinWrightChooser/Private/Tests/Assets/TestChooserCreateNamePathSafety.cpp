// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestChooserCreateNamePathSafety.cpp - regression coverage for the chooser.create site of
// B-createpackage-unvalidated-paths-plugin-wide.
//
// WHAT WAS WRONG. chooser.create composes its destination two ways. Given `path` alone the whole
// string is one caller-supplied package path and goes through NormalizePackagePath
// (SanitizeProjectRelativePath). Given `name` as well, `path` becomes the FOLDER and `name` is the
// leaf - and the leaf was concatenated on raw. That is the ticket's "folder guarded, caller's NAME
// unchecked" shape: a `name` of "Sub/Leaf" or "/Game/X" composed a package the caller never named
// and wrote the chooser there silently, and one character worse ("a//b") is the input CreatePackage
// (UObjectGlobals.cpp:1094-1096) logs at **Fatal** - a verbosity not compiled out in any
// configuration, which ends the PROCESS and every unsaved package in it.
//
// The fix routes the name branch of BuildCreatePaths through the shared
// PinWrightComposeAssetPackagePath (Handlers/PackagePathCompose.h): FName::IsValidXName +
// INVALID_OBJECTNAME_CHARACTERS on the bare name, FPackageName::IsValidLongPackageName on the
// composed path, both engine reasons surfaced verbatim, refused INVALID_ARGUMENT.
//
// WHY THIS TEST DOES NOT DRIVE THE CRASH, AND CANNOT ACCIDENTALLY DRIVE IT. A Fatal takes the test
// host down with it, so a test that reproduced the defect would abort the whole suite rather than
// report a red - and a suite that dies mid-queue is an absence of a signal, not a failure one (the
// DID_NOT_COMPLETE state in the plugin's testing notes). The assertion is on the post-fix CONTRACT
// - the refusal - and the fixture is built so a build WITHOUT the fix takes a harmless path:
//
//   1. ON THE FIXED BUILD nothing is ever composed. INVALID_OBJECTNAME_CHARACTERS contains '/'
//      (NameTypes.h:191), so IsValidXName refuses every path-shaped `name` BEFORE the join, and
//      the join itself cannot double a slash: BuildCreatePaths chops every trailing '/' off the
//      folder and the name that reaches Printf("%s/%s") is known to contain no '/' at all.
//   2. ON A REVERTED BUILD (this guard removed, everything else as it was) the old
//      `Folder / Name` runs. FString::operator/ -> PathAppend (String.cpp.inl:855-885) does NOT
//      double a leading slash on the right, and the PRE-EXISTING
//      FPackageName::IsValidLongPackageName at the end of BuildCreatePaths - untouched by this
//      change, and still above the CreatePackage call - refuses "a//b", "Trailing/" and the
//      backslash case there instead, as INVALID_PATH.
//   3. FOR THE RESIDUAL CASES that a reverted build composes into a perfectly VALID path
//      ("Sub/Leaf", "/Game/X/Y" - no "//", non-empty), every refusal case below pairs its bad
//      `name` with contextObjectType "/Script/Engine.DoesNotExist": a well-formed object path in
//      an ALREADY-LOADED script package that names no class. HandleCreate resolves that class
//      ABOVE CreatePackage, so the reverted build is refused CLASS_NOT_FOUND there and the
//      TestEqual on INVALID_ARGUMENT below goes red while the process lives.
//
// CreatePackage is therefore unreachable on both the fixed and the reverted build. That property
// depends on the new check staying ABOVE the concatenation and above the context-class
// resolution, and BuildCreatePaths carries a comment saying so. Do NOT "improve" these cases by
// supplying a context class that resolves, and do NOT rewrite them into a crash expectation:
// either change hands a live editor a string that ends it.
//
// BOTH of CreatePackage's Fatals are covered, not just the well-known one. "a//b" aims at the
// double-slash Fatal (:1094-1096); ".." aims at the empty-name Fatal (:1118, empty after
// ResolveName2). INVALID_OBJECTNAME_CHARACTERS covers '/', '.' and ':' (NameTypes.h:191) and
// INVALID_LONGPACKAGE_CHARACTERS covers '.' too (:197), so one guard turns away both classes and
// neither case can reach either Fatal on either build.
//
// THE SECOND CONTROL IS NOT OPTIONAL. The shared helper joins with Printf("%s/%s"), which doubles
// the separator when the folder already ends in '/' - where the old FString::operator/ did not.
// TrailingSlashFolder below is what proves the fix did not start refusing folders that work
// today; the refusal cases alone would pass a build that had.
//
// THE FOLDER IS A KILL VECTOR IN ITS OWN RIGHT, not just the name, and a bare legal `name` cannot
// save a folder carrying "//". Here it is disarmed one step earlier than the name is:
// NormalizePackagePath routes the folder through SanitizeProjectRelativePath, whose collapse loop
// (PathUtils.cpp:57-60) rewrites "//" to "/" before anything is composed, so both of
// chooser.create's branches answer CLASS_NOT_FOUND rather than refusing. The last two cases assert
// exactly that, so this file goes RED - never fatal - if that sanitizer is ever swapped for one
// that only normalizes (strips trailing slashes, maps /Content to /Game) without collapsing, the
// shape found in a sibling cluster of this same sweep. The backstop if that happens is that the
// COMPOSED path is validated too, by FPackageName::IsValidLongPackageName inside
// PinWrightComposeAssetPackagePath and again at the end of BuildCreatePaths; a name-only character
// check would not have covered the folder at all.
//
// WHY THE REFUSAL IS NOT A DEAD END. Unlike the foliage instance - where the destination was a
// literal and putting a path in `name` was the only lever a caller had - chooser.create already
// exposes `path`, and `path` is REQUIRED. A caller who wants a nested destination names it there
// and omits `name` entirely, which is the branch the whole-path sanitizer already covered. The
// control case below is what proves a legitimate bare `name` still gets through.

#include "Misc/AutomationTest.h"

#include "Chooser.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"

// Named (not anonymous) namespace: the plugin's tests share one module per sub-module with Unity
// builds enabled, where same-named anonymous-namespace helpers collide across merged TUs.
namespace ChooserCreateNamePathSafetyHelpers
{
    // A folder that is NOT chooser.create's /Game/Choosers default, so a refusal that leaked an
    // asset anyway is visible under a path this test owns and cleans.
    constexpr const TCHAR* SafetyScratchFolder = TEXT("/Game/PinWrightTests/ChooserCreateNameSafety");

    // A well-formed object path in an already-loaded script package that names no class. This is
    // the load-bearing half of the no-crash guarantee in the file header: it makes a REVERTED
    // build bail at HandleCreate's context-class resolution, above CreatePackage, for the bad
    // names whose composed path would otherwise have been valid. "Already loaded" matters -
    // StaticLoadObject resolves through ResolveName2 with Create=true, which itself calls
    // CreatePackage on an unloaded package name (UObjectGlobals.cpp:1310).
    constexpr const TCHAR* SafetyAbsentContextClass = TEXT("/Script/Engine.DoesNotExist");

    inline FString SafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWChooserNameSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString SafetyPackagePath(const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s"), SafetyScratchFolder, *Name);
    }

    inline FString SafetyObjectPath(const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s.%s"), SafetyScratchFolder, *Name, *Name);
    }

    // Drives one refusal case and reports on all three facts that separate the post-fix contract
    // from the pre-fix behaviour: it is refused, it is refused as a CALLER ARGUMENT error rather
    // than as an unresolvable class, and the message names the offending value so the caller can
    // act on it.
    inline void ExpectNameRefused(FAutomationTestBase& Test, const FString& BadName,
        const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("path"), SafetyScratchFolder);
        Params->SetStringField(TEXT("name"), BadName);
        Params->SetStringField(TEXT("contextObjectType"), SafetyAbsentContextClass);

        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("chooser.create"), Params, Capture);
        Test.TestTrue(TEXT("chooser.create handler is registered"), bFound);
        Test.TestTrue(*FString::Printf(TEXT("chooser.create answered the %s name"), Label),
            Capture.bWasCalled);

        Test.TestFalse(*FString::Printf(TEXT("a %s name ('%s') is refused"), Label, *BadName),
            Capture.bSuccess);
        // The discriminator against a reverted fix: pre-fix this same payload answers either
        // INVALID_PATH (from the surviving composed-path check) or CLASS_NOT_FOUND (from the
        // context class), never INVALID_ARGUMENT.
        Test.TestEqual(*FString::Printf(
            TEXT("a %s name is refused as a caller argument error, not as a path or class error"),
            Label), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(
            TEXT("the %s refusal quotes the offending name or composed path"), Label),
            Capture.Message.Contains(BadName));
        // A refusal that still left a half-built chooser behind would be the same data hazard in
        // slower motion - a later editor-wide save-all flushes it into host Content.
        Test.TestNull(*FString::Printf(TEXT("the %s refusal creates no chooser table"), Label),
            FindObject<UChooserTable>(nullptr, *SafetyObjectPath(BadName)));
    }
}

// ============================================================================
// A `name` carrying a path is refused instead of being concatenated onto the folder
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FChooserCreateNameCarryingAPathIsRefusedTest,
    "PinWright.chooser.CreateNameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FChooserCreateNameCarryingAPathIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace ChooserCreateNamePathSafetyHelpers;

    // The ticket's one-argument kill: a name that itself carries "//" is fatal at CreatePackage
    // whichever composition style the handler uses.
    ExpectNameRefused(*this, TEXT("a//b"), TEXT("double slash"));

    // A leading slash - the shape measured on the foliage instance. PathAppend does not double
    // it, so this one never fataled; it silently wrote to a package the caller did not name,
    // which is the same argument confusion one step short of the crash.
    ExpectNameRefused(*this, TEXT("/Game/PinWrightScratch/PWChooserRooted"), TEXT("rooted path"));

    // An interior slash composes a nested package rather than "//" - same silent misplacement.
    ExpectNameRefused(*this, TEXT("Sub/Leaf"), TEXT("interior slash"));

    // A backslash is NOT in INVALID_OBJECTNAME_CHARACTERS but IS in
    // INVALID_LONGPACKAGE_CHARACTERS (NameTypes.h:191,197), so this case is what proves the
    // composed path is checked against the engine's PACKAGE rules as well as the bare name
    // against its OBJECT rules. Drop either half of the helper and this case stops being refused.
    ExpectNameRefused(*this, TEXT("Sub\\Leaf"), TEXT("backslash"));

    // Traversal, in the argument that has no sanitizer of its own.
    ExpectNameRefused(*this, TEXT("../Escape"), TEXT("traversal"));

    // A bare ".." aims at CreatePackage's OTHER Fatal (UObjectGlobals.cpp:1118, empty after
    // ResolveName2), not the double-slash one. INVALID_OBJECTNAME_CHARACTERS covers '.'
    // (NameTypes.h:191) so the bare-name half refuses it; on a reverted build the composed
    // "<folder>/.." is refused by INVALID_LONGPACKAGE_CHARACTERS (:197), which also covers '.',
    // so this case cannot reach either Fatal on either build.
    ExpectNameRefused(*this, TEXT(".."), TEXT("bare traversal"));

    // A trailing slash composes "<folder>/Name/", which CreatePackage would resolve to something
    // other than the caller's name.
    ExpectNameRefused(*this, TEXT("Trailing/"), TEXT("trailing slash"));

    // CONTROL. Without this, a handler that refused every name would satisfy every case above.
    // Driven with the same unresolvable context class, so it stops at CLASS_NOT_FOUND - which is
    // exactly the proof that a BARE name got PAST the new check and reached the class resolution.
    {
        const FString ControlName = SafetyUniqueName(TEXT("Control"));
        // Torn down anyway: if the control DID create something, this test must not leak it.
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(SafetyPackagePath(ControlName));
        };

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("path"), SafetyScratchFolder);
        Params->SetStringField(TEXT("name"), ControlName);
        Params->SetStringField(TEXT("contextObjectType"), SafetyAbsentContextClass);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("chooser.create"), Params, Capture);

        TestFalse(TEXT("the control call still fails - its context class does not exist"),
            Capture.bSuccess);
        TestEqual(TEXT("a BARE name passes the name check and is refused on the class instead"),
            Capture.ErrorCode, FString(TEXT("CLASS_NOT_FOUND")));
        TestNull(TEXT("the control call creates no chooser table either"),
            FindObject<UChooserTable>(nullptr, *SafetyObjectPath(ControlName)));
    }

    // SECOND CONTROL, guarding the fix against its own failure mode. The shared helper joins
    // with Printf("%s/%s"), which doubles the separator when the folder already ends in '/' -
    // where the old FString::operator/ (PathAppend, String.cpp.inl:855-885) did not. A folder
    // spelled with a trailing slash therefore MUST still work: BuildCreatePaths chops it (and
    // SanitizeProjectRelativePath collapses "//" before that), so the composed path is the same
    // one the caller meant. Without this case the fix could start refusing a folder that works
    // today for a "//" the caller never wrote, and every assertion above would still pass.
    {
        const FString ControlName = SafetyUniqueName(TEXT("TrailingSlashFolder"));
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(SafetyPackagePath(ControlName));
        };

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("path"), FString(SafetyScratchFolder) + TEXT("/"));
        Params->SetStringField(TEXT("name"), ControlName);
        Params->SetStringField(TEXT("contextObjectType"), SafetyAbsentContextClass);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("chooser.create"), Params, Capture);

        TestFalse(TEXT("the trailing-slash-folder control still fails on its context class"),
            Capture.bSuccess);
        TestEqual(TEXT("a folder spelled with a trailing slash is NOT refused as a bad argument "
                       "- it composes the same path and reaches the class resolution"),
            Capture.ErrorCode, FString(TEXT("CLASS_NOT_FOUND")));
    }

    // THE FOLDER IS A KILL VECTOR TOO, and it is a different code path from every case above: a
    // bare, perfectly legal `name` cannot save a folder carrying "//". Both of chooser.create's
    // branches are driven, because they compose differently and only share the final validation.
    //
    // On this build both answer CLASS_NOT_FOUND rather than a refusal, and that is the correct
    // contract, not a hole: NormalizePackagePath routes the folder through
    // SanitizeProjectRelativePath, whose collapse loop (PathUtils.cpp:57-60) rewrites "//" to "/"
    // BEFORE anything is composed - so "/Game/X//Y" is the caller's evident intent spelled
    // sloppily, and it lands at /Game/X/Y. The assertion is deliberately on that outcome so this
    // goes RED, not fatal, if that sanitizer is ever swapped for one that merely normalizes
    // (strips trailing slashes, maps /Content to /Game) without collapsing - the exact shape found
    // in a sibling cluster of this same sweep.
    //
    // And if that day comes the answer flips to INVALID_ARGUMENT, never to a dead editor: the
    // composed path is validated by FPackageName::IsValidLongPackageName inside
    // PinWrightComposeAssetPackagePath (name branch) and again at the end of BuildCreatePaths
    // (both branches). A name-only character check would NOT have covered this, which is the whole
    // reason the shared helper validates the composed string as well as the bare leaf.
    {
        const FString FolderName = SafetyUniqueName(TEXT("DoubleSlashFolder"));
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(SafetyPackagePath(FolderName));
        };

        // Branch 1: folder + name. The "//" sits in the FOLDER; the leaf is impeccable.
        TSharedPtr<FJsonObject> FolderParams = MakeShared<FJsonObject>();
        FolderParams->SetStringField(TEXT("path"),
            TEXT("/Game/PinWrightTests//ChooserCreateNameSafety"));
        FolderParams->SetStringField(TEXT("name"), FolderName);
        FolderParams->SetStringField(TEXT("contextObjectType"), SafetyAbsentContextClass);

        FTestResponseCapture FolderCapture;
        InvokeHandlerWithCapture(TEXT("chooser.create"), FolderParams, FolderCapture);

        TestFalse(TEXT("the double-slash-folder case still fails on its context class"),
            FolderCapture.bSuccess);
        TestEqual(TEXT("a folder carrying '//' is collapsed before composition, so a bare name "
                       "reaches the class resolution instead of CreatePackage"),
            FolderCapture.ErrorCode, FString(TEXT("CLASS_NOT_FOUND")));
        TestNull(TEXT("the double-slash-folder case creates no chooser table"),
            FindObject<UChooserTable>(nullptr, *SafetyObjectPath(FolderName)));
    }

    {
        // Branch 2: `path` alone, no `name`, so the whole string is one caller-supplied package
        // path and NormalizePackagePath is the ONLY thing between it and CreatePackage. This
        // branch is untouched by the fix and is asserted so the sweep's other half stays closed.
        const FString WholePathName = SafetyUniqueName(TEXT("DoubleSlashWholePath"));
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(SafetyPackagePath(WholePathName));
        };

        TSharedPtr<FJsonObject> WholePathParams = MakeShared<FJsonObject>();
        WholePathParams->SetStringField(TEXT("path"),
            FString::Printf(TEXT("/Game/PinWrightTests//ChooserCreateNameSafety/%s"),
                *WholePathName));
        WholePathParams->SetStringField(TEXT("contextObjectType"), SafetyAbsentContextClass);

        FTestResponseCapture WholePathCapture;
        InvokeHandlerWithCapture(TEXT("chooser.create"), WholePathParams, WholePathCapture);

        TestFalse(TEXT("the double-slash whole-path case still fails on its context class"),
            WholePathCapture.bSuccess);
        TestEqual(TEXT("a whole `path` carrying '//' is collapsed and validated, never handed to "
                       "CreatePackage as written"),
            WholePathCapture.ErrorCode, FString(TEXT("CLASS_NOT_FOUND")));
        TestNull(TEXT("the double-slash whole-path case creates no chooser table"),
            FindObject<UChooserTable>(nullptr, *SafetyObjectPath(WholePathName)));
    }

    return true;
}
