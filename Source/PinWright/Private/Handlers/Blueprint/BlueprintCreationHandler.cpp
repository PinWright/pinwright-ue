// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"
#include "State/PluginState.h"
#include "State/BlueprintTracker.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/AssetUtils.h"
#include "Utils/ClassUtils.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Components/ActorComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EditorAssetLibrary.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Factories/BlueprintFactory.h"
#include "Factories/BlueprintInterfaceFactory.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/Interface.h"
#include "UObject/UnrealType.h"

// SubobjectDataSubsystem detection
#if defined(__has_include)
#  if __has_include("Subsystems/SubobjectDataSubsystem.h")
#    include "Subsystems/SubobjectDataSubsystem.h"
#  elif __has_include("SubobjectDataSubsystem.h")
#    include "SubobjectDataSubsystem.h"
#  elif __has_include("SubobjectData/SubobjectDataSubsystem.h")
#    include "SubobjectData/SubobjectDataSubsystem.h"
#  endif
#else
#  include "SubobjectDataSubsystem.h"
#endif

// Static helper: apply JSON properties to a UObject recursively
static void ApplyPropertiesToObject(UObject* TargetObj, const TSharedPtr<FJsonObject>& Properties)
{
    if (!TargetObj || !Properties.IsValid()) return;

    for (const auto& Pair : Properties->Values)
    {
        FProperty* Property = TargetObj->GetClass()->FindPropertyByName(*Pair.Key);
        if (!Property) continue;

        if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
        {
            if (Pair.Value->Type == EJson::Object)
            {
                UObject* ChildObj = ObjProp->GetObjectPropertyValue_InContainer(TargetObj);
                if (ChildObj)
                {
                    ApplyPropertiesToObject(ChildObj, Pair.Value->AsObject());
                }
                continue;
            }
        }

        FString TextValue;
        if (Pair.Value->Type == EJson::String)
        {
            TextValue = Pair.Value->AsString();
        }
        else if (Pair.Value->Type == EJson::Number)
        {
            double Val = Pair.Value->AsNumber();
            if (Property->IsA<FIntProperty>() || Property->IsA<FInt64Property>() || Property->IsA<FByteProperty>())
            {
                TextValue = FString::Printf(TEXT("%lld"), (long long)Val);
            }
            else
            {
                TextValue = FString::SanitizeFloat(Val);
            }
        }
        else if (Pair.Value->Type == EJson::Boolean)
        {
            TextValue = Pair.Value->AsBool() ? TEXT("True") : TEXT("False");
        }

        if (!TextValue.IsEmpty())
        {
            Property->ImportText_Direct(*TextValue, Property->ContainerPtrToValuePtr<void>(TargetObj), TargetObj, 0);
        }
    }
}

struct FBlueprintCreateOptions
{
    UBlueprintFactory* Factory = nullptr;
    UClass* ParentClass = nullptr;
    bool bIsInterface = false;
};

static bool ResolveBlueprintCreateOptions(
    FHandlerContext& Ctx,
    const FString& ParentClassSpec,
    const FString& BlueprintTypeSpec,
    FBlueprintCreateOptions& OutOptions)
{
    const FString LowerType = BlueprintTypeSpec.TrimStartAndEnd().ToLower();
    const FString TrimmedParentClassSpec = ParentClassSpec.TrimStartAndEnd();
    OutOptions.bIsInterface = LowerType == TEXT("interface");
    OutOptions.ParentClass = TrimmedParentClassSpec.IsEmpty() ? nullptr : ResolveUClass(TrimmedParentClassSpec);

    if (!OutOptions.ParentClass && !LowerType.IsEmpty())
    {
        if (LowerType == TEXT("actor")) OutOptions.ParentClass = AActor::StaticClass();
        else if (LowerType == TEXT("pawn")) OutOptions.ParentClass = APawn::StaticClass();
        else if (LowerType == TEXT("character")) OutOptions.ParentClass = ACharacter::StaticClass();
        else if (OutOptions.bIsInterface) OutOptions.ParentClass = UInterface::StaticClass();
    }

    if (OutOptions.bIsInterface)
    {
        if (!TrimmedParentClassSpec.IsEmpty()
            && (!OutOptions.ParentClass || !OutOptions.ParentClass->IsChildOf(UInterface::StaticClass())))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("blueprintType:\"interface\" requires parentClass to resolve to UInterface or a UInterface subclass."));
            return false;
        }

        if (!OutOptions.ParentClass)
        {
            OutOptions.ParentClass = UInterface::StaticClass();
        }

        // UBlueprintInterfaceFactory has no export-API macro, so its GetPrivateStaticClass symbol is not
        // exported from UnrealEd. Resolve the UClass via the reflection registry to avoid the link error.
        UClass* InterfaceFactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/UnrealEd.BlueprintInterfaceFactory"));
        if (!InterfaceFactoryClass)
        {
            Ctx.SendError(TEXT("INTERNAL_ERROR"),
                TEXT("Failed to resolve UBlueprintInterfaceFactory via reflection; UnrealEd may not be loaded."));
            return false;
        }
        UBlueprintInterfaceFactory* InterfaceFactory =
            static_cast<UBlueprintInterfaceFactory*>(NewObject<UObject>(GetTransientPackage(), InterfaceFactoryClass));
        InterfaceFactory->ParentClass = OutOptions.ParentClass;
        InterfaceFactory->BlueprintType = BPTYPE_Interface;
        OutOptions.Factory = InterfaceFactory;
        return true;
    }

    UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
    Factory->ParentClass = OutOptions.ParentClass ? OutOptions.ParentClass : AActor::StaticClass();
    OutOptions.ParentClass = Factory->ParentClass;
    OutOptions.Factory = Factory;
    return true;
}

