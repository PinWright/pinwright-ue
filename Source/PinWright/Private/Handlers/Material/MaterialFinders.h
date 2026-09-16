// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared file-local helpers used by Material/*.cpp handler files.
// Header-only; each consumer .cpp must include this exactly where it needs the helpers.

#pragma once


#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "UObject/ObjectMacros.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "MaterialExpressionIO.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialFunction.h"
#include "Material/MaterialExpressionFactory.h"
#include "Material/MaterialPinNames.h"
#include "MaterialEditingLibrary.h"
#include "Subsystems/AssetEditorSubsystem.h"

// Match a single expression against a GUID / object-name / path / parameter-name needle.
// Shared by the UMaterial and UMaterialFunction lookups below so the two cannot drift.
inline bool ExpressionMatchesNeedle(UMaterialExpression* Expr, const FString& Needle)
{
    if (!Expr) return false;
    if (Expr->MaterialExpressionGuid.ToString() == Needle) return true;
    if (Expr->GetName() == Needle) return true;
    if (Expr->GetPathName() == Needle) return true;

    if (UMaterialExpressionParameter* ParamExpr = Cast<UMaterialExpressionParameter>(Expr))
    {
        if (ParamExpr->ParameterName.ToString() == Needle)
            return true;
    }
    else if (UMaterialExpressionTextureSampleParameter2D* TexParamExpr = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
    {
        if (TexParamExpr->ParameterName.ToString() == Needle)
            return true;
    }
    return false;
}

// Every expression a nodeId needle matched. A parameter NAME is legally shared by several nodes —
// a triplanar or multi-band material samples one texture parameter on two or three projection
// planes, and they share one value at the instance level — so a needle can name more than one
// expression. Returning the first match made a mutator write one node and report plain success,
// leaving the siblings on the old value with nothing in the response to show it.
struct FExpressionResolution
{
    // The single match; nullptr when nothing matched AND when several did.
    UMaterialExpression* Expression = nullptr;
    TArray<UMaterialExpression*> Matches;

    bool IsAmbiguous() const { return Matches.Num() > 1; }
};

// Iterate a GetExpressions()-shaped view and collect every expression matching the (already-trimmed)
// needle. Shared by the UMaterial and UMaterialFunction overloads so the iteration half cannot drift.
inline FExpressionResolution ResolveExpressionInView(
    TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions, const FString& Needle)
{
    FExpressionResolution Resolution;
    for (const TObjectPtr<UMaterialExpression>& Expr : Expressions)
    {
        if (ExpressionMatchesNeedle(Expr.Get(), Needle))
            Resolution.Matches.Add(Expr.Get());
    }
    if (Resolution.Matches.Num() == 1)
        Resolution.Expression = Resolution.Matches[0];
    return Resolution;
}

// Resolve a material expression by GUID, name, full path, or parameter name (covers both scalar/vector
// and texture-2d parameters), reporting how many nodes the needle matched.
inline FExpressionResolution ResolveExpressionByIdOrName(UMaterial* Material, const FString& IdOrName)
{
    if (IdOrName.IsEmpty() || !Material)
        return FExpressionResolution();

    return ResolveExpressionInView(Material->GetExpressions(), IdOrName.TrimStartAndEnd());
}

// Function-graph counterpart of ResolveExpressionByIdOrName, iterating the function's expression collection.
inline FExpressionResolution ResolveExpressionByIdOrName(UMaterialFunction* Function, const FString& IdOrName)
{
    if (IdOrName.IsEmpty() || !Function)
        return FExpressionResolution();

    return ResolveExpressionInView(Function->GetExpressions(), IdOrName.TrimStartAndEnd());
}

// Single-match convenience for callers with no response channel (test fixtures, internal lookups).
// Returns nullptr for BOTH "nothing matched" and "several matched" — a handler must go through
// PinWright::Material::ResolveExpressionOrSendError instead, so an ambiguous needle is refused with
// AMBIGUOUS_NODE rather than reported as a missing node.
inline UMaterialExpression* FindExpressionByIdOrName(UMaterial* Material, const FString& IdOrName)
{
    return ResolveExpressionByIdOrName(Material, IdOrName).Expression;
}

inline UMaterialExpression* FindExpressionByIdOrName(UMaterialFunction* Function, const FString& IdOrName)
{
    return ResolveExpressionByIdOrName(Function, IdOrName).Expression;
}

namespace PinWright::Material
{

// Emit the canonical AMBIGUOUS_NODE error for a nodeId that named several expressions. Every
// candidate is listed with its GUID, unique object name, class and parameter name, so the caller
// can re-issue against one of them without a round trip. Picking one here is the defect this error
// exists to prevent: the graph stays legal and compiles clean with the siblings unchanged, so no
// in-band signal — success payload, compile status, or the matching read verb — can reveal it.
inline void SendAmbiguousExpressionError(
    const FHandlerContext& Ctx,
    const FString& IdOrName,
    const FExpressionResolution& Resolution)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("requestedNodeId"), IdOrName);

    TArray<TSharedPtr<FJsonValue>> Candidates;
    for (UMaterialExpression* Candidate : Resolution.Matches)
    {
        if (!Candidate)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("nodeId"), Candidate->MaterialExpressionGuid.ToString());
        Entry->SetStringField(TEXT("name"), Candidate->GetName());
        Entry->SetStringField(TEXT("class"), Candidate->GetClass()->GetName());
        const FName ParameterName = Candidate->GetParameterName();
        if (!ParameterName.IsNone())
        {
            Entry->SetStringField(TEXT("parameterName"), ParameterName.ToString());
        }
        Candidates.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Data->SetNumberField(TEXT("candidateCount"), Candidates.Num());
    Data->SetArrayField(TEXT("candidates"), Candidates);

    Ctx.SendError(ErrorCodes::ERR_AMBIGUOUS_NODE,
        FString::Printf(
            TEXT("'%s' matches %d expressions, so it does not identify one node. A parameter name is ")
            TEXT("legally shared by several nodes; re-issue once per node with its nodeId (GUID) or ")
            TEXT("unique object name, both of which are in the candidates array."),
            *IdOrName, Candidates.Num()),
        Data);
}

