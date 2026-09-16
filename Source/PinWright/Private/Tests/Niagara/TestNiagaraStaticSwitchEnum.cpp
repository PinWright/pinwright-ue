// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for enum static-switch value resolution, the write path behind
// niagara.set_static_switch.
//
// Two defects, one mechanism:
//   B-niagara-static-switch-enum-display-name — the resolver looked up the AUTHORED entry
//     name only, so every label the Niagara editor shows for a user-defined enum was rejected
//     with INVALID_VALUE, and the numeric fallback was never range-checked.
//   B-niagara-static-switch-enum-value-map-undiscoverable — the integer it did accept could
//     not be derived from any published read, so a caller had to guess a branch.
//
// The fixture is a synthetic UUserDefinedEnum shaped like the ones Niagara's stock modules
// use for their switches: authored entry names are NewEnumeratorN, the labels live in
// DisplayNameMap, and the entry ORDER is a permutation of the name order — which is exactly
// what makes the branch index unguessable from the names alone. No Niagara graph is needed:
// the resolution helpers take the UEnum directly.

#include "Misc/AutomationTest.h"

#include "Handlers/Niagara/NiagaraEditTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/UserDefinedEnum.h"
#include "Compat/EngineVersionCompat.h"
#include "NiagaraNodeStaticSwitch.h"
#include "UObject/Package.h"

namespace
{
    // Authored name -> label, in the ORDER the entries sit in (index order). Note that the
    // digit in NewEnumeratorN does not match the index: entry 1 is NewEnumerator3, entry 2 is
    // NewEnumerator1. A real user-defined enum ends up this way as soon as someone reorders it
    // in the enum editor, because the authored names are minted once and never renumbered.
    const TCHAR* const PinWrightSwitchEnumAuthoredNames[] = { TEXT("NewEnumerator0"), TEXT("NewEnumerator3"), TEXT("NewEnumerator1") };
    const TCHAR* const PinWrightSwitchEnumLabels[] = { TEXT("Unset"), TEXT("Uniform"), TEXT("Random Uniform") };

    UUserDefinedEnum* MakePinWrightSwitchEnumFixture()
    {
        UUserDefinedEnum* Enum = NewObject<UUserDefinedEnum>(
            GetTransientPackage(),
            MakeUniqueObjectName(GetTransientPackage(), UUserDefinedEnum::StaticClass(), TEXT("PinWrightSwitchEnum")));
        if (!Enum)
        {
            return nullptr;
        }

        const FString EnumName = Enum->GetName();
        TArray<TPair<FName, int64>> Names;
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(PinWrightSwitchEnumAuthoredNames); ++Index)
        {
            Names.Emplace(
                FName(*FString::Printf(TEXT("%s::%s"), *EnumName, PinWrightSwitchEnumAuthoredNames[Index])),
                static_cast<int64>(Index));
        }
        // UE 5.8 inserted an EUnderlyingType parameter and changed the trailing
        // bAddMaxKeyIfMissing from a bool to UEnum::EAddMaxKeyIfMissing.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        if (!Enum->SetEnums(Names, UEnum::ECppForm::Namespaced, UEnum::EUnderlyingType::uint8,
                            EEnumFlags::None, UEnum::EAddMaxKeyIfMissing::Yes))
#else
        if (!Enum->SetEnums(Names, UEnum::ECppForm::Namespaced, EEnumFlags::None,
                            /*bAddMaxKeyIfMissing=*/true))
