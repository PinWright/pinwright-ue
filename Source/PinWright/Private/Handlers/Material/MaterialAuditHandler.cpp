// Copyright (c) 2026 Alexander Penkin. MIT License.

// MaterialAuditHandler.cpp - material.audit, the graph-validation half of material checking.
//
// material.compile-state answers "does the shader compile". Nothing answered "is the GRAPH
// sound": dead islands, texture nodes with no texture, function calls with no function, parameters
// nothing reads, two parameters fighting over one name, or pins the blend mode / shading model
// ignores. This verb walks each UMaterial's expression graph once and reports those, under the
// shared audit contract (Audit/AuditFramework.h, docs/rpc-design.md section 18).
//
// REACHABILITY IS THE ONE RISKY PART. An island is "an expression nothing that compiles reaches".
// A missed root turns into false islands, so the roots are every material-property input the
// engine exposes (GetExpressionInputForProperty over MP_*, which covers CustomizedUVs and
// FrontMaterial), every UMaterialExpressionCustomOutput (vertex interpolators, RVT output,
// landscape grass / physical-material output), and named-reroute usages pull in their declaration.
// Composite subgraph scaffolding (the composite node, its pin bases and their reroutes) is visual
// only and never reported.
//
// Read-only: loads materials, writes nothing. includeShaderCompile is the one costly path: it
// blocks on PinWright::MaterialShaderState::ProbeAndWait per material.

#include "Audit/AuditFramework.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Material/MaterialShaderState.h"
#include "Handlers/ParamSpec.h"
#include "Material/MaterialInputIterCompat.h"
#include "Utils/GuardedLoad.h"
#include "Utils/PathUtils.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Materials/Material.h"
#if __has_include("MaterialDomain.h")
#include "MaterialDomain.h" // EMaterialDomain, split out of MaterialShared.h
#endif
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionComposite.h"
#include "Materials/MaterialExpressionCustomOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionPinBase.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialParameters.h"
#include "Misc/EngineVersionComparison.h"
#include "Modules/ModuleManager.h"
#include "SceneTypes.h"

// bUseUnity = true: every name is prefixed MaterialAudit so a sibling TU merged into the same blob
// cannot collide.
namespace
{
    enum class EMaterialAuditCheck : uint8
    {
        Island = 0,
        NullTexture,
        NullFunction,
        UnusedParam,
        DuplicateParam,
        BlendOutputMismatch,
        UvWidth,
        ExpressionBudget,
        ShaderCompile,
        Count
    };

    constexpr int32 MaterialAuditCheckCount = static_cast<int32>(EMaterialAuditCheck::Count);
    static_assert(MaterialAuditCheckCount <= 32, "The check selection bitmask is a uint32.");

    // A graph over this many expressions is reported. Monolith's validate_material uses the same bar.
    constexpr int32 MaterialAuditExpressionBudget = 200;
    constexpr int32 MaterialAuditDefaultLimit = 100;
    constexpr int32 MaterialAuditMaxLimit = 1000;

    struct FMaterialAuditCheckInfo
    {
        EMaterialAuditCheck Check;
        const TCHAR* Id;
        const TCHAR* Code;
        bool bDefaultOn;
        const TCHAR* Summary;
    };

    const TArray<FMaterialAuditCheckInfo>& MaterialAuditAllChecks()
    {
        static const TArray<FMaterialAuditCheckInfo> Checks = {
            { EMaterialAuditCheck::Island, TEXT("island"), ErrorCodes::ERR_MATERIAL_AUDIT_ISLAND, true,
              TEXT("Expression no material output, custom output or named-reroute chain reaches. Warning: "
                   "dead nodes compile to nothing.") },
            { EMaterialAuditCheck::NullTexture, TEXT("null_texture"), ErrorCodes::ERR_MATERIAL_AUDIT_NULL_TEXTURE, true,
              TEXT("Texture node with no texture and no TextureObject input. Error when reachable (the "
                   "compile fails), warning on an island.") },
            { EMaterialAuditCheck::NullFunction, TEXT("null_function"), ErrorCodes::ERR_MATERIAL_AUDIT_NULL_FUNCTION, true,
              TEXT("MaterialFunctionCall with no function. Error when reachable, warning on an island.") },
            { EMaterialAuditCheck::UnusedParam, TEXT("unused_param"), ErrorCodes::ERR_MATERIAL_AUDIT_UNUSED_PARAM, true,
              TEXT("Parameter node nothing reaches: it shows in instances and changes nothing. Warning.") },
            { EMaterialAuditCheck::DuplicateParam, TEXT("duplicate_param"), ErrorCodes::ERR_MATERIAL_AUDIT_DUPLICATE_PARAM, true,
              TEXT("Parameter name used by several nodes. Error when their parameter TYPES differ, warning "
                   "when only their default values differ; identical copies are a shared parameter and pass.") },
            { EMaterialAuditCheck::BlendOutputMismatch, TEXT("blend_output_mismatch"),
              ErrorCodes::ERR_MATERIAL_AUDIT_BLEND_OUTPUT_MISMATCH, true,
              TEXT("Blend mode / domain against connected pins: Masked without OpacityMask (error), "
                   "PostProcess without EmissiveColor (error), Translucent without Opacity (warning), and "
                   "any connected pin the blend mode, shading model or domain ignores (warning). "
                   "Unrunnable on a material-attributes material, whose per-property pins are not read.") },
            { EMaterialAuditCheck::UvWidth, TEXT("uv_width"), ErrorCodes::ERR_MATERIAL_AUDIT_UV_WIDTH, true,
              TEXT("Texture sample whose Coordinates input has a definite width the texture does not take: "
                   "too narrow fails the compile (error when reachable), too wide is silently truncated "
                   "(warning). Sources of indeterminate width are not judged.") },
            { EMaterialAuditCheck::ExpressionBudget, TEXT("expression_budget"),
              ErrorCodes::ERR_MATERIAL_AUDIT_EXPRESSION_BUDGET, true,
              TEXT("More than 200 expressions in one graph. Warning.") },
            { EMaterialAuditCheck::ShaderCompile, TEXT("shader_compile"),
              ErrorCodes::ERR_MATERIAL_AUDIT_SHADER_COMPILE_FAILED, false,
              TEXT("Blocks on a full shader compile (material.compile-state ProbeAndWait) and folds failed "
                   "permutations in as errors; no final verdict is unrunnable. Off by default; "
                   "includeShaderCompile:true selects it.") },
        };
        static_assert(MaterialAuditCheckCount == 9,
            "MaterialAuditAllChecks must list every EMaterialAuditCheck in order.");
        return Checks;
    }

