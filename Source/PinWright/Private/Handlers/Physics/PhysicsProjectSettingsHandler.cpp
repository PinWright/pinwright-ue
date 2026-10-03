// Copyright (c) 2026 Alexander Penkin. MIT License.

// PhysicsProjectSettingsHandler.cpp — typed read/write for UPhysicsSettings (Project Settings ->
// Engine -> Physics), persisted to DefaultEngine.ini [/Script/Engine.PhysicsSettings]. Sibling of
// rendering.* (RenderingProjectSettingsHandler.cpp). Board: F-physics-project-settings.
//
// The save is measured by RELOADING each written property from the config hierarchy: the live value
// is cleared, LoadConfig re-imports it from GConfig (which TryUpdateDefaultConfigFile just reloaded
// from disk), and the result must equal what the caller asked for. A key the write did not land
// therefore reads back as the cleared value and fails the check instead of passing as unchanged.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamSpec.h"
#include "Utils/AssetSaveState.h"
#include "Utils/PropertyUtils.h"
#include "Compat/JsonKeyCompat.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace PwPhysicsSettings
{
    // Valid EPhysicalSurface entries a project may name. 0 is SurfaceType_Default (never renamed);
    // SurfaceType_Max (63) is the sentinel.
    constexpr int32 MinSurfaceIndex = 1;
    constexpr int32 MaxSurfaceIndex = SurfaceType_Max - 1;

    bool IsConfigProperty(const FProperty* Prop)
    {
        return Prop && Prop->HasAnyPropertyFlags(static_cast<EPropertyFlags>(CPF_Config | CPF_GlobalConfig));
    }

    FString ConfigSection(const UObject* S)
    {
        return S->GetClass()->GetPathName();
    }

    FProperty* SurfacesProperty()
    {
        return FindFProperty<FProperty>(UPhysicsSettings::StaticClass(),
            GET_MEMBER_NAME_CHECKED(UPhysicsSettings, PhysicalSurfaces));
    }

    // Clear the live value, reload it from the config hierarchy, compare with what the caller set,
    // and always leave the caller's value in memory.
    bool ReloadFromConfigMatches(UObject* S, FProperty* Prop)
    {
        void* Live = Prop->ContainerPtrToValuePtr<void>(S);
        void* Expected = Prop->AllocateAndInitializeValue();
        Prop->CopyCompleteValue(Expected, Live);
        Prop->ClearValue(Live);
        S->LoadConfig(nullptr, nullptr, 0, Prop);
        const bool bMatch = Prop->Identical(Expected, Live);
        Prop->CopyCompleteValue(Live, Expected);
        Prop->DestroyAndFreeValue(Expected);
        return bMatch;
    }

    // Shared persistence + response fields for every physics.* writer. Always reports which file
    // and section the write targets; savedTo appears only when every written property reloaded
    // from config with the requested value.
    void SaveAndReport(UPhysicsSettings* S, const TArray<FProperty*>& Written, bool bSave,
        const TSharedPtr<FJsonObject>& Resp, bool& bOutSaveFailed)
    {
        bOutSaveFailed = false;
        const FString ConfigFile = S->GetDefaultConfigFilename();
        Resp->SetStringField(TEXT("configFile"), ConfigFile);
        Resp->SetStringField(TEXT("configSection"), ConfigSection(S));

        const bool bSaveRequested = bSave && Written.Num() > 0;
        Resp->SetBoolField(TEXT("saveRequested"), bSaveRequested);
        if (!bSaveRequested)
        {
            Resp->SetBoolField(TEXT("saved"), false);
            Resp->SetStringField(TEXT("saveState"), AssetSaveStateToWire(EAssetSaveState::NotRequested));
            Resp->SetStringField(TEXT("saveDetail"), bSave
                ? TEXT("Nothing was applied, so nothing was written.")
                : TEXT("No config save was requested; applied settings are in memory only and are lost on editor restart."));
            return;
        }

        const bool bWriterSucceeded = S->TryUpdateDefaultConfigFile(ConfigFile);
        TArray<TSharedPtr<FJsonValue>> Verified;
        TArray<TSharedPtr<FJsonValue>> Mismatched;
        if (bWriterSucceeded)
        {
            for (FProperty* Prop : Written)
            {
                (ReloadFromConfigMatches(S, Prop) ? Verified : Mismatched)
                    .Add(MakeShared<FJsonValueString>(Prop->GetName()));
            }
        }
        const bool bSaved = bWriterSucceeded && Mismatched.Num() == 0;
        bOutSaveFailed = !bSaved;

        Resp->SetBoolField(TEXT("saved"), bSaved);
        Resp->SetStringField(TEXT("saveState"),
            AssetSaveStateToWire(bSaved ? EAssetSaveState::Written : EAssetSaveState::Failed));
        Resp->SetArrayField(TEXT("reloadVerified"), Verified);
        Resp->SetArrayField(TEXT("reloadMismatch"), Mismatched);
        const FString FileName = FPaths::GetCleanFilename(ConfigFile);
        if (bSaved)
        {
            Resp->SetStringField(TEXT("savedTo"), ConfigFile);
            Resp->SetStringField(TEXT("saveDetail"), FString::Printf(
                TEXT("Rewrote section [%s] of %s (the Project Settings path: every config property of the class that differs from the base layer is written) and reloaded each written property from config with the requested value."),
                *ConfigSection(S), *FileName));
        }
        else if (!bWriterSucceeded)
        {
            Resp->SetStringField(TEXT("saveDetail"), FString::Printf(
                TEXT("The engine config writer refused %s (read-only file); nothing was written. Make the file writable (check it out of source control) and retry."),
                *FileName));
        }
        else
        {
            Resp->SetStringField(TEXT("saveDetail"), FString::Printf(
                TEXT("%s was rewritten but the properties in reloadMismatch did not reload from config with the requested value; a later layer (Saved/ or platform ini) may override them."),
                *FileName));
        }
    }

    TSharedPtr<FJsonValue> SurfaceToJson(const FPhysicalSurfaceName& Entry)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetNumberField(TEXT("index"), static_cast<int32>(Entry.Type.GetValue()));
        O->SetStringField(TEXT("name"), Entry.Name.ToString());
        return MakeShared<FJsonValueObject>(O);
    }

    TArray<TSharedPtr<FJsonValue>> SurfacesToJson(const TArray<FPhysicalSurfaceName>& Table)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const FPhysicalSurfaceName& Entry : Table)
        {
            Out.Add(SurfaceToJson(Entry));
        }
        return Out;
    }

    // UPhysicalMaterial assets whose SurfaceType is one of Indices. Loads every registered physical
    // material first so unloaded assets count; in-memory (unsaved) assets are found by the iterator.
    TMap<int32, TArray<FString>> FindPhysicalMaterialsUsing(const TSet<int32>& Indices)
    {
        TMap<int32, TArray<FString>> Users;
        IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TArray<FAssetData> Assets;
        Registry.GetAssetsByClass(UPhysicalMaterial::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
        for (const FAssetData& Asset : Assets)
        {
            Asset.GetAsset();
        }
        for (TObjectIterator<UPhysicalMaterial> It; It; ++It)
        {
            const int32 Index = static_cast<int32>(It->SurfaceType.GetValue());
            if (It->IsAsset() && Indices.Contains(Index))
            {
                Users.FindOrAdd(Index).Add(It->GetPathName());
            }
        }
        return Users;
    }
}

