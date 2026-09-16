// Copyright (c) 2026 Alexander Penkin. MIT License.

// MaterialParameterCollectionHandler.cpp
// Authoring surface for UMaterialParameterCollection assets and the
// UMaterialExpressionCollectionParameter graph node. Closes the third leg of
// the material-authoring triangle (UMaterial / UMaterialInstanceConstant / MPC).
//
// RPCs (all under material.authoring.*):
//   create_parameter_collection
//   add_collection_scalar_parameter
//   add_collection_vector_parameter
//   set_collection_parameter_default
//   remove_collection_parameter
//   get_parameter_collection_info
//   add_collection_parameter_node   <-- typed node drop with atomic
//                                       Collection+ParameterName+ParameterId
//                                       binding; without all three the node
//                                       renders "(Invalid Parameter)".

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/PackagePathCompose.h"
// For TrimTrailingFolderSeparator only: this verb lives in the same material.authoring.create_*
// family but keeps its own plain `name`/`path` slots.
#include "Handlers/Material/MaterialCreatePathParamUtils.h"
#include "Handlers/Material/MaterialFinders.h"
#include "PinWrightHelpers.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/MaterialParameterCollectionFactoryNew.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialParameterCollection.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
    // UMaterialParameterCollection::GetScalarParameterIndexByName / GetVectorParameterIndexByName
    // are declared in the public header but defined out-of-line in the ENGINE
    // module *without* ENGINE_API — so they don't resolve at link time outside
    // the Engine DLL. The data arrays themselves are UPROPERTY and accessible,
    // so we walk them manually here. Identical semantics to the engine helpers.
    int32 FindScalarParameterIndexByName(const UMaterialParameterCollection* Collection, FName ParameterName)
    {
        for (int32 Index = 0; Index < Collection->ScalarParameters.Num(); ++Index)
        {
            if (Collection->ScalarParameters[Index].ParameterName == ParameterName)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }

    int32 FindVectorParameterIndexByName(const UMaterialParameterCollection* Collection, FName ParameterName)
    {
        for (int32 Index = 0; Index < Collection->VectorParameters.Num(); ++Index)
        {
            if (Collection->VectorParameters[Index].ParameterName == ParameterName)
            {
                return Index;
            }
        }
        return INDEX_NONE;
    }
}

// MPC material-node drops mutate UMaterial graphs, so they use the shared guarded loader.
#define MPC_LOAD_MATERIAL_OR_RETURN()                                              \
    FString AssetPath;                                                             \
    UMaterial* Material = PinWright::Material::LoadMaterialForMutationOrReportError( \
        Ctx, TEXT("assetPath"), AssetPath);                                        \
    if (!Material) return true