// Turn a resolution into exactly one expression or the right refusal: AMBIGUOUS_NODE (with
// candidates) when several matched, the caller's own not-found code when none did. Handlers do
//   UMaterialExpression* Expr = ResolveExpressionOrSendError(
//       Ctx, ResolveExpressionByIdOrName(Material, NodeId), NodeId, TEXT("NOT_FOUND"), TEXT("Node not found."));
//   if (!Expr) return true;
// because a bare FindExpressionByIdOrName now returns nullptr for both cases, which would make a
// verb report "node not found" about a needle that in fact matched two nodes.
inline UMaterialExpression* ResolveExpressionOrSendError(
    const FHandlerContext& Ctx,
    const FExpressionResolution& Resolution,
    const FString& IdOrName,
    const TCHAR* NotFoundCode,
    const FString& NotFoundMessage)
{
    if (Resolution.IsAmbiguous())
    {
        SendAmbiguousExpressionError(Ctx, IdOrName, Resolution);
        return nullptr;
    }
    if (!Resolution.Expression)
    {
        Ctx.SendError(NotFoundCode, NotFoundMessage);
        return nullptr;
    }
    return Resolution.Expression;
}

// True when an asset editor (FMaterialEditor) currently has Material open. Graph mutations applied
// to the LoadObject-loaded UMaterial are clobbered by the editor's working-copy graph on its next
// sync/save, so mutating handlers must reject the edit while an editor is open rather than silently
// succeed. Returns false when Material is null or GEditor/AssetEditorSubsystem are unavailable.
inline bool IsMaterialEditorOpen(UMaterial* Material)
{
    if (!Material || !GEditor)
        return false;

    UAssetEditorSubsystem* AssetEditorSS = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!AssetEditorSS)
        return false;

    return AssetEditorSS->FindEditorForAsset(Material, false) != nullptr;
}

