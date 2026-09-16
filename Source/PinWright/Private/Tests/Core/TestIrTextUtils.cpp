// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "IrCore/IrTextUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIrTextUtilsNameTokenRoundTripTest,
    "PinWright.core.ir_text.name_token.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIrTextUtilsNameTokenRoundTripTest::RunTest(const FString& Parameters)
{
    FString Error;
    FString Name;

    const FString BareToken = FIrTextUtils::FormatNameToken(TEXT("AnimGraph"));
    TestEqual(TEXT("Bare identifier stays bare"), BareToken, TEXT("AnimGraph"));
    TestTrue(TEXT("Bare token unwraps"),
        FIrTextUtils::TryUnwrapNameToken(BareToken, Name, Error));
    TestEqual(TEXT("Bare token value"), Name, TEXT("AnimGraph"));

    const FString DelimitedToken = FIrTextUtils::FormatNameToken(TEXT("/Game/ABP Sample.AnimGraph"));
    TestEqual(TEXT("Complex name uses backticks"), DelimitedToken, TEXT("`/Game/ABP Sample.AnimGraph`"));
    TestTrue(TEXT("Backtick token unwraps"),
        FIrTextUtils::TryUnwrapNameToken(DelimitedToken, Name, Error));
    TestEqual(TEXT("Backtick token value"), Name, TEXT("/Game/ABP Sample.AnimGraph"));

    const FString EscapedToken = FIrTextUtils::FormatNameToken(TEXT("State `A`"));
    TestEqual(TEXT("Backticks inside names are escaped"), EscapedToken, TEXT("`State \\`A\\``"));
    TestTrue(TEXT("Escaped backtick token unwraps"),
        FIrTextUtils::TryUnwrapNameToken(EscapedToken, Name, Error));
    TestEqual(TEXT("Escaped backtick token value"), Name, TEXT("State `A`"));

    TestFalse(TEXT("Double-quoted strings are not name tokens"),
        FIrTextUtils::TryUnwrapNameToken(TEXT("\"AnimGraph\""), Name, Error));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIrTextUtilsBacktickScanningTest,
    "PinWright.core.ir_text.scanning.IgnoresBacktickDelimitedNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIrTextUtilsBacktickScanningTest::RunTest(const FString& Parameters)
{
    const FString CommentInput = TEXT("entry anim_graph `Name # not comment` # real comment");
    TestEqual(TEXT("Comment marker inside backticks is ignored"),
        FIrTextUtils::StripTrailingComment(CommentInput),
        TEXT("entry anim_graph `Name # not comment`"));

    const TArray<FString> Parts = FIrTextUtils::SmartSplit(TEXT("from=`State A`, to=`State, B`, weight=1"), TEXT(','));
    TestEqual(TEXT("Top-level split count"), Parts.Num(), 3);
    if (Parts.Num() == 3)
    {
        TestEqual(TEXT("Comma inside backticks stays in token"), Parts[1], TEXT("to=`State, B`"));
    }

    FString Line = TEXT("state `Name @(not position)` @(12, -4)");
    FVector2D Position = FVector2D::ZeroVector;
    FString Error;
    TestTrue(TEXT("Position marker outside backticks is extracted"),
        FIrTextUtils::TryExtractPosition(Line, Position, Error));
    TestEqual(TEXT("Position-stripped line"), Line, TEXT("state `Name @(not position)`"));
    TestEqual(TEXT("Position X"), Position.X, 12.0);
    TestEqual(TEXT("Position Y"), Position.Y, -4.0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIrTextUtilsNumericLocalIdTest,
    "PinWright.core.ir_text.local_id.NumericFormat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIrTextUtilsNumericLocalIdTest::RunTest(const FString& Parameters)
{
    // Shared helper consumed by BPIR/MGIR/MSIR/CRIR decompilers. The bare
    // `nN` form is the on-the-wire token; the `%` reference sigil is added by
    // emission-site format strings. If this assertion ever breaks, the four
    // IR-family round-trip suites all break with it because their text output
    // diverges from the grammar.
    TestEqual(TEXT("Counter 0 yields n0"),
        FIrTextUtils::FormatNumericLocalId(0), TEXT("n0"));
    TestEqual(TEXT("Counter 7 yields n7"),
        FIrTextUtils::FormatNumericLocalId(7), TEXT("n7"));
    TestEqual(TEXT("No leading %% sigil — added at emission site"),
        FIrTextUtils::FormatNumericLocalId(42), TEXT("n42"));

    return true;
}
