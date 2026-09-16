// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestAiCreateAssetNamePathSafety.cpp - regression coverage for the four AIHandler.cpp entries on
// board B-createpackage-unvalidated-paths-plugin-wide.
//
// WHAT WAS WRONG. Four ai.* verbs composed a package path out of caller text and handed it to
// CreatePackage with no engine check:
//
//   ai.create_blackboard_asset         folder sanitised by SanitizeAIAssetPath, `name` concatenated
//                                      onto it RAW - a guard that read as if it were working
//   ai.create_state_tree               `path / name`, BOTH arguments raw off Ctx.GetString
//   ai.create_smart_object_definition  same
//   ai.create_mass_entity_config       same
//
// CreatePackage (UObjectGlobals.cpp:1086-1120) logs at Fatal for a name containing "//"
// (:1094-1096) and for one that resolves to empty (:1118). Fatal is not compiled out in any
// configuration, so the call does not fail: the editor PROCESS dies, taking every unsaved package
// in it - measured on B-foliage-add-type-name-with-slash-kills-the-editor. `name: "a//b"` was a
// one-argument editor kill at all four verbs, and the `if (!Package)` branch each one carries
// could never fire, because nothing after CreatePackage runs.
//
// WHY THIS TEST NEVER DRIVES THE FATAL, ON A FIXED OR A REVERTED BUILD. A Fatal takes the suite
// host down with it, so a test that reproduced the defect would END the run rather than report a
// red - and a run that dies mid-queue is an absence of a signal, not a failure signal (the
// DID_NOT_COMPLETE state in the plugin's testing notes). Three properties keep every case here
// away from the Fatal. None is decoration.
//
//  1. NO CASE BELOW SENDS A NAME CONTAINING "//", AND NONE NEEDS TO. '/' is a member of
//     INVALID_OBJECTNAME_CHARACTERS (Core/Public/UObject/NameTypes.h:191), so FName::IsValidXName -
//     the first half of the guard - refuses "Sub/Leaf" and "a//b" by the SAME predicate for the
//     SAME reason. Driving the survivable member of that class proves the lethal one is refused
//     without ever handing a live editor the string that ends it. DO NOT "complete the coverage"
//     by adding a "//" case here: at three of these four verbs nothing argument-driven sits above
//     the concatenation, so such a case is unconditionally fatal on a reverted build.
//
//  2. EVERY REFUSAL CASE PAIRS ITS BAD NAME WITH AN UNMOUNTED FOLDER (SafetyUnmountedFolder). For
//     ai.create_blackboard_asset that makes CreatePackage strictly UNREACHABLE on a reverted
//     build: the pre-fix path ran SanitizeAIAssetPath on the folder first and its IsValidMountPoint
//     check refuses an unmounted root, so the call is answered CREATION_FAILED ABOVE the
//     concatenation and the TestEqual on INVALID_ARGUMENT below goes red while the process lives.
//     That depends on the fix keeping its composition check ABOVE CreateBlackboardAsset, and the
//     handler carries a comment saying so. Do NOT "improve" these cases by supplying a mounted
//     folder - that removes the only thing standing between a reverted build and the Fatal here.
//
//  3. The other three verbs have nothing argument-driven above their concatenation, so on a
//     REVERTED build CreatePackage IS reached - deliberately, with an argument that is provably
//     not fatal. Each composed candidate is "<unmounted folder>/<bad name>": none contains "//"
//     (FString::operator/ appends without doubling - PathAppend,
//     Core/Private/Containers/String.cpp.inl:855-885 - and no name here contains "//" per property
//     1), and none is empty. Neither Fatal branch is entered - and the SECOND one matters as much
//     as the first: CreatePackage also logs Fatal when the name resolves to EMPTY (:1118), which a
//     '.' can produce because ResolveName2 splits the string on '.' and reassigns the remainder.
//     The four shared names are therefore dot-free, so that loop returns on its first iteration
//     (UObjectGlobals.cpp:1236-1240) and cannot shorten anything; the one dotted case
//     ("../Escape") is driven ONLY at ai.create_blackboard_asset, where property 2 means
//     CreatePackage is never called at all. The unmounted root additionally makes the follow-up
//     save a no-op, so a reverted build leaves nothing on disk - it answers success or a
//     downstream failure, neither of which is INVALID_ARGUMENT, and the test goes red.
//
// WHAT ELSE IS ASSERTED, AND WHY IT BELONGS IN THE SAME FILE. Each verb also runs a CONTROL: a
// bare name with a real mounted folder must NOT be refused as a caller argument. Without it a fix
// that refused every name would satisfy every refusal case above. The control runs FIRST because
// it doubles as the host probe - three of these verbs sit behind an optional engine plugin or a
// header gate, and on a host without it the refusal cases would be answered by that gate rather
// than by the guard under test, which is a skip, not a pass.

