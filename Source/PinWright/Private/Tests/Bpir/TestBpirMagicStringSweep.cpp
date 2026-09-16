// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-bpir-magic-strings-and-duplications (sub-item 3) —
// NormalizeEventName mapping table coverage.
//
// Counterfactual: If the new TMap-based NormalizeEventName body is reverted to
// the if-chain and one of the entries is removed, the corresponding TestEqual
// fails because the lookup returns the input unchanged.
//
// Sub-item 5 (enum-equality routing) was deferred during the sprint: no
// enum-pin function-routing surface exists in the plugin, and the
// EqualEqual_EnumEnum alias resolved to no real UFunction. Nothing observable
// to test until the routing surface is reactivated, so no test is included
// here for sub-item 5.

#include "Misc/AutomationTest.h"

#include "Decompiler/BpirTextEmitter.h"


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirNormalizeEventNameMappingTest,
    "PinWright.bpir.magic_strings.NormalizeEventName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirNormalizeEventNameMappingTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("ReceiveBeginPlay maps to BeginPlay"),
        FBpirTextEmitter::NormalizeEventName(TEXT("ReceiveBeginPlay")),
        FString(TEXT("BeginPlay")));

    TestEqual(TEXT("ReceiveTick maps to Tick"),
        FBpirTextEmitter::NormalizeEventName(TEXT("ReceiveTick")),
        FString(TEXT("Tick")));

    TestEqual(TEXT("ReceiveActorBeginOverlap maps to ActorBeginOverlap"),
        FBpirTextEmitter::NormalizeEventName(TEXT("ReceiveActorBeginOverlap")),
        FString(TEXT("ActorBeginOverlap")));

    TestEqual(TEXT("Non-mapped name passes through unchanged"),
        FBpirTextEmitter::NormalizeEventName(TEXT("MyCustomEvent")),
        FString(TEXT("MyCustomEvent")));

    return true;
}
