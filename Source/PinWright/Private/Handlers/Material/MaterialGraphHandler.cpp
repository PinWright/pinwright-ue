// Copyright (c) 2026 Alexander Penkin. MIT License.

// MaterialGraphHandler.cpp - Migrated from PinWright_MaterialGraphHandlers.cpp
// Material graph node operations: add/remove nodes, connect/disconnect, get details,
// add texture samples, add expressions, batch-create nodes

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Material/MaterialInputIterCompat.h"
#include "Material/MaterialPinNames.h"
#include "Utils/GuardedLoad.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Material/MaterialExpressionFactory.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Engine/Texture.h"

#include "Handlers/Material/MaterialFinders.h"
#include "Handlers/Material/MaterialHandlerUtils.h"
// AddMaterialVerification: a graph write is not a shader compile, so every response here carries
// the measured shader state (E-material-verbs-have-no-shader-compile-signal).
#include "Handlers/Material/MaterialShaderState.h"
#include "Handlers/Material/MainInputBindings.h"
#include "MGIR/MGIRExpressionUtils.h"


// ---- material.graph.add_node ----
REGISTER_RPC_HANDLER("material.graph.add_node", "material.graph", "Add a UMaterialExpression node of the requested type to a UMaterial or UMaterialFunction graph at the given (x, y). Generic alternative to material.authoring's typed add_* helpers.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material or material-function asset path")),
        MaterialHandlerUtils::MaterialExpressionClassParamOpt(TEXT("nodeType"), TEXT("classref"), TEXT("Type of expression node to add")),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_OPT("name", "string", "Parameter name if applicable")
    ))
{
    FString AssetPath;
    PinWright::Material::FMaterialMutationTarget Target =
        PinWright::Material::LoadMaterialOrFunctionForMutationOrReportError(
            Ctx, MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath);
    if (!Target.IsValid()) return true;

    FString NodeType = Ctx.GetStringFirstOf(MaterialHandlerUtils::MaterialExpressionClassKeys());
    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    float X = static_cast<float>(XD), Y = static_cast<float>(YD);

    UClass* ExpressionClass = FMaterialExpressionFactory::ResolveExpressionClass(NodeType);
    if (!ExpressionClass)
    {
        Ctx.SendError(TEXT("UNKNOWN_TYPE"),
            FString::Printf(TEXT("Unknown node type: %s. Available types: TextureSample, VectorParameter, ScalarParameter, "
                "Add, Multiply, Constant, Constant3Vector, Color, ConstantVectorParameter. "
                "Or use full class name like 'MaterialExpressionLerp'."), *NodeType));
        return true;
    }

    FCreateResult CreateResult = Target.CreateExpression(ExpressionClass, nullptr, FVector2D(X, Y));
    if (CreateResult.IsSuccess())
    {
        UMaterialExpression* NewExpr = CreateResult.Expression;

        FString ParamName = Ctx.GetString(TEXT("name"));
        if (!ParamName.IsEmpty())
        {
            if (UMaterialExpressionParameter* ParamExpr = Cast<UMaterialExpressionParameter>(NewExpr))
                ParamExpr->ParameterName = FName(*ParamName);
        }

        Target.NotifyEdited();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        PinWright::Material::AddMaterialVerification(Result, Target.AssetObject());
        Result->SetStringField(TEXT("nodeId"), NewExpr->MaterialExpressionGuid.ToString());
        Result->SetStringField(TEXT("nodeType"), ExpressionClass->GetName());
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(CreateResult.ErrorCode, CreateResult.ErrorMessage);
    }
    return true;
}

