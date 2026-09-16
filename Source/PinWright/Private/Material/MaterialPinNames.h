// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// MaterialPinNames — display names for UMaterialExpression pins, derived the way the
// engine derives them.
//
// WHY THIS EXISTS: FExpressionOutput::OutputName is an FName that the engine leaves as
// NAME_None for most expressions. UMaterialExpressionVertexColor's five outputs are built
// as FExpressionOutput(TEXT(""), 1,1,1,1,0) / (1,0,0,0) / ... in
// Engine/Private/Materials/MaterialExpressions.cpp:10027-10031, so reporting
// Output.OutputName.ToString() directly emits the literal string "None" five times and the
// caller is left guessing an index. The engine never stores those display names; it derives
// them from the mask bits.
//
// THE DERIVATION, copied from the engine (not invented here):
//   1. A non-None OutputName wins verbatim.
//      Editor/MaterialEditor/Private/MaterialEditingLibrary.cpp:1161-1163
//      (static GetExpressionOutputName, the body behind the published
//      UMaterialEditingLibrary::GetMaterialExpressionOutputNames).
//   2. Otherwise, when Mask is set, the name is the set channel bits in RGBA order.
//      The same MaterialEditingLibrary.cpp:1165-1183 spells the four single-channel cases
//      "R" / "G" / "B" / "A" and stops there; the multi-channel spellings are equally
//      concrete elsewhere — UMaterialGraphNode::CreateOutputPins tags the all-four mask
//      PSC_RGBA (Editor/UnrealEd/Private/MaterialGraphNode.cpp:801-804, subcategory string
//      at MaterialGraphSchema.cpp:281), and MaterialExpressions.cpp names the identical
//      mask patterns "RGB" (1,1,1,0) and "RGBA" (1,1,1,1) verbatim wherever an expression
//      does name them (:2575, :5162). Concatenating the bits reproduces every one of those
//      spellings and generalises the leftovers (Constant2Vector's 1,1,0,0 -> "RG") instead
//      of hardcoding a private table.
//   3. Otherwise the engine's own last-resort pin name: UMaterialGraphNode::CreateOutputPins
//      falls back to CreateUniquePinName(TEXT("Output")) (MaterialGraphNode.cpp:809-813),
//      and CreateInputPins to CreateUniquePinName(TEXT("Input")) (:744-748).
//      UEdGraphNode::CreateUniquePinName (Runtime/Engine/Classes/EdGraph/EdGraphNode.h:679-691)
//      keeps the bare stem for the first pin and appends a 2-based counter on collision,
//      i.e. "Output", "Output2", "Output3" — reproduced by MakeUniqueFallbackName below.
//      The collision check is against the names already assigned on THIS node, which is why
//      the list builders are the primitive here and the single-index helpers go through them.
//
// A name emitted from here must also be accepted where a pin is addressed by name, or the
// report is worse than "None": PinWright::Material::ApplyConnection (Handlers/Material/
// MaterialFinders.h) resolves connect_nodes' sourcePin through ResolveOutputPinIndex, and
// FMaterialExpressionFactory::FindExpressionInputByName falls back to ResolveInputPinIndex.

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "MaterialExpressionIO.h"
#include "Material/MaterialInputIterCompat.h"

namespace PinWright::MaterialPinNames
{

// UEdGraphNode::CreateUniquePinName (EdGraphNode.h:679-691): bare stem first, then a
// 2-based numeric suffix for each collision against the names already on the node.
inline FString MakeUniqueFallbackName(const TArray<FString>& Assigned, const TCHAR* Stem)
{
    FString Candidate(Stem);
    int32 Index = 1;
    while (Assigned.Contains(Candidate))
    {
        ++Index;
        Candidate = FString::Printf(TEXT("%s%d"), Stem, Index);
    }
    return Candidate;
}

// Steps 1-2 of the derivation. Returns an empty string when neither applies, leaving the
// stem fallback to the caller (which is the only place that knows the sibling names).
inline FString DerivedOutputNameOrEmpty(const FExpressionOutput& Output)
{
    if (!Output.OutputName.IsNone())
    {
        return Output.OutputName.ToString();
    }

    if (Output.Mask)
    {
        FString Channels;
        if (Output.MaskR) Channels += TEXT("R");
        if (Output.MaskG) Channels += TEXT("G");
        if (Output.MaskB) Channels += TEXT("B");
        if (Output.MaskA) Channels += TEXT("A");
        return Channels;
    }

    return FString();
}

// Display name per output pin, positionally aligned with GetOutputs() — element i is the
// name of the output addressed as sourceOutputIndex == i.
inline TArray<FString> DeriveOutputPinNames(UMaterialExpression* Expression)
{
    TArray<FString> Names;
    if (!Expression)
    {
        return Names;
    }

    const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
    Names.Reserve(Outputs.Num());
    for (const FExpressionOutput& Output : Outputs)
    {
        FString Name = DerivedOutputNameOrEmpty(Output);
        if (Name.IsEmpty())
        {
            Name = MakeUniqueFallbackName(Names, TEXT("Output"));
        }
        Names.Add(MoveTemp(Name));
    }
    return Names;
}

// Display name per input pin, positionally aligned with ForEachExpressionInput's Index.
// UMaterialExpression::GetInputName is authoritative when it returns anything (it is what
// UMaterialGraphNode::CreateInputPins feeds the pin), so only the NAME_None case is derived.
inline TArray<FString> DeriveInputPinNames(UMaterialExpression* Expression)
{
    TArray<FString> Names;
    if (!Expression)
    {
        return Names;
    }

    ForEachExpressionInput(Expression, [&Names, Expression](FExpressionInput*, int32 Index) -> bool
    {
        // Keep element i == input index i even if the iterator skipped a null input, so a
        // caller can index this array with the same Index the iterator handed it.
        while (Names.Num() < Index)
        {
            Names.Add(MakeUniqueFallbackName(Names, TEXT("Input")));
        }

        const FName Raw = Expression->GetInputName(Index);
        FString Name = Raw.IsNone() ? FString() : Raw.ToString();
        if (Name.IsEmpty())
        {
            Name = MakeUniqueFallbackName(Names, TEXT("Input"));
        }
        Names.Add(MoveTemp(Name));
        return false;
    });
    return Names;
}

// Case-insensitive name -> index, over exactly the names DeriveOutputPinNames emits.
// INDEX_NONE when the expression is null, the name is empty, or nothing matches.
inline int32 ResolveOutputPinIndex(UMaterialExpression* Expression, const FString& PinName)
{
    if (!Expression || PinName.IsEmpty())
    {
        return INDEX_NONE;
    }

    const TArray<FString> Names = DeriveOutputPinNames(Expression);
    for (int32 Index = 0; Index < Names.Num(); ++Index)
    {
        if (Names[Index].Equals(PinName, ESearchCase::IgnoreCase))
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

// Case-insensitive name -> index, over exactly the names DeriveInputPinNames emits.
inline int32 ResolveInputPinIndex(UMaterialExpression* Expression, const FString& PinName)
{
    if (!Expression || PinName.IsEmpty())
    {
        return INDEX_NONE;
    }

    const TArray<FString> Names = DeriveInputPinNames(Expression);
    for (int32 Index = 0; Index < Names.Num(); ++Index)
    {
        if (Names[Index].Equals(PinName, ESearchCase::IgnoreCase))
        {
            return Index;
        }
    }
    return INDEX_NONE;
}

} // namespace PinWright::MaterialPinNames
