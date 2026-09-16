// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/ClassUtils.h"
#include "Utils/PropertyExport.h"
#include "Utils/PropertyImport.h"
#include "Utils/PropertyInspection.h"
#include "Utils/AssetUtils.h"

#include "AssetToolsModule.h"
#include "EditorAssetLibrary.h"
#include "Factories/Factory.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/EngineVersion.h"

namespace
{
    const TCHAR* GetMappingStoragePath()
    {
#if (ENGINE_MAJOR_VERSION > 5) || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
        return TEXT("DefaultKeyMappings.Mappings");
#else
        return TEXT("Mappings");
#endif
    }

    TSharedPtr<FJsonObject> ExportInputMappingObject(const UObject* Object)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        if (!Object)
        {
            return Result;
        }

        Result->SetStringField(TEXT("class"), Object->GetClass()->GetName());
        Result->SetStringField(TEXT("classPath"), Object->GetClass()->GetPathName());

        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property->HasAnyPropertyFlags(CPF_Edit) ||
                Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated | CPF_SkipSerialization))
            {
                continue;
            }

            if (TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(
                    const_cast<UObject*>(Object), Property))
            {
                Properties->SetField(Property->GetName(), Value);
            }
        }
        Result->SetObjectField(TEXT("properties"), Properties);
        return Result;
    }

    template <typename TObjectType>
    TArray<TSharedPtr<FJsonValue>> ExportInputMappingObjects(
        const TArray<TObjectPtr<TObjectType>>& Objects)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        Result.Reserve(Objects.Num());
        for (const TObjectPtr<TObjectType>& Object : Objects)
        {
            Result.Add(MakeShared<FJsonValueObject>(ExportInputMappingObject(Object.Get())));
        }
        return Result;
    }

    TSharedPtr<FJsonObject> ExportInputMapping(const FEnhancedActionKeyMapping& Mapping)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        const UInputAction* Action = Mapping.Action.Get();
        Result->SetStringField(TEXT("actionPath"), Action ? Action->GetPathName() : FString());
        Result->SetStringField(TEXT("key"), Mapping.Key.ToString());
        Result->SetArrayField(TEXT("modifiers"), ExportInputMappingObjects(Mapping.Modifiers));
        Result->SetArrayField(TEXT("triggers"), ExportInputMappingObjects(Mapping.Triggers));
        return Result;
    }

    template <typename TObjectType>
    bool BuildInputMappingObjects(
        const TArray<TSharedPtr<FJsonValue>>* Specs,
        const TCHAR* ParamName,
        UInputMappingContext* Context,
        TArray<TObjectPtr<TObjectType>>& OutObjects,
        FString& OutErrorCode,
        FString& OutError)
    {
        if (!Specs)
        {
            return true;
        }

        OutObjects.Reserve(Specs->Num());
        for (int32 Index = 0; Index < Specs->Num(); ++Index)
        {
            const TSharedPtr<FJsonObject>* Spec = nullptr;
            if (!(*Specs)[Index].IsValid() || !(*Specs)[Index]->TryGetObject(Spec) ||
                !Spec || !Spec->IsValid())
            {
                OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutError = FString::Printf(TEXT("%s[%d] must be an object with class and optional properties fields."),
                    ParamName, Index);
                return false;
            }

            FString ClassName;
            if (!(*Spec)->TryGetStringField(TEXT("class"), ClassName) || ClassName.IsEmpty())
            {
                OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutError = FString::Printf(TEXT("%s[%d].class is required."), ParamName, Index);
                return false;
            }

            UClass* ObjectClass = ResolveClassByName(ClassName);
            if (!ObjectClass || !ObjectClass->IsChildOf(TObjectType::StaticClass()) ||
                ObjectClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            {
                OutErrorCode = ErrorCodes::ERR_INVALID_CLASS;
                OutError = FString::Printf(TEXT("%s[%d].class '%s' is not a concrete %s class."),
                    ParamName, Index, *ClassName, *TObjectType::StaticClass()->GetName());
                return false;
            }

            const TSharedPtr<FJsonObject>* Properties = nullptr;
            if ((*Spec)->HasField(TEXT("properties")) &&
                (!(*Spec)->TryGetObjectField(TEXT("properties"), Properties) ||
                    !Properties || !Properties->IsValid()))
            {
                OutErrorCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutError = FString::Printf(TEXT("%s[%d].properties must be an object."), ParamName, Index);
                return false;
            }

            TObjectType* Object = NewObject<TObjectType>(Context, ObjectClass);
            if (!Object)
            {
                OutErrorCode = ErrorCodes::ERR_CREATION_FAILED;
                OutError = FString::Printf(TEXT("Failed to create %s[%d] from class '%s'."),
                    ParamName, Index, *ClassName);
                return false;
            }

            if (Properties && Properties->IsValid())
            {
                for (const TPair<FString, TSharedPtr<FJsonValue>> Pair : (*Properties)->Values)
                {
                    FProperty* Property = FindPropertyCI(ObjectClass, Pair.Key);
                    if (!Property)
                    {
                        OutErrorCode = ErrorCodes::ERR_PROPERTY_NOT_FOUND;
                        OutError = FString::Printf(TEXT("%s[%d].properties.%s does not exist on %s."),
                            ParamName, Index, *Pair.Key, *ObjectClass->GetName());
                        return false;
                    }

                    FString ApplyError;
                    if (!ApplyJsonValueToProperty(Object, Property, Pair.Value, ApplyError))
                    {
                        OutErrorCode = ErrorCodes::ERR_PROPERTY_SET_FAILED;
                        OutError = FString::Printf(TEXT("Could not set %s[%d].properties.%s on %s: %s"),
                            ParamName, Index, *Pair.Key, *ObjectClass->GetName(), *ApplyError);
                        return false;
                    }
                }
            }

            OutObjects.Add(Object);
        }
        return true;
    }
}

