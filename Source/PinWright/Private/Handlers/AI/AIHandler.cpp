// Copyright (c) 2026 Alexander Penkin. MIT License.

// AIHandler.cpp
// Migrated from PinWright_AIHandlers.cpp (Phase 13)
// Covers: AI Controllers, Blackboards, Behavior Trees (basic), EQS, Perception,
//         State Trees, Smart Objects, Mass AI, AI info, configuration, and aliases.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/PackagePathCompose.h"
#include "Handlers/AI/EQSHandler.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"
#include "PinWrightGlobals.h"
#include "Compat/EngineVersionCompat.h"
#include "Utils/AssetUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectHash.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "AIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISenseConfig_Damage.h"
#include "Perception/AISense_Sight.h"
#include "Perception/AISense_Hearing.h"
#include "Perception/AISense_Damage.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "NavModifierComponent.h"
#include "NavAreas/NavArea.h"
#include "NavAreas/NavArea_Default.h"
#include "NavAreas/NavArea_Null.h"
#include "NavAreas/NavArea_Obstacle.h"
#include "GameplayTagContainer.h"
#include "Interfaces/IPluginManager.h"

// Attempt to include State Tree (UE 5.3+)
#if __has_include("StateTree.h")
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeCompiler.h"
#include "StateTreeCompilerLog.h"
// UE 5.7+ moved StateTreeComponentSchema to GameplayStateTreeModule
#if __has_include("Components/StateTreeComponentSchema.h")
#include "Components/StateTreeComponentSchema.h"
#define MCP_STATE_TREE_COMPONENT_SCHEMA_AVAILABLE 1
#else
#define MCP_STATE_TREE_COMPONENT_SCHEMA_AVAILABLE 0
#endif
#define MCP_STATE_TREE_HEADERS_AVAILABLE 1
#else
#define MCP_STATE_TREE_HEADERS_AVAILABLE 0
#define MCP_STATE_TREE_COMPONENT_SCHEMA_AVAILABLE 0
#endif

// Smart Objects and Mass AI are accessed reflection-only (no headers, no module
// linkage) — see the Smart Objects section below for the pattern.

// Log category for AI handlers
DEFINE_LOG_CATEGORY_STATIC(LogMcpAIHandlers, Log, All);

// =====================================================================
// Local Helpers
// =====================================================================

// First FObjectProperty on OwnerClass whose value type is (a subclass of) ValueClass.
// AI controllers expose at most one BehaviorTree / Blackboard CDO property, so the
// assign/stop handlers all locate it by this single-match reflection walk.
static FObjectProperty* FindFirstObjectPropertyOfClass(const UClass* OwnerClass, const UClass* ValueClass)
{
    if (!OwnerClass || !ValueClass) return nullptr;
    for (TFieldIterator<FObjectProperty> PropIt(OwnerClass); PropIt; ++PropIt)
    {
        FObjectProperty* ObjProp = *PropIt;
        if (ObjProp && ObjProp->PropertyClass && ObjProp->PropertyClass->IsChildOf(ValueClass))
        {
            return ObjProp;
        }
    }
    return nullptr;
}

// Assign Value as the default for the first ValueClass-typed FObjectProperty on a
// controller blueprint's CDO, creating a fallback member variable if none exists.
// Shared by ai.assign_behavior_tree (UBehaviorTree/"DefaultBehaviorTree") and
// ai.assign_blackboard (UBlackboardData/"DefaultBlackboard"), which differ only in
// the value type, the fallback variable name, and their result messages.
// Returns whether the CDO was written; OutPropertyName receives the property name
// that was targeted (matching the prior per-handler reporting: the discovered
// property's name, or the fallback variable name once AddMemberVariable succeeds).
static bool AssignObjectDefaultToControllerCDO(UBlueprint* Controller, UClass* ValueClass,
    UObject* Value, FName FallbackVarName, FString& OutPropertyName,
    BlueprintHandlerUtils::FBlueprintCompileDiagnostics& OutCompileDiagnostics,
    bool& bOutCompileAttempted)
{
    if (!Controller || !Controller->GeneratedClass) return false;
    AAIController* CDO = Cast<AAIController>(Controller->GeneratedClass->GetDefaultObject());
    if (!CDO) return false;

    // Try to find an existing ValueClass* property on the CDO.
    if (FObjectProperty* ObjProp = FindFirstObjectPropertyOfClass(Controller->GeneratedClass, ValueClass))
    {
        ObjProp->SetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CDO), Value);
        OutPropertyName = ObjProp->GetName();
        return true;
    }

    // No existing property found — add a Blueprint variable for the reference.
    FEdGraphPinType PinType;
    PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
    PinType.PinSubCategoryObject = ValueClass;

    bool bPropertySet = false;
    if (FBlueprintEditorUtils::AddMemberVariable(Controller, FallbackVarName, PinType))
    {
        // AddMemberVariable only appends to NewVariables; the FProperty does not
        // exist on GeneratedClass until the blueprint is recompiled. Compile so the
        // property (and a fresh CDO) materialize, then set the value on the new CDO.
        OutCompileDiagnostics = BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(Controller);
        bOutCompileAttempted = true;
        if (AAIController* NewCDO = Controller->GeneratedClass
                ? Cast<AAIController>(Controller->GeneratedClass->GetDefaultObject())
                : nullptr)
        {
            FProperty* NewProp = Controller->GeneratedClass->FindPropertyByName(FallbackVarName);
            if (FObjectProperty* ObjProp = CastField<FObjectProperty>(NewProp))
            {
                ObjProp->SetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(NewCDO), Value);
                bPropertySet = true;
            }
        }
    }
    OutPropertyName = FallbackVarName.ToString();
    return bPropertySet;
}

// Helper to MARK a package for a later save. It does not write anything.
// Note: This helper is used for NEW assets created with CreatePackage + factory.
// FullyLoad() must NOT be called on new packages - it corrupts bulkdata in UE 5.7+.
// void, not bool: its whole body was `return McpSafeAssetSave(Asset)`, which was the
// literal true for any non-null asset. All three callers (:259, :524, :582) already
// discarded the value. Callers that need to report persistence must measure it with
// IsAssetPersistedToDisk / AddMarkDirtySaveReport.
static void SavePackageHelperAI(UPackage* Package, UObject* Asset)
{
    if (!Package || !Asset) return;

    // Use centralized helper for safe saving (UE 5.7+ compatible)
    McpSafeAssetSave(Asset);
}

/**
 * Sanitize and validate an asset path for AI asset creation.
 * - Removes double slashes that cause Fatal Error in UObjectGlobals.cpp
 * - Validates path is within a valid mount point (/Game/, /Plugin/, etc.)
 * - Returns false and sets OutError if path is invalid (security check)
 */
static bool SanitizeAIAssetPath(const FString& InputPath, FString& OutSanitizedPath, FString& OutError)
{
    // Start with the input path
    OutSanitizedPath = InputPath;

    // 1. Remove duplicate slashes (prevents Fatal Error in UObjectGlobals.cpp)
    OutSanitizedPath.ReplaceInline(TEXT("//"), TEXT("/"));
    while (OutSanitizedPath.Contains(TEXT("//")))
    {
        OutSanitizedPath.ReplaceInline(TEXT("//"), TEXT("/"));
    }

    // 2. Trim leading/trailing whitespace
    OutSanitizedPath.TrimStartAndEndInline();

    // 3. Validate that path starts with a valid mount point
    // Valid mount points: /Game/, /Engine/, /PluginName/, etc.
    if (!OutSanitizedPath.StartsWith(TEXT("/")))
    {
        OutError = FString::Printf(TEXT("Invalid path: must start with '/' (got: %s)"), *InputPath);
        return false;
    }

    // 4. Check for path traversal attempts (security)
    if (OutSanitizedPath.Contains(TEXT("..")) ||
        OutSanitizedPath.Contains(TEXT("~")) ||
        OutSanitizedPath.Contains(TEXT("\\")))
    {
        OutError = FString::Printf(TEXT("Invalid path: contains forbidden characters (path traversal attempt): %s"), *InputPath);
        return false;
    }

    // 5. Validate path starts with a registered mount point
    if (!IsValidMountPoint(OutSanitizedPath))
    {
        OutError = FString::Printf(TEXT("Invalid path: not a registered mount point (got: %s)"), *InputPath);
        return false;
    }

    return true;
}

// Compose "<folder>/<name>" for an ai.* asset-creating verb, or refuse the pair.
//
// CreatePackage (UObjectGlobals.cpp:1086-1120) logs at Fatal - a verbosity that is not compiled
// out in any configuration, so it ends the PROCESS - for a name containing "//" (:1094-1096) and
// for one that resolves to empty (:1118). The ai.* create verbs below handed it `Path / Name`
// taken straight off Ctx.GetString, so `name: "a//b"` was a one-argument editor kill from the
// wire; the `if (!Package)` check after each call could never fire, because nothing after
// CreatePackage is reached. Board: B-createpackage-unvalidated-paths-plugin-wide, measured on
// B-foliage-add-type-name-with-slash-kills-the-editor.
//
// The engine's own rules are the check (Handlers/PackagePathCompose.h: FName::IsValidXName on the
// bare name, FPackageName::IsValidLongPackageName on the composed path) and both reason texts are
// surfaced verbatim, so a refused caller is told which rule it broke. INVALID_ARGUMENT rather
// than CREATION_FAILED: it is the caller's argument that is wrong, not the engine.
//
// Distinctively named and file-scope static because Unity merges these translation units.
static bool PinWrightAiComposeCreatePackagePath(FHandlerContext& Ctx, const FString& FolderPath,
    const FString& AssetName, FString& OutPackagePath)
{
    // FString::operator/ pops a trailing separator off the left side, so these verbs used to
    // accept a folder spelled with one. The shared composer's Printf would turn it into exactly
    // the "//" it exists to reject, so strip it instead of refusing a spelling that worked.
    FString Folder = FolderPath;
    Folder.RemoveFromEnd(TEXT("/"));

    FString ComposeError;
    if (!PinWrightComposeAssetPackagePath(Folder, AssetName, OutPackagePath, ComposeError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("%s Pass a bare asset name and choose the folder with 'path'."),
                *ComposeError));
        return false;
    }
    return true;
}

// Helper to create Blackboard asset.
//
// FullPath is composed AND validated by PinWrightAiComposeCreatePackagePath at the call site, and
// is the exact string handed to CreatePackage below. It must not be recomposed or re-sanitised
// here: re-deriving it from the caller's raw arguments is the "validated one string, passed a
// different one" shape the board ticket calls out, and it is what let a sanitised folder carry an
// unchecked path-shaped `name` into the Fatal. The engine package-name check the composer runs is
// strictly stronger than the SanitizeAIAssetPath call this replaced - it rejects the same
// traversal, backslash, '~' and unmounted-root inputs, and additionally refuses "//" rather than
// silently repairing it.
static UBlackboardData* CreateBlackboardAsset(const FString& FullPath, const FString& Name, FString& OutError)
{
    // Check if asset already exists
    if (FindObject<UBlackboardData>(nullptr, *FullPath) != nullptr)
    {
        OutError = FString::Printf(TEXT("Asset already exists: %s"), *FullPath);
        return nullptr;
    }

    // Also check if the package exists
    if (FPackageName::DoesPackageExist(FullPath))
    {
        OutError = FString::Printf(TEXT("Package already exists: %s"), *FullPath);
        return nullptr;
    }

    UPackage* Package = CreatePackage(*FullPath);
    if (!Package)
    {
        OutError = FString::Printf(TEXT("Failed to create package: %s"), *FullPath);
        return nullptr;
    }

    UBlackboardData* Blackboard = NewObject<UBlackboardData>(Package, UBlackboardData::StaticClass(), FName(*Name), RF_Public | RF_Standalone);
    if (!Blackboard)
    {
        OutError = TEXT("Failed to create Blackboard asset");
        return nullptr;
    }

    FAssetRegistryModule::AssetCreated(Blackboard);
    SavePackageHelperAI(Package, Blackboard);

    return Blackboard;
}

// =====================================================================
// 16.1 AI Controller (3 actions)
// =====================================================================

