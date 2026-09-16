// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestLevelStructureNameSafety.cpp - regression coverage for the three
// LevelStructureHandler.cpp members of B-createpackage-unvalidated-paths-plugin-wide:
// level.structure.create_level, level.structure.create_data_layer and
// level.structure.configure_hlod_layer.
//
// WHAT WAS WRONG. Each of the three composes "<folder>/<caller name>" and hands the result to
// CreatePackage. CreatePackage (UObjectGlobals.cpp:1086-1120) logs at **Fatal** - a verbosity that
// is not compiled out in any configuration - for a name containing "//" (:1094-1096) and for a
// name that resolves to empty (:1118). So the call did not fail: the editor PROCESS died, taking
// every unsaved package with it. The handlers' own `if (!Package)` branches could never fire,
// because nothing after CreatePackage was reached.
//
// The FOLDER half was already guarded at all three - SanitizeProjectRelativePath collapses "//",
// rejects ".." and unmounted roots, and each handler assigns its result back over the raw
// argument before composing. The NAME half was not. `dataLayerName` and `hlodLayerName` were
// checked only for emptiness, and `levelName`'s hand-rolled character filter covers '/' and '\'
// but not '.', so `".."` composed "/Game/Maps/.." which CreatePackage trims to "/Game/Maps/." and
// ResolveName2 then empties - the second Fatal, reachable from a single argument.
//
// WHY THIS TEST DOES NOT DRIVE THE CRASH, AND CANNOT ACCIDENTALLY DRIVE IT. A Fatal takes the test
// host down with it, so a test that reproduced the defect would abort the whole suite rather than
// report a red - and a suite that dies mid-queue is an absence of a signal, not a failure one (the
// DID_NOT_COMPLETE state in the plugin's testing notes). The assertion is therefore on the
// post-fix CONTRACT - the refusal - and the fixture is built so a build WITHOUT the fix takes a
// different, harmless path:
//
//   every refusal case pairs its bad name with a `<verb>Path` of "/Game/../../Engine/Content".
//   SanitizeProjectRelativePath rejects any path containing "..", and every one of the three
//   handlers runs that sanitizer ABOVE its composition. On a reverted build the call is therefore
//   refused SECURITY_VIOLATION at the folder - or, on create_data_layer, even earlier at the
//   editor-world / World-Partition / subsystem gates - and the TestEqual on INVALID_ARGUMENT below
//   goes red while the process lives. This holds on every host and needs no content fixture: it
//   depends only on the "…" literal this file supplies, not on what level happens to be loaded.
//   CreatePackage is unreachable on both the fixed and the reverted build.
//
// That property depends on the fix keeping each name check ABOVE that handler's folder sanitizer,
// and all three handlers carry a comment saying so. Do NOT "improve" these cases by supplying a
// well-formed folder, and do NOT rewrite them into a crash expectation: either change hands a live
// editor a string that ends it.
//
// WHAT IS NOT COVERED, DELIBERATELY. The fix has a second half -
// PinWrightLevelStructureValidatePackagePath, which re-validates the exact composed string after
// the `IsValidMountPoint` fallback prepends "/Game/" to a path that already begins with '/'. That
// fallback manufactures "//" by itself, but only for a name carrying a character that is legal for
// an object yet not for a package path ('\', '*', '?', '<', '>') combined with a mounted folder
// other than /Game|/Engine|/Script. Exercising it from the wire would mean handing a live editor
// exactly the input that kills it on a reverted build, so it is left to code review rather than
// covered here. It is not dead code: the bare-name check does not reject those five characters.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses, which also validates each payload
// against the declared ParamSpec.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"