#define MPC_REQUIRE_NODE_POSITION_OR_RETURN(VarX, VarY)                            \
    double VarX##D = 0.0, VarY##D = 0.0;                                           \
    if (!Ctx.RequireNumber(TEXT("x"), VarX##D)) return true;                       \
    if (!Ctx.RequireNumber(TEXT("y"), VarY##D)) return true;                       \
    float VarX = static_cast<float>(VarX##D);                                      \
    float VarY = static_cast<float>(VarY##D)

// Sets the owning-material back-pointer before finalizing: without it a later edit of the node
// never forwards to the material — see FMaterialExpressionFactory::Create.
#define MPC_FINALIZE_EXPR_AND_RESPOND(Expr, Message)                               \
    (Expr)->Material = Material;                                                   \
    (Expr)->MaterialExpressionEditorX = (int32)ExprX;                              \
    (Expr)->MaterialExpressionEditorY = (int32)ExprY;                              \
    Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Expr);     \
    Material->PostEditChange();                                                    \
    Material->MarkPackageDirty();                                                  \
    {                                                                              \
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();                     \
        R->SetStringField(TEXT("nodeId"), (Expr)->MaterialExpressionGuid.ToString());\
        Ctx.SendSuccess(R);                                                        \
    }                                                                              \
    return true

// ============================================================================
// File-local helpers
// ============================================================================

namespace
{
    // Mirrors the LOAD_MATERIAL_OR_RETURN flow from MaterialAuthoringHandler.cpp
    // but for UMaterialParameterCollection. Returns nullptr after sending the
    // appropriate error response; caller must `return true` on null.
    UMaterialParameterCollection* LoadCollectionOrReportError(
        FHandlerContext& Ctx, const FString& ParamName, FString& OutAssetPath)
    {
        if (!Ctx.RequireAssetPath(ParamName, OutAssetPath))
        {
            return nullptr;
        }
        UMaterialParameterCollection* Collection =
            LoadObject<UMaterialParameterCollection>(nullptr, *OutAssetPath);
        if (!Collection)
        {
            Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
                FString::Printf(TEXT("Could not load UMaterialParameterCollection at '%s'."), *OutAssetPath));
            return nullptr;
        }
        return Collection;
    }

    // After mutating Scalar/VectorParameters, force a property-changed event so
    // every live UMaterialParameterCollectionInstance + dependent material
    // rebuilds. ScalarParameters/VectorParameters are the only fields we touch
    // in this file; report whichever array was mutated.
    void NotifyCollectionParametersChanged(UMaterialParameterCollection* Collection, FName ArrayPropertyName)
    {
        if (!Collection) return;
        Collection->Modify();
        FProperty* Prop = FindFProperty<FProperty>(UMaterialParameterCollection::StaticClass(), ArrayPropertyName);
        FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
        Collection->PostEditChangeProperty(Event);
        Collection->MarkPackageDirty();
    }

    bool ReadLinearColorParam(FHandlerContext& Ctx, const FString& Key,
        const FLinearColor& Fallback, FLinearColor& OutColor)
    {
        OutColor = Fallback;
        const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
        if (!Payload.IsValid() || !Payload->HasField(Key))
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* AsObject = nullptr;
        if (Payload->TryGetObjectField(Key, AsObject) && AsObject && AsObject->IsValid())
        {
            const TSharedPtr<FJsonObject>& Obj = *AsObject;
            double R = Fallback.R, G = Fallback.G, B = Fallback.B, A = Fallback.A;
            Obj->TryGetNumberField(TEXT("R"), R);
            Obj->TryGetNumberField(TEXT("G"), G);
            Obj->TryGetNumberField(TEXT("B"), B);
            Obj->TryGetNumberField(TEXT("A"), A);
            OutColor = FLinearColor((float)R, (float)G, (float)B, (float)A);
            return true;
        }

        const TArray<TSharedPtr<FJsonValue>>* AsArray = nullptr;
        if (Payload->TryGetArrayField(Key, AsArray) && AsArray && AsArray->Num() >= 3)
        {
            const float R = (float)(*AsArray)[0]->AsNumber();
            const float G = (float)(*AsArray)[1]->AsNumber();
            const float B = (float)(*AsArray)[2]->AsNumber();
            const float A = AsArray->Num() >= 4 ? (float)(*AsArray)[3]->AsNumber() : Fallback.A;
            OutColor = FLinearColor(R, G, B, A);
            return true;
        }

        return false;
    }

    TSharedPtr<FJsonObject> ScalarParameterToJson(const FCollectionScalarParameter& Param)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Param.ParameterName.ToString());
        Obj->SetNumberField(TEXT("default"), Param.DefaultValue);
        Obj->SetStringField(TEXT("parameterId"), Param.Id.ToString());
        return Obj;
    }

    TSharedPtr<FJsonObject> VectorParameterToJson(const FCollectionVectorParameter& Param)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Param.ParameterName.ToString());
        TSharedPtr<FJsonObject> Default = MakeShared<FJsonObject>();
        Default->SetNumberField(TEXT("R"), Param.DefaultValue.R);
        Default->SetNumberField(TEXT("G"), Param.DefaultValue.G);
        Default->SetNumberField(TEXT("B"), Param.DefaultValue.B);
        Default->SetNumberField(TEXT("A"), Param.DefaultValue.A);
        Obj->SetObjectField(TEXT("default"), Default);
        Obj->SetStringField(TEXT("parameterId"), Param.Id.ToString());
        return Obj;
    }
}

// ============================================================================
// create_parameter_collection
// ============================================================================

