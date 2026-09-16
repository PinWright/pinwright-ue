// Copyright (c) 2026 Alexander Penkin. MIT License.

#if defined(__has_include) && __has_include("ChaosVehicleMovementComponent.h")

#include "ChaosVehicleMovementComponent.h"
#include "ChaosVehicleWheel.h"
#include "ChaosWheeledVehicleMovementComponent.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include "PinWrightHelpers.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Utils/AssetUtils.h"
#include "Utils/ComponentPathUtils.h"
#include "Utils/PathUtils.h"
#include "Utils/PropertyUtils.h"
#include "Compat/JsonKeyCompat.h"

namespace
{
    // Apply every supplied JSON property to the target object's container via
    // reflection. Keys absent from Payload are untouched. Unknown keys (no
    // matching FProperty by friendly name) silently skipped — the per-payload
    // knob set is open-ended and not every Chaos wheel field is exposed.
    void ApplyJsonPropertiesByName(
        UObject* Owner,
        void* Container,
        UStruct* Struct,
        const TSharedPtr<FJsonObject>& Payload,
        const TArray<TPair<FString, FString>>& NameMap, // JSON key -> UProperty name
        TArray<FString>& OutWarnings)
    {
        if (!Owner || !Container || !Struct || !Payload.IsValid())
        {
            return;
        }
        for (const TPair<FString, FString>& Pair : NameMap)
        {
            const FString& JsonKey = Pair.Key;
            const FString& PropName = Pair.Value;
            const TSharedPtr<FJsonValue>* Found = Payload->Values.Find(EARGCompat::JsonFieldKey(JsonKey));
            if (!Found || !Found->IsValid() || (*Found)->Type == EJson::Null) continue;

            FProperty* Prop = Struct->FindPropertyByName(FName(*PropName));
            if (!Prop)
            {
                OutWarnings.Add(FString::Printf(TEXT("Property not found: %s"), *PropName));
                continue;
            }
            FString ApplyError;
            if (!ApplyJsonValueToProperty(Container, Prop, *Found, ApplyError))
            {
                OutWarnings.Add(FString::Printf(TEXT("Apply failed for %s: %s"), *PropName, *ApplyError));
            }
        }
    }

    const TArray<TPair<FString, FString>>& WheelPropertyNameMap()
    {
        static const TArray<TPair<FString, FString>> Map =
        {
            { TEXT("radius"),                   TEXT("WheelRadius") },
            { TEXT("width"),                    TEXT("WheelWidth") },
            { TEXT("mass"),                     TEXT("WheelMass") },
            { TEXT("frictionMultiplier"),       TEXT("FrictionForceMultiplier") },
            { TEXT("maxSteerAngle"),            TEXT("MaxSteerAngle") },
            { TEXT("maxBrakeTorque"),           TEXT("MaxBrakeTorque") },
            { TEXT("maxHandbrakeTorque"),       TEXT("MaxHandBrakeTorque") },
            { TEXT("suspensionMaxRaise"),       TEXT("SuspensionMaxRaise") },
            { TEXT("suspensionMaxDrop"),        TEXT("SuspensionMaxDrop") },
            { TEXT("springRate"),               TEXT("SpringRate") },
            { TEXT("springPreload"),            TEXT("SpringPreload") },
            { TEXT("dampingRatio"),             TEXT("SuspensionDampingRatio") },
            { TEXT("bAffectedByBrake"),         TEXT("bAffectedByBrake") },
            { TEXT("bAffectedByHandbrake"),     TEXT("bAffectedByHandbrake") },
            { TEXT("bAffectedBySteering"),      TEXT("bAffectedBySteering") },
            { TEXT("bAffectedByEngine"),        TEXT("bAffectedByEngine") },
            { TEXT("bABSEnabled"),              TEXT("bABSEnabled") },
            { TEXT("bTractionControlEnabled"),  TEXT("bTractionControlEnabled") },
        };
        return Map;
    }

