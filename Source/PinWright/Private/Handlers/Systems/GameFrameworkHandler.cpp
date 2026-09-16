// Copyright (c) 2026 Alexander Penkin. MIT License.

// GameFrameworkHandler.cpp - Migrated from PinWright_GameFrameworkHandlers.cpp
// Game mode creation, class configuration, match flow, player management

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintPathLoad.h"
#include "Handlers/PackagePathCompose.h"
#include "Handlers/ParamSpec.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Misc/EngineVersionComparison.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameMode.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/GameState.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Engine/GameInstance.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/SpectatorPawn.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/Pawn.h"
#include "EdGraphSchema_K2.h"
#include "Kismet/GameplayStatics.h"

DEFINE_LOG_CATEGORY_STATIC(LogMcpGameFrameworkHandlers, Log, All);

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------


// Set blueprint variable default value via CDO reflection
static void SetBPVarDefaultValueGF(UBlueprint* Blueprint, FName VarName, const FString& DefaultValue)
{
    if (!Blueprint) return;
    BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);
    if (Blueprint->GeneratedClass)
    {
        if (UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject())
        {
            FProperty* Property = FindFProperty<FProperty>(Blueprint->GeneratedClass, VarName);
            if (Property)
            {
                void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CDO);
                Property->ImportText_Direct(*DefaultValue, ValuePtr, CDO, 0);
                Blueprint->MarkPackageDirty();
            }
        }
    }
}

namespace GameFrameworkHelpers
{
    // One shared body, in Handlers/Blueprint/BlueprintPathLoad.h. This file and
    // NetworkingHandler.cpp each carried a verbatim copy; the "//" refusal the merged body now
    // performs is the reason they must not: a guard duplicated across two files is a guard that
    // has to be remembered twice. The using-declaration keeps the name a member of this namespace,
    // so the eight call sites below - which reach it through `using namespace GameFrameworkHelpers`
    // - are unchanged.
    using PinWrightBlueprintPathLoad::LoadBlueprintFromPath;