// ---- input.create_input_action ----
REGISTER_RPC_HANDLER("input.create_input_action", "input", "Create a new Enhanced Input UInputAction asset at the given folder. Idempotent: re-running against an existing UInputAction returns it with existing:true, mode:\"updated_in_place\". Pass overwrite:true to replace it instead (ASSET_IN_USE when other packages reference it). Errors ASSET_ALREADY_EXISTS when a different asset class occupies the path; auto-saves on success.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset name (without extension), e.g. 'IA_Jump'."),
        RPC_PARAM_REQ("path", "path", "Content-browser folder for the new asset, e.g. /Game/Input."),
        RPC_PARAM_DEF("overwrite", "boolean", "Delete and recreate an existing Input Action instead of returning it. Rejected with ASSET_IN_USE when any package still references it.", "false")
    ))
{
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = Ctx.GetString(TEXT("path"));

    if (Name.IsEmpty() || Path.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Name and path are required."));
        return true;
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *Path, *Name);

    // Replaces the registry-only DoesAssetExist / ASSET_EXISTS pre-check: that missed a
    // same-session in-memory asset and let it reach CanCreateAsset's modal chain.
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        FullPath, Name, UInputAction::StaticClass(), Ctx.GetBool(TEXT("overwrite"), false));
    if (Resolution.IsRejected())
    {
        return AssetCreatePolicy::SendRejection(Ctx, Resolution);
    }
    if (Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace)
    {
        // The verb carries no per-call configuration, so an existing action already IS
        // the requested state. Returning it preserves the IMC mappings pointing at it.
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("assetPath"), Resolution.Existing->GetPathName());
        AssetCreatePolicy::AddCreateReport(Result, Resolution);
        AddAssetVerification(Result, Resolution.Existing);
        Ctx.SendSuccess(Result);
        return true;
    }

    IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

    UClass* ActionClass = UInputAction::StaticClass();
    UObject* NewAsset = AssetTools.CreateAsset(Name, Path, ActionClass, nullptr);

    if (NewAsset)
    {
        SaveLoadedAssetThrottled(NewAsset, -1.0, true);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("assetPath"), NewAsset->GetPathName());
        AssetCreatePolicy::AddCreateReport(Result, Resolution);
        AddAssetVerification(Result, NewAsset);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_CREATION_FAILED, TEXT("Failed to create Input Action."));
    }
    return true;
}

// ---- input.create_input_mapping_context ----
REGISTER_RPC_HANDLER("input.create_input_mapping_context", "input", "Create a new Enhanced Input UInputMappingContext (IMC) asset. Idempotent: re-running against an existing IMC returns it with existing:true, mode:\"updated_in_place\", keeping the mappings already added to it. Pass overwrite:true to replace it instead (ASSET_IN_USE when other packages reference it). Errors ASSET_ALREADY_EXISTS when a different asset class occupies the path; auto-saves on success.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset name (without extension), e.g. 'IMC_Default'."),
        RPC_PARAM_REQ("path", "path", "Content-browser folder for the new asset, e.g. /Game/Input."),
        RPC_PARAM_DEF("overwrite", "boolean", "Delete and recreate an existing IMC (discarding its key mappings) instead of returning it. Rejected with ASSET_IN_USE when any package still references it.", "false")
    ))
{
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = Ctx.GetString(TEXT("path"));

    if (Name.IsEmpty() || Path.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Name and path are required."));
        return true;
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *Path, *Name);

    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        FullPath, Name, UInputMappingContext::StaticClass(), Ctx.GetBool(TEXT("overwrite"), false));
    if (Resolution.IsRejected())
    {
        return AssetCreatePolicy::SendRejection(Ctx, Resolution);
    }
    if (Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace)
    {
        // Returning the existing IMC is what an idempotent re-run wants: recreating it
        // would drop every input.add_mapping already applied.
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("assetPath"), Resolution.Existing->GetPathName());
        AssetCreatePolicy::AddCreateReport(Result, Resolution);
        AddAssetVerification(Result, Resolution.Existing);
        Ctx.SendSuccess(Result);
        return true;
    }

    IAssetTools& AssetTools = FModuleManager::Get().LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

    UClass* ContextClass = UInputMappingContext::StaticClass();
    UObject* NewAsset = AssetTools.CreateAsset(Name, Path, ContextClass, nullptr);

    if (NewAsset)
    {
        SaveLoadedAssetThrottled(NewAsset, -1.0, true);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("assetPath"), NewAsset->GetPathName());
        AssetCreatePolicy::AddCreateReport(Result, Resolution);
        AddAssetVerification(Result, NewAsset);
        Ctx.SendSuccess(Result);
    }
    else
    {
        Ctx.SendError(ErrorCodes::ERR_CREATION_FAILED,
            TEXT("Failed to create Input Mapping Context."));
    }
    return true;
}