REGISTER_RPC_HANDLER("material.authoring.create_parameter_collection", "material.authoring",
    "Create a UMaterialParameterCollection asset — a shared, globally-tweakable bag of scalar/vector parameters that any material can sample via add_collection_parameter_node and any Blueprint can set via SetScalarParameterValue/SetVectorParameterValue.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Collection asset name"),
        RPC_PARAM_OPT("path", "path", "Content path (default /Game/Materials/Collections)"),
        RPC_PARAM_OPT("save", "boolean", "Mark dirty after creation (default true)")
    ))
{
    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;
    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game/Materials/Collections"));

    // Both halves arrive raw off the wire. CreatePackage (UObjectGlobals.cpp:1087-1120) logs at
    // Fatal - never compiled out - for a name containing "//" or one that resolves to empty, so
    // `name: "a//b"` here ended the editor PROCESS rather than failing the call, and the
    // `if (!Package)` below was never reached (B-createpackage-unvalidated-paths-plugin-wide).
    // The shared composer checks the bare name against UObject's naming rules and the composed
    // path against CreatePackage's own input rules, surfacing the engine's reason verbatim. The
    // folder goes in untrimmed: the composer joins with FString::operator/, which absorbs one
    // trailing separator, so `path: "/Game/X/"` still works.
    UMaterialParameterCollectionFactoryNew* Factory = NewObject<UMaterialParameterCollectionFactoryNew>();
    FString PackagePath;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(Path, Name, PackagePath, PathError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("%s Pass a bare asset name in 'name' and choose the folder with "
                                 "'path' (default /Game/Materials/Collections)."), *PathError));
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_ERROR"), TEXT("Failed to create package."));
        return true;
    }

    UMaterialParameterCollection* NewCollection = Cast<UMaterialParameterCollection>(
        Factory->FactoryCreateNew(UMaterialParameterCollection::StaticClass(), Package,
            FName(*Name), RF_Public | RF_Standalone, nullptr, GWarn));
    if (!NewCollection)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create UMaterialParameterCollection."));
        return true;
    }

    NewCollection->PostEditChange();
    NewCollection->MarkPackageDirty();

    if (Ctx.GetBool(TEXT("save"), true))
    {
        NewCollection->MarkPackageDirty();
    }
    FAssetRegistryModule::AssetCreated(NewCollection);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddAssetVerification(Result, NewCollection);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// add_collection_scalar_parameter
// ============================================================================

REGISTER_RPC_HANDLER("material.authoring.add_collection_scalar_parameter", "material.authoring",
    "Append a scalar parameter to an existing UMaterialParameterCollection. The new FGuid Id returned in parameterId is the stable handle used by add_collection_parameter_node to bind nodes across renames.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "MPC asset path"),
        RPC_PARAM_REQ("name", "string", "Parameter name"),
        RPC_PARAM_OPT("default", "number", "Default value (default 0.0)")
    ))
{
    FString AssetPath;
    UMaterialParameterCollection* Collection = LoadCollectionOrReportError(Ctx, TEXT("assetPath"), AssetPath);
    if (!Collection) return true;

    FString ParamName;
    if (!Ctx.RequireString(TEXT("name"), ParamName)) return true;
    const float DefaultValue = (float)Ctx.GetNumber(TEXT("default"), 0.0);

    FCollectionScalarParameter NewParam;
    NewParam.ParameterName = FName(*ParamName);
    NewParam.DefaultValue = DefaultValue;
    NewParam.Id = FGuid::NewGuid();
    Collection->ScalarParameters.Add(NewParam);

    NotifyCollectionParametersChanged(Collection,
        GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, ScalarParameters));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("added"), true);
    Result->SetStringField(TEXT("parameterId"), NewParam.Id.ToString());
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// add_collection_vector_parameter
// ============================================================================

REGISTER_RPC_HANDLER("material.authoring.add_collection_vector_parameter", "material.authoring",
    "Append a vector (FLinearColor) parameter to an existing UMaterialParameterCollection. Default accepts either {R,G,B,A} object or [R,G,B,A] array; missing components fall back to (0,0,0,1).",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "MPC asset path"),
        RPC_PARAM_REQ("name", "string", "Parameter name"),
        RPC_PARAM_OPT("default", "object", "Default linear color {R,G,B,A} (default {0,0,0,1})")
    ))
{
    FString AssetPath;
    UMaterialParameterCollection* Collection = LoadCollectionOrReportError(Ctx, TEXT("assetPath"), AssetPath);
    if (!Collection) return true;

    FString ParamName;
    if (!Ctx.RequireString(TEXT("name"), ParamName)) return true;

    FLinearColor DefaultValue(0.f, 0.f, 0.f, 1.f);
    ReadLinearColorParam(Ctx, TEXT("default"), DefaultValue, DefaultValue);

    FCollectionVectorParameter NewParam;
    NewParam.ParameterName = FName(*ParamName);
    NewParam.DefaultValue = DefaultValue;
    NewParam.Id = FGuid::NewGuid();
    Collection->VectorParameters.Add(NewParam);

    NotifyCollectionParametersChanged(Collection,
        GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, VectorParameters));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("added"), true);
    Result->SetStringField(TEXT("parameterId"), NewParam.Id.ToString());
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// set_collection_parameter_default
// ============================================================================
//
// Critical: we mutate DefaultValue only; we do NOT touch the FGuid Id. Every
// UMaterialExpressionCollectionParameter node in the project that was wired
// to this parameter caches that Id, so regenerating it here would orphan all
// of them (they'd render as "(Invalid Parameter)" after the change).

