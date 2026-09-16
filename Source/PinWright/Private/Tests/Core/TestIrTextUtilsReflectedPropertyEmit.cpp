// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "TestIrTextUtilsReflectedPropertyEmit.h"

#include "IrCore/IrTextUtils.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"


namespace
{
FProperty* ResolveFixtureProperty(FAutomationTestBase& Test, FName PropertyName)
{
    FProperty* Property = UTestIrTextUtilsReflectedPropertyHost::StaticClass()->FindPropertyByName(PropertyName);
    Test.TestNotNull(*FString::Printf(TEXT("%s property resolved"), *PropertyName.ToString()), Property);
    return Property;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIrTextUtilsReflectedPropertyEmitTest,
    "PinWright.core.ir_text.reflected_property.Emit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIrTextUtilsReflectedPropertyEmitTest::RunTest(const FString& Parameters)
{
    UTestIrTextUtilsReflectedPropertyHost* Host =
        NewObject<UTestIrTextUtilsReflectedPropertyHost>(GetTransientPackage());
    TestNotNull(TEXT("Fixture host created"), Host);
    if (!Host)
    {
        return false;
    }

    UTestIrTextUtilsReflectedObject* RefObject =
        NewObject<UTestIrTextUtilsReflectedObject>(Host, TEXT("RefObject"));
    TestNotNull(TEXT("Fixture object ref created"), RefObject);
    if (!RefObject)
    {
        return false;
    }

    const FSoftObjectPath SoftObjectPath(TEXT("/Game/IrTextUtils/SoftAsset.SoftAsset"));
    const FSoftObjectPath SoftClassPath(UTestIrTextUtilsReflectedObject::StaticClass()->GetPathName());

    Host->BoolValue = true;
    Host->IntValue = -42;
    Host->FloatValue = 3.5f;
    Host->EnumValue = EIrTextUtilsReflectedEnum::Second;
    Host->ByteEnumValue = IrTextUtilsReflectedByteOne;
    Host->RawByteValue = 7;
    Host->ObjectValue = RefObject;
    Host->ClassValue = UTestIrTextUtilsReflectedObject::StaticClass();
    Host->SoftObjectValue = TSoftObjectPtr<UObject>(SoftObjectPath);
    Host->SoftClassValue = TSoftClassPtr<UObject>(SoftClassPath);
    Host->StringValue = TEXT("Alpha \"Beta\"");
    Host->NameValue = FName(TEXT("Name Value"));
    Host->TextValue = FText::FromString(TEXT("Text Value"));
    Host->StringArray = { TEXT("One"), TEXT("Two") };

    const auto FormatValue = [this, Host](FName PropertyName, const FReflectedFieldEmitOptions& Options)
    {
        FProperty* Property = ResolveFixtureProperty(*this, PropertyName);
        return FIrTextUtils::FormatReflectedPropertyValue(Host, nullptr, Property, Host, Options);
    };

    const auto FormatDefaultValue = [&FormatValue](FName PropertyName)
    {
        FReflectedFieldEmitOptions Options;
        return FormatValue(PropertyName, Options);
    };

    TestEqual(TEXT("Bool property emits keyword"), FormatDefaultValue(TEXT("BoolValue")), FString(TEXT("true")));
    TestEqual(TEXT("Int property emits signed integer"), FormatDefaultValue(TEXT("IntValue")), FString(TEXT("-42")));
    TestEqual(TEXT("Float property emits sanitized float"),
        FormatDefaultValue(TEXT("FloatValue")),
        FString::SanitizeFloat(Host->FloatValue));

    TestEqual(TEXT("Enum property emits quoted enum name"),
        FormatDefaultValue(TEXT("EnumValue")),
        FIrTextUtils::Quote(StaticEnum<EIrTextUtilsReflectedEnum>()->GetNameStringByValue(
            static_cast<int64>(Host->EnumValue))));
    // UE 5.8 stopped emitting a StaticEnum<>() specialization for namespace-scoped
    // (non-enum-class) UENUMs, so resolve the UEnum through the reflected property
    // instead. The TEnumAsByte<> UPROPERTY reflects as an FByteProperty carrying the
    // UEnum; this path is correct on every supported engine (5.3-5.8).
    const FByteProperty* ByteEnumProp =
        CastField<FByteProperty>(ResolveFixtureProperty(*this, TEXT("ByteEnumValue")));
    TestNotNull(TEXT("ByteEnumValue is a byte enum property"), ByteEnumProp);
    UEnum* ByteEnum = ByteEnumProp ? ByteEnumProp->Enum : nullptr;
    TestNotNull(TEXT("ByteEnumValue resolves its UEnum"), ByteEnum);
    if (ByteEnum)
    {
        TestEqual(TEXT("Byte enum property emits quoted enum name"),
            FormatDefaultValue(TEXT("ByteEnumValue")),
            FIrTextUtils::Quote(ByteEnum->GetNameStringByValue(
                static_cast<int64>(IrTextUtilsReflectedByteOne))));
    }
    TestEqual(TEXT("Raw byte property emits integer"), FormatDefaultValue(TEXT("RawByteValue")), FString(TEXT("7")));

    TestEqual(TEXT("Object property emits quoted path"),
        FormatDefaultValue(TEXT("ObjectValue")),
        FIrTextUtils::Quote(RefObject->GetPathName()));
    TestEqual(TEXT("Class property emits quoted class path"),
        FormatDefaultValue(TEXT("ClassValue")),
        FIrTextUtils::Quote(UTestIrTextUtilsReflectedObject::StaticClass()->GetPathName()));
    TestEqual(TEXT("Soft object property emits quoted soft path"),
        FormatDefaultValue(TEXT("SoftObjectValue")),
        FIrTextUtils::Quote(SoftObjectPath.ToString()));
    TestEqual(TEXT("Soft class property emits quoted soft class path"),
        FormatDefaultValue(TEXT("SoftClassValue")),
        FIrTextUtils::Quote(SoftClassPath.ToString()));

    const FString StringFallback = FormatDefaultValue(TEXT("StringValue"));
    TestTrue(TEXT("String fallback is quoted"), StringFallback.StartsWith(TEXT("\"")));
    TestTrue(TEXT("String fallback carries value"), StringFallback.Contains(TEXT("Alpha")));

    const FString NameFallback = FormatDefaultValue(TEXT("NameValue"));
    TestTrue(TEXT("Name fallback is quoted"), NameFallback.StartsWith(TEXT("\"")));
    TestTrue(TEXT("Name fallback carries value"), NameFallback.Contains(TEXT("Name Value")));

    const FString TextFallback = FormatDefaultValue(TEXT("TextValue"));
    TestTrue(TEXT("Text fallback is quoted"), TextFallback.StartsWith(TEXT("\"")));
    TestTrue(TEXT("Text fallback carries value"), TextFallback.Contains(TEXT("Text Value")));

    FReflectedFieldEmitOptions BracketArrayOptions;
    BracketArrayOptions.bEmitArraysAsBracketList = true;
    TestEqual(TEXT("MGIR array option emits bracket list"),
        FormatValue(TEXT("StringArray"), BracketArrayOptions),
        FString(TEXT("[\"One\",\"Two\"]")));

    const FString DefaultArrayValue = FormatDefaultValue(TEXT("StringArray"));
    TestFalse(TEXT("Default array option does not emit MGIR bracket list"),
        DefaultArrayValue.StartsWith(TEXT("[")));
    TestTrue(TEXT("Default array option falls back to quoted ExportText"),
        DefaultArrayValue.StartsWith(TEXT("\"")));

    UTestIrTextUtilsReflectedPropertyHost* LoopHost =
        NewObject<UTestIrTextUtilsReflectedPropertyHost>(GetTransientPackage());
    TestNotNull(TEXT("Loop fixture host created"), LoopHost);
    if (!LoopHost)
    {
        return false;
    }

    LoopHost->AlphaSortValue = 1;
    LoopHost->ZetaSortValue = 2;
    LoopHost->ExplicitValue = 3;
    LoopHost->RejectedByNameValue = 4;
    LoopHost->VectorValue = FVector(1.0, 2.0, 3.0);

    TSet<FName> ExplicitProperties;
    ExplicitProperties.Add(TEXT("ExplicitValue"));

    FReflectedFieldEmitOptions LoopOptions;
    LoopOptions.FieldSeparator = TEXT(" => ");

    TArray<FString> Fields;
    FIrTextUtils::AppendReflectedFields(
        UTestIrTextUtilsReflectedPropertyHost::StaticClass(),
        LoopHost,
        UTestIrTextUtilsReflectedPropertyHost::StaticClass()->GetDefaultObject(),
        LoopHost,
        ExplicitProperties,
        [](const FStructProperty* StructProperty)
        {
            return StructProperty && StructProperty->GetFName() == FName(TEXT("VectorValue"));
        },
        [](FName PropertyName)
        {
            return PropertyName == FName(TEXT("RejectedByNameValue"));
        },
        LoopOptions,
        Fields);

    const FString JoinedFields = FString::Join(Fields, TEXT("\n"));
    TestTrue(TEXT("AppendReflectedFields uses requested separator"),
        JoinedFields.Contains(TEXT("AlphaSortValue => 1")));
    TestFalse(TEXT("AppendReflectedFields does not use default separator when overridden"),
        JoinedFields.Contains(TEXT("AlphaSortValue: ")));
    TestFalse(TEXT("AppendReflectedFields skips explicit properties"),
        JoinedFields.Contains(TEXT("ExplicitValue")));
    TestFalse(TEXT("AppendReflectedFields skips default-identical properties"),
        JoinedFields.Contains(TEXT("DefaultIdenticalValue")));
    TestFalse(TEXT("AppendReflectedFields applies name reject predicate"),
        JoinedFields.Contains(TEXT("RejectedByNameValue")));
    TestFalse(TEXT("AppendReflectedFields applies struct reject predicate"),
        JoinedFields.Contains(TEXT("VectorValue")));

    const int32 AlphaIndex = Fields.IndexOfByPredicate([](const FString& Field)
    {
        return Field.StartsWith(TEXT("AlphaSortValue => "));
    });
    const int32 ZetaIndex = Fields.IndexOfByPredicate([](const FString& Field)
    {
        return Field.StartsWith(TEXT("ZetaSortValue => "));
    });
    TestTrue(TEXT("AppendReflectedFields emits sorted property names"),
        AlphaIndex != INDEX_NONE && ZetaIndex != INDEX_NONE && AlphaIndex < ZetaIndex);

    return true;
}

// Regression: B-decompile-struct-subfield-dropped. AppendReflectedFields must
// thread the real archetype/CDO default into the struct-export fallback so a
// struct-valued property's SUB-fields suppress against their per-field CDO
// defaults, not the property type's zero-value. Before the fix
// FormatReflectedPropertyValue exported the struct with a nullptr default, so
// (a) a non-default sub-field equal to its type's zero-value was silently dropped
// and (b) a sub-field left at a non-zero default was spuriously emitted — the
// exact inversion the ticket reports on PCG's bRandomizedPruning. The fixture is
// built in-code (no asset load): a UStruct with a bool defaulting true and an int
// defaulting 5, on a transient UObject host whose CDO carries those defaults.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIrTextUtilsReflectedStructSubfieldDefaultTest,
    "PinWright.core.ir_text.reflected_property.StructSubfieldDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIrTextUtilsReflectedStructSubfieldDefaultTest::RunTest(const FString& Parameters)
{
    UTestIrTextUtilsReflectedStructDefaultHost* Host =
        NewObject<UTestIrTextUtilsReflectedStructDefaultHost>(GetTransientPackage());
    if (!TestNotNull(TEXT("Struct-default fixture host created"), Host))
    {
        return false;
    }

    UObject* Cdo = UTestIrTextUtilsReflectedStructDefaultHost::StaticClass()->GetDefaultObject();
    if (!TestNotNull(TEXT("Struct-default fixture CDO resolved"), Cdo))
    {
        return false;
    }

    const auto EmitFields = [this, Host, Cdo]()
    {
        TArray<FString> Fields;
        FIrTextUtils::AppendReflectedFields(
            UTestIrTextUtilsReflectedStructDefaultHost::StaticClass(),
            Host,
            Cdo,
            Host,
            TSet<FName>(),
            [](const FStructProperty*) { return false; },
            [](FName) { return false; },
            FReflectedFieldEmitOptions(),
            Fields);
        return FString::Join(Fields, TEXT("\n"));
    };

    // Non-default bool (false) equals its type's zero-value; DefaultedInt stays at
    // its non-zero default (5). This is the reported inversion case.
    Host->SubStructValue.bDefaultTrueFlag = false;
    Host->SubStructValue.DefaultedInt = 5;

    const FString Joined = EmitFields();

    TestTrue(TEXT("struct property is emitted (differs from CDO overall)"),
        Joined.Contains(TEXT("SubStructValue")));
    // The fix: a non-default sub-field equal to its zero-value is emitted against
    // the real archetype default. Reverting to the nullptr default drops it.
    TestTrue(TEXT("non-default bool sub-field (false, default true) is emitted"),
        Joined.Contains(TEXT("bDefaultTrueFlag=False")));
    // The fix: a sub-field left at its non-zero default is suppressed. Reverting to
    // the nullptr default spuriously emits DefaultedInt=5.
    TestFalse(TEXT("defaulted non-zero sub-field is not spuriously emitted"),
        Joined.Contains(TEXT("DefaultedInt")));

    // Positive control: a genuinely-overridden non-zero sub-field must still be
    // emitted — the default-diffing must not over-suppress real changes.
    Host->SubStructValue.DefaultedInt = 9;
    const FString JoinedChanged = EmitFields();
    TestTrue(TEXT("overridden non-default sub-field is still emitted"),
        JoinedChanged.Contains(TEXT("DefaultedInt=9")));

    return true;
}