// ---- material.graph.remove_node ----
REGISTER_RPC_HANDLER("material.graph.remove_node", "material.graph", "Delete a material expression from a UMaterial or UMaterialFunction graph by GUID, name, or path. Disconnects surviving inbound wires and material root inputs.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material or material-function asset path")),
        RPC_PARAM_REQ("nodeId", "string", "GUID, name, or path of the node to remove")
    ))
{
    FString AssetPath;
    PinWright::Material::FMaterialMutationTarget Target =
        PinWright::Material::LoadMaterialOrFunctionForMutationOrReportError(
            Ctx, MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath);
    if (!Target.IsValid()) return true;

    FString NodeId = Ctx.GetString(TEXT("nodeId"));
    if (NodeId.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'nodeId'."));
        return true;
    }

    UMaterialExpression* TargetExpr = PinWright::Material::ResolveExpressionOrSendError(
        Ctx, Target.ResolveExpression(NodeId), NodeId,
        TEXT("NODE_NOT_FOUND"), TEXT("Node not found."));
    if (!TargetExpr) return true;

    FString RemovedNodeId = TargetExpr->MaterialExpressionGuid.ToString();
    if (Target.RemoveExpression(TargetExpr))
    {
        Target.NotifyEdited();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        PinWright::Material::AddMaterialVerification(Result, Target.AssetObject());
        Result->SetStringField(TEXT("nodeId"), RemovedNodeId);
        Result->SetBoolField(TEXT("removed"), true);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(TEXT("REMOVE_FAILED"),
            FString::Printf(TEXT("Expression '%s' was not removed from the graph."), *RemovedNodeId));
    }
    return true;
}

// ---- material.graph.connect_nodes ----
REGISTER_RPC_HANDLER("material.graph.connect_nodes", "material.graph", "Wire one expression's output to another's named input pin, in a UMaterial or UMaterialFunction graph. Pass empty / 'Main' as targetNodeId to wire into a main-material input (BaseColor, Roughness, Normal, etc.) — material-only; for a function, wire into the FunctionOutput node's input by passing its node id as targetNodeId.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material or material-function asset path")),
        RPC_PARAM_REQ("sourceNodeId", "string", "Source node GUID/name"),
        RPC_PARAM_OPT("targetNodeId", "string", "Target node GUID/name (empty or 'Main' for main material node — material-only)"),
        RPC_PARAM_REQ("inputName", "string", "Input pin name on target (e.g. BaseColor, Roughness)"),
        RPC_PARAM_OPT("sourcePin", "string", "Output pin name on source node"),
        RPC_PARAM_OPT("sourceOutputIndex", "integer", "Output index fallback when sourcePin not matched")
    ))
{
    FString AssetPath;
    PinWright::Material::FMaterialMutationTarget Target =
        PinWright::Material::LoadMaterialOrFunctionForMutationOrReportError(
            Ctx, MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath);
    if (!Target.IsValid()) return true;

    FString SourceNodeId = Ctx.GetString(TEXT("sourceNodeId"));
    FString TargetNodeId = Ctx.GetString(TEXT("targetNodeId"));
    FString InputName = Ctx.GetString(TEXT("inputName"));
    FString SourcePin = Ctx.GetString(TEXT("sourcePin"), TEXT(""));
    int32 SourceOutputIndex = Ctx.GetInt(TEXT("sourceOutputIndex"), -1);

    UMaterialExpression* SourceExpr = PinWright::Material::ResolveExpressionOrSendError(
        Ctx, Target.ResolveExpression(SourceNodeId), SourceNodeId,
        TEXT("NODE_NOT_FOUND"), TEXT("Source node not found."));
    if (!SourceExpr) return true;

    const bool bMainTarget = TargetNodeId.IsEmpty() || TargetNodeId == TEXT("Main");

    // The "Main" pseudo-node is the material's root output; a UMaterialFunction has no such node —
    // its outputs are FunctionOutput expressions, wired by passing the output node's id as targetNodeId.
    if (bMainTarget && Target.Function)
    {
        Ctx.SendError(TEXT("INVALID_PIN"),
            TEXT("A material function has no 'Main' output node. Wire into a FunctionOutput node by passing its node id as targetNodeId."));
        return true;
    }

    // Target is main material node
    if (bMainTarget)
    {
        bool bFound = false;
        if (FExpressionInput* In = PinWright::Material::ResolveMainInput(Target.Material->GetEditorOnlyData(), InputName))
        {
            PinWright::Material::ApplyConnection(*In, SourceExpr, SourcePin, SourceOutputIndex);
            bFound = true;
        }

        if (bFound)
        {
            Target.NotifyEdited();
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            PinWright::Material::AddMaterialVerification(Result, Target.AssetObject());
            Result->SetStringField(TEXT("inputName"), InputName);
            Ctx.SendSuccess(Result);
        }
        else
        {
            Ctx.SendError(TEXT("INVALID_PIN"),
                FString::Printf(TEXT("Unknown input on main node: %s. Valid: %s"),
                    *InputName, *PinWright::Material::GetValidMainInputNames()));
        }
        return true;
    }
    else
    {
        UMaterialExpression* TargetExpr = PinWright::Material::ResolveExpressionOrSendError(
            Ctx, Target.ResolveExpression(TargetNodeId), TargetNodeId,
            TEXT("NODE_NOT_FOUND"), TEXT("Target node not found."));
        if (!TargetExpr) return true;

        if (FExpressionInput* InputPtr = FMaterialExpressionFactory::FindExpressionInputByName(TargetExpr, InputName))
        {
            PinWright::Material::ApplyConnection(*InputPtr, SourceExpr, SourcePin, SourceOutputIndex);
            Target.NotifyEdited();
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            PinWright::Material::AddMaterialVerification(Result, Target.AssetObject());
            Result->SetStringField(TEXT("inputName"), InputName);
            Ctx.SendSuccess(Result);
            return true;
        }

        Ctx.SendError(TEXT("PIN_NOT_FOUND"),
            FString::Printf(TEXT("Input pin '%s' not found or not compatible."), *InputName));
        return true;
    }
}