#include "Misc/Guid.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace LevelStructureNameSafetyHelpers
{
    // The load-bearing half of the no-crash guarantee described in the file header. Every one of
    // the three handlers runs SanitizeProjectRelativePath on its folder argument above its
    // composition, and that helper rejects any path containing "..", so a build without the name
    // check bails here instead of reaching CreatePackage.
    constexpr const TCHAR* SafetyRefusedFolder = TEXT("/Game/../../Engine/Content");

    // Names the engine's own object-naming rule (INVALID_OBJECTNAME_CHARACTERS) rejects. Each is
    // one member of the class the fix closes, not five spellings of one bug.
    struct FSafetyBadName
    {
        const TCHAR* Value;
        const TCHAR* Label;
    };

    inline const TArray<FSafetyBadName>& SafetyBadNames()
    {
        static const TArray<FSafetyBadName> Names = {
            // The measured shape on B-foliage-add-type-name-with-slash-kills-the-editor: a name
            // that is itself a rooted path.
            { TEXT("/Game/PinWrightScratch/PWNameSafety"), TEXT("rooted path") },
            // An interior slash does not compose "//", but it silently writes to a package the
            // caller never named - the same argument confusion one step short of the crash.
            { TEXT("Sub/Leaf"), TEXT("interior slash") },
            // The first Fatal, reachable from this one argument alone.
            { TEXT("a//b"), TEXT("embedded double slash") },
            // The second Fatal: "<folder>/.." trims to "<folder>/." and then resolves to empty.
            // This is the case create_level's character filter did NOT cover.
            { TEXT(".."), TEXT("traversal") },
            // A dot makes CreatePackage treat the tail as a subobject and create a short-named
            // package instead of the one the caller asked for.
            { TEXT("Layer.Sub"), TEXT("dotted name") },
        };
        return Names;
    }

    inline FString SafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWNameSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Drives every bad name against one verb and reports on the two facts that separate the
    // post-fix contract from the pre-fix behaviour: it is refused, and it is refused as a CALLER
    // ARGUMENT error rather than as an unsafe folder.
    inline void ExpectBadNamesRefused(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const TCHAR* Method, const TCHAR* NameKey,
        const TCHAR* PathKey)
    {
        for (const FSafetyBadName& Bad : SafetyBadNames())
        {
            TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
            Params->SetStringField(NameKey, Bad.Value);
            Params->SetStringField(PathKey, SafetyRefusedFolder);

            bool bSuccess = true;
            FString ErrorCode;
            DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
                TEXT("req-level-structure-name-safety"), Params, bSuccess, ErrorCode);

            Test.TestFalse(*FString::Printf(TEXT("%s: a %s %s ('%s') is refused"),
                Method, Bad.Label, NameKey, Bad.Value), bSuccess);
            // The discriminator against a reverted fix: pre-fix this same payload answers
            // SECURITY_VIOLATION from the folder sanitizer (or an earlier gate), never
            // INVALID_ARGUMENT.
            Test.TestEqual(*FString::Printf(
                TEXT("%s: a %s %s is refused as a caller argument error, not as an unsafe folder"),
                Method, Bad.Label, NameKey),
                ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
            // Names the parameter rather than the value: create_level's older character filter
            // refuses the slash-bearing cases before the new check and reports the offending
            // CHARACTER instead of the whole string, so quoting the value is not universal here.
            Test.TestTrue(*FString::Printf(
                TEXT("%s: the %s refusal names the parameter the caller must fix"),
                Method, Bad.Label), Sink->Message.Contains(NameKey));
        }
    }

    // CONTROL. Without this, a handler that refused every name would satisfy every case above.
    // Driven with the same refused folder, so whatever stops the call is something OTHER than the
    // name check - which is exactly the proof that a bare name got past it. The stopping code is
    // asserted only as "not INVALID_ARGUMENT": create_data_layer's world / World-Partition /
    // subsystem gates sit between the name check and the folder sanitizer, so which of them
    // answers first depends on the host's loaded level, and pinning it would make this test
    // measure the host rather than the fix.
    inline void ExpectBareNameAccepted(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const TCHAR* Method, const TCHAR* NameKey,
        const TCHAR* PathKey, FString& OutErrorCode)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(NameKey, SafetyUniqueName(TEXT("Control")));
        Params->SetStringField(PathKey, SafetyRefusedFolder);

        bool bSuccess = true;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method,
            TEXT("req-level-structure-name-safety-control"), Params, bSuccess, OutErrorCode);

        Test.TestFalse(*FString::Printf(
            TEXT("%s: the control call still fails - its folder contains '..'"), Method),
            bSuccess);
        Test.TestNotEqual(*FString::Printf(
            TEXT("%s: a BARE name passes the name check and is stopped further down instead"),
            Method), OutErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }
}

// Each RunTest below opens the helper namespace inside its own body rather than at file scope: a
// file-scope using-directive would leak into every other test .cpp that Unity merges after this
// one into the same translation unit.

// ============================================================================
// level.structure.create_level: a levelName carrying a path (or "..") is refused instead of
// composing something CreatePackage dies on
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureCreateLevelNameIsRefusedTest,
    "PinWright.level.structure.create_level.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureCreateLevelNameIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace LevelStructureNameSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // Note on what each case proves HERE. create_level already carried a character filter that
    // rejects '/', so the three slash-bearing cases were refused INVALID_ARGUMENT before this fix
    // too - they pin the whole class rather than discriminate. The traversal and dotted-name cases
    // are the ones that filter let through, and they are what a reverted build fails.
    ExpectBadNamesRefused(*this, Dispatcher, Sink, TEXT("level.structure.create_level"),
        TEXT("levelName"), TEXT("levelPath"));

    // On this verb nothing sits between the name check and the folder sanitizer, so the control's
    // stopping code is pinnable exactly - and pinning it is what proves the refused folder really
    // is what a reverted build would answer with for the cases above.
    FString ControlErrorCode;
    ExpectBareNameAccepted(*this, Dispatcher, Sink, TEXT("level.structure.create_level"),
        TEXT("levelName"), TEXT("levelPath"), ControlErrorCode);
    TestEqual(TEXT("create_level: a bare name reaches the folder sanitizer, which rejects '..'"),
        ControlErrorCode, FString(TEXT("SECURITY_VIOLATION")));

    return true;
}

// ============================================================================
// level.structure.create_data_layer: same class, on the argument that had no validation at all
// beyond an emptiness check
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureCreateDataLayerNameIsRefusedTest,
    "PinWright.level.structure.create_data_layer.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureCreateDataLayerNameIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace LevelStructureNameSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    ExpectBadNamesRefused(*this, Dispatcher, Sink, TEXT("level.structure.create_data_layer"),
        TEXT("dataLayerName"), TEXT("dataLayerAssetPath"));

    FString ControlErrorCode;
    ExpectBareNameAccepted(*this, Dispatcher, Sink, TEXT("level.structure.create_data_layer"),
        TEXT("dataLayerName"), TEXT("dataLayerAssetPath"), ControlErrorCode);

    return true;
}

// ============================================================================
// level.structure.configure_hlod_layer: same class again
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelStructureConfigureHlodLayerNameIsRefusedTest,
    "PinWright.level.structure.configure_hlod_layer.NameCarryingAPathIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelStructureConfigureHlodLayerNameIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace LevelStructureNameSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    ExpectBadNamesRefused(*this, Dispatcher, Sink, TEXT("level.structure.configure_hlod_layer"),
        TEXT("hlodLayerName"), TEXT("hlodLayerPath"));

    FString ControlErrorCode;
    ExpectBareNameAccepted(*this, Dispatcher, Sink, TEXT("level.structure.configure_hlod_layer"),
        TEXT("hlodLayerName"), TEXT("hlodLayerPath"), ControlErrorCode);

    return true;
}