REGISTER_RPC_HANDLER("ai.assign_behavior_tree", "ai",
    "Assign a Behavior Tree to an AI Controller blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("controllerPath", "path", "Path to the AI Controller blueprint"),
        RPC_PARAM_REQ("behaviorTreePath", "path", "Path to the Behavior Tree asset")
    ))
{
    FString ControllerPath = Ctx.GetString(TEXT("controllerPath"));
    FString BehaviorTreePath = Ctx.GetString(TEXT("behaviorTreePath"));

    UBlueprint* Controller = LoadObject<UBlueprint>(nullptr, *ControllerPath);
    if (!Controller)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("AI Controller not found: %s"), *ControllerPath));
        return true;
    }

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *BehaviorTreePath);
    if (!BT)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Behavior Tree not found: %s"), *BehaviorTreePath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics;
    bool bCompileAttempted = false;

    // Set default BehaviorTree property on the generated class CDO using reflection
    if (Controller->GeneratedClass && Cast<AAIController>(Controller->GeneratedClass->GetDefaultObject()))
    {
        FString PropertyName;
        const bool bPropertySet = AssignObjectDefaultToControllerCDO(
            Controller, UBehaviorTree::StaticClass(), BT, TEXT("DefaultBehaviorTree"), PropertyName,
            CompileDiagnostics, bCompileAttempted);

        Result->SetStringField(TEXT("propertyName"), PropertyName);
        Result->SetBoolField(TEXT("propertyAssigned"), bPropertySet);
        Result->SetStringField(TEXT("message"), bPropertySet
            ? TEXT("Behavior Tree property assigned on CDO")
            : TEXT("Behavior Tree reference registered (call RunBehaviorTree in BeginPlay)"));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Controller);
    McpSafeAssetSave(Controller);
    Result->SetStringField(TEXT("controllerPath"), ControllerPath);
    Result->SetStringField(TEXT("behaviorTreePath"), BehaviorTreePath);
    if (bCompileAttempted)
    {
        BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Result);
    }
    AddAssetVerification(Result, Controller);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("ai.assign_blackboard", "ai",
    "Assign a Blackboard asset to an AI Controller blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("controllerPath", "path", "Path to the AI Controller blueprint"),
        RPC_PARAM_REQ("blackboardPath", "path", "Path to the Blackboard asset")
    ))
{
    FString ControllerPath = Ctx.GetString(TEXT("controllerPath"));
    FString BlackboardPath = Ctx.GetString(TEXT("blackboardPath"));

    UBlueprint* Controller = LoadObject<UBlueprint>(nullptr, *ControllerPath);
    if (!Controller)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("AI Controller not found: %s"), *ControllerPath));
        return true;
    }

    UBlackboardData* BB = LoadObject<UBlackboardData>(nullptr, *BlackboardPath);
    if (!BB)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Blackboard not found: %s"), *BlackboardPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics;
    bool bCompileAttempted = false;

    // Set default Blackboard property on the generated class CDO using reflection
    if (Controller->GeneratedClass && Cast<AAIController>(Controller->GeneratedClass->GetDefaultObject()))
    {
        FString PropertyName;
        const bool bPropertySet = AssignObjectDefaultToControllerCDO(
            Controller, UBlackboardData::StaticClass(), BB, TEXT("DefaultBlackboard"), PropertyName,
            CompileDiagnostics, bCompileAttempted);

        Result->SetStringField(TEXT("propertyName"), PropertyName);
        Result->SetBoolField(TEXT("propertyAssigned"), bPropertySet);
        Result->SetStringField(TEXT("message"), bPropertySet
            ? TEXT("Blackboard property assigned on CDO (call UseBlackboard in BeginPlay with this asset)")
            : TEXT("Blackboard reference registered (call UseBlackboard in BeginPlay with this asset)"));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Controller);
    // Mark-dirty only (Blueprint: the immediate write is the bulkdata-corruption
    // vector). saved used to be McpSafeAssetSave's constant true; measure instead, and
    // emit pendingFlush so the caller knows the CDO edit needs a flush to survive.
    McpSafeAssetSave(Controller);
    AddMarkDirtySaveReport(Result, Controller, /*bSaveRequested=*/true);
    Result->SetStringField(TEXT("controllerPath"), ControllerPath);
    Result->SetStringField(TEXT("blackboardPath"), BlackboardPath);
    if (bCompileAttempted)
    {
        BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, Result);
    }
    AddAssetVerification(Result, Controller);
    Ctx.SendSuccess(Result);
    return true;
}

// =====================================================================
// 16.2 Blackboard (3 actions)
// =====================================================================

REGISTER_RPC_HANDLER("ai.create_blackboard_asset", "ai",
    "Create a new Blackboard data asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the new Blackboard"),
        RPC_PARAM_DEF("path", "path", "Content path for the asset", "/Game/AI/Blackboards")
    ))
{
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game/AI/Blackboards"));

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Missing name parameter"));
        return true;
    }

    // COMPOSED AND CHECKED HERE, ABOVE CreateBlackboardAsset, and the ordering is load-bearing
    // twice over. (1) `name` used to be concatenated raw onto the folder INSIDE that helper, after
    // the folder had passed SanitizeAIAssetPath - so a guard that looked like it was working
    // carried a path-shaped name straight into CreatePackage's Fatal. (2) The regression test
    // (Tests/Gameplay/TestAiCreateAssetNamePathSafety.cpp) pairs every bad name with an UNMOUNTED
    // folder: on a build without this check the call is refused by SanitizeAIAssetPath - ABOVE the
    // concatenation - so the test goes red on the wrong error code instead of killing the test
    // host. Do not move this below CreateBlackboardAsset, and do not recompose FullPath there.
    FString FullPath;
    if (!PinWrightAiComposeCreatePackagePath(Ctx, Path, Name, FullPath))
    {
        return true;
    }

    FString Error;
    UBlackboardData* Blackboard = CreateBlackboardAsset(FullPath, Name, Error);
    if (!Blackboard)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), Error);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("blackboardPath"), Blackboard->GetPathName());
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Created Blackboard: %s"), *Name));
    AddAssetVerification(Result, Blackboard);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("ai.add_blackboard_key", "ai",
    "Add a key to a Blackboard asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("blackboardPath", "path", "Path to the Blackboard asset"),
        RPC_PARAM_REQ("keyName", "string", "Name for the new key"),
        RPC_PARAM_REQ("keyType", "string", "Type of the key (Bool, Int, Float, Vector, Rotator, Object, Class, Enum, Name, String)"),
        RPC_PARAM_OPT("baseObjectClass", "classref", "Base class for Object keys"),
        RPC_PARAM_OPT("isInstanceSynced", "boolean", "Whether the key is instance synced")
    ))
{
    FString BlackboardPath = Ctx.GetString(TEXT("blackboardPath"));
    FString KeyName = Ctx.GetString(TEXT("keyName"));
    FString KeyType = Ctx.GetString(TEXT("keyType"));

    UBlackboardData* Blackboard = LoadObject<UBlackboardData>(nullptr, *BlackboardPath);
    if (!Blackboard)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Blackboard not found: %s"), *BlackboardPath));
        return true;
    }

    // Resolve the optional base class up front so an unresolvable value fails
    // cleanly BEFORE the asset is mutated, rather than silently falling back to
    // UObject. The param accepts a bare class name ("Actor") or a full path
    // ("/Script/Engine.Pawn"); ResolveClassByName handles both and returns null
    // on empty input (caller did not constrain the base class). Resolution is
    // unconditional on a non-empty value — including for an unrecognized keyType
    // that defaults to an Object key below — so a real baseObjectClass is never
    // silently dropped, and a non-empty-but-unresolvable value is always rejected.
    const FString BaseClassInput = Ctx.GetString(TEXT("baseObjectClass"), TEXT(""));
    UClass* ResolvedBaseClass = nullptr;
    if (!BaseClassInput.IsEmpty())
    {
        ResolvedBaseClass = ResolveClassByName(BaseClassInput);
        if (!ResolvedBaseClass)
        {
            Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
                FString::Printf(TEXT("Could not resolve baseObjectClass '%s' to a UClass"), *BaseClassInput));
            return true;
        }
    }

    // Create appropriate key type
    FBlackboardEntry NewEntry;
    NewEntry.EntryName = FName(*KeyName);

    // Object and Class keys both expose an identical TObjectPtr<UClass> BaseClass
    // field. Capture whichever one is created so the resolved base class is
    // applied once below, rather than duplicating the assign per branch.
    TObjectPtr<UClass>* OutBaseClassField = nullptr;

    if (KeyType.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
    {
        NewEntry.KeyType = NewObject<UBlackboardKeyType_Bool>(Blackboard);
    }
    else if (KeyType.Equals(TEXT("Int"), ESearchCase::IgnoreCase))
    {
        NewEntry.KeyType = NewObject<UBlackboardKeyType_Int>(Blackboard);
    }
    else if (KeyType.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
    {
        NewEntry.KeyType = NewObject<UBlackboardKeyType_Float>(Blackboard);
    }
    else if (KeyType.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
    {
        NewEntry.KeyType = NewObject<UBlackboardKeyType_Vector>(Blackboard);
    }
    else if (KeyType.Equals(TEXT("Rotator"), ESearchCase::IgnoreCase))
    {
        NewEntry.KeyType = NewObject<UBlackboardKeyType_Rotator>(Blackboard);
    }
    else if (KeyType.Equals(TEXT("Object"), ESearchCase::IgnoreCase))
    {
        UBlackboardKeyType_Object* ObjectKey = NewObject<UBlackboardKeyType_Object>(Blackboard);
        OutBaseClassField = &ObjectKey->BaseClass;
        NewEntry.KeyType = ObjectKey;
    }
    else if (KeyType.Equals(TEXT("Class"), ESearchCase::IgnoreCase))
    {
        UBlackboardKeyType_Class* ClassKey = NewObject<UBlackboardKeyType_Class>(Blackboard);
        OutBaseClassField = &ClassKey->BaseClass;
        NewEntry.KeyType = ClassKey;
    }
    else if (KeyType.Equals(TEXT("Enum"), ESearchCase::IgnoreCase))
    {
        NewEntry.KeyType = NewObject<UBlackboardKeyType_Enum>(Blackboard);
    }
    else if (KeyType.Equals(TEXT("Name"), ESearchCase::IgnoreCase))
    {
        NewEntry.KeyType = NewObject<UBlackboardKeyType_Name>(Blackboard);
    }
    else if (KeyType.Equals(TEXT("String"), ESearchCase::IgnoreCase))
    {
        NewEntry.KeyType = NewObject<UBlackboardKeyType_String>(Blackboard);
    }
    else
    {
        // Default to Object
        UBlackboardKeyType_Object* ObjectKey = NewObject<UBlackboardKeyType_Object>(Blackboard);
        OutBaseClassField = &ObjectKey->BaseClass;
        NewEntry.KeyType = ObjectKey;
    }

    // Apply the resolved base class once, on whichever Object/Class key was
    // created above (including the default-to-Object path). Non-Object/Class key
    // types leave OutBaseClassField null; a baseObjectClass was already required
    // to resolve, so it is preserved here rather than silently dropped.
    if (ResolvedBaseClass && OutBaseClassField)
    {
        *OutBaseClassField = ResolvedBaseClass;
    }

    NewEntry.bInstanceSynced = Ctx.GetBool(TEXT("isInstanceSynced"), false);

    Blackboard->Keys.Add(NewEntry);
    Blackboard->MarkPackageDirty();
    SavePackageHelperAI(Blackboard->GetOutermost(), Blackboard);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetNumberField(TEXT("keyIndex"), Blackboard->Keys.Num() - 1);
    Result->SetStringField(TEXT("keyName"), KeyName);
    Result->SetStringField(TEXT("keyType"), KeyType);
    // Echo the resolved base class so a caller can confirm baseObjectClass stuck
    // (Object/Class keys only — gated on OutBaseClassField so it reflects what was
    // actually applied; absent when no base class was supplied or the key type has
    // no BaseClass field).
    if (ResolvedBaseClass && OutBaseClassField)
    {
        Result->SetStringField(TEXT("baseObjectClass"), ResolvedBaseClass->GetPathName());
    }
    AddAssetVerification(Result, Blackboard);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("ai.set_key_instance_synced", "ai",
    "Set instance sync flag on a Blackboard key",
    RPC_PARAMS(
        RPC_PARAM_REQ("blackboardPath", "path", "Path to the Blackboard asset"),
        RPC_PARAM_REQ("keyName", "string", "Name of the key"),
        RPC_PARAM_DEF("isInstanceSynced", "boolean", "Whether the key is instance synced", "true")
    ))
{
    FString BlackboardPath = Ctx.GetString(TEXT("blackboardPath"));
    FString KeyName = Ctx.GetString(TEXT("keyName"));
    bool bInstanceSynced = Ctx.GetBool(TEXT("isInstanceSynced"), true);

    UBlackboardData* Blackboard = LoadObject<UBlackboardData>(nullptr, *BlackboardPath);
    if (!Blackboard)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Blackboard not found: %s"), *BlackboardPath));
        return true;
    }

    bool bFound = false;
    for (FBlackboardEntry& Entry : Blackboard->Keys)
    {
        if (Entry.EntryName.ToString() == KeyName)
        {
            Entry.bInstanceSynced = bInstanceSynced;
            bFound = true;
            break;
        }
    }

    if (!bFound)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Key not found: %s"), *KeyName));
        return true;
    }

    Blackboard->MarkPackageDirty();
    SavePackageHelperAI(Blackboard->GetOutermost(), Blackboard);

    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    Result->SetStringField(TEXT("keyName"), KeyName);
    Result->SetBoolField(TEXT("isInstanceSynced"), bInstanceSynced);
    AddAssetVerification(Result, Blackboard);
    Ctx.SendSuccess(Result);
    return true;
}

// =====================================================================
// 16.3 Behavior Tree - Expanded (6 actions)
// =====================================================================

REGISTER_RPC_HANDLER("ai.add_composite_node", "ai",
    "Add a composite node to a Behavior Tree",
    RPC_NO_PARAMS)
{
    // Disabled like ai.add_decorator / ai.add_service: this verb only ever set
    // BT->RootNode directly and never created the UBehaviorTreeGraph node that the
    // editor and behavior_tree.* surface address by id, so it returned success with
    // no node id and orphaned the composite (nothing could connect to it). Author
    // composites through behavior_tree.add_node (on a behavior_tree.create asset that
    // seeds the BTGraph + Root), then wire them with behavior_tree.connect_nodes.
    Ctx.SendError(TEXT("DEPRECATED_HANDLER"), TEXT("ai.add_composite_node returned no node id and orphaned the composite on a graph-less asset, and is disabled. Use behavior_tree.create then behavior_tree.add_node (Selector/Sequence) with assetPath and nodeClass, and behavior_tree.connect_nodes to wire it."));
    return true;
}

REGISTER_RPC_HANDLER("ai.add_task_node", "ai",
    "Add a task node to a Behavior Tree",
    RPC_NO_PARAMS)
{
    // Disabled like ai.add_decorator / ai.add_service: this verb constructed the
    // task UObject but never attached it to anything (not even BT->RootNode) and
    // never created the UBehaviorTreeGraph node, so it returned success with no node
    // id and left a pure orphan that nothing could address or connect. Author tasks
    // through behavior_tree.add_node (on a behavior_tree.create asset that seeds the
    // BTGraph + Root), then wire them with behavior_tree.connect_nodes.
    Ctx.SendError(TEXT("DEPRECATED_HANDLER"), TEXT("ai.add_task_node returned no node id and left the task orphaned on a graph-less asset, and is disabled. Use behavior_tree.create then behavior_tree.add_node (MoveTo/Wait) with assetPath and nodeClass, and behavior_tree.connect_nodes to wire it."));
    return true;
}

REGISTER_RPC_HANDLER("ai.add_decorator", "ai",
    "Add a decorator to a Behavior Tree",
    RPC_PARAMS(
        RPC_PARAM_REQ("behaviorTreePath", "path", "Path to the Behavior Tree asset"),
        RPC_PARAM_REQ("decoratorType", "string", "Type of decorator (Blackboard, Cooldown, Loop)")
    ))
{
    Ctx.SendError(TEXT("DEPRECATED_HANDLER"), TEXT("ai.add_decorator created orphaned Behavior Tree decorators and is disabled. Use behavior_tree.attach_decorator with assetPath, parentNodeId, and decoratorClass."));
    return true;
}

REGISTER_RPC_HANDLER("ai.add_service", "ai",
    "Add a service to a Behavior Tree",
    RPC_PARAMS(
        RPC_PARAM_REQ("behaviorTreePath", "path", "Path to the Behavior Tree asset"),
        RPC_PARAM_REQ("serviceType", "string", "Type of service")
    ))
{
    Ctx.SendError(TEXT("DEPRECATED_HANDLER"), TEXT("ai.add_service only created a fake Behavior Tree service reference and is disabled. Use behavior_tree.attach_service with assetPath, parentNodeId, and serviceClass."));
    return true;
}

// =====================================================================
// 16.4 Environment Query System - EQS (5 actions)
// =====================================================================

REGISTER_RPC_HANDLER("ai.create_eqs_query", "ai",
    "Deprecated alias for eqs.create",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the new EQS Query"),
        RPC_PARAM_DEF("path", "path", "Content path for the asset", "/Game/AI/EQS")
    ))
{
    return PinWrightEQS::HandleCreate(Ctx);
}