    UBlueprint* CreateGameFrameworkBlueprint(const FString& Path, const FString& Name, UClass* ParentClass, FString& OutError)
    {
        if (!ParentClass) { OutError = TEXT("Invalid parent class"); return nullptr; }
        FString FullPath = Path;
        if (!IsValidMountPoint(FullPath))
        {
            if (FullPath.StartsWith(TEXT("/Content/")))
                FullPath = FullPath.Replace(TEXT("/Content/"), TEXT("/Game/"));
            else if (!FullPath.StartsWith(TEXT("/")))
                FullPath = TEXT("/Game/") + FullPath;
        }
        // COMPOSED THROUGH THE SHARED CHECKER, never with a bare operator/ or Printf.
        // CreatePackage (UObjectGlobals.cpp:1094-1096) logs at Fatal - not compiled out in any
        // configuration - on a name containing "//", which ends the editor PROCESS and every
        // unsaved package in it. FString::operator/ does not double a leading slash
        // (PathAppend, String.cpp.inl:855-885), but it copies a "//" inside `Name` through
        // untouched, so a caller name of "a//b" was a one-argument kill here. The normalization
        // directly above is a second source: it leaves an unmounted rooted path alone once
        // IsValidMountPoint has rejected it, so the composed result was never checked against
        // the engine's package rules at all. PinWrightComposeAssetPackagePath runs
        // FName::IsValidXName over the bare name and FPackageName::IsValidLongPackageName over
        // the composed path and surfaces both engine reasons verbatim. Board:
        // B-createpackage-unvalidated-paths-plugin-wide.
        //
        // NO TRAILING-SLASH TRIM IS NEEDED BEFORE THAT CALL. The checker now joins with
        // FString::operator/, whose PathAppend (String.cpp.inl:855-885) pops the left side's
        // terminator when FolderPath already ends in '/' instead of adding a second separator, so
        // "/Game/X/" + "Y" composes "/Game/X/Y" and is accepted. The trim that used to sit here
        // existed only to undo the old Printf("%s/%s") join and is gone with it.
        FString AssetPath;
        if (!PinWrightComposeAssetPackagePath(FullPath, Name, AssetPath, OutError)) { return nullptr; }
        UPackage* Package = CreatePackage(*AssetPath);
        if (!Package) { OutError = FString::Printf(TEXT("Failed to create package: %s"), *AssetPath); return nullptr; }
        UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
        Factory->ParentClass = ParentClass;
        UBlueprint* Blueprint = Cast<UBlueprint>(
            Factory->FactoryCreateNew(UBlueprint::StaticClass(), Package, FName(*Name),
                RF_Public | RF_Standalone, nullptr, GWarn));
        if (!Blueprint) { OutError = FString::Printf(TEXT("Failed to create %s blueprint"), *ParentClass->GetName()); return nullptr; }
        FAssetRegistryModule::AssetCreated(Blueprint);
        Blueprint->MarkPackageDirty();
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Blueprint);
        return Blueprint;
    }

    bool SetClassProperty(UBlueprint* Blueprint, const FName& PropertyName, UClass* ClassToSet, FString& OutError)
    {
        if (!Blueprint || !Blueprint->GeneratedClass) { OutError = TEXT("Invalid blueprint or generated class"); return false; }
        UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
        if (!CDO) { OutError = TEXT("Failed to get CDO"); return false; }
        FProperty* Prop = Blueprint->GeneratedClass->FindPropertyByName(PropertyName);
        if (!Prop) Prop = Blueprint->ParentClass->FindPropertyByName(PropertyName);
        if (!Prop) { OutError = FString::Printf(TEXT("Property '%s' not found"), *PropertyName.ToString()); return false; }
        FClassProperty* ClassProp = CastField<FClassProperty>(Prop);
        if (ClassProp) { ClassProp->SetPropertyValue_InContainer(CDO, ClassToSet); CDO->MarkPackageDirty(); return true; }
        FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop);
        if (SoftClassProp) { FSoftObjectPtr SoftPtr(ClassToSet); SoftClassProp->SetPropertyValue_InContainer(CDO, SoftPtr); CDO->MarkPackageDirty(); return true; }
        OutError = FString::Printf(TEXT("Property '%s' is not a class property"), *PropertyName.ToString());
        return false;
    }

    UClass* LoadClassFromPath(const FString& ClassPath)
    {
        // THIS IS A LIVE EDITOR KILL FROM A VERB WITH NO CreatePackage ANYWHERE IN IT, which is
        // why the guard sits at the top rather than at a create site. LoadClass<UObject> below
        // routes LoadClass -> StaticLoadClass -> StaticLoadObject -> StaticLoadObjectInternal ->
        // ResolveName2(..., Create=true) -> CreatePackage on the partial name, and CreatePackage
        // logs a "//" at Fatal - a verbosity not compiled out in any configuration, so it ends
        // the PROCESS and every unsaved package in it. `BPPath = ClassPath + "_C"` appends to the
        // caller's text and preserves any "//" it carried. Reachable today through
        // game_framework.configure_spectating (spectatorClass, read with a raw Ctx.GetString) and
        // through every CREATE_GF_BP_HANDLER-generated verb's parentClass.
        //
        // FindObject directly below is safe (Create=false) and would not have caught it anyway:
        // a malformed path simply misses it and falls through to the lethal LoadClass. Refusal is
        // the nullptr this function already returns for an unresolvable class, plus one Warning -
        // never an Error, which bElevateLogWarningsToErrors would turn into a test failure. Board
        // B-createpackage-unvalidated-paths-plugin-wide.
        if (ClassPath.IsEmpty()) return nullptr;
        if (CanReachCreatePackageFatal(ClassPath))
        {
            UE_LOG(LogMcpGameFrameworkHandlers, Warning,
                TEXT("LoadClassFromPath refused '%s': a class path may not contain '//'."),
                *ClassPath);
            return nullptr;
        }
        UClass* NativeClass = FindObject<UClass>(nullptr, *ClassPath);
        if (NativeClass) return NativeClass;
        FString BPPath = ClassPath;
        if (!BPPath.EndsWith(TEXT("_C"))) BPPath += TEXT("_C");
        UClass* BPClass = LoadClass<UObject>(nullptr, *BPPath);
        if (BPClass) return BPClass;
        UBlueprint* BP = LoadBlueprintFromPath(ClassPath);
        if (BP && BP->GeneratedClass) return BP->GeneratedClass;
        return nullptr;
    }

    bool AddBlueprintVariable(UBlueprint* Blueprint, const FString& VarName, const FEdGraphPinType& PinType, const FString& Category = TEXT(""))
    {
        if (!Blueprint) return false;
        bool bSuccess = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VarName), PinType);
        if (bSuccess && !Category.IsEmpty())
            FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, FName(*VarName), nullptr, FText::FromString(Category));
        return bSuccess;
    }

    void SetVariableDefaultValue(UBlueprint* Blueprint, const FString& VarName, const FString& DefaultValue)
    {
        if (!Blueprint) return;
        SetBPVarDefaultValueGF(Blueprint, FName(*VarName), DefaultValue);
    }

    FEdGraphPinType MakeIntPinType() { FEdGraphPinType P; P.PinCategory = UEdGraphSchema_K2::PC_Int; return P; }
    FEdGraphPinType MakeFloatPinType() { FEdGraphPinType P; P.PinCategory = UEdGraphSchema_K2::PC_Real; P.PinSubCategory = UEdGraphSchema_K2::PC_Float; return P; }
    FEdGraphPinType MakeBoolPinType() { FEdGraphPinType P; P.PinCategory = UEdGraphSchema_K2::PC_Boolean; return P; }
    FEdGraphPinType MakeNamePinType() { FEdGraphPinType P; P.PinCategory = UEdGraphSchema_K2::PC_Name; return P; }
    FEdGraphPinType MakeStringPinType() { FEdGraphPinType P; P.PinCategory = UEdGraphSchema_K2::PC_String; return P; }
    FEdGraphPinType MakeBytePinType() { FEdGraphPinType P; P.PinCategory = UEdGraphSchema_K2::PC_Byte; return P; }

    // Common parameter extraction pattern used by most handlers
    struct FCommonParams
    {
        FString Name;
        FString Path;
        bool bSave;
        FString GameModeBlueprint;
        FString BlueprintPath;

        static bool Extract(FHandlerContext& Ctx, FCommonParams& Out)
        {
            const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
            Out.Name = Ctx.GetString(TEXT("name"));
            Out.bSave = Ctx.GetBool(TEXT("save"), false);

            Out.GameModeBlueprint = Ctx.GetString(TEXT("gameModeBlueprint"));
            if (Out.GameModeBlueprint.IsEmpty())
                Out.GameModeBlueprint = Ctx.GetString(TEXT("blueprintPath"));
            if (!Out.GameModeBlueprint.IsEmpty())
            {
                FString SanitizedBP = SanitizeProjectRelativePath(Out.GameModeBlueprint);
                if (SanitizedBP.IsEmpty())
                {
                    Ctx.SendError(TEXT("SECURITY_VIOLATION"),
                        TEXT("Invalid gameModeBlueprint path: path traversal detected"));
                    return false;
                }
                Out.GameModeBlueprint = SanitizedBP;
            }
            Out.BlueprintPath = Out.GameModeBlueprint;
            return true;
        }

        // The `path` slot lives here rather than in Extract because
        // CreateGameFrameworkBlueprint(P.Path, ...) inside GF_CREATE_CLASS_HANDLER is its ONLY
        // consumer, and only that macro family declares RPC_PARAM_OPT("path", ...). The seven
        // game_framework.configure_* / set_respawn_rules verbs that also call Extract load an
        // existing GameMode by `gameModeBlueprint` and never look at FCommonParams::Path, so
        // reading it there handed every one of them a key FRpcDispatcher::ValidateHandlerParams
        // refuses with UNKNOWN_PARAMS before the body runs
        // (PinWright.infra.declared_params.HandlersOnlyReadDeclaredParams).
        static bool ExtractSavePath(FHandlerContext& Ctx, FCommonParams& Out)
        {
            Out.Path = Ctx.GetString(TEXT("path"), TEXT("/Game"));

            // Validate path
            FString SanitizedPath = SanitizeProjectRelativePath(Out.Path);
            if (SanitizedPath.IsEmpty() && !Out.Path.IsEmpty())
            {
                Ctx.SendError(TEXT("SECURITY_VIOLATION"),
                    TEXT("Invalid path: path traversal or invalid characters detected."));
                return false;
            }
            if (!SanitizedPath.IsEmpty()) Out.Path = SanitizedPath;
            return true;
        }
    };
}


