// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"

#include "Utils/AssetUtils.h"
#include "Utils/PropertyUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "UObject/Object.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include "ScopedTransaction.h"

namespace
{
    static FString ToSnakeCase(const FString& Name)
    {
        FString Out;
        Out.Reserve(Name.Len() + 8);
        for (int32 i = 0; i < Name.Len(); ++i)
        {
            const TCHAR C = Name[i];
            if (i > 0 && FChar::IsUpper(C) && !FChar::IsUpper(Name[i - 1]))
            {
                Out.AppendChar(TEXT('_'));
            }
            Out.AppendChar(FChar::ToLower(C));
        }
        return Out;
    }

    static TSharedPtr<FJsonValue> LookupArg(const TSharedPtr<FJsonObject>& Args,
                                            const FString& ParamName)
    {
        if (!Args.IsValid())
        {
            return nullptr;
        }
        TSharedPtr<FJsonValue> Hit = Args->TryGetField(ParamName);
        if (Hit.IsValid())
        {
            return Hit;
        }
        const FString Snake = ToSnakeCase(ParamName);
        if (!Snake.Equals(ParamName))
        {
            Hit = Args->TryGetField(Snake);
            if (Hit.IsValid())
            {
                return Hit;
            }
        }
        return nullptr;
    }
}