// The per-instance route a UMaterial-only base-property verb should hand back when it is called
// against a material instance. Named once here so set_blend_mode / set_shading_model /
// set_two_sided and the shared loader below cannot drift on which verb they advertise.
inline const TCHAR* InstanceBasePropertyOverrideVerb()
{
    return TEXT("material.authoring.set_material_instance_base_property_overrides");
}

// Report a failed UMaterial load, separating "nothing loadable at that path" from "the path
// loaded fine, but the asset is not a UMaterial". Both used to answer ASSET_NOT_FOUND, which
// sends the caller off to re-check a path that was correct all along — the most expensive wrong
// error in this family, because a material INSTANCE is the single most common thing to point one
// of these verbs at. The wrong-class branch names the class actually found and, when the target
// is another UMaterialInterface (i.e. an instance) and the caller supplied a per-instance
// counterpart verb, names that verb too. UNSUPPORTED_ASSET_CLASS is the spelling this handler
// family already uses for class discrimination (get_material_info,
// LoadMaterialOrFunctionForMutationOrReportError, LoadMaterialInstanceOrError), so no new code.
inline void ReportMaterialLoadFailure(FHandlerContext& Ctx, const FString& AssetPath,
                                      const TCHAR* PerInstanceRouteVerb = nullptr)
{
    if (UObject* RawAsset = LoadObject<UObject>(nullptr, *AssetPath))
    {
        FString Message = FString::Printf(
            TEXT("Asset is not a Material. Received class: %s"), *RawAsset->GetClass()->GetName());
        if (PerInstanceRouteVerb && RawAsset->IsA<UMaterialInterface>())
        {
            Message += FString::Printf(
                TEXT(" This verb is UMaterial-only; the per-instance counterpart is %s."),
                PerInstanceRouteVerb);
        }
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_CLASS, Message);
        return;
    }

    Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, TEXT("Could not load Material."));
}

inline UMaterial* LoadMaterialForMutationAtPathOrReportError(FHandlerContext& Ctx, const FString& AssetPath)
{
    UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Material)
    {
        ReportMaterialLoadFailure(Ctx, AssetPath);
        return nullptr;
    }

    if (IsMaterialEditorOpen(Material))
    {
        Ctx.SendError(ErrorCodes::ERR_EDITOR_OPEN,
            TEXT("Material is currently open in the Material Editor. Close the asset before editing."));
        return nullptr;
    }

    return Material;
}

// Load a material for mutating RPCs and reject edits that would be clobbered by an open Material Editor.
inline UMaterial* LoadMaterialForMutationOrReportError(
    FHandlerContext& Ctx,
    const TArray<FString>& AssetPathKeys,
    FString& OutAssetPath)
{
    if (!Ctx.RequireAssetPath(AssetPathKeys, OutAssetPath))
        return nullptr;

    return LoadMaterialForMutationAtPathOrReportError(Ctx, OutAssetPath);
}

inline UMaterial* LoadMaterialForMutationOrReportError(
    FHandlerContext& Ctx,
    const FString& AssetPathKey,
    FString& OutAssetPath)
{
    if (!Ctx.RequireAssetPath(AssetPathKey, OutAssetPath))
        return nullptr;

    return LoadMaterialForMutationAtPathOrReportError(Ctx, OutAssetPath);
}

inline bool IsOwnedExpressionInCollection(
    const UObject* Owner,
    UMaterialExpression* Expression,
    TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions)
{
    return Owner && Expression
        && Expression->GetOuter() == Owner
        && Expressions.Contains(Expression);
}

inline bool IsNativeExpressionDeleteComplete(
    UMaterialExpression* Expression,
    TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions)
{
    // ::IsValid, not the Garbage internal flag: through UE 5.3, UObject::MarkAsGarbage - which
    // DeleteMaterialExpression calls - branches on bPendingKillDisabled and sets PendingKill
    // instead of Garbage whenever pending-kill support is on, which it is by default there
    // (UObjectBaseUtility.h). Testing only Garbage therefore reports every successful 5.3 delete
    // as a REMOVE_FAILED. IsValid answers "the engine invalidated this object" for either flag,
    // and on 5.4+, where pending kill is gone, it is exactly the Garbage test.
    return Expression
        && !Expressions.Contains(Expression)
        && !IsValid(Expression);
}