// ---------------------------------------------------------------------------
// Macro for repetitive "create class blueprint" handlers
// ---------------------------------------------------------------------------

#define GF_CREATE_CLASS_HANDLER(MethodName, Category, Summary, DefaultParentClass, ClassLabel) \
REGISTER_RPC_HANDLER(MethodName, Category, Summary, \
    RPC_PARAMS( \
        RPC_PARAM_REQ("name", "string", "Blueprint name"), \
        RPC_PARAM_OPT("path", "path", "Save path (default /Game)"), \
        RPC_PARAM_OPT("parentClass", "classref", "Parent class path"), \
        RPC_PARAM_OPT("save", "boolean", "Save after creation") \
    )) \
{ \
    using namespace GameFrameworkHelpers; \
    FCommonParams P; \
    if (!FCommonParams::Extract(Ctx, P)) return true; \
    if (!FCommonParams::ExtractSavePath(Ctx, P)) return true; \
    if (P.Name.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'name'.")); return true; } \
    FString ParentClassPath = Ctx.GetString(TEXT("parentClass")); \
    UClass* ParentClass = ParentClassPath.IsEmpty() ? DefaultParentClass::StaticClass() : LoadClassFromPath(ParentClassPath); \
    if (!ParentClass) ParentClass = DefaultParentClass::StaticClass(); \
    FString Error; \
    UBlueprint* BP = CreateGameFrameworkBlueprint(P.Path, P.Name, ParentClass, Error); \
    if (!BP) { Ctx.SendError(TEXT("CREATION_FAILED"), Error); return true; } \
    if (P.bSave) McpSafeAssetSave(BP); \
    TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject()); \
    Response->SetBoolField(TEXT("success"), true); \
    Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Created " ClassLabel " blueprint: %s"), *P.Name)); \
    Response->SetStringField(TEXT("blueprintPath"), BP->GetPathName()); \
    AddAssetVerification(Response, BP); \
    Ctx.SendSuccess(Response); \
}
// ---- game_framework.configure_game_rules ----
REGISTER_RPC_HANDLER("game_framework.configure_game_rules", "game_framework",
    "Configure game rules on a GameMode blueprint (max players, friendly fire, etc.)",
    RPC_PARAMS(
        RPC_PARAM_REQ("gameModeBlueprint", "path", "GameMode blueprint path"),
        RPC_PARAM_OPT("maxPlayers", "number", "Maximum player count"),
        RPC_PARAM_OPT("bFriendlyFire", "boolean", "Enable friendly fire"),
        RPC_PARAM_OPT("bAllowRespawn", "boolean", "Allow respawning"),
        RPC_PARAM_OPT("respawnDelay", "number", "Respawn delay in seconds"),
        RPC_PARAM_OPT("bAutoAssignTeams", "boolean", "Auto-assign teams"),
        RPC_PARAM_OPT("save", "boolean", "Save after change")
    ))
{
    using namespace GameFrameworkHelpers;
    FCommonParams P;
    if (!FCommonParams::Extract(Ctx, P)) return true;
    if (P.GameModeBlueprint.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'gameModeBlueprint'.")); return true; }

    UBlueprint* BP = LoadBlueprintFromPath(P.GameModeBlueprint);
    if (!BP) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Failed to load GameMode: %s"), *P.GameModeBlueprint)); return true; }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    TArray<FString> ConfiguredRules;

    // Add blueprint variables for game rules
    if (Payload->HasField(TEXT("maxPlayers")))
    {
        AddBlueprintVariable(BP, TEXT("MaxPlayers"), MakeIntPinType(), TEXT("Game Rules"));
        SetVariableDefaultValue(BP, TEXT("MaxPlayers"), FString::FromInt(Ctx.GetInt(TEXT("maxPlayers"), 4)));
        ConfiguredRules.Add(TEXT("MaxPlayers"));
    }
    if (Payload->HasField(TEXT("bFriendlyFire")))
    {
        AddBlueprintVariable(BP, TEXT("bFriendlyFire"), MakeBoolPinType(), TEXT("Game Rules"));
        SetVariableDefaultValue(BP, TEXT("bFriendlyFire"), Ctx.GetBool(TEXT("bFriendlyFire")) ? TEXT("true") : TEXT("false"));
        ConfiguredRules.Add(TEXT("bFriendlyFire"));
    }
    if (Payload->HasField(TEXT("bAllowRespawn")))
    {
        AddBlueprintVariable(BP, TEXT("bAllowRespawn"), MakeBoolPinType(), TEXT("Game Rules"));
        SetVariableDefaultValue(BP, TEXT("bAllowRespawn"), Ctx.GetBool(TEXT("bAllowRespawn")) ? TEXT("true") : TEXT("false"));
        ConfiguredRules.Add(TEXT("bAllowRespawn"));
    }
    if (Payload->HasField(TEXT("respawnDelay")))
    {
        AddBlueprintVariable(BP, TEXT("RespawnDelay"), MakeFloatPinType(), TEXT("Game Rules"));
        SetVariableDefaultValue(BP, TEXT("RespawnDelay"), FString::SanitizeFloat(Ctx.GetNumber(TEXT("respawnDelay"), 5.0)));
        ConfiguredRules.Add(TEXT("RespawnDelay"));
    }
    if (Payload->HasField(TEXT("bAutoAssignTeams")))
    {
        AddBlueprintVariable(BP, TEXT("bAutoAssignTeams"), MakeBoolPinType(), TEXT("Game Rules"));
        SetVariableDefaultValue(BP, TEXT("bAutoAssignTeams"), Ctx.GetBool(TEXT("bAutoAssignTeams")) ? TEXT("true") : TEXT("false"));
        ConfiguredRules.Add(TEXT("bAutoAssignTeams"));
    }

    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    if (P.bSave) McpSafeAssetSave(BP);

    TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject());
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Configured %d game rules"), ConfiguredRules.Num()));
    Response->SetStringField(TEXT("blueprintPath"), BP->GetPathName());
    TArray<TSharedPtr<FJsonValue>> RulesArray;
    for (const FString& Rule : ConfiguredRules)
        RulesArray.Add(MakeShared<FJsonValueString>(Rule));
    Response->SetArrayField(TEXT("configuredRules"), RulesArray);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Response);
    AddAssetVerification(Response, BP);
    Ctx.SendSuccess(Response);
    return true;
}
// ---- game_framework.configure_round_system ----
REGISTER_RPC_HANDLER("game_framework.configure_round_system", "game_framework",
    "Configure round-based gameplay on a GameMode blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("gameModeBlueprint", "path", "GameMode blueprint path"),
        RPC_PARAM_OPT("maxRounds", "number", "Maximum rounds"),
        RPC_PARAM_OPT("roundDuration", "number", "Duration per round in seconds"),
        RPC_PARAM_OPT("bAutoStartRound", "boolean", "Auto-start rounds"),
        RPC_PARAM_OPT("roundTransitionDelay", "number", "Delay between rounds"),
        RPC_PARAM_OPT("warmupDuration", "number", "Warmup period before round"),
        RPC_PARAM_OPT("save", "boolean", "Save after change")
    ))
{
    using namespace GameFrameworkHelpers;
    FCommonParams P;
    if (!FCommonParams::Extract(Ctx, P)) return true;
    if (P.GameModeBlueprint.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'gameModeBlueprint'.")); return true; }

    UBlueprint* BP = LoadBlueprintFromPath(P.GameModeBlueprint);
    if (!BP) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Failed to load GameMode: %s"), *P.GameModeBlueprint)); return true; }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    TArray<FString> ConfiguredVars;

    AddBlueprintVariable(BP, TEXT("CurrentRound"), MakeIntPinType(), TEXT("Round System"));
    SetVariableDefaultValue(BP, TEXT("CurrentRound"), TEXT("0"));
    ConfiguredVars.Add(TEXT("CurrentRound"));

    if (Payload->HasField(TEXT("maxRounds")))
    {
        AddBlueprintVariable(BP, TEXT("MaxRounds"), MakeIntPinType(), TEXT("Round System"));
        SetVariableDefaultValue(BP, TEXT("MaxRounds"), FString::FromInt(Ctx.GetInt(TEXT("maxRounds"), 3)));
        ConfiguredVars.Add(TEXT("MaxRounds"));
    }
    if (Payload->HasField(TEXT("roundDuration")))
    {
        AddBlueprintVariable(BP, TEXT("RoundDuration"), MakeFloatPinType(), TEXT("Round System"));
        SetVariableDefaultValue(BP, TEXT("RoundDuration"), FString::SanitizeFloat(Ctx.GetNumber(TEXT("roundDuration"), 300.0)));
        ConfiguredVars.Add(TEXT("RoundDuration"));
    }
    if (Payload->HasField(TEXT("bAutoStartRound")))
    {
        AddBlueprintVariable(BP, TEXT("bAutoStartRound"), MakeBoolPinType(), TEXT("Round System"));
        SetVariableDefaultValue(BP, TEXT("bAutoStartRound"), Ctx.GetBool(TEXT("bAutoStartRound")) ? TEXT("true") : TEXT("false"));
        ConfiguredVars.Add(TEXT("bAutoStartRound"));
    }
    if (Payload->HasField(TEXT("roundTransitionDelay")))
    {
        AddBlueprintVariable(BP, TEXT("RoundTransitionDelay"), MakeFloatPinType(), TEXT("Round System"));
        SetVariableDefaultValue(BP, TEXT("RoundTransitionDelay"), FString::SanitizeFloat(Ctx.GetNumber(TEXT("roundTransitionDelay"), 5.0)));
        ConfiguredVars.Add(TEXT("RoundTransitionDelay"));
    }
    if (Payload->HasField(TEXT("warmupDuration")))
    {
        AddBlueprintVariable(BP, TEXT("WarmupDuration"), MakeFloatPinType(), TEXT("Round System"));
        SetVariableDefaultValue(BP, TEXT("WarmupDuration"), FString::SanitizeFloat(Ctx.GetNumber(TEXT("warmupDuration"), 10.0)));
        ConfiguredVars.Add(TEXT("WarmupDuration"));
    }

    AddBlueprintVariable(BP, TEXT("bRoundInProgress"), MakeBoolPinType(), TEXT("Round System"));
    AddBlueprintVariable(BP, TEXT("RoundTimeRemaining"), MakeFloatPinType(), TEXT("Round System"));

    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    if (P.bSave) McpSafeAssetSave(BP);

    TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject());
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Configured round system with %d settings"), ConfiguredVars.Num()));
    Response->SetStringField(TEXT("blueprintPath"), BP->GetPathName());
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Response);
    AddAssetVerification(Response, BP);
    Ctx.SendSuccess(Response);
    return true;
}

