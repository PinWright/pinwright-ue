// Copyright (c) 2026 Alexander Penkin. MIT License.

// Safety tests for FCodeFunctionResolver defensive validation and FPluginState cache invalidation.
#include "Misc/AutomationTest.h"
#include "Compiler/CodeFunctionResolver.h"

#include "Kismet/KismetMathLibrary.h"
#include "UObject/Class.h"

#include "State/PluginState.h"

// RAII guard that saves a UClass's ClassFlags on construction and restores them
// on destruction — ensures flags are always restored even if a test assertion
// triggers an early return or exception.
struct FClassFlagGuard
{
    UClass* Class;
    EClassFlags OriginalFlags;
    FClassFlagGuard(UClass* InClass) : Class(InClass), OriginalFlags(InClass->ClassFlags) {}
    ~FClassFlagGuard() { Class->ClassFlags = OriginalFlags; }
};

// ============================================================================
// FCodeFunctionResolver — ResolveFunction rejects null class
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodeFunctionResolverNullClassTest,
    "PinWright.core.code_function_resolver.RejectsNullClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodeFunctionResolverNullClassTest::RunTest(const FString& Parameters)
{
    FCodeFunctionResolver Resolver;
    UFunction* Found = Resolver.ResolveFunction(nullptr, TEXT("Add_DoubleDouble"));
    TestTrue(TEXT("ResolveFunction(nullptr, ...) returns null without crashing"), Found == nullptr);
    return true;
}

// ============================================================================
// FCodeFunctionResolver — ResolveFunction rejects stale-flagged class
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodeFunctionResolverStaleClassTest,
    "PinWright.core.code_function_resolver.RejectsStaleClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodeFunctionResolverStaleClassTest::RunTest(const FString& Parameters)
{
    UClass* MathLib = UKismetMathLibrary::StaticClass();
    FClassFlagGuard Guard(MathLib);

    // Inject the stale hot-reload flag
    MathLib->ClassFlags = static_cast<EClassFlags>(MathLib->ClassFlags | CLASS_NewerVersionExists);

    FCodeFunctionResolver Resolver;
    UFunction* Found = Resolver.ResolveFunction(MathLib, TEXT("Add_DoubleDouble"));
    TestTrue(TEXT("ResolveFunction rejects a class with CLASS_NewerVersionExists"), Found == nullptr);

    // Guard restores flags automatically
    return true;
}

// ============================================================================
// FCodeFunctionResolver — ResolveFunctionAcrossLibraries skips stale entries
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCodeFunctionResolverLibrarySkipsStaleTest,
    "PinWright.core.code_function_resolver.LibrarySkipsStale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCodeFunctionResolverLibrarySkipsStaleTest::RunTest(const FString& Parameters)
{
    UClass* MathLib = UKismetMathLibrary::StaticClass();

    // Phase 1: flag the library as stale and verify the function is not found
    {
        FClassFlagGuard Guard(MathLib);
        MathLib->ClassFlags = static_cast<EClassFlags>(MathLib->ClassFlags | CLASS_NewerVersionExists);

        FCodeFunctionResolver Resolver;
        UFunction* NotFound = Resolver.ResolveFunctionAcrossLibraries(TEXT("Add_DoubleDouble"));
        TestTrue(TEXT("Add_DoubleDouble not found when owning library is stale"), NotFound == nullptr);
    }
    // Guard destructor has restored the original flags

    // Phase 2: verify the function resolves normally after flag restoration
    {
        FCodeFunctionResolver Resolver;
        UFunction* Found = Resolver.ResolveFunctionAcrossLibraries(TEXT("Add_DoubleDouble"));
        TestTrue(TEXT("Add_DoubleDouble found after stale flag is removed"), Found != nullptr);
    }
    return true;
}

// ============================================================================
// FPluginState — InvalidateFunctionLibraryCache forces rescan
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPluginStateInvalidateFunctionLibraryCacheTest,
    "PinWright.core.code_function_resolver.InvalidateFunctionLibraryCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPluginStateInvalidateFunctionLibraryCacheTest::RunTest(const FString& Parameters)
{
    FPluginState& State = FPluginState::Get();

    // First call — triggers the initial scan
    const TMap<FString, UClass*>& Libraries1 = State.GetScannedFunctionLibraries();
    const int32 Count1 = Libraries1.Num();
    TestTrue(TEXT("Initial scan returns at least one function library"), Count1 > 0);

    // Invalidate the cache
    State.InvalidateFunctionLibraryCache();

    // Second call — should re-scan and produce a consistent result
    const TMap<FString, UClass*>& Libraries2 = State.GetScannedFunctionLibraries();
    const int32 Count2 = Libraries2.Num();
    TestTrue(TEXT("Rescan returns at least one function library"), Count2 > 0);
    // TObjectIterator<UClass> is a live walk — new function library classes may load
    // between scans (side effects of other tests). Use a tolerance instead of exact equality.
    TestTrue(TEXT("Rescan count is within tolerance of initial scan count"),
        FMath::Abs(Count2 - Count1) <= 20);
    return true;
}
