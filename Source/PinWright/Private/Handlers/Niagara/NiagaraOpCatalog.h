// Copyright (c) 2026 Alexander Penkin. MIT License.
// Shared snapshot of the engine's Niagara op leaf names and their categories.
//
// FNiagaraOpInfo::GetOpInfo / GetOpInfoArray are not NIAGARAEDITOR_API exported, so
// neither niagara.graph.search_ops nor niagara.graph.create_node can read the live op
// registry. Both previously kept their own divergent hand-mirrored copies of the same
// engine data (a {Leaf, Category} table in NiagaraSearchHandler.cpp, a bare-leaf TSet
// in NiagaraGraphHandler.cpp). Keeping two copies caused B-niagara-create-op-bare-leaf-
// pinless: search_ops emitted bare leaf names ("Mul") that create_node accepted but
// stored un-canonicalized, so the registry lookup (keyed on "Category::Leaf") missed
// and the node allocated zero pins.
//
// This single catalog is now the one source of truth. search_ops builds its results
// from it, and create_node validates + canonicalizes a bare leaf back to the full
// "Category::Leaf" registry key through it — so the opName/signature search_ops returns
// round-trips into create_node and resolves the real engine op.
//
// Refresh policy: this is static engine data. If Epic adds/renames an op upstream,
// re-grep `Op->BuildName(TEXT("..."))` and its `CategoryName` in
// Engine/Plugins/FX/Niagara/Source/NiagaraEditor/Private/NiagaraEditorCommon.cpp and
// update the single table below.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"

namespace NiagaraOpCatalog
{
    struct FOpEntry
    {
        const TCHAR* Leaf;
        const TCHAR* Category;
    };

    /** The canonical leaf->category op snapshot, mirrored from NiagaraEditorCommon.cpp. */
    inline const TArray<FOpEntry>& GetEntries()
    {
        static const TArray<FOpEntry> Entries = {
            // Numeric (FNiagaraOpInfo::Init: Add..CmpNEQ)
            { TEXT("Add"), TEXT("Numeric") },
            { TEXT("Subtract"), TEXT("Numeric") },
            { TEXT("Mul"), TEXT("Numeric") },
            { TEXT("Div"), TEXT("Numeric") },
            { TEXT("Madd"), TEXT("Numeric") },
            { TEXT("Lerp"), TEXT("Numeric") },
            { TEXT("RcpFast"), TEXT("Numeric") },
            { TEXT("Rcp"), TEXT("Numeric") },
            { TEXT("RSqrt"), TEXT("Numeric") },
            { TEXT("Sqrt"), TEXT("Numeric") },
            { TEXT("OneMinus"), TEXT("Numeric") },
            { TEXT("Negate"), TEXT("Numeric") },
            { TEXT("Abs"), TEXT("Numeric") },
            { TEXT("Exp"), TEXT("Numeric") },
            { TEXT("Exp2"), TEXT("Numeric") },
            { TEXT("Log"), TEXT("Numeric") },
            { TEXT("Log2"), TEXT("Numeric") },
            { TEXT("Sine"), TEXT("Numeric") },
            { TEXT("Sine(Radians)"), TEXT("Numeric") },
            { TEXT("Sine(Degrees)"), TEXT("Numeric") },
            { TEXT("Cosine"), TEXT("Numeric") },
            { TEXT("Cosine(Radians)"), TEXT("Numeric") },
            { TEXT("Cosine(Degrees)"), TEXT("Numeric") },
            { TEXT("Tangent"), TEXT("Numeric") },
            { TEXT("Tangent(Radians)"), TEXT("Numeric") },
            { TEXT("Tangent(Degrees)"), TEXT("Numeric") },
            { TEXT("ArcSine"), TEXT("Numeric") },
            { TEXT("ArcSine(Radians)"), TEXT("Numeric") },
            { TEXT("ArcSine(Degrees)"), TEXT("Numeric") },
            { TEXT("PI"), TEXT("Numeric") },
            { TEXT("TWO_PI"), TEXT("Numeric") },
            { TEXT("ArcCosine"), TEXT("Numeric") },
            { TEXT("ArcCosine(Radians)"), TEXT("Numeric") },
            { TEXT("ArcCosine(Degrees)"), TEXT("Numeric") },
            { TEXT("ArcTangent"), TEXT("Numeric") },
            { TEXT("ArcTangent(Radians)"), TEXT("Numeric") },
            { TEXT("ArcTangent(Degrees)"), TEXT("Numeric") },
            { TEXT("ArcTangent2"), TEXT("Numeric") },
            { TEXT("ArcTangent2(Radians)"), TEXT("Numeric") },
            { TEXT("ArcTangent2(Degrees)"), TEXT("Numeric") },
            { TEXT("DegreesToRadians"), TEXT("Numeric") },
            { TEXT("RadiansToDegrees"), TEXT("Numeric") },
            { TEXT("Ceil"), TEXT("Numeric") },
            { TEXT("Floor"), TEXT("Numeric") },
            { TEXT("Round"), TEXT("Numeric") },
            { TEXT("FMod"), TEXT("Numeric") },
            { TEXT("FModFast"), TEXT("Numeric") },
            { TEXT("Frac"), TEXT("Numeric") },
            { TEXT("Trunc"), TEXT("Numeric") },
            { TEXT("Clamp"), TEXT("Numeric") },
            { TEXT("Min"), TEXT("Numeric") },
            { TEXT("Max"), TEXT("Numeric") },
            { TEXT("Pow"), TEXT("Numeric") },
            { TEXT("Sign"), TEXT("Numeric") },
            { TEXT("Step"), TEXT("Numeric") },
            { TEXT("Noise"), TEXT("Numeric") },
            { TEXT("Dot"), TEXT("Numeric") },
            { TEXT("Normalize"), TEXT("Numeric") },
            { TEXT("Length"), TEXT("Numeric") },
            { TEXT("DistancePos"), TEXT("Numeric") },
            { TEXT("Rand"), TEXT("Numeric") },
            { TEXT("Rand Integer"), TEXT("Numeric") },
            { TEXT("Rand Float"), TEXT("Numeric") },
            { TEXT("SeededRand"), TEXT("Numeric") },
            { TEXT("SeededRand Integer"), TEXT("Numeric") },
            { TEXT("SeededRand Float"), TEXT("Numeric") },
            { TEXT("Hash Integer"), TEXT("Numeric") },
            { TEXT("Hash Float"), TEXT("Numeric") },
            { TEXT("CmpLT"), TEXT("Numeric") },
            { TEXT("CmpLE"), TEXT("Numeric") },
            { TEXT("CmpGT"), TEXT("Numeric") },
            { TEXT("CmpGE"), TEXT("Numeric") },
            { TEXT("CmpEQ"), TEXT("Numeric") },
            { TEXT("CmpNEQ"), TEXT("Numeric") },
            // Integer
            { TEXT("BitAnd"), TEXT("Integer") },
            { TEXT("BitOr"), TEXT("Integer") },
            { TEXT("BitXOr"), TEXT("Integer") },
            { TEXT("BitNot"), TEXT("Integer") },
            { TEXT("BitLShift"), TEXT("Integer") },
            { TEXT("BitRShift"), TEXT("Integer") },
            { TEXT("SelectStaticInt"), TEXT("Integer") },
            { TEXT("EnumEq"), TEXT("Integer") },
            { TEXT("EnumNEq"), TEXT("Integer") },
            // Bool
            { TEXT("LogicAnd"), TEXT("Bool") },
            { TEXT("LogicOr"), TEXT("Bool") },
            { TEXT("LogicNot"), TEXT("Bool") },
            { TEXT("LogicEq"), TEXT("Bool") },
            { TEXT("LogicNEq"), TEXT("Bool") },
            // Matrix
            { TEXT("Transpose"), TEXT("Matrix") },
            { TEXT("Row0"), TEXT("Matrix") },
            { TEXT("Row1"), TEXT("Matrix") },
            { TEXT("Row2"), TEXT("Matrix") },
            { TEXT("Row3"), TEXT("Matrix") },
            { TEXT("MatrixMultiply"), TEXT("Matrix") },
            { TEXT("MatrixVectorMultiply"), TEXT("Matrix") },
            { TEXT("TransformPosition"), TEXT("Matrix") },
            { TEXT("TransformVector"), TEXT("Matrix") },
            // Vec3
            { TEXT("Cross"), TEXT("Vec3") },
            // Util
            { TEXT("ExecIndex"), TEXT("Util") },
            { TEXT("SpawnInterpolation"), TEXT("Util") },
        };
        return Entries;
    }