    enum class EMaterialAuditOutcome : uint8 { Clean, Flagged, Unrunnable, NotApplicable };

    struct FMaterialAuditSubject
    {
        FString AssetPath;
        int32 ExpressionCount = 0;
        EMaterialAuditOutcome Outcome[MaterialAuditCheckCount] = {};
        TArray<TSharedPtr<FJsonValue>> Findings;
    };

    // Collects findings for one subject and keeps the verdict and per-check outcome in step, so a
    // finding can never be emitted without being counted.
    struct FMaterialAuditRecorder
    {
        FMaterialAuditSubject& Subject;
        PinWrightAudit::FVerdict& Verdict;

        TSharedPtr<FJsonObject> Add(EMaterialAuditCheck Check, PinWrightAudit::EFindingStatus Status,
            PinWrightAudit::ESeverity Severity, const TCHAR* Code, const UMaterialExpression* Node,
            const FString& Message)
        {
            const int32 Index = static_cast<int32>(Check);
            const bool bUnrunnable = Status == PinWrightAudit::EFindingStatus::Unrunnable;
            if (bUnrunnable)
            {
                ++Verdict.UnrunnableCount;
                if (Subject.Outcome[Index] != EMaterialAuditOutcome::Flagged)
                {
                    Subject.Outcome[Index] = EMaterialAuditOutcome::Unrunnable;
                }
            }
            else
            {
                ++(Severity == PinWrightAudit::ESeverity::Error ? Verdict.ErrorCount : Verdict.WarningCount);
                Subject.Outcome[Index] = EMaterialAuditOutcome::Flagged;
            }

            TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
            Finding->SetStringField(TEXT("check"), PinWrightAudit::CheckInfo(MaterialAuditAllChecks(), Check).Id);
            Finding->SetStringField(TEXT("status"), PinWrightAudit::StatusToWire(Status));
            if (!bUnrunnable)
            {
                Finding->SetStringField(TEXT("severity"), PinWrightAudit::SeverityToWire(Severity));
            }
            Finding->SetStringField(TEXT("code"), Code);
            Finding->SetStringField(TEXT("assetPath"), Subject.AssetPath);
            if (Node)
            {
                // The same id material.graph.* returns and accepts, so a caller can pipe it straight
                // into material.graph.remove_node.
                Finding->SetStringField(TEXT("nodeId"), Node->MaterialExpressionGuid.ToString());
                Finding->SetStringField(TEXT("nodeName"), Node->GetName());
                Finding->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName());
            }
            Finding->SetStringField(TEXT("message"), Message);
            Subject.Findings.Add(MakeShared<FJsonValueObject>(Finding));
            return Finding;
        }

