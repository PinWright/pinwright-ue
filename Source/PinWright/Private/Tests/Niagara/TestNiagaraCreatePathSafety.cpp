// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestNiagaraCreatePathSafety.cpp - regression coverage for the niagara.create_system /
// niagara.create_emitter half of B-createpackage-unvalidated-paths-plugin-wide.
//
// WHAT WAS WRONG. Both verbs took `savePath` and `name` straight off the wire, concatenated them
// ("<savePath>/" + name) and handed the result to CreatePackage with no validation of any kind.
// CreatePackage logs at **Fatal** for a name containing "//" (UObjectGlobals.cpp:1094-1096) and
// for one that resolves to empty (:1118), and Fatal is not compiled out in any configuration - so
// the call does not fail, it ends the editor PROCESS and every unsaved package in it. The
// handlers' own `if (!Package)` could never fire, because nothing after CreatePackage is reached.
// A single `name` of "/Game/X" or "a//b" was enough. The mechanism was MEASURED on a sibling verb
// (B-foliage-add-type-name-with-slash-kills-the-editor, editor pid 7856).
//
// WHY THIS TEST CANNOT DRIVE THE FATAL - AND WHY ITS CONSTRUCTION DIFFERS FROM THE FOLIAGE AND
// LANDSCAPE ONES. A Fatal takes the test host down with it, so a test that reproduced the defect
// would abort the whole suite rather than report a red, and a suite that dies mid-queue is not a
// failure signal but the absence of one (the DID_NOT_COMPLETE state in the plugin's testing
// notes). Tests/Environment/TestFoliageAddTypeNamePathSafety.cpp keeps a reverted build off the
// fatal path by pairing every bad `name` with a `meshPath` that resolves to nothing, so the
// pre-fix handler bails ABOVE the concatenation. **These two verbs have no such argument.**
// Between `savePath` and CreatePackage a reverted build passes only
// `FModuleManager::IsModuleLoaded("Niagara")`, which is true on every host where this file
// compiles. There is therefore NO wire payload carrying a "//" that a reverted build survives,
// and none is sent. The coverage is split instead:
//
//   PART A drives the guard itself - PinWrightComposeAssetPackagePath (Handlers/
//   PackagePathCompose.h) - as a pure function. It is what both handlers now route through, it
//   is where the "//" refusal lives, and it calls nothing: CreatePackage does not appear
//   anywhere in its call graph, on any build. Every shape that kills a live editor is asserted
//   here, at zero risk.
//
//   PART B proves at the WIRE that the handler actually consults that guard, using the one
//   refusal class whose reverted-build behaviour is provably harmless: an UNMOUNTED package
//   root. `savePath: "/PinWrightNotAMountedRoot/..."` composes a path with no "//" and no
//   invalid characters, so a reverted build hands CreatePackage a string it accepts, builds the
//   object under a clean FName, fails only at the save, and answers success - which the
//   TestEqual on INVALID_ARGUMENT below scores red while the process lives. CreatePackage's two
//   Fatals are unreachable on the fixed build (refused before the call) and on the reverted one
//   (no "//", non-empty). The assertion is on the composer's own message shape - the offending
//   composed candidate plus the engine's verbatim mount-root reason - because nothing else in
//   either handler can produce it, which is what makes it a routing proof rather than a generic
//   "some error came back".
//
//   PART C is the control. Without it a handler that refused every request would satisfy A and
//   B. It asserts a well-formed name + folder is NOT refused as a caller argument.
//
// Do NOT "strengthen" Part B by moving a slash-bearing `name` onto the wire: on a build with the
// guard removed that payload does not fail the test, it ends the process running it.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses, which also validates each payload
// against the declared ParamSpec.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/PackagePathCompose.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace NiagaraCreatePathSafetyHelpers
{
    // A root no project mounts, so IsValidLongPackageName refuses it with PackageNamePathNotMounted
    // while every character in it stays legal - the property Part B of the file header rests on.
    constexpr const TCHAR* NiagaraPathSafetyUnmountedFolder =
        TEXT("/PinWrightNotAMountedRoot/NiagaraCreate");

    // A real, mounted destination for the control case.
    constexpr const TCHAR* NiagaraPathSafetyScratchFolder = TEXT("/Game/PinWrightTests");

    inline FString NiagaraPathSafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWNiagaraPathSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // PART A. Every shape that reaches one of CreatePackage's two Fatals, asserted against the
    // composer directly. Nothing here touches CreatePackage on any build, so the whole class -
    // including the rooted name that was measured killing an editor - is safe to state here and
    // only here.
    inline void NiagaraPathSafetyAssertComposerRefusals(FAutomationTestBase& Test)
    {
        const TCHAR* Folder = TEXT("/Game/PinWrightTests/ComposerProbe");
        struct FCase { const TCHAR* Name; const TCHAR* Label; };
        const FCase Cases[] = {
            // The measured killer: a rooted path in the name slot composes "<folder>//Game/...".
            { TEXT("/Game/PinWrightScratch/PWRooted"), TEXT("rooted path") },
            // The one-argument kill the sweep ticket calls out: "//" carried inside the name
            // itself, which no composition style can dilute.
            { TEXT("a//b"),                            TEXT("embedded double slash") },
            // An interior slash does not compose "//", so it does not fatal - it silently writes
            // to a package the caller never named, the same argument confusion one step short.
            { TEXT("Sub/Leaf"),                        TEXT("interior slash") },
            // Not in INVALID_OBJECTNAME_CHARACTERS but IS in INVALID_LONGPACKAGE_CHARACTERS, so
            // this is the case proving the COMPOSED PATH is checked and not just the bare name.
            { TEXT("Sub\\Leaf"),                       TEXT("backslash") },
            { TEXT("../Escape"),                       TEXT("traversal") },
            { TEXT("Trailing/"),                       TEXT("trailing slash") },
            // CreatePackage's OTHER Fatal is an empty resolved name. Note which half catches
            // this: FName::IsValidXName returns true for an empty string (UnrealNames.cpp:3932),
            // so it is the composed path's trailing slash that is refused, not the bare name -
            // which is exactly why the composer checks both and not just the name.
            { TEXT(""),                                TEXT("empty") },
        };

        for (const FCase& Case : Cases)
        {
            FString Composed = TEXT("sentinel-not-cleared");
            FString Error;
            const bool bAccepted =
                PinWrightComposeAssetPackagePath(Folder, Case.Name, Composed, Error);
            Test.TestFalse(*FString::Printf(
                TEXT("the composer refuses a %s name ('%s')"), Case.Label, Case.Name), bAccepted);
            Test.TestTrue(*FString::Printf(
                TEXT("a refused %s name yields no package path to pass on"), Case.Label),
                Composed.IsEmpty());
            Test.TestFalse(*FString::Printf(
                TEXT("the %s refusal carries a reason the caller can act on"), Case.Label),
                Error.IsEmpty());
        }

        // THE FOLDER IS AN EQUALLY LIVE ONE-ARGUMENT KILL, and it is the half that reads safe.
        // `savePath: "/Game//FX"` with a perfectly bare `name` composes "/Game//FX/<name>" and
        // reaches the same Fatal - a folder normalizer that only trims trailing slashes and maps
        // aliases does NOT collapse an interior "//". This is why the composer checks the
        // COMPOSED path with FPackageName::IsValidLongPackageName and not just the bare name;
        // a name-only character check would pass every case below.
        const FCase FolderCases[] = {
            { TEXT("/Game//FX"),                       TEXT("interior double slash") },
            { TEXT("//Game/FX"),                       TEXT("doubled leading slash") },
            { TEXT("/Game/FX//Sub"),                   TEXT("double slash deeper in") },
            { TEXT("/Game/../Escape"),                 TEXT("traversal") },
            { TEXT(""),                                TEXT("empty") },
            { NiagaraPathSafetyUnmountedFolder,        TEXT("unmounted root") },
        };
        for (const FCase& Case : FolderCases)
        {
            FString Composed = TEXT("sentinel-not-cleared");
            FString Error;
            Test.TestFalse(*FString::Printf(
                TEXT("the composer refuses a %s folder ('%s') even with a bare name"),
                Case.Label, Case.Name),
                PinWrightComposeAssetPackagePath(Case.Name, TEXT("PWBareLeaf"), Composed, Error));
            Test.TestTrue(*FString::Printf(
                TEXT("a refused %s folder yields no package path to pass on"), Case.Label),
                Composed.IsEmpty());
        }

        // Positive branch, so the assertions above cannot be satisfied by a composer that
        // refuses everything.
        FString GoodComposed;
        FString GoodError;
        Test.TestTrue(TEXT("the composer accepts a bare name under a mounted folder"),
            PinWrightComposeAssetPackagePath(Folder, TEXT("PWBareName"), GoodComposed, GoodError));
        Test.TestEqual(TEXT("the composer composes exactly '<folder>/<name>'"), GoodComposed,
            FString(TEXT("/Game/PinWrightTests/ComposerProbe/PWBareName")));
    }

    // PART B + PART C for one verb. Both niagara create verbs have byte-identical argument
    // handling, so the drive is shared rather than copied.
    inline void NiagaraPathSafetyAssertVerbRoutesThroughComposer(
        FAutomationTestBase& Test, const TCHAR* Method)
    {
        DispatcherTestHelpers::FSinkPtr Sink;
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        const FString UnroutableName = NiagaraPathSafetyUniqueName(TEXT("Unroutable"));
        {
            TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
            Params->SetStringField(TEXT("name"), UnroutableName);
            Params->SetStringField(TEXT("savePath"), NiagaraPathSafetyUnmountedFolder);

            bool bSuccess = true;
            FString ErrorCode;
            DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
                TEXT("req-niagara-create-path-safety"), Params, bSuccess, ErrorCode);

            Test.TestFalse(*FString::Printf(
                TEXT("%s refuses a savePath under no mounted root"), Method), bSuccess);
            // The discriminator against a reverted guard: without it this payload composes a
            // path CreatePackage accepts, the verb builds the asset and answers success.
            Test.TestEqual(*FString::Printf(
                TEXT("%s refuses it as a caller argument error"), Method),
                ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
            // Routing proof: only PinWrightComposeAssetPackagePath emits the composed candidate
            // together with the engine's own package-path reason.
            Test.TestTrue(*FString::Printf(
                TEXT("%s quotes the composed path it refused"), Method),
                Sink->Message.Contains(FString::Printf(TEXT("%s/%s"),
                    NiagaraPathSafetyUnmountedFolder, *UnroutableName)));
            Test.TestTrue(*FString::Printf(
                TEXT("%s surfaces the engine's own reason verbatim"), Method),
                Sink->Message.Contains(TEXT("not a valid package path")));
        }

        // PART C. A well-formed name and folder must get PAST the guard. Asserted as "not
        // refused as a caller argument" rather than as full success, so the control measures the
        // guard rather than the Niagara factory.
        {
            const FString ControlName = NiagaraPathSafetyUniqueName(TEXT("Control"));
            const FString ControlPackage = FString::Printf(TEXT("%s/%s"),
                NiagaraPathSafetyScratchFolder, *ControlName);
            ON_SCOPE_EXIT { CleanupTestAsset(ControlPackage); };

            TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
            Params->SetStringField(TEXT("name"), ControlName);
            Params->SetStringField(TEXT("savePath"), NiagaraPathSafetyScratchFolder);

            bool bSuccess = false;
            FString ErrorCode;
            DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
                TEXT("req-niagara-create-path-control"), Params, bSuccess, ErrorCode);

            Test.TestNotEqual(*FString::Printf(
                TEXT("%s lets a bare name under a mounted folder past the path guard"), Method),
                ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        }
    }
}

// ============================================================================
// niagara.create_system composes its package path through the guard
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraCreateSystemPathSafetyTest,
    "PinWright.niagara.create_system.PathIsGuardedBeforeCreatePackage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCreateSystemPathSafetyTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraCreatePathSafetyHelpers;

    NiagaraPathSafetyAssertComposerRefusals(*this);
    NiagaraPathSafetyAssertVerbRoutesThroughComposer(*this, TEXT("niagara.create_system"));
    return true;
}

// ============================================================================
// niagara.create_emitter does the same, and is asserted separately because it carries its own
// copy of the composition rather than sharing one with create_system
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraCreateEmitterPathSafetyTest,
    "PinWright.niagara.create_emitter.PathIsGuardedBeforeCreatePackage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCreateEmitterPathSafetyTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraCreatePathSafetyHelpers;

    NiagaraPathSafetyAssertVerbRoutesThroughComposer(*this, TEXT("niagara.create_emitter"));
    return true;
}