#include "Misc/AutomationTest.h"
#include "Containers/ArrayView.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace AiCreateAssetNamePathSafetyHelpers
{
    // A well-formed long-package spelling under a root nothing mounts. Load-bearing twice over -
    // see properties 2 and 3 of the file header.
    constexpr const TCHAR* SafetyUnmountedFolder = TEXT("/PinWrightMissingRoot/AiNameSafety");

    // Real and mounted, used only by the controls.
    constexpr const TCHAR* SafetyMountedFolder = TEXT("/Game/__PW_GatewayTests/AiNameSafety");

    struct FSafetyBadName
    {
        const TCHAR* Name;
        const TCHAR* Label;
    };

    // The shapes driven at all four verbs. Dot-free by construction (property 3). The first three
    // are refused by the bare-name half of the guard because '/' is in
    // INVALID_OBJECTNAME_CHARACTERS; the backslash case is what proves the COMPOSED-PATH half runs
    // too, since '\' is NOT in INVALID_OBJECTNAME_CHARACTERS but IS in
    // INVALID_LONGPACKAGE_CHARACTERS.
    inline TArrayView<const FSafetyBadName> SafetySharedBadNames()
    {
        static const FSafetyBadName Names[] = {
            { TEXT("/Game/PinWrightScratch/PWAiNameSafety"), TEXT("rooted path") },
            { TEXT("Sub/Leaf"),                              TEXT("interior slash") },
            { TEXT("Trailing/"),                             TEXT("trailing slash") },
            { TEXT("Sub\\Leaf"),                             TEXT("backslash") },
        };
        return MakeArrayView(Names);
    }

    inline FString SafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWAiNameSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Drives one refusal case and reports the facts that separate the post-fix contract from the
    // pre-fix behaviour: it is refused, it is refused as a CALLER ARGUMENT rather than as a
    // creation failure, the message quotes the offending value so the caller can act, and no
    // package was left behind at the path the pre-fix build would have composed.
    inline void ExpectNameRefused(FAutomationTestBase& Test, const TCHAR* Method,
        const TCHAR* BadName, const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), BadName);
        Payload->SetStringField(TEXT("path"), SafetyUnmountedFolder);

        FTestResponseCapture Capture;
        if (!Test.TestTrue(*FString::Printf(TEXT("%s handler registered"), Method),
                InvokeHandlerWithCapture(Method, Payload, Capture)))
        {
            return;
        }

        Test.TestFalse(*FString::Printf(TEXT("%s refuses a %s name ('%s')"), Method, Label, BadName),
            Capture.bSuccess);
        // The discriminator against a reverted fix. At ai.create_blackboard_asset the pre-fix path
        // answers CREATION_FAILED from the folder sanitiser; at the other three it answers success
        // or a downstream failure. Never INVALID_ARGUMENT.
        Test.TestEqual(*FString::Printf(
            TEXT("%s refuses a %s name as a caller argument, not as a creation failure"),
            Method, Label), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(TEXT("the %s %s refusal quotes the offending value"),
            Method, Label), Capture.Message.Contains(BadName));
        // A refusal that still left a half-built package behind would be the same data hazard in
        // slower motion, and this is the exact string the pre-fix build handed to CreatePackage.
        // Skipped for a dotted name: FindObject would route through ResolveName2's package-
        // splitting walk, which can log, and a log warning raised inside a running automation test
        // is elevated to an error by default (bElevateLogWarningsToErrors). The only dotted case
        // is the traversal one, driven at the single verb where a pre-fix build never reached
        // CreatePackage anyway, so nothing is lost.
        if (!FCString::Strchr(BadName, TEXT('.')))
        {
            const FString PreFixCandidate = FString(SafetyUnmountedFolder) / FString(BadName);
            Test.TestNull(*FString::Printf(TEXT("the %s %s refusal creates no package"),
                Method, Label), FindObject<UPackage>(nullptr, *PreFixCandidate));
        }
    }

    // The whole per-verb body. Shared because the four verbs differ only in their method name and
    // in whether the control can demand outright success: ai.create_blackboard_asset has no gate
    // and must succeed, while the other three sit behind an optional engine plugin or a header
    // gate that may legitimately answer something else on a given host.
    inline void RunNamePathSafetyCases(FAutomationTestBase& Test, const TCHAR* Method,
        bool bRequireControlSuccess)
    {
        const FString ControlName = SafetyUniqueName(TEXT("Control"));
        const FString ControlPackagePath = FString(SafetyMountedFolder) / ControlName;
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(ControlPackagePath);
        };

        TSharedPtr<FJsonObject> ControlPayload = MakeShared<FJsonObject>();
        ControlPayload->SetStringField(TEXT("name"), ControlName);
        ControlPayload->SetStringField(TEXT("path"), SafetyMountedFolder);

        FTestResponseCapture Control;
        if (!Test.TestTrue(*FString::Printf(TEXT("%s handler registered"), Method),
                InvokeHandlerWithCapture(Method, ControlPayload, Control)))
        {
            return;
        }

        // Host probe, run before anything is asserted: a verb answered by its plugin gate never
        // reaches the guard under test, so measuring it here would measure the gate.
        if (!Control.bSuccess && Control.ErrorCode == TEXT("PLUGIN_DISABLED"))
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("optional-plugin-not-shipped"),
                FString::Printf(TEXT("%s answered PLUGIN_DISABLED on this host, so its package-path "
                    "guard is unreachable here."), Method));
            return;
        }
        bool bHeadersUnavailable = false;
        if (Control.Result.IsValid() &&
            Control.Result->TryGetBoolField(TEXT("headersUnavailable"), bHeadersUnavailable) &&
            bHeadersUnavailable)
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("optional-plugin-not-shipped"),
                FString::Printf(TEXT("%s reports headersUnavailable on this host, so it never "
                    "composes a package path here."), Method));
            return;
        }

        Test.TestNotEqual(*FString::Printf(
            TEXT("%s does not refuse a BARE name - the guard is not a blanket refusal"), Method),
            Control.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        if (bRequireControlSuccess)
        {
            Test.TestTrue(*FString::Printf(
                TEXT("%s accepts a bare name in a mounted folder (error=%s: %s)"),
                Method, *Control.ErrorCode, *Control.Message), Control.bSuccess);
        }

        for (const FSafetyBadName& Bad : SafetySharedBadNames())
        {
            ExpectNameRefused(Test, Method, Bad.Name, Bad.Label);
        }
    }
}