// ---- material.graph.break_connections ----
REGISTER_RPC_HANDLER("material.graph.break_connections", "material.graph", "Disconnect every wire on a material expression node, or just the named pin. Pass empty / 'Main' as nodeId to operate on the main material node.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_OPT("nodeId", "string", "Node GUID/name (empty or 'Main' for main material node)"),
        RPC_PARAM_OPT("pinName", "string", "Specific pin to disconnect")
    ))
{
    FString AssetPath;
    UMaterial* Material = PinWright::Material::LoadMaterialForMutationOrReportError(
        Ctx, MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath);
    if (!Material) return true;

    FString NodeId = Ctx.GetString(TEXT("nodeId"));
    FString PinName = Ctx.GetString(TEXT("pinName"));

    // Main material node
    if (NodeId.IsEmpty() || NodeId == TEXT("Main"))
    {
        if (!PinName.IsEmpty())
        {
            bool bFound = false;
            if (FExpressionInput* In = PinWright::Material::ResolveMainInput(Material->GetEditorOnlyData(), PinName))
            {
                In->Expression = nullptr;
                bFound = true;
            }

            if (bFound)
            {
                Material->PostEditChange();
                Material->MarkPackageDirty();
                TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
                PinWright::Material::AddMaterialVerification(Result, Material);
                Result->SetStringField(TEXT("pinName"), PinName);
                Ctx.SendSuccess(Result);
                return true;
            }
        }
    }

    const FExpressionResolution Resolved = ResolveExpressionByIdOrName(Material, NodeId);
    if (Resolved.IsAmbiguous())
    {
        PinWright::Material::SendAmbiguousExpressionError(Ctx, NodeId, Resolved);
        return true;
    }

    UMaterialExpression* TargetExpr = Resolved.Expression;
    if (TargetExpr)
    {
        TArray<FString> PinsBroken;
        if (!PinName.IsEmpty())
        {
            FExpressionInput* In = FMaterialExpressionFactory::FindExpressionInputByName(TargetExpr, PinName);
            if (!In)
            {
                Ctx.SendError(TEXT("PIN_NOT_FOUND"),
                    FString::Printf(TEXT("Input pin '%s' not found on node."), *PinName));
                return true;
            }
            if (In->Expression != nullptr)
            {
                PinsBroken.Add(PinName);
                In->Expression = nullptr;
            }
        }
        else
        {
            // Report the derived pin name, not the raw FName: GetInputName is NAME_None on
            // several classes, so pinsBroken used to hand back the literal "None" for a pin
            // that was really just unnamed — and a name that cannot be passed back as pinName.
            const TArray<FString> InputNames =
                PinWright::MaterialPinNames::DeriveInputPinNames(TargetExpr);
            ForEachExpressionInput(TargetExpr, [&](FExpressionInput* Input, int32 Index) -> bool
            {
                if (Input && Input->Expression)
                {
                    PinsBroken.Add(InputNames.IsValidIndex(Index) ? InputNames[Index] : FString());
                    Input->Expression = nullptr;
                }
                return false;
            });
        }

        Material->PostEditChange();
        Material->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        PinWright::Material::AddMaterialVerification(Result, Material);
        Result->SetStringField(TEXT("nodeId"), NodeId);
        Result->SetBoolField(TEXT("broken"), true);
        TArray<TSharedPtr<FJsonValue>> PinsArray;
        for (const FString& Name : PinsBroken)
        {
            PinsArray.Add(MakeShared<FJsonValueString>(Name));
        }
        Result->SetArrayField(TEXT("pinsBroken"), PinsArray);
        Ctx.SendSuccess(Result);
        return true;
    }

    Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Node not found."));
    return true;
}

