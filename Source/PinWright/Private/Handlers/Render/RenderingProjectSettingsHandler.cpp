// Copyright (c) 2026 Alexander Penkin. MIT License.

// RenderingProjectSettingsHandler.cpp — rendering.* namespace targeting URendererSettings.
// Typed read/write surface for the developer-settings class persisted to
// DefaultEngine.ini ([/Script/Engine.RendererSettings]). See the generated wiki
// page rendering.md for design rationale.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Environment/GIMethodCVarHelper.h"
#include "Utils/AssetSaveState.h"
#include "Utils/PropertyUtils.h"
#include "Compat/JsonKeyCompat.h"

#include "Engine/RendererSettings.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace
{
    // Filter: include UPROPERTYs that round-trip through the project config ini.
    // CPF_Config and CPF_GlobalConfig are the engine markers; anything missing both
    // is editor-state-only and shouldn't appear in get/set.
    bool IsConfigProperty(const FProperty* Prop)
    {
        return Prop && Prop->HasAnyPropertyFlags(static_cast<EPropertyFlags>(CPF_Config | CPF_GlobalConfig));
    }

    // Build the {settings:{}, configFile:""} response object. Optional substring
    // filter is matched case-insensitive against the FProperty name.
    TSharedPtr<FJsonObject> BuildSettingsSnapshot(URendererSettings* S, const FString& Filter)
    {
        TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> It(S->GetClass()); It; ++It)
        {
            FProperty* Prop = *It;
            if (!IsConfigProperty(Prop)) continue;
            const FString Name = Prop->GetName();
            if (!Filter.IsEmpty() && !Name.Contains(Filter)) continue;
            TSharedPtr<FJsonValue> JsonValue = ExportPropertyToJsonValue(S, Prop);
            if (JsonValue.IsValid())
            {
                Settings->SetField(Name, JsonValue);
            }
        }

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetObjectField(TEXT("settings"), Settings);
        Resp->SetStringField(TEXT("configFile"), S->GetDefaultConfigFilename());
        return Resp;
    }

    // Apply one {name → JsonValue} update to S. Pushes results into Applied/Rejected
    // arrays and returns the FProperty* of a successful write (or nullptr) so the caller
    // can batch a single OnObjectPropertyChanged broadcast after the loop.
    FProperty* ApplyOneUpdate(URendererSettings* S, const FString& Name,
        const TSharedPtr<FJsonValue>& Value,
        TArray<TSharedPtr<FJsonValue>>& Applied,
        TArray<TSharedPtr<FJsonValue>>& Rejected)
    {
        auto Reject = [&Rejected](const FString& N, const TCHAR* Reason)
        {
            TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("name"), N);
            R->SetStringField(TEXT("reason"), Reason);
            Rejected.Add(MakeShared<FJsonValueObject>(R));
        };

        FProperty* Prop = FindPropertyCI(S->GetClass(), Name);
        if (!Prop)
        {
            Reject(Name, TEXT("unknown_property"));
            return nullptr;
        }
        if (!IsConfigProperty(Prop))
        {
            Reject(Name, TEXT("not_config_serializable"));
            return nullptr;
        }

        FString ApplyError;
        if (!ApplyJsonValueToProperty(S, Prop, Value, ApplyError))
        {
            Reject(Name, *ApplyError);
            return nullptr;
        }

        Applied.Add(MakeShared<FJsonValueString>(Prop->GetName()));
        return Prop;
    }

    // Core write loop, factored so the typed wrappers (set_lumen_method,
    // set_dynamic_gi_method) can reuse it.
    TSharedPtr<FJsonObject> ApplyUpdates(URendererSettings* S,
        const TSharedPtr<FJsonObject>& Updates, bool bSave, bool& bOutSaveFailed)
    {
        bOutSaveFailed = false;
        TArray<TSharedPtr<FJsonValue>> Applied;
        TArray<TSharedPtr<FJsonValue>> Rejected;
        FProperty* LastApplied = nullptr;

        if (Updates.IsValid())
        {
            for (const auto& Pair : Updates->Values)
            {
                if (FProperty* Prop = ApplyOneUpdate(S, EARGCompat::JsonKeyToString(Pair.Key), Pair.Value, Applied, Rejected))
                {
                    LastApplied = Prop;
                }
            }
        }

        // Single broadcast at the end so any open Project Settings tab rebuilds once,
        // not once per applied property. The event source must be a real FProperty —
        // UE observers (e.g. property editor row widgets) dereference it without
        // null-checking, so a nullptr event would crash. Use the last successful
        // write's property as the representative source.
        if (LastApplied)
        {
            FPropertyChangedEvent ChangeEvent(LastApplied, EPropertyChangeType::ValueSet);
            FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(S, ChangeEvent);
        }

        const bool bSaveRequested = bSave && Applied.Num() > 0;
        bool bSaved = false;
        EAssetSaveState SaveState = EAssetSaveState::NotRequested;
        FString ConfigFile;
        if (bSaveRequested)
        {
            ConfigFile = S->GetDefaultConfigFilename();
            const bool bWriterSucceeded = S->TryUpdateDefaultConfigFile(ConfigFile);
            bSaved = bWriterSucceeded && IFileManager::Get().FileSize(*ConfigFile) >= 0;
            SaveState = bSaved ? EAssetSaveState::Written : EAssetSaveState::Failed;
            bOutSaveFailed = !bSaved;
        }

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetArrayField(TEXT("applied"), Applied);
        Resp->SetArrayField(TEXT("rejected"), Rejected);
        Resp->SetBoolField(TEXT("saveRequested"), bSaveRequested);
        Resp->SetBoolField(TEXT("saved"), bSaved);
        Resp->SetStringField(TEXT("saveState"), AssetSaveStateToWire(SaveState));
        if (SaveState == EAssetSaveState::NotRequested)
        {
            Resp->SetStringField(TEXT("saveDetail"),
                TEXT("No config save was requested; applied settings remain in memory only."));
        }
        else if (SaveState == EAssetSaveState::Written)
        {
            Resp->SetStringField(TEXT("saveDetail"),
                TEXT("This call wrote DefaultEngine.ini and confirmed the file is present."));
            Resp->SetStringField(TEXT("savedTo"), ConfigFile);
        }
        else
        {
            Resp->SetStringField(TEXT("saveDetail"),
                TEXT("The config writer did not establish a durable DefaultEngine.ini update; clear the write failure and retry."));
        }
        return Resp;
    }
}

