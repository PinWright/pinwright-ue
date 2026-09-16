// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestWidgetCreatePackagePathSafety.cpp - regression coverage for the UI/widget cluster of
// B-createpackage-unvalidated-paths-plugin-wide: widget.create_widget_blueprint,
// editor.create_utility_widget, and the shared WidgetAuthoringHelpers::LoadWidgetBlueprint.
//
// WHAT WAS WRONG. The two verbs composed a package path from caller text and handed it to
// CreatePackage with nothing checking the result: they ran their `folder` through
// SanitizeProjectRelativePath and then concatenated the caller's `name` onto it RAW, so the guard
// read as if it worked while the argument that could end the process went unchecked. The shared
// loader is the same defect through the other door - it hands caller text straight to
// StaticLoadObject, which reaches the identical Fatal via
// ResolveName2(..., Create=true) -> CreatePackage. CreatePackage
// (UObjectGlobals.cpp:1094-1096) logs a package name containing "//" at **Fatal** - a verbosity
// that is not compiled out in any configuration - so `name: "a//b"` did not fail the call: it
// ended the editor PROCESS and every unsaved package in it. The handlers' own `if (!Package)`
// could never fire, because nothing after CreatePackage was reached. Measured once, with a
// callstack, on B-foliage-add-type-name-with-slash-kills-the-editor. There is a SECOND Fatal on
// the same function (:1118): ResolveName2 splits the name on '.', so `name: ".."` resolves to an
// empty package name and dies on its own line. Both are covered, because
// INVALID_OBJECTNAME_CHARACTERS carries '.' and ':' as well as '/'.
//
// WHY THIS TEST DOES NOT DRIVE THE CRASH, AND CANNOT ACCIDENTALLY DRIVE IT. A Fatal takes the test
// host down with it, so a test that reproduced the defect would abort the whole suite instead of
// reporting a red - and a suite that dies mid-queue is not a failure signal, it is the absence of
// one (the DID_NOT_COMPLETE state in the plugin's testing notes). Every assertion here is on the
// post-fix CONTRACT - the refusal - and each case is built so a build WITHOUT the fix takes a
// different, harmless path instead of the fatal one. There are three constructions, one per site:
//
//   * widget.create_widget_blueprint pairs every bad `name` with a `folder` that
//     SanitizeProjectRelativePath REJECTS ("/Game/../../Engine/Content"). A build without the fix
//     answers SECURITY_VIOLATION from the folder block - which sits ABOVE the concatenation - so
//     the TestEqual on INVALID_ARGUMENT goes red while the process lives. That depends on the
//     handler keeping its bare-name check ABOVE the folder block, and the handler carries a
//     comment saying so. The one case that cannot use that lever is the backslash, which survives
//     INVALID_OBJECTNAME_CHARACTERS and is caught by the package rule below the folder block; it
//     is driven with a VALID folder instead, and is safe to do so because a backslash composes no
//     "//" and no empty name - a reverted build creates an asset and fails the TestFalse rather
//     than dying.
//
//   * editor.create_utility_widget pairs every bad `name` with a well-formed `parentClass` that
//     names NO existing class (a fresh GUID). A build without the fix is refused
//     INVALID_PARENT_CLASS at the parent-class resolution - which sits ABOVE CreatePackage - so
//     the TestEqual on INVALID_ARGUMENT goes red while the process lives. That depends on the
//     handler keeping its path check ABOVE the parent-class block; the handler says so too.
//
//   * WidgetAuthoringHelpers::LoadWidgetBlueprint is called directly rather than through a verb,
//     and IS driven with a "//" - the one case in this file that is. Its guard is exactly
//     Contains("//"), so no non-lethal input discriminates a build with it from one without, and
//     the test therefore asserts the post-fix contract rather than reproducing the defect. That
//     case carries its own header saying so; do not generalise it to the two verbs above, whose
//     guards are the broad engine package rule and whose cases must stay non-lethal.
//
//     (This bullet used to describe WidgetAuthoringHelpers::CreateAssetPackage. That helper was
//     deleted along with its test: re-measured 2026-08-31, it had NO callers anywhere under
//     Source/ - including the gated sub-modules - so it was dead code carrying a guard for a
//     caller that never arrived. The identically named CreateAssetPackage in
//     BlueprintTypeDefinitionHandler.cpp is a separate file-local static and is unaffected.)
//
// Do NOT "improve" any case by supplying the missing half, and do NOT rewrite one into a crash
// expectation: either change hands a live editor a string that ends it.
//
// EVERY REFUSAL CASE IS PAIRED WITH A CONTROL. Without one, a handler that refused every name
// would satisfy every case above. The controls create a real asset with a bare name, which is
// also what proves the guard sits above CreatePackage rather than replacing it.
//
// AND WITH AN OVER-REFUSAL PIN, because the fix changed the composition. PinWrightComposeAssetPackagePath
// now joins with FString::operator/, whose PathAppend (Core/Private/Containers/String.cpp.inl:855-885)
// ABSORBS one separator - it pops the left side's terminator when the folder already ends in '/'
// rather than adding a second one - so no caller needs a trailing-slash trim of its own. (It joined
// with Printf("%s/%s"), which doubled that separator, for exactly one wave; the per-handler trims
// that existed to undo it are being removed with it.) The pins assert that a trailing-slash folder
// and an interior-"//" folder are still ACCEPTED - the second because
// SanitizeProjectRelativePath collapses "//" in a loop, which is what makes `folder` a
// non-hazardous argument at these two sites even though it is a live one elsewhere in the sweep.
//
// PLACEMENT, against the other door to the same Fatal. StaticLoadObjectInternal calls
// ResolveName2(..., Create=true) (UObjectGlobals.cpp:1427), and ResolveName2 calls CreatePackage
// (:1310) - so a LoadObject / LoadClass / LoadPackage on an unvalidated path kills the process just
// as CreatePackage does, and a guard placed below one is not a guard. Checked on all three sites:
// none of them loads the COMPOSED path before creating it (the existence check is absent here, and
// FindObject, used by CleanupTestAsset, is Create=false at :620), and each guard is above every
// intervening call. The one nearby load is editor.create_utility_widget's LoadClass on the separate
// `parentClass` argument, which every case here supplies as a "//"-free GUID path precisely so the
// lever cannot become the crash.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses, which also validates each payload
// against the declared ParamSpec.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units. The
// widget cluster already consolidates its shared test helpers this way (WidgetXmlTestHelpers,
// LiveUiSnapshotTestHelpers).
namespace WidgetCreatePackagePathSafetyHelpers
{
    // Scratch destination for the controls. Not /Game/UI, so a control that landed in the verb's
    // default folder instead of the requested one is visible.
    constexpr const TCHAR* SafetyScratchFolder = TEXT("/Game/PinWrightTests/WidgetCreatePathSafety");