// ---- game_framework.configure_team_system ----
REGISTER_RPC_HANDLER("game_framework.configure_team_system", "game_framework",
    "Configure team system on a GameMode blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("gameModeBlueprint", "path", "GameMode blueprint path"),
        RPC_PARAM_OPT("maxTeams", "number", "Maximum team count"),
        RPC_PARAM_OPT("maxPlayersPerTeam", "number", "Max players per team"),
        RPC_PARAM_OPT("teamNames", "array", "Array of team name strings"),
        RPC_PARAM_OPT("bAutoBalance", "boolean", "Auto-balance teams"),
        RPC_PARAM_OPT("save", "boolean", "Save after change")
    ))
{
    using namespace GameFrameworkHelpers;
    FCommonParams P;
    if (!FCommonParams::Extract(Ctx, P)) return true;
    if (P.GameModeBlueprint.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'gameModeBlueprint'.")); return true; }

    UBlueprint* BP = LoadBlueprintFromPath(P.GameModeBlueprint);
    if (!BP) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Failed to load GameMode: %s"), *P.GameModeBlueprint)); return true; }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    AddBlueprintVariable(BP, TEXT("NumTeams"), MakeIntPinType(), TEXT("Team System"));
    int32 MaxTeams = Ctx.GetInt(TEXT("maxTeams"), 2);
    SetVariableDefaultValue(BP, TEXT("NumTeams"), FString::FromInt(MaxTeams));

    if (Payload->HasField(TEXT("maxPlayersPerTeam")))
    {
        AddBlueprintVariable(BP, TEXT("MaxPlayersPerTeam"), MakeIntPinType(), TEXT("Team System"));
        SetVariableDefaultValue(BP, TEXT("MaxPlayersPerTeam"), FString::FromInt(Ctx.GetInt(TEXT("maxPlayersPerTeam"), 5)));
    }
    if (Payload->HasField(TEXT("bAutoBalance")))
    {
        AddBlueprintVariable(BP, TEXT("bAutoBalance"), MakeBoolPinType(), TEXT("Team System"));
        SetVariableDefaultValue(BP, TEXT("bAutoBalance"), Ctx.GetBool(TEXT("bAutoBalance")) ? TEXT("true") : TEXT("false"));
    }

    // Team names
    const TArray<TSharedPtr<FJsonValue>>* TeamNamesArray = Ctx.GetArray(TEXT("teamNames"));
    TArray<FString> TeamNames;
    if (TeamNamesArray)
    {
        for (const TSharedPtr<FJsonValue>& V : *TeamNamesArray)
        {
            if (V.IsValid() && V->Type == EJson::String)
                TeamNames.Add(V->AsString());
        }
    }
    if (TeamNames.Num() == 0)
    {
        TeamNames = {TEXT("Team 1"), TEXT("Team 2")};
        while (TeamNames.Num() < MaxTeams) TeamNames.Add(FString::Printf(TEXT("Team %d"), TeamNames.Num() + 1));
    }

    // Store team names as individual variables
    for (int32 i = 0; i < TeamNames.Num(); ++i)
    {
        FString VarName = FString::Printf(TEXT("Team%d_Name"), i);
        AddBlueprintVariable(BP, VarName, MakeStringPinType(), TEXT("Team System"));
        SetVariableDefaultValue(BP, VarName, TeamNames[i]);
    }

    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    if (P.bSave) McpSafeAssetSave(BP);

    TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject());
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Configured %d teams"), MaxTeams));
    Response->SetStringField(TEXT("blueprintPath"), BP->GetPathName());
    TArray<TSharedPtr<FJsonValue>> TeamNamesJson;
    for (const FString& N : TeamNames) TeamNamesJson.Add(MakeShared<FJsonValueString>(N));
    Response->SetArrayField(TEXT("teamNames"), TeamNamesJson);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Response);
    AddAssetVerification(Response, BP);
    Ctx.SendSuccess(Response);
    return true;
}

