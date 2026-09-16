// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestGasCreateAbilitySetPathSafety.cpp - regression coverage for the gas.create_ability_set site
// of B-createpackage-unvalidated-paths-plugin-wide.
//
// WHAT WAS WRONG. gas.create_ability_set took `setPath` (or its `assetPath` alias) straight off the
// wire, prepended "/Game/" when IsValidMountPoint rejected the root, and handed the result to
// LoadObject and then to CreatePackage without ever checking it against the engine's package
// rules. CreatePackage (UObjectGlobals.cpp:1094-1096) logs at **Fatal** on a name containing "//",
// and Fatal is not compiled out in any configuration: the call does not fail, the editor PROCESS
// dies, taking every unsaved package in it. Two argument shapes reached it - a caller "//" copied
// through untouched, and any unmounted rooted path, because the mount-point fallback manufactures
// the double slash itself ("/NotAMount/X" -> "/Game//NotAMount/X").
//
// THE CHECK SITS ABOVE THE LoadObject, NOT JUST ABOVE CreatePackage, AND THAT IS NOT COSMETIC.
// The existence check between them is a second door to the same Fatal: on a path carrying no '.',
// StaticLoadObjectInternal retries as "<path>.<shortname>", and ResolveName2 then calls
// CreatePackage on the package half itself (UObjectGlobals.cpp:1297-1311). Moving the check below
// the LoadObject would leave the verb just as lethal while looking guarded.
//
// WHY THIS TEST CANNOT DRIVE THE CRASH ON EITHER BUILD. A Fatal takes the test host down with it,
// and a suite that dies mid-queue is an absence of a signal rather than a failure (the
// DID_NOT_COMPLETE state in the plugin's testing notes). So the refusal case below is driven with
// a `setPath` that NAMES AN EXISTING ASSET through its object-path spelling
// ("/Engine/BasicShapes/Cube.Cube"): the '.' is in INVALID_LONGPACKAGE_CHARACTERS, so the fixed
// build refuses it INVALID_ARGUMENT above everything, while a build WITHOUT the fix resolves it at
// the LoadObject, answers `already_exists`, and returns - ABOVE the concatenation, having created
// and saved nothing. The reverted build therefore goes red on the TestEqual below with its process
// intact. Both builds leave CreatePackage unreached.
//
// WHAT IS DELIBERATELY NOT DRIVEN, AND WHY. The "//" members of the same class - the ones that
// actually killed an editor - are NOT exercised here. Unlike foliage.add_type, this verb exposes
// no second argument (no mesh, no parent class, no attribute set) that a pre-fix build could bail
// on first: `setPath` is the entire payload, so any "//" value reaches the Fatal on a reverted
// build and would end the suite host rather than report a red. The guard is a single
// FPackageName::IsValidLongPackageName call, and "//" and the invalid-character class are the same
// call's two rejections - so proving it fires on the case that can be driven safely is what is
// available here without handing a live editor a string that ends it. Do NOT "complete" the
// coverage by adding a "//" case.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"

#include "UObject/Object.h"
#include "UObject/UObjectGlobals.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace GasCreateAbilitySetPathSafetyHelpers
{
    // The engine asset both cases lean on. The refusal case needs it so a REVERTED build stops at
    // the handler's already_exists branch instead of at CreatePackage; the control needs it so a
    // FIXED build stops there too instead of creating an ability set inside Engine content. One
    // fixture, two different reasons, so one gate covers both.
    constexpr const TCHAR* CubePackagePath = TEXT("/Engine/BasicShapes/Cube");

    // The same string with the object-path suffix the handler's own LoadObject would append. The
    // '.' is in INVALID_LONGPACKAGE_CHARACTERS, which is what makes this both a legitimate refusal
    // and a path a pre-fix build resolves.
    constexpr const TCHAR* CubeObjectPath = TEXT("/Engine/BasicShapes/Cube.Cube");
}

// ============================================================================
// setPath is checked against the engine's package rules before anything can create a package
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGasCreateAbilitySetPathCheckedBeforePackageCreationTest,
    "PinWright.gas.create_ability_set.SetPathIsCheckedBeforePackageCreation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGasCreateAbilitySetPathCheckedBeforePackageCreationTest::RunTest(const FString& Parameters)
{
    using namespace GasCreateAbilitySetPathSafetyHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    // Exactly the call the handler makes on the control path. If it comes back null, neither case
    // below is safe to drive: the refusal case loses its reverted-build bail and the control would
    // write into Engine content.
    if (LoadObject<UObject>(nullptr, CubePackagePath) == nullptr)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("basic-shapes-cube-fixture-absent"),
            FString::Printf(TEXT("LoadObject('%s') returned null, so neither the refusal case's "
                                 "pre-fix bail nor the control's already_exists answer is "
                                 "guaranteed"), CubePackagePath));
        return true;
    }

    // REFUSAL. An object-path spelling carries a '.', which IsValidLongPackageName rejects.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("setPath"), CubeObjectPath);

        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("gas.create_ability_set"),
            TEXT("req-gas-create-ability-set-path-refusal"), Params, bSuccess, ErrorCode);

        if (ErrorCode == TEXT("GAS_NOT_AVAILABLE"))
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("gameplay-abilities-plugin-disabled"),
                TEXT("gas.create_ability_set answered GAS_NOT_AVAILABLE, so the path check "
                     "under it never ran"));
            return true;
        }

        TestFalse(TEXT("a setPath carrying an object-path suffix is refused"), bSuccess);
        // The discriminator against a reverted guard: pre-fix this same payload resolves the cube
        // at the LoadObject and answers success/already_exists, never INVALID_ARGUMENT.
        TestEqual(TEXT("it is refused as a caller argument error, not answered already_exists"),
            ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        // Asserted on this plugin's own wording rather than on the engine's reason text, which is
        // NSLOCTEXT and would make the assertion locale-dependent. The engine reason is appended
        // after this prefix by construction.
        TestTrue(TEXT("the refusal quotes the offending path and names the rule it broke"),
            Sink->Message.Contains(CubeObjectPath) &&
            Sink->Message.Contains(TEXT("is not a valid package path:")));
    }

    // CONTROL. Without this, a handler that refused every setPath would satisfy the case above.
    // Driven with the package-path spelling of the same asset, so it stops at the existence check
    // that sits BELOW the new guard - which is exactly the proof that a well-formed path got past
    // it - while still creating and saving nothing.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("setPath"), CubePackagePath);

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("gas.create_ability_set"),
            TEXT("req-gas-create-ability-set-path-control"), Params, bSuccess, Result, ErrorCode);

        if (!TestTrue(*FString::Printf(
                TEXT("a well-formed setPath passes the path check (error=%s: %s)"),
                *ErrorCode, *Sink->Message), bSuccess) || !Result.IsValid())
        {
            return true;
        }

        FString Status;
        TestTrue(TEXT("the control reaches the existence check below the guard"),
            Result->TryGetStringField(TEXT("status"), Status) &&
                Status == TEXT("already_exists"));
    }

    return true;
}