// ---- physics.get_project_settings ----
REGISTER_RPC_HANDLER("physics.get_project_settings", "physics",
    "Read UPhysicsSettings config UPROPERTYs (DefaultEngine.ini -> [/Script/Engine.PhysicsSettings]), including the live PhysicalSurfaces table",
    RPC_PARAMS(
        RPC_PARAM_OPT("filter", "string", "Case-insensitive substring filter on property name")
    ))
{
    UPhysicsSettings* S = GetMutableDefault<UPhysicsSettings>();
    const FString Filter = Ctx.GetString(TEXT("filter"));

    TSharedPtr<FJsonObject> Settings = MakeShared<FJsonObject>();
    for (TFieldIterator<FProperty> It(S->GetClass()); It; ++It)
    {
        FProperty* Prop = *It;
        if (!PwPhysicsSettings::IsConfigProperty(Prop) || (!Filter.IsEmpty() && !Prop->GetName().Contains(Filter)))
        {
            continue;
        }
        if (TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(S, Prop))
        {
            Settings->SetField(Prop->GetName(), Value);
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetObjectField(TEXT("settings"), Settings);
    Resp->SetArrayField(TEXT("surfaceTypes"), PwPhysicsSettings::SurfacesToJson(S->PhysicalSurfaces));
    Resp->SetStringField(TEXT("configFile"), S->GetDefaultConfigFilename());
    Resp->SetStringField(TEXT("configSection"), PwPhysicsSettings::ConfigSection(S));
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- physics.set_project_settings ----
REGISTER_RPC_HANDLER("physics.set_project_settings", "physics",
    "Write UPhysicsSettings config UPROPERTYs (with PostEditChange) and optionally persist to DefaultEngine.ini with a reload check",
    RPC_PARAMS(
        RPC_PARAM_REQ("updates", "object", "Map of UPROPERTY name -> JSON value. PhysicalSurfaces is refused here; use physics.set_surface_types."),
        RPC_PARAM_DEF("save", "boolean", "Persist to DefaultEngine.ini after applying", "true")
    ))
{
    UPhysicsSettings* S = GetMutableDefault<UPhysicsSettings>();
    TSharedPtr<FJsonObject> Updates;
    if (!Ctx.RequireObject(TEXT("updates"), Updates))
    {
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Applied;
    TArray<TSharedPtr<FJsonValue>> Rejected;
    TArray<FProperty*> Written;
    auto Reject = [&Rejected](const FString& Name, const FString& Reason)
    {
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("name"), Name);
        R->SetStringField(TEXT("reason"), Reason);
        Rejected.Add(MakeShared<FJsonValueObject>(R));
    };

    for (const auto& Pair : Updates->Values)
    {
        const FString Name = EARGCompat::JsonKeyToString(Pair.Key);
        FProperty* Prop = FindPropertyCI(S->GetClass(), Name);
        if (!Prop)
        {
            Reject(Name, TEXT("unknown_property"));
            continue;
        }
        if (!PwPhysicsSettings::IsConfigProperty(Prop))
        {
            Reject(Name, TEXT("not_config_serializable"));
            continue;
        }
        if (Prop == PwPhysicsSettings::SurfacesProperty())
        {
            // The surface table drives EPhysicalSurface's display metadata and is referenced by
            // index from UPhysicalMaterial assets; only the typed verb validates and refreshes it.
            Reject(Name, TEXT("use_physics.set_surface_types"));
            continue;
        }
        FString ApplyError;
        if (!ApplyJsonValueToProperty(S, Prop, Pair.Value, ApplyError))
        {
            Reject(Name, ApplyError);
            continue;
        }
        // UPhysicsSettings::PostEditChangeProperty is where the setting takes effect (CVar export,
        // physical-material rebuild, Chaos settings refresh). UDeveloperSettings does not chain to
        // UObject's OnObjectPropertyChanged broadcast, so send it here for open Project Settings tabs.
        FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
        S->PostEditChangeProperty(Event);
        FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(S, Event);
        Applied.Add(MakeShared<FJsonValueString>(Prop->GetName()));
        Written.Add(Prop);
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("applied"), Applied);
    Resp->SetArrayField(TEXT("rejected"), Rejected);
    bool bSaveFailed = false;
    PwPhysicsSettings::SaveAndReport(S, Written, Ctx.GetBool(TEXT("save"), true), Resp, bSaveFailed);
    if (bSaveFailed)
    {
        Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED,
            TEXT("Physics settings were applied in memory, but DefaultEngine.ini was not durably written."), Resp);
        return true;
    }
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- physics.set_surface_types ----
REGISTER_RPC_HANDLER("physics.set_surface_types", "physics",
    "Register EPhysicalSurface names (UPhysicsSettings::PhysicalSurfaces): validated, enum metadata refreshed, persisted to DefaultEngine.ini",
    RPC_PARAMS(
        RPC_PARAM_REQ_NESTED("surfaces", "array",
            "Entries [{index, name}]: index 1..62 (SurfaceType1..SurfaceType62), name non-empty and unique in the resulting table. Closed slot: any other element key is refused.",
            TEXT("index"), TEXT("name")),
        RPC_PARAM_DEF("replace", "boolean", "true: the table becomes exactly `surfaces` (indices not listed are removed; removing one a UPhysicalMaterial asset uses is refused). false: upsert by index.", "false"),
        RPC_PARAM_DEF("save", "boolean", "Persist to DefaultEngine.ini after applying", "true")
    ))
{
    UPhysicsSettings* S = GetMutableDefault<UPhysicsSettings>();
    const TArray<TSharedPtr<FJsonValue>>* Surfaces = nullptr;
    if (!Ctx.RequireArray(TEXT("surfaces"), Surfaces))
    {
        return true;
    }
    const bool bReplace = Ctx.GetBool(TEXT("replace"), false);

    // Parse and validate the request before touching anything.
    TMap<int32, FName> Requested;
    for (int32 i = 0; i < Surfaces->Num(); ++i)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        double IndexNumber = 0.0;
        FString Name;
        if (!(*Surfaces)[i]->TryGetObject(Entry) || !(*Entry)->TryGetNumberField(TEXT("index"), IndexNumber)
            || !(*Entry)->TryGetStringField(TEXT("name"), Name))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                TEXT("surfaces[%d] must be an object {index: int, name: string}."), i));
            return true;
        }
        if (FMath::Frac(IndexNumber) != 0.0 || IndexNumber < PwPhysicsSettings::MinSurfaceIndex || IndexNumber > PwPhysicsSettings::MaxSurfaceIndex)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                TEXT("surfaces[%d].index %g is not an integer in %d..%d (SurfaceType1..SurfaceType%d); index 0 is SurfaceType_Default and cannot be renamed."),
                i, IndexNumber, PwPhysicsSettings::MinSurfaceIndex, PwPhysicsSettings::MaxSurfaceIndex, PwPhysicsSettings::MaxSurfaceIndex));
            return true;
        }
        Name.TrimStartAndEndInline();
        if (Name.IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                TEXT("surfaces[%d].name is empty. To remove an index, call with replace:true and leave it out."), i));
            return true;
        }
        const int32 Index = static_cast<int32>(IndexNumber);
        if (Requested.Contains(Index))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                TEXT("surfaces lists index %d more than once."), Index));
            return true;
        }
        Requested.Add(Index, FName(*Name));
    }

    // Resulting table. Upsert keeps every entry of an unlisted index verbatim: a hand-edited ini can
    // list several entries for one index, and dropping those would rewrite the project's
    // DefaultEngine.ini on save. A listed index's entries become the one requested entry,
    // at the position of its first old entry.
    const TArray<FPhysicalSurfaceName> Previous = S->PhysicalSurfaces;
    TArray<FPhysicalSurfaceName> Result;
    TSet<int32> Placed;
    if (!bReplace)
    {
        for (const FPhysicalSurfaceName& Entry : Previous)
        {
            const int32 Index = static_cast<int32>(Entry.Type.GetValue());
            const FName* Name = Requested.Find(Index);
            if (!Name)
            {
                Result.Add(Entry);
            }
            else if (!Placed.Contains(Index))
            {
                Result.Add(FPhysicalSurfaceName(Entry.Type, *Name));
                Placed.Add(Index);
            }
        }
    }
    TArray<int32> NewIndices;
    Requested.GetKeys(NewIndices);
    NewIndices.Sort();
    for (int32 Index : NewIndices)
    {
        if (!Placed.Contains(Index))
        {
            Result.Add(FPhysicalSurfaceName(static_cast<EPhysicalSurface>(Index), Requested[Index]));
        }
    }

    // The name each index actually gets: the last entry wins, as in UPhysicsSettings::LoadSurfaceType.
    TMap<int32, FName> Table;
    for (const FPhysicalSurfaceName& Entry : Result)
    {
        Table.Add(static_cast<int32>(Entry.Type.GetValue()), Entry.Name);
    }

    // Only the requested names are checked, against every other index's effective name; a duplicate
    // the host table already carries is not the caller's to fix. FName equality is case-insensitive,
    // matching the Project Settings editor's duplicate check.
    for (int32 Index : NewIndices)
    {
        const FName Name = Requested[Index];
        for (const TPair<int32, FName>& Pair : Table)
        {
            if (Pair.Key != Index && Pair.Value == Name)
            {
                TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
                Data->SetStringField(TEXT("name"), Name.ToString());
                TArray<TSharedPtr<FJsonValue>> Indices;
                Indices.Add(MakeShared<FJsonValueNumber>(FMath::Min(Index, Pair.Key)));
                Indices.Add(MakeShared<FJsonValueNumber>(FMath::Max(Index, Pair.Key)));
                Data->SetArrayField(TEXT("indices"), Indices);
                Ctx.SendError(ErrorCodes::ERR_DUPLICATE_NAME, FString::Printf(
                    TEXT("Surface name '%s' would be used by both index %d and index %d; names must be unique."),
                    *Name.ToString(), FMath::Min(Index, Pair.Key), FMath::Max(Index, Pair.Key)), Data);
                return true;
            }
        }
    }

    TSet<int32> Removed;
    for (const FPhysicalSurfaceName& Entry : Previous)
    {
        const int32 Index = static_cast<int32>(Entry.Type.GetValue());
        if (!Table.Contains(Index))
        {
            Removed.Add(Index);
        }
    }
    if (Removed.Num() > 0)
    {
        const TMap<int32, TArray<FString>> Users = PwPhysicsSettings::FindPhysicalMaterialsUsing(Removed);
        if (Users.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> InUse;
            for (const TPair<int32, TArray<FString>>& Pair : Users)
            {
                TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                Row->SetNumberField(TEXT("index"), Pair.Key);
                TArray<TSharedPtr<FJsonValue>> Paths;
                for (const FString& Path : Pair.Value)
                {
                    Paths.Add(MakeShared<FJsonValueString>(Path));
                }
                Row->SetArrayField(TEXT("physicalMaterials"), Paths);
                InUse.Add(MakeShared<FJsonValueObject>(Row));
            }
            TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetArrayField(TEXT("inUse"), InUse);
            Ctx.SendError(ErrorCodes::ERR_SURFACE_TYPE_IN_USE,
                TEXT("replace:true would remove surface indices that UPhysicalMaterial assets use; nothing was changed. Keep those indices in `surfaces`, or reassign the listed materials' SurfaceType first."),
                Data);
            return true;
        }
    }

    // Apply, then refresh EPhysicalSurface metadata the way the Project Settings editor does.
    S->PhysicalSurfaces = Result;
    FProperty* Prop = PwPhysicsSettings::SurfacesProperty();
    FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
    S->PostEditChangeProperty(Event);
    FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(S, Event);

    UEnum* Enum = StaticEnum<EPhysicalSurface>();
    bool bEnumRefreshed = true;
    // Hides every entry without Hidden metadata (so removed indices become Unused again), then
    // un-hides and names the entries in the table.
    S->LoadSurfaceType();
    for (const TPair<int32, FName>& Pair : Table)
    {
        bEnumRefreshed &= !Enum->HasMetaData(TEXT("Hidden"), Pair.Key)
            && Enum->GetMetaData(TEXT("DisplayName"), Pair.Key) == Pair.Value.ToString();
    }
    for (int32 Index : Removed)
    {
        bEnumRefreshed &= Enum->HasMetaData(TEXT("Hidden"), Index);
    }

    TArray<TSharedPtr<FJsonValue>> RemovedJson;
    for (int32 Index : Removed)
    {
        RemovedJson.Add(MakeShared<FJsonValueNumber>(Index));
    }
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("registered"), PwPhysicsSettings::SurfacesToJson(S->PhysicalSurfaces));
    Resp->SetArrayField(TEXT("previous"), PwPhysicsSettings::SurfacesToJson(Previous));
    Resp->SetArrayField(TEXT("removed"), RemovedJson);
    Resp->SetBoolField(TEXT("enumRefreshed"), bEnumRefreshed);

    bool bSaveFailed = false;
    PwPhysicsSettings::SaveAndReport(S, { Prop }, Ctx.GetBool(TEXT("save"), true), Resp, bSaveFailed);
    if (bSaveFailed)
    {
        Ctx.SendError(ErrorCodes::ERR_SAVE_FAILED,
            TEXT("The surface table was applied in memory, but DefaultEngine.ini was not durably written."), Resp);
        return true;
    }
    Ctx.SendSuccess(Resp);
    return true;
}