REGISTER_RPC_HANDLER("ai.add_eqs_generator", "ai",
    "Deprecated alias for eqs.add_generator",
    RPC_PARAMS(
        RPC_PARAM_REQ("queryPath", "path", "Path to the EQS Query asset"),
        RPC_PARAM_REQ("generatorType", "classref", "Type of generator (ActorsOfClass, OnCircle, SimpleGrid)")
    ))
{
    return PinWrightEQS::HandleAddGenerator(Ctx);
}

REGISTER_RPC_HANDLER("ai.add_eqs_context", "ai",
    "Deprecated alias for eqs.set_context_class",
    RPC_PARAMS(
        RPC_PARAM_REQ("queryPath", "path", "Path to the EQS Query asset"),
        RPC_PARAM_REQ("contextType", "classref", "Type of context to add")
    ))
{
    return PinWrightEQS::HandleSetContextClass(Ctx);
}

REGISTER_RPC_HANDLER("ai.add_eqs_test", "ai",
    "Deprecated alias for eqs.add_test",
    RPC_PARAMS(
        RPC_PARAM_REQ("queryPath", "path", "Path to the EQS Query asset"),
        RPC_PARAM_REQ("testType", "classref", "Type of test (Distance, Trace)")
    ))
{
    return PinWrightEQS::HandleAddTest(Ctx);
}

REGISTER_RPC_HANDLER("ai.configure_test_scoring", "ai",
    "Deprecated alias for eqs.set_test_scoring",
    RPC_PARAMS(
        RPC_PARAM_REQ("queryPath", "path", "Path to the EQS Query asset"),
        RPC_PARAM_DEF("testIndex", "integer", "Index of the test to configure", "0")
    ))
{
    return PinWrightEQS::HandleSetTestScoring(Ctx);
}

// Locate the UAIPerceptionComponent template on a controller blueprint's SCS.
// ai.set_ai_perception uses this to find an existing perception component before
// falling back to creating one; returns nullptr when the blueprint has none.
static UAIPerceptionComponent* FindPerceptionComponentTemplate(UBlueprint* Blueprint)
{
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return nullptr;
    }

    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->ComponentTemplate)
        {
            if (UAIPerceptionComponent* Comp = Cast<UAIPerceptionComponent>(Node->ComponentTemplate))
            {
                return Comp;
            }
        }
    }
    return nullptr;
}

// =====================================================================
// 16.6 State Trees - UE5.3+ (4 actions)
// =====================================================================

REGISTER_RPC_HANDLER("ai.create_state_tree", "ai",
    "Create a new State Tree asset (UE 5.3+)",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the new State Tree"),
        RPC_PARAM_DEF("path", "path", "Content path for the asset", "/Game/AI/StateTrees"),
        RPC_PARAM_DEF("schemaType", "string", "Schema type for the State Tree", "Component")
    ))
{
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

#if MCP_STATE_TREE_HEADERS_AVAILABLE
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game/AI/StateTrees"));
    FString SchemaType = Ctx.GetString(TEXT("schemaType"), TEXT("Component"));

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("State Tree name is required"));
        return true;
    }

    // Create the package and asset. Both halves were caller text taken straight off Ctx.GetString
    // and concatenated raw, so this verb was one of the doors into CreatePackage's Fatal.
    FString FullPath;
    if (!PinWrightAiComposeCreatePackagePath(Ctx, Path, Name, FullPath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*FullPath);
    if (!Package)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"),
            FString::Printf(TEXT("Failed to create package: %s"), *FullPath));
        return true;
    }

    UStateTree* StateTree = NewObject<UStateTree>(Package, *Name, RF_Public | RF_Standalone);
    if (!StateTree)
    {
        Package->MarkAsGarbage();
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create StateTree asset"));
        return true;
    }

    // Create and attach EditorData
    UStateTreeEditorData* EditorData = NewObject<UStateTreeEditorData>(StateTree, TEXT("EditorData"), RF_Transactional);
    if (!EditorData)
    {
        StateTree->ConditionalBeginDestroy();
        Package->MarkAsGarbage();
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create StateTree EditorData"));
        return true;
    }
    StateTree->EditorData = EditorData;

    // Assign schema based on type
#if MCP_STATE_TREE_COMPONENT_SCHEMA_AVAILABLE
    EditorData->Schema = NewObject<UStateTreeComponentSchema>(EditorData);
#else
    // UE 5.7+ or schema not available - skip schema assignment
#endif

    // Add a default root state
    UStateTreeState& RootState = EditorData->AddRootState();
    RootState.Name = FName(TEXT("Root"));

    // Save the asset
    McpSafeAssetSave(StateTree);

    Result->SetStringField(TEXT("stateTreePath"), FullPath);
    Result->SetStringField(TEXT("rootStateName"), TEXT("Root"));
    Result->SetStringField(TEXT("message"), TEXT("State Tree created with root state"));
    AddAssetVerification(Result, StateTree);
    Ctx.SendSuccess(Result);
#else
    // Headers not available but version supports it
    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game/AI/StateTrees"));
    Result->SetStringField(TEXT("stateTreePath"), Path / Name);
    Result->SetStringField(TEXT("message"), TEXT("State Tree creation registered (headers unavailable - enable StateTree plugin)"));
    Result->SetBoolField(TEXT("headersUnavailable"), true);
    Ctx.SendSuccess(Result);
#endif
    return true;
}

REGISTER_RPC_HANDLER("ai.add_state_tree_state", "ai",
    "Add a state to a State Tree (UE 5.3+)",
    RPC_PARAMS(
        RPC_PARAM_REQ("stateTreePath", "path", "Path to the State Tree asset"),
        RPC_PARAM_REQ("stateName", "string", "Name for the new state"),
        RPC_PARAM_DEF("parentStateName", "string", "Name of the parent state", "Root"),
        RPC_PARAM_DEF("stateType", "string", "Type of state (State, Group, Linked, LinkedAsset)", "State")
    ))
{
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

#if MCP_STATE_TREE_HEADERS_AVAILABLE
    FString StateTreePath = Ctx.GetString(TEXT("stateTreePath"));
    FString StateName = Ctx.GetString(TEXT("stateName"));
    FString ParentStateName = Ctx.GetString(TEXT("parentStateName"), TEXT("Root"));
    FString StateType = Ctx.GetString(TEXT("stateType"), TEXT("State"));

    if (StateTreePath.IsEmpty() || StateName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("stateTreePath and stateName are required"));
        return true;
    }

    // Load the StateTree
    UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *StateTreePath);
    if (!StateTree)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("StateTree not found: %s"), *StateTreePath));
        return true;
    }

    UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
    if (!EditorData)
    {
        Ctx.SendError(TEXT("INVALID_STATE"), TEXT("StateTree has no EditorData"));
        return true;
    }

    // Find the parent state
    UStateTreeState* ParentState = nullptr;
    for (UStateTreeState* SubTree : EditorData->SubTrees)
    {
        if (SubTree && SubTree->Name.ToString().Equals(ParentStateName, ESearchCase::IgnoreCase))
        {
            ParentState = SubTree;
            break;
        }
        // Check children recursively
        if (SubTree)
        {
            for (UStateTreeState* Child : SubTree->Children)
            {
                if (Child && Child->Name.ToString().Equals(ParentStateName, ESearchCase::IgnoreCase))
                {
                    ParentState = Child;
                    break;
                }
            }
        }
    }

    if (!ParentState)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Parent state '%s' not found"), *ParentStateName));
        return true;
    }

    // Determine state type
    EStateTreeStateType Type = EStateTreeStateType::State;
    if (StateType.Equals(TEXT("Group"), ESearchCase::IgnoreCase))
    {
        Type = EStateTreeStateType::Group;
    }
    else if (StateType.Equals(TEXT("Linked"), ESearchCase::IgnoreCase))
    {
        Type = EStateTreeStateType::Linked;
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    // EStateTreeStateType::LinkedAsset was added in UE 5.4; the value does not exist on 5.3.
    else if (StateType.Equals(TEXT("LinkedAsset"), ESearchCase::IgnoreCase))
    {
        Type = EStateTreeStateType::LinkedAsset;
    }
#endif

    // Add the child state
    UStateTreeState& NewState = ParentState->AddChildState(FName(*StateName), Type);

    // Save
    McpSafeAssetSave(StateTree);

    Result->SetStringField(TEXT("stateName"), StateName);
    Result->SetStringField(TEXT("parentState"), ParentStateName);
    Result->SetStringField(TEXT("stateType"), StateType);
    Result->SetStringField(TEXT("message"), TEXT("State added to StateTree"));
    AddAssetVerification(Result, StateTree);
    Ctx.SendSuccess(Result);
#else
    FString StateTreePath = Ctx.GetString(TEXT("stateTreePath"));
    FString StateName = Ctx.GetString(TEXT("stateName"));
    Result->SetStringField(TEXT("stateName"), StateName);
    Result->SetStringField(TEXT("message"), TEXT("State addition registered (headers unavailable)"));
    Result->SetBoolField(TEXT("headersUnavailable"), true);
    Ctx.SendSuccess(Result);
#endif
    return true;
}