    // A folder SanitizeProjectRelativePath rejects. This is the load-bearing half of the no-crash
    // guarantee for widget.create_widget_blueprint: it makes a PRE-FIX handler bail at its folder
    // check, above the concatenation, instead of reaching CreatePackage.
    constexpr const TCHAR* SafetyRejectedFolder = TEXT("/Game/../../Engine/Content");

    inline FString SafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWWidgetPathSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // A well-formed class path that names no existing class. This is the load-bearing half of the
    // no-crash guarantee for editor.create_utility_widget: LoadClass returns null and the handler
    // answers INVALID_PARENT_CLASS, above CreatePackage, on a build without the fix.
    inline FString SafetyAbsentParentClass()
    {
        const FString Stem = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        return FString::Printf(TEXT("/Game/PinWrightMissing/BP_AbsentUtilityWidget_%s.BP_AbsentUtilityWidget_%s_C"),
            *Stem, *Stem);
    }

    inline FString SafetyPackagePath(const TCHAR* Folder, const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s"), Folder, *Name);
    }

    inline FString SafetyObjectPath(const TCHAR* Folder, const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s.%s"), Folder, *Name, *Name);
    }

    // Drives one refusal case and reports the two facts that separate the post-fix contract from
    // the pre-fix behaviour: it is refused, and it is refused as a CALLER ARGUMENT error rather
    // than as the unrelated failure a reverted build would report. The message check is what makes
    // the refusal actionable - the caller is told which value to change.
    inline void ExpectRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const TCHAR* Method, const TCHAR* RequestId,
        const TSharedPtr<FJsonObject>& Params, const FString& BadName, const TCHAR* Label)
    {
        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method, RequestId, Params,
            bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(TEXT("%s: a %s name ('%s') is refused"), Method, Label,
            *BadName), bSuccess);
        Test.TestEqual(*FString::Printf(
            TEXT("%s: a %s name is refused as a caller argument error"), Method, Label),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(TEXT("%s: the %s refusal quotes the offending name"),
            Method, Label), Sink->Message.Contains(BadName));
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest below opens the helper namespace inside its own body rather than at file scope: a
// file-scope using-directive would leak into every other test .cpp that Unity merges after this
// one into the same translation unit.

