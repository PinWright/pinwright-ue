// Copyright (c) 2026 Alexander Penkin. MIT License.

// CharacterHandler.cpp - Migrated from PinWright_CharacterHandlers.cpp
// Character creation, movement configuration, and advanced movement systems

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Dom/JsonObject.h"
#include "Utils/AssetUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Factories/BlueprintFactory.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimBlueprint.h"
#include "EdGraphSchema_K2.h"

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------


static bool SavePackageHelperChar(UPackage* Package, UObject* Asset)
{
    if (!Package || !Asset) return false;
    McpSafeAssetSave(Asset);
    return true;
}

// Persist a Blueprint member variable's default value by writing the
// FBPVariableDescription.DefaultValue string (the engine import-text format).
// Returns false if the variable isn't found in NewVariables.
//
// Writing DefaultValue (rather than poking the CDO post-compile) is durable: the
// value survives recompiles, is what `blueprint.get` reports under "defaults",
// and is the same field FBlueprintEditorUtils::AddMemberVariable seeds. Callers
// pass the value already formatted for the variable's pin type (a plain number
// for float/int scalars; the ((Key=Value)) container format for maps).
static bool SetBPVarDefaultValue(UBlueprint* Blueprint, FName VarName, const FString& DefaultValue)
{
    if (!Blueprint) return false;

    for (FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        if (Var.VarName == VarName)
        {
            Var.DefaultValue = DefaultValue;
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            return true;
        }
    }

    UE_LOG(LogPinWrightSubsystem, Warning,
           TEXT("SetBPVarDefaultValue: variable '%s' not found in NewVariables."),
           *VarName.ToString());
    return false;
}

// Reads the existing DefaultValue of a named member variable, or an empty string
// if the variable has no default / isn't found. Used to accumulate map entries
// across repeated calls rather than overwriting.
static FString GetBPVarDefaultValue(UBlueprint* Blueprint, FName VarName)
{
    if (!Blueprint) return FString();
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        if (Var.VarName == VarName)
        {
            return Var.DefaultValue;
        }
    }
    return FString();
}

// Builds the engine import-text default-value string for a Map<Name, SoftObject>
// variable, upserting one Key->Value entry into the entries already present in
// ExistingDefault. The serialized format matches FMapProperty::ExportText_Internal
// — an outer "(...)" wrapping comma-separated "(Key, "Value")" pairs — so a
// post-compile read-back (and `blueprint.get` "defaults") reflects every mapping.
//
// Parsing the prior default with a property's own ImportText is awkward without a
// live container, so we parse the well-defined pair structure directly: split the
// outer parens into "(Key, Value)" groups, then split each on the first top-level
// comma. Keys are de-quoted FName tokens; re-inserting the same key replaces it.
static FString BuildNameSoftObjectMapDefault(const FString& ExistingDefault,
                                             const FString& Key, const FString& Value)
{
    // Preserve insertion order while allowing key replacement.
    TArray<FString> OrderedKeys;
    TMap<FString, FString> Entries;

    auto Unquote = [](const FString& In) -> FString
    {
        FString S = In.TrimStartAndEnd();
        if (S.Len() >= 2 && S.StartsWith(TEXT("\"")) && S.EndsWith(TEXT("\"")))
        {
            S = S.Mid(1, S.Len() - 2);
        }
        return S;
    };

    // Strip the single outer pair of parens, if present, to expose the pair list.
    FString Inner = ExistingDefault.TrimStartAndEnd();
    if (Inner.StartsWith(TEXT("(")) && Inner.EndsWith(TEXT(")")))
    {
        Inner = Inner.Mid(1, Inner.Len() - 2);
    }

    // Walk the pair list at depth 0, capturing each parenthesised "(Key, Value)".
    int32 Depth = 0;
    int32 GroupStart = INDEX_NONE;
    for (int32 i = 0; i < Inner.Len(); ++i)
    {
        const TCHAR C = Inner[i];
        if (C == TCHAR('('))
        {
            if (Depth == 0) GroupStart = i + 1;
            ++Depth;
        }
        else if (C == TCHAR(')'))
        {
            --Depth;
            if (Depth == 0 && GroupStart != INDEX_NONE)
            {
                const FString Group = Inner.Mid(GroupStart, i - GroupStart);
                // Split on the first top-level comma into key / value.
                int32 InnerDepth = 0;
                int32 CommaPos = INDEX_NONE;
                for (int32 j = 0; j < Group.Len(); ++j)
                {
                    const TCHAR G = Group[j];
                    if (G == TCHAR('(')) ++InnerDepth;
                    else if (G == TCHAR(')')) --InnerDepth;
                    else if (G == TCHAR(',') && InnerDepth == 0) { CommaPos = j; break; }
                }
                if (CommaPos != INDEX_NONE)
                {
                    const FString PrevKey = Unquote(Group.Left(CommaPos));
                    const FString PrevVal = Unquote(Group.Mid(CommaPos + 1));
                    if (!PrevKey.IsEmpty())
                    {
                        if (!Entries.Contains(PrevKey)) OrderedKeys.Add(PrevKey);
                        Entries.Add(PrevKey, PrevVal);
                    }
                }
                GroupStart = INDEX_NONE;
            }
        }
    }

    // Upsert the new entry.
    if (!Entries.Contains(Key)) OrderedKeys.Add(Key);
    Entries.Add(Key, Value);

    // Re-serialize in the engine's "(...)" / "(Key, "Value")" format.
    FString Out = TEXT("(");
    for (int32 i = 0; i < OrderedKeys.Num(); ++i)
    {
        if (i > 0) Out += TEXT(",");
        Out += FString::Printf(TEXT("(%s, \"%s\")"), *OrderedKeys[i], *Entries[OrderedKeys[i]]);
    }
    Out += TEXT(")");
    return Out;
}