// ---- material.graph.get_node_details ----
REGISTER_RPC_HANDLER("material.graph.get_node_details", "material.graph", "Return detailed info (class, position, pin connectivity, parameter name) for one node, or list every node in the graph when nodeId is omitted.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_OPT("nodeId", "string", "Node GUID/name, or 'Main' for the main material output node (omit to list all nodes)")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireAssetPath(MaterialHandlerUtils::MaterialAssetPathKeys(), AssetPath)) return true;

    UMaterial* Material = LoadObject<UMaterial>(nullptr, *AssetPath);
    if (!Material)
    {
        // Not ASSET_NOT_FOUND: a material instance (or any other asset) loads fine at this path
        // and is simply the wrong class. Shared with the mutating verbs in this family via the
        // loader in MaterialFinders.h.
        PinWright::Material::ReportMaterialLoadFailure(Ctx, AssetPath);
        return true;
    }

    FString NodeId = Ctx.GetString(TEXT("nodeId"));

    // The main material output node is not a UMaterialExpression, so the expression resolver
    // cannot resolve it. Accept the documented "Main" token that connect_nodes / break_connections
    // accept (an explicit non-empty "Main"; an omitted nodeId stays list-all mode).
    if (!NodeId.IsEmpty() && NodeId.Equals(TEXT("Main"), ESearchCase::IgnoreCase))
    {
        TSharedPtr<FJsonObject> Result = PinWright::Material::BuildMainNodeDetailsJson(Material);
        PinWright::Material::AddMaterialVerification(Result, Material);
        Ctx.SendSuccess(Result);
        return true;
    }

    auto AllExpressions = Material->GetExpressions();

    UMaterialExpression* TargetExpr = nullptr;
    if (!NodeId.IsEmpty())
    {
        // An ambiguous nodeId must not fall through to list-all mode: that reads as "no such node"
        // while the graph in fact holds several, and it is what let the write side's wrong pick go
        // unnoticed when a caller checked it here.
        const FExpressionResolution Resolved = ResolveExpressionByIdOrName(Material, NodeId);
        if (Resolved.IsAmbiguous())
        {
            PinWright::Material::SendAmbiguousExpressionError(Ctx, NodeId, Resolved);
            return true;
        }
        TargetExpr = Resolved.Expression;
    }

    if (TargetExpr)
    {
        TSharedPtr<FJsonObject> Result = MGIRExpressionUtils::BuildExpressionDetailsJson(TargetExpr);
        PinWright::Material::AddMaterialVerification(Result, Material);
        Ctx.SendSuccess(Result);
    }
    else
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> NodeList;

        for (int32 i = 0; i < AllExpressions.Num(); ++i)
        {
            UMaterialExpression* Expr = AllExpressions[i];
            TSharedPtr<FJsonObject> NodeInfo = MakeShared<FJsonObject>();
            NodeInfo->SetStringField(TEXT("nodeId"), Expr->MaterialExpressionGuid.ToString());
            NodeInfo->SetStringField(TEXT("nodeType"), Expr->GetClass()->GetName());
            NodeInfo->SetNumberField(TEXT("index"), i);
            if (!Expr->Desc.IsEmpty())
                NodeInfo->SetStringField(TEXT("desc"), Expr->Desc);
            NodeList.Add(MakeShared<FJsonValueObject>(NodeInfo));
        }

        Result->SetArrayField(TEXT("availableNodes"), NodeList);
        Result->SetNumberField(TEXT("nodeCount"), AllExpressions.Num());

        if (NodeId.IsEmpty())
        {
            // Documented list-all mode (nodeId omitted): the freshly-built node
            // list is the success payload, not an error.
            PinWright::Material::AddMaterialVerification(Result, Material);
            Ctx.SendSuccess(Result);
        }
        else
        {
            // nodeId supplied but did not resolve to a node — a genuine error.
            const FString Message = FString::Printf(
                TEXT("Node '%s' not found. Material has %d nodes."), *NodeId, AllExpressions.Num());
            Ctx.SendError(TEXT("NODE_NOT_FOUND"), Message);
        }
    }
    return true;
}