        TSharedPtr<FJsonObject> Flag(EMaterialAuditCheck Check, PinWrightAudit::ESeverity Severity,
            const UMaterialExpression* Node, const FString& Message)
        {
            return Add(Check, PinWrightAudit::EFindingStatus::Flagged, Severity,
                PinWrightAudit::CheckInfo(MaterialAuditAllChecks(), Check).Code, Node, Message);
        }
    };

    PinWrightAudit::ESeverity MaterialAuditErrorIfReached(bool bReached)
    {
        return bReached ? PinWrightAudit::ESeverity::Error : PinWrightAudit::ESeverity::Warning;
    }

    FString MaterialAuditPropertyName(EMaterialProperty Property)
    {
        return StaticEnum<EMaterialProperty>()->GetNameStringByValue(static_cast<int64>(Property));
    }

    bool MaterialAuditIsConnected(UMaterial* Material, EMaterialProperty Property)
    {
        const FExpressionInput* Input = Material->GetExpressionInputForProperty(Property);
        return Input && Input->Expression;
    }

    // Channel count of a definite-width float, 0 for anything whose width is not known statically
    // (MCT_Float is "some float" - the common case for math nodes - and is never judged).
    int32 MaterialAuditFloatWidth(uint64 Type)
    {
        switch (Type)
        {
        case MCT_Float1: return 1;
        case MCT_Float2: return 2;
        case MCT_Float3: return 3;
        case MCT_Float4: return 4;
        default:         return 0;
        }
    }

    void MaterialAuditOne(UMaterial* Material, uint32 Selected, FMaterialAuditRecorder& Rec)
    {
        using PinWrightAudit::ESeverity;
        const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Material->GetExpressions();

        // ---- reachability ----
        TSet<const UMaterialExpression*> Reached;
        TArray<UMaterialExpression*> Stack;
        auto Visit = [&Reached, &Stack](UMaterialExpression* Expr)
        {
            if (Expr && !Reached.Contains(Expr))
            {
                Reached.Add(Expr);
                Stack.Push(Expr);
            }
        };
        for (int32 Property = 0; Property < MP_MAX; ++Property)
        {
            if (FExpressionInput* Input = Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(Property)))
            {
                Visit(Input->Expression);
            }
        }
        TArray<UMaterialExpressionCustomOutput*> CustomOutputs;
        Material->GetAllCustomOutputExpressions(CustomOutputs);
        for (UMaterialExpressionCustomOutput* Output : CustomOutputs)
        {
            Visit(Output);
        }
        while (Stack.Num() > 0)
        {
            UMaterialExpression* Expr = Stack.Pop();
            ForEachExpressionInput(Expr, [&Visit](FExpressionInput* Input, int32) -> bool
            {
                Visit(Input->Expression);
                return false;
            });
            if (const UMaterialExpressionNamedRerouteUsage* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Expr))
            {
                Visit(Usage->Declaration.Get());
            }
        }

        // Visual-only scaffolding: never an island, never counted against the budget.
        TSet<const UMaterialExpression*> Scaffolding;
        for (UMaterialExpression* Expr : Expressions)
        {
            if (!Expr || Expr->IsA<UMaterialExpressionComment>() || Expr->IsA<UMaterialExpressionComposite>())
            {
                Scaffolding.Add(Expr);
            }
            else if (const UMaterialExpressionPinBase* PinBase = Cast<UMaterialExpressionPinBase>(Expr))
            {
                Scaffolding.Add(Expr);
                for (const FCompositeReroute& Pin : PinBase->ReroutePins)
                {
                    Scaffolding.Add(Pin.Expression.Get());
                }
            }
        }
        Rec.Subject.ExpressionCount = Expressions.Num() - Scaffolding.Num();

        TMap<FName, TArray<UMaterialExpression*>> ParamsByName;

        for (UMaterialExpression* Expr : Expressions)
        {
            if (Scaffolding.Contains(Expr))
            {
                continue;
            }
            const bool bReached = Reached.Contains(Expr);

            if (!bReached && PinWrightAudit::HasCheck(Selected, EMaterialAuditCheck::Island))
            {
                Rec.Flag(EMaterialAuditCheck::Island, ESeverity::Warning, Expr,
                    FString::Printf(TEXT("'%s' is reached by no material output, custom output or named "
                        "reroute; it compiles to nothing. Remove it with material.graph.remove_node or wire it in."),
                        *Expr->GetName()));
            }

            if (const UMaterialExpressionTextureBase* TextureNode = Cast<UMaterialExpressionTextureBase>(Expr))
            {
                const UMaterialExpressionTextureSample* Sample = Cast<UMaterialExpressionTextureSample>(Expr);
                const bool bTextureFromInput = Sample && Sample->TextureObject.Expression;
                if (!TextureNode->Texture && !bTextureFromInput
                    && PinWrightAudit::HasCheck(Selected, EMaterialAuditCheck::NullTexture))
                {
                    Rec.Flag(EMaterialAuditCheck::NullTexture, MaterialAuditErrorIfReached(bReached), Expr,
                        FString::Printf(TEXT("'%s' has no texture and no TextureObject input%s. Assign one with "
                            "material.authoring.set_texture_sample_texture."), *Expr->GetName(),
                            bReached ? TEXT("; the material fails to compile") : TEXT(" (on an island)")));
                }

                // ---- uv_width ----
                if (Sample && TextureNode->Texture && !bTextureFromInput && Sample->Coordinates.Expression
                    && PinWrightAudit::HasCheck(Selected, EMaterialAuditCheck::UvWidth))
                {
                    const FExpressionInput& Coords = Sample->Coordinates;
                    int32 Width = 0;
                    if (Coords.Mask)
                    {
                        Width = Coords.MaskR + Coords.MaskG + Coords.MaskB + Coords.MaskA;
                    }
                    else
                    {
#if UE_VERSION_OLDER_THAN(5, 6, 0)
                        Width = MaterialAuditFloatWidth(Coords.Expression->GetOutputType(Coords.OutputIndex));
#else
                        Width = MaterialAuditFloatWidth(Coords.Expression->GetOutputValueType(Coords.OutputIndex));
#endif
                    }
                    const uint64 TextureType = TextureNode->Texture->GetMaterialType();
                    const int32 Expected =
                        (TextureType & (MCT_Texture2D | MCT_TextureVirtual | MCT_TextureExternal)) ? 2
                        : (TextureType & (MCT_TextureCube | MCT_VolumeTexture | MCT_Texture2DArray)) ? 3 : 0;
                    // Width 1 broadcasts and is legal; only a definite vector of the wrong width is judged.
                    if (Expected > 0 && Width > 1 && Width != Expected)
                    {
                        const bool bTooNarrow = Width < Expected;
                        TSharedPtr<FJsonObject> Finding = Rec.Flag(EMaterialAuditCheck::UvWidth,
                            bTooNarrow ? MaterialAuditErrorIfReached(bReached) : ESeverity::Warning, Expr,
                            FString::Printf(TEXT("'%s' samples a texture that takes float%d coordinates but its "
                                "Coordinates input is float%d: %s."), *Expr->GetName(), Expected, Width,
                                bTooNarrow ? TEXT("the compile cannot widen it")
                                           : TEXT("the extra channels are silently dropped")));
                        Finding->SetNumberField(TEXT("coordinateWidth"), Width);
                        Finding->SetNumberField(TEXT("expectedWidth"), Expected);
                    }
                }
            }

            if (const UMaterialExpressionMaterialFunctionCall* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
            {
                if (!Call->MaterialFunction && PinWrightAudit::HasCheck(Selected, EMaterialAuditCheck::NullFunction))
                {
                    Rec.Flag(EMaterialAuditCheck::NullFunction, MaterialAuditErrorIfReached(bReached), Expr,
                        FString::Printf(TEXT("'%s' calls no material function%s."), *Expr->GetName(),
                            bReached ? TEXT("; the material fails to compile") : TEXT(" (on an island)")));
                }
            }

            if (Expr->HasAParameterName() && Expr->GetParameterName() != NAME_None)
            {
                ParamsByName.FindOrAdd(Expr->GetParameterName()).Add(Expr);
                if (!bReached && PinWrightAudit::HasCheck(Selected, EMaterialAuditCheck::UnusedParam))
                {
                    TSharedPtr<FJsonObject> Finding = Rec.Flag(EMaterialAuditCheck::UnusedParam, ESeverity::Warning, Expr,
                        FString::Printf(TEXT("Parameter '%s' is reached by no output: instances expose it and "
                            "it changes nothing."), *Expr->GetParameterName().ToString()));
                    Finding->SetStringField(TEXT("parameterName"), Expr->GetParameterName().ToString());
                }
            }
        }

        // ---- duplicate_param ----
        if (PinWrightAudit::HasCheck(Selected, EMaterialAuditCheck::DuplicateParam))
        {
            for (const TPair<FName, TArray<UMaterialExpression*>>& Pair : ParamsByName)
            {
                if (Pair.Value.Num() < 2)
                {
                    continue;
                }
                FMaterialParameterMetadata First;
                const bool bFirstKnown = Pair.Value[0]->GetParameterValue(First);
                bool bTypeConflict = false;
                bool bValueConflict = false;
                for (int32 Index = 1; Index < Pair.Value.Num() && bFirstKnown; ++Index)
                {
                    FMaterialParameterMetadata Other;
                    if (!Pair.Value[Index]->GetParameterValue(Other))
                    {
                        continue;
                    }
                    bTypeConflict |= Other.Value.Type != First.Value.Type;
                    bValueConflict |= !(Other.Value == First.Value);
                }
                if (!bTypeConflict && !bValueConflict)
                {
                    continue; // identical copies are one shared parameter, which is deliberate
                }
                TSharedPtr<FJsonObject> Finding = Rec.Flag(EMaterialAuditCheck::DuplicateParam,
                    bTypeConflict ? ESeverity::Error : ESeverity::Warning, Pair.Value[1],
                    FString::Printf(TEXT("Parameter name '%s' is used by %d nodes whose %s differ; an "
                        "instance can hold only one value for it. Rename one."),
                        *Pair.Key.ToString(), Pair.Value.Num(),
                        bTypeConflict ? TEXT("parameter types") : TEXT("default values")));
                Finding->SetStringField(TEXT("parameterName"), Pair.Key.ToString());
                TArray<TSharedPtr<FJsonValue>> NodeIds;
                for (const UMaterialExpression* Node : Pair.Value)
                {
                    NodeIds.Add(MakeShared<FJsonValueString>(Node->MaterialExpressionGuid.ToString()));
                }
                Finding->SetArrayField(TEXT("nodeIds"), NodeIds);
            }
        }

        // ---- blend_output_mismatch ----
        if (PinWrightAudit::HasCheck(Selected, EMaterialAuditCheck::BlendOutputMismatch))
        {
            if (Material->bUseMaterialAttributes)
            {
                Rec.Add(EMaterialAuditCheck::BlendOutputMismatch, PinWrightAudit::EFindingStatus::Unrunnable,
                    ESeverity::Warning, ErrorCodes::ERR_MATERIAL_AUDIT_BLEND_UNRUNNABLE, nullptr,
                    TEXT("The material uses material attributes, so which properties are written is decided "
                         "inside the attributes graph and was not evaluated. Omit blend_output_mismatch from "
                         "`checks` to audit this material without it."));
            }
            else
            {
                // Read the RAW blend mode: UMaterial::GetBlendMode() reports a Masked material with no
                // mask as Opaque (bCanMaskedBeAssumedOpaque), which is exactly the case under test.
                const EBlendMode Blend = Material->BlendMode;
                const EMaterialDomain Domain = Material->MaterialDomain;
                const bool bSubstrateFront = MaterialAuditIsConnected(Material, MP_FrontMaterial);
                auto Mismatch = [&Rec](ESeverity Severity, EMaterialProperty Property, const FString& Message)
                {
                    Rec.Flag(EMaterialAuditCheck::BlendOutputMismatch, Severity, nullptr, Message)
                        ->SetStringField(TEXT("property"), MaterialAuditPropertyName(Property));
                };

                if (Blend == BLEND_Masked && (Domain == MD_Surface || Domain == MD_UI)
                    && !MaterialAuditIsConnected(Material, MP_OpacityMask))
                {
                    Mismatch(ESeverity::Error, MP_OpacityMask,
                        TEXT("Blend mode is Masked but OpacityMask is not connected: nothing is clipped, so it "
                             "renders as Opaque at Masked cost. Wire OpacityMask or set the blend mode to Opaque."));
                }
                if ((Blend == BLEND_Translucent || Blend == BLEND_AlphaComposite) && Domain == MD_Surface
                    && !bSubstrateFront && !MaterialAuditIsConnected(Material, MP_Opacity))
                {
                    Mismatch(ESeverity::Warning, MP_Opacity,
                        TEXT("Blend mode is translucent but Opacity is not connected, so opacity is the constant "
                             "default. Wire Opacity or use an opaque blend mode."));
                }
                if (Domain == MD_PostProcess && !bSubstrateFront && !MaterialAuditIsConnected(Material, MP_EmissiveColor))
                {
                    Mismatch(ESeverity::Error, MP_EmissiveColor,
                        TEXT("Post-process material with no EmissiveColor: it outputs nothing."));
                }
                for (int32 Property = 0; Property < MP_MAX; ++Property)
                {
                    const EMaterialProperty P = static_cast<EMaterialProperty>(Property);
                    if (P != MP_MaterialAttributes && MaterialAuditIsConnected(Material, P)
                        && !Material->IsPropertyActiveInEditor(P))
                    {
                        Mismatch(ESeverity::Warning, P, FString::Printf(
                            TEXT("%s is connected but ignored by this material's domain, blend mode or shading "
                                 "model; the wired graph has no effect."), *MaterialAuditPropertyName(P)));
                    }
                }
            }
        }

        // ---- expression_budget ----
        if (PinWrightAudit::HasCheck(Selected, EMaterialAuditCheck::ExpressionBudget)
            && Rec.Subject.ExpressionCount > MaterialAuditExpressionBudget)
        {
            Rec.Flag(EMaterialAuditCheck::ExpressionBudget, ESeverity::Warning, nullptr,
                FString::Printf(TEXT("%d expressions, over the budget of %d. Factor repeated subgraphs into "
                    "material functions."), Rec.Subject.ExpressionCount, MaterialAuditExpressionBudget))
                ->SetNumberField(TEXT("expressionCount"), Rec.Subject.ExpressionCount);
        }

        // ---- shader_compile ----
        if (PinWrightAudit::HasCheck(Selected, EMaterialAuditCheck::ShaderCompile))
        {
            namespace MSS = PinWright::MaterialShaderState;
            const MSS::FState State = MSS::ProbeAndWait(Material);
            if (State.Failed())
            {
                TSharedPtr<FJsonObject> Finding = Rec.Flag(EMaterialAuditCheck::ShaderCompile, ESeverity::Error, nullptr,
                    FString::Printf(TEXT("Shader compile failed with %d error(s); the renderer draws the "
                        "Default Material."), State.Errors.Num()));
                TArray<TSharedPtr<FJsonValue>> Errors;
                for (const FString& Error : State.Errors)
                {
                    Errors.Add(MakeShared<FJsonValueString>(Error));
                }
                Finding->SetArrayField(TEXT("errors"), Errors);
            }
            else if (State.Status != MSS::EStatus::Completed)
            {
                Rec.Add(EMaterialAuditCheck::ShaderCompile, PinWrightAudit::EFindingStatus::Unrunnable,
                    ESeverity::Error, ErrorCodes::ERR_MATERIAL_AUDIT_SHADER_UNVERIFIED, nullptr,
                    FString::Printf(TEXT("The shader probe ended '%s', not a final verdict, so the compile "
                        "was not verified."), MSS::ToWire(State.Status)));
            }
        }
    }

    const TCHAR* MaterialAuditOutcomeToWire(EMaterialAuditOutcome Outcome)
    {
        switch (Outcome)
        {
        case EMaterialAuditOutcome::Flagged:       return TEXT("flagged");
        case EMaterialAuditOutcome::Unrunnable:    return TEXT("unrunnable");
        case EMaterialAuditOutcome::NotApplicable: return TEXT("not_applicable");
        default:                                   return TEXT("clean");
        }
    }
}

