// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "EdGraphSchema_K2.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"

namespace BlueprintOverrideSignatureTestUtils
{
    BlueprintHandlerUtils::FNamedPinTypeDescriptor MakeDescriptor(
        const TCHAR* Name,
        const FName& Category)
    {
        BlueprintHandlerUtils::FNamedPinTypeDescriptor Descriptor;
        Descriptor.Name = Name;
        Descriptor.Type.PinCategory = Category;
        return Descriptor;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintOverrideSignatureMatchesByNameAndDirectionTest,
    "PinWright.blueprint.override.SignatureMatchesByNameAndDirection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintOverrideSignatureMatchesByNameAndDirectionTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintOverrideSignatureTestUtils;

    const TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> ExpectedInputs = {
        MakeDescriptor(TEXT("Enabled"), UEdGraphSchema_K2::PC_Boolean),
        MakeDescriptor(TEXT("Count"), UEdGraphSchema_K2::PC_Int)
    };
    const TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> ExpectedOutputs = {
        MakeDescriptor(TEXT("Accepted"), UEdGraphSchema_K2::PC_Boolean),
        MakeDescriptor(TEXT("Remaining"), UEdGraphSchema_K2::PC_Int)
    };
    const TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> ReorderedInputs = {
        MakeDescriptor(TEXT("count"), UEdGraphSchema_K2::PC_Int),
        MakeDescriptor(TEXT("ENABLED"), UEdGraphSchema_K2::PC_Boolean)
    };
    const TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> ReorderedOutputs = {
        MakeDescriptor(TEXT("remaining"), UEdGraphSchema_K2::PC_Int),
        MakeDescriptor(TEXT("ACCEPTED"), UEdGraphSchema_K2::PC_Boolean)
    };

    FString Mismatch;
    TestTrue(TEXT("equivalent signatures match independently of property order and name case"),
        BlueprintHandlerUtils::DoPinTypeDescriptorsMatch(
            ExpectedInputs,
            ExpectedOutputs,
            ReorderedInputs,
            ReorderedOutputs,
            Mismatch));

    Mismatch.Reset();
    TestFalse(TEXT("a name and type cannot match across signature directions"),
        BlueprintHandlerUtils::DoPinTypeDescriptorsMatch(
            ExpectedInputs,
            ExpectedOutputs,
            ReorderedOutputs,
            ReorderedInputs,
            Mismatch));
    TestTrue(TEXT("cross-direction mismatch identifies the input side"),
        Mismatch.Contains(TEXT("Input"), ESearchCase::IgnoreCase));

    TArray<BlueprintHandlerUtils::FNamedPinTypeDescriptor> DuplicateInputs = ReorderedInputs;
    DuplicateInputs[1].Name = TEXT("COUNT");
    Mismatch.Reset();
    TestFalse(TEXT("duplicate actual names cannot satisfy a signature bijection"),
        BlueprintHandlerUtils::DoPinTypeDescriptorsMatch(
            ExpectedInputs,
            ExpectedOutputs,
            DuplicateInputs,
            ReorderedOutputs,
            Mismatch));
    TestTrue(TEXT("duplicate-name mismatch is explicit"),
        Mismatch.Contains(TEXT("duplicate actual name"), ESearchCase::IgnoreCase));

    using FDescriptor = BlueprintHandlerUtils::FNamedPinTypeDescriptor;
    const TArray<FDescriptor> EmptyDescriptors;
    auto TestTypeQualifierMismatch = [this, &EmptyDescriptors](
        const TCHAR* Label,
        const FDescriptor& Expected,
        const FDescriptor& Actual)
    {
        FString QualifierMismatch;
        const TArray<FDescriptor> ExpectedDescriptors = {Expected};
        const TArray<FDescriptor> ActualDescriptors = {Actual};
        TestFalse(Label,
            BlueprintHandlerUtils::DoPinTypeDescriptorsMatch(
                ExpectedDescriptors,
                EmptyDescriptors,
                ActualDescriptors,
                EmptyDescriptors,
                QualifierMismatch));
        TestTrue(
            *FString::Printf(TEXT("%s reports a type mismatch"), Label),
            QualifierMismatch.Contains(TEXT("type mismatch"), ESearchCase::IgnoreCase));
    };

    const FDescriptor QualifiedExpected = MakeDescriptor(
        TEXT("Qualified"),
        UEdGraphSchema_K2::PC_Object);

    FDescriptor ReferenceActual = QualifiedExpected;
    ReferenceActual.Type.bIsReference = true;
    TestTypeQualifierMismatch(
        TEXT("reference qualifier must match"),
        QualifiedExpected,
        ReferenceActual);

    FDescriptor ConstActual = QualifiedExpected;
    ConstActual.Type.bIsConst = true;
    TestTypeQualifierMismatch(
        TEXT("const qualifier must match"),
        QualifiedExpected,
        ConstActual);

    FDescriptor WeakActual = QualifiedExpected;
    WeakActual.Type.bIsWeakPointer = true;
    TestTypeQualifierMismatch(
        TEXT("weak-pointer qualifier must match"),
        QualifiedExpected,
        WeakActual);

    FDescriptor MemberReferenceExpected = MakeDescriptor(
        TEXT("Callback"),
        UEdGraphSchema_K2::PC_Delegate);
    FDescriptor MemberReferenceActual = MemberReferenceExpected;
    MemberReferenceActual.Type.PinSubCategoryMemberReference.MemberName = TEXT("Signature");
    TestTypeQualifierMismatch(
        TEXT("member-reference qualifier must match"),
        MemberReferenceExpected,
        MemberReferenceActual);

    FDescriptor TerminalExpected = MakeDescriptor(TEXT("Lookup"), UEdGraphSchema_K2::PC_Name);
    TerminalExpected.Type.ContainerType = EPinContainerType::Map;
    TerminalExpected.Type.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Object;

    FDescriptor TerminalConstActual = TerminalExpected;
    TerminalConstActual.Type.PinValueType.bTerminalIsConst = true;
    TestTypeQualifierMismatch(
        TEXT("terminal const qualifier must match"),
        TerminalExpected,
        TerminalConstActual);

    FDescriptor TerminalWeakActual = TerminalExpected;
    TerminalWeakActual.Type.PinValueType.bTerminalIsWeakPointer = true;
    TestTypeQualifierMismatch(
        TEXT("terminal weak-pointer qualifier must match"),
        TerminalExpected,
        TerminalWeakActual);

    return true;
}
