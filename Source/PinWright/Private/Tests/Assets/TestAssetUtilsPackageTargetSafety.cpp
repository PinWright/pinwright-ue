// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestAssetUtilsPackageTargetSafety.cpp - regression coverage for the two AssetUtils.cpp members
// of B-createpackage-unvalidated-paths-plugin-wide (PrepareBlueprintPackageGuardingNameCollision
// and McpCreateControlRigBlueprint).
//
// WHAT WAS WRONG. Both helpers built a package name out of caller-supplied text and handed it
// straight to CreatePackage. CreatePackage (UObjectGlobals.cpp:1087-1120) logs at **Fatal** for a
// name containing "//" (:1094-1096) and for one that resolves to empty (:1118), and Fatal is not
// compiled out in any configuration - it ends the PROCESS. So the call did not fail: it killed the
// editor and every unsaved package in it (measured on
// B-foliage-add-type-name-with-slash-kills-the-editor, editor pid 7856). Each helper's own
// `if (!Package)` branch could never fire, because nothing after CreatePackage was reached. These
// two are SHARED helpers, so every caller of either inherited the hazard; the guard therefore
// lives inside them, and this file tests them as pure functions rather than through any verb.
//
// WHY THIS TEST CANNOT DRIVE THE FATAL, ON EITHER A FIXED OR A REVERTED BUILD. A Fatal takes the
// test host down with it, so a test that reproduced the defect would abort the whole suite instead
// of reporting a red - and a suite that dies mid-queue is an absence of a signal, not a failure
// one (the DID_NOT_COMPLETE state in the plugin's testing notes). The assertion is therefore on
// the post-fix CONTRACT - the refusal - and every fixture argument is chosen so that a build with
// the guard REMOVED reaches CreatePackage with a string neither Fatal branch can fire on:
//
//   * Only two inputs are fatal: a name containing "//", and a name that resolves to empty. **No
//     string in this file contains "//" in the argument that reaches CreatePackage, and none
//     composes one**: every folder literal here is a mounted path with no trailing slash, and
//     every bad asset name is free of a leading slash.
//   * The SECOND Fatal is not covered by that alone, so it is excluded separately: a name empties
//     only through ResolveName2 (UObjectGlobals.cpp:1219-1240), which walks '.' and ':' delimiters
//     and returns the name UNCHANGED the moment it finds neither (`if (*DelimiterOrEnd == '\0')
//     return true`). ".." is the known killer - CreatePackage trims its trailing '.' and
//     ResolveName2 then empties the rest. **No string in this file that reaches CreatePackage
//     contains '.' or ':'** (the GUID suffixes are EGuidFormats::Digits, hex only), so the
//     resolved name is the input and cannot be empty. Do not add a '.' to any of these fixtures.
//   * PrepareBlueprintPackageGuardingNameCollision passes ONLY its PackagePath to CreatePackage -
//     its AssetName is used for the collision probe and never concatenated. That is what makes the
//     `Sub//Leaf` and `/Game/...`-shaped NAME cases below safe to drive here even though they are
//     exactly the shape that killed an editor one level up (where the caller composes
//     `Path / Name` and the "//" lands in the path). Verified at the call site before writing them.
//   * McpCreateControlRigBlueprint DOES concatenate both halves into the CreatePackage argument,
//     so its cases below use bad names carrying a single '/' or a '\' - refused by the guard,
//     harmless to a reverted build - and deliberately NOT a "//" one.
//
// On a reverted build each refusal case therefore RETURNS SUCCESS (a package, or a Control Rig
// Blueprint) and the TestFalse/TestNull below goes red while the process lives. Do NOT "improve"
// these cases by feeding a "//" into the argument that reaches CreatePackage: that hands a live
// suite host a string that ends it, and there is no assertion worth that.
//
// WHAT IS NOT COVERED, STATED RATHER THAN IMPLIED. The "//" input itself is unreachable by any
// test that must leave a live process, so it is covered by construction instead: the guard is a
// single FPackageName::IsValidLongPackageName call, which rejects "//" in the same pass as the
// unmounted-root and INVALID_LONGPACKAGE_CHARACTERS cases that ARE asserted here. If that call is
// ever replaced by a hand-rolled character list, these cases are what fail first.
//
// Each test ends with a valid-input CONTROL. Without it a helper that refused everything would
// satisfy every case above.
#include "Misc/AutomationTest.h"