#endif
        {
            return nullptr;
        }

        for (int32 Index = 0; Index < UE_ARRAY_COUNT(PinWrightSwitchEnumAuthoredNames); ++Index)
        {
            Enum->DisplayNameMap.Add(FName(PinWrightSwitchEnumAuthoredNames[Index]), FText::FromString(PinWrightSwitchEnumLabels[Index]));
        }
        return Enum;
    }

    bool EncodePinWrightSwitchEnum(
        const UEnum* Enum,
        const TSharedPtr<FJsonValue>& Value,
        FString& OutPinDefault,
        FString& OutError,
        NiagaraStaticSwitch::FEnumSwitchOption& OutOption)
    {
        OutPinDefault.Reset();
        OutError.Reset();
        OutOption = NiagaraStaticSwitch::FEnumSwitchOption();
        return NiagaraStaticSwitch::EncodePinDefault(
            Value, ENiagaraStaticSwitchType::Enum, Enum, OutPinDefault, OutError, &OutOption);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraStaticSwitchEnumBranchResolutionTest,
    "PinWright.niagara.set_static_switch.EnumBranchResolution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraStaticSwitchEnumBranchResolutionTest::RunTest(const FString& Parameters)
{
    UUserDefinedEnum* Enum = MakePinWrightSwitchEnumFixture();
    if (!Enum)
    {
        AddError(TEXT("Could not build the UUserDefinedEnum fixture"));
        return false;
    }

    // Guard the fixture itself: every assertion below depends on the index/name permutation.
    TestEqual(TEXT("Fixture carries three entries plus the synthetic _MAX"), Enum->NumEnums(), 4);
    TestEqual(TEXT("Entry 1 is authored NewEnumerator3"), Enum->GetNameStringByIndex(1), FString(TEXT("NewEnumerator3")));
    TestEqual(TEXT("Entry 2 is authored NewEnumerator1"), Enum->GetNameStringByIndex(2), FString(TEXT("NewEnumerator1")));

    FString PinDefault;
    FString Error;
    NiagaraStaticSwitch::FEnumSwitchOption Option;

    // The label the Niagara editor shows. Before the fix the enum branch did
    // GenerateFullEnumName + GetIndexByName only, so this returned false with
    // "Enum value 'Random Uniform' not found" — no visible label was ever accepted.
    TestTrue(TEXT("Display name is accepted"),
        EncodePinWrightSwitchEnum(Enum, MakeShared<FJsonValueString>(TEXT("Random Uniform")), PinDefault, Error, Option));
    TestEqual(TEXT("Display name writes the authored entry name to the pin"), PinDefault, FString(TEXT("NewEnumerator1")));
    TestEqual(TEXT("Display name selects branch index 2"), Option.Index, 2);
    TestEqual(TEXT("Selected branch reports the editor's label"), Option.DisplayName, FString(TEXT("Random Uniform")));

    // Spaces and case are presentation, not identity.
    TestTrue(TEXT("Space- and case-insensitive display name is accepted"),
        EncodePinWrightSwitchEnum(Enum, MakeShared<FJsonValueString>(TEXT("  randomUNIFORM ")), PinDefault, Error, Option));
    TestEqual(TEXT("Collapsed display name selects the same branch"), Option.Index, 2);

    // The form that already worked keeps working, unchanged.
    TestTrue(TEXT("Authored entry name is still accepted"),
        EncodePinWrightSwitchEnum(Enum, MakeShared<FJsonValueString>(TEXT("NewEnumerator3")), PinDefault, Error, Option));
    TestEqual(TEXT("Authored entry name writes itself to the pin"), PinDefault, FString(TEXT("NewEnumerator3")));
    TestEqual(TEXT("Authored entry name selects branch index 1"), Option.Index, 1);

    // A number is still the branch index Niagara's own ResolveConstantValue produces, so index
    // 1 stays NewEnumerator3 rather than becoming the name with the matching digit.
    TestTrue(TEXT("Branch index is still accepted as a number"),
        EncodePinWrightSwitchEnum(Enum, MakeShared<FJsonValueNumber>(1.0), PinDefault, Error, Option));
    TestEqual(TEXT("Branch index 1 is the second entry, not NewEnumerator1"), PinDefault, FString(TEXT("NewEnumerator3")));

    // Before the fix GetNameStringByIndex(7) returned an empty string and the empty pin default
    // was written anyway; the compiler then clamped the unresolvable selector to branch 0, with
    // success reported at every layer.
    TestFalse(TEXT("Out-of-range branch index is refused"),
        EncodePinWrightSwitchEnum(Enum, MakeShared<FJsonValueNumber>(7.0), PinDefault, Error, Option));
    TestTrue(TEXT("Rejection publishes the whole branch table"),
        Error.Contains(TEXT("Random Uniform / NewEnumerator1 (index 2)")));

    // The trailing _MAX sentinel is a name, not a branch.
    TestFalse(TEXT("The _MAX sentinel index is refused"),
        EncodePinWrightSwitchEnum(Enum, MakeShared<FJsonValueNumber>(3.0), PinDefault, Error, Option));

    // A name this enum does not carry names the accepted ones in the rejection.
    TestFalse(TEXT("Unknown name is refused"),
        EncodePinWrightSwitchEnum(Enum, MakeShared<FJsonValueString>(TEXT("Sphere")), PinDefault, Error, Option));
    TestTrue(TEXT("Unknown-name rejection still names the rejected input"), Error.Contains(TEXT("'Sphere'")));
    TestTrue(TEXT("Unknown-name rejection publishes the whole branch table"),
        Error.Contains(TEXT("Uniform / NewEnumerator3 (index 1)")));

    // Anything that is neither a number nor a name used to fall through to index 0 and write a
    // branch nobody asked for.
    TestFalse(TEXT("A non-numeric, non-string value is refused rather than selecting branch 0"),
        EncodePinWrightSwitchEnum(Enum, MakeShared<FJsonValueBoolean>(true), PinDefault, Error, Option));

    // A switch with no enum set is still reported as such rather than resolving to anything.
    TestFalse(TEXT("A null enum is refused"),
        EncodePinWrightSwitchEnum(nullptr, MakeShared<FJsonValueNumber>(0.0), PinDefault, Error, Option));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraStaticSwitchEnumBranchTableTest,
    "PinWright.niagara.set_static_switch.EnumBranchTablePublished",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraStaticSwitchEnumBranchTableTest::RunTest(const FString& Parameters)
{
    UUserDefinedEnum* Enum = MakePinWrightSwitchEnumFixture();
    if (!Enum)
    {
        AddError(TEXT("Could not build the UUserDefinedEnum fixture"));
        return false;
    }

    // Degrades rather than crashing on a switch with no enum set.
    TestEqual(TEXT("A null enum publishes an empty branch table"),
        NiagaraStaticSwitch::MakeEnumOptionsJson(nullptr).Num(), 0);

    // This table is what no read published before: neither the enum's asset dump nor any
    // Niagara readback carried a value <-> name mapping, so the integer the setter wanted could
    // only be found by writing one candidate at a time and reading the echoed name back.
    const TArray<TSharedPtr<FJsonValue>> Options = NiagaraStaticSwitch::MakeEnumOptionsJson(Enum);
    if (!TestEqual(TEXT("Branch table lists every selectable entry and drops the _MAX sentinel"), Options.Num(), 3))
    {
        return false;
    }

    const TCHAR* const ExpectedSwitchEnumNames[] = { TEXT("NewEnumerator0"), TEXT("NewEnumerator3"), TEXT("NewEnumerator1") };
    const TCHAR* const ExpectedSwitchEnumLabels[] = { TEXT("Unset"), TEXT("Uniform"), TEXT("Random Uniform") };
    for (int32 Index = 0; Index < Options.Num(); ++Index)
    {
        const TSharedPtr<FJsonObject> Entry = Options[Index].IsValid() ? Options[Index]->AsObject() : TSharedPtr<FJsonObject>();
        if (!Entry.IsValid())
        {
            AddError(FString::Printf(TEXT("Branch table entry %d is not an object"), Index));
            continue;
        }
        TestEqual(*FString::Printf(TEXT("Entry %d publishes its branch index"), Index),
            static_cast<int32>(Entry->GetNumberField(TEXT("index"))), Index);
        TestEqual(*FString::Printf(TEXT("Entry %d publishes its authored name"), Index),
            Entry->GetStringField(TEXT("name")), FString(ExpectedSwitchEnumNames[Index]));
        TestEqual(*FString::Printf(TEXT("Entry %d publishes the editor's label"), Index),
            Entry->GetStringField(TEXT("displayName")), FString(ExpectedSwitchEnumLabels[Index]));
    }

    // The point of publishing it: index 2 is "Random Uniform", which neither the index nor the
    // authored name predicts. Without this table a caller reading NewEnumerator1 at index 2 has
    // no way to tell it is not the entry named with the digit 1.
    const TSharedPtr<FJsonObject> Third = Options[2]->AsObject();
    if (Third.IsValid())
    {
        TestNotEqual(TEXT("The authored name's digit is not the branch index"),
            Third->GetStringField(TEXT("name")), FString(TEXT("NewEnumerator2")));
    }

    return true;
}
