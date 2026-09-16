// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestAudioCreatePackagePathSafety.cpp - regression coverage for the AUDIO cluster of
// B-createpackage-unvalidated-paths-plugin-wide (15 sites: 13 in Handlers/Audio/
// AudioAuthoringHandler.cpp, 2 in Handlers/Audio/MetaSound/MetaSoundPatchPresetHandler.cpp).
//
// WHAT WAS WRONG. Every audio create verb read a `path` folder and a bare `name`, wrote
// `Path / Name`, and handed the result to CreatePackage with no validation of the name at all.
// CreatePackage (UObjectGlobals.cpp:1086-1120) logs at **Fatal** for a name containing "//"
// (:1094-1096) and for one that resolves to empty (:1118) - a verbosity that is not compiled out
// in any configuration. So such a call did not fail: the editor PROCESS died, taking every unsaved
// package in it. The handlers' own `if (!Package)` could never fire, because nothing after
// CreatePackage was reached. Note that FString::operator/ does NOT double a leading slash
// (PathAppend, Core/Private/Containers/String.cpp.inl:855-885), so these sites were not the
// doubling composition - but a `name` that itself carries "//" is a one-argument kill regardless,
// which is the input driven below.
//
// ONE GUARD, NOT FIFTEEN, AND WHY ONE TEST COVERS THEM. All 15 sites read the same two arguments
// and composed them the same way, so they were converted to one shared helper,
// PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse (Handlers/Audio/
// AudioPackagePathGuard.h). This file therefore asserts three separate things:
//
//   1. NameCarryingAPathIsRefused drives the guard END TO END through the real dispatcher on one
//      verb, audio.authoring.create_sound_concurrency, and proves the refusal contract plus a
//      valid-input control.
//   2. EveryCreatePackageIsGuarded is a SOURCE SCAN proving that all 15 sites - not just the one
//      driven above - go through that same helper, and that the old unguarded idiom is gone. It
//      is what makes "the audio cluster is guarded" a property of the cluster rather than a
//      snapshot of whichever site happened to be tested. A sixteenth verb added with the old
//      two-line idiom turns it red.
//   3. TrailingSlashFolderComposesWithoutDoubling exercises the helper DIRECTLY, because a shared
//      guard that is stricter than the composition it replaced breaks all 15 verbs at once and no
//      end-to-end test on this cluster can currently see it. See that test's own header.
//
// WHY THIS TEST DOES NOT DRIVE THE CRASH, AND CANNOT ACCIDENTALLY DRIVE IT. A Fatal takes the test
// host down with it, so a test that reproduced the defect would abort the whole suite rather than
// report a red - and a suite that dies mid-queue is an ABSENCE of a signal, not a failure one (the
// DID_NOT_COMPLETE state in the plugin's testing notes). The assertion is therefore on the
// post-fix CONTRACT - the refusal - and the fixture is built so that a build WITHOUT the fix takes
// a different, harmless path instead of the fatal one:
//
//   every refusal case pairs its bad `name` with a `resolutionRule` that is well-formed text
//   naming NO rule in the verb's vocabulary. create_sound_concurrency resolved resolutionRule
//   BEFORE composing the package path even before this fix (the pre-existing "Resolve
//   resolutionRule up front" check), so on a reverted build the call is refused
//   INVALID_RESOLUTION_RULE ABOVE the concatenation - and the TestEqual on INVALID_ARGUMENT below
//   goes red while the process lives. CreatePackage is unreachable on both the fixed and the
//   reverted build.
//
// That property is why the guard sits ABOVE the resolutionRule check in the handler rather than at
// the concatenation, and the handler carries a comment saying so. Do NOT "improve" these cases by
// supplying a real resolutionRule, and do NOT rewrite them into a crash expectation: either change
// hands a live editor a string that ends it.
//
// create_sound_concurrency was chosen as the driven verb precisely because it is host-independent:
// USoundConcurrency is core engine, its handler carries no feature `#if`, and its pre-check needs
// no plugin, asset or registry. There is consequently no conditional-skip path in this file and no
// PINWRIGHT_ASSERTIONS_SKIPPED emitter - every assertion below runs on every host. The two
// MetaSound sites are covered by the source scan, which reads files from disk and so does not
// depend on the MetaSound registry being initialised either.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses, which also validates each payload
// against the declared ParamSpec.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/Audio/AudioPackagePathGuard.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace AudioCreatePackagePathSafetyHelpers
{
    // The verb's default destination, which every refusal case must leave untouched.
    constexpr const TCHAR* SafetyConcurrencyFolder = TEXT("/Game/Audio/Concurrency");

    // Well-formed text that names no rule in the verb's closed vocabulary. This is the
    // load-bearing half of the no-crash guarantee described in the file header: it makes a build
    // WITHOUT the fix bail at its pre-existing resolutionRule check, above the concatenation,
    // instead of reaching CreatePackage.
    constexpr const TCHAR* SafetyUnknownResolutionRule = TEXT("PinWrightNotAResolutionRule");

    inline FString SafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWAudioPathSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString SafetyObjectPath(const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s.%s"), SafetyConcurrencyFolder, *Name, *Name);
    }

    // Drives one refusal case and reports on all three facts that separate the post-fix contract
    // from the pre-fix behaviour: it is refused, it is refused as a CALLER ARGUMENT error rather
    // than as an unknown resolution rule, and the message names the offending value.
    inline void ExpectNameRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& BadName, const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), BadName);
        Params->SetStringField(TEXT("resolutionRule"), SafetyUnknownResolutionRule);

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink,
            TEXT("audio.authoring.create_sound_concurrency"),
            TEXT("req-audio-concurrency-name-safety"), Params, bSuccess, ErrorCode);

        Test.TestFalse(*FString::Printf(TEXT("a %s name ('%s') is refused"), Label, *BadName),
            bSuccess);
        // The discriminator against a reverted fix: pre-fix this same payload answers
        // INVALID_RESOLUTION_RULE from the rule check, never INVALID_ARGUMENT.
        Test.TestEqual(*FString::Printf(
            TEXT("a %s name is refused as a caller argument error, not as an unknown rule"), Label),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        Test.TestTrue(*FString::Printf(
            TEXT("the %s refusal quotes the offending name or composed path"), Label),
            Sink->Message.Contains(BadName));
        // A refusal that still left a half-built package behind would be the same data hazard in
        // slower motion - a later editor-wide save-all flushes it into host Content.
        Test.TestTrue(*FString::Printf(TEXT("the %s refusal creates no object"), Label),
            FindObject<UObject>(nullptr, *SafetyObjectPath(BadName)) == nullptr);
    }

    // --- source-scan helpers ------------------------------------------------------------------

    // The two files that carry the audio cluster's CreatePackage sites.
    inline TArray<FString> SafetyGuardedSourceFiles()
    {
        TArray<FString> Files;
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return Files;
        }
        const FString AudioDir = Plugin->GetBaseDir()
            / TEXT("Source") / TEXT("PinWright") / TEXT("Private")
            / TEXT("Handlers") / TEXT("Audio");
        Files.Add(AudioDir / TEXT("AudioAuthoringHandler.cpp"));
        Files.Add(AudioDir / TEXT("MetaSound") / TEXT("MetaSoundPatchPresetHandler.cpp"));
        return Files;
    }

    // Counts occurrences of Token that are CODE, not prose: a token preceded on its own line by a
    // "//" is a mention, not a call. Deliberately line-based rather than a full comment stripper -
    // every token counted here sits alone on its line in these files, and a stripper that has to
    // reason about string literals is more ways to be wrong than the property is worth.
    inline int32 SafetyCountCodeOccurrences(const TArray<FString>& Lines, const TCHAR* Token)
    {
        int32 Count = 0;
        for (const FString& Line : Lines)
        {
            const int32 TokenAt = Line.Find(Token);
            if (TokenAt == INDEX_NONE)
            {
                continue;
            }
            const int32 CommentAt = Line.Find(TEXT("//"));
            if (CommentAt != INDEX_NONE && CommentAt < TokenAt)
            {
                continue;
            }
            ++Count;
        }
        return Count;
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest below opens the helper namespace inside its own body rather than at file scope: a
// file-scope using-directive would leak into every other test .cpp that Unity merges after this
// one into the same translation unit.

// ============================================================================
// A `name` carrying a path is refused instead of composing "//" into CreatePackage
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioCreateSoundConcurrencyNameCarryingAPathIsRefusedTest,
    "PinWright.audio.authoring.create_sound_concurrency.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioCreateSoundConcurrencyNameCarryingAPathIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace AudioCreatePackagePathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // The literal one-argument kill: "//" inside the name reaches CreatePackage's Fatal
    // (UObjectGlobals.cpp:1094-1096) whichever composition the site uses.
    ExpectNameRefused(*this, Dispatcher, Sink, TEXT("Bus//Master"), TEXT("embedded double slash"));

    // The shape measured on B-foliage-add-type-name-with-slash-kills-the-editor: a rooted path
    // passed as a name.
    ExpectNameRefused(*this, Dispatcher, Sink,
        TEXT("/Game/PinWrightScratch/PWScratch_ConcurrencySafety"), TEXT("rooted path"));

    // The whole class, not just the fatal member. An interior slash composes a nested package
    // rather than "//", so it does not fatal - but it silently writes somewhere the caller did not
    // name, which is the same argument confusion one step short of the crash.
    ExpectNameRefused(*this, Dispatcher, Sink, TEXT("Sub/Leaf"), TEXT("interior slash"));

    // A backslash is not in INVALID_OBJECTNAME_CHARACTERS but IS in
    // INVALID_LONGPACKAGE_CHARACTERS, so this case is what proves the composed path is checked
    // against the engine's package rules as well as the name against its object rules.
    ExpectNameRefused(*this, Dispatcher, Sink, TEXT("Sub\\Leaf"), TEXT("backslash"));

    // Traversal, in the argument that has no sanitizer of its own - `path` is normalized,
    // `name` never was.
    ExpectNameRefused(*this, Dispatcher, Sink, TEXT("../Escape"), TEXT("traversal"));

    // ".." on its own reaches CreatePackage's OTHER Fatal, not the "//" one: ResolveName2
    // consumes it and the name resolves to empty, which is UObjectGlobals.cpp:1118. Caught here
    // by the same first half of the guard, because INVALID_OBJECTNAME_CHARACTERS contains '.'.
    ExpectNameRefused(*this, Dispatcher, Sink, TEXT(".."), TEXT("bare parent-directory"));

    // A trailing slash composes ".../Name/", which CreatePackage would resolve to something other
    // than the caller's name.
    ExpectNameRefused(*this, Dispatcher, Sink, TEXT("Trailing/"), TEXT("trailing slash"));

    // CONTROL. Without this, a handler that refused every name would satisfy every case above.
    // Driven with the same unknown resolutionRule, so it stops at INVALID_RESOLUTION_RULE - which
    // is exactly the proof that a bare name got PAST the new guard and reached the rule check.
    {
        const FString ControlName = SafetyUniqueName(TEXT("Control"));
        // Torn down anyway: if the control DID create something, the test must not leak it.
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(FString::Printf(TEXT("%s/%s"), SafetyConcurrencyFolder, *ControlName));
        };

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), ControlName);
        Params->SetStringField(TEXT("resolutionRule"), SafetyUnknownResolutionRule);

        bool bSuccess = true;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("audio.authoring.create_sound_concurrency"),
            TEXT("req-audio-concurrency-name-control"), Params, bSuccess, ErrorCode);

        TestFalse(TEXT("the control call still fails - its resolutionRule does not exist"),
            bSuccess);
        TestEqual(TEXT("a BARE name passes the guard and is refused on the rule instead"),
            ErrorCode, FString(TEXT("INVALID_RESOLUTION_RULE")));
    }

    return true;
}