#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "Utils/AssetUtils.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace AssetUtilsPackageTargetSafetyHelpers
{
    // A mounted folder with no trailing slash. Both properties are load-bearing for the
    // no-Fatal guarantee in the file header: mounted so the CONTROL is not refused, no trailing
    // slash so no composition here can produce "//".
    constexpr const TCHAR* SafetyFolder = TEXT("/Game/PinWrightTests");

    inline FString SafetyUniqueName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWAssetUtilsSafety_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString SafetyJoin(const FString& Folder, const FString& Name)
    {
        return FString::Printf(TEXT("%s/%s"), *Folder, *Name);
    }

    inline FString SafetyObjectPath(const FString& PackagePath, const FString& Name)
    {
        return FString::Printf(TEXT("%s.%s"), *PackagePath, *Name);
    }

    // Drives one refusal case of PrepareBlueprintPackageGuardingNameCollision and reports the
    // three facts that separate the post-fix contract from the pre-fix behaviour: it is refused,
    // it is refused as a MALFORMED ARGUMENT rather than as an existing asset (bNameCollision
    // false, which is what lets a caller keep INVALID_ARGUMENT and ASSET_EXISTS apart), and the
    // message quotes the offending value so the caller can act on it.
    inline void ExpectBlueprintTargetRefused(FAutomationTestBase& Test, const FString& PackagePath,
        const FString& AssetName, const FString& Quoted, const TCHAR* Label)
    {
        UPackage* Package = nullptr;
        FString AssetObjectPath;
        bool bNameCollision = true;
        FString Error;

        const bool bPrepared = PrepareBlueprintPackageGuardingNameCollision(
            PackagePath, AssetName, Package, AssetObjectPath, bNameCollision, Error);

        Test.TestFalse(*FString::Printf(TEXT("a %s target is refused"), Label), bPrepared);
        // The discriminator against a reverted fix: pre-fix this same pair returned true with a
        // live package. A null package is also the proof that nothing half-built was left behind.
        Test.TestNull(*FString::Printf(TEXT("a %s target creates no package"), Label), Package);
        Test.TestFalse(*FString::Printf(
            TEXT("a %s target is a malformed argument, not a name collision"), Label),
            bNameCollision);
        Test.TestTrue(*FString::Printf(TEXT("the %s refusal quotes the offending value"), Label),
            Error.Contains(Quoted));
    }

    // Same for McpCreateControlRigBlueprint, whose failure channel is a null return.
    inline void ExpectControlRigTargetRefused(FAutomationTestBase& Test, const FString& Folder,
        const FString& AssetName, const TCHAR* Label)
    {
        const FString ComposedPackagePath = SafetyJoin(Folder, AssetName);
        // Torn down whatever happens: on a build without the guard this call SUCCEEDS and writes
        // a real Control Rig Blueprint there, and the test must not leak it into host content.
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(ComposedPackagePath);
        };

        FString Error;
        UBlueprint* Created =
            McpCreateControlRigBlueprint(AssetName, Folder, /*TargetSkeleton*/ nullptr, Error);

        Test.TestNull(*FString::Printf(TEXT("a %s target is refused"), Label), Created);
        Test.TestTrue(*FString::Printf(TEXT("the %s refusal quotes the offending value"), Label),
            Error.Contains(AssetName));
        Test.TestNull(*FString::Printf(TEXT("a %s target builds no asset"), Label),
            FindObject<UObject>(nullptr, *SafetyObjectPath(ComposedPackagePath, AssetName)));
    }
}

// Each RunTest below opens the helper namespace inside its own body rather than at file scope: a
// file-scope using-directive would leak into every other test .cpp that Unity merges after this
// one into the same translation unit.