// ---- input.add_mapping ----
REGISTER_RPC_HANDLER("input.add_mapping", "input", "Bind a hardware key to an Input Action inside an Input Mapping Context, optionally with ordered per-mapping modifiers and triggers. The IMC asset is loaded, modified, saved, and the stored mapping is returned. Multiple keys can map to the same action by calling repeatedly.",
    RPC_PARAMS(
        RPC_PARAM_REQ("contextPath", "path", "Object path to the UInputMappingContext asset."),
        RPC_PARAM_REQ("actionPath", "path", "Object path to the UInputAction asset to be triggered by the key."),
        RPC_PARAM_REQ("key", "string", "FKey name to bind, e.g. 'SpaceBar', 'LeftMouseButton', 'Gamepad_FaceButton_Bottom'."),
        RPC_PARAM_OPT_NESTED("modifiers", "array", "Ordered per-mapping modifier descriptors. Each entry is {class, properties}; class accepts a short name such as InputModifierSwizzleAxis or a full class path, and properties is an optional object applied through the shared reflected-property writer.", TEXT("class"), TEXT("properties")),
        RPC_PARAM_OPT_NESTED("triggers", "array", "Ordered per-mapping trigger descriptors. Each entry is {class, properties}; class accepts a short name such as InputTriggerHold or a full class path, and properties is an optional object applied through the shared reflected-property writer.", TEXT("class"), TEXT("properties"))
    ))
{
    FString ContextPath = Ctx.GetString(TEXT("contextPath"));
    FString ActionPath = Ctx.GetString(TEXT("actionPath"));
    FString KeyName = Ctx.GetString(TEXT("key"));

    UInputMappingContext* Context =
        Cast<UInputMappingContext>(ResolveAsset(ContextPath, /*bLoadObject=*/true).Object);
    UInputAction* InAction =
        Cast<UInputAction>(ResolveAsset(ActionPath, /*bLoadObject=*/true).Object);

    if (!Context || !InAction || KeyName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("Invalid context, action, or key."));
        return true;
    }

    FKey Key = FKey(FName(*KeyName));
    if (!Key.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Invalid key name."));
        return true;
    }

    TArray<TObjectPtr<UInputModifier>> Modifiers;
    TArray<TObjectPtr<UInputTrigger>> Triggers;
    FString ConfigErrorCode;
    FString ConfigError;
    if (!BuildInputMappingObjects(Ctx.GetArray(TEXT("modifiers")), TEXT("modifiers"),
            Context, Modifiers, ConfigErrorCode, ConfigError) ||
        !BuildInputMappingObjects(Ctx.GetArray(TEXT("triggers")), TEXT("triggers"),
            Context, Triggers, ConfigErrorCode, ConfigError))
    {
        Ctx.SendError(ConfigErrorCode, ConfigError);
        return true;
    }

    Context->Modify();
    FEnhancedActionKeyMapping& Mapping = Context->MapKey(InAction, Key);
    Mapping.Modifiers = MoveTemp(Modifiers);
    Mapping.Triggers = MoveTemp(Triggers);
    SaveLoadedAssetThrottled(Context, -1.0, true);

    const FEnhancedActionKeyMapping& StoredMapping = Context->GetMappings().Last();
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> MappingResult = ExportInputMapping(StoredMapping);
    Result->SetStringField(TEXT("contextPath"), Context->GetPathName());
    Result->SetStringField(TEXT("actionPath"), MappingResult->GetStringField(TEXT("actionPath")));
    Result->SetStringField(TEXT("key"), MappingResult->GetStringField(TEXT("key")));
    Result->SetArrayField(TEXT("modifiers"), MappingResult->GetArrayField(TEXT("modifiers")));
    Result->SetArrayField(TEXT("triggers"), MappingResult->GetArrayField(TEXT("triggers")));
    Result->SetStringField(TEXT("mappingStorage"), GetMappingStoragePath());
    AddAssetVerificationNested(Result, TEXT("contextVerification"), Context);
    AddAssetVerificationNested(Result, TEXT("actionVerification"), InAction);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- input.remove_mapping ----