REGISTER_RPC_HANDLER("ai.add_state_tree_transition", "ai",
    "Add a transition between states in a State Tree (UE 5.3+)",
    RPC_PARAMS(
        RPC_PARAM_REQ("stateTreePath", "path", "Path to the State Tree asset"),
        RPC_PARAM_REQ("fromState", "string", "Name of the source state"),
        RPC_PARAM_REQ("toState", "string", "Name of the target state"),
        RPC_PARAM_DEF("triggerType", "string", "Transition trigger type", "OnStateCompleted")
    ))
{
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

#if MCP_STATE_TREE_HEADERS_AVAILABLE
    FString StateTreePath = Ctx.GetString(TEXT("stateTreePath"));
    FString FromState = Ctx.GetString(TEXT("fromState"));
    FString ToState = Ctx.GetString(TEXT("toState"));
    FString TriggerType = Ctx.GetString(TEXT("triggerType"), TEXT("OnStateCompleted"));

    if (StateTreePath.IsEmpty() || FromState.IsEmpty() || ToState.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("stateTreePath, fromState, and toState are required"));
        return true;
    }

    // Load the StateTree
    UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *StateTreePath);
    if (!StateTree)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("StateTree not found: %s"), *StateTreePath));
        return true;
    }

    UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
    if (!EditorData)
    {
        Ctx.SendError(TEXT("INVALID_STATE"), TEXT("StateTree has no EditorData"));
        return true;
    }

    // Find source and target states
    UStateTreeState* SourceState = nullptr;
    UStateTreeState* TargetState = nullptr;

    // Helper lambda to find state recursively
    TFunction<UStateTreeState*(UStateTreeState*, const FString&)> FindState;
    FindState = [&FindState](UStateTreeState* State, const FString& Name) -> UStateTreeState* {
        if (!State) return nullptr;
        if (State->Name.ToString().Equals(Name, ESearchCase::IgnoreCase))
        {
            return State;
        }
        for (UStateTreeState* Child : State->Children)
        {
            if (UStateTreeState* Found = FindState(Child, Name))
            {
                return Found;
            }
        }
        return nullptr;
    };

    for (UStateTreeState* SubTree : EditorData->SubTrees)
    {
        if (!SourceState) SourceState = FindState(SubTree, FromState);
        if (!TargetState) TargetState = FindState(SubTree, ToState);
    }

    if (!SourceState)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Source state '%s' not found"), *FromState));
        return true;
    }

    if (!TargetState)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Target state '%s' not found"), *ToState));
        return true;
    }

    // Determine trigger type
    EStateTreeTransitionTrigger Trigger = EStateTreeTransitionTrigger::OnStateCompleted;
    if (TriggerType.Equals(TEXT("OnStateFailed"), ESearchCase::IgnoreCase))
    {
        Trigger = EStateTreeTransitionTrigger::OnStateFailed;
    }
    else if (TriggerType.Equals(TEXT("OnTick"), ESearchCase::IgnoreCase))
    {
        Trigger = EStateTreeTransitionTrigger::OnTick;
    }
    else if (TriggerType.Equals(TEXT("OnEvent"), ESearchCase::IgnoreCase))
    {
        Trigger = EStateTreeTransitionTrigger::OnEvent;
    }

    // Add transition
    FStateTreeTransition& Transition = SourceState->AddTransition(Trigger, EStateTreeTransitionType::GotoState, TargetState);

    // Save
    McpSafeAssetSave(StateTree);

    Result->SetStringField(TEXT("fromState"), FromState);
    Result->SetStringField(TEXT("toState"), ToState);
    Result->SetStringField(TEXT("triggerType"), TriggerType);
    Result->SetStringField(TEXT("transitionId"), Transition.ID.ToString());
    Result->SetStringField(TEXT("message"), TEXT("Transition added"));
    Ctx.SendSuccess(Result);
#else
    FString StateTreePath = Ctx.GetString(TEXT("stateTreePath"));
    FString FromState = Ctx.GetString(TEXT("fromState"));
    FString ToState = Ctx.GetString(TEXT("toState"));
    Result->SetStringField(TEXT("fromState"), FromState);
    Result->SetStringField(TEXT("toState"), ToState);
    Result->SetStringField(TEXT("message"), TEXT("Transition registered (headers unavailable)"));
    Result->SetBoolField(TEXT("headersUnavailable"), true);
    Ctx.SendSuccess(Result);
#endif
    return true;
}

// =====================================================================
// 16.7 Smart Objects (4 actions)
//
// SmartObjects and MassGameplay are optional plugins this module neither
// links nor includes headers from. All access below is reflection-only,
// following the engine's linkage-free optional-plugin pattern
// (LevelSnapshots' PCGRestoration): IPluginManager gate, FindObject<UClass>
// on /Script paths, FProperty member access.
// =====================================================================

// Verified owning modules, stable across UE 5.3-5.8: the SmartObject classes
// live in SmartObjectsModule, UMassEntityConfigAsset in MassSpawner.
static const TCHAR* const SmartObjectDefinitionClassPath = TEXT("/Script/SmartObjectsModule.SmartObjectDefinition");
static const TCHAR* const SmartObjectBehaviorDefinitionClassPath = TEXT("/Script/SmartObjectsModule.SmartObjectBehaviorDefinition");
static const TCHAR* const SmartObjectComponentClassPath = TEXT("/Script/SmartObjectsModule.SmartObjectComponent");
static const TCHAR* const MassEntityConfigAssetClassPath = TEXT("/Script/MassSpawner.MassEntityConfigAsset");

// Entry gate for the Smart Object / Mass handlers: the owning plugin must be
// installed and enabled. Mirrors PoseSearchHandler's EnsurePoseSearchAvailable
// PLUGIN_DISABLED contract.
static bool EnsureOptionalPluginEnabled(FHandlerContext& Ctx, const TCHAR* PluginName, const TCHAR* HandlerFamily)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
    if (!Plugin.IsValid() || !Plugin->IsEnabled())
    {
        Ctx.SendError(TEXT("PLUGIN_DISABLED"),
            FString::Printf(TEXT("%s plugin is not enabled. Enable the %s plugin before using %s handlers."),
                PluginName, PluginName, HandlerFamily));
        return false;
    }
    return true;
}

// Resolve a class from an optional plugin by its /Script path. Only called after
// the plugin gate passed, so a miss means the class moved or was renamed in this
// engine version — report it rather than degrade.
static UClass* ResolveOptionalPluginClass(FHandlerContext& Ctx, const TCHAR* ScriptPath)
{
    UClass* Cls = FindObject<UClass>(nullptr, ScriptPath);
    if (!Cls)
    {
        Cls = LoadObject<UClass>(nullptr, ScriptPath);
    }
    if (!Cls)
    {
        Ctx.SendError(TEXT("CLASS_NOT_FOUND"),
            FString::Printf(TEXT("Class %s not found even though its owning plugin is enabled (renamed in this engine version?)"), ScriptPath));
    }
    return Cls;
}

// Locate the private Slots array (TArray<FSmartObjectSlotDefinition>) on a
// SmartObjectDefinition instance. Sends PROPERTY_NOT_FOUND and returns false
// if the reflected layout does not match (engine drift).
static bool ResolveSmartObjectSlotsArray(FHandlerContext& Ctx, UObject* Definition,
    FArrayProperty*& OutSlotsProp, FStructProperty*& OutSlotStructProp)
{
    OutSlotsProp = CastField<FArrayProperty>(Definition->GetClass()->FindPropertyByName(TEXT("Slots")));
    OutSlotStructProp = OutSlotsProp ? CastField<FStructProperty>(OutSlotsProp->Inner) : nullptr;
    if (!OutSlotStructProp)
    {
        Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
            TEXT("SmartObjectDefinition.Slots array property not found via reflection (engine layout drift?)"));
        return false;
    }
    return true;
}

