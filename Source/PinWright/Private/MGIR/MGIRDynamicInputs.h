// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UClass;
class UMaterialExpression;

// Expression classes whose input pins are a RUNTIME ARRAY rather than fixed reflected
// FExpressionInput fields. Those pins do not exist until something creates them, so a
// document that only wires them wires nothing, and MGIR's single statement of what they are
// is the call's named pin arguments:
//
//     %c = call `/Script/Engine.MaterialExpressionCustom`(UV: %uv, I: %i, Code: "...")
//
// declares two inputs, named UV and I in that order, and wires each. Deriving the names from
// the same text that carries the connections is what keeps the two from disagreeing; a
// separate declaration list would be a second place to spell the same names, and the engine
// has no use for a declared-but-unwired input anyway (UMaterialExpressionCustom::Compile
// refuses one with "missing input").
//
// UMaterialExpressionSetMaterialAttributes is the other dynamic-pin class and deliberately
// stays on its own path (FMGIRMaterialAttributeUtils): its pin names are material attribute
// names that must resolve to FGuids, so its declaration is the explicit `Attributes: [...]`
// list rather than the pin arguments. A class with free-form input names plugs in here by
// adding one line to GetInputArrayPropertyName plus one branch in ApplyDeclaredInputNames.
namespace MGIRDynamicInputs
{
    // The reflected array property holding this class's dynamic input pins, or NAME_None
    // when the class has fixed inputs. Also the property name the decompiler must NOT
    // reflect (the pin arguments already carry it) and the compiler must refuse as a call
    // argument, so both sides read it from here.
    FName GetInputArrayPropertyName(const UClass* ExpressionClass);

    inline bool DeclaresInputsFromCallArgs(const UClass* ExpressionClass)
    {
        return !GetInputArrayPropertyName(ExpressionClass).IsNone();
    }

    // Replaces the class-default pin array with exactly one input per declared name, in
    // document order. Refuses an empty or duplicated name: both produce an HLSL function
    // parameter the node's own code cannot address.
    bool ApplyDeclaredInputNames(
        UMaterialExpression* Expression,
        const TArray<FString>& InputNames,
        FString& OutError);
}
