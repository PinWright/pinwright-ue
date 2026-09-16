// Copyright (c) 2026 Alexander Penkin. MIT License.

// AGIRCompilerHelpers.cpp
//
// Out-of-line definition of WriteUObjectFieldByName. Kept out of the header
// because it depends on IrCore/IrTextUtils and Utils/PropertyUtils, which we
// don't want every AGIRCompiler_*.cpp to pick up transitively.

#include "AGIR/AGIRCompilerHelpers.h"


#include "AGIR/AGIRPinBindings.h"
#include "Handlers/Animation/AnimGraphConstructionUtils.h"
#include "IrCore/IrTextUtils.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"
#include "Utils/PropertyUtils.h"

namespace AGIRCliff
{
namespace Helpers
{
// Strip surrounding string or name tokens so reflective writes receive the raw
// payload that ImportText_Direct expects for FName / FString fields.
static FString UnquoteIfQuoted(const FString& Value)
{
    FString Unwrapped;
    FString Error;
    if (FIrTextUtils::TryUnwrapStringLiteral(Value, Unwrapped, Error) ||
        FIrTextUtils::TryUnwrapNameToken(Value, Unwrapped, Error))
    {
        return Unwrapped;
    }
    return Value;
}

FString WriteUObjectFieldByName(UObject* Target, FName FieldName, const FString& ValueAsText)
{
    if (!Target)
    {
        return TEXT("Target is null");
    }
    FProperty* Property = Target->GetClass()->FindPropertyByName(FieldName);
    if (!Property)
    {
        return FString::Printf(TEXT("Field '%s' not found on %s"),
            *FieldName.ToString(), *Target->GetClass()->GetName());
    }
    FString ImportError;
    if (!ImportTextToProperty(Target, Property, UnquoteIfQuoted(ValueAsText), ImportError))
    {
        return ImportError;
    }
    return FString();
}

FString WriteAnimNodeArg(UAnimGraphNode_Base* Node, FName FieldName, const FString& ValueAsText)
{
    if (AGIRPinBindings::IsBindingValue(ValueAsText))
    {
        return AGIRPinBindings::ApplyBindingArg(Node, FieldName, ValueAsText);
    }
    return AnimGraphConstructionUtils::WriteAnimNodeFieldByName(Node, FieldName, ValueAsText);
}
} // namespace Helpers
} // namespace AGIRCliff