static UBlueprint* CreateCharacterBlueprint(const FString& Path, const FString& Name, FString& OutError)
{
    FString FullPath = Path / Name;

    if (!IsValidAssetPath(FullPath))
    {
        OutError = FString::Printf(TEXT("Invalid asset path: '%s'. Path must start with '/', cannot contain '..' or '//'."), *FullPath);
        return nullptr;
    }

    if (ResolveAsset(FullPath).bExists)
    {
        OutError = FString::Printf(TEXT("Asset already exists at path: %s"), *FullPath);
        return nullptr;
    }

    UPackage* Package = CreatePackage(*FullPath);
    if (!Package)
    {
        OutError = FString::Printf(TEXT("Failed to create package: %s"), *FullPath);
        return nullptr;
    }

    UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
    Factory->ParentClass = ACharacter::StaticClass();

    UBlueprint* Blueprint = Cast<UBlueprint>(
        Factory->FactoryCreateNew(UBlueprint::StaticClass(), Package, FName(*Name),
                                  RF_Public | RF_Standalone, nullptr, GWarn));

    if (!Blueprint)
    {
        OutError = TEXT("Failed to create character blueprint");
        return nullptr;
    }

    FAssetRegistryModule::AssetCreated(Blueprint);
    Blueprint->MarkPackageDirty();
    return Blueprint;
}

static FVector GetVectorFromJsonChar(const TSharedPtr<FJsonObject>& Obj)
{
    if (!Obj.IsValid()) return FVector::ZeroVector;
    return FVector(
        GetJsonNumberField(Obj, TEXT("x"), 0.0),
        GetJsonNumberField(Obj, TEXT("y"), 0.0),
        GetJsonNumberField(Obj, TEXT("z"), 0.0)
    );
}

static FRotator GetRotatorFromJsonChar(const TSharedPtr<FJsonObject>& Obj)
{
    if (!Obj.IsValid()) return FRotator::ZeroRotator;
    return FRotator(
        GetJsonNumberField(Obj, TEXT("pitch"), 0.0),
        GetJsonNumberField(Obj, TEXT("yaw"), 0.0),
        GetJsonNumberField(Obj, TEXT("roll"), 0.0)
    );
}

namespace {
static bool AddBlueprintVariableChar(UBlueprint* Blueprint, const FString& VarName, const FEdGraphPinType& PinType, const FString& Category = TEXT(""))
{
    if (!Blueprint) return false;

    bool bSuccess = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VarName), PinType);

    if (bSuccess && !Category.IsEmpty())
    {
        FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, FName(*VarName), nullptr, FText::FromString(Category));
    }

    return bSuccess;
}
} // namespace