    // Suspension subset of the wheel property map (used by vehicle.set_suspension).
    const TArray<TPair<FString, FString>>& SuspensionPropertyNameMap()
    {
        static const TArray<TPair<FString, FString>> Map =
        {
            { TEXT("suspensionMaxRaise"),   TEXT("SuspensionMaxRaise") },
            { TEXT("suspensionMaxDrop"),    TEXT("SuspensionMaxDrop") },
            { TEXT("springRate"),           TEXT("SpringRate") },
            { TEXT("springPreload"),        TEXT("SpringPreload") },
            { TEXT("dampingRatio"),         TEXT("SuspensionDampingRatio") },
            { TEXT("suspensionForceOffset"),TEXT("SuspensionForceOffset") },
        };
        return Map;
    }

    UChaosWheeledVehicleMovementComponent* ResolveWheeledComp(
        const FString& ComponentPath, FString& OutError)
    {
        UActorComponent* Comp = PinWrightRpc::ComponentPath::Resolve(ComponentPath, OutError);
        if (!Comp) return nullptr;
        UChaosWheeledVehicleMovementComponent* Wheeled = Cast<UChaosWheeledVehicleMovementComponent>(Comp);
        if (!Wheeled)
        {
            OutError = TEXT("NOT_A_WHEELED_VEHICLE_MOVEMENT");
            return nullptr;
        }
        return Wheeled;
    }

    // Find the owning Blueprint when Obj is a CDO subobject. For component instances
    // on a live actor (PIE/editor world) returns nullptr — the actor's Modify() in
    // the caller is sufficient.
    UBlueprint* FindOwningBlueprint(UObject* Obj)
    {
        if (!Obj) return nullptr;
        // CDO subobject case: GetTypedOuter<UClass> walks to the owning generated class.
        if (UBlueprint* BP = UBlueprint::GetBlueprintFromClass(Obj->GetTypedOuter<UClass>()))
        {
            return BP;
        }
        // GetOuter() may also be the CDO directly; in that case the class is one hop up.
        if (UObject* Outer = Obj->GetOuter())
        {
            if (UBlueprint* BP = UBlueprint::GetBlueprintFromClass(Outer->GetClass()))
            {
                return BP;
            }
        }
        return nullptr;
    }

    void MergeCompileDiagnostics(
        BlueprintHandlerUtils::FBlueprintCompileDiagnostics& Aggregate,
        const BlueprintHandlerUtils::FBlueprintCompileDiagnostics& Next,
        bool bFirst)
    {
        if (bFirst)
        {
            Aggregate = Next;
            return;
        }

        Aggregate.bCompiled = Aggregate.bCompiled && Next.bCompiled;
        Aggregate.Errors.Append(Next.Errors);
        Aggregate.Warnings.Append(Next.Warnings);
        Aggregate.Status = Aggregate.bCompiled
            ? (Aggregate.Warnings.IsEmpty() ? TEXT("UpToDate") : TEXT("UpToDateWithWarnings"))
            : TEXT("Error");

        Aggregate.Reinstanced.InstanceCount += Next.Reinstanced.InstanceCount;
        Aggregate.Reinstanced.ActorCount += Next.Reinstanced.ActorCount;
        Aggregate.Reinstanced.bPieActive |= Next.Reinstanced.bPieActive;
        for (const BlueprintReinstancingGuard::FWorldInstanceCount& NextWorld : Next.Reinstanced.Worlds)
        {
            BlueprintReinstancingGuard::FWorldInstanceCount* Existing =
                Aggregate.Reinstanced.Worlds.FindByPredicate(
                    [&NextWorld](const BlueprintReinstancingGuard::FWorldInstanceCount& Candidate)
                    {
                        return Candidate.WorldName == NextWorld.WorldName
                            && Candidate.WorldType == NextWorld.WorldType;
                    });
            if (Existing)
            {
                Existing->InstanceCount += NextWorld.InstanceCount;
                Existing->ActorCount += NextWorld.ActorCount;
            }
            else
            {
                Aggregate.Reinstanced.Worlds.Add(NextWorld);
            }
        }
    }