// ---- game_framework.configure_scoring_system ----
REGISTER_RPC_HANDLER("game_framework.configure_scoring_system", "game_framework",
    "Configure scoring system on a GameMode blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("gameModeBlueprint", "path", "GameMode blueprint path"),
        RPC_PARAM_OPT("scoreToWin", "number", "Score needed to win"),
        RPC_PARAM_OPT("killScore", "number", "Score per kill"),
        RPC_PARAM_OPT("deathPenalty", "number", "Score penalty for death"),
        RPC_PARAM_OPT("objectiveScore", "number", "Score per objective"),
        RPC_PARAM_OPT("bTeamScoring", "boolean", "Use team-based scoring"),
        RPC_PARAM_OPT("save", "boolean", "Save after change")
    ))
{
    using namespace GameFrameworkHelpers;
    FCommonParams P;
    if (!FCommonParams::Extract(Ctx, P)) return true;
    if (P.GameModeBlueprint.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'gameModeBlueprint'.")); return true; }

    UBlueprint* BP = LoadBlueprintFromPath(P.GameModeBlueprint);
    if (!BP) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Failed to load GameMode: %s"), *P.GameModeBlueprint)); return true; }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    TArray<FString> ConfiguredVars;

    if (Payload->HasField(TEXT("scoreToWin")))
    {
        AddBlueprintVariable(BP, TEXT("ScoreToWin"), MakeIntPinType(), TEXT("Scoring"));
        SetVariableDefaultValue(BP, TEXT("ScoreToWin"), FString::FromInt(Ctx.GetInt(TEXT("scoreToWin"), 100)));
        ConfiguredVars.Add(TEXT("ScoreToWin"));
    }
    if (Payload->HasField(TEXT("killScore")))
    {
        AddBlueprintVariable(BP, TEXT("KillScore"), MakeIntPinType(), TEXT("Scoring"));
        SetVariableDefaultValue(BP, TEXT("KillScore"), FString::FromInt(Ctx.GetInt(TEXT("killScore"), 1)));
        ConfiguredVars.Add(TEXT("KillScore"));
    }
    if (Payload->HasField(TEXT("deathPenalty")))
    {
        AddBlueprintVariable(BP, TEXT("DeathPenalty"), MakeIntPinType(), TEXT("Scoring"));
        SetVariableDefaultValue(BP, TEXT("DeathPenalty"), FString::FromInt(Ctx.GetInt(TEXT("deathPenalty"), 0)));
        ConfiguredVars.Add(TEXT("DeathPenalty"));
    }
    if (Payload->HasField(TEXT("objectiveScore")))
    {
        AddBlueprintVariable(BP, TEXT("ObjectiveScore"), MakeIntPinType(), TEXT("Scoring"));
        SetVariableDefaultValue(BP, TEXT("ObjectiveScore"), FString::FromInt(Ctx.GetInt(TEXT("objectiveScore"), 10)));
        ConfiguredVars.Add(TEXT("ObjectiveScore"));
    }
    if (Payload->HasField(TEXT("bTeamScoring")))
    {
        AddBlueprintVariable(BP, TEXT("bTeamScoring"), MakeBoolPinType(), TEXT("Scoring"));
        SetVariableDefaultValue(BP, TEXT("bTeamScoring"), Ctx.GetBool(TEXT("bTeamScoring")) ? TEXT("true") : TEXT("false"));
        ConfiguredVars.Add(TEXT("bTeamScoring"));
    }

    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    if (P.bSave) McpSafeAssetSave(BP);

    TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject());
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Configured scoring system with %d settings"), ConfiguredVars.Num()));
    Response->SetStringField(TEXT("blueprintPath"), BP->GetPathName());
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Response);
    AddAssetVerification(Response, BP);
    Ctx.SendSuccess(Response);
    return true;
}