// ---------------------------------------------------------------------------
// character.configure_capsule_component
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.configure_capsule_component", "character", "Configure the capsule component on a character blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("capsuleRadius", "number", "Capsule radius (default 42)"),
        RPC_PARAM_OPT("capsuleHalfHeight", "number", "Capsule half height (default 96)")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    float CapsuleRadius = static_cast<float>(Ctx.GetNumber(TEXT("capsuleRadius"), 42.0));
    float CapsuleHalfHeight = static_cast<float>(Ctx.GetNumber(TEXT("capsuleHalfHeight"), 96.0));

    ACharacter* CharCDO = Blueprint->GeneratedClass
        ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject())
        : nullptr;

    if (CharCDO && CharCDO->GetCapsuleComponent())
    {
        CharCDO->GetCapsuleComponent()->SetCapsuleRadius(CapsuleRadius);
        CharCDO->GetCapsuleComponent()->SetCapsuleHalfHeight(CapsuleHalfHeight);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetNumberField(TEXT("capsuleRadius"), CapsuleRadius);
    Result->SetNumberField(TEXT("capsuleHalfHeight"), CapsuleHalfHeight);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// character.configure_mesh_component
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.configure_mesh_component", "character", "Configure the skeletal mesh component on a character blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("skeletalMeshPath", "path", "Path to skeletal mesh asset"),
        RPC_PARAM_OPT("animBlueprintPath", "path", "Path to animation blueprint"),
        RPC_PARAM_OPT("meshOffset", "object", "Mesh offset {x,y,z}"),
        RPC_PARAM_OPT("meshRotation", "object", "Mesh rotation {pitch,yaw,roll}")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    FString SkeletalMeshPath = Ctx.GetString(TEXT("skeletalMeshPath"));
    FString AnimBPPath = Ctx.GetString(TEXT("animBlueprintPath"));

    ACharacter* CharCDO = Blueprint->GeneratedClass
        ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject())
        : nullptr;

    if (CharCDO && CharCDO->GetMesh())
    {
        if (!SkeletalMeshPath.IsEmpty())
        {
            USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *SkeletalMeshPath);
            if (Mesh)
            {
                CharCDO->GetMesh()->SetSkeletalMesh(Mesh);
            }
        }

        if (!AnimBPPath.IsEmpty())
        {
            UAnimBlueprint* AnimBP = LoadObject<UAnimBlueprint>(nullptr, *AnimBPPath);
            if (AnimBP && AnimBP->GeneratedClass)
            {
                CharCDO->GetMesh()->SetAnimInstanceClass(AnimBP->GeneratedClass);
            }
        }

        TSharedPtr<FJsonObject> RawPayload = Ctx.GetRawPayload();
        const TSharedPtr<FJsonObject>* OffsetObj;
        if (RawPayload->TryGetObjectField(TEXT("meshOffset"), OffsetObj))
        {
            FVector Offset = GetVectorFromJsonChar(*OffsetObj);
            CharCDO->GetMesh()->SetRelativeLocation(Offset);
        }

        const TSharedPtr<FJsonObject>* RotObj;
        if (RawPayload->TryGetObjectField(TEXT("meshRotation"), RotObj))
        {
            FRotator Rotation = GetRotatorFromJsonChar(*RotObj);
            CharCDO->GetMesh()->SetRelativeRotation(Rotation);
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    if (!SkeletalMeshPath.IsEmpty()) Result->SetStringField(TEXT("skeletalMesh"), SkeletalMeshPath);
    if (!AnimBPPath.IsEmpty()) Result->SetStringField(TEXT("animBlueprint"), AnimBPPath);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// character.configure_camera_component
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.configure_camera_component", "character", "Configure spring arm and camera on a character blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("springArmLength", "number", "Spring arm target length (default 300)"),
        RPC_PARAM_OPT("cameraUsePawnControlRotation", "boolean", "Use pawn control rotation (default true)"),
        RPC_PARAM_OPT("springArmLagEnabled", "boolean", "Enable camera lag (default false)"),
        RPC_PARAM_OPT("springArmLagSpeed", "number", "Camera lag speed (default 10)")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    float SpringArmLength = static_cast<float>(Ctx.GetNumber(TEXT("springArmLength"), 300.0));
    bool UsePawnControlRotation = Ctx.GetBool(TEXT("cameraUsePawnControlRotation"), true);
    bool LagEnabled = Ctx.GetBool(TEXT("springArmLagEnabled"), false);
    float LagSpeed = static_cast<float>(Ctx.GetNumber(TEXT("springArmLagSpeed"), 10.0));

    bool bHasSpringArm = false;
    bool bHasCamera = false;

    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->ComponentTemplate)
        {
            if (Node->ComponentTemplate->IsA<USpringArmComponent>())
            {
                bHasSpringArm = true;
                USpringArmComponent* SpringArm = Cast<USpringArmComponent>(Node->ComponentTemplate);
                SpringArm->TargetArmLength = SpringArmLength;
                SpringArm->bUsePawnControlRotation = UsePawnControlRotation;
                SpringArm->bEnableCameraLag = LagEnabled;
                SpringArm->CameraLagSpeed = LagSpeed;
            }
            if (Node->ComponentTemplate->IsA<UCameraComponent>())
            {
                bHasCamera = true;
            }
        }
    }

    if (!bHasSpringArm)
    {
        USCS_Node* SpringArmNode = Blueprint->SimpleConstructionScript->CreateNode(
            USpringArmComponent::StaticClass(), FName(TEXT("CameraBoom")));
        if (SpringArmNode)
        {
            USpringArmComponent* SpringArm = Cast<USpringArmComponent>(SpringArmNode->ComponentTemplate);
            if (SpringArm)
            {
                SpringArm->TargetArmLength = SpringArmLength;
                SpringArm->bUsePawnControlRotation = UsePawnControlRotation;
                SpringArm->bEnableCameraLag = LagEnabled;
                SpringArm->CameraLagSpeed = LagSpeed;
            }
            Blueprint->SimpleConstructionScript->AddNode(SpringArmNode);

            USCS_Node* CameraNode = Blueprint->SimpleConstructionScript->CreateNode(
                UCameraComponent::StaticClass(), FName(TEXT("FollowCamera")));
            if (CameraNode)
            {
                CameraNode->SetParent(SpringArmNode);
                Blueprint->SimpleConstructionScript->AddNode(CameraNode);
            }
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetNumberField(TEXT("springArmLength"), SpringArmLength);
    Result->SetBoolField(TEXT("usePawnControlRotation"), UsePawnControlRotation);
    Result->SetBoolField(TEXT("lagEnabled"), LagEnabled);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// character.configure_movement_speeds
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.configure_movement_speeds", "character", "Configure various movement speeds on a character blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("walkSpeed", "number", "Sets MaxWalkSpeed (the single ground speed). Alias of runSpeed; pass only one. For a separate run/sprint speed use character.configure_sprint"),
        RPC_PARAM_OPT("runSpeed", "number", "Sets MaxWalkSpeed (alias of walkSpeed - same field). Passing both walkSpeed and runSpeed is rejected as ambiguous. For a separate run/sprint speed use character.configure_sprint"),
        RPC_PARAM_OPT("crouchSpeed", "number", "Max crouch speed"),
        RPC_PARAM_OPT("swimSpeed", "number", "Max swim speed"),
        RPC_PARAM_OPT("flySpeed", "number", "Max fly speed"),
        RPC_PARAM_OPT("acceleration", "number", "Max acceleration"),
        RPC_PARAM_OPT("deceleration", "number", "Braking deceleration walking"),
        RPC_PARAM_OPT("groundFriction", "number", "Ground friction")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    TSharedPtr<FJsonObject> RawPayload = Ctx.GetRawPayload();

    // walkSpeed and runSpeed are aliases for the single UCharacterMovementComponent::MaxWalkSpeed
    // field (UE has only one ground speed). Passing both is ambiguous - runSpeed would silently
    // clobber walkSpeed with no in-band signal - so reject it before mutating instead of dropping
    // one value (validate-before-mutate). For a genuinely distinct run/sprint speed, callers should
    // use character.configure_sprint (writes MaxCustomMovementSpeed).
    if (RawPayload->HasField(TEXT("walkSpeed")) && RawPayload->HasField(TEXT("runSpeed")))
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            TEXT("walkSpeed and runSpeed both set MaxWalkSpeed (they are aliases for the same field); ")
            TEXT("pass only one. For a separate run/sprint speed use character.configure_sprint."));
        return true;
    }

    // Resolve the single ground speed once: walkSpeed and runSpeed are aliases for
    // MaxWalkSpeed and the both-present case was already rejected above, so at most
    // one is present here.
    const bool bHasGroundSpeed = RawPayload->HasField(TEXT("walkSpeed")) || RawPayload->HasField(TEXT("runSpeed"));
    const double GroundSpeed = RawPayload->HasField(TEXT("runSpeed"))
        ? Ctx.GetNumber(TEXT("runSpeed"), 600.0)
        : Ctx.GetNumber(TEXT("walkSpeed"), 600.0);

    ACharacter* CharCDO = Blueprint->GeneratedClass
        ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject())
        : nullptr;

    if (CharCDO && CharCDO->GetCharacterMovement())
    {
        UCharacterMovementComponent* Movement = CharCDO->GetCharacterMovement();

        if (bHasGroundSpeed)
            Movement->MaxWalkSpeed = static_cast<float>(GroundSpeed);
        if (RawPayload->HasField(TEXT("crouchSpeed")))
            Movement->MaxWalkSpeedCrouched = static_cast<float>(Ctx.GetNumber(TEXT("crouchSpeed"), 300.0));
        if (RawPayload->HasField(TEXT("swimSpeed")))
            Movement->MaxSwimSpeed = static_cast<float>(Ctx.GetNumber(TEXT("swimSpeed"), 300.0));
        if (RawPayload->HasField(TEXT("flySpeed")))
            Movement->MaxFlySpeed = static_cast<float>(Ctx.GetNumber(TEXT("flySpeed"), 600.0));
        if (RawPayload->HasField(TEXT("acceleration")))
            Movement->MaxAcceleration = static_cast<float>(Ctx.GetNumber(TEXT("acceleration"), 2048.0));
        if (RawPayload->HasField(TEXT("deceleration")))
            Movement->BrakingDecelerationWalking = static_cast<float>(Ctx.GetNumber(TEXT("deceleration"), 2048.0));
        if (RawPayload->HasField(TEXT("groundFriction")))
            Movement->GroundFriction = static_cast<float>(Ctx.GetNumber(TEXT("groundFriction"), 8.0));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    // Echo the requested ground speed (walkSpeed == runSpeed -> MaxWalkSpeed) so the
    // caller has an in-band signal of what was written.
    if (bHasGroundSpeed)
        Result->SetNumberField(TEXT("walkSpeed"), GroundSpeed);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// character.configure_jump
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.configure_jump", "character", "Configure jump parameters on a character blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("jumpHeight", "number", "Jump Z velocity"),
        RPC_PARAM_OPT("airControl", "number", "Air control amount (0-1)"),
        RPC_PARAM_OPT("gravityScale", "number", "Gravity scale"),
        RPC_PARAM_OPT("fallingLateralFriction", "number", "Friction while falling"),
        RPC_PARAM_OPT("maxJumpCount", "integer", "Max number of jumps"),
        RPC_PARAM_OPT("jumpHoldTime", "number", "Max jump hold time")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    ACharacter* CharCDO = Blueprint->GeneratedClass
        ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject())
        : nullptr;

    TSharedPtr<FJsonObject> RawPayload = Ctx.GetRawPayload();
    if (CharCDO && CharCDO->GetCharacterMovement())
    {
        UCharacterMovementComponent* Movement = CharCDO->GetCharacterMovement();

        if (RawPayload->HasField(TEXT("jumpHeight")))
            Movement->JumpZVelocity = static_cast<float>(Ctx.GetNumber(TEXT("jumpHeight"), 600.0));
        if (RawPayload->HasField(TEXT("airControl")))
            Movement->AirControl = static_cast<float>(Ctx.GetNumber(TEXT("airControl"), 0.35));
        if (RawPayload->HasField(TEXT("gravityScale")))
            Movement->GravityScale = static_cast<float>(Ctx.GetNumber(TEXT("gravityScale"), 1.0));
        if (RawPayload->HasField(TEXT("fallingLateralFriction")))
            Movement->FallingLateralFriction = static_cast<float>(Ctx.GetNumber(TEXT("fallingLateralFriction"), 0.0));
        if (RawPayload->HasField(TEXT("maxJumpCount")))
            CharCDO->JumpMaxCount = static_cast<int32>(Ctx.GetNumber(TEXT("maxJumpCount"), 1));
        if (RawPayload->HasField(TEXT("jumpHoldTime")))
            CharCDO->JumpMaxHoldTime = static_cast<float>(Ctx.GetNumber(TEXT("jumpHoldTime"), 0.0));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// character.configure_rotation
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.configure_rotation", "character", "Configure rotation settings on a character blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("orientToMovement", "boolean", "Orient rotation to movement"),
        RPC_PARAM_OPT("useControllerRotationYaw", "boolean", "Use controller rotation yaw"),
        RPC_PARAM_OPT("useControllerRotationPitch", "boolean", "Use controller rotation pitch"),
        RPC_PARAM_OPT("useControllerRotationRoll", "boolean", "Use controller rotation roll"),
        RPC_PARAM_OPT("rotationRate", "number", "Rotation rate (yaw)")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    ACharacter* CharCDO = Blueprint->GeneratedClass
        ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject())
        : nullptr;

    TSharedPtr<FJsonObject> RawPayload = Ctx.GetRawPayload();
    if (CharCDO && CharCDO->GetCharacterMovement())
    {
        UCharacterMovementComponent* Movement = CharCDO->GetCharacterMovement();

        if (RawPayload->HasField(TEXT("orientToMovement")))
            Movement->bOrientRotationToMovement = Ctx.GetBool(TEXT("orientToMovement"), true);
        if (RawPayload->HasField(TEXT("useControllerRotationYaw")))
            CharCDO->bUseControllerRotationYaw = Ctx.GetBool(TEXT("useControllerRotationYaw"), false);
        if (RawPayload->HasField(TEXT("useControllerRotationPitch")))
            CharCDO->bUseControllerRotationPitch = Ctx.GetBool(TEXT("useControllerRotationPitch"), false);
        if (RawPayload->HasField(TEXT("useControllerRotationRoll")))
            CharCDO->bUseControllerRotationRoll = Ctx.GetBool(TEXT("useControllerRotationRoll"), false);
        if (RawPayload->HasField(TEXT("rotationRate")))
            Movement->RotationRate = FRotator(0.0, Ctx.GetNumber(TEXT("rotationRate"), 540.0), 0.0);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// character.add_custom_movement_mode
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.add_custom_movement_mode", "character", "Add a custom movement mode with state tracking variables",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("modeName", "string", "Name for the custom mode (default Custom)"),
        RPC_PARAM_OPT("modeId", "number", "Custom mode ID (default 0)"),
        RPC_PARAM_OPT("customSpeed", "number", "Movement speed for this mode (default 600)")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    FString ModeName = Ctx.GetString(TEXT("modeName"), TEXT("Custom"));
    int32 ModeId = Ctx.GetInt(TEXT("modeId"), 0);
    float CustomSpeed = static_cast<float>(Ctx.GetNumber(TEXT("customSpeed"), 600.0));

    FString StateVarName = FString::Printf(TEXT("bIsIn%sMode"), *ModeName);
    FEdGraphPinType BoolPinType;
    BoolPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    AddBlueprintVariableChar(Blueprint, StateVarName, BoolPinType, TEXT("Movement States"));

    FString ModeIdVarName = FString::Printf(TEXT("CustomModeId_%s"), *ModeName);
    FEdGraphPinType IntPinType;
    IntPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
    AddBlueprintVariableChar(Blueprint, ModeIdVarName, IntPinType, TEXT("Movement States"));

    FString SpeedVarName = FString::Printf(TEXT("%sSpeed"), *ModeName);
    FEdGraphPinType FloatPinType;
    FloatPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    AddBlueprintVariableChar(Blueprint, SpeedVarName, FloatPinType, TEXT("Movement States"));

    SetBPVarDefaultValue(Blueprint, FName(*ModeIdVarName), FString::FromInt(ModeId));
    SetBPVarDefaultValue(Blueprint, FName(*SpeedVarName), FString::SanitizeFloat(CustomSpeed));

    ACharacter* CharCDO = Blueprint->GeneratedClass
        ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject())
        : nullptr;

    if (CharCDO && CharCDO->GetCharacterMovement())
    {
        CharCDO->GetCharacterMovement()->MaxCustomMovementSpeed = CustomSpeed;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("modeName"), ModeName);
    Result->SetNumberField(TEXT("modeId"), ModeId);
    Result->SetStringField(TEXT("stateVariable"), StateVarName);
    Result->SetStringField(TEXT("speedVariable"), SpeedVarName);
    Result->SetNumberField(TEXT("customSpeed"), CustomSpeed);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// character.configure_nav_movement
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.configure_nav_movement", "character", "Configure navigation movement properties",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("navAgentRadius", "number", "Navigation agent radius"),
        RPC_PARAM_OPT("navAgentHeight", "number", "Navigation agent height"),
        RPC_PARAM_OPT("avoidanceEnabled", "boolean", "Enable RVO avoidance")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    ACharacter* CharCDO = Blueprint->GeneratedClass
        ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject())
        : nullptr;

    TSharedPtr<FJsonObject> RawPayload = Ctx.GetRawPayload();
    if (CharCDO && CharCDO->GetCharacterMovement())
    {
        UCharacterMovementComponent* Movement = CharCDO->GetCharacterMovement();

        if (RawPayload->HasField(TEXT("navAgentRadius")))
            Movement->NavAgentProps.AgentRadius = static_cast<float>(Ctx.GetNumber(TEXT("navAgentRadius"), 42.0));
        if (RawPayload->HasField(TEXT("navAgentHeight")))
            Movement->NavAgentProps.AgentHeight = static_cast<float>(Ctx.GetNumber(TEXT("navAgentHeight"), 192.0));
        if (RawPayload->HasField(TEXT("avoidanceEnabled")))
            Movement->bUseRVOAvoidance = Ctx.GetBool(TEXT("avoidanceEnabled"), false);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}