REGISTER_RPC_HANDLER("ai.create_smart_object_definition", "ai",
    "Create a new Smart Object Definition asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the definition"),
        RPC_PARAM_DEF("path", "path", "Content path for the asset", "/Game/AI/SmartObjects")
    ))
{
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

    if (!EnsureOptionalPluginEnabled(Ctx, TEXT("SmartObjects"), TEXT("ai.* Smart Object"))) return true;

    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game/AI/SmartObjects"));

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Smart Object Definition name is required"));
        return true;
    }

    UClass* DefinitionClass = ResolveOptionalPluginClass(Ctx, SmartObjectDefinitionClassPath);
    if (!DefinitionClass) return true;

    // Create the package and asset. Both halves were caller text taken straight off Ctx.GetString
    // and concatenated raw, so this verb was one of the doors into CreatePackage's Fatal.
    FString FullPath;
    if (!PinWrightAiComposeCreatePackagePath(Ctx, Path, Name, FullPath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*FullPath);
    if (!Package)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"),
            FString::Printf(TEXT("Failed to create package: %s"), *FullPath));
        return true;
    }

    UObject* Definition = NewObject<UObject>(Package, DefinitionClass, FName(*Name), RF_Public | RF_Standalone);
    if (!Definition)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create SmartObjectDefinition asset"));
        return true;
    }

    McpSafeAssetSave(Definition);

    Result->SetStringField(TEXT("definitionPath"), FullPath);
    Result->SetNumberField(TEXT("slotCount"), 0);
    Result->SetStringField(TEXT("message"), TEXT("Smart Object Definition created"));
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("ai.add_smart_object_slot", "ai",
    "Add a slot to a Smart Object Definition",
    RPC_PARAMS(
        RPC_PARAM_REQ("definitionPath", "path", "Path to the Smart Object Definition"),
        RPC_PARAM_OPT("offset", "object", "Slot offset (x, y, z)"),
        RPC_PARAM_OPT("rotation", "object", "Slot rotation (pitch, yaw, roll)"),
        RPC_PARAM_DEF("enabled", "boolean", "Whether the slot is enabled", "true")
    ))
{
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

    if (!EnsureOptionalPluginEnabled(Ctx, TEXT("SmartObjects"), TEXT("ai.* Smart Object"))) return true;

    FString DefinitionPath = Ctx.GetString(TEXT("definitionPath"));
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FVector Offset = ExtractVectorField(Payload, TEXT("offset"), FVector::ZeroVector);
    FRotator Rotation = ExtractRotatorField(Payload, TEXT("rotation"), FRotator::ZeroRotator);
    bool bEnabled = Ctx.GetBool(TEXT("enabled"), true);

    if (DefinitionPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("definitionPath is required"));
        return true;
    }

    UClass* DefinitionClass = ResolveOptionalPluginClass(Ctx, SmartObjectDefinitionClassPath);
    if (!DefinitionClass) return true;

    UObject* Definition = LoadObject<UObject>(nullptr, *DefinitionPath);
    if (!Definition || !Definition->IsA(DefinitionClass))
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("SmartObjectDefinition not found: %s"), *DefinitionPath));
        return true;
    }

    // Append a default-initialized slot to the private Slots array, then write the
    // requested fields through the slot struct's reflected properties. Slot layout
    // (UE 5.1+): Offset = FVector3f, Rotation = FRotator3f, bEnabled = bool,
    // ID = FGuid — all UPROPERTYs, verified stable across UE 5.3-5.8.
    FArrayProperty* SlotsProp = nullptr;
    FStructProperty* SlotStructProp = nullptr;
    if (!ResolveSmartObjectSlotsArray(Ctx, Definition, SlotsProp, SlotStructProp)) return true;

    UScriptStruct* SlotStruct = SlotStructProp->Struct;
    FStructProperty* OffsetProp = CastField<FStructProperty>(SlotStruct->FindPropertyByName(TEXT("Offset")));
    FStructProperty* RotationProp = CastField<FStructProperty>(SlotStruct->FindPropertyByName(TEXT("Rotation")));
    FBoolProperty* EnabledProp = CastField<FBoolProperty>(SlotStruct->FindPropertyByName(TEXT("bEnabled")));
    FStructProperty* IdProp = CastField<FStructProperty>(SlotStruct->FindPropertyByName(TEXT("ID")));
    const bool bSlotLayoutOk =
        OffsetProp && OffsetProp->Struct == TVariantStructure<FVector3f>::Get() &&
        RotationProp && RotationProp->Struct == TVariantStructure<FRotator3f>::Get() &&
        EnabledProp &&
        IdProp && IdProp->Struct == TBaseStructure<FGuid>::Get();
    if (!bSlotLayoutOk)
    {
        Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
            TEXT("FSmartObjectSlotDefinition fields (Offset/Rotation/bEnabled/ID) did not match the expected reflected layout (engine drift?)"));
        return true;
    }

    FScriptArrayHelper SlotsHelper(SlotsProp, SlotsProp->ContainerPtrToValuePtr<void>(Definition));
    const int32 SlotIndex = SlotsHelper.AddValue();
    uint8* SlotPtr = SlotsHelper.GetRawPtr(SlotIndex);
    *OffsetProp->ContainerPtrToValuePtr<FVector3f>(SlotPtr) = FVector3f(Offset);
    *RotationProp->ContainerPtrToValuePtr<FRotator3f>(SlotPtr) = FRotator3f(Rotation);
    EnabledProp->SetPropertyValue(EnabledProp->ContainerPtrToValuePtr<void>(SlotPtr), bEnabled);
    *IdProp->ContainerPtrToValuePtr<FGuid>(SlotPtr) = FGuid::NewGuid();

    McpSafeAssetSave(Definition);

    Result->SetNumberField(TEXT("slotIndex"), SlotIndex);
    Result->SetStringField(TEXT("definitionPath"), DefinitionPath);
    Result->SetStringField(TEXT("message"), TEXT("Slot added to Smart Object Definition"));
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("ai.configure_slot_behavior", "ai",
    "Configure behavior for a Smart Object slot",
    RPC_PARAMS(
        RPC_PARAM_REQ("definitionPath", "path", "Path to the Smart Object Definition"),
        RPC_PARAM_DEF("slotIndex", "integer", "Index of the slot to configure", "0"),
        RPC_PARAM_OPT("behaviorType", "string", "Type of behavior"),
        RPC_PARAM_OPT("activityTags", "array", "Array of gameplay tag strings"),
        RPC_PARAM_OPT("enabled", "boolean", "Whether the slot is enabled")
    ))
{
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

    if (!EnsureOptionalPluginEnabled(Ctx, TEXT("SmartObjects"), TEXT("ai.* Smart Object"))) return true;

    FString DefinitionPath = Ctx.GetString(TEXT("definitionPath"));
    int32 SlotIndex = Ctx.GetInt(TEXT("slotIndex"), 0);
    FString BehaviorType = Ctx.GetString(TEXT("behaviorType"), TEXT(""));

    if (DefinitionPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("definitionPath is required"));
        return true;
    }

    UClass* DefinitionClass = ResolveOptionalPluginClass(Ctx, SmartObjectDefinitionClassPath);
    if (!DefinitionClass) return true;

    UObject* Definition = LoadObject<UObject>(nullptr, *DefinitionPath);
    if (!Definition || !Definition->IsA(DefinitionClass))
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("SmartObjectDefinition not found: %s"), *DefinitionPath));
        return true;
    }

    FArrayProperty* SlotsProp = nullptr;
    FStructProperty* SlotStructProp = nullptr;
    if (!ResolveSmartObjectSlotsArray(Ctx, Definition, SlotsProp, SlotStructProp)) return true;

    FScriptArrayHelper SlotsHelper(SlotsProp, SlotsProp->ContainerPtrToValuePtr<void>(Definition));
    if (SlotIndex < 0 || SlotIndex >= SlotsHelper.Num())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("Invalid slot index: %d"), SlotIndex));
        return true;
    }

    // Reflected slot-field lookups used below, resolved before any mutation so
    // layout drift is rejected up front (validate-before-mutate). BehaviorDefinitions
    // is needed on every path because behaviorCount is always reported.
    UScriptStruct* SlotStruct = SlotStructProp->Struct;
    uint8* SlotPtr = SlotsHelper.GetRawPtr(SlotIndex);
    FArrayProperty* BehaviorsProp = CastField<FArrayProperty>(SlotStruct->FindPropertyByName(TEXT("BehaviorDefinitions")));
    FObjectProperty* BehaviorObjProp = BehaviorsProp ? CastField<FObjectProperty>(BehaviorsProp->Inner) : nullptr;
    if (!BehaviorObjProp)
    {
        Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
            TEXT("FSmartObjectSlotDefinition.BehaviorDefinitions array property not found via reflection (engine drift?)"));
        return true;
    }
    FStructProperty* TagsProp = CastField<FStructProperty>(SlotStruct->FindPropertyByName(TEXT("ActivityTags")));
    if (TagsProp && TagsProp->Struct != FGameplayTagContainer::StaticStruct())
    {
        TagsProp = nullptr;
    }
    FBoolProperty* EnabledProp = CastField<FBoolProperty>(SlotStruct->FindPropertyByName(TEXT("bEnabled")));

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    // ---- Validate everything BEFORE mutating, so a rejected param never leaves a
    // ---- silent partial write behind (the defect this handler used to ship). ----

    // behaviorType: resolve the named type to a concrete subclass of the (reflected)
    // USmartObjectBehaviorDefinition base. The base class is Abstract and every concrete
    // subclass lives in an optional plugin (GameplayBehaviorSmartObjects /
    // GameplayInteractions / MassSmartObjects) that this gateway does not link, so both
    // the base and the subclass are looked up by name rather than referenced directly.
    // A non-empty behaviorType that does not resolve to an instantiable subclass is
    // rejected with INVALID_PARAMS — never silently ignored.
    UClass* BehaviorClass = nullptr;
    if (!BehaviorType.IsEmpty())
    {
        UClass* const BehaviorBase = ResolveOptionalPluginClass(Ctx, SmartObjectBehaviorDefinitionClassPath);
        if (!BehaviorBase) return true;

        // Single concrete-subclass predicate, used both to validate the resolved class
        // and to enumerate the available types on the error path.
        auto IsConcreteBehavior = [BehaviorBase](const UClass* Candidate)
        {
            return Candidate && Candidate->IsChildOf(BehaviorBase) && Candidate != BehaviorBase
                && !Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
        };

        // Resolve through the shared class resolver (full /Script/ path, exact name,
        // U-prefixed name, load-by-asset) — the same resolve-then-constrain pattern the
        // sibling AI handlers (EQSHandler, BehaviorTreeHandler) use.
        BehaviorClass = ResolveUClass(BehaviorType);
        if (!BehaviorClass)
        {
            BehaviorClass = ResolveClassByName(BehaviorType);
        }

        if (!IsConcreteBehavior(BehaviorClass))
        {
            BehaviorClass = nullptr;

            // Enumerate the concrete subclasses currently available so the caller knows
            // which behaviorType values are accepted on this editor's plugin set.
            TArray<FString> Available;
            TArray<UClass*> Derived;
            GetDerivedClasses(BehaviorBase, Derived, /*bRecursive*/ true);
            for (UClass* const Candidate : Derived)
            {
                if (IsConcreteBehavior(Candidate))
                {
                    Available.Add(Candidate->GetPathName());
                }
            }
            TSharedPtr<FJsonObject> ErrData = MakeShareable(new FJsonObject());
            ErrData->SetStringField(TEXT("behaviorType"), BehaviorType);
            ErrData->SetArrayField(TEXT("availableBehaviorTypes"), EmitStringArray(Available));
            const FString Msg = Available.Num() > 0
                ? FString::Printf(TEXT("Unknown behaviorType '%s'. No concrete USmartObjectBehaviorDefinition subclass matches; see availableBehaviorTypes."), *BehaviorType)
                : FString::Printf(TEXT("behaviorType '%s' cannot be applied: no concrete USmartObjectBehaviorDefinition subclass is loaded (enable a behavior plugin such as GameplayBehaviorSmartObjects, GameplayInteractions, or MassSmartObjects)."), *BehaviorType);
            Ctx.SendError(TEXT("INVALID_PARAMS"), Msg, ErrData);
            return true;
        }
    }

    // activityTags: resolve each tag against the registry. Collect any that do not
    // resolve so we can report them instead of silently dropping them.
    TArray<FGameplayTag> ResolvedTags;
    TArray<FString> DroppedTags;
    if (Payload->HasField(TEXT("activityTags")))
    {
        const TArray<TSharedPtr<FJsonValue>>* TagsArray = nullptr;
        if (Payload->TryGetArrayField(TEXT("activityTags"), TagsArray))
        {
            for (const auto& TagValue : *TagsArray)
            {
                FString TagStr = TagValue->AsString();
                if (TagStr.IsEmpty())
                {
                    continue;
                }
                FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagStr), false);
                if (Tag.IsValid())
                {
                    ResolvedTags.Add(Tag);
                }
                else
                {
                    DroppedTags.Add(TagStr);
                }
            }
        }
    }

    if (DroppedTags.Num() > 0)
    {
        TSharedPtr<FJsonObject> ErrData = MakeShareable(new FJsonObject());
        ErrData->SetArrayField(TEXT("droppedTags"), EmitStringArray(DroppedTags));
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            FString::Printf(TEXT("%d activity tag(s) are not registered and would be dropped; register them first (e.g. via gameplay_tags.add) then retry. See droppedTags."), DroppedTags.Num()),
            ErrData);
        return true;
    }

    // Reject requested writes whose reflected slot field is missing before mutating
    // anything — never silently drop part of the request on engine layout drift.
    if (ResolvedTags.Num() > 0 && !TagsProp)
    {
        Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
            TEXT("FSmartObjectSlotDefinition.ActivityTags property not found via reflection (engine drift?)"));
        return true;
    }
    if (Payload->HasField(TEXT("enabled")) && !EnabledProp)
    {
        Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
            TEXT("FSmartObjectSlotDefinition.bEnabled property not found via reflection (engine drift?)"));
        return true;
    }

    // ---- All params valid: mutate the slot through its reflected properties. ----

    // Attach the behavior definition. BehaviorDefinitions is an Instanced array, so the
    // object must be created with the Definition asset as its outer. Replace any existing
    // definition of the same class (the array is documented as one-per-type).
    FScriptArrayHelper BehaviorsHelper(BehaviorsProp, BehaviorsProp->ContainerPtrToValuePtr<void>(SlotPtr));
    if (BehaviorClass)
    {
        for (int32 Index = BehaviorsHelper.Num() - 1; Index >= 0; --Index)
        {
            UObject* Existing = BehaviorObjProp->GetObjectPropertyValue(BehaviorsHelper.GetRawPtr(Index));
            if (Existing && Existing->GetClass() == BehaviorClass)
            {
                BehaviorsHelper.RemoveValues(Index, 1);
            }
        }
        UObject* NewBehavior = NewObject<UObject>(Definition, BehaviorClass, NAME_None, RF_Public);
        if (NewBehavior)
        {
            const int32 NewIndex = BehaviorsHelper.AddValue();
            BehaviorObjProp->SetObjectPropertyValue(BehaviorsHelper.GetRawPtr(NewIndex), NewBehavior);
        }
        else
        {
            Ctx.SendError(TEXT("CREATION_FAILED"),
                FString::Printf(TEXT("Failed to instantiate behavior definition of type %s"), *BehaviorClass->GetPathName()));
            return true;
        }
    }

    // Apply resolved activity tags (all validated above). FGameplayTagContainer lives
    // in the always-linked GameplayTags module, so once the FProperty is located the
    // real container type can be used.
    if (ResolvedTags.Num() > 0)
    {
        FGameplayTagContainer* TagContainer = TagsProp->ContainerPtrToValuePtr<FGameplayTagContainer>(SlotPtr);
        for (const FGameplayTag& Tag : ResolvedTags)
        {
            TagContainer->AddTag(Tag);
        }
    }

    // Configure enabled state
    if (Payload->HasField(TEXT("enabled")))
    {
        EnabledProp->SetPropertyValue(EnabledProp->ContainerPtrToValuePtr<void>(SlotPtr), Ctx.GetBool(TEXT("enabled"), true));
    }

    McpSafeAssetSave(Definition);

    Result->SetNumberField(TEXT("slotIndex"), SlotIndex);
    Result->SetNumberField(TEXT("behaviorCount"), BehaviorsHelper.Num());
    // The attach block early-returns on failure, so a non-null BehaviorClass here means
    // the behavior was instantiated and attached.
    Result->SetBoolField(TEXT("behaviorAttached"), BehaviorClass != nullptr);
    if (BehaviorClass)
    {
        Result->SetStringField(TEXT("behaviorClass"), BehaviorClass->GetPathName());
    }
    Result->SetNumberField(TEXT("activityTagsAdded"), ResolvedTags.Num());
    Result->SetStringField(TEXT("message"), TEXT("Slot behavior configured"));
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("ai.add_smart_object_component", "ai",
    "Add a Smart Object component to a blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_OPT("definitionPath", "path", "Path to the Smart Object Definition to assign"),
        RPC_PARAM_DEF("componentName", "string", "Name for the component", "SmartObjectComponent")
    ))
{
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

    if (!EnsureOptionalPluginEnabled(Ctx, TEXT("SmartObjects"), TEXT("ai.* Smart Object"))) return true;

    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    FString DefinitionPath = Ctx.GetString(TEXT("definitionPath"), TEXT(""));
    FString ComponentName = Ctx.GetString(TEXT("componentName"), TEXT("SmartObjectComponent"));

    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("blueprintPath is required"));
        return true;
    }

    UClass* ComponentClass = ResolveOptionalPluginClass(Ctx, SmartObjectComponentClassPath);
    if (!ComponentClass) return true;

    // Load the Blueprint
    FString NormalizedPath, LoadError;
    UBlueprint* Blueprint = LoadBlueprintAsset(BlueprintPath, NormalizedPath, LoadError);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), LoadError);
        return true;
    }

    // Load the definition if provided (silently ignored when unresolvable, matching
    // the prior typed-load behavior).
    UObject* Definition = nullptr;
    if (!DefinitionPath.IsEmpty())
    {
        UClass* DefinitionClass = FindObject<UClass>(nullptr, SmartObjectDefinitionClassPath);
        UObject* Loaded = LoadObject<UObject>(nullptr, *DefinitionPath);
        if (Loaded && DefinitionClass && Loaded->IsA(DefinitionClass))
        {
            Definition = Loaded;
        }
    }

    // Get the SCS
    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    if (!SCS)
    {
        Ctx.SendError(TEXT("INVALID_STATE"), TEXT("Blueprint has no SimpleConstructionScript"));
        return true;
    }

    // Create the component node
    USCS_Node* NewNode = SCS->CreateNode(ComponentClass, FName(*ComponentName));
    if (!NewNode)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create SCS node for SmartObjectComponent"));
        return true;
    }

    // Assign the definition on the component template. SetDefinition() is a typed
    // accessor we cannot call without linking SmartObjectsModule; on a fresh SCS
    // template it reduces to writing the backing UPROPERTY, which moved across
    // versions: DefinitionRef.SmartObjectDefinition (FSmartObjectDefinitionReference,
    // UE 5.4+) vs. the direct DefinitionAsset object property (UE 5.3).
    UActorComponent* Template = NewNode->ComponentTemplate;
    if (Template && Definition)
    {
        bool bAssigned = false;
        if (FStructProperty* RefProp = CastField<FStructProperty>(Template->GetClass()->FindPropertyByName(TEXT("DefinitionRef"))))
        {
            if (FObjectProperty* InnerDefProp = CastField<FObjectProperty>(RefProp->Struct->FindPropertyByName(TEXT("SmartObjectDefinition"))))
            {
                void* RefPtr = RefProp->ContainerPtrToValuePtr<void>(Template);
                InnerDefProp->SetObjectPropertyValue(InnerDefProp->ContainerPtrToValuePtr<void>(RefPtr), Definition);
                bAssigned = true;
            }
        }
        if (!bAssigned)
        {
            if (FObjectProperty* AssetProp = CastField<FObjectProperty>(Template->GetClass()->FindPropertyByName(TEXT("DefinitionAsset"))))
            {
                AssetProp->SetObjectPropertyValue(AssetProp->ContainerPtrToValuePtr<void>(Template), Definition);
                bAssigned = true;
            }
        }
        if (!bAssigned)
        {
            Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
                TEXT("Could not assign the Smart Object definition: neither DefinitionRef.SmartObjectDefinition nor DefinitionAsset found on the component template (engine drift?)"));
            return true;
        }
    }

    // Add to SCS
    SCS->AddNode(NewNode);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    McpSafeAssetSave(Blueprint);

    Result->SetStringField(TEXT("componentName"), ComponentName);
    Result->SetStringField(TEXT("blueprintPath"), NormalizedPath);
    if (Definition)
    {
        Result->SetStringField(TEXT("definitionPath"), DefinitionPath);
    }
    Result->SetStringField(TEXT("message"), TEXT("Smart Object component added to blueprint"));
    Ctx.SendSuccess(Result);
    return true;
}