// A node-mutation target that resolves to EITHER a UMaterial or a UMaterialFunction at the same
// asset path, so the node-add / connect / remove / auto_layout RPC family can author a function's
// internal graph the same way it authors a material's. This struct + the loader below are the single
// source of truth for that Material-vs-Function dispatch. Exactly one of Material / Function is
// non-null after a successful load.
struct FMaterialMutationTarget
{
    UMaterial* Material = nullptr;
    UMaterialFunction* Function = nullptr;

    bool IsValid() const { return Material != nullptr || Function != nullptr; }

    // The asset to dirty / report / pass to AddAssetVerification (whichever side resolved).
    UObject* AssetObject() const
    {
        return Material ? static_cast<UObject*>(Material) : static_cast<UObject*>(Function);
    }

    // Remove an expression from the resolved container using the engine's native cleanup path.
    // Ownership and collection membership are checked before calling it; success is reported only
    // when the native helper removed the expression and marked it garbage.
    bool RemoveExpression(UMaterialExpression* Expr) const
    {
        if (!Expr) return false;
        if (Material && Material->GetEditorOnlyData())
        {
            TArray<TObjectPtr<UMaterialExpression>>& Expressions =
                Material->GetEditorOnlyData()->ExpressionCollection.Expressions;
            if (!IsOwnedExpressionInCollection(Material, Expr, Expressions)) return false;
            UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expr);
            return IsNativeExpressionDeleteComplete(Expr, Expressions);
        }
        if (Function && Function->GetEditorOnlyData())
        {
            TArray<TObjectPtr<UMaterialExpression>>& Expressions =
                Function->GetEditorOnlyData()->ExpressionCollection.Expressions;
            if (!IsOwnedExpressionInCollection(Function, Expr, Expressions)) return false;
            UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, Expr);
            return IsNativeExpressionDeleteComplete(Expr, Expressions);
        }
        return false;
    }

    // Resolution (not a bare pointer) so the caller can refuse an ambiguous needle instead of
    // acting on whichever of several same-named nodes iterated first.
    FExpressionResolution ResolveExpression(const FString& IdOrName) const
    {
        if (Material) return ResolveExpressionByIdOrName(Material, IdOrName);
        if (Function) return ResolveExpressionByIdOrName(Function, IdOrName);
        return FExpressionResolution();
    }

    // Create an expression in the resolved container, dispatching to the matching
    // FMaterialExpressionFactory::Create overload so callers never re-derive the Material-vs-Function
    // branch. Class-object form.
    FCreateResult CreateExpression(UClass* ExpressionClass, const TSharedPtr<FJsonObject>& Properties,
                                   const FVector2D& Position) const
    {
        return Material
            ? FMaterialExpressionFactory::Create(Material, ExpressionClass, Properties, Position)
            : FMaterialExpressionFactory::Create(Function, ExpressionClass, Properties, Position);
    }

    // Class-name form (e.g. "Add" or "MaterialExpressionAdd").
    FCreateResult CreateExpression(const FString& ExpressionClassName, const TSharedPtr<FJsonObject>& Properties,
                                   const FVector2D& Position) const
    {
        return Material
            ? FMaterialExpressionFactory::Create(Material, ExpressionClassName, Properties, Position)
            : FMaterialExpressionFactory::Create(Function, ExpressionClassName, Properties, Position);
    }

    void NotifyEdited() const
    {
        if (UObject* Asset = AssetObject())
        {
            Asset->PostEditChange();
            Asset->MarkPackageDirty();
        }
    }
};