// ---------------------------------------------------------------------------
// character.map_surface_to_sound
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.map_surface_to_sound", "character", "Map a physical surface type to a footstep sound",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("surfaceType", "string", "Physical surface type name"),
        RPC_PARAM_OPT("footstepSoundPath", "path", "Path to footstep sound asset"),
        RPC_PARAM_OPT("footstepParticlePath", "path", "Path to footstep particle asset"),
        RPC_PARAM_OPT("footstepDecalPath", "path", "Path to footstep decal asset")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    FString SurfaceType = Ctx.GetString(TEXT("surfaceType"));
    FString SoundPath = Ctx.GetString(TEXT("footstepSoundPath"));
    FString ParticlePath = Ctx.GetString(TEXT("footstepParticlePath"));
    FString DecalPath = Ctx.GetString(TEXT("footstepDecalPath"));

    // A mapping needs both a surface key and a sound value; if either is missing
    // there is nothing to store, so reject up front rather than mutate the
    // blueprint and report a success that echoes inputs which were never recorded.
    if (SurfaceType.IsEmpty() || SoundPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_PARAMS"),
            TEXT("map_surface_to_sound requires both 'surfaceType' and 'footstepSoundPath' to record a mapping."));
        return true;
    }

    FEdGraphPinType MapPinType;
    MapPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
    MapPinType.ContainerType = EPinContainerType::Map;
    MapPinType.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_SoftObject;
    AddBlueprintVariableChar(Blueprint, TEXT("FootstepSoundMap"), MapPinType, TEXT("Footsteps"));

    // Persist the surface->sound association into the map variable's default value,
    // accumulating across repeated calls (was a silent no-op that dropped every
    // surfaceType key). SetBPVarDefaultValue marks the blueprint structurally
    // modified on success, so no separate mark is needed here.
    const FString Existing = GetBPVarDefaultValue(Blueprint, TEXT("FootstepSoundMap"));
    const FString NewDefault = BuildNameSoftObjectMapDefault(Existing, SurfaceType, SoundPath);
    if (!SetBPVarDefaultValue(Blueprint, TEXT("FootstepSoundMap"), NewDefault))
    {
        Ctx.SendError(TEXT("INTERNAL_ERROR"),
            TEXT("FootstepSoundMap variable was added but its default could not be written."));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("surfaceType"), SurfaceType);
    Result->SetStringField(TEXT("sound"), SoundPath);
    if (!ParticlePath.IsEmpty()) Result->SetStringField(TEXT("particle"), ParticlePath);
    if (!DecalPath.IsEmpty()) Result->SetStringField(TEXT("decal"), DecalPath);
    Result->SetStringField(TEXT("mapVariable"), TEXT("FootstepSoundMap"));
    Result->SetStringField(TEXT("mapDefault"), NewDefault);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// character.configure_footstep_fx
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.configure_footstep_fx", "character", "Configure footstep visual/audio effect scaling",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("volumeMultiplier", "number", "Sound volume multiplier (default 1.0)"),
        RPC_PARAM_OPT("particleScale", "number", "Particle effect scale (default 1.0)")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    float VolumeMultiplier = static_cast<float>(Ctx.GetNumber(TEXT("volumeMultiplier"), 1.0));
    float ParticleScale = static_cast<float>(Ctx.GetNumber(TEXT("particleScale"), 1.0));

    FEdGraphPinType FloatPinType;
    FloatPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    AddBlueprintVariableChar(Blueprint, TEXT("FootstepVolumeMultiplier"), FloatPinType, TEXT("Footsteps"));
    AddBlueprintVariableChar(Blueprint, TEXT("FootstepParticleScale"), FloatPinType, TEXT("Footsteps"));

    // Persist the scalar defaults so a read-back reflects them (was a silent no-op).
    // Each SetBPVarDefaultValue marks the blueprint structurally modified on
    // success, so no separate mark is needed here.
    SetBPVarDefaultValue(Blueprint, TEXT("FootstepVolumeMultiplier"), FString::SanitizeFloat(VolumeMultiplier));
    SetBPVarDefaultValue(Blueprint, TEXT("FootstepParticleScale"), FString::SanitizeFloat(ParticleScale));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetNumberField(TEXT("volumeMultiplier"), VolumeMultiplier);
    Result->SetNumberField(TEXT("particleScale"), ParticleScale);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// character.get_character_info
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.get_character_info", "character", "Get information about a character blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetStringField(TEXT("assetName"), Blueprint->GetName());

    ACharacter* CharCDO = Blueprint->GeneratedClass
        ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject())
        : nullptr;

    if (CharCDO)
    {
        if (CharCDO->GetCapsuleComponent())
        {
            Result->SetNumberField(TEXT("capsuleRadius"), CharCDO->GetCapsuleComponent()->GetUnscaledCapsuleRadius());
            Result->SetNumberField(TEXT("capsuleHalfHeight"), CharCDO->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight());
        }

        if (CharCDO->GetCharacterMovement())
        {
            UCharacterMovementComponent* Movement = CharCDO->GetCharacterMovement();
            Result->SetNumberField(TEXT("walkSpeed"), Movement->MaxWalkSpeed);
            Result->SetNumberField(TEXT("jumpZVelocity"), Movement->JumpZVelocity);
            Result->SetNumberField(TEXT("airControl"), Movement->AirControl);
            Result->SetBoolField(TEXT("orientToMovement"), Movement->bOrientRotationToMovement);
            Result->SetNumberField(TEXT("gravityScale"), Movement->GravityScale);
            Result->SetNumberField(TEXT("customMovementSpeed"), Movement->MaxCustomMovementSpeed);
            // Mirror the configure_movement_speeds groundFriction / deceleration
            // writes so they can be read back through this getter (reads
            // GroundFriction and BrakingDecelerationWalking, the fields that
            // setter writes).
            Result->SetNumberField(TEXT("groundFriction"), Movement->GroundFriction);
            Result->SetNumberField(TEXT("brakingDeceleration"), Movement->BrakingDecelerationWalking);
            // Mirror the configure_nav_movement writes on the same CDO movement
            // component so its nav-agent fields round-trip through this getter
            // (field names match the configure_nav_movement params; reads the
            // NavAgentProps members and bUseRVOAvoidance that setter writes).
            Result->SetNumberField(TEXT("navAgentRadius"), Movement->NavAgentProps.AgentRadius);
            Result->SetNumberField(TEXT("navAgentHeight"), Movement->NavAgentProps.AgentHeight);
            Result->SetBoolField(TEXT("avoidanceEnabled"), Movement->bUseRVOAvoidance);
        }

        Result->SetNumberField(TEXT("maxJumpCount"), CharCDO->JumpMaxCount);
        Result->SetBoolField(TEXT("useControllerRotationYaw"), CharCDO->bUseControllerRotationYaw);
    }

    bool bHasSpringArm = false;
    bool bHasCamera = false;
    for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
    {
        if (Node && Node->ComponentTemplate)
        {
            if (Node->ComponentTemplate->IsA<USpringArmComponent>()) bHasSpringArm = true;
            if (Node->ComponentTemplate->IsA<UCameraComponent>()) bHasCamera = true;
        }
    }
    Result->SetBoolField(TEXT("hasSpringArm"), bHasSpringArm);
    Result->SetBoolField(TEXT("hasCamera"), bHasCamera);

    TArray<TSharedPtr<FJsonValue>> MovementVars;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        FString VarName = Var.VarName.ToString();
        if (VarName.StartsWith(TEXT("bIs")) || VarName.StartsWith(TEXT("bCan")) ||
            VarName.Contains(TEXT("Speed")) || VarName.Contains(TEXT("Movement")) ||
            // Footstep* variables persisted by configure_footstep_fx /
            // map_surface_to_sound (e.g. FootstepVolumeMultiplier,
            // FootstepParticleScale, FootstepSoundMap) so their writes
            // surface in this read-back instead of needing blueprint.inspect.
            VarName.StartsWith(TEXT("Footstep")))
        {
            MovementVars.Add(MakeShared<FJsonValueString>(VarName));
        }
    }
    if (MovementVars.Num() > 0)
    {
        Result->SetArrayField(TEXT("movementVariables"), MovementVars);
    }

    Ctx.SendSuccess(Result);
    return true;
}
// ---------------------------------------------------------------------------
// character.configure_crouch
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.configure_crouch", "character", "Configure crouch parameters on a character blueprint",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("crouchSpeed", "number", "Max crouch speed (default 300)"),
        RPC_PARAM_OPT("crouchedHalfHeight", "number", "Crouched capsule half height (default 44)"),
        RPC_PARAM_OPT("canCrouch", "boolean", "Whether character can crouch (default true)")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    double CrouchSpeed = Ctx.GetNumber(TEXT("crouchSpeed"), 300.0);
    double CrouchedHalfHeight = Ctx.GetNumber(TEXT("crouchedHalfHeight"), 44.0);
    bool CanCrouch = Ctx.GetBool(TEXT("canCrouch"), true);

    ACharacter* CharCDO = Blueprint->GeneratedClass
        ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject())
        : nullptr;

    if (CharCDO && CharCDO->GetCharacterMovement())
    {
        UCharacterMovementComponent* Movement = CharCDO->GetCharacterMovement();
        Movement->MaxWalkSpeedCrouched = static_cast<float>(CrouchSpeed);
        Movement->SetCrouchedHalfHeight(static_cast<float>(CrouchedHalfHeight));
        Movement->NavAgentProps.bCanCrouch = CanCrouch;
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetNumberField(TEXT("crouchSpeed"), CrouchSpeed);
    Result->SetNumberField(TEXT("crouchedHalfHeight"), CrouchedHalfHeight);
    Result->SetBoolField(TEXT("canCrouch"), CanCrouch);
    Ctx.SendSuccess(Result);
    return true;
}