// ============================================================================
// All 15 audio CreatePackage sites route through the one shared guard
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioPackagePathGuardEveryCreatePackageIsGuardedTest,
    "PinWright.audio.authoring.package_path_guard.EveryCreatePackageIsGuarded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioPackagePathGuardEveryCreatePackageIsGuardedTest::RunTest(const FString& Parameters)
{
    using namespace AudioCreatePackagePathSafetyHelpers;

    const TArray<FString> Files = SafetyGuardedSourceFiles();
    if (!TestTrue(TEXT("resolved the audio handler sources through IPluginManager"),
            Files.Num() == 2))
    {
        return false;
    }

    int32 TotalCreateSites = 0;
    for (const FString& File : Files)
    {
        const FString Leaf = FPaths::GetCleanFilename(File);

        TArray<FString> Lines;
        if (!TestTrue(*FString::Printf(TEXT("%s loads from disk"), *Leaf),
                FFileHelper::LoadFileToStringArray(Lines, *File)))
        {
            continue;
        }

        const int32 CreateSites = SafetyCountCodeOccurrences(Lines, TEXT("CreatePackage("));
        const int32 GuardSites =
            SafetyCountCodeOccurrences(Lines, TEXT("ComposeAudioAssetPackagePathOrRefuse("));

        // Emptiness guard: a file that stopped calling CreatePackage entirely would otherwise
        // satisfy the equality below while proving nothing.
        if (!TestTrue(*FString::Printf(TEXT("%s still has CreatePackage sites to guard"), *Leaf),
                CreateSites > 0))
        {
            continue;
        }
        TestEqual(*FString::Printf(
            TEXT("%s routes every CreatePackage through the shared audio guard"), *Leaf),
            GuardSites, CreateSites);

        // The old idiom itself. Equality above would already catch a new unguarded site, but this
        // names what was wrong so a failure reads as an instruction rather than a riddle.
        TestEqual(*FString::Printf(
            TEXT("%s no longer composes a package path with the unguarded 'Path / Name' idiom"),
            *Leaf),
            SafetyCountCodeOccurrences(Lines, TEXT("PackagePath = Path / Name")), 0);

        TotalCreateSites += CreateSites;
    }

    // The cluster total the sweep enumerated. Asserted as a lower bound, not an equality: another
    // workstream adding a guarded audio create verb must not turn this red, but a site quietly
    // disappearing from the scan's reach must.
    TestTrue(*FString::Printf(
        TEXT("the scan still reaches all 15 enumerated audio CreatePackage sites (saw %d)"),
        TotalCreateSites), TotalCreateSites >= 15);

    return true;
}