REGISTER_RPC_HANDLER("material.authoring.set_collection_parameter_default", "material.authoring",
    "Overwrite an existing MPC parameter's default value. parameterType picks the array ('scalar' or 'vector'); the parameter's FGuid Id is preserved so existing CollectionParameter nodes stay bound.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "MPC asset path"),
        RPC_PARAM_REQ("parameterName", "string", "Parameter to mutate"),
        RPC_PARAM_REQ("parameterType", "string", "scalar | vector"),
        RPC_PARAM_REQ("value", "any", "New default — number for scalar, {R,G,B,A} or array for vector")
    ))
{
    FString AssetPath;
    UMaterialParameterCollection* Collection = LoadCollectionOrReportError(Ctx, TEXT("assetPath"), AssetPath);
    if (!Collection) return true;

    FString ParamName;
    if (!Ctx.RequireString(TEXT("parameterName"), ParamName)) return true;
    FString ParamType;
    if (!Ctx.RequireString(TEXT("parameterType"), ParamType)) return true;

    const FName ParamFName(*ParamName);
    const FString ParamTypeLower = ParamType.ToLower();

    if (ParamTypeLower == TEXT("scalar"))
    {
        const int32 Index = FindScalarParameterIndexByName(Collection, ParamFName);
        if (Index == INDEX_NONE)
        {
            Ctx.SendError(TEXT("PARAMETER_NOT_FOUND"),
                FString::Printf(TEXT("Scalar parameter '%s' not found on collection."), *ParamName));
            return true;
        }
        const float NewValue = (float)Ctx.GetNumber(TEXT("value"), 0.0);
        Collection->ScalarParameters[Index].DefaultValue = NewValue;
        NotifyCollectionParametersChanged(Collection,
            GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, ScalarParameters));
    }
    else if (ParamTypeLower == TEXT("vector"))
    {
        const int32 Index = FindVectorParameterIndexByName(Collection, ParamFName);
        if (Index == INDEX_NONE)
        {
            Ctx.SendError(TEXT("PARAMETER_NOT_FOUND"),
                FString::Printf(TEXT("Vector parameter '%s' not found on collection."), *ParamName));
            return true;
        }
        FLinearColor NewValue = Collection->VectorParameters[Index].DefaultValue;
        ReadLinearColorParam(Ctx, TEXT("value"), NewValue, NewValue);
        Collection->VectorParameters[Index].DefaultValue = NewValue;
        NotifyCollectionParametersChanged(Collection,
            GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, VectorParameters));
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("parameterType must be 'scalar' or 'vector', got '%s'."), *ParamType));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("applied"), true);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// remove_collection_parameter
// ============================================================================

REGISTER_RPC_HANDLER("material.authoring.remove_collection_parameter", "material.authoring",
    "Drop a parameter from an MPC's scalar or vector array. Any existing CollectionParameter nodes bound to the removed parameter will render as '(Invalid Parameter)' on next compile.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "MPC asset path"),
        RPC_PARAM_REQ("parameterName", "string", "Parameter to remove"),
        RPC_PARAM_REQ("parameterType", "string", "scalar | vector")
    ))
{
    FString AssetPath;
    UMaterialParameterCollection* Collection = LoadCollectionOrReportError(Ctx, TEXT("assetPath"), AssetPath);
    if (!Collection) return true;

    FString ParamName;
    if (!Ctx.RequireString(TEXT("parameterName"), ParamName)) return true;
    FString ParamType;
    if (!Ctx.RequireString(TEXT("parameterType"), ParamType)) return true;

    const FName ParamFName(*ParamName);
    const FString ParamTypeLower = ParamType.ToLower();

    if (ParamTypeLower == TEXT("scalar"))
    {
        const int32 Index = FindScalarParameterIndexByName(Collection, ParamFName);
        if (Index == INDEX_NONE)
        {
            Ctx.SendError(TEXT("PARAMETER_NOT_FOUND"),
                FString::Printf(TEXT("Scalar parameter '%s' not found on collection."), *ParamName));
            return true;
        }
        Collection->ScalarParameters.RemoveAt(Index);
        NotifyCollectionParametersChanged(Collection,
            GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, ScalarParameters));
    }
    else if (ParamTypeLower == TEXT("vector"))
    {
        const int32 Index = FindVectorParameterIndexByName(Collection, ParamFName);
        if (Index == INDEX_NONE)
        {
            Ctx.SendError(TEXT("PARAMETER_NOT_FOUND"),
                FString::Printf(TEXT("Vector parameter '%s' not found on collection."), *ParamName));
            return true;
        }
        Collection->VectorParameters.RemoveAt(Index);
        NotifyCollectionParametersChanged(Collection,
            GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, VectorParameters));
    }
    else
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("parameterType must be 'scalar' or 'vector', got '%s'."), *ParamType));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("removed"), true);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// get_parameter_collection_info
// ============================================================================