// ============================================================================
// widget.create_widget_blueprint refuses a path-shaped `name` instead of composing it into
// CreatePackage
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetCreateWidgetBlueprintNameCarryingAPathIsRefusedTest,
    "PinWright.widget.create_widget_blueprint.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetCreateWidgetBlueprintNameCarryingAPathIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace WidgetCreatePackagePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // Cases refused by the engine's object-naming rule, which the handler applies ABOVE the folder
    // block. Each is driven with the rejected folder, so a build without that check answers
    // SECURITY_VIOLATION instead of reaching the concatenation.
    const TCHAR* NameRuleCases[] = {
        // The killer input: "//" INSIDE the name survives every composition style and is what
        // CreatePackage logs at Fatal.
        TEXT("a//b"),
        // A rooted path. FString::operator/ does not double here (PathAppend pops the terminator),
        // so this one writes to a package the caller never named rather than crashing - the same
        // argument confusion one step short of the crash.
        TEXT("/Game/PinWrightScratch/PWScratch"),
        TEXT("Sub/Leaf"),
        TEXT("../Escape"),
        TEXT("Trailing/"),
        // CreatePackage's OTHER Fatal (:1118): ResolveName2 splits on '.', so a name of ".."
        // resolves to an empty package name and dies on a different line than the "//" case.
        // INVALID_OBJECTNAME_CHARACTERS carries '.' as well as '/', so one check covers both.
        TEXT(".."),
    };
    const TCHAR* NameRuleLabels[] = {
        TEXT("double-slash"), TEXT("rooted path"), TEXT("interior slash"),
        TEXT("traversal"), TEXT("trailing slash"), TEXT("dot-dot"),
    };
    static_assert(UE_ARRAY_COUNT(NameRuleCases) == UE_ARRAY_COUNT(NameRuleLabels),
        "each case needs a label");
    const int32 NumNameRuleCases = static_cast<int32>(UE_ARRAY_COUNT(NameRuleCases));

    for (int32 Index = 0; Index < NumNameRuleCases; ++Index)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), NameRuleCases[Index]);
        Params->SetStringField(TEXT("folder"), SafetyRejectedFolder);
        ExpectRefused(*this, Dispatcher, Sink, TEXT("widget.create_widget_blueprint"),
            TEXT("req-widget-create-name-safety"), Params, FString(NameRuleCases[Index]),
            NameRuleLabels[Index]);
    }

    // A backslash is NOT in INVALID_OBJECTNAME_CHARACTERS but IS in
    // INVALID_LONGPACKAGE_CHARACTERS, so this case is what proves the COMPOSED PATH is checked
    // against the engine's package rule as well as the name against its object rule. It is driven
    // with a valid folder because it is caught below the folder block - and that is safe: a
    // backslash composes neither "//" nor an empty name, so a reverted build creates an asset and
    // fails the TestFalse rather than ending the process.
    {
        const FString BackslashName = TEXT("Sub\\Leaf");
        ON_SCOPE_EXIT
        {
            // Only a reverted build gets this far; a fixed build creates nothing.
            CleanupTestAsset(SafetyPackagePath(SafetyScratchFolder, BackslashName));
        };

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), BackslashName);
        Params->SetStringField(TEXT("folder"), SafetyScratchFolder);
        ExpectRefused(*this, Dispatcher, Sink, TEXT("widget.create_widget_blueprint"),
            TEXT("req-widget-create-name-backslash"), Params, BackslashName, TEXT("backslash"));
    }

    // ORDERING WITNESS. A valid name with the same rejected folder must still be refused by the
    // folder guard - the name check was inserted above it, not in place of it.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), SafetyUniqueName(TEXT("Folder")));
        Params->SetStringField(TEXT("folder"), SafetyRejectedFolder);

        bool bSuccess = true;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("widget.create_widget_blueprint"),
            TEXT("req-widget-create-folder-traversal"), Params, bSuccess, ErrorCode);

        TestFalse(TEXT("a folder containing '..' is still refused"), bSuccess);
        TestEqual(TEXT("the folder guard still answers SECURITY_VIOLATION when the name is fine"),
            ErrorCode, FString(TEXT("SECURITY_VIOLATION")));
    }

    // A DOUBLED SLASH IN THE FOLDER IS NOW REFUSED, AND THAT IS A DELIBERATE NARROWING.
    // This shape used to be ACCEPTED here and pinned as such: SanitizeProjectRelativePath collapsed
    // the "//" in a loop, so the canonical destination was created and nothing reached CreatePackage
    // malformed. The dispatch-boundary wave made `folder` a declared `path` param, and a `path`
    // carrying "//" is refused by the gate before the handler runs. Silently collapsing a malformed
    // separator was never a documented contract - it was a side effect of the normalizer - and the
    // refusal is the behaviour the wave exists to produce, so the pin is inverted rather than the
    // gate exempted. Driven on both builds identically; nothing here can crash a reverted build,
    // which would simply collapse it and succeed, turning this case red.
    {
        const FString RefusedName = SafetyUniqueName(TEXT("FolderDoubleSlash"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), RefusedName);
        Params->SetStringField(TEXT("folder"), TEXT("/Game/PinWrightTests//WidgetCreatePathSafety"));

        bool bSuccess = true;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("widget.create_widget_blueprint"),
            TEXT("req-widget-create-folder-double-slash"), Params, bSuccess, ErrorCode);

        TestFalse(TEXT("a folder containing '//' is refused"), bSuccess);
        TestEqual(TEXT("the doubled-slash folder is refused by the dispatch gate"),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(TEXT("the refusal quotes the offending folder"),
            Sink->Message.Contains(TEXT("/Game/PinWrightTests//WidgetCreatePathSafety")));
        // Nothing was created under the collapsed spelling either.
        TestFalse(TEXT("no asset was created at the canonical destination"),
            UEditorAssetLibrary::DoesAssetExist(
                SafetyObjectPath(SafetyScratchFolder, RefusedName)));
    }

    // OVER-REFUSAL PINS. The guard added a composition step this handler did not have before, so a
    // folder shape that works today must be proven to still work, or the fix trades a crash for a
    // regression. (The checker's join has since moved from Printf("%s/%s") to FString::operator/,
    // which absorbs a trailing separator rather than doubling it - strictly widening, so these pins
    // hold either way and are what demonstrate that.)
    {
        const TCHAR* AcceptedFolders[] = {
            // A trailing slash, which the sanitizer preserves and the handler pops.
            TEXT("/Game/PinWrightTests/WidgetCreatePathSafety/"),
        };
        const int32 NumAcceptedFolders = static_cast<int32>(UE_ARRAY_COUNT(AcceptedFolders));

        for (int32 Index = 0; Index < NumAcceptedFolders; ++Index)
        {
            const FString AcceptedName = SafetyUniqueName(TEXT("Folder"));
            ON_SCOPE_EXIT
            {
                CleanupTestAsset(SafetyPackagePath(SafetyScratchFolder, AcceptedName));
            };

            TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
            Params->SetStringField(TEXT("name"), AcceptedName);
            Params->SetStringField(TEXT("folder"), AcceptedFolders[Index]);

            bool bSuccess = false;
            FString ErrorCode;
            Dispatch(Dispatcher, Sink, TEXT("widget.create_widget_blueprint"),
                TEXT("req-widget-create-folder-shape"), Params, bSuccess, ErrorCode);

            TestTrue(*FString::Printf(TEXT("folder '%s' is still accepted (error=%s: %s)"),
                AcceptedFolders[Index], *ErrorCode, *Sink->Message), bSuccess);
            TestTrue(*FString::Printf(
                TEXT("folder '%s' resolves to the canonical destination"), AcceptedFolders[Index]),
                UEditorAssetLibrary::DoesAssetExist(
                    SafetyObjectPath(SafetyScratchFolder, AcceptedName)));
        }
    }

    // CONTROL. Without this, a handler that refused every name would satisfy every case above.
    {
        const FString ControlName = SafetyUniqueName(TEXT("Control"));
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(SafetyPackagePath(SafetyScratchFolder, ControlName));
        };

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), ControlName);
        Params->SetStringField(TEXT("folder"), SafetyScratchFolder);

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("widget.create_widget_blueprint"),
            TEXT("req-widget-create-control"), Params, bSuccess, ErrorCode);

        TestTrue(*FString::Printf(TEXT("a bare name still creates the asset (error=%s: %s)"),
            *ErrorCode, *Sink->Message), bSuccess);
        TestTrue(TEXT("the control asset is addressable under the requested folder"),
            UEditorAssetLibrary::DoesAssetExist(SafetyObjectPath(SafetyScratchFolder, ControlName)));
    }

    return true;
}

