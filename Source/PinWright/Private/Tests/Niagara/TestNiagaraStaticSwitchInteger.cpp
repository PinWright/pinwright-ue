// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for Integer values accepted by niagara.set_static_switch.

#include "Misc/AutomationTest.h"

#include "Handlers/Niagara/NiagaraEditTypes.h"

#include "Dom/JsonValue.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeStaticSwitch.h"
#include "NiagaraScriptSource.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
    UNiagaraNodeStaticSwitch* MakeIntegerSwitchFixture(int32 OptionCount)
    {
        UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(
            GetTransientPackage(),
            UNiagaraScriptSource::StaticClass(),
            NAME_None,
            RF_Transient);
        UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(
            Source,
            UNiagaraGraph::StaticClass(),
            NAME_None,
            RF_Transient);
        if (!Source || !Graph)
        {
            return nullptr;
        }

        Graph->Schema = UEdGraphSchema_Niagara::StaticClass();
        Source->NodeGraph = Graph;

        UNiagaraNodeStaticSwitch* Switch = NewObject<UNiagaraNodeStaticSwitch>(
            Graph,
            UNiagaraNodeStaticSwitch::StaticClass(),
            NAME_None,
            RF_Transient);
        if (!Switch)
        {
            return nullptr;
        }

        Switch->InputParameterName = FName(TEXT("IntegerSwitch"));
        Switch->SwitchTypeData.SwitchType = ENiagaraStaticSwitchType::Integer;
        Graph->AddNode(Switch, false, false);

        // NumOptionsPerVariable is the serialized declaration read by Niagara's public
        // GetOptionValues virtual, but it is protected. Reflection keeps this fixture on the
        // same metadata path as loaded module graphs without needing a package asset.
        FIntProperty* OptionCountProperty = FindFProperty<FIntProperty>(
            Switch->GetClass(), FName(TEXT("NumOptionsPerVariable")));
        if (!OptionCountProperty)
        {
            return nullptr;
        }
        OptionCountProperty->SetPropertyValue_InContainer(Switch, OptionCount);
        return Switch;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraStaticSwitchIntegerValueValidationTest,
    "PinWright.niagara.set_static_switch.IntegerValueValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraStaticSwitchIntegerValueValidationTest::RunTest(const FString& Parameters)
{
    UNiagaraNodeStaticSwitch* Switch = MakeIntegerSwitchFixture(/*OptionCount=*/2);
    if (!Switch)
    {
        AddError(TEXT("Could not build the Integer static-switch fixture"));
        return false;
    }

    FString PinDefault;
    FString Error;

    TestTrue(TEXT("Boolean true encodes for an Integer switch"),
        NiagaraStaticSwitch::EncodePinDefault(
            MakeShared<FJsonValueBoolean>(true),
            ENiagaraStaticSwitchType::Integer,
            nullptr,
            PinDefault,
            Error));
    TestEqual(TEXT("Boolean true selects Integer branch 1"), PinDefault, FString(TEXT("1")));
    TestTrue(TEXT("Encoded branch 1 is selectable"),
        NiagaraStaticSwitch::ValidateIntegerOption(*Switch, 1, Error));

    TestTrue(TEXT("Boolean false encodes for an Integer switch"),
        NiagaraStaticSwitch::EncodePinDefault(
            MakeShared<FJsonValueBoolean>(false),
            ENiagaraStaticSwitchType::Integer,
            nullptr,
            PinDefault,
            Error));
    TestEqual(TEXT("Boolean false selects Integer branch 0"), PinDefault, FString(TEXT("0")));
    TestTrue(TEXT("Encoded branch 0 is selectable"),
        NiagaraStaticSwitch::ValidateIntegerOption(*Switch, 0, Error));

    TestFalse(TEXT("An Integer above the declared branches is refused"),
        NiagaraStaticSwitch::ValidateIntegerOption(*Switch, 2, Error));
    TestTrue(TEXT("High rejection names the valid range"), Error.Contains(TEXT("0..1")));

    TestFalse(TEXT("A negative Integer is refused"),
        NiagaraStaticSwitch::ValidateIntegerOption(*Switch, -1, Error));
    TestTrue(TEXT("Low rejection names the valid range"), Error.Contains(TEXT("0..1")));

    return true;
}

// B-niagara-switch-coercion: a fractional number was truncated, an unparsable string went
// through permissive Atoi, and null/array/object fell back to 0 — each a valid-looking branch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraStaticSwitchIntegerRejectsMalformedTest,
    "PinWright.niagara.set_static_switch.IntegerValueRejectsMalformed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraStaticSwitchIntegerRejectsMalformedTest::RunTest(const FString& Parameters)
{
    const TPair<const TCHAR*, TSharedPtr<FJsonValue>> Rejected[] = {
        { TEXT("fractional 1.9"), MakeShared<FJsonValueNumber>(1.9) },
        { TEXT("fractional -0.5"), MakeShared<FJsonValueNumber>(-0.5) },
        { TEXT("out-of-int32 number"), MakeShared<FJsonValueNumber>(1e12) },
        { TEXT("unparsable string"), MakeShared<FJsonValueString>(TEXT("abc")) },
        { TEXT("trailing-garbage string"), MakeShared<FJsonValueString>(TEXT("1x")) },
        { TEXT("fractional string"), MakeShared<FJsonValueString>(TEXT("1.9")) },
        { TEXT("out-of-int32 string"), MakeShared<FJsonValueString>(TEXT("4294967298")) },
        { TEXT("empty string"), MakeShared<FJsonValueString>(TEXT("")) },
        { TEXT("null"), MakeShared<FJsonValueNull>() },
        { TEXT("array"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueNumber>(1) }) },
        { TEXT("object"), MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()) },
    };
    for (const TPair<const TCHAR*, TSharedPtr<FJsonValue>>& Case : Rejected)
    {
        FString PinDefault;
        FString Error;
        TestFalse(FString::Printf(TEXT("%s is refused"), Case.Key),
            NiagaraStaticSwitch::EncodePinDefault(Case.Value, ENiagaraStaticSwitchType::Integer, nullptr, PinDefault, Error));
        TestTrue(FString::Printf(TEXT("%s: rejection explains the accepted forms"), Case.Key), Error.Contains(TEXT("is not an integer")));
    }

    const TPair<TSharedPtr<FJsonValue>, const TCHAR*> Accepted[] = {
        { MakeShared<FJsonValueNumber>(2.0), TEXT("2") },
        { MakeShared<FJsonValueNumber>(-3), TEXT("-3") },
        { MakeShared<FJsonValueString>(TEXT(" 4 ")), TEXT("4") },
        { MakeShared<FJsonValueBoolean>(true), TEXT("1") },
    };
    for (const TPair<TSharedPtr<FJsonValue>, const TCHAR*>& Case : Accepted)
    {
        FString PinDefault;
        FString Error;
        TestTrue(FString::Printf(TEXT("%s encodes"), Case.Value),
            NiagaraStaticSwitch::EncodePinDefault(Case.Key, ENiagaraStaticSwitchType::Integer, nullptr, PinDefault, Error));
        TestEqual(FString::Printf(TEXT("%s pin default"), Case.Value), PinDefault, FString(Case.Value));
    }
    return true;
}