// ---- game_framework.configure_spawn_system ----
REGISTER_RPC_HANDLER("game_framework.configure_spawn_system", "game_framework",
    "Configure spawn system on a GameMode blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("gameModeBlueprint", "path", "GameMode blueprint path"),
        RPC_PARAM_OPT("spawnMethod", "string", "Spawn method (Random, RoundRobin, NearTeam, FarthestFromEnemy)"),
        RPC_PARAM_OPT("bUseSpawnAreas", "boolean", "Use spawn areas"),
        RPC_PARAM_OPT("minSpawnDistance", "number", "Min distance from enemies"),
        RPC_PARAM_OPT("spawnProtectionDuration", "number", "Invulnerability duration after spawn"),
        RPC_PARAM_OPT("bRandomizeSpawn", "boolean", "Randomize spawn points"),
        RPC_PARAM_OPT("save", "boolean", "Save after change")
    ))
{
    using namespace GameFrameworkHelpers;
    FCommonParams P;
    if (!FCommonParams::Extract(Ctx, P)) return true;
    if (P.GameModeBlueprint.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'gameModeBlueprint'.")); return true; }

    UBlueprint* BP = LoadBlueprintFromPath(P.GameModeBlueprint);
    if (!BP) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Failed to load GameMode: %s"), *P.GameModeBlueprint)); return true; }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    TArray<FString> ConfiguredVars;

    if (Payload->HasField(TEXT("spawnMethod")))
    {
        AddBlueprintVariable(BP, TEXT("SpawnMethod"), MakeNamePinType(), TEXT("Spawn System"));
        SetVariableDefaultValue(BP, TEXT("SpawnMethod"), Ctx.GetString(TEXT("spawnMethod"), TEXT("Random")));
        ConfiguredVars.Add(TEXT("SpawnMethod"));
    }
    if (Payload->HasField(TEXT("bUseSpawnAreas")))
    {
        AddBlueprintVariable(BP, TEXT("bUseSpawnAreas"), MakeBoolPinType(), TEXT("Spawn System"));
        SetVariableDefaultValue(BP, TEXT("bUseSpawnAreas"), Ctx.GetBool(TEXT("bUseSpawnAreas")) ? TEXT("true") : TEXT("false"));
        ConfiguredVars.Add(TEXT("bUseSpawnAreas"));
    }
    if (Payload->HasField(TEXT("minSpawnDistance")))
    {
        AddBlueprintVariable(BP, TEXT("MinSpawnDistance"), MakeFloatPinType(), TEXT("Spawn System"));
        SetVariableDefaultValue(BP, TEXT("MinSpawnDistance"), FString::SanitizeFloat(Ctx.GetNumber(TEXT("minSpawnDistance"), 1000.0)));
        ConfiguredVars.Add(TEXT("MinSpawnDistance"));
    }
    if (Payload->HasField(TEXT("spawnProtectionDuration")))
    {
        AddBlueprintVariable(BP, TEXT("SpawnProtectionDuration"), MakeFloatPinType(), TEXT("Spawn System"));
        SetVariableDefaultValue(BP, TEXT("SpawnProtectionDuration"), FString::SanitizeFloat(Ctx.GetNumber(TEXT("spawnProtectionDuration"), 3.0)));
        ConfiguredVars.Add(TEXT("SpawnProtectionDuration"));
    }
    if (Payload->HasField(TEXT("bRandomizeSpawn")))
    {
        AddBlueprintVariable(BP, TEXT("bRandomizeSpawn"), MakeBoolPinType(), TEXT("Spawn System"));
        SetVariableDefaultValue(BP, TEXT("bRandomizeSpawn"), Ctx.GetBool(TEXT("bRandomizeSpawn"), true) ? TEXT("true") : TEXT("false"));
        ConfiguredVars.Add(TEXT("bRandomizeSpawn"));
    }

    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    if (P.bSave) McpSafeAssetSave(BP);

    TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject());
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Configured spawn system with %d settings"), ConfiguredVars.Num()));
    Response->SetStringField(TEXT("blueprintPath"), BP->GetPathName());
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Response);
    AddAssetVerification(Response, BP);
    Ctx.SendSuccess(Response);
    return true;
}