// ============================================================================
// editor.create_utility_widget refuses a path-shaped `name` instead of composing it into
// CreatePackage
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorCreateUtilityWidgetNameCarryingAPathIsRefusedTest,
    "PinWright.editor.create_utility_widget.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorCreateUtilityWidgetNameCarryingAPathIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace WidgetCreatePackagePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // Every case here can use the parentClass lever, so the backslash rides along with the rest.
    const TCHAR* BadNames[] = {
        TEXT("a//b"),
        TEXT("/Game/PinWrightScratch/PWScratch"),
        TEXT("Sub/Leaf"),
        TEXT("Sub\\Leaf"),
        TEXT("../Escape"),
        TEXT("Trailing/"),
        // CreatePackage's other Fatal (:1118) - see the sibling test above.
        TEXT(".."),
    };
    const TCHAR* Labels[] = {
        TEXT("double-slash"), TEXT("rooted path"), TEXT("interior slash"),
        TEXT("backslash"), TEXT("traversal"), TEXT("trailing slash"), TEXT("dot-dot"),
    };
    static_assert(UE_ARRAY_COUNT(BadNames) == UE_ARRAY_COUNT(Labels), "each case needs a label");
    const int32 NumBadNames = static_cast<int32>(UE_ARRAY_COUNT(BadNames));

    for (int32 Index = 0; Index < NumBadNames; ++Index)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), BadNames[Index]);
        Params->SetStringField(TEXT("folder"), SafetyScratchFolder);
        // The discriminator against a reverted fix: pre-fix this same payload answers
        // INVALID_PARENT_CLASS from the parent-class resolution, never INVALID_ARGUMENT.
        Params->SetStringField(TEXT("parentClass"), SafetyAbsentParentClass());
        ExpectRefused(*this, Dispatcher, Sink, TEXT("editor.create_utility_widget"),
            TEXT("req-editor-create-utility-widget-name-safety"), Params,
            FString(BadNames[Index]), Labels[Index]);
    }

    // CONTROL, driven without parentClass so it exercises the default UEditorUtilityWidget parent
    // and actually reaches CreatePackage with a bare name.
    {
        const FString ControlName = SafetyUniqueName(TEXT("UtilControl"));
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(SafetyPackagePath(SafetyScratchFolder, ControlName));
        };

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), ControlName);
        Params->SetStringField(TEXT("folder"), SafetyScratchFolder);

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("editor.create_utility_widget"),
            TEXT("req-editor-create-utility-widget-control"), Params, bSuccess, ErrorCode);

        TestTrue(*FString::Printf(TEXT("a bare name still creates the asset (error=%s: %s)"),
            *ErrorCode, *Sink->Message), bSuccess);
        TestTrue(TEXT("the control asset is addressable under the requested folder"),
            UEditorAssetLibrary::DoesAssetExist(SafetyObjectPath(SafetyScratchFolder, ControlName)));
    }

    // OVER-REFUSAL PIN. A folder shape that works today must still work. The checker now joins with
    // FString::operator/, which absorbs a trailing separator, so this holds whether or not the
    // handler keeps its own trailing-slash pop.
    {
        const FString TrailingName = SafetyUniqueName(TEXT("UtilFolder"));
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(SafetyPackagePath(SafetyScratchFolder, TrailingName));
        };

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), TrailingName);
        Params->SetStringField(TEXT("folder"),
            FString::Printf(TEXT("%s/"), SafetyScratchFolder));

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("editor.create_utility_widget"),
            TEXT("req-editor-create-utility-widget-folder-shape"), Params, bSuccess, ErrorCode);

        TestTrue(*FString::Printf(TEXT("a trailing-slash folder is still accepted (error=%s: %s)"),
            *ErrorCode, *Sink->Message), bSuccess);
        TestTrue(TEXT("a trailing-slash folder resolves to the canonical destination"),
            UEditorAssetLibrary::DoesAssetExist(SafetyObjectPath(SafetyScratchFolder, TrailingName)));
    }

    return true;
}

