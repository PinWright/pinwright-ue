// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for UPinWrightSettings port derivation: DerivePortFromPath / ResolveHttpPort
#include "Misc/AutomationTest.h"
#include "PinWrightSettings.h"
#include "UObject/Package.h"

// Reserved derived-port range: [DerivedPortBase, DerivedPortBase + DerivedPortSlotCount - 1] = [19880, 30119].
namespace
{
    constexpr int32 DerivedPortRangeMin = 19880;
    constexpr int32 DerivedPortRangeMax = 30119;
}

// ============================================================================
// DerivePortFromPath - deterministic for a fixed path
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsPortDerivationDeterminismTest,
    "PinWright.infra.settings.PortDerivation.Determinism",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSettingsPortDerivationDeterminismTest::RunTest(const FString& Parameters)
{
    const int32 First = UPinWrightSettings::DerivePortFromPath(TEXT("C:/Projects/MyGame"));
    const int32 Second = UPinWrightSettings::DerivePortFromPath(TEXT("C:/Projects/MyGame"));

    TestEqual(TEXT("Same path derives same port across calls"), First, Second);

    return true;
}

// ============================================================================
// DerivePortFromPath - invariant to case and slash style (normalize + lowercase)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsPortDerivationCaseSlashInvarianceTest,
    "PinWright.infra.settings.PortDerivation.CaseSlashInvariance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSettingsPortDerivationCaseSlashInvarianceTest::RunTest(const FString& Parameters)
{
    // NormalizeDirectoryName converts backslashes to '/' and strips a single trailing '/'
    // (drive-root ":/" excepted), then DerivePortFromPath lowercases. All three forms below
    // therefore normalize to "c:/projects/mygame" and must hash to the same port.
    const int32 Forward = UPinWrightSettings::DerivePortFromPath(TEXT("C:/Projects/MyGame"));
    const int32 BackslashTrailing = UPinWrightSettings::DerivePortFromPath(TEXT("C:\\Projects\\MyGame\\"));
    const int32 Lowercase = UPinWrightSettings::DerivePortFromPath(TEXT("c:/projects/mygame"));

    TestEqual(TEXT("Backslash + trailing slash form matches forward-slash form"), BackslashTrailing, Forward);
    TestEqual(TEXT("Lowercase form matches mixed-case form"), Lowercase, Forward);

    return true;
}

// ============================================================================
// DerivePortFromPath - result always within the reserved port range
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsPortDerivationRangeBoundsTest,
    "PinWright.infra.settings.PortDerivation.RangeBounds",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSettingsPortDerivationRangeBoundsTest::RunTest(const FString& Parameters)
{
    const TArray<FString> CannedPaths = {
        TEXT("C:/Projects/MyGame"),
        TEXT("D:/Work/Another/Project"),
        TEXT("/home/user/projects/sample-game"),
        TEXT("E:/a"),
        TEXT("C:/Work/SampleProject")
    };

    for (const FString& Path : CannedPaths)
    {
        const int32 Port = UPinWrightSettings::DerivePortFromPath(Path);
        TestTrue(FString::Printf(TEXT("Port >= %d for '%s'"), DerivedPortRangeMin, *Path),
            Port >= DerivedPortRangeMin);
        TestTrue(FString::Printf(TEXT("Port <= %d for '%s'"), DerivedPortRangeMax, *Path),
            Port <= DerivedPortRangeMax);
    }

    return true;
}

// ============================================================================
// DerivePortFromPath - distinct paths derive distinct ports
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsPortDerivationDistinctnessTest,
    "PinWright.infra.settings.PortDerivation.Distinctness",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSettingsPortDerivationDistinctnessTest::RunTest(const FString& Parameters)
{
    // These two canned paths share no normalized prefix; the StrCrc32 hash makes a collision
    // within a 10240-slot space extremely unlikely. Verified non-colliding at authoring time.
    const int32 PortA = UPinWrightSettings::DerivePortFromPath(TEXT("C:/Projects/GameAlpha"));
    const int32 PortB = UPinWrightSettings::DerivePortFromPath(TEXT("C:/Projects/GameBravo"));

    // TestNotEqual has no int32 overload; assert the inequality directly via TestTrue.
    TestTrue(TEXT("Distinct project paths derive distinct ports"), PortA != PortB);

    return true;
}

// ============================================================================
// ResolveHttpPort - returns the fixed HttpPort when auto-derive is disabled
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsResolveHttpPortOverrideTest,
    "PinWright.infra.settings.PortDerivation.ResolveOverride",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSettingsResolveHttpPortOverrideTest::RunTest(const FString& Parameters)
{
    UPinWrightSettings* Inst =
        NewObject<UPinWrightSettings>(GetTransientPackage());
    TestTrue(TEXT("Settings instance created"), Inst != nullptr);
    if (!Inst)
    {
        return false;
    }

    Inst->bAutoDerivePort = false;

    Inst->HttpPort = 20000;
    TestEqual(TEXT("Override returns HttpPort 20000"),
        UPinWrightSettings::ResolveHttpPort(Inst), 20000);

    Inst->HttpPort = 1;
    TestEqual(TEXT("Override returns HttpPort 1"),
        UPinWrightSettings::ResolveHttpPort(Inst), 1);

    return true;
}

// ============================================================================
// ResolveHttpPort - derives from the live project dir when auto-derive is enabled
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSettingsResolveHttpPortAutoTest,
    "PinWright.infra.settings.PortDerivation.ResolveAuto",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSettingsResolveHttpPortAutoTest::RunTest(const FString& Parameters)
{
    UPinWrightSettings* Inst =
        NewObject<UPinWrightSettings>(GetTransientPackage());
    TestTrue(TEXT("Settings instance created"), Inst != nullptr);
    if (!Inst)
    {
        return false;
    }

    Inst->bAutoDerivePort = true;

    // With auto-derive on, ResolveHttpPort ignores HttpPort and derives from the project dir;
    // the result must land in the reserved range regardless of where the project lives.
    const int32 Resolved = UPinWrightSettings::ResolveHttpPort(Inst);
    TestTrue(FString::Printf(TEXT("Resolved auto port >= %d"), DerivedPortRangeMin),
        Resolved >= DerivedPortRangeMin);
    TestTrue(FString::Printf(TEXT("Resolved auto port <= %d"), DerivedPortRangeMax),
        Resolved <= DerivedPortRangeMax);

    return true;
}