// ============================================================================
// The shared guard is a faithful replacement for `Path / Name`, not a stricter one
// ============================================================================
// This is the one property no dispatcher-driven test above can catch. The normalizer feeding the
// 15 sites (NormalizeContentAssetPath, which absorbed the former file-local NormalizeAudioPath and
// MetaSound::NormalizeAudioAssetPath) strips trailing slashes itself, so a trailing-slash `path`
// never reaches the guard through a live verb today - and a regression here would therefore be
// invisible end to end until that normalizer changed, at which point 15 verbs would start refusing
// a folder that used to work. The helper is exercised directly so the property is asserted rather
// than assumed.
//
// WHAT PROVIDES THE PROPERTY MOVED, AND THE ASSERTIONS DID NOT. This function used to trim the
// folder itself because PinWrightComposeAssetPackagePath joined with Printf("%s/%s"), which
// doubled the separator. That composer now joins with FString::operator/, so the absorption
// happens one layer down and the trim was deleted as dead weight. Every expectation below is
// unchanged and still holds - which is the point of writing them against the composed OUTPUT
// rather than against the trim.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAudioPackagePathGuardTrailingSlashFolderComposesTest,
    "PinWright.audio.authoring.package_path_guard.TrailingSlashFolderComposesWithoutDoubling",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAudioPackagePathGuardTrailingSlashFolderComposesTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    FHandlerContext Ctx = FHandlerContext::MakeTestContextWithCapture(
        TEXT("test-audio-package-path-guard"), TEXT("audio.authoring.create_sound_concurrency"),
        MakeShared<FJsonObject>(), &Capture);

    // A folder WITHOUT a trailing slash - the shape every live caller produces today. Establishes
    // the expected output before the interesting case, so a failure below is unambiguous.
    {
        FString Composed;
        const bool bOk = PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(
            Ctx, TEXT("/Game/Audio/Cues"), TEXT("SC_Probe"), Composed);
        TestTrue(TEXT("a bare folder and a bare name compose"), bOk);
        TestEqual(TEXT("the composed path is folder + '/' + name"),
            Composed, FString(TEXT("/Game/Audio/Cues/SC_Probe")));
    }

    // The same request with a trailing slash on the folder. PinWrightComposeAssetPackagePath joins
    // with FString::operator/, whose PathAppend (Core/Private/Containers/String.cpp.inl:855-885)
    // POPS the left side's terminator instead of adding a second one, so this composes to the same
    // path as the bare folder above. Refusing it would be a regression: the Printf("%s/%s") join
    // this replaced WOULD double it into "//", which IsValidLongPackageName rejects.
    {
        FString Composed;
        const bool bOk = PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(
            Ctx, TEXT("/Game/Audio/Cues/"), TEXT("SC_Probe"), Composed);
        TestTrue(TEXT("a trailing-slash folder is accepted, not refused"), bOk);
        TestEqual(TEXT("a trailing-slash folder composes identically to a bare one"),
            Composed, FString(TEXT("/Game/Audio/Cues/SC_Probe")));
        TestFalse(TEXT("the composed path carries no doubled separator"),
            Composed.Contains(TEXT("//")));
        TestFalse(TEXT("nothing was sent - an accepted compose reports nothing"),
            Capture.bWasCalled);
    }

    // CONTROL. Without this the two cases above would be satisfied by a helper that accepted
    // everything. PathAppend absorbs exactly ONE separator between the two halves; it does not
    // collapse a "//" that is inside either half, and a "//" in the NAME is refused a step earlier
    // by FName::IsValidXName.
    {
        // Pre-seeded so "left untouched" cannot masquerade as "cleared": a caller that ignored
        // the bool must still find nothing usable in the out-parameter.
        FString Composed = TEXT("/Game/Stale/Leftover");
        const bool bOk = PinWrightAudioPackagePath::ComposeAudioAssetPackagePathOrRefuse(
            Ctx, TEXT("/Game/Audio/Cues/"), TEXT("Bus//Master"), Composed);
        TestFalse(TEXT("separator absorption does not also let a '//' NAME through"), bOk);
        TestTrue(TEXT("the refusal was dispatched"), Capture.bWasCalled);
        TestFalse(TEXT("the dispatched response is an error"), Capture.bSuccess);
        TestEqual(TEXT("the dispatched code is INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(TEXT("the composed path is left empty on refusal"), Composed.IsEmpty());
    }

    return true;
}