// ============================================================================
// The shared widget loader refuses a "//" path instead of handing it to StaticLoadObject
// ============================================================================
// THIS CASE DOES CARRY A "//", UNLIKE EVERY OTHER CASE IN THIS FILE, and that is deliberate
// rather than an oversight of the header's warning above. The two verbs' guards are the engine's
// broad package rule, so a backslash or a trailing slash discriminates them without being lethal;
// LoadWidgetBlueprint's guard is exactly `Contains("//")` and NOTHING ELSE, because it sits over a
// resolver that legitimately accepts a bare name, a package path, an object path and a subobject
// path (see CanReachCreatePackageFatal's contract in Utils/PathUtils.h). There is therefore no
// non-lethal input that a build without the guard answers differently, and a case built from one
// would assert nothing.
//
// So this test asserts the post-fix CONTRACT and is NOT a reproduction: on a build with the guard
// reverted this input reaches StaticLoadObject -> ResolveName2(..., Create=true) -> CreatePackage
// and ends the process rather than going red. That is the same trade the plan's precedent test
// (Tests/Sequencer/TestSequencerExportAnimSequencePathSafety.cpp) documents for a guard over a
// load. It is called DIRECTLY, never through a widget verb, because a verb has other unguarded
// loads on its own arguments and driving one would add crash surface without adding coverage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetAuthoringUtilsLoadWidgetBlueprintRefusesDoubleSlashTest,
    "PinWright.widget.authoring_utils.LoadWidgetBlueprintRefusesDoubleSlash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FWidgetAuthoringUtilsLoadWidgetBlueprintRefusesDoubleSlashTest::RunTest(const FString& Parameters)
{
    using namespace WidgetCreatePackagePathSafetyHelpers;

    // Every shape the resolver accepts, each carrying the one lethal property. The bare-name case
    // matters most: it has no leading slash and no dot, and is still a kill, because
    // StaticLoadObjectInternal re-enters itself with InName + "." + GetShortName(InName).
    const TCHAR* LethalPaths[] = {
        TEXT("/Game//PinWrightMissing/WBP_PathSafety"),
        TEXT("/Game/PinWrightMissing//WBP_PathSafety"),
        TEXT("/Game//PinWrightMissing/WBP_PathSafety.WBP_PathSafety"),
        TEXT("PinWrightMissing//WBP_PathSafety"),
    };
    for (const TCHAR* LethalPath : LethalPaths)
    {
        TestNull(*FString::Printf(TEXT("'%s' is refused before any load"), LethalPath),
            WidgetAuthoringHelpers::LoadWidgetBlueprint(LethalPath));
    }

    // CONTROL, and it has to be a REAL asset. Refusal and not-found are the same nullptr here, so
    // a well-formed path that resolves to nothing would satisfy the cases above just as well - only
    // a load that SUCCEEDS proves the guard did not swallow the whole function. The asset is
    // created through the verb (the same one this file already exercises) and torn down after.
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        MakeDispatcher(Sink, Dispatcher);

        const FString ControlName = SafetyUniqueName(TEXT("Load"));
        const FString ControlPath = SafetyPackagePath(SafetyScratchFolder, ControlName);
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(ControlPath);
        };

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), ControlName);
        Params->SetStringField(TEXT("folder"), SafetyScratchFolder);

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("widget.create_widget_blueprint"),
            TEXT("req-widget-load-control"), Params, bSuccess, ErrorCode);

        if (TestTrue(*FString::Printf(TEXT("the control asset was created (error=%s: %s)"),
                *ErrorCode, *Sink->Message), bSuccess))
        {
            TestNotNull(TEXT("a well-formed path still loads the widget blueprint"),
                WidgetAuthoringHelpers::LoadWidgetBlueprint(ControlPath));
        }
    }

    return true;
}