// ---- material.graph.add_texture_sample ----
REGISTER_RPC_HANDLER("material.graph.add_texture_sample", "material.graph", "Add a UMaterialExpressionTextureSample referencing the given UTexture asset. Specialised convenience for the most common expression; for arbitrary expressions use material.graph.add_expression.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("materialPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("texturePath", "path", "Texture asset path"),
        RPC_PARAM_OPT("coordinateIndex", "integer", "UV coordinate index"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    FString MaterialPath;
    UMaterial* Material = PinWright::Material::LoadMaterialForMutationOrReportError(
        Ctx, MaterialHandlerUtils::MaterialAssetPathKeys(), MaterialPath);
    if (!Material) return true;

    FString TexturePath = Ctx.GetString(TEXT("texturePath"));
    if (TexturePath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'texturePath'."));
        return true;
    }

    UTexture* Texture = LoadObject<UTexture>(nullptr, *TexturePath);
    if (!Texture)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load Texture: %s"), *TexturePath));
        return true;
    }

    int32 CoordinateIndex = Ctx.GetInt(TEXT("coordinateIndex"), 0);
    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    float X = static_cast<float>(XD), Y = static_cast<float>(YD);

    Material->Modify();
    UMaterialExpressionTextureSample* TexSample = NewObject<UMaterialExpressionTextureSample>(
        Material, UMaterialExpressionTextureSample::StaticClass(), NAME_None, RF_Transactional);

    if (!TexSample)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create TextureSample expression."));
        return true;
    }

    // Owning-material back-pointer — see FMaterialExpressionFactory::Create. Without it a later
    // edit of this node never forwards to the material and the shader is never retranslated.
    TexSample->Material = Material;
    TexSample->Texture = Texture;
    TexSample->ConstCoordinate = CoordinateIndex;
    TexSample->MaterialExpressionEditorX = (int32)X;
    TexSample->MaterialExpressionEditorY = (int32)Y;

    if (Material->GetEditorOnlyData())
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(TexSample);

    Material->PreEditChange(nullptr);
    Material->PostEditChange();
    McpSafeAssetSave(Material);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Material);
    Result->SetStringField(TEXT("nodeId"), TexSample->MaterialExpressionGuid.ToString());
    Result->SetStringField(TEXT("texturePath"), Texture->GetPathName());
    AddMarkDirtySaveReport(Result, Material, /*bSaveRequested=*/true);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- material.graph.add_expression ----