REGISTER_RPC_HANDLER("object.call_function", "object",
    "Invoke a UFUNCTION on any UObject (actor, component, asset, subsystem) "
    "by full path. Converts JSON args through the property layer used by "
    "property.set, serializes return value and out-params back to the caller. "
    "Refuses ClassDefaultObject targets.",
    RPC_PARAMS(
        RPC_PARAM_REQ("objectPath", "path", "Full UObject path (e.g. /Game/Maps/MyMap.MyMap:PersistentLevel.Actor_0 or transient subobject path)."),
        RPC_PARAM_REQ("function", "string", "Exact reflected UFUNCTION name (case-insensitive)."),
        RPC_PARAM_OPT("args", "object", "Map of parameter name → JSON value. Accepts camelCase or snake_case aliases.")
    ))
{
    auto LookupTopLevelString = [&](std::initializer_list<const TCHAR*> Aliases) -> FString
    {
        for (const TCHAR* Alias : Aliases)
        {
            FString Value = Ctx.GetString(Alias);
            if (!Value.IsEmpty())
            {
                return Value;
            }
        }
        return FString();
    };

    const FString ObjectPath = LookupTopLevelString({TEXT("objectPath"), TEXT("object_path")});
    const FString FunctionName = LookupTopLevelString({TEXT("function"), TEXT("functionName"), TEXT("function_name")});

    if (ObjectPath.IsEmpty() || FunctionName.IsEmpty())
    {
        Ctx.SendError(TEXT("MISSING_PARAM"), TEXT("objectPath and function are required"));
        return true;
    }

    FString ResolveError;
    UObject* Target = ResolveUObjectByPath(ObjectPath, ResolveError);
    if (!Target)
    {
        Ctx.SendError(TEXT("OBJECT_NOT_FOUND"),
            ResolveError.IsEmpty()
                ? FString::Printf(TEXT("Object not found: %s"), *ObjectPath)
                : ResolveError);
        return true;
    }

    if (Target->HasAnyFlags(RF_ClassDefaultObject))
    {
        Ctx.SendError(TEXT("INVALID_TARGET"),
            FString::Printf(TEXT("Refusing to invoke on ClassDefaultObject: %s"), *ObjectPath));
        return true;
    }

    UFunction* Fn = Target->FindFunction(FName(*FunctionName));
    if (!Fn)
    {
        Ctx.SendError(TEXT("FUNCTION_NOT_FOUND"),
            FString::Printf(TEXT("Function not found on %s: %s"),
                *Target->GetClass()->GetName(), *FunctionName));
        return true;
    }

    TSharedPtr<FJsonObject> Args = Ctx.GetObject(TEXT("args"));

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: object.call_function")));
    Target->Modify();

    if (Fn->ParmsSize == 0)
    {
        // AActor::ProcessEvent silently no-ops in editor worlds unless
        // GAllowActorScriptExecutionInEditor is true (Actor.cpp ~L1444). The
        // guard flips that flag for the duration of this call so reflective
        // setters/getters actually run on editor-spawned actors.
        FEditorScriptExecutionGuard ScriptGuard;
        Target->ProcessEvent(Fn, nullptr);

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("objectPath"), Target->GetPathName());
        Data->SetStringField(TEXT("function"), Fn->GetName());
        Data->SetBoolField(TEXT("void"), true);
        Ctx.SendSuccess(Data);
        return true;
    }

    uint8* Parms = static_cast<uint8*>(
        FMemory::Malloc(Fn->ParmsSize, Fn->GetMinAlignment()));
    FMemory::Memzero(Parms, Fn->ParmsSize);
    ON_SCOPE_EXIT
    {
        FMemory::Free(Parms);
    };

    struct FParamRecord
    {
        FProperty* Prop = nullptr;
        bool bIsReturn = false;
        bool bIsOut = false;      // real out (export to outParams)
        bool bReadFromArgs = false;
    };
    TArray<FParamRecord> Records;
    Records.Reserve(8);

    // ProcessEvent expects a UFunction parameter frame; only CPF_Parm slots
    // inside that frame should be initialized and later destroyed.
    auto DestroyAll = [&]()
    {
        for (const FParamRecord& Rec : Records)
        {
            if (Rec.Prop && Rec.Prop->GetSize() > 0)
            {
                Rec.Prop->DestroyValue_InContainer(Parms);
            }
        }
    };

    FString ApplyError;
    bool bMissingRequired = false;
    FString MissingName;
    FString InvalidTypeMessage;
    bool bInvalidType = false;

    for (TFieldIterator<FProperty> It(Fn); It && (It->PropertyFlags & CPF_Parm); ++It)
    {
        FProperty* P = *It;
        if (P->GetSize() > 0)
        {
            P->InitializeValue_InContainer(Parms);
        }

        const EPropertyFlags Flags = P->GetPropertyFlags();
        FParamRecord Rec;
        Rec.Prop = P;

        if (Flags & CPF_ReturnParm)
        {
            Rec.bIsReturn = true;
            Records.Add(Rec);
            continue;
        }

        const bool bIsOutFlag = (Flags & CPF_OutParm) != 0;
        const bool bIsConst   = (Flags & CPF_ConstParm) != 0;
        const bool bIsRefFlag = (Flags & CPF_ReferenceParm) != 0;

        // Classification:
        // - Out + Const (no Return)        → const-ref input (read, do NOT export)
        // - Out + Ref   (no Const, no Ret) → in-out (read AND export)
        // - Out only    (no Const, no Ret) → real out-param (no read, export)
        // - Otherwise                       → pure input (read)
        bool bRead = true;
        bool bExport = false;

        if (bIsOutFlag && !bIsConst)
        {
            bExport = true;
            if (!bIsRefFlag)
            {
                bRead = false; // pure out-param
            }
        }

        Rec.bIsOut = bExport;
        Rec.bReadFromArgs = bRead;
        Records.Add(Rec);

        if (!bRead)
        {
            continue;
        }

        const FString PName = P->GetName();
        TSharedPtr<FJsonValue> JsonVal = LookupArg(Args, PName);

        if (!JsonVal.IsValid())
        {
            // Allow function defaults declared via the CPP_Default_<ParamName>
            // metadata key on the UFunction. Editor-only metadata, but this
            // module is Editor-only. CPF_HasDefaultValue was removed in UE 5.6,
            // so the metadata probe is the only portable check available now.
            const FName DefaultKey(*(FString(TEXT("CPP_Default_")) + PName));
            if (Fn->HasMetaData(DefaultKey))
            {
                continue;
            }
            bMissingRequired = true;
            MissingName = PName;
            break;
        }

        if (!ApplyJsonValueToProperty(Parms, P, JsonVal, ApplyError))
        {
            bInvalidType = true;
            InvalidTypeMessage = FString::Printf(TEXT("Failed to apply '%s': %s"), *PName, *ApplyError);
            break;
        }
    }

    if (bMissingRequired)
    {
        DestroyAll();
        Ctx.SendError(TEXT("MISSING_PARAM"),
            FString::Printf(TEXT("Required parameter missing: %s"), *MissingName));
        return true;
    }
    if (bInvalidType)
    {
        DestroyAll();
        Ctx.SendError(TEXT("INVALID_PARAM_TYPE"), InvalidTypeMessage);
        return true;
    }

    {
        // See note above the zero-param ProcessEvent: editor-world AActors
        // skip reflective UFUNCTION execution unless this guard is in scope.
        FEditorScriptExecutionGuard ScriptGuard;
        Target->ProcessEvent(Fn, Parms);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("objectPath"), Target->GetPathName());
    Data->SetStringField(TEXT("function"), Fn->GetName());

    bool bHasReturn = false;
    TSharedPtr<FJsonObject> OutParams = MakeShared<FJsonObject>();
    bool bHasOutParam = false;
    FString ExportError;

    for (const FParamRecord& Rec : Records)
    {
        if (Rec.bIsReturn)
        {
            const FPropertyExportResult RetVal = ExportPropertyToJsonValueStrict(
                FPropertyExportSource::FromRaw(Parms), Rec.Prop);
            if (RetVal.bSupported && RetVal.Value.IsValid())
            {
                Data->SetField(TEXT("returnValue"), RetVal.Value);
                bHasReturn = true;
            }
            else
            {
                ExportError = FString::Printf(TEXT("Unsupported return type: %s"),
                    *Rec.Prop->GetCPPType());
                break;
            }
        }
        else if (Rec.bIsOut)
        {
            const FPropertyExportResult OutVal = ExportPropertyToJsonValueStrict(
                FPropertyExportSource::FromRaw(Parms), Rec.Prop);
            if (OutVal.bSupported && OutVal.Value.IsValid())
            {
                OutParams->SetField(Rec.Prop->GetName(), OutVal.Value);
                bHasOutParam = true;
            }
            else
            {
                ExportError = FString::Printf(TEXT("Unsupported out parameter '%s' type: %s"),
                    *Rec.Prop->GetName(), *Rec.Prop->GetCPPType());
                break;
            }
        }
    }

    DestroyAll();

    if (!ExportError.IsEmpty())
    {
        Ctx.SendError(TEXT("UNSUPPORTED_PARAM_TYPE"), ExportError);
        return true;
    }

    Data->SetBoolField(TEXT("void"), !bHasReturn);
    if (bHasOutParam)
    {
        Data->SetObjectField(TEXT("outParams"), OutParams);
    }

    Ctx.SendSuccess(Data);
    return true;
}