static TSharedPtr<FJsonObject> BuildBlueprintCreateResult(
    UBlueprint* Blueprint,
    const AssetCreatePolicy::FResolution& Resolution,
    bool& bOutSaved,
    EAssetSaveState& OutSaveState)
{
    OutSaveState = EAssetSaveState::Failed;
    bOutSaved = SaveAssetToDiskReportingPresence(
        Blueprint, /*bForce=*/true, nullptr, nullptr, &OutSaveState);
    ScanPathSynchronous(Blueprint->GetOutermost()->GetName());

    FString NormalizedPath = Blueprint->GetPathName();
    if (NormalizedPath.Contains(TEXT(".")))
    {
        NormalizedPath = NormalizedPath.Left(NormalizedPath.Find(TEXT(".")));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), NormalizedPath);
    Result->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    AddAssetSaveReport(Result, /*bSaveRequested=*/true, bOutSaved, OutSaveState);
    AssetCreatePolicy::AddCreateReport(Result, Resolution);
    AddAssetVerification(Result, Blueprint);
    return Result;
}

static FString DescribeBlueprintCreateSaveFailure(EAssetSaveState SaveState)
{
    return FString::Printf(
        TEXT("Blueprint exists in memory, but its save was not durable (saveState=%s)."),
        AssetSaveStateToWire(SaveState));
}