REGISTER_RPC_HANDLER("material.audit", "material",
    "Validate the expression GRAPH of one or more UMaterials in one read-only call: islands (nodes "
    "nothing reaches), texture nodes with no texture, function calls with no function, unused or "
    "conflicting duplicate parameters, blend-mode / domain / shading-model vs connected-pin mismatches "
    "(Masked without OpacityMask, PostProcess without Emissive, pins the material ignores), "
    "texture-coordinate width mismatches and oversized graphs. Optionally folds in a blocking shader "
    "compile. Every finding names its check id and, where it is about one node, the nodeId that "
    "material.graph.* verbs accept, so dead nodes can be batch-removed. Writes nothing - there is no "
    "fix mode. A content defect returns success with pass:false, never an RPC error; a material that "
    "would not load is UNRUNNABLE, never clean.",
    RPC_PARAMS(
        RPC_PARAM_OPT("assets", "array",
            "Material asset paths (package path /Game/M/M_X or object path /Game/M/M_X.M_X). Provide exactly "
            "one of assets or folder. A string that is not a content path is rejected with INVALID_ARGUMENT; "
            "a well-formed path that loads nothing is reported UNRUNNABLE; an asset that is not a UMaterial "
            "(e.g. a material instance, which has no graph of its own) is reported not_applicable."),
        RPC_PARAM_OPT("folder", "path",
            "Content folder whose UMaterial assets to audit, e.g. /Game/Materials. Provide exactly one of "
            "assets or folder. A folder that matches nothing is an error, not a clean sweep."),
        RPC_PARAM_DEF("recursive", "boolean", "Include sub-folders of `folder`.", "true"),
        RPC_PARAM_OPT("checks", "array",
            "Check ids to run: island, null_texture, null_function, unused_param, duplicate_param, "
            "blend_output_mismatch, uv_width, expression_budget, shader_compile. Omit for every check "
            "except shader_compile. An unknown id is rejected with AUDIT_UNKNOWN_CHECK rather than skipped."),
        RPC_PARAM_DEF("failOn", "string",
            "Severity that makes pass false: 'error' (default), 'any', or 'none'. Islands, unused parameters "
            "and ignored pins are warnings, so pass failOn:'any' to fail on them. It moves the severity bar "
            "only: any unrunnable check or a truncated sweep fails whatever failOn says.",
            "error"),
        RPC_PARAM_DEF("includeShaderCompile", "boolean",
            "Also run shader_compile: block on a full shader compile per material and report failed "
            "permutations as errors. Slow - seconds per material.",
            "false"),
        RPC_PARAM_DEF("limit", "integer",
            "Most materials audited in one call (1-1000). Matches past it are not audited and the sweep "
            "reports truncated:true, which fails pass.",
            "100")
    ))
{
    // ---- scope ----
    const FString RawFolder = Ctx.GetString(TEXT("folder")).TrimStartAndEnd();
    const TArray<TSharedPtr<FJsonValue>>* AssetArray = Ctx.GetArray(TEXT("assets"));
    const bool bHasAssets = AssetArray && AssetArray->Num() > 0;
    if (RawFolder.IsEmpty() == !bHasAssets)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("Provide exactly one of `assets` (material paths) or `folder` (a content folder)."));
        return true;
    }

    // ---- checks ----
    uint32 Selected = PinWrightAudit::DefaultCheckMask(MaterialAuditAllChecks());
    if (const TArray<TSharedPtr<FJsonValue>>* CheckArray = Ctx.GetArray(TEXT("checks")))
    {
        Selected = 0;
        for (const TSharedPtr<FJsonValue>& Value : *CheckArray)
        {
            FString Id;
            EMaterialAuditCheck Check;
            if (!Value.IsValid() || !Value->TryGetString(Id)
                || !PinWrightAudit::ParseCheckId(MaterialAuditAllChecks(), Id, Check))
            {
                Ctx.SendError(ErrorCodes::ERR_AUDIT_UNKNOWN_CHECK,
                    FString::Printf(TEXT("Unknown check '%s'. Valid ids: %s."),
                        *Id, *PinWrightAudit::ValidCheckIdList(MaterialAuditAllChecks())));
                return true;
            }
            Selected |= PinWrightAudit::CheckBit(Check);
        }
    }
    if (Ctx.GetBool(TEXT("includeShaderCompile"), false))
    {
        Selected |= PinWrightAudit::CheckBit(EMaterialAuditCheck::ShaderCompile);
    }
    if (Selected == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("`checks` was empty, so nothing would be measured. Omit it to run the default checks."));
        return true;
    }

    // ---- failOn ----
    const FString FailOnToken = Ctx.GetString(TEXT("failOn"), TEXT("error"));
    PinWrightAudit::EFailOn FailOnMode;
    if (!PinWrightAudit::ParseFailOn(FailOnToken, FailOnMode))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown failOn '%s'. Valid: error, any, none."), *FailOnToken));
        return true;
    }

    const int32 Limit = FMath::Clamp(Ctx.GetInt(TEXT("limit"), MaterialAuditDefaultLimit), 1, MaterialAuditMaxLimit);

    // ---- resolve the subject list (object paths, sorted for a stable report) ----
    TArray<FString> ObjectPaths;
    if (!RawFolder.IsEmpty())
    {
        FString Folder = SanitizeProjectRelativePath(RawFolder);
        while (Folder.Len() > 1 && Folder.EndsWith(TEXT("/"))) { Folder.LeftChopInline(1); }
        if (Folder.IsEmpty() || !IsValidAssetPath(Folder) || Folder.Contains(TEXT(".")))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
                FString::Printf(TEXT("'folder' is '%s', which is not a content folder path, e.g. /Game/Materials."),
                    *RawFolder));
            return true;
        }
        FARFilter Filter;
        Filter.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;
        Filter.bRecursivePaths = Ctx.GetBool(TEXT("recursive"), true);
        Filter.PackagePaths.Add(FName(*Folder));
        TArray<FAssetData> Matched;
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Matched);
        for (const FAssetData& Data : Matched)
        {
            ObjectPaths.Add(Data.GetObjectPathString());
        }
        if (ObjectPaths.Num() == 0)
        {
            Ctx.SendError(ErrorCodes::ERR_NO_ASSETS_MATCHED,
                FString::Printf(TEXT("No UMaterial assets under '%s'. Nothing was measured, so this is an error "
                    "rather than a clean sweep."), *Folder));
            return true;
        }
    }
    else
    {
        for (const TSharedPtr<FJsonValue>& Value : *AssetArray)
        {
            FString Entry;
            FString ObjectPath;
            FString NormalizeError;
            if (!Value.IsValid() || !Value->TryGetString(Entry)
                || !NormalizeToObjectPath(SanitizeProjectRelativePath(Entry.TrimStartAndEnd()), ObjectPath, NormalizeError))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("'%s' in `assets` is not a material asset path. %s Pass /Game/M/M_X or "
                        "/Game/M/M_X.M_X."), *Entry, *NormalizeError));
                return true;
            }
            ObjectPaths.AddUnique(ObjectPath);
        }
    }
    ObjectPaths.Sort();

    PinWrightAudit::FVerdict Verdict;
    const int32 TotalMatched = ObjectPaths.Num();
    if (TotalMatched > Limit)
    {
        ObjectPaths.SetNum(Limit);
        Verdict.bTruncated = true;
    }

    // ---- the sweep ----
    TArray<FMaterialAuditSubject> Subjects;
    for (const FString& ObjectPath : ObjectPaths)
    {
        FMaterialAuditSubject& Subject = Subjects.AddDefaulted_GetRef();
        Subject.AssetPath = ObjectPath;
        FMaterialAuditRecorder Rec{ Subject, Verdict };

        FString Refusal;
        UObject* Loaded = PinWrightGuardedLoad::LoadObjectChecked<UObject>(ObjectPath, &Refusal, LOAD_NoWarn | LOAD_Quiet);
        UMaterial* Material = Cast<UMaterial>(Loaded);
        if (!Loaded)
        {
            // One unrunnable finding for the subject; every selected check is unrunnable on it.
            for (const FMaterialAuditCheckInfo& Info : MaterialAuditAllChecks())
            {
                if (PinWrightAudit::HasCheck(Selected, Info.Check))
                {
                    Subject.Outcome[static_cast<int32>(Info.Check)] = EMaterialAuditOutcome::Unrunnable;
                }
            }
            TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
            Finding->SetStringField(TEXT("status"), PinWrightAudit::StatusToWire(PinWrightAudit::EFindingStatus::Unrunnable));
            Finding->SetStringField(TEXT("code"), ErrorCodes::ERR_MATERIAL_AUDIT_UNLOADABLE);
            Finding->SetStringField(TEXT("assetPath"), ObjectPath);
            Finding->SetStringField(TEXT("message"), Refusal.IsEmpty()
                ? FString(TEXT("Nothing loads at this path, so no check ran on it."))
                : FString::Printf(TEXT("Load refused: %s"), *Refusal));
            Subject.Findings.Add(MakeShared<FJsonValueObject>(Finding));
            ++Verdict.UnrunnableCount;
            continue;
        }
        if (!Material)
        {
            for (int32 Index = 0; Index < MaterialAuditCheckCount; ++Index)
            {
                Subject.Outcome[Index] = EMaterialAuditOutcome::NotApplicable;
            }
            continue;
        }
        MaterialAuditOne(Material, Selected, Rec);
    }

    // ---- report ----
    int32 Buckets[4] = {};
    TArray<TSharedPtr<FJsonValue>> MaterialRows;
    TArray<TSharedPtr<FJsonValue>> FindingRows;
    for (const FMaterialAuditSubject& Subject : Subjects)
    {
        bool bAnyFlagged = false, bAnyUnrunnable = false, bAnyMeasured = false;
        for (const FMaterialAuditCheckInfo& Info : MaterialAuditAllChecks())
        {
            if (!PinWrightAudit::HasCheck(Selected, Info.Check)) { continue; }
            const EMaterialAuditOutcome Outcome = Subject.Outcome[static_cast<int32>(Info.Check)];
            bAnyFlagged |= Outcome == EMaterialAuditOutcome::Flagged;
            bAnyUnrunnable |= Outcome == EMaterialAuditOutcome::Unrunnable;
            bAnyMeasured |= Outcome != EMaterialAuditOutcome::NotApplicable;
        }
        const EMaterialAuditOutcome Status = bAnyFlagged ? EMaterialAuditOutcome::Flagged
            : bAnyUnrunnable ? EMaterialAuditOutcome::Unrunnable
            : bAnyMeasured ? EMaterialAuditOutcome::Clean : EMaterialAuditOutcome::NotApplicable;
        ++Buckets[static_cast<int32>(Status)];

        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("assetPath"), Subject.AssetPath);
        Row->SetStringField(TEXT("status"), MaterialAuditOutcomeToWire(Status));
        Row->SetNumberField(TEXT("expressionCount"), Subject.ExpressionCount);
        Row->SetNumberField(TEXT("findingCount"), Subject.Findings.Num());
        MaterialRows.Add(MakeShared<FJsonValueObject>(Row));
        FindingRows.Append(Subject.Findings);
    }

    // Every check, selected or not, with per-subject buckets that sum to `subjects`.
    TArray<TSharedPtr<FJsonValue>> CheckRows;
    for (const FMaterialAuditCheckInfo& Info : MaterialAuditAllChecks())
    {
        const bool bSelected = PinWrightAudit::HasCheck(Selected, Info.Check);
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("id"), Info.Id);
        Row->SetStringField(TEXT("code"), Info.Code);
        Row->SetBoolField(TEXT("selected"), bSelected);
        Row->SetBoolField(TEXT("defaultOn"), Info.bDefaultOn);
        if (bSelected)
        {
            int32 Counts[4] = {};
            for (const FMaterialAuditSubject& Subject : Subjects)
            {
                ++Counts[static_cast<int32>(Subject.Outcome[static_cast<int32>(Info.Check)])];
            }
            Row->SetNumberField(TEXT("subjects"), Subjects.Num());
            Row->SetNumberField(TEXT("clean"), Counts[static_cast<int32>(EMaterialAuditOutcome::Clean)]);
            Row->SetNumberField(TEXT("flagged"), Counts[static_cast<int32>(EMaterialAuditOutcome::Flagged)]);
            Row->SetNumberField(TEXT("unrunnable"), Counts[static_cast<int32>(EMaterialAuditOutcome::Unrunnable)]);
            Row->SetNumberField(TEXT("notApplicable"), Counts[static_cast<int32>(EMaterialAuditOutcome::NotApplicable)]);
        }
        else
        {
            Row->SetStringField(TEXT("notSelectedReason"), TEXT("not selected: not named in `checks`"));
        }
        Row->SetStringField(TEXT("summary"), Info.Summary);
        CheckRows.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("subjects"), Subjects.Num());
    Summary->SetNumberField(TEXT("totalMatched"), TotalMatched);
    Summary->SetNumberField(TEXT("clean"), Buckets[static_cast<int32>(EMaterialAuditOutcome::Clean)]);
    Summary->SetNumberField(TEXT("flagged"), Buckets[static_cast<int32>(EMaterialAuditOutcome::Flagged)]);
    Summary->SetNumberField(TEXT("unrunnable"), Buckets[static_cast<int32>(EMaterialAuditOutcome::Unrunnable)]);
    Summary->SetNumberField(TEXT("notApplicable"), Buckets[static_cast<int32>(EMaterialAuditOutcome::NotApplicable)]);
    Summary->SetNumberField(TEXT("findings"), FindingRows.Num());
    Summary->SetNumberField(TEXT("errors"), Verdict.ErrorCount);
    Summary->SetNumberField(TEXT("warnings"), Verdict.WarningCount);
    Summary->SetNumberField(TEXT("unrunnableChecks"), Verdict.UnrunnableCount);
    Summary->SetBoolField(TEXT("truncated"), Verdict.bTruncated);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetBoolField(TEXT("pass"), Verdict.DerivePass(FailOnMode));
    Resp->SetStringField(TEXT("failOn"), PinWrightAudit::FailOnToWire(FailOnMode));
    Resp->SetStringField(TEXT("passRule"), PinWrightAudit::PassRuleText(/*bIncludeTruncation=*/true,
        TEXT("A material that would not load, a material-attributes material under blend_output_mismatch, "
             "and a shader probe without a final verdict are unrunnable, never clean.")));
    Resp->SetObjectField(TEXT("summary"), Summary);
    Resp->SetArrayField(TEXT("checks"), CheckRows);
    Resp->SetArrayField(TEXT("materials"), MaterialRows);
    Resp->SetArrayField(TEXT("findings"), FindingRows);
    Ctx.SendSuccess(Resp);
    return true;
}
