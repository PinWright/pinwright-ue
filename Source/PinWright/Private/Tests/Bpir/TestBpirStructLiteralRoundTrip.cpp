// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestBpirStructLiteralRoundTrip.cpp - regression tests for FBpirValueResolver::GetLiteralText
// catching the latent positional-CSV bug for FVector/FRotator/FLinearColor literals.
//
// Counterfactual: without the BpirStructLiteralUtils helper, GetLiteralText("FVector(1.5,-2.25,3.75)")
// yields "1.5,-2.25,3.75" (positional CSV), which UScriptStruct::ImportText cannot bind
// to X=/Y=/Z= keys, so re-import yields (0,0,0) and the equality assertion fails.

#include "Misc/AutomationTest.h"

#include "Compiler/BpirStructLiteralUtils.h"
#include "Compiler/BpirValueResolver.h"
#include "Math/Color.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "UObject/Class.h"

namespace
{
    // Parse a pin-default text via UScriptStruct::ImportText into a fresh struct
    // instance. Returns true on a clean import (the cursor advanced past the buffer).
    template <typename StructT>
    bool ImportPinTextToStruct(const FString& PinText, StructT& OutValue)
    {
        UScriptStruct* Struct = TBaseStructure<StructT>::Get();
        if (!Struct)
        {
            return false;
        }

        OutValue = StructT();
        const TCHAR* Cursor = Struct->ImportText(*PinText, &OutValue, nullptr, PPF_None, GLog, Struct->GetName());
        return Cursor != nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirStructLiteralVectorRoundTripTest,
    "PinWright.bpir.struct_literal.Vector",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirStructLiteralVectorRoundTripTest::RunTest(const FString& Parameters)
{
    const FString Input = TEXT("FVector(1.5,-2.25,3.75)");
    const FString PinText = FBpirValueResolver::GetLiteralText(Input);

    FVector Roundtripped(ForceInitToZero);
    TestTrue(TEXT("FVector pin text imports cleanly"), ImportPinTextToStruct<FVector>(PinText, Roundtripped));

    TestEqual(TEXT("FVector.X round-trips"), Roundtripped.X, 1.5, 1e-3);
    TestEqual(TEXT("FVector.Y round-trips"), Roundtripped.Y, -2.25, 1e-3);
    TestEqual(TEXT("FVector.Z round-trips"), Roundtripped.Z, 3.75, 1e-3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirStructLiteralLinearColorRoundTripTest,
    "PinWright.bpir.struct_literal.LinearColor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirStructLiteralLinearColorRoundTripTest::RunTest(const FString& Parameters)
{
    const FString Input = TEXT("FLinearColor(0.25,0.5,0.75,0.875)");
    const FString PinText = FBpirValueResolver::GetLiteralText(Input);

    FLinearColor Roundtripped(ForceInitToZero);
    TestTrue(TEXT("FLinearColor pin text imports cleanly"), ImportPinTextToStruct<FLinearColor>(PinText, Roundtripped));

    TestEqual(TEXT("FLinearColor.R round-trips"), Roundtripped.R, 0.25f, 1e-3f);
    TestEqual(TEXT("FLinearColor.G round-trips"), Roundtripped.G, 0.5f, 1e-3f);
    TestEqual(TEXT("FLinearColor.B round-trips"), Roundtripped.B, 0.75f, 1e-3f);
    TestEqual(TEXT("FLinearColor.A round-trips"), Roundtripped.A, 0.875f, 1e-3f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirStructLiteralRotatorPinDefaultTest,
    "PinWright.bpir.struct_literal.RotatorPinDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirStructLiteralRotatorPinDefaultTest::RunTest(const FString& Parameters)
{
    // Calls the production helper directly; asserting the emitted pin text matches
    // K2's IsStringValidRotator/IsStringValidVector primary CSV grammar (no parens,
    // no `Pitch=`/`Yaw=`/`Roll=` keys), which the previous ExportText round-trip
    // form failed and silently zeroed the pin.
    const FString Input = TEXT("FRotator(11.5,22.5,33.5)");
    FString PinText;
    const bool bFormatted = BpirStructLiteralUtils::TryFormatPositionalStructLiteralAsPinText(Input, PinText);
    TestTrue(TEXT("Helper formatted FRotator literal"), bFormatted);

    TestFalse(TEXT("Pin text does not contain Pitch= (rejected by K2 validator)"), PinText.Contains(TEXT("Pitch=")));
    TestFalse(TEXT("Pin text has no parens (positional CSV form)"), PinText.Contains(TEXT("(")));

    TArray<FString> Parts;
    PinText.ParseIntoArray(Parts, TEXT(","), false);
    TestEqual(TEXT("Pin text splits into 3 CSV components"), Parts.Num(), 3);
    if (Parts.Num() == 3)
    {
        const double Pitch = FCString::Atod(*Parts[0]);
        const double Yaw = FCString::Atod(*Parts[1]);
        const double Roll = FCString::Atod(*Parts[2]);
        TestEqual(TEXT("CSV[0] = pitch"), Pitch, 11.5, 1e-3);
        TestEqual(TEXT("CSV[1] = yaw"), Yaw, 22.5, 1e-3);
        TestEqual(TEXT("CSV[2] = roll"), Roll, 33.5, 1e-3);
    }
    return true;
}