// ---------------------------------------------------------------------------
// character.configure_sprint
// ---------------------------------------------------------------------------
REGISTER_RPC_HANDLER("character.configure_sprint", "character", "Configure sprint parameters with state variables",
    RPC_PARAMS(
        RPC_PARAM_REQ("blueprintPath", "path", "Path to the character blueprint"),
        RPC_PARAM_OPT("sprintSpeed", "number", "Sprint speed (default 900)")
    ))
{
    FString BlueprintPath;
    if (!Ctx.RequireString(TEXT("blueprintPath"), BlueprintPath)) return true;

    UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintPath));
        return true;
    }

    double SprintSpeed = Ctx.GetNumber(TEXT("sprintSpeed"), 900.0);

    FEdGraphPinType BoolPinType;
    BoolPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    AddBlueprintVariableChar(Blueprint, TEXT("bIsSprinting"), BoolPinType, TEXT("Sprint"));

    FEdGraphPinType FloatPinType;
    FloatPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
    FloatPinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
    AddBlueprintVariableChar(Blueprint, TEXT("SprintSpeed"), FloatPinType, TEXT("Sprint"));

    // Persist the SprintSpeed default so the created "Sprint Speed" variable holds
    // the configured speed (mirrors add_custom_movement_mode's <Mode>Speed write);
    // without it the variable keeps its float-zero default while the CDO carries 900.
    SetBPVarDefaultValue(Blueprint, TEXT("SprintSpeed"), FString::SanitizeFloat(static_cast<float>(SprintSpeed)));

    ACharacter* CharCDO = Blueprint->GeneratedClass
        ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject())
        : nullptr;

    if (CharCDO && CharCDO->GetCharacterMovement())
    {
        CharCDO->GetCharacterMovement()->MaxCustomMovementSpeed = static_cast<float>(SprintSpeed);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprintPath"), BlueprintPath);
    Result->SetNumberField(TEXT("sprintSpeed"), SprintSpeed);
    Result->SetStringField(TEXT("stateVariable"), TEXT("bIsSprinting"));
    Ctx.SendSuccess(Result);
    return true;
}