// ---- game_framework.configure_player_start ----
REGISTER_RPC_HANDLER("game_framework.configure_player_start", "game_framework",
    "Configure player start points in the level",
    RPC_PARAMS(
        RPC_PARAM_OPT("spawnPoints", "array", "Array of {name, location, rotation, teamIndex} objects"),
        RPC_PARAM_OPT("bClearExisting", "boolean", "Clear existing player starts"),
        RPC_PARAM_OPT("save", "boolean", "Save after change")
    ))
{
    using namespace GameFrameworkHelpers;
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available")); return true; }

    bool bClearExisting = Ctx.GetBool(TEXT("bClearExisting"), false);
    if (bClearExisting)
    {
        TArray<AActor*> ExistingStarts;
        for (TActorIterator<APlayerStart> It(World); It; ++It)
            ExistingStarts.Add(*It);
        for (AActor* Actor : ExistingStarts)
            Actor->Destroy();
    }

    const TArray<TSharedPtr<FJsonValue>>* SpawnPoints = Ctx.GetArray(TEXT("spawnPoints"));
    TArray<TSharedPtr<FJsonValue>> CreatedPoints;

    if (SpawnPoints)
    {
        for (const TSharedPtr<FJsonValue>& PointValue : *SpawnPoints)
        {
            if (!PointValue.IsValid() || PointValue->Type != EJson::Object) continue;
            TSharedPtr<FJsonObject> PointObj = PointValue->AsObject();
            if (!PointObj.IsValid()) continue;

            FString PointName = GetJsonStringField(PointObj, TEXT("name"), TEXT("PlayerStart"));
            FVector Location = ExtractVectorField(PointObj, TEXT("location"), FVector::ZeroVector);
            FRotator Rotation = ExtractRotatorField(PointObj, TEXT("rotation"), FRotator::ZeroRotator);

            FActorSpawnParameters SpawnParams;
            SpawnParams.Name = MakeUniqueObjectName(World, APlayerStart::StaticClass(), FName(*PointName));
            SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

            APlayerStart* NewStart = World->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), Location, Rotation, SpawnParams);
            if (NewStart)
            {
                NewStart->SetActorLabel(*PointName);
                int32 TeamIndex = static_cast<int32>(GetJsonNumberField(PointObj, TEXT("teamIndex"), -1));
                if (TeamIndex >= 0)
                {
                    // Store team index as tag
                    NewStart->PlayerStartTag = FName(*FString::Printf(TEXT("Team%d"), TeamIndex));
                }

                TSharedPtr<FJsonObject> PointResult = MakeShareable(new FJsonObject());
                PointResult->SetStringField(TEXT("name"), PointName);
                AddActorVerification(PointResult, NewStart);
                CreatedPoints.Add(MakeShared<FJsonValueObject>(PointResult));
            }
        }
    }

    TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject());
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Configured %d player start points"), CreatedPoints.Num()));
    Response->SetArrayField(TEXT("playerStarts"), CreatedPoints);
    Ctx.SendSuccess(Response);
    return true;
}

// ---- game_framework.set_respawn_rules ----
REGISTER_RPC_HANDLER("game_framework.set_respawn_rules", "game_framework",
    "Configure respawn rules on a GameMode blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("gameModeBlueprint", "path", "GameMode blueprint path"),
        RPC_PARAM_OPT("respawnDelay", "number", "Delay before respawn"),
        RPC_PARAM_OPT("maxLives", "number", "Maximum lives (-1 for unlimited)"),
        RPC_PARAM_OPT("bForceRespawn", "boolean", "Force respawn"),
        RPC_PARAM_OPT("respawnInvulnerability", "number", "Invulnerability seconds after respawn"),
        RPC_PARAM_OPT("save", "boolean", "Save after change")
    ))
{
    using namespace GameFrameworkHelpers;
    FCommonParams P;
    if (!FCommonParams::Extract(Ctx, P)) return true;
    if (P.GameModeBlueprint.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'gameModeBlueprint'.")); return true; }

    UBlueprint* BP = LoadBlueprintFromPath(P.GameModeBlueprint);
    if (!BP) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Failed to load GameMode: %s"), *P.GameModeBlueprint)); return true; }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    TArray<FString> ConfiguredVars;

    if (Payload->HasField(TEXT("respawnDelay")))
    {
        AddBlueprintVariable(BP, TEXT("RespawnDelay"), MakeFloatPinType(), TEXT("Respawn"));
        SetVariableDefaultValue(BP, TEXT("RespawnDelay"), FString::SanitizeFloat(Ctx.GetNumber(TEXT("respawnDelay"), 5.0)));
        ConfiguredVars.Add(TEXT("RespawnDelay"));
    }
    if (Payload->HasField(TEXT("maxLives")))
    {
        AddBlueprintVariable(BP, TEXT("MaxLives"), MakeIntPinType(), TEXT("Respawn"));
        SetVariableDefaultValue(BP, TEXT("MaxLives"), FString::FromInt(Ctx.GetInt(TEXT("maxLives"), -1)));
        ConfiguredVars.Add(TEXT("MaxLives"));
    }
    if (Payload->HasField(TEXT("bForceRespawn")))
    {
        AddBlueprintVariable(BP, TEXT("bForceRespawn"), MakeBoolPinType(), TEXT("Respawn"));
        SetVariableDefaultValue(BP, TEXT("bForceRespawn"), Ctx.GetBool(TEXT("bForceRespawn")) ? TEXT("true") : TEXT("false"));
        ConfiguredVars.Add(TEXT("bForceRespawn"));
    }
    if (Payload->HasField(TEXT("respawnInvulnerability")))
    {
        AddBlueprintVariable(BP, TEXT("RespawnInvulnerability"), MakeFloatPinType(), TEXT("Respawn"));
        SetVariableDefaultValue(BP, TEXT("RespawnInvulnerability"), FString::SanitizeFloat(Ctx.GetNumber(TEXT("respawnInvulnerability"), 3.0)));
        ConfiguredVars.Add(TEXT("RespawnInvulnerability"));
    }

    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    if (P.bSave) McpSafeAssetSave(BP);

    TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject());
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("message"), FString::Printf(TEXT("Configured %d respawn rules"), ConfiguredVars.Num()));
    Response->SetStringField(TEXT("blueprintPath"), BP->GetPathName());
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Response);
    AddAssetVerification(Response, BP);
    Ctx.SendSuccess(Response);
    return true;
}

