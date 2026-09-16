// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for board ticket B-widget-xml-import-rejects-export-braces.
//
// widget.export_xml serialises struct-valued widget properties as a brace/`=`
// hybrid `{Key=Value,...}` (JsonValueToAttrString, EJson::Object case in
// Handlers/UI/WidgetXmlUtils.h) — e.g. BrushColor="{R=0.03,G=0.05,B=0.09,A=1.0}".
// That form is neither valid JSON nor an ExportText paren literal, so the shared
// import coercion used to reject it and an exported tree could not be re-imported.
//
// The fix rewrites the hybrid into the ExportText literal `(Key=Value,...)` inside
// CoerceStringToJsonValueByProperty's FStructProperty branch, so ImportText_Direct
// can apply it. This test drives the exact two-step production path
// widget.import_xml uses (ApplyAttributeToObject -> CoerceStringToJsonValueByProperty
// then ApplyJsonValueToProperty) and asserts the struct value round-trips. It fails
// if the fix is reverted (apply returns false on the raw brace string).
//
// Fixture is built entirely in-code (a transient UImage) — no asset load, no
// example content required.
#include "Misc/AutomationTest.h"
#include "Utils/PropertyImport.h"
#include "Dom/JsonValue.h"
#include "Components/Image.h"
#include "Components/Widget.h"
#include "Slate/WidgetTransform.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWidgetXmlStructBraceImportTest,
    "PinWright.widget.import_xml.StructBraceRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FWidgetXmlStructBraceImportTest::RunTest(const FString& Parameters)
{
    UImage* Image = NewObject<UImage>(GetTransientPackage());
    if (!TestNotNull(TEXT("UImage fixture constructed"), Image))
    {
        return false;
    }

    // --- Flat struct: FLinearColor via the exporter's brace form (ticket repro) ---
    FProperty* ColorProp = Image->GetClass()->FindPropertyByName(TEXT("ColorAndOpacity"));
    if (!TestNotNull(TEXT("UImage.ColorAndOpacity property resolved"), ColorProp))
    {
        return false;
    }
    {
        const FString BraceForm = TEXT("{R=0.03,G=0.05,B=0.09,A=1.0}");
        TSharedPtr<FJsonValue> Coerced = CoerceStringToJsonValueByProperty(BraceForm, ColorProp);
        if (!TestTrue(TEXT("coercion produced a value for brace FLinearColor"), Coerced.IsValid()))
        {
            return false;
        }
        FString ApplyError;
        const bool bApplied = ApplyJsonValueToProperty(Image, ColorProp, Coerced, ApplyError);
        if (!bApplied)
        {
            AddError(FString::Printf(TEXT("apply brace FLinearColor failed: %s"), *ApplyError));
        }
        TestTrue(TEXT("apply brace FLinearColor succeeded"), bApplied);

        const FLinearColor Got = Image->GetColorAndOpacity();
        TestTrue(TEXT("FLinearColor round-tripped from brace form"),
            Got.Equals(FLinearColor(0.03f, 0.05f, 0.09f, 1.0f), 0.001f));
    }

    // --- Nested struct: FWidgetTransform (nested FVector2D sub-structs) ---
    // Proves the brace->paren rewrite recurses, matching the exporter's nested
    // output like Slot.LayoutData="{Offsets={...},Anchors={...}}".
    FProperty* XformProp = Image->GetClass()->FindPropertyByName(TEXT("RenderTransform"));
    if (!TestNotNull(TEXT("UWidget.RenderTransform property resolved"), XformProp))
    {
        return false;
    }
    {
        const FString BraceForm =
            TEXT("{Translation={X=10.0,Y=20.0},Scale={X=2.0,Y=3.0},Shear={X=0.0,Y=0.0},Angle=45.0}");
        TSharedPtr<FJsonValue> Coerced = CoerceStringToJsonValueByProperty(BraceForm, XformProp);
        if (!TestTrue(TEXT("coercion produced a value for nested brace struct"), Coerced.IsValid()))
        {
            return false;
        }
        FString ApplyError;
        const bool bApplied = ApplyJsonValueToProperty(Image, XformProp, Coerced, ApplyError);
        if (!bApplied)
        {
            AddError(FString::Printf(TEXT("apply nested brace FWidgetTransform failed: %s"), *ApplyError));
        }
        TestTrue(TEXT("apply nested brace FWidgetTransform succeeded"), bApplied);

        const FWidgetTransform Got = Image->GetRenderTransform();
        TestTrue(TEXT("nested Translation round-tripped"),
            FMath::IsNearlyEqual((double)Got.Translation.X, 10.0, 0.001)
            && FMath::IsNearlyEqual((double)Got.Translation.Y, 20.0, 0.001));
        TestTrue(TEXT("nested Scale round-tripped"),
            FMath::IsNearlyEqual((double)Got.Scale.X, 2.0, 0.001)
            && FMath::IsNearlyEqual((double)Got.Scale.Y, 3.0, 0.001));
        TestTrue(TEXT("nested Angle round-tripped"),
            FMath::IsNearlyEqual((double)Got.Angle, 45.0, 0.001));
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