// ---- rendering.get_project_settings ----
REGISTER_RPC_HANDLER("rendering.get_project_settings", "rendering",
    "Read URendererSettings UPROPERTYs (DefaultEngine.ini → [/Script/Engine.RendererSettings])",
    RPC_PARAMS(
        RPC_PARAM_OPT("filter", "string", "Case-insensitive substring filter on property name")
    ))
{
    URendererSettings* S = GetMutableDefault<URendererSettings>();
    if (!S)
    {
        Ctx.SendError(ErrorCodes::ERR_INTERNAL_ERROR, TEXT("Failed to resolve URendererSettings CDO"));
        return true;
    }
    const FString Filter = Ctx.GetString(TEXT("filter"));
    Ctx.SendSuccess(BuildSettingsSnapshot(S, Filter));
    return true;
}

// ---- rendering.set_project_settings ----
REGISTER_RPC_HANDLER("rendering.set_project_settings", "rendering",
    "Write URendererSettings UPROPERTYs and (optionally) persist to DefaultEngine.ini",
    RPC_PARAMS(
        RPC_PARAM_REQ("updates", "object", "Map of UPROPERTY name → JSON value to apply"),
        RPC_PARAM_DEF("save", "boolean", "Persist to DefaultEngine.ini after applying", "true")
    ))
{
    URendererSettings* S = GetMutableDefault<URendererSettings>();
    if (!S)
    {
        Ctx.SendError(ErrorCodes::ERR_INTERNAL_ERROR, TEXT("Failed to resolve URendererSettings CDO"));
        return true;
    }

    TSharedPtr<FJsonObject> Updates;
    if (!Ctx.RequireObject(TEXT("updates"), Updates))
    {
        return true;
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    bool bSaveFailed = false;
    TSharedPtr<FJsonObject> Resp = ApplyUpdates(S, Updates, bSave, bSaveFailed);
    if (bSaveFailed)
    {
        Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED,
            TEXT("Renderer settings were applied in memory, but DefaultEngine.ini was not written."), Resp);
        return true;
    }
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- rendering.set_lumen_method ----
// Typed convenience verb delegating to ApplyUpdates for the UPROPERTY-backed fields.
// finalGatherQuality has no UPROPERTY in URendererSettings (CVar-only, r.Lumen.DiffuseIndirect.MeshSDF)
// — we mirror it via CVar and flag it in the response. See wiki rendering.md.
REGISTER_RPC_HANDLER("rendering.set_lumen_method", "rendering",
    "Set the highest-traffic Lumen fields with persistence (UPROPERTY-backed)",
    RPC_PARAMS(
        RPC_PARAM_OPT("hardwareRT", "boolean", "bUseHardwareRayTracingForLumen UPROPERTY"),
        RPC_PARAM_OPT("finalGatherQuality", "number", "Lumen final gather quality (writes r.Lumen.DiffuseIndirect.Quality CVar — not persisted to ini)"),
        RPC_PARAM_OPT("reflectionMethod", "string", "Reflections UPROPERTY: None | Lumen | ScreenSpace"),
        RPC_PARAM_OPT("softwareRTMode", "string", "LumenSoftwareTracingMode UPROPERTY: Detail | Global"),
        RPC_PARAM_DEF("save", "boolean", "Persist to DefaultEngine.ini after applying", "true")
    ))
{
    URendererSettings* S = GetMutableDefault<URendererSettings>();
    if (!S)
    {
        Ctx.SendError(ErrorCodes::ERR_INTERNAL_ERROR, TEXT("Failed to resolve URendererSettings CDO"));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    TSharedPtr<FJsonObject> Updates = MakeShared<FJsonObject>();

    bool bHWRT = false;
    if (Payload->TryGetBoolField(TEXT("hardwareRT"), bHWRT))
    {
        Updates->SetBoolField(TEXT("bUseHardwareRayTracingForLumen"), bHWRT);
    }

    FString Refl;
    if (Payload->TryGetStringField(TEXT("reflectionMethod"), Refl))
    {
        // Reflections is TEnumAsByte<EReflectionMethod::Type> — ApplyJsonValueToProperty
        // accepts enum tokens by name. Map common aliases to engine enum tokens.
        FString Mapped = Refl;
        if (Refl == TEXT("RT") || Refl == TEXT("Lumen")) Mapped = TEXT("Lumen");
        else if (Refl == TEXT("SSR")) Mapped = TEXT("ScreenSpace");
        Updates->SetStringField(TEXT("Reflections"), Mapped);
    }

    FString SwRTMode;
    if (Payload->TryGetStringField(TEXT("softwareRTMode"), SwRTMode))
    {
        // LumenSoftwareTracingMode is TEnumAsByte<ELumenSoftwareTracingMode::Type>.
        // The documented API tokens "Detail"/"Global" are intentionally short and match
        // NEITHER the enumerator identifier (DetailTracing/GlobalTracing) NOR the UMETA
        // DisplayName ("Detail Tracing"/"Global Tracing") — so they must be aliased to the
        // engine enum identifiers here regardless of how the shared coercion in
        // ApplyJsonValueToProperty evolves. Mirrors the reflectionMethod alias block above.
        FString Mapped = SwRTMode;
        if (SwRTMode == TEXT("Detail")) Mapped = TEXT("DetailTracing");
        else if (SwRTMode == TEXT("Global")) Mapped = TEXT("GlobalTracing");
        Updates->SetStringField(TEXT("LumenSoftwareTracingMode"), Mapped);
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    bool bSaveFailed = false;
    TSharedPtr<FJsonObject> Resp = ApplyUpdates(S, Updates, bSave, bSaveFailed);

    // finalGatherQuality: CVar-only mirror, no UPROPERTY. Flag explicitly in the response
    // so callers know it didn't go through the persistence path.
    double FinalGather = 0.0;
    if (Payload->TryGetNumberField(TEXT("finalGatherQuality"), FinalGather))
    {
        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.DiffuseIndirect.Quality"));
        if (CVar) CVar->Set((float)FinalGather);
        Resp->SetBoolField(TEXT("finalGatherQualityCvarOnly"), true);
    }

    if (bSaveFailed)
    {
        Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED,
            TEXT("Lumen settings were applied in memory, but DefaultEngine.ini was not written."), Resp);
        return true;
    }
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- rendering.set_dynamic_gi_method ----
// Persistent sibling of lighting.setup_global_illumination. Always mirrors to CVars
// for immediate viewport effect; optionally also writes the UPROPERTY + .ini for
// editor-restart persistence.
REGISTER_RPC_HANDLER("rendering.set_dynamic_gi_method", "rendering",
    "Set GI method on URendererSettings (with optional persistence + live CVar mirror)",
    RPC_PARAMS(
        RPC_PARAM_REQ("method", "string", "GI method: LumenGI, ScreenSpace, None, RayTraced, Lightmass"),
        RPC_PARAM_DEF("persist", "boolean", "Write UPROPERTY + ini, not just CVars", "true")
    ))
{
    FString Method;
    if (!Ctx.RequireString(TEXT("method"), Method))
    {
        return true;
    }

    if (!ApplyDynamicGIMethodToCVars(Method))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_GI_METHOD,
            FString::Printf(TEXT("Invalid GI method: %s. Valid values: LumenGI, ScreenSpace, None, RayTraced, Lightmass"), *Method));
        return true;
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("method"), Method);

    const bool bPersist = Ctx.GetBool(TEXT("persist"), true);
    if (bPersist)
    {
        URendererSettings* S = GetMutableDefault<URendererSettings>();
        if (!S)
        {
            Ctx.SendError(ErrorCodes::ERR_INTERNAL_ERROR, TEXT("Failed to resolve URendererSettings CDO"));
            return true;
        }

        TSharedPtr<FJsonObject> Updates = MakeShared<FJsonObject>();
        // Mirror the CVar integer mapping into the UPROPERTY enum tokens.
        // EDynamicGlobalIlluminationMethod (UE 5.6): None=0, Lumen=1, ScreenSpace=2, Plugin=3.
        // EReflectionMethod: None=0, Lumen=1, ScreenSpace=2.
        // Note: "RayTraced" has no UPROPERTY enum token in UE 5.6 — the live CVar path still
        // accepts the legacy integer 3, so persistence falls back to Lumen + bUseHardwareRayTracingForLumen.
        if (Method == TEXT("LumenGI"))
        {
            Updates->SetStringField(TEXT("DynamicGlobalIllumination"), TEXT("Lumen"));
            Updates->SetStringField(TEXT("Reflections"), TEXT("Lumen"));
        }
        else if (Method == TEXT("ScreenSpace"))
        {
            Updates->SetStringField(TEXT("DynamicGlobalIllumination"), TEXT("ScreenSpace"));
        }
        else if (Method == TEXT("None") || Method == TEXT("Lightmass"))
        {
            // Lightmass uses static-only lighting → match the inline CVar behavior which
            // sets the dynamic GI method to None.
            Updates->SetStringField(TEXT("DynamicGlobalIllumination"), TEXT("None"));
        }
        else if (Method == TEXT("RayTraced"))
        {
            Updates->SetStringField(TEXT("DynamicGlobalIllumination"), TEXT("Lumen"));
            Updates->SetBoolField(TEXT("bUseHardwareRayTracingForLumen"), true);
        }

        bool bSaveFailed = false;
        Resp = ApplyUpdates(S, Updates, /*bSave=*/true, bSaveFailed);
        Resp->SetStringField(TEXT("method"), Method);
        FString SavedTo;
        if (Resp->TryGetStringField(TEXT("savedTo"), SavedTo))
        {
            Resp->SetStringField(TEXT("configFile"), SavedTo);
        }
        if (bSaveFailed)
        {
            Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED,
                TEXT("Dynamic GI settings were applied in memory, but DefaultEngine.ini was not written."), Resp);
            return true;
        }
    }
    else
    {
        Resp->SetBoolField(TEXT("saveRequested"), false);
        Resp->SetBoolField(TEXT("saved"), false);
        Resp->SetStringField(TEXT("saveState"), AssetSaveStateToWire(EAssetSaveState::NotRequested));
        Resp->SetStringField(TEXT("saveDetail"),
            TEXT("No config save was requested; only the live CVar mirror was changed."));
    }

    Ctx.SendSuccess(Resp);
    return true;
}