// Each RunTest below opens the helper namespace inside its own body rather than at file scope: a
// file-scope using-directive would leak into every other test .cpp that Unity merges after this
// one into the same translation unit.

// ============================================================================
// ai.create_blackboard_asset - the "folder guarded, caller's NAME unchecked" member of the class
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAiCreateBlackboardAssetNamePathSafetyTest,
    "PinWright.ai.create_blackboard_asset.NamePathSafety",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAiCreateBlackboardAssetNamePathSafetyTest::RunTest(const FString& Parameters)
{
    using namespace AiCreateAssetNamePathSafetyHelpers;

    // No plugin gate and no header gate on this verb, so the control must actually succeed.
    RunNamePathSafetyCases(*this, TEXT("ai.create_blackboard_asset"),
        /*bRequireControlSuccess=*/true);

    // Traversal, driven only here, and only because this is the one verb where a reverted build
    // cannot reach CreatePackage at all (SanitizeAIAssetPath refuses the unmounted folder above
    // the concatenation). A '.' puts ResolveName2's splitting loop in play, which is the road to
    // CreatePackage's SECOND Fatal - the empty resolved name at UObjectGlobals.cpp:1118 - so this
    // case must not be copied to the other three verbs. The guard covers it because
    // INVALID_OBJECTNAME_CHARACTERS contains '.' and ':' as well as '/'; a hand-rolled filter that
    // only blocked slashes would not.
    ExpectNameRefused(*this, TEXT("ai.create_blackboard_asset"), TEXT("../Escape"),
        TEXT("traversal"));

    return true;
}

// ============================================================================
// ai.create_state_tree - `path / name`, both arguments raw
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAiCreateStateTreeNamePathSafetyTest,
    "PinWright.ai.create_state_tree.NamePathSafety",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAiCreateStateTreeNamePathSafetyTest::RunTest(const FString& Parameters)
{
    using namespace AiCreateAssetNamePathSafetyHelpers;

    // The verb fake-succeeds with headersUnavailable=true when the StateTree headers are absent,
    // which the shared driver treats as a skip rather than as a pass.
    RunNamePathSafetyCases(*this, TEXT("ai.create_state_tree"), /*bRequireControlSuccess=*/false);
    return true;
}

// ============================================================================
// ai.create_smart_object_definition - `path / name`, both arguments raw
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAiCreateSmartObjectDefinitionNamePathSafetyTest,
    "PinWright.ai.create_smart_object_definition.NamePathSafety",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAiCreateSmartObjectDefinitionNamePathSafetyTest::RunTest(const FString& Parameters)
{
    using namespace AiCreateAssetNamePathSafetyHelpers;

    // Reflection-only against the optional SmartObjects plugin; PLUGIN_DISABLED is a skip.
    RunNamePathSafetyCases(*this, TEXT("ai.create_smart_object_definition"),
        /*bRequireControlSuccess=*/false);
    return true;
}

// ============================================================================
// ai.create_mass_entity_config - `path / name`, both arguments raw
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAiCreateMassEntityConfigNamePathSafetyTest,
    "PinWright.ai.create_mass_entity_config.NamePathSafety",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAiCreateMassEntityConfigNamePathSafetyTest::RunTest(const FString& Parameters)
{
    using namespace AiCreateAssetNamePathSafetyHelpers;

    // Reflection-only against the optional MassGameplay plugin; PLUGIN_DISABLED is a skip.
    RunNamePathSafetyCases(*this, TEXT("ai.create_mass_entity_config"),
        /*bRequireControlSuccess=*/false);
    return true;
}