    // Resolve a TSubclassOf<UChaosVehicleWheel> from a JSON asset path string.
    // Accepts "/Game/.../BP_Wheel" (Blueprint package), "/Game/.../BP_Wheel.BP_Wheel_C"
    // (generated class), or "/Game/.../BP_Wheel.BP_Wheel" (Blueprint object) forms.
    UClass* LoadWheelClassFromPath(const FString& AssetPath)
    {
        if (AssetPath.IsEmpty()) return nullptr;

        // First try as a UClass directly.
        if (UClass* AsClass = LoadObject<UClass>(nullptr, *AssetPath))
        {
            if (AsClass->IsChildOf(UChaosVehicleWheel::StaticClass()))
            {
                return AsClass;
            }
            return nullptr;
        }
        // Fall back to UBlueprint -> GeneratedClass.
        if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath))
        {
            if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(UChaosVehicleWheel::StaticClass()))
            {
                return BP->GeneratedClass;
            }
        }
        return nullptr;
    }
}

REGISTER_RPC_HANDLER("vehicle.create_wheel_asset", "vehicle",
    "Create a new UChaosVehicleWheel Blueprint asset. Optional wheel-property knobs are written to the generated CDO before save. Defers engine/transmission/steering curves to vehicle.* curve handlers (sibling ticket).",
    RPC_PARAMS(
        RPC_PARAM_REQ("path", "path", "Project-relative asset path (e.g. /Game/Vehicles/BP_StockWheel)."),
        RPC_PARAM_OPT("parentClass", "classref", "Parent class path; must derive from ChaosVehicleWheel. Defaults to ChaosVehicleWheel itself."),
        RPC_PARAM_OPT("radius", "number", "WheelRadius (cm)."),
        RPC_PARAM_OPT("width", "number", "WheelWidth (cm)."),
        RPC_PARAM_OPT("mass", "number", "WheelMass (kg)."),
        RPC_PARAM_OPT("frictionMultiplier", "number", "FrictionForceMultiplier."),
        RPC_PARAM_OPT("maxSteerAngle", "number", "MaxSteerAngle (deg)."),
        RPC_PARAM_OPT("maxBrakeTorque", "number", "MaxBrakeTorque (Nm)."),
        RPC_PARAM_OPT("maxHandbrakeTorque", "number", "MaxHandBrakeTorque (Nm)."),
        RPC_PARAM_OPT("suspensionMaxRaise", "number", "SuspensionMaxRaise (cm)."),
        RPC_PARAM_OPT("suspensionMaxDrop", "number", "SuspensionMaxDrop (cm)."),
        RPC_PARAM_OPT("springRate", "number", "SpringRate (N/m)."),
        RPC_PARAM_OPT("springPreload", "number", "SpringPreload (N/m)."),
        RPC_PARAM_OPT("dampingRatio", "number", "SuspensionDampingRatio."),
        RPC_PARAM_OPT("bAffectedByBrake", "bool", "Whether the wheel responds to brake torque."),
        RPC_PARAM_OPT("bAffectedByHandbrake", "bool", "Whether the wheel responds to handbrake torque."),
        RPC_PARAM_OPT("bAffectedBySteering", "bool", "Whether the wheel turns with steering input."),
        RPC_PARAM_OPT("bAffectedByEngine", "bool", "Whether the wheel receives engine torque (drive wheel)."),
        RPC_PARAM_OPT("bABSEnabled", "bool", "Enable ABS (anti-lock braking) for this wheel."),
        RPC_PARAM_OPT("bTractionControlEnabled", "bool", "Enable traction control for this wheel."),
        RPC_PARAM_DEF("save", "bool", "Save the asset to disk after creation.", "true")
    ))
{
    FString Path;
    if (!Ctx.RequireString(TEXT("path"), Path)) return true;

    FString SanitizedPath = SanitizeProjectRelativePath(Path);
    if (SanitizedPath.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_SECURITY_VIOLATION,
            TEXT("Invalid path: path traversal or invalid characters detected"));
        return true;
    }
    if (!IsValidMountPoint(SanitizedPath))
    {
        // `TEXT("/Game") / P`, never `TEXT("/Game/") + P`. Concatenation MANUFACTURES a "//"
        // whenever P already carries a leading slash, and that is what CreatePackage logs Fatal
        // on. The re-check on the next line would NOT catch it - IsValidMountPoint answers the
        // mount question and a duplicate separator does not change which mount a path names - so
        // composing correctly is the only thing standing here.
        SanitizedPath = FString(TEXT("/Game")) / SanitizedPath;
        if (!IsValidMountPoint(SanitizedPath))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, TEXT("Path is not under a valid content mount point."));
            return true;
        }
    }

    UClass* ParentClass = UChaosVehicleWheel::StaticClass();
    const FString ParentClassStr = Ctx.GetString(TEXT("parentClass"));
    if (!ParentClassStr.IsEmpty())
    {
        UClass* Resolved = LoadObject<UClass>(nullptr, *ParentClassStr);
        if (!Resolved)
        {
            // Allow short-name resolver as a fallback.
            Resolved = ResolveUClass(ParentClassStr);
        }
        if (!Resolved || !Resolved->IsChildOf(UChaosVehicleWheel::StaticClass()))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PARENT_CLASS,
                FString::Printf(TEXT("parentClass '%s' does not derive from ChaosVehicleWheel"), *ParentClassStr));
            return true;
        }
        ParentClass = Resolved;
    }

    const FString AssetName = FPackageName::GetLongPackageAssetName(SanitizedPath);
    if (AssetName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, TEXT("Could not derive asset name from path."));
        return true;
    }

    UPackage* Package = CreatePackage(*SanitizedPath);
    if (!Package)
    {
        Ctx.SendError(ErrorCodes::ERR_PACKAGE_ERROR, TEXT("Failed to create package"));
        return true;
    }

    UBlueprint* NewBP = Cast<UBlueprint>(FKismetEditorUtilities::CreateBlueprint(
        ParentClass,
        Package,
        FName(*AssetName),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        FName(TEXT("MCP_CreateWheel"))));

    if (!NewBP || !NewBP->GeneratedClass)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATION_ERROR, TEXT("Failed to create wheel Blueprint"));
        return true;
    }

    UChaosVehicleWheel* CDO = Cast<UChaosVehicleWheel>(NewBP->GeneratedClass->GetDefaultObject());
    TArray<FString> Warnings;
    if (CDO)
    {
        ApplyJsonPropertiesByName(CDO, CDO, CDO->GetClass(),
            Ctx.GetRawPayload(), WheelPropertyNameMap(), Warnings);
    }
    else
    {
        Warnings.Add(TEXT("Generated class CDO was not a ChaosVehicleWheel; properties not applied."));
    }

    FAssetRegistryModule::AssetCreated(NewBP);
    NewBP->MarkPackageDirty();
    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(NewBP);

    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    if (bSave)
    {
        McpSafeAssetSave(NewBP);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("assetPath"), NewBP->GetPathName());
    Result->SetStringField(TEXT("className"), NewBP->GeneratedClass->GetName());
    Result->SetStringField(TEXT("parentClass"), ParentClass->GetName());
    if (Warnings.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> WarnArr;
        for (const FString& W : Warnings)
        {
            WarnArr.Add(MakeShared<FJsonValueString>(W));
        }
        Result->SetArrayField(TEXT("warnings"), WarnArr);
    }
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(
        Diagnostics, Result, TEXT("compileErrors"), TEXT("compileWarnings"));
    AddAssetVerification(Result, NewBP);

    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("vehicle.set_wheel_asset_property", "vehicle",
    "Write a single property on a wheel-asset CDO via reflection. Use this for fast-path tuning sweeps without re-passing the full property bag.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Wheel Blueprint asset path."),
        RPC_PARAM_REQ("propertyName", "string", "UPROPERTY name on UChaosVehicleWheel (e.g. WheelRadius, MaxBrakeTorque)."),
        RPC_PARAM_REQ("value", "any", "JSON value to write. Type is coerced from the FProperty shape."),
        RPC_PARAM_DEF("save", "bool", "Save asset to disk after edit.", "true")
    ))
{
    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;
    FString PropName;
    if (!Ctx.RequireString(TEXT("propertyName"), PropName)) return true;

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const TSharedPtr<FJsonValue>* ValueField = Payload.IsValid() ? Payload->Values.Find(TEXT("value")) : nullptr;
    if (!ValueField || !ValueField->IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, TEXT("Missing required parameter: value"));
        return true;
    }

    UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
    if (!BP || !BP->GeneratedClass)
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND,
            FString::Printf(TEXT("Wheel asset not found: %s"), *AssetPath));
        return true;
    }
    UChaosVehicleWheel* CDO = Cast<UChaosVehicleWheel>(BP->GeneratedClass->GetDefaultObject());
    if (!CDO)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_A_WHEEL_ASSET,
            TEXT("Asset does not derive from UChaosVehicleWheel"));
        return true;
    }

    FProperty* Prop = CDO->GetClass()->FindPropertyByName(FName(*PropName));
    if (!Prop)
    {
        Ctx.SendError(ErrorCodes::ERR_PROPERTY_NOT_FOUND,
            FString::Printf(TEXT("Property not found on wheel: %s"), *PropName));
        return true;
    }

    FString ApplyError;
    if (!ApplyJsonValueToProperty(CDO, Prop, *ValueField, ApplyError))
    {
        Ctx.SendError(ErrorCodes::ERR_APPLY_FAILED, ApplyError);
        return true;
    }

    BP->MarkPackageDirty();
    const BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics =
        BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(BP);

    if (Ctx.GetBool(TEXT("save"), true))
    {
        McpSafeAssetSave(BP);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("assetPath"), BP->GetPathName());
    Result->SetStringField(TEXT("propertyName"), PropName);
    BlueprintHandlerUtils::AddCompileDiagnosticsToJson(Diagnostics, Result);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("vehicle.set_wheel_setup", "vehicle",
    "Mutate or append a FChaosWheelSetup entry on a UChaosWheeledVehicleMovementComponent. Setting wheelIndex == WheelSetups.Num() appends a default entry; > Num() returns INVALID_INDEX.",
    RPC_PARAMS(
        RPC_PARAM_REQ("componentPath", "path", "Path to the UChaosWheeledVehicleMovementComponent that hosts the wheels, NOT a wheel asset. BP CDO subobject path (\"/Game/Vehicles/BP_Car.BP_Car_C:VehicleMovement\"; the \"BP_Car:VehicleMovement\" and \".Default__BP_Car_C:VehicleMovement\" forms resolve to the same component) or live actor component path (\"ActorLabel:VehicleMovement\")."),
        RPC_PARAM_REQ("wheelIndex", "integer", "Index into WheelSetups[]. Append-by-one semantics."),
        RPC_PARAM_OPT("wheelClass", "classref", "Wheel asset path (Blueprint or UClass)."),
        RPC_PARAM_OPT("boneName", "string", "Skeletal-mesh bone to bind the wheel to."),
        RPC_PARAM_OPT("additionalOffset", "object", "FVector offset from bone."),
        RPC_PARAM_OPT("compile", "bool", "Compile owning Blueprint after edit."),
        RPC_PARAM_OPT("save", "bool", "Save owning Blueprint to disk.")
    ))
{
    FString ComponentPath;
    if (!Ctx.RequireString(TEXT("componentPath"), ComponentPath)) return true;
    int32 WheelIndex = -1;
    if (!Ctx.RequireInt(TEXT("wheelIndex"), WheelIndex)) return true;
    if (WheelIndex < 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX, TEXT("wheelIndex must be >= 0"));
        return true;
    }

    FString ResolveError;
    UChaosWheeledVehicleMovementComponent* Comp = ResolveWheeledComp(ComponentPath, ResolveError);
    if (!Comp)
    {
        Ctx.SendError(ResolveError.IsEmpty() ? TEXT("COMPONENT_NOT_FOUND") : *ResolveError,
            FString::Printf(TEXT("Failed to resolve component: %s"), *ComponentPath));
        return true;
    }

    if (WheelIndex > Comp->WheelSetups.Num())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX,
            FString::Printf(TEXT("wheelIndex %d exceeds WheelSetups.Num() %d (append allowed only at Num())"),
                WheelIndex, Comp->WheelSetups.Num()));
        return true;
    }
    Comp->Modify();
    if (WheelIndex == Comp->WheelSetups.Num())
    {
        Comp->WheelSetups.AddDefaulted();
    }
    FChaosWheelSetup& Setup = Comp->WheelSetups[WheelIndex];

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    if (Payload.IsValid())
    {
        if (Payload->HasField(TEXT("wheelClass")))
        {
            const FString WheelClassPath = Ctx.GetString(TEXT("wheelClass"));
            if (!WheelClassPath.IsEmpty())
            {
                UClass* WheelClass = LoadWheelClassFromPath(WheelClassPath);
                if (!WheelClass)
                {
                    Ctx.SendError(ErrorCodes::ERR_INVALID_WHEEL_CLASS,
                        FString::Printf(TEXT("wheelClass '%s' could not be resolved to a UChaosVehicleWheel subclass"), *WheelClassPath));
                    return true;
                }
                Setup.WheelClass = WheelClass;
            }
        }
        if (Payload->HasField(TEXT("boneName")))
        {
            Setup.BoneName = FName(*Ctx.GetString(TEXT("boneName")));
        }
        if (Payload->HasField(TEXT("additionalOffset")))
        {
            Setup.AdditionalOffset = Ctx.GetVector(TEXT("additionalOffset"), Setup.AdditionalOffset);
        }
        // bDisableSteering is in the ticket spec but FChaosWheelSetup does not carry that
        // field in UE 5.6 — steering disable lives on the wheel asset (bAffectedBySteering).
        // Silently ignored here; callers can use vehicle.set_wheel_asset_property instead.
    }

    UBlueprint* OwningBP = FindOwningBlueprint(Comp);
    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics;
    bool bCompileAttempted = false;
    if (OwningBP)
    {
        OwningBP->MarkPackageDirty();
        if (Ctx.GetBool(TEXT("compile"), false))
        {
            Diagnostics = BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(OwningBP);
            bCompileAttempted = true;
        }
        if (Ctx.GetBool(TEXT("save"), false))
        {
            McpSafeAssetSave(OwningBP);
        }
    }
    else if (UPackage* Pkg = Comp->GetOutermost())
    {
        Pkg->MarkPackageDirty();
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetNumberField(TEXT("wheelIndex"), WheelIndex);
    Result->SetNumberField(TEXT("wheelSetupCount"), Comp->WheelSetups.Num());
    if (bCompileAttempted)
    {
        BlueprintHandlerUtils::AddCompileDiagnosticsToJson(Diagnostics, Result);
    }
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("vehicle.remove_wheel_setup", "vehicle",
    "Remove a FChaosWheelSetup entry by index from a UChaosWheeledVehicleMovementComponent.",
    RPC_PARAMS(
        RPC_PARAM_REQ("componentPath", "path", "Path to the UChaosWheeledVehicleMovementComponent that hosts the wheels, NOT a wheel asset. BP CDO subobject path (\"/Game/Vehicles/BP_Car.BP_Car_C:VehicleMovement\"; the \"BP_Car:VehicleMovement\" and \".Default__BP_Car_C:VehicleMovement\" forms resolve to the same component) or live actor component path (\"ActorLabel:VehicleMovement\")."),
        RPC_PARAM_REQ("wheelIndex", "integer", "Index to remove."),
        RPC_PARAM_OPT("compile", "bool", "Compile owning Blueprint after edit."),
        RPC_PARAM_OPT("save", "bool", "Save owning Blueprint to disk.")
    ))
{
    FString ComponentPath;
    if (!Ctx.RequireString(TEXT("componentPath"), ComponentPath)) return true;
    int32 WheelIndex = -1;
    if (!Ctx.RequireInt(TEXT("wheelIndex"), WheelIndex)) return true;

    FString ResolveError;
    UChaosWheeledVehicleMovementComponent* Comp = ResolveWheeledComp(ComponentPath, ResolveError);
    if (!Comp)
    {
        Ctx.SendError(ResolveError.IsEmpty() ? TEXT("COMPONENT_NOT_FOUND") : *ResolveError,
            FString::Printf(TEXT("Failed to resolve component: %s"), *ComponentPath));
        return true;
    }

    if (WheelIndex < 0 || WheelIndex >= Comp->WheelSetups.Num())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX,
            FString::Printf(TEXT("wheelIndex %d out of range [0,%d)"),
                WheelIndex, Comp->WheelSetups.Num()));
        return true;
    }

    Comp->Modify();
    Comp->WheelSetups.RemoveAt(WheelIndex);

    UBlueprint* OwningBP = FindOwningBlueprint(Comp);
    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics;
    bool bCompileAttempted = false;
    if (OwningBP)
    {
        OwningBP->MarkPackageDirty();
        if (Ctx.GetBool(TEXT("compile"), false))
        {
            Diagnostics = BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(OwningBP);
            bCompileAttempted = true;
        }
        if (Ctx.GetBool(TEXT("save"), false))
        {
            McpSafeAssetSave(OwningBP);
        }
    }
    else if (UPackage* Pkg = Comp->GetOutermost())
    {
        Pkg->MarkPackageDirty();
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetNumberField(TEXT("wheelSetupCount"), Comp->WheelSetups.Num());
    if (bCompileAttempted)
    {
        BlueprintHandlerUtils::AddCompileDiagnosticsToJson(Diagnostics, Result);
    }
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("vehicle.set_suspension", "vehicle",
    "Edit suspension fields on the wheel-asset CDO referenced by WheelSetups[idx].WheelClass (suspension lives on the wheel, not on FChaosWheelSetup). If wheelIndex is omitted, applies to every wheel setup with a valid WheelClass.",
    RPC_PARAMS(
        RPC_PARAM_REQ("componentPath", "path", "Path to the UChaosWheeledVehicleMovementComponent that hosts the wheels, NOT a wheel asset. BP CDO subobject path (\"/Game/Vehicles/BP_Car.BP_Car_C:VehicleMovement\"; the \"BP_Car:VehicleMovement\" and \".Default__BP_Car_C:VehicleMovement\" forms resolve to the same component) or live actor component path (\"ActorLabel:VehicleMovement\")."),
        RPC_PARAM_OPT("wheelIndex", "integer", "Wheel setup index to target; omit to apply to all."),
        RPC_PARAM_OPT("suspensionMaxRaise", "number", "SuspensionMaxRaise (cm)."),
        RPC_PARAM_OPT("suspensionMaxDrop", "number", "SuspensionMaxDrop (cm)."),
        RPC_PARAM_OPT("springRate", "number", "SpringRate (N/m)."),
        RPC_PARAM_OPT("springPreload", "number", "SpringPreload (N/m)."),
        RPC_PARAM_OPT("dampingRatio", "number", "SuspensionDampingRatio."),
        RPC_PARAM_OPT("suspensionForceOffset", "object", "FVector offset applied to the suspension force application point (CoM-relative, cm)."),
        RPC_PARAM_DEF("save", "bool", "Save touched wheel assets to disk.", "true")
    ))
{
    FString ComponentPath;
    if (!Ctx.RequireString(TEXT("componentPath"), ComponentPath)) return true;

    FString ResolveError;
    UChaosWheeledVehicleMovementComponent* Comp = ResolveWheeledComp(ComponentPath, ResolveError);
    if (!Comp)
    {
        Ctx.SendError(ResolveError.IsEmpty() ? TEXT("COMPONENT_NOT_FOUND") : *ResolveError,
            FString::Printf(TEXT("Failed to resolve component: %s"), *ComponentPath));
        return true;
    }

    int32 SpecificIndex = INDEX_NONE;
    bool bHasIndex = false;
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    if (Payload.IsValid() && Payload->HasField(TEXT("wheelIndex")))
    {
        bHasIndex = true;
        if (!Ctx.RequireInt(TEXT("wheelIndex"), SpecificIndex)) return true;
        if (SpecificIndex < 0 || SpecificIndex >= Comp->WheelSetups.Num())
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_INDEX,
                FString::Printf(TEXT("wheelIndex %d out of range [0,%d)"),
                    SpecificIndex, Comp->WheelSetups.Num()));
            return true;
        }
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    TArray<FString> Warnings;
    TSet<UClass*> TouchedClasses;
    int32 AppliedCount = 0;
    BlueprintHandlerUtils::FBlueprintCompileDiagnostics Diagnostics;
    TArray<TSharedPtr<FJsonValue>> CompileResults;
    int32 CompileAttemptCount = 0;
    int32 CompileFailureCount = 0;

    for (int32 Idx = 0; Idx < Comp->WheelSetups.Num(); ++Idx)
    {
        if (bHasIndex && Idx != SpecificIndex) continue;

        UClass* WheelClass = Comp->WheelSetups[Idx].WheelClass.Get();
        if (!WheelClass)
        {
            Warnings.Add(FString::Printf(TEXT("WheelSetups[%d].WheelClass is null; skipped"), Idx));
            continue;
        }
        if (TouchedClasses.Contains(WheelClass)) { ++AppliedCount; continue; }
        TouchedClasses.Add(WheelClass);

        UChaosVehicleWheel* CDO = Cast<UChaosVehicleWheel>(WheelClass->GetDefaultObject());
        if (!CDO)
        {
            Warnings.Add(FString::Printf(TEXT("WheelSetups[%d] CDO not a UChaosVehicleWheel; skipped"), Idx));
            continue;
        }

        ApplyJsonPropertiesByName(CDO, CDO, CDO->GetClass(), Payload, SuspensionPropertyNameMap(), Warnings);
        ++AppliedCount;

        // Dirty the wheel asset's package; save if requested.
        if (UBlueprint* WheelBP = Cast<UBlueprint>(WheelClass->ClassGeneratedBy))
        {
            WheelBP->MarkPackageDirty();
            const BlueprintHandlerUtils::FBlueprintCompileDiagnostics WheelDiagnostics =
                BlueprintHandlerUtils::CompileBlueprintWithDiagnostics(WheelBP);
            MergeCompileDiagnostics(Diagnostics, WheelDiagnostics, CompileAttemptCount == 0);
            ++CompileAttemptCount;
            if (!WheelDiagnostics.bCompiled)
            {
                ++CompileFailureCount;
            }

            TSharedPtr<FJsonObject> CompileResult = MakeShared<FJsonObject>();
            CompileResult->SetStringField(TEXT("assetPath"), WheelBP->GetPathName());
            BlueprintHandlerUtils::AddCompileDiagnosticsToJson(WheelDiagnostics, CompileResult);
            CompileResults.Add(MakeShared<FJsonValueObject>(CompileResult));
            if (bSave)
            {
                McpSafeAssetSave(WheelBP);
            }
        }
        else if (UPackage* Pkg = WheelClass->GetOutermost())
        {
            Pkg->MarkPackageDirty();
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), CompileFailureCount == 0);
    Result->SetNumberField(TEXT("appliedCount"), AppliedCount);
    Result->SetNumberField(TEXT("wheelSetupCount"), Comp->WheelSetups.Num());
    if (Warnings.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> WarnArr;
        for (const FString& W : Warnings)
        {
            WarnArr.Add(MakeShared<FJsonValueString>(W));
        }
        Result->SetArrayField(TEXT("warnings"), WarnArr);
    }
    if (CompileAttemptCount > 0)
    {
        BlueprintHandlerUtils::AddCompileDiagnosticsToJson(
            Diagnostics, Result, TEXT("compileErrors"), TEXT("compileWarnings"));
        Result->SetArrayField(TEXT("compileResults"), CompileResults);
    }
    if (CompileFailureCount > 0)
    {
        Ctx.SendError(ErrorCodes::ERR_COMPILE_FAILED,
            FString::Printf(TEXT("vehicle.set_suspension failed to compile %d of %d touched wheel Blueprint(s)"),
                CompileFailureCount, CompileAttemptCount),
            Result);
        return true;
    }
    Ctx.SendSuccess(Result);
    return true;
}

#endif // __has_include("ChaosVehicleMovementComponent.h")