// ============================================================================
// PrepareBlueprintPackageGuardingNameCollision refuses a target CreatePackage would die on
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetUtilsBlueprintPackageTargetSafetyTest,
    "PinWright.Assets.AssetUtils.BlueprintPackageTargetRefusesMalformedPathOrName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetUtilsBlueprintPackageTargetSafetyTest::RunTest(const FString& Parameters)
{
    using namespace AssetUtilsPackageTargetSafetyHelpers;

    const FString GoodFolder(SafetyFolder);

    // --- bad NAME, well-formed path. Safe here for the reason in the file header: this helper
    // hands only PackagePath to CreatePackage. ---

    // The measured kill shape, at the argument that carries it one level up. The only caller
    // composes `Path / Name` and hands the result in as PackagePath, so a name like this is what
    // puts "//" into the path.
    ExpectBlueprintTargetRefused(*this, SafetyJoin(GoodFolder, SafetyUniqueName(TEXT("A"))),
        TEXT("Sub//Leaf"), TEXT("Sub//Leaf"), TEXT("double-slash name"));

    // An interior slash does not compose "//", so it is not fatal - it silently writes to a
    // nested package the caller did not name, which is the same argument confusion one step short
    // of the crash.
    ExpectBlueprintTargetRefused(*this, SafetyJoin(GoodFolder, SafetyUniqueName(TEXT("B"))),
        TEXT("Sub/Leaf"), TEXT("Sub/Leaf"), TEXT("interior-slash name"));

    // A rooted path passed as a name.
    ExpectBlueprintTargetRefused(*this, SafetyJoin(GoodFolder, SafetyUniqueName(TEXT("C"))),
        TEXT("/Game/Elsewhere/Thing"), TEXT("/Game/Elsewhere/Thing"), TEXT("rooted name"));

    // --- bad PATH, well-formed bare name. This is the argument that DOES reach CreatePackage, so
    // every case here is chosen free of "//" on purpose. ---

    // A backslash is not in INVALID_OBJECTNAME_CHARACTERS but IS in
    // INVALID_LONGPACKAGE_CHARACTERS, so this case is what proves the PATH is checked against the
    // engine's package rules and not only the name against its object rules.
    {
        const FString BadPath = FString::Printf(TEXT("%s/Bad\\Leaf"), SafetyFolder);
        ExpectBlueprintTargetRefused(*this, BadPath, SafetyUniqueName(TEXT("D")), BadPath,
            TEXT("backslash path"));
    }

    // No leading slash: a short package name, which CreatePackage would resolve somewhere the
    // caller never named.
    {
        const FString BadPath = TEXT("PinWrightTests/NoLeadingSlash");
        ExpectBlueprintTargetRefused(*this, BadPath, SafetyUniqueName(TEXT("E")), BadPath,
            TEXT("unrooted path"));
    }

    // An unmounted root, which IsValidLongPackageName rejects even with read-only roots included.
    {
        const FString BadPath = TEXT("/NotAMountPoint/Thing");
        ExpectBlueprintTargetRefused(*this, BadPath, SafetyUniqueName(TEXT("F")), BadPath,
            TEXT("unmounted path"));
    }

    // CONTROL. Without this, a helper that refused every input would satisfy every case above.
    // It creates an empty UPackage and nothing else - no asset, no dirty flag, nothing saved.
    {
        const FString Name = SafetyUniqueName(TEXT("Control"));
        const FString PackagePath = SafetyJoin(GoodFolder, Name);

        UPackage* Package = nullptr;
        FString AssetObjectPath;
        bool bNameCollision = true;
        FString Error;

        const bool bPrepared = PrepareBlueprintPackageGuardingNameCollision(
            PackagePath, Name, Package, AssetObjectPath, bNameCollision, Error);

        TestTrue(*FString::Printf(TEXT("a well-formed path and bare name are accepted (%s)"),
            *Error), bPrepared);
        TestNotNull(TEXT("the accepted target yields a package"), Package);
        TestFalse(TEXT("the accepted target reports no name collision"), bNameCollision);
        TestEqual(TEXT("the accepted target reports PackagePath.AssetName"), AssetObjectPath,
            SafetyObjectPath(PackagePath, Name));
    }

    return true;
}

// ============================================================================
// McpCreateControlRigBlueprint refuses a target CreatePackage would die on
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetUtilsControlRigPackageTargetSafetyTest,
    "PinWright.Assets.AssetUtils.ControlRigCreateRefusesMalformedPathOrName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetUtilsControlRigPackageTargetSafetyTest::RunTest(const FString& Parameters)
{
    using namespace AssetUtilsPackageTargetSafetyHelpers;

    const FString GoodFolder(SafetyFolder);

    // This helper concatenates folder and name into the CreatePackage argument, so a "//" in
    // either half is the Fatal itself and is deliberately absent. A single interior slash is
    // refused by the object-name rule and composes a merely-nested path on a reverted build.
    ExpectControlRigTargetRefused(*this, GoodFolder, TEXT("Sub/Leaf"), TEXT("interior-slash name"));

    // The folder is backslash-normalized before composition; the NAME never is, so a backslash
    // here is what reaches the composed path and proves it is checked against the engine's
    // package rules rather than only against object-name rules.
    ExpectControlRigTargetRefused(*this, GoodFolder, TEXT("Bad\\Leaf"), TEXT("backslash name"));

    // CONTROL. A well-formed pair must still create the asset - the guard sits above the create,
    // so a regression there would be indistinguishable from a broken helper without this.
    {
        const FString Name = SafetyUniqueName(TEXT("CRControl"));
        const FString PackagePath = SafetyJoin(GoodFolder, Name);
        ON_SCOPE_EXIT
        {
            CleanupTestAsset(PackagePath);
        };

        FString Error;
        UBlueprint* Created =
            McpCreateControlRigBlueprint(Name, GoodFolder, /*TargetSkeleton*/ nullptr, Error);

        // Hard failure, not a skip: ControlRig / ControlRigDeveloper / RigVM are unconditional
        // dependencies of this module (PinWright.Build.cs), so a null return here is a plugin
        // regression, never a host-content difference. Same stance as CRIRTestHelpers.h.
        TestNotNull(*FString::Printf(
            TEXT("a well-formed name and folder still create the Control Rig BP (%s)"), *Error),
            Created);
    }

    return true;
}