REGISTER_RPC_HANDLER("material.authoring.get_parameter_collection_info", "material.authoring",
    "Return both parameter arrays of an MPC as JSON: [{ name, default, parameterId }] for scalars and [{ name, default:{R,G,B,A}, parameterId }] for vectors.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "MPC asset path")
    ))
{
    FString AssetPath;
    UMaterialParameterCollection* Collection = LoadCollectionOrReportError(Ctx, TEXT("assetPath"), AssetPath);
    if (!Collection) return true;

    TArray<TSharedPtr<FJsonValue>> ScalarArray;
    ScalarArray.Reserve(Collection->ScalarParameters.Num());
    for (const FCollectionScalarParameter& Scalar : Collection->ScalarParameters)
    {
        ScalarArray.Add(MakeShared<FJsonValueObject>(ScalarParameterToJson(Scalar)));
    }

    TArray<TSharedPtr<FJsonValue>> VectorArray;
    VectorArray.Reserve(Collection->VectorParameters.Num());
    for (const FCollectionVectorParameter& Vector : Collection->VectorParameters)
    {
        VectorArray.Add(MakeShared<FJsonValueObject>(VectorParameterToJson(Vector)));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetArrayField(TEXT("scalars"), ScalarArray);
    Result->SetArrayField(TEXT("vectors"), VectorArray);
    Ctx.SendSuccess(Result);
    return true;
}

// ============================================================================
// add_collection_parameter_node
// ============================================================================
//
// The critical bind: a UMaterialExpressionCollectionParameter needs Collection,
// ParameterName, AND ParameterId set in one shot. Without the Id lookup the
// node renders "(Invalid Parameter)" even if Collection + ParameterName are
// correct (Id is what the renderer keys off, name is just for display and
// rename-tracking). We mirror the use_material_function flow at
// MaterialAuthoringHandler.cpp:1486-1515 for the cross-asset typed node drop.

REGISTER_RPC_HANDLER("material.authoring.add_collection_parameter_node", "material.authoring",
    "Drop a CollectionParameter node into a material's graph, bound atomically to (collection, parameterName, parameterId). Both the collection asset and the named parameter must already exist; missing parameters return INVALID_PARAMS rather than silently leaving an unbound node.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Material asset path"),
        RPC_PARAM_REQ("collectionPath", "path", "UMaterialParameterCollection asset path"),
        RPC_PARAM_REQ("parameterName", "string", "Existing parameter on the collection"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_OPT("comment", "string", "Optional node comment shown in the editor")
    ))
{
    MPC_LOAD_MATERIAL_OR_RETURN();
    MPC_REQUIRE_NODE_POSITION_OR_RETURN(ExprX, ExprY);

    FString CollectionPath;
    if (!Ctx.RequireString(TEXT("collectionPath"), CollectionPath)) return true;
    FString ParameterName;
    if (!Ctx.RequireString(TEXT("parameterName"), ParameterName)) return true;

    UMaterialParameterCollection* Collection =
        LoadObject<UMaterialParameterCollection>(nullptr, *CollectionPath);
    if (!Collection)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"),
            FString::Printf(TEXT("Could not load UMaterialParameterCollection at '%s'."), *CollectionPath));
        return true;
    }

    const FName ParamFName(*ParameterName);
    const FGuid ParameterId = Collection->GetParameterId(ParamFName);
    if (!ParameterId.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("Parameter '%s' does not exist on collection '%s'."),
                *ParameterName, *CollectionPath));
        return true;
    }

    UMaterialExpressionCollectionParameter* Node = NewObject<UMaterialExpressionCollectionParameter>(
        Material, UMaterialExpressionCollectionParameter::StaticClass(), NAME_None, RF_Transactional);
    // Atomic three-field bind — see file header comment.
    Node->Collection = Collection;
    Node->ParameterName = ParamFName;
    Node->ParameterId = ParameterId;

    const FString Comment = Ctx.GetString(TEXT("comment"));
    if (!Comment.IsEmpty())
    {
        Node->Desc = Comment;
        Node->bCommentBubbleVisible = true;
    }

    MPC_FINALIZE_EXPR_AND_RESPOND(Node, TEXT("Collection parameter node added."));
}