REGISTER_RPC_HANDLER("input.remove_mapping", "input", "Unbind every key currently mapped to an Input Action inside an Input Mapping Context. Reports the list of keys that were removed; saves the IMC.",
    RPC_PARAMS(
        RPC_PARAM_REQ("contextPath", "path", "Object path to the UInputMappingContext asset to modify."),
        RPC_PARAM_REQ("actionPath", "path", "Object path to the UInputAction whose key bindings should be cleared from the context.")
    ))
{
    FString ContextPath = Ctx.GetString(TEXT("contextPath"));
    FString ActionPath = Ctx.GetString(TEXT("actionPath"));

    UInputMappingContext* Context =
        Cast<UInputMappingContext>(ResolveAsset(ContextPath, /*bLoadObject=*/true).Object);
    UInputAction* InAction =
        Cast<UInputAction>(ResolveAsset(ActionPath, /*bLoadObject=*/true).Object);

    if (!Context || !InAction)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("Invalid context or action."));
        return true;
    }

    TArray<FKey> KeysToRemove;
    for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
    {
        if (Mapping.Action == InAction)
        {
            KeysToRemove.Add(Mapping.Key);
        }
    }
    for (const FKey& KeyToRemove : KeysToRemove)
    {
        Context->UnmapKey(InAction, KeyToRemove);
    }
    SaveLoadedAssetThrottled(Context, -1.0, true);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("contextPath"), ContextPath);
    Result->SetStringField(TEXT("actionPath"), ActionPath);
    Result->SetNumberField(TEXT("keysRemoved"), KeysToRemove.Num());
    TArray<TSharedPtr<FJsonValue>> RemovedKeys;
    for (const FKey& Key : KeysToRemove)
    {
        RemovedKeys.Add(MakeShared<FJsonValueString>(Key.ToString()));
    }
    Result->SetArrayField(TEXT("removedKeys"), RemovedKeys);
    AddAssetVerificationNested(Result, TEXT("contextVerification"), Context);
    AddAssetVerificationNested(Result, TEXT("actionVerification"), InAction);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- input.get_input_info ----
REGISTER_RPC_HANDLER("input.get_input_info", "input", "Inspect an Input Action or Input Mapping Context asset. For UInputAction returns valueType and bConsumeInput; for UInputMappingContext returns mappingCount, mappingStorage, and mappings with each key's action, modifiers, and triggers.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Object path to the input asset; accepts either UInputAction or UInputMappingContext.")
    ))
{
    FString AssetPath = Ctx.GetString(TEXT("assetPath"));

    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("assetPath is required."));
        return true;
    }

    UObject* Asset = ResolveAsset(AssetPath, /*bLoadObject=*/true).Object;
    if (!Asset)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_FOUND,
            FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), AssetPath);
    Result->SetStringField(TEXT("assetClass"), Asset->GetClass()->GetName());
    Result->SetStringField(TEXT("assetName"), Asset->GetName());

    if (UInputAction* InputAction = Cast<UInputAction>(Asset))
    {
        Result->SetStringField(TEXT("type"), TEXT("InputAction"));
        Result->SetStringField(TEXT("valueType"), FString::FromInt((int32)InputAction->ValueType));
        // Emit the field under the documented key (REGISTER_RPC_HANDLER summary
        // above) and the underlying UE property name `bConsumeInput`, keeping the
        // b-prefix like every other bool UPROPERTY mirror across the handlers.
        Result->SetBoolField(TEXT("bConsumeInput"), InputAction->bConsumeInput);
    }
    else if (UInputMappingContext* Context = Cast<UInputMappingContext>(Asset))
    {
        Result->SetStringField(TEXT("type"), TEXT("InputMappingContext"));
        Result->SetNumberField(TEXT("mappingCount"), Context->GetMappings().Num());
        Result->SetStringField(TEXT("mappingStorage"), GetMappingStoragePath());
        TArray<TSharedPtr<FJsonValue>> Mappings;
        Mappings.Reserve(Context->GetMappings().Num());
        for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
        {
            Mappings.Add(MakeShared<FJsonValueObject>(ExportInputMapping(Mapping)));
        }
        Result->SetArrayField(TEXT("mappings"), Mappings);
    }

    AddAssetVerification(Result, Asset);
    Ctx.SendSuccess(Result);
    return true;
}