// =====================================================================
// 16.8 Mass AI / Crowds (3 actions)
//
// Reflection-only against the optional MassGameplay plugin — same pattern
// as the Smart Objects section above.
// =====================================================================

REGISTER_RPC_HANDLER("ai.create_mass_entity_config", "ai",
    "Create a new Mass Entity Config asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the config asset"),
        RPC_PARAM_DEF("path", "path", "Content path for the asset", "/Game/AI/Mass")
    ))
{
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

    if (!EnsureOptionalPluginEnabled(Ctx, TEXT("MassGameplay"), TEXT("ai.* Mass"))) return true;

    FString Name = Ctx.GetString(TEXT("name"));
    FString Path = Ctx.GetString(TEXT("path"), TEXT("/Game/AI/Mass"));

    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("Mass Entity Config name is required"));
        return true;
    }

    UClass* ConfigAssetClass = ResolveOptionalPluginClass(Ctx, MassEntityConfigAssetClassPath);
    if (!ConfigAssetClass) return true;

    // Both halves were caller text taken straight off Ctx.GetString and concatenated raw, so this
    // verb was one of the doors into CreatePackage's Fatal.
    FString FullPath;
    if (!PinWrightAiComposeCreatePackagePath(Ctx, Path, Name, FullPath))
    {
        return true;
    }

    UPackage* Package = CreatePackage(*FullPath);
    if (!Package)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"),
            FString::Printf(TEXT("Failed to create package: %s"), *FullPath));
        return true;
    }

    UObject* ConfigAsset = NewObject<UObject>(Package, ConfigAssetClass, FName(*Name), RF_Public | RF_Standalone);
    if (!ConfigAsset)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create MassEntityConfigAsset"));
        return true;
    }

    McpSafeAssetSave(ConfigAsset);

    Result->SetStringField(TEXT("configPath"), FullPath);
    Result->SetNumberField(TEXT("traitCount"), 0);
    Result->SetStringField(TEXT("message"), TEXT("Mass Entity Config created"));
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("ai.configure_mass_entity", "ai",
    "Configure a Mass Entity Config asset",
    RPC_PARAMS(
        RPC_PARAM_REQ("configPath", "path", "Path to the Mass Entity Config asset"),
        RPC_PARAM_OPT("parentConfigPath", "path", "Path to a parent config asset")
    ))
{
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());

    if (!EnsureOptionalPluginEnabled(Ctx, TEXT("MassGameplay"), TEXT("ai.* Mass"))) return true;

    FString ConfigPath = Ctx.GetString(TEXT("configPath"));
    FString ParentConfigPath = Ctx.GetString(TEXT("parentConfigPath"), TEXT(""));

    if (ConfigPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"), TEXT("configPath is required"));
        return true;
    }

    UClass* ConfigAssetClass = ResolveOptionalPluginClass(Ctx, MassEntityConfigAssetClassPath);
    if (!ConfigAssetClass) return true;

    UObject* ConfigAsset = LoadObject<UObject>(nullptr, *ConfigPath);
    if (!ConfigAsset || !ConfigAsset->IsA(ConfigAssetClass))
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("MassEntityConfigAsset not found: %s"), *ConfigPath));
        return true;
    }

    // The FMassEntityConfig payload lives in the asset's Config struct UPROPERTY;
    // Parent and Traits inside it are UPROPERTYs too (stable across UE 5.3-5.8).
    FStructProperty* ConfigProp = CastField<FStructProperty>(ConfigAsset->GetClass()->FindPropertyByName(TEXT("Config")));
    if (!ConfigProp)
    {
        Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
            TEXT("MassEntityConfigAsset.Config struct property not found via reflection (engine drift?)"));
        return true;
    }
    void* ConfigPtr = ConfigProp->ContainerPtrToValuePtr<void>(ConfigAsset);

    FArrayProperty* TraitsProp = CastField<FArrayProperty>(ConfigProp->Struct->FindPropertyByName(TEXT("Traits")));
    if (!TraitsProp)
    {
        Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
            TEXT("FMassEntityConfig.Traits array property not found via reflection (engine drift?)"));
        return true;
    }

    // Set parent config if provided (silently skipped when unresolvable, matching the
    // prior typed-load behavior). SetParentAsset() only assigns the Parent UPROPERTY,
    // so a direct property write is equivalent.
    if (!ParentConfigPath.IsEmpty())
    {
        UObject* ParentConfig = LoadObject<UObject>(nullptr, *ParentConfigPath);
        if (ParentConfig && ParentConfig->IsA(ConfigAssetClass))
        {
            FObjectProperty* ParentProp = CastField<FObjectProperty>(ConfigProp->Struct->FindPropertyByName(TEXT("Parent")));
            if (!ParentProp)
            {
                Ctx.SendError(TEXT("PROPERTY_NOT_FOUND"),
                    TEXT("FMassEntityConfig.Parent object property not found via reflection (engine drift?)"));
                return true;
            }
            ParentProp->SetObjectPropertyValue(ParentProp->ContainerPtrToValuePtr<void>(ConfigPtr), ParentConfig);
        }
    }

    McpSafeAssetSave(ConfigAsset);

    FScriptArrayHelper TraitsHelper(TraitsProp, TraitsProp->ContainerPtrToValuePtr<void>(ConfigPtr));
    Result->SetStringField(TEXT("configPath"), ConfigPath);
    Result->SetNumberField(TEXT("traitCount"), TraitsHelper.Num());
    Result->SetStringField(TEXT("message"), TEXT("Mass Entity configured"));
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("ai.add_mass_spawner", "ai",
    "Not implemented: returns NOT_IMPLEMENTED without modifying the blueprint (see the method page for the real Mass spawner authoring route)",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint (unused: this verb is an unimplemented stub)")
    ))
{
    // ai.add_mass_spawner is a stub: the previous implementation loaded the target
    // Blueprint, then did nothing but MarkPackageDirty() + McpSafeAssetSave() and echoed
    // the request params (componentName/spawnCount/configPath) back inside a SendSuccess
    // result — it never added a component, wrote the CDO, or wired the config/count onto
    // the asset, so the caller got an affirmative success for a no-op. Engine Mass also has
    // no UMassSpawnerComponent: the spawner is the AMassSpawner *actor* (Count +
    // EntityTypes[].EntityConfig), so the promised "MassSpawner" component could never
    // exist. Fail loud instead of reporting a phantom success (matches the accepted
    // NOT_IMPLEMENTED resolution of B-material-stub-handlers-silent-success and
    // B-input-trigger-modifier-stub-silent-success). Unconditional (no plugin gate):
    // the verb references no Mass type and is a stub whether or not the MassGameplay
    // plugin is enabled, so every host returns the same NOT_IMPLEMENTED — a gate here
    // would guard nothing, and UNSUPPORTED_VERSION wrongly implied a newer engine
    // could run it (per CLAUDE.md, UNSUPPORTED_ENGINE_VERSION is reserved for
    // features needing a newer UE).
    Ctx.SendError(TEXT("NOT_IMPLEMENTED"),
        TEXT("ai.add_mass_spawner is a stub and does not modify the blueprint. "
             "Author a Mass spawner directly: reparent the blueprint to AMassSpawner "
             "(/Script/MassSpawner.MassSpawner), then set Count and "
             "EntityTypes[0].EntityConfig on the CDO (blueprint.set_default + property.set)."));
    return true;
}

// =====================================================================
// Utility (1 action)
// =====================================================================

REGISTER_RPC_HANDLER("ai.get_ai_info", "ai",
    "Get information about AI assets (controllers, behavior trees, blackboards, EQS)",
    RPC_PARAMS(
        RPC_PARAM_OPT("controllerPath", "path", "Path to an AI Controller blueprint"),
        RPC_PARAM_OPT("behaviorTreePath", "path", "Path to a Behavior Tree asset"),
        RPC_PARAM_OPT("blackboardPath", "path", "Path to a Blackboard asset"),
        RPC_PARAM_OPT("queryPath", "path", "Path to an EQS Query asset")
    ))
{
    TSharedPtr<FJsonObject> Result = MakeShareable(new FJsonObject());
    TSharedPtr<FJsonObject> AIInfo = MakeShareable(new FJsonObject());

    // Check for controller
    FString ControllerPath = Ctx.GetString(TEXT("controllerPath"));
    if (!ControllerPath.IsEmpty())
    {
        UBlueprint* Controller = LoadObject<UBlueprint>(nullptr, *ControllerPath);
        if (Controller)
        {
            AIInfo->SetStringField(TEXT("controllerClass"), Controller->GeneratedClass ? Controller->GeneratedClass->GetName() : TEXT("Unknown"));
        }
    }

    // Check for behavior tree
    FString BTPath = Ctx.GetString(TEXT("behaviorTreePath"));
    if (!BTPath.IsEmpty())
    {
        UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *BTPath);
        if (BT)
        {
            AIInfo->SetStringField(TEXT("behaviorTreeName"), BT->GetName());
            AIInfo->SetBoolField(TEXT("hasRootNode"), BT->RootNode != nullptr);
        }
    }

    // Check for blackboard
    FString BBPath = Ctx.GetString(TEXT("blackboardPath"));
    if (!BBPath.IsEmpty())
    {
        UBlackboardData* BB = LoadObject<UBlackboardData>(nullptr, *BBPath);
        if (BB)
        {
            AIInfo->SetNumberField(TEXT("keyCount"), BB->Keys.Num());
            TArray<TSharedPtr<FJsonValue>> KeysArray;
            for (const FBlackboardEntry& Entry : BB->Keys)
            {
                TSharedPtr<FJsonObject> KeyObj = MakeShareable(new FJsonObject());
                KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
                KeyObj->SetStringField(TEXT("type"), Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : TEXT("Unknown"));
                KeyObj->SetBoolField(TEXT("instanceSynced"), Entry.bInstanceSynced);
                KeysArray.Add(MakeShareable(new FJsonValueObject(KeyObj)));
            }
            AIInfo->SetArrayField(TEXT("keys"), KeysArray);
        }
    }

    // Check for EQS query
    FString QueryPath = Ctx.GetString(TEXT("queryPath"));
    if (!QueryPath.IsEmpty())
    {
        UEnvQuery* Query = LoadObject<UEnvQuery>(nullptr, *QueryPath);
        if (Query)
        {
            AIInfo->SetStringField(TEXT("queryName"), Query->GetName());
        }
    }

    Result->SetObjectField(TEXT("aiInfo"), AIInfo);
    Ctx.SendSuccess(Result);
    return true;
}

// =====================================================================
// Configuration Actions (3 actions)
// =====================================================================