REGISTER_RPC_HANDLER("material.graph.add_expression", "material.graph", "Add a material expression node identified by class name to a UMaterial or UMaterialFunction graph. Accepts both the short form ('Add') and the full UE class name ('MaterialExpressionAdd'). Generic counterpart to add_texture_sample.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("materialPath"), TEXT("path"), TEXT("Material or material-function asset path")),
        MaterialHandlerUtils::MaterialExpressionClassParamReq(TEXT("expressionClass"), TEXT("classref"), TEXT("Expression class name (e.g. 'Add' or 'MaterialExpressionAdd')")),
        RPC_PARAM_OPT("properties", "object", "Optional property map applied to the new expression via reflected property names"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)")
    ))
{
    FString MaterialPath;
    PinWright::Material::FMaterialMutationTarget Target =
        PinWright::Material::LoadMaterialOrFunctionForMutationOrReportError(
            Ctx, MaterialHandlerUtils::MaterialAssetPathKeys(), MaterialPath);
    if (!Target.IsValid()) return true;

    FString ExpressionClassName = Ctx.GetStringFirstOf(MaterialHandlerUtils::MaterialExpressionClassKeys());
    if (ExpressionClassName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'expressionClass'."));
        return true;
    }

    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    float X = static_cast<float>(XD), Y = static_cast<float>(YD);

    UObject* Asset = Target.AssetObject();
    Asset->Modify();
    const TSharedPtr<FJsonObject> Properties = Ctx.GetObject(TEXT("properties"));
    FCreateResult CreateResult = Target.CreateExpression(ExpressionClassName, Properties, FVector2D(X, Y));
    if (!CreateResult.IsSuccess())
    {
        Ctx.SendError(CreateResult.ErrorCode, CreateResult.ErrorMessage);
        return true;
    }
    UMaterialExpression* NewExpr = CreateResult.Expression;

    Asset->PreEditChange(nullptr);
    Asset->PostEditChange();
    McpSafeAssetSave(Asset);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    PinWright::Material::AddMaterialVerification(Result, Asset);
    Result->SetStringField(TEXT("nodeId"), NewExpr->MaterialExpressionGuid.ToString());
    Result->SetStringField(TEXT("expressionClass"), NewExpr->GetClass()->GetName());
    AddMarkDirtySaveReport(Result, Asset, /*bSaveRequested=*/true);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- material.graph.create_nodes ----
REGISTER_RPC_HANDLER("material.graph.create_nodes", "material.graph", "Add many material expression nodes in a single call by passing an array of {type, x, y, name, texturePath, value} entries. Faster than repeated material.graph.add_node when authoring complex graphs. Like every verb in this namespace it writes the GRAPH, not the shader: the response's shaderCompile block reports the measured shader state, and because a batch write is where an author usually stops, this verb accepts waitForShaderCompile:true to block on the real compile verdict instead of publishing the non-blocking probe.",
    RPC_PARAMS(
        MaterialHandlerUtils::MaterialAssetPathParamReq(TEXT("materialPath"), TEXT("path"), TEXT("Material asset path")),
        RPC_PARAM_REQ("nodes", "array", "Array of node objects: {type, x, y, name, texturePath, value}"),
        PinWright::MaterialShaderState::WaitParamSpec()
    ))
{
    FString MaterialPath;
    UMaterial* Material = PinWright::Material::LoadMaterialForMutationOrReportError(
        Ctx, MaterialHandlerUtils::MaterialAssetPathKeys(), MaterialPath);
    if (!Material) return true;

    const TArray<TSharedPtr<FJsonValue>>* NodesArray = Ctx.GetArray(TEXT("nodes"));
    if (!NodesArray)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'nodes' array."));
        return true;
    }

    Material->Modify();
    TArray<TSharedPtr<FJsonValue>> CreatedNodes;
    int32 SuccessCount = 0;
    int32 FailCount = 0;

    for (const auto& NodeVal : *NodesArray)
    {
        TSharedPtr<FJsonObject> NodeObj = NodeVal->AsObject();
        if (!NodeObj.IsValid()) { FailCount++; continue; }

        FString NodeType;
        if (!NodeObj->TryGetStringField(TEXT("type"), NodeType) || NodeType.IsEmpty())
        { FailCount++; continue; }

        float X = 0.0f, Y = 0.0f;
        NodeObj->TryGetNumberField(TEXT("x"), X);
        NodeObj->TryGetNumberField(TEXT("y"), Y);

        FCreateResult CreateResult = FMaterialExpressionFactory::Create(
            Material, NodeType, nullptr, FVector2D(X, Y));
        if (!CreateResult.IsSuccess()) { FailCount++; continue; }
        UMaterialExpression* NewExpr = CreateResult.Expression;

        // Parameter name
        FString ParamName;
        if (NodeObj->TryGetStringField(TEXT("name"), ParamName))
        {
            if (UMaterialExpressionParameter* ParamExpr = Cast<UMaterialExpressionParameter>(NewExpr))
                ParamExpr->ParameterName = FName(*ParamName);
        }

        // Texture path for texture samples
        FString TexturePath;
        if (NodeObj->TryGetStringField(TEXT("texturePath"), TexturePath))
        {
            if (UMaterialExpressionTextureSample* TexSample = Cast<UMaterialExpressionTextureSample>(NewExpr))
            {
                // GUARDED, because texturePath sits inside an ARRAY ELEMENT (`nodes[]`), which the
                // dispatch-boundary type gate cannot see - it reads top-level params only. Board
                // B-nested-path-values-reach-createpackage-fatal.
                UTexture* Texture = PinWrightGuardedLoad::LoadObjectChecked<UTexture>(TexturePath);
                if (Texture)
                    TexSample->Texture = Texture;
            }
        }

        // Default value for constant expressions
        double DefaultValue = 0.0;
        if (NodeObj->TryGetNumberField(TEXT("value"), DefaultValue))
        {
            if (UMaterialExpressionConstant* ConstExpr = Cast<UMaterialExpressionConstant>(NewExpr))
                ConstExpr->R = (float)DefaultValue;
        }

        TSharedPtr<FJsonObject> NodeInfo = MakeShared<FJsonObject>();
        NodeInfo->SetStringField(TEXT("nodeId"), NewExpr->MaterialExpressionGuid.ToString());
        NodeInfo->SetStringField(TEXT("type"), NewExpr->GetClass()->GetName());
        CreatedNodes.Add(MakeShared<FJsonValueObject>(NodeInfo));
        SuccessCount++;
    }

    Material->PreEditChange(nullptr);
    Material->PostEditChange();
    McpSafeAssetSave(Material);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    // The wire name comes from WaitParamName(), the same source WaitParamSpec() declares it from,
    // so the declared key and the read key cannot drift apart.
    PinWright::Material::AddMaterialVerification(Result, Material,
        Ctx.GetBool(PinWright::MaterialShaderState::WaitParamName(), false));
    Result->SetArrayField(TEXT("createdNodes"), CreatedNodes);
    Result->SetNumberField(TEXT("successCount"), SuccessCount);
    Result->SetNumberField(TEXT("failCount"), FailCount);
    AddMarkDirtySaveReport(Result, Material, /*bSaveRequested=*/true);
    Ctx.SendSuccess(Result);
    return true;
}