// Resolve the asset-path slot to a UMaterial-or-UMaterialFunction mutation target. Tries UMaterial
// first (honouring the open-Material-Editor guard when bCheckEditorOpen), then UMaterialFunction.
// This is the single source of truth for the Material-vs-Function dispatch + class-discrimination
// error vocabulary; graph-mutating handlers keep the guard, while layout-only callers (auto_layout)
// pass bCheckEditorOpen=false because repositioning doesn't get clobbered by the editor's working copy.
// On failure it has already sent the appropriate error (ASSET_NOT_FOUND / EDITOR_OPEN /
// UNSUPPORTED_ASSET_CLASS) and returns an invalid target.
inline FMaterialMutationTarget LoadMaterialOrFunctionForMutationOrReportError(
    FHandlerContext& Ctx,
    const TArray<FString>& AssetPathKeys,
    FString& OutAssetPath,
    bool bCheckEditorOpen = true)
{
    FMaterialMutationTarget Target;
    if (!Ctx.RequireAssetPath(AssetPathKeys, OutAssetPath))
        return Target;

    if (UMaterial* Material = LoadObject<UMaterial>(nullptr, *OutAssetPath))
    {
        if (bCheckEditorOpen && IsMaterialEditorOpen(Material))
        {
            Ctx.SendError(ErrorCodes::ERR_EDITOR_OPEN,
                TEXT("Material is currently open in the Material Editor. Close the asset before editing."));
            return Target;
        }
        Target.Material = Material;
        return Target;
    }

    if (UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *OutAssetPath))
    {
        Target.Function = Function;
        return Target;
    }

    // Distinguish "loaded as some other asset class" from "missing" for a precise error, as auto_layout does.
    if (UObject* AnyObject = LoadObject<UObject>(nullptr, *OutAssetPath))
    {
        Ctx.SendError(ErrorCodes::ERR_UNSUPPORTED_ASSET_CLASS,
            FString::Printf(TEXT("Asset '%s' is %s; expected UMaterial or UMaterialFunction."),
                *OutAssetPath, *AnyObject->GetClass()->GetName()));
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, TEXT("Could not load Material or Material Function."));
    }
    return Target;
}

// Wire SourceExpr -> Input, resolving the output index from SourcePinName (case-insensitive match on
// the derived output pin name) with a fallback to SourceOutputIndex when name doesn't match or is
// empty. Defaults to OutputIndex = 0.
//
// The match runs against PinWright::MaterialPinNames::DeriveOutputPinNames — exactly the spellings
// list_expression_types / search_expression_types / get_node_details report — so every reported name
// is also an accepted one. Matching the raw FExpressionOutput::OutputName instead accepted nothing at
// all for an unnamed-output expression (VertexColor, Constant3Vector, FontSample, ...), silently
// falling through to sourceOutputIndex and wiring output 0.
inline void ApplyConnection(FExpressionInput& Input,
                            UMaterialExpression* SourceExpr,
                            const FString& SourcePinName,
                            int32 SourceOutputIndex)
{
    Input.Expression = SourceExpr;

    int32 Resolved = 0;
    bool bResolved = false;

    if (!SourcePinName.IsEmpty() && SourceExpr)
    {
        const int32 ByName = PinWright::MaterialPinNames::ResolveOutputPinIndex(SourceExpr, SourcePinName);
        if (ByName != INDEX_NONE)
        {
            Resolved = ByName;
            bResolved = true;
        }

        // Raw-name fallback for material function call outputs (mirrors T2 input-side fix for symmetry).
        if (!bResolved)
        {
            if (UMaterialExpressionMaterialFunctionCall* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(SourceExpr))
            {
                for (int32 i = 0; i < FuncCall->FunctionOutputs.Num(); ++i)
                {
                    if (FuncCall->FunctionOutputs[i].Output.OutputName.ToString().Equals(SourcePinName, ESearchCase::IgnoreCase))
                    {
                        Resolved = i;
                        bResolved = true;
                        break;
                    }
                }
            }
        }
    }

    if (!bResolved && SourceOutputIndex >= 0)
    {
        Resolved = SourceOutputIndex;
    }

    Input.OutputIndex = Resolved;
}

} // namespace PinWright::Material