REGISTER_RPC_HANDLER("ai.set_ai_perception", "ai",
    "Configure AI perception (sight, hearing, damage) on an AI Controller blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("controllerPath", "path", "Path to the AI Controller blueprint"),
        RPC_PARAM_OPT("enableSight", "boolean", "Enable sight sense"),
        RPC_PARAM_OPT("sightRadius", "number", "Sight radius"),
        RPC_PARAM_OPT("loseSightRadius", "number", "Lose sight radius"),
        RPC_PARAM_OPT("peripheralVisionAngle", "number", "Peripheral vision angle in degrees"),
        RPC_PARAM_OPT("enableHearing", "boolean", "Enable hearing sense"),
        RPC_PARAM_OPT("hearingRange", "number", "Hearing range"),
        RPC_PARAM_OPT("enableDamage", "boolean", "Enable damage sense"),
        RPC_PARAM_OPT("dominantSense", "string", "Dominant sense (Sight, Hearing, Damage)")
    ))
{
    FString ControllerPath = Ctx.GetString(TEXT("controllerPath"));
    if (ControllerPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing controllerPath"));
        return true;
    }

    UBlueprint* ControllerBP = LoadObject<UBlueprint>(nullptr, *ControllerPath);
    if (!ControllerBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Controller blueprint not found: %s"), *ControllerPath));
        return true;
    }

    if (!ControllerBP->SimpleConstructionScript)
    {
        Ctx.SendError(TEXT("INVALID_STATE"), TEXT("Blueprint has no SimpleConstructionScript"));
        return true;
    }

    // Find or create AIPerceptionComponent
    UAIPerceptionComponent* PerceptionComp = FindPerceptionComponentTemplate(ControllerBP);
    USCS_Node* PerceptionNode = nullptr;

    bool bCreatedNew = false;
    if (!PerceptionComp)
    {
        PerceptionNode = ControllerBP->SimpleConstructionScript->CreateNode(
            UAIPerceptionComponent::StaticClass(), TEXT("AIPerceptionComponent"));
        if (!PerceptionNode)
        {
            Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create perception component node"));
            return true;
        }
        ControllerBP->SimpleConstructionScript->AddNode(PerceptionNode);
        PerceptionComp = Cast<UAIPerceptionComponent>(PerceptionNode->ComponentTemplate);
        if (!PerceptionComp)
        {
            Ctx.SendError(TEXT("CAST_FAILED"), TEXT("Failed to cast perception component"));
            return true;
        }
        bCreatedNew = true;
    }

    if (!PerceptionComp)
    {
        Ctx.SendError(TEXT("NULL_COMPONENT"), TEXT("Perception component is null"));
        return true;
    }

    TArray<FString> SensesConfigured;

    // Configure sight sense
    bool bEnableSight = Ctx.GetBool(TEXT("enableSight"));
    if (bEnableSight)
    {
        float SightRadius = Ctx.GetNumber(TEXT("sightRadius"), 3000.0f);
        float LoseSightRadius = Ctx.GetNumber(TEXT("loseSightRadius"), SightRadius + 500.0f);
        float PeripheralVisionAngle = Ctx.GetNumber(TEXT("peripheralVisionAngle"), 90.0f);

        UAISenseConfig_Sight* SightConfig = NewObject<UAISenseConfig_Sight>(PerceptionComp);
        SightConfig->SightRadius = SightRadius;
        SightConfig->LoseSightRadius = LoseSightRadius;
        SightConfig->PeripheralVisionAngleDegrees = PeripheralVisionAngle;
        SightConfig->DetectionByAffiliation.bDetectEnemies = true;
        SightConfig->DetectionByAffiliation.bDetectNeutrals = true;
        SightConfig->DetectionByAffiliation.bDetectFriendlies = false;
        SightConfig->SetMaxAge(5.0f);

        PerceptionComp->ConfigureSense(*SightConfig);
        SensesConfigured.Add(TEXT("Sight"));
    }

    // Configure hearing sense
    bool bEnableHearing = Ctx.GetBool(TEXT("enableHearing"));
    if (bEnableHearing)
    {
        float HearingRange = Ctx.GetNumber(TEXT("hearingRange"), 3000.0f);

        UAISenseConfig_Hearing* HearingConfig = NewObject<UAISenseConfig_Hearing>(PerceptionComp);
        HearingConfig->HearingRange = HearingRange;
        HearingConfig->DetectionByAffiliation.bDetectEnemies = true;
        HearingConfig->DetectionByAffiliation.bDetectNeutrals = true;
        HearingConfig->DetectionByAffiliation.bDetectFriendlies = false;
        HearingConfig->SetMaxAge(5.0f);

        PerceptionComp->ConfigureSense(*HearingConfig);
        SensesConfigured.Add(TEXT("Hearing"));
    }

    // Configure damage sense
    bool bEnableDamage = Ctx.GetBool(TEXT("enableDamage"));
    if (bEnableDamage)
    {
        UAISenseConfig_Damage* DamageConfig = NewObject<UAISenseConfig_Damage>(PerceptionComp);
        DamageConfig->SetMaxAge(10.0f);

        PerceptionComp->ConfigureSense(*DamageConfig);
        SensesConfigured.Add(TEXT("Damage"));
    }

    // Set dominant sense if specified
    FString DominantSense = Ctx.GetString(TEXT("dominantSense"));
    if (!DominantSense.IsEmpty())
    {
        if (DominantSense.Equals(TEXT("Sight"), ESearchCase::IgnoreCase))
        {
            PerceptionComp->SetDominantSense(UAISense_Sight::StaticClass());
        }
        else if (DominantSense.Equals(TEXT("Hearing"), ESearchCase::IgnoreCase))
        {
            PerceptionComp->SetDominantSense(UAISense_Hearing::StaticClass());
        }
        else if (DominantSense.Equals(TEXT("Damage"), ESearchCase::IgnoreCase))
        {
            PerceptionComp->SetDominantSense(UAISense_Damage::StaticClass());
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ControllerBP);
    McpSafeAssetSave(ControllerBP);

    TSharedPtr<FJsonObject> PerceptionResult = MakeShareable(new FJsonObject());
    PerceptionResult->SetStringField(TEXT("controllerPath"), ControllerPath);
    PerceptionResult->SetBoolField(TEXT("createdNew"), bCreatedNew);

    TArray<TSharedPtr<FJsonValue>> SensesArray;
    for (const FString& Sense : SensesConfigured)
    {
        SensesArray.Add(MakeShareable(new FJsonValueString(Sense)));
    }
    PerceptionResult->SetArrayField(TEXT("sensesConfigured"), SensesArray);

    if (!DominantSense.IsEmpty())
    {
        PerceptionResult->SetStringField(TEXT("dominantSense"), DominantSense);
    }

    Ctx.SendSuccess(PerceptionResult);
    return true;
}

REGISTER_RPC_HANDLER("ai.create_nav_modifier", "ai",
    "Create a navigation modifier component on a blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_OPT("componentName", "string", "Name for the component"),
        RPC_PARAM_OPT("areaClass", "classref", "Nav area class (Default, Null, Obstacle)"),
        RPC_PARAM_OPT("failsafeToDefaultNavmesh", "boolean", "Use default nav area as failsafe")
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath"));
        return true;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    if (!Blueprint->SimpleConstructionScript)
    {
        Ctx.SendError(TEXT("INVALID_STATE"), TEXT("Blueprint has no SimpleConstructionScript"));
        return true;
    }

    FString ComponentName = Ctx.GetString(TEXT("componentName"));
    if (ComponentName.IsEmpty())
    {
        ComponentName = TEXT("NavModifierComponent");
    }

    USCS_Node* NavModNode = Blueprint->SimpleConstructionScript->CreateNode(
        UNavModifierComponent::StaticClass(), *ComponentName);
    if (!NavModNode)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create nav modifier node"));
        return true;
    }

    Blueprint->SimpleConstructionScript->AddNode(NavModNode);
    UNavModifierComponent* NavModComp = Cast<UNavModifierComponent>(NavModNode->ComponentTemplate);

    if (NavModComp)
    {
        // Configure fail-safe defaults
        bool bFailsafe = Ctx.GetBool(TEXT("failsafeToDefaultNavmesh"));
        NavModComp->SetAreaClass(bFailsafe ? UNavArea_Default::StaticClass() : UNavArea_Obstacle::StaticClass());

        // Set area class if specified
        FString AreaClassName = Ctx.GetString(TEXT("areaClass"));
        if (!AreaClassName.IsEmpty())
        {
            UClass* AreaClass = FindObject<UClass>(nullptr, *AreaClassName);
            if (!AreaClass)
            {
                // Try common area classes
                if (AreaClassName.Equals(TEXT("NavArea_Null"), ESearchCase::IgnoreCase) ||
                    AreaClassName.Equals(TEXT("Null"), ESearchCase::IgnoreCase))
                {
                    AreaClass = UNavArea_Null::StaticClass();
                }
                else if (AreaClassName.Equals(TEXT("NavArea_Obstacle"), ESearchCase::IgnoreCase) ||
                         AreaClassName.Equals(TEXT("Obstacle"), ESearchCase::IgnoreCase))
                {
                    AreaClass = UNavArea_Obstacle::StaticClass();
                }
                else if (AreaClassName.Equals(TEXT("NavArea_Default"), ESearchCase::IgnoreCase) ||
                         AreaClassName.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
                {
                    AreaClass = UNavArea_Default::StaticClass();
                }
            }

            if (AreaClass && AreaClass->IsChildOf(UNavArea::StaticClass()))
            {
                NavModComp->SetAreaClass(AreaClass);
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    McpSafeAssetSave(Blueprint);

    TSharedPtr<FJsonObject> NavModResult = MakeShareable(new FJsonObject());
    NavModResult->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    NavModResult->SetStringField(TEXT("componentName"), ComponentName);
    // Echo the nav area class that actually landed on the component template. The
    // default no-arg path applies NavArea_Obstacle (not "Default"), so read the
    // resolved class back off the component instead of hard-coding a literal --
    // mirrors navigation.create_nav_modifier_component's resolvedAreaClass echo.
    NavModResult->SetStringField(TEXT("areaClass"),
        (NavModComp && NavModComp->AreaClass) ? NavModComp->AreaClass->GetPathName() : FString());

    Ctx.SendSuccess(NavModResult);
    return true;
}

REGISTER_RPC_HANDLER("ai.set_ai_movement", "ai",
    "Configure AI movement parameters on a blueprint's CharacterMovementComponent",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the blueprint"),
        RPC_PARAM_OPT("maxWalkSpeed", "number", "Maximum walk speed"),
        RPC_PARAM_OPT("maxAcceleration", "number", "Maximum acceleration"),
        RPC_PARAM_OPT("brakingDeceleration", "number", "Braking deceleration when walking"),
        RPC_PARAM_OPT("rotationRate", "number", "Rotation rate (yaw degrees/sec)"),
        RPC_PARAM_OPT("useAccelerationForPaths", "boolean", "Use acceleration for path following"),
        RPC_PARAM_OPT("orientRotationToMovement", "boolean", "Orient rotation to movement direction"),
        RPC_PARAM_OPT("useRVOAvoidance", "boolean", "Use RVO avoidance"),
        RPC_PARAM_OPT("avoidanceWeight", "number", "Avoidance weight"),
        RPC_PARAM_OPT("maxFlySpeed", "number", "Maximum fly speed"),
        RPC_PARAM_OPT("jumpZVelocity", "number", "Jump Z velocity")
    ))
{
    FString BlueprintPath = Ctx.GetString(TEXT("blueprintPath"));
    if (BlueprintPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blueprintPath"));
        return true;
    }

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    if (!Blueprint->SimpleConstructionScript)
    {
        Ctx.SendError(TEXT("INVALID_STATE"), TEXT("Blueprint has no SimpleConstructionScript"));
        return true;
    }

    // Find CharacterMovementComponent
    UCharacterMovementComponent* MovementComp = nullptr;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->ComponentTemplate)
        {
            if (UCharacterMovementComponent* Comp = Cast<UCharacterMovementComponent>(Node->ComponentTemplate))
            {
                MovementComp = Comp;
                break;
            }
        }
    }

    if (!MovementComp)
    {
        // Check CDO for native component
        if (Blueprint->GeneratedClass)
        {
            if (AActor* CDO = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject()))
            {
                MovementComp = CDO->FindComponentByClass<UCharacterMovementComponent>();
            }
        }
    }

    if (!MovementComp)
    {
        Ctx.SendError(TEXT("COMPONENT_NOT_FOUND"), TEXT("No CharacterMovementComponent found in blueprint"));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    TArray<FString> PropertiesSet;

    // Walking speed
    float MaxWalkSpeed = Ctx.GetNumber(TEXT("maxWalkSpeed"), -1.0f);
    if (MaxWalkSpeed > 0.0f)
    {
        MovementComp->MaxWalkSpeed = MaxWalkSpeed;
        PropertiesSet.Add(TEXT("MaxWalkSpeed"));
    }

    // Max acceleration
    float MaxAcceleration = Ctx.GetNumber(TEXT("maxAcceleration"), -1.0f);
    if (MaxAcceleration > 0.0f)
    {
        MovementComp->MaxAcceleration = MaxAcceleration;
        PropertiesSet.Add(TEXT("MaxAcceleration"));
    }

    // Braking deceleration walking
    float BrakingDeceleration = Ctx.GetNumber(TEXT("brakingDeceleration"), -1.0f);
    if (BrakingDeceleration > 0.0f)
    {
        MovementComp->BrakingDecelerationWalking = BrakingDeceleration;
        PropertiesSet.Add(TEXT("BrakingDecelerationWalking"));
    }

    // Rotation rate
    float RotationRate = Ctx.GetNumber(TEXT("rotationRate"), -1.0f);
    if (RotationRate > 0.0f)
    {
        MovementComp->RotationRate = FRotator(0.0f, RotationRate, 0.0f);
        PropertiesSet.Add(TEXT("RotationRate"));
    }

    // Use acceleration for paths
    // UE 5.7+: bUseAccelerationForPaths was removed from UNavMovementComponent
    // Use bRequestedMoveUseAcceleration in UCharacterMovementComponent instead
    bool bUseAcceleration = Ctx.GetBool(TEXT("useAccelerationForPaths"));
    if (Payload->HasField(TEXT("useAccelerationForPaths")))
    {
        MovementComp->bRequestedMoveUseAcceleration = bUseAcceleration;
        PropertiesSet.Add(TEXT("bRequestedMoveUseAcceleration"));
    }

    // Orient rotation to movement
    bool bOrientToMovement = Ctx.GetBool(TEXT("orientRotationToMovement"));
    if (Payload->HasField(TEXT("orientRotationToMovement")))
    {
        MovementComp->bOrientRotationToMovement = bOrientToMovement;
        PropertiesSet.Add(TEXT("bOrientRotationToMovement"));
    }

    // Use RVO avoidance
    bool bUseRVOAvoidance = Ctx.GetBool(TEXT("useRVOAvoidance"));
    if (Payload->HasField(TEXT("useRVOAvoidance")))
    {
        MovementComp->bUseRVOAvoidance = bUseRVOAvoidance;
        PropertiesSet.Add(TEXT("bUseRVOAvoidance"));
    }

    // Avoidance weight
    float AvoidanceWeight = Ctx.GetNumber(TEXT("avoidanceWeight"), -1.0f);
    if (AvoidanceWeight >= 0.0f)
    {
        MovementComp->AvoidanceWeight = AvoidanceWeight;
        PropertiesSet.Add(TEXT("AvoidanceWeight"));
    }

    // Max fly speed (for flying AI)
    float MaxFlySpeed = Ctx.GetNumber(TEXT("maxFlySpeed"), -1.0f);
    if (MaxFlySpeed > 0.0f)
    {
        MovementComp->MaxFlySpeed = MaxFlySpeed;
        PropertiesSet.Add(TEXT("MaxFlySpeed"));
    }

    // Jump Z velocity
    float JumpZVelocity = Ctx.GetNumber(TEXT("jumpZVelocity"), -1.0f);
    if (JumpZVelocity > 0.0f)
    {
        MovementComp->JumpZVelocity = JumpZVelocity;
        PropertiesSet.Add(TEXT("JumpZVelocity"));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    McpSafeAssetSave(Blueprint);

    TSharedPtr<FJsonObject> MovementResult = MakeShareable(new FJsonObject());
    MovementResult->SetStringField(TEXT("blueprintPath"), BlueprintPath);

    TArray<TSharedPtr<FJsonValue>> PropsArray;
    for (const FString& Prop : PropertiesSet)
    {
        PropsArray.Add(MakeShareable(new FJsonValueString(Prop)));
    }
    MovementResult->SetArrayField(TEXT("propertiesSet"), PropsArray);
    MovementResult->SetNumberField(TEXT("propertyCount"), PropertiesSet.Num());

    // Include current values
    TSharedPtr<FJsonObject> CurrentValues = MakeShareable(new FJsonObject());
    CurrentValues->SetNumberField(TEXT("maxWalkSpeed"), MovementComp->MaxWalkSpeed);
    CurrentValues->SetNumberField(TEXT("maxAcceleration"), MovementComp->MaxAcceleration);
    CurrentValues->SetNumberField(TEXT("rotationRateYaw"), MovementComp->RotationRate.Yaw);
    CurrentValues->SetBoolField(TEXT("orientRotationToMovement"), MovementComp->bOrientRotationToMovement);
    CurrentValues->SetBoolField(TEXT("useRVOAvoidance"), MovementComp->bUseRVOAvoidance);
    MovementResult->SetObjectField(TEXT("currentValues"), CurrentValues);

    Ctx.SendSuccess(MovementResult);
    return true;
}

// =====================================================================
// Aliases & Convenience Actions
// =====================================================================

REGISTER_RPC_HANDLER("ai.create_blackboard", "ai",
    "Create a Blackboard asset (alias for create_blackboard_asset)",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the Blackboard"),
        RPC_PARAM_OPT("path", "path", "Content path for the asset")
    ))
{
    FString Name = Ctx.GetString(TEXT("name"));
    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing name"));
        return true;
    }

    FString Path = Ctx.GetString(TEXT("path"));
    if (Path.IsEmpty())
    {
        Path = TEXT("/Game/AI/Blackboards");
    }

    FString AssetPath = Path / Name;
    FString SanitizedPath, SanitizeError;
    if (!SanitizeAIAssetPath(AssetPath, SanitizedPath, SanitizeError))
    {
        Ctx.SendError(TEXT("INVALID_PATH"), SanitizeError);
        return true;
    }

    if (ResolveAsset(SanitizedPath).bExists)
    {
        TSharedPtr<FJsonObject> ExistResult = MakeShareable(new FJsonObject());
        ExistResult->SetStringField(TEXT("blackboardPath"), SanitizedPath);
        ExistResult->SetBoolField(TEXT("alreadyExisted"), true);
        Ctx.SendSuccess(ExistResult);
        return true;
    }

    UBlackboardData* NewBB = NewObject<UBlackboardData>(CreatePackage(*SanitizedPath), *FPaths::GetBaseFilename(SanitizedPath), RF_Public | RF_Standalone);
    if (!NewBB)
    {
        Ctx.SendError(TEXT("CREATION_FAILED"), TEXT("Failed to create blackboard data asset"));
        return true;
    }

    McpSafeAssetSave(NewBB);

    TSharedPtr<FJsonObject> BBResult = MakeShareable(new FJsonObject());
    BBResult->SetStringField(TEXT("blackboardPath"), SanitizedPath);
    BBResult->SetBoolField(TEXT("alreadyExisted"), false);
    Ctx.SendSuccess(BBResult);
    return true;
}