    /**
     * Build the engine op-registry key ("Category::Leaf") the way
     * FNiagaraOpInfo::BuildName does. This is the single owner of that key format:
     * both create_node's canonicalization and search_ops' `signature` go through it,
     * so the round-trip identity (search_ops signature == create_node canonical key)
     * is structural, not a coincidence of two matching Printf call sites.
     */
    inline FString MakeOpKey(const FString& Category, const FString& Leaf)
    {
        return FString::Printf(TEXT("%s::%s"), *Category, *Leaf);
    }

    /** Leaf-name -> category lookup built once from GetEntries(). */
    inline const TMap<FName, FString>& GetLeafToCategory()
    {
        static const TMap<FName, FString> Map = []
        {
            TMap<FName, FString> Out;
            for (const FOpEntry& Entry : GetEntries())
            {
                Out.Add(FName(Entry.Leaf), FString(Entry.Category));
            }
            return Out;
        }();
        return Map;
    }

    /**
     * Resolve a payload op name (either a bare leaf "Mul" or a full "Numeric::Mul"
     * registry key) to the canonical "Category::Leaf" key the engine op registry is
     * keyed on. Returns true and fills OutCanonical on success; returns false if the
     * leaf is not in the catalog.
     */
    inline bool CanonicalizeOpName(const FString& InOpName, FString& OutCanonical)
    {
        // Strip any "Category::" prefix down to the bare leaf for lookup.
        FString LeafName = InOpName;
        int32 SepIndex = INDEX_NONE;
        if (InOpName.FindLastChar(TEXT(':'), SepIndex) && SepIndex > 0)
        {
            LeafName = InOpName.RightChop(SepIndex + 1);
        }

        const FString* Category = GetLeafToCategory().Find(FName(*LeafName));
        if (!Category)
        {
            return false;
        }

        OutCanonical = MakeOpKey(*Category, LeafName);
        return true;
    }
}