// ---- blueprint.create ----
REGISTER_RPC_MUTATING_HANDLER("blueprint.create", "blueprint", "Create a new UBlueprint asset deriving from a parent UClass. Optionally seeds CDO properties at creation. Idempotent: an existing Blueprint at the path is returned unchanged with existing:true, mode:\"updated_in_place\"; ASSET_ALREADY_EXISTS when a non-Blueprint asset occupies the path. Use blueprint.scs.add_component afterwards to add component templates, or blueprint.compile_bpir to add graph logic.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Asset filename (no extension) for the new Blueprint."),
        RPC_PARAM_OPT("savePath", "path", "Content-browser folder; defaults to /Game."),
        RPC_PARAM_OPT("parentClass", "classref", "Parent UClass: short name (e.g. 'Actor', 'Pawn', 'UserWidget') or full /Script path. Snake_case parent_class accepted."),
        RPC_PARAM_OPT("blueprintType", "string", "Convenience hint to pick the parent or asset kind when parentClass is omitted: 'actor' | 'pawn' | 'character' map to AActor/APawn/ACharacter; 'interface' creates a Blueprint Interface."),
        RPC_PARAM_OPT("properties", "object", "Map of UPROPERTY names to JSON values applied to the new Blueprint's CDO after compilation."),
        RPC_PARAM_DEF("overwrite", "boolean", "Delete and recreate an existing Blueprint (discarding its graphs and components) instead of returning it. Rejected with ASSET_IN_USE when any package still references it.", "false")
    ))
{
    UPinWrightSubsystem* Self = Ctx.GetSubsystem();
    TSharedPtr<FJsonObject> LocalPayload = Ctx.GetRawPayload();

    FString Name;
    LocalPayload->TryGetStringField(TEXT("name"), Name);
    if (Name.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("blueprint_create requires a name."));
        return true;
    }
    FString SavePath;
    LocalPayload->TryGetStringField(TEXT("savePath"), SavePath);
    if (SavePath.TrimStartAndEnd().IsEmpty()) SavePath = TEXT("/Game");
    FString ParentClassSpec;
    LocalPayload->TryGetStringField(TEXT("parentClass"), ParentClassSpec);
    FString BlueprintTypeSpec;
    LocalPayload->TryGetStringField(TEXT("blueprintType"), BlueprintTypeSpec);

    const bool bOverwriteRequested = Ctx.GetBool(TEXT("overwrite"), false);

    if (!Self)
    {
        // Test contexts: no subsystem/transport — create synchronously and respond via Ctx.
        FBlueprintCreateOptions CreateOptions;
        if (!ResolveBlueprintCreateOptions(Ctx, ParentClassSpec, BlueprintTypeSpec, CreateOptions))
        {
            return true;
        }

        const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
            FString::Printf(TEXT("%s/%s"), *SavePath, *Name), Name,
            UBlueprint::StaticClass(), bOverwriteRequested);
        if (Resolution.IsRejected())
        {
            return AssetCreatePolicy::SendRejection(Ctx, Resolution);
        }
        if (Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace)
        {
            UBlueprint* ExistingBlueprint = CastChecked<UBlueprint>(Resolution.Existing);
            bool bSaved = false;
            EAssetSaveState SaveState = EAssetSaveState::Failed;
            TSharedPtr<FJsonObject> ExistingPayload = BuildBlueprintCreateResult(
                ExistingBlueprint, Resolution, bSaved, SaveState);
            if (!bSaved)
            {
                Ctx.SendError(TEXT("SAVE_FAILED"),
                    DescribeBlueprintCreateSaveFailure(SaveState), ExistingPayload);
                return true;
            }
            Ctx.SendSuccess(TEXT("Blueprint already exists"), ExistingPayload);
            return true;
        }

        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.create")));
        FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
        UObject* NewObj = AssetToolsModule.Get().CreateAsset(Name, SavePath, UBlueprint::StaticClass(), CreateOptions.Factory);
        UBlueprint* CreatedBlueprint = Cast<UBlueprint>(NewObj);
        if (!CreatedBlueprint)
        {
            Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create blueprint asset"));
            return true;
        }
        bool bSaved = false;
        EAssetSaveState SaveState = EAssetSaveState::Failed;
        TSharedPtr<FJsonObject> ResultPayload = BuildBlueprintCreateResult(
            CreatedBlueprint, Resolution, bSaved, SaveState);
        if (!bSaved)
        {
            Ctx.SendError(TEXT("SAVE_FAILED"),
                DescribeBlueprintCreateSaveFailure(SaveState), ResultPayload);
            return true;
        }
        Ctx.SendSuccess(ResultPayload);
        return true;
    }

    FString RequestId = Ctx.GetRequestId();
    const double Now = FPlatformTime::Seconds();
    const FString CreateKey = FString::Printf(TEXT("%s/%s"), *SavePath, *Name);

    FBlueprintCreateOptions CreateOptions;
    if (!ResolveBlueprintCreateOptions(Ctx, ParentClassSpec, BlueprintTypeSpec, CreateOptions))
    {
        return true;
    }

    FBlueprintTracker& Tracker = FPluginState::Get().Blueprints();

    const auto SendTrackedCreateResponse =
        [&Ctx, Self, &Tracker, &CreateKey](bool bSuccess, const FString& SuccessMessage,
            const TSharedPtr<FJsonObject>& Payload, EAssetSaveState SaveState)
        {
            const FString Message = bSuccess
                ? SuccessMessage
                : DescribeBlueprintCreateSaveFailure(SaveState);
            const FString ErrorCode = bSuccess ? FString() : FString(TEXT("SAVE_FAILED"));
            TArray<FString> Subscribers = Tracker.DrainCreateInflightSubscribers(CreateKey);
            if (Subscribers.Num() > 0)
            {
                for (const FString& SubRequestId : Subscribers)
                {
                    Self->SendAutomationResponse(
                        SubRequestId, bSuccess, Message, Payload, ErrorCode);
                }
            }
            else if (bSuccess)
            {
                Ctx.SendSuccess(SuccessMessage, Payload);
            }
            else
            {
                Ctx.SendError(TEXT("SAVE_FAILED"), Message, Payload);
            }
        };

    // Track in-flight requests so all waiters receive completion
    if (Tracker.SubscribeOrCreateInflight(CreateKey, RequestId, Now))
    {
        return true;
    }

    // Check if asset already exists. Skipped when the caller asked to overwrite, so the
    // policy below gets the chance to pre-flight referencers and delete instead.
    FString PreExistingNormalized;
    FString PreExistingError;
    UBlueprint* PreExistingBP = bOverwriteRequested
        ? nullptr
        : LoadBlueprintAsset(CreateKey, PreExistingNormalized, PreExistingError);
    if (PreExistingBP)
    {
        AssetCreatePolicy::FResolution ExistingResolution;
        ExistingResolution.Action = AssetCreatePolicy::EAction::UpdateInPlace;
        ExistingResolution.bExistingFound = true;

        bool bSaved = false;
        EAssetSaveState SaveState = EAssetSaveState::Failed;
        TSharedPtr<FJsonObject> ResultPayload = BuildBlueprintCreateResult(
            PreExistingBP, ExistingResolution, bSaved, SaveState);
        SendTrackedCreateResponse(
            bSaved, TEXT("Blueprint already exists"), ResultPayload, SaveState);
        return true;
    }

    // LoadBlueprintAsset above only sees UBlueprints. A non-Blueprint asset squatting the
    // path (and the overwrite path itself) still reaches IAssetTools::CreateAsset, whose
    // CanCreateAsset raises the three-modal chain this policy exists to prevent.
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        CreateKey, Name, UBlueprint::StaticClass(), bOverwriteRequested);
    if (Resolution.IsRejected())
    {
        TArray<FString> Subscribers = Tracker.DrainCreateInflightSubscribers(CreateKey);
        if (Subscribers.Num() > 0)
        {
            const TSharedPtr<FJsonObject> ErrorData = AssetCreatePolicy::MakeErrorData(Resolution);
            for (const FString& SubRequestId : Subscribers)
            {
                Self->SendAutomationResponse(SubRequestId, false, Resolution.ErrorMessage,
                    ErrorData, Resolution.ErrorCode);
            }
            return true;
        }
        return AssetCreatePolicy::SendRejection(Ctx, Resolution);
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.create")));
    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    UObject* NewObj = AssetToolsModule.Get().CreateAsset(Name, SavePath, UBlueprint::StaticClass(), CreateOptions.Factory);

    UBlueprint* CreatedBlueprint = Cast<UBlueprint>(NewObj);

    // Apply optional CDO properties
    if (CreatedBlueprint && CreatedBlueprint->GeneratedClass)
    {
        const TSharedPtr<FJsonObject>* PropertiesPtr;
        if (LocalPayload->TryGetObjectField(TEXT("properties"), PropertiesPtr))
        {
            UObject* CDO = CreatedBlueprint->GeneratedClass->GetDefaultObject();
            if (CDO)
            {
                ApplyPropertiesToObject(CDO, *PropertiesPtr);
                CreatedBlueprint->Modify();
            }
        }
    }

    if (!CreatedBlueprint)
    {
        // Check if asset already exists (AssetTools returns nullptr for duplicates)
        FString ExistingNormalized;
        FString ExistingError;
        UBlueprint* ExistingBP = LoadBlueprintAsset(CreateKey, ExistingNormalized, ExistingError);
        if (ExistingBP)
        {
            AssetCreatePolicy::FResolution LateExistingResolution;
            LateExistingResolution.Action = AssetCreatePolicy::EAction::UpdateInPlace;
            LateExistingResolution.bExistingFound = true;

            bool bSaved = false;
            EAssetSaveState SaveState = EAssetSaveState::Failed;
            TSharedPtr<FJsonObject> ResultPayload = BuildBlueprintCreateResult(
                ExistingBP, LateExistingResolution, bSaved, SaveState);
            SendTrackedCreateResponse(
                bSaved, TEXT("Blueprint already exists"), ResultPayload, SaveState);
            return true;
        }

        FString CreationError = FString::Printf(TEXT("Created asset is not a Blueprint: %s"),
            NewObj ? *NewObj->GetPathName() : TEXT("<null>"));

        {
            TArray<FString> Subscribers = Tracker.DrainCreateInflightSubscribers(CreateKey);
            if (Subscribers.Num() > 0)
            {
                for (const FString& SubRequestId : Subscribers)
                    Self->SendAutomationResponse(SubRequestId, false, CreationError, nullptr, TEXT("CREATE_FAILED"));
            }
            else
            {
                Ctx.SendError(TEXT("CREATE_FAILED"), CreationError);
            }
        }
        return true;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    AssetRegistryModule.AssetCreated(CreatedBlueprint);

    bool bSaved = false;
    EAssetSaveState SaveState = EAssetSaveState::Failed;
    TSharedPtr<FJsonObject> ResultPayload = BuildBlueprintCreateResult(
        CreatedBlueprint, Resolution, bSaved, SaveState);
    SendTrackedCreateResponse(bSaved, TEXT("Blueprint created"), ResultPayload, SaveState);

    return true;
}