REGISTER_RPC_HANDLER("ai.set_blackboard_value", "ai",
    "Set a default value on a Blackboard key (UE 5.5+ for value setting)",
    RPC_PARAMS(
        RPC_PARAM_REQ("blackboardPath", "path", "Path to the Blackboard asset"),
        RPC_PARAM_REQ("keyName", "string", "Name of the key to set"),
        RPC_PARAM_OPT("value", "string", "Value to set (string representation)")
    ))
{
    FString BBPath = Ctx.GetString(TEXT("blackboardPath"));
    if (BBPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing blackboardPath"));
        return true;
    }

    FString KeyName = Ctx.GetString(TEXT("keyName"));
    if (KeyName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing keyName"));
        return true;
    }

    UBlackboardData* BBData = LoadObject<UBlackboardData>(nullptr, *BBPath);
    if (!BBData)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Blackboard not found: %s"), *BBPath));
        return true;
    }

    // Find the key and set its value
    bool bKeyFound = false;
    bool bValueSet = false;
    FString ValueStr = Ctx.GetString(TEXT("value"));

    for (FBlackboardEntry& Key : BBData->Keys)
    {
        if (Key.EntryName.ToString() == KeyName)
        {
            bKeyFound = true;

            // Set the default value based on key type
            // Note: DefaultValue properties on BlackboardKeyType are only available in UE 5.5+
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
            if (Key.KeyType && !ValueStr.IsEmpty())
            {
                if (UBlackboardKeyType_Bool* BoolKey = Cast<UBlackboardKeyType_Bool>(Key.KeyType))
                {
                    BoolKey->bDefaultValue = ValueStr.ToLower() == TEXT("true") || ValueStr == TEXT("1");
                    bValueSet = true;
                }
                else if (UBlackboardKeyType_Int* IntKey = Cast<UBlackboardKeyType_Int>(Key.KeyType))
                {
                    IntKey->DefaultValue = FCString::Atoi(*ValueStr);
                    bValueSet = true;
                }
                else if (UBlackboardKeyType_Float* FloatKey = Cast<UBlackboardKeyType_Float>(Key.KeyType))
                {
                    FloatKey->DefaultValue = FCString::Atof(*ValueStr);
                    bValueSet = true;
                }
                else if (UBlackboardKeyType_Vector* VectorKey = Cast<UBlackboardKeyType_Vector>(Key.KeyType))
                {
                    VectorKey->DefaultValue.InitFromString(ValueStr);
                    VectorKey->bUseDefaultValue = true;
                    bValueSet = true;
                }
                else if (UBlackboardKeyType_Rotator* RotatorKey = Cast<UBlackboardKeyType_Rotator>(Key.KeyType))
                {
                    RotatorKey->DefaultValue.InitFromString(ValueStr);
                    RotatorKey->bUseDefaultValue = true;
                    bValueSet = true;
                }
                else if (UBlackboardKeyType_Name* NameKey = Cast<UBlackboardKeyType_Name>(Key.KeyType))
                {
                    NameKey->DefaultValue = FName(*ValueStr);
                    bValueSet = true;
                }
                else if (UBlackboardKeyType_String* StringKey = Cast<UBlackboardKeyType_String>(Key.KeyType))
                {
                    StringKey->DefaultValue = ValueStr;
                    bValueSet = true;
                }
                else
                {
                    bValueSet = false;
                }
            }
#else
            // UE 5.0-5.4: DefaultValue properties not available on BlackboardKeyType
            bValueSet = false;
#endif
            break;
        }
    }

    if (!bKeyFound)
    {
        Ctx.SendError(TEXT("KEY_NOT_FOUND"),
            FString::Printf(TEXT("Key '%s' not found in blackboard"), *KeyName));
        return true;
    }

    McpSafeAssetSave(BBData);

    TSharedPtr<FJsonObject> SetResult = MakeShareable(new FJsonObject());
    SetResult->SetStringField(TEXT("blackboardPath"), BBPath);
    SetResult->SetStringField(TEXT("keyName"), KeyName);
    SetResult->SetStringField(TEXT("value"), ValueStr);
    SetResult->SetBoolField(TEXT("valueSet"), bValueSet);

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
    Ctx.SendSuccess(SetResult);
#else
    Ctx.SendSuccess(SetResult);
#endif
    return true;
}

REGISTER_RPC_HANDLER("ai.run_behavior_tree", "ai",
    "Assign a Behavior Tree to run on an AI Controller (alias for assign_behavior_tree)",
    RPC_PARAMS(
        RPC_PARAM_REQ("controllerPath", "path", "Path to the AI Controller blueprint"),
        RPC_PARAM_REQ("behaviorTreePath", "path", "Path to the Behavior Tree asset")
    ))
{
    FString ControllerPath = Ctx.GetString(TEXT("controllerPath"));
    if (ControllerPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing controllerPath"));
        return true;
    }

    FString BTPath = Ctx.GetString(TEXT("behaviorTreePath"));
    if (BTPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing behaviorTreePath"));
        return true;
    }

    UBlueprint* ControllerBP = LoadObject<UBlueprint>(nullptr, *ControllerPath);
    if (!ControllerBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Controller blueprint not found: %s"), *ControllerPath));
        return true;
    }

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *BTPath);
    if (!BT)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Behavior tree not found: %s"), *BTPath));
        return true;
    }

    TSharedPtr<FJsonObject> RunResult = MakeShareable(new FJsonObject());

    // This verb is documented as an alias for assign_behavior_tree, so wire the BT the
    // same way: write it onto the controller CDO's DefaultBehaviorTree property (the
    // property the engine reads to auto-run a tree), creating the member variable if no
    // native property exists. The previous implementation added an empty, valueless
    // "AssignedBehaviorTree" member variable, never set DefaultBehaviorTree, and
    // hard-coded assigned:true — a silent no-op that misreported success.
    bool bAssigned = false;
    FString PropertyName;
    BlueprintHandlerUtils::FBlueprintCompileDiagnostics CompileDiagnostics;
    bool bCompileAttempted = false;
    if (ControllerBP->GeneratedClass && Cast<AAIController>(ControllerBP->GeneratedClass->GetDefaultObject()))
    {
        bAssigned = AssignObjectDefaultToControllerCDO(
            ControllerBP, UBehaviorTree::StaticClass(), BT, TEXT("DefaultBehaviorTree"), PropertyName,
            CompileDiagnostics, bCompileAttempted);
        RunResult->SetStringField(TEXT("propertyName"), PropertyName);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ControllerBP);
    McpSafeAssetSave(ControllerBP);

    RunResult->SetStringField(TEXT("controllerPath"), ControllerPath);
    RunResult->SetStringField(TEXT("behaviorTreePath"), BTPath);
    // Report the real outcome (whether the CDO property was written), not a hard-coded true.
    RunResult->SetBoolField(TEXT("assigned"), bAssigned);
    if (bCompileAttempted)
    {
        BlueprintHandlerUtils::AddCompileDiagnosticsToJson(CompileDiagnostics, RunResult);
    }
    Ctx.SendSuccess(RunResult);
    return true;
}

REGISTER_RPC_HANDLER("ai.stop_behavior_tree", "ai",
    "Remove behavior tree assignment from an AI Controller",
    RPC_PARAMS(
        RPC_PARAM_REQ("controllerPath", "path", "Path to the AI Controller blueprint")
    ))
{
    FString ControllerPath = Ctx.GetString(TEXT("controllerPath"));
    if (ControllerPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing controllerPath"));
        return true;
    }

    UBlueprint* ControllerBP = LoadObject<UBlueprint>(nullptr, *ControllerPath);
    if (!ControllerBP)
    {
        Ctx.SendError(TEXT("NOT_FOUND"),
            FString::Printf(TEXT("Controller blueprint not found: %s"), *ControllerPath));
        return true;
    }

    TSharedPtr<FJsonObject> StopResult = MakeShareable(new FJsonObject());
    bool bCleared = false;
    FString ClearedPropertyName;

    // Null whatever UBehaviorTree* property assign_behavior_tree / run_behavior_tree set on
    // the CDO. Both verbs either set a discovered native UBehaviorTree* property or, when
    // none exists, create a member variable named "DefaultBehaviorTree". Mirror that
    // discovery loop here so stop actually undoes the assignment.
    if (ControllerBP->GeneratedClass)
    {
        if (UObject* CDO = ControllerBP->GeneratedClass->GetDefaultObject())
        {
            if (FObjectProperty* ObjProp = FindFirstObjectPropertyOfClass(ControllerBP->GeneratedClass, UBehaviorTree::StaticClass()))
            {
                void* ValuePtr = ObjProp->ContainerPtrToValuePtr<void>(CDO);
                if (ObjProp->GetObjectPropertyValue(ValuePtr) != nullptr)
                {
                    ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
                    bCleared = true;
                    ClearedPropertyName = ObjProp->GetName();
                }
            }
        }
    }

    if (bCleared)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(ControllerBP);
        McpSafeAssetSave(ControllerBP);
        StopResult->SetStringField(TEXT("clearedProperty"), ClearedPropertyName);
    }

    StopResult->SetStringField(TEXT("controllerPath"), ControllerPath);
    // Report honestly: stopped reflects whether an assigned behavior tree was actually cleared.
    StopResult->SetBoolField(TEXT("stopped"), bCleared);
    Ctx.SendSuccess(StopResult);
    return true;
}
