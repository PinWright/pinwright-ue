// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/PropertyImport.h"

#include "Components/TextBlock.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Text.h"
#include "UObject/Package.h"
#include "UObject/TextProperty.h"
#include "UObject/UObjectGlobals.h"

// An FText that already has a namespace/key keeps that identity when set from an
// identity-less value, but must store the parsed payload of a text macro, not its syntax
// (INVTEXT("x") -> "x"). Drives ApplyJsonValueToProperty, the path property.set and
// widget.set share.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyImportTextMacrosOnKeyedTextTest,
    "PinWright.utils.property_import.TextMacrosOnKeyedText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyImportTextMacrosOnKeyedTextTest::RunTest(const FString& Parameters)
{
    UTextBlock* Host = NewObject<UTextBlock>(GetTransientPackage());
    FTextProperty* TextProp = Host ? CastField<FTextProperty>(Host->GetClass()->FindPropertyByName(TEXT("Text"))) : nullptr;
    TestNotNull(TEXT("UTextBlock::Text property found"), TextProp);
    if (!TextProp)
    {
        return false;
    }

    const FString ExistingNamespace = TEXT("PwTextMacroNs");
    const FString ExistingKey = TEXT("PwTextMacroKey");

    // Resets the host to a keyed value, applies Input, and checks the stored source string
    // and identity.
    auto ApplyAndCheck = [&](const FString& Input, const FString& ExpectedSource,
                             const FString& ExpectedNamespace, const FString& ExpectedKey)
    {
        TextProp->SetPropertyValue_InContainer(Host,
            FText::ChangeKey(FTextKey(*ExistingNamespace), FTextKey(*ExistingKey), FText::FromString(TEXT("Old"))));

        FString Error;
        const bool bApplied = ApplyJsonValueToProperty(Host, TextProp, MakeShared<FJsonValueString>(Input), Error);
        TestTrue(FString::Printf(TEXT("%s applied: %s"), *Input, *Error), bApplied);

        const FText Stored = TextProp->GetPropertyValue_InContainer(Host);
        TestEqual(FString::Printf(TEXT("%s stores its source string"), *Input), Stored.ToString(), ExpectedSource);
        TestEqual(FString::Printf(TEXT("%s namespace"), *Input),
                  FTextInspector::GetNamespace(Stored).Get(FString()), ExpectedNamespace);
        TestEqual(FString::Printf(TEXT("%s key"), *Input),
                  FTextInspector::GetKey(Stored).Get(FString()), ExpectedKey);
        return Stored;
    };

    ApplyAndCheck(TEXT("INVTEXT(\"Fierce Mole\")"), TEXT("Fierce Mole"), ExistingNamespace, ExistingKey);
    ApplyAndCheck(TEXT("LOCTEXT(\"NoNsKey\", \"Loc Source\")"), TEXT("Loc Source"), ExistingNamespace, ExistingKey);
    ApplyAndCheck(TEXT("Plain Value"), TEXT("Plain Value"), ExistingNamespace, ExistingKey);
    const FText Localized = ApplyAndCheck(
        TEXT("NSLOCTEXT(\"PwNewNs\", \"PwNewKey\", \"New Source\")"), TEXT("New Source"), TEXT("PwNewNs"), TEXT("PwNewKey"));

    // Readback equality: the exported value re-applies to the same text and identity.
    FString Exported;
    TextProp->ExportTextItem_Direct(Exported, &Localized, nullptr, nullptr, PPF_None);
    ApplyAndCheck(Exported, TEXT("New Source"), TEXT("PwNewNs"), TEXT("PwNewKey"));

    const FText InvStored = ApplyAndCheck(TEXT("INVTEXT(\"Fierce Mole\")"), TEXT("Fierce Mole"), ExistingNamespace, ExistingKey);
    FString InvExported;
    TextProp->ExportTextItem_Direct(InvExported, &InvStored, nullptr, nullptr, PPF_None);
    ApplyAndCheck(InvExported, TEXT("Fierce Mole"), ExistingNamespace, ExistingKey);
    return true;
}