// ---- game_framework.configure_spectating ----
REGISTER_RPC_HANDLER("game_framework.configure_spectating", "game_framework",
    "Configure spectating on a GameMode blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("gameModeBlueprint", "path", "GameMode blueprint path"),
        RPC_PARAM_OPT("bAllowSpectating", "boolean", "Allow spectating"),
        RPC_PARAM_OPT("spectatorClass", "classref", "Spectator pawn class"),
        RPC_PARAM_OPT("bOnlyDeadCanSpectate", "boolean", "Only dead players can spectate"),
        RPC_PARAM_OPT("save", "boolean", "Save after change")
    ))
{
    using namespace GameFrameworkHelpers;
    FCommonParams P;
    if (!FCommonParams::Extract(Ctx, P)) return true;
    if (P.GameModeBlueprint.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'gameModeBlueprint'.")); return true; }

    UBlueprint* BP = LoadBlueprintFromPath(P.GameModeBlueprint);
    if (!BP) { Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Failed to load GameMode: %s"), *P.GameModeBlueprint)); return true; }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    if (Payload->HasField(TEXT("bAllowSpectating")))
    {
        AddBlueprintVariable(BP, TEXT("bAllowSpectating"), MakeBoolPinType(), TEXT("Spectating"));
        SetVariableDefaultValue(BP, TEXT("bAllowSpectating"), Ctx.GetBool(TEXT("bAllowSpectating"), true) ? TEXT("true") : TEXT("false"));
    }
    if (Payload->HasField(TEXT("bOnlyDeadCanSpectate")))
    {
        AddBlueprintVariable(BP, TEXT("bOnlyDeadCanSpectate"), MakeBoolPinType(), TEXT("Spectating"));
        SetVariableDefaultValue(BP, TEXT("bOnlyDeadCanSpectate"), Ctx.GetBool(TEXT("bOnlyDeadCanSpectate")) ? TEXT("true") : TEXT("false"));
    }

    FString SpectatorClassPath = Ctx.GetString(TEXT("spectatorClass"));
    if (!SpectatorClassPath.IsEmpty())
    {
        UClass* SpecClass = LoadClassFromPath(SpectatorClassPath);
        if (SpecClass)
        {
            FString Error;
            SetClassProperty(BP, TEXT("SpectatorClass"), SpecClass, Error);
        }
    }

    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);
    if (P.bSave) McpSafeAssetSave(BP);

    TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject());
    Response->SetBoolField(TEXT("success"), true);
    Response->SetStringField(TEXT("message"), TEXT("Configured spectating settings"));
    Response->SetStringField(TEXT("blueprintPath"), BP->GetPathName());
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Response);
    AddAssetVerification(Response, BP);
    Ctx.SendSuccess(Response);
    return true;
}

// ---- game_framework.get_game_framework_info ----
REGISTER_RPC_HANDLER("game_framework.get_game_framework_info", "game_framework",
    "Get information about the current game framework setup",
    RPC_NO_PARAMS)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World) { Ctx.SendError(TEXT("NO_EDITOR_WORLD"), TEXT("No editor world available")); return true; }

    TSharedPtr<FJsonObject> InfoJson = MakeShareable(new FJsonObject());

    // Current Game Mode
    AWorldSettings* WorldSettings = World->GetWorldSettings();
    if (WorldSettings)
    {
        TSubclassOf<AGameModeBase> GameModeClass = WorldSettings->DefaultGameMode;
        if (GameModeClass)
        {
            InfoJson->SetStringField(TEXT("gameMode"), GameModeClass->GetPathName());
        }
        else
        {
            InfoJson->SetStringField(TEXT("gameMode"), TEXT("(default)"));
        }
    }

    // Player Starts
    TArray<TSharedPtr<FJsonValue>> PlayerStarts;
    for (TActorIterator<APlayerStart> It(World); It; ++It)
    {
        TSharedPtr<FJsonObject> StartObj = MakeShareable(new FJsonObject());
        StartObj->SetStringField(TEXT("name"), It->GetActorLabel());
        StartObj->SetStringField(TEXT("tag"), It->PlayerStartTag.ToString());
        FVector Loc = It->GetActorLocation();
        TSharedPtr<FJsonObject> LocObj = MakeShareable(new FJsonObject());
        LocObj->SetNumberField(TEXT("x"), Loc.X);
        LocObj->SetNumberField(TEXT("y"), Loc.Y);
        LocObj->SetNumberField(TEXT("z"), Loc.Z);
        StartObj->SetObjectField(TEXT("location"), LocObj);
        PlayerStarts.Add(MakeShared<FJsonValueObject>(StartObj));
    }
    InfoJson->SetArrayField(TEXT("playerStarts"), PlayerStarts);
    InfoJson->SetNumberField(TEXT("playerStartCount"), PlayerStarts.Num());

    TSharedPtr<FJsonObject> Response = MakeShareable(new FJsonObject());
    Response->SetObjectField(TEXT("gameFrameworkInfo"), InfoJson);
    Ctx.SendSuccess(Response);
    return true;
}
