// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for NiagaraDumpBuilder::BuildStaticSwitchInputs.
// Exercises the helper directly to prove it's exported from the plugin module
// and degrades gracefully on null / unset inputs. A full integration fixture
// (transient UNiagaraSystem + UNiagaraNodeFunctionCall + FunctionScript with a
// called UNiagaraGraph containing a UNiagaraNodeStaticSwitch) requires deep
// engine integration (graph compilation pipeline, script-source plumbing) that
// is not viable in a transient package without crashing post-load assertions;
// this thinner test still proves the symbol is exported and the helper is
// reachable, which is the main coverage gap the ticket calls out.
#include "Misc/AutomationTest.h"

#include "Handlers/Niagara/NiagaraDumpBuilder.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"

#include "Dom/JsonValue.h"
#include "Engine/UserDefinedEnum.h"
#include "Compat/EngineVersionCompat.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeStaticSwitch.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraDumpStaticSwitchInputsTest,
    "PinWright.niagara.dump.StaticSwitchInputs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpStaticSwitchInputsTest::RunTest(const FString& Parameters)
{
    // Null input: empty array.
    {
        const TArray<TSharedPtr<FJsonValue>> Result = NiagaraDumpBuilder::BuildStaticSwitchInputs(nullptr);
        TestEqual(TEXT("Null function-call node yields empty staticSwitchInputs array"), Result.Num(), 0);
    }

    // Function-call node with no FunctionScript / no called graph: empty array, no crash.
    {
        UNiagaraNodeFunctionCall* Node = NewObject<UNiagaraNodeFunctionCall>(GetTransientPackage());
        TestNotNull(TEXT("Transient UNiagaraNodeFunctionCall constructed"), Node);
        const TArray<TSharedPtr<FJsonValue>> Result = NiagaraDumpBuilder::BuildStaticSwitchInputs(Node);
        TestEqual(TEXT("Function-call without called graph yields empty staticSwitchInputs array"), Result.Num(), 0);
    }

    return true;
}

// Regression test for B-niagara-decode-pin-default-coerces-to-zero: the read-back path behind
// the `value` field of a staticSwitchInputs entry. DecodePinDefault used to answer branch 0 and
// return true for any enum name it could not resolve — including when the switch had no enum
// class at all — so the dump published a number no reader could tell apart from a genuine
// branch-0 override. BuildStaticSwitchInputs then compounded it: its else branch wrote the
// declared default under source:"override". The decoder is exercised directly here because a
// UNiagaraNodeStaticSwitch inside a compiled called-graph is not constructible in a transient
// package (see the header comment above); the helper takes the UEnum on its own.
namespace
{
    // Authored names in index order, deliberately permuted the way a user-defined enum ends up
    // after a reorder in the enum editor: the digit in NewEnumeratorN is not the entry index.
    // Decoding therefore has to answer with the index, and a name-shaped guess cannot fake it.
    const TCHAR* const PinWrightDumpSwitchEnumNames[] = { TEXT("NewEnumerator0"), TEXT("NewEnumerator3"), TEXT("NewEnumerator1") };

    UUserDefinedEnum* MakePinWrightDumpSwitchEnumFixture()
    {
        UUserDefinedEnum* Enum = NewObject<UUserDefinedEnum>(
            GetTransientPackage(),
            MakeUniqueObjectName(GetTransientPackage(), UUserDefinedEnum::StaticClass(), TEXT("PinWrightDumpSwitchEnum")));
        if (!Enum)
        {
            return nullptr;
        }

        const FString EnumName = Enum->GetName();
        TArray<TPair<FName, int64>> Names;
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(PinWrightDumpSwitchEnumNames); ++Index)
        {
            Names.Emplace(
                FName(*FString::Printf(TEXT("%s::%s"), *EnumName, PinWrightDumpSwitchEnumNames[Index])),
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
        return Enum;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraDumpStaticSwitchPinDefaultDecodeTest,
    "PinWright.niagara.dump.StaticSwitchPinDefaultDecode",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpStaticSwitchPinDefaultDecodeTest::RunTest(const FString& Parameters)
{
    UUserDefinedEnum* Enum = MakePinWrightDumpSwitchEnumFixture();
    if (!Enum)
    {
        AddError(TEXT("Could not build the UUserDefinedEnum fixture"));
        return false;
    }
    TestEqual(TEXT("Fixture entry 2 is authored NewEnumerator1"), Enum->GetNameStringByIndex(2), FString(TEXT("NewEnumerator1")));

    TSharedPtr<FJsonValue> Value;
    FString Error;

    // A resolvable stored name still decodes, and to the branch INDEX rather than the digit in
    // the authored name.
    TestTrue(TEXT("Resolvable stored enum name decodes"),
        NiagaraStaticSwitch::DecodePinDefault(TEXT("NewEnumerator1"), ENiagaraStaticSwitchType::Enum, Enum, Value, &Error));
    if (TestTrue(TEXT("Decoded enum value is a number"), Value.IsValid() && Value->Type == EJson::Number))
    {
        TestEqual(TEXT("Authored name NewEnumerator1 decodes to branch index 2"), Value->AsNumber(), 2.0);
    }

    // The defect: this used to return true with branch 0.
    Value.Reset();
    Error.Reset();
    TestFalse(TEXT("Unresolvable stored enum name is rejected instead of becoming branch 0"),
        NiagaraStaticSwitch::DecodePinDefault(TEXT("NotAnEntry"), ENiagaraStaticSwitchType::Enum, Enum, Value, &Error));
    TestFalse(TEXT("Rejected enum name produces no value"), Value.IsValid());
    TestTrue(TEXT("Rejection names the stored value"), Error.Contains(TEXT("NotAnEntry")));
    TestTrue(TEXT("Rejection lists the branch table"), Error.Contains(TEXT("NewEnumerator3")));

    // Same defect through the other door: an enum switch whose enum class is gone also answered
    // branch 0.
    Value.Reset();
    Error.Reset();
    TestFalse(TEXT("Enum switch with no enum class is rejected instead of becoming branch 0"),
        NiagaraStaticSwitch::DecodePinDefault(TEXT("NewEnumerator1"), ENiagaraStaticSwitchType::Enum, nullptr, Value, &Error));
    TestFalse(TEXT("Missing enum class produces no value"), Value.IsValid());
    TestFalse(TEXT("Missing enum class is explained"), Error.IsEmpty());

    // The non-enum types are unchanged; a decoder that started failing on them would empty the
    // `value` field of every bool and integer switch in a dump.
    Value.Reset();
    TestTrue(TEXT("Bool pin default decodes"),
        NiagaraStaticSwitch::DecodePinDefault(TEXT("true"), ENiagaraStaticSwitchType::Bool, nullptr, Value));
    TestTrue(TEXT("Bool pin default decodes to true"), Value.IsValid() && Value->AsBool());

    Value.Reset();
    TestTrue(TEXT("Integer pin default decodes"),
        NiagaraStaticSwitch::DecodePinDefault(TEXT("7"), ENiagaraStaticSwitchType::Integer, nullptr, Value));
    TestTrue(TEXT("Integer pin default decodes to 7"), Value.IsValid() && Value->AsNumber() == 7.0);

    return true;
}
