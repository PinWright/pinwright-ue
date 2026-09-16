// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintComponentHandler.cpp - Migrated from PinWright_BlueprintHandlers.cpp
// Blueprint component/SCS operations: modify_scs, add_node, connect_pins

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintReinstancingGuard.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include <functional>

#include "Components/ActorComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EditorAssetLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

// SubobjectDataSubsystem conditional includes
#if defined(__has_include)
#if __has_include("Subsystems/SubobjectDataSubsystem.h")
#include "Subsystems/SubobjectDataSubsystem.h"
#elif __has_include("SubobjectDataSubsystem.h")
#include "SubobjectDataSubsystem.h"
#elif __has_include("SubobjectData/SubobjectDataSubsystem.h")
#include "SubobjectData/SubobjectDataSubsystem.h"
#endif
#else
#include "SubobjectDataSubsystem.h"
#endif

// SFINAE trait templates for SubobjectDataSubsystem API detection
namespace BlueprintComponentHandlerTraits {
template <typename, typename = void> struct THasK2Add : std::false_type {};
template <typename T>
struct THasK2Add<T, std::void_t<decltype(std::declval<T>().K2_AddNewSubobject(
                        std::declval<FAddNewSubobjectParams>()))>>
    : std::true_type {};

template <typename, typename = void> struct THasAdd : std::false_type {};
template <typename T>
struct THasAdd<T, std::void_t<decltype(std::declval<T>().AddNewSubobject(
                      std::declval<FAddNewSubobjectParams>()))>>
    : std::true_type {};

template <typename, typename = void> struct THasAddTwoArg : std::false_type {};
template <typename T>
struct THasAddTwoArg<
    T, std::void_t<decltype(std::declval<T>().AddNewSubobject(
           std::declval<FAddNewSubobjectParams>(), std::declval<FText&>()))>>
    : std::true_type {};

template <typename, typename = void>
struct THandleHasIsValid : std::false_type {};
template <typename T>
struct THandleHasIsValid<T, std::void_t<decltype(std::declval<T>().IsValid())>>
    : std::true_type {};

template <typename, typename = void> struct THasRename : std::false_type {};
template <typename T>
struct THasRename<
    T, std::void_t<decltype(std::declval<T>().RenameSubobjectMemberVariable(
           std::declval<UBlueprint*>(), std::declval<FSubobjectDataHandle>(),
           std::declval<FName>()))>> : std::true_type {};

template <typename, typename = void>
struct THasDeleteSubobject : std::false_type {};
template <typename T>
struct THasDeleteSubobject<
    T, std::void_t<decltype(std::declval<T>().DeleteSubobject(
           std::declval<const FSubobjectDataHandle&>(),
           std::declval<const FSubobjectDataHandle&>(),
           std::declval<UBlueprint*>()))>> : std::true_type {};

template <typename, typename = void> struct THasAttach : std::false_type {};
template <typename T>
struct THasAttach<T, std::void_t<decltype(std::declval<T>().AttachSubobject(
                         std::declval<FSubobjectDataHandle>(),
                         std::declval<FSubobjectDataHandle>()))>>
    : std::true_type {};
} // namespace BlueprintComponentHandlerTraits

using namespace BlueprintHandlerUtils;

// ---- blueprint.modify_scs ----
REGISTER_RPC_HANDLER("blueprint.modify_scs", "blueprint", "Batch modify Blueprint Simple Construction Script (SCS) components",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ("operations", "array", "Array of SCS operations [{type, componentName, ...}]"),
        RPC_PARAM_DEF("compile", "boolean", "Whether to compile after operations", "false"),
        RPC_PARAM_DEF("save", "boolean", "Whether to save after operations", "false"),
        BlueprintReinstancingGuard::AllowReinstancingParam()
    ))
{
    const double HandlerStartTimeSec = FPlatformTime::Seconds();
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    if (!Payload.IsValid())
    {
        Ctx.SendError(TEXT("INVALID_PAYLOAD"), TEXT("blueprint.modify_scs payload missing."));
        return true;
    }

    // Resolve blueprint path or candidate list
    FString BlueprintPath;
    TArray<FString> CandidatePaths;

    VisitBlueprintPathScalarFieldNames(
        EBlueprintPathParamAliasSet::ResolveBlueprintPath,
        [&Payload, &BlueprintPath](const TCHAR* FieldName)
        {
            FString Candidate;
            if (!Payload->TryGetStringField(FieldName, Candidate))
            {
                return true;
            }

            Candidate = Candidate.TrimStartAndEnd();
            if (Candidate.IsEmpty())
            {
                return true;
            }

            BlueprintPath = Candidate;
            return false;
        });

    if (BlueprintPath.IsEmpty())
    {
        const TArray<TSharedPtr<FJsonValue>>* CandidateArray = nullptr;
        VisitBlueprintPathCandidateArrayFieldNames(
            [&Payload, &CandidateArray](const TCHAR* FieldName)
            {
                if (Payload->TryGetArrayField(FieldName, CandidateArray) &&
                    CandidateArray != nullptr &&
                    CandidateArray->Num() > 0)
                {
                    return false;
                }
                return true;
            });

        if (CandidateArray == nullptr || CandidateArray->Num() == 0)
        {
            Ctx.SendError(TEXT("INVALID_BLUEPRINT"),
                TEXT("blueprint.modify_scs requires a non-empty Blueprint path alias or candidate array."));
            return true;
        }
        for (const TSharedPtr<FJsonValue>& Val : *CandidateArray)
        {
            if (!Val.IsValid()) continue;
            const FString Candidate = Val->AsString();
            if (!Candidate.TrimStartAndEnd().IsEmpty())
                CandidatePaths.Add(Candidate);
        }
        if (CandidatePaths.Num() == 0)
        {
            Ctx.SendError(TEXT("INVALID_BLUEPRINT_CANDIDATES"),
                TEXT("blueprint.modify_scs candidate array provided but contains no valid strings."));
            return true;
        }
    }

    // Operations are required
    const TArray<TSharedPtr<FJsonValue>>* OperationsArray = nullptr;
    if (!Payload->TryGetArrayField(TEXT("operations"), OperationsArray) || OperationsArray == nullptr)
    {
        Ctx.SendError(TEXT("INVALID_OPERATIONS"), TEXT("blueprint.modify_scs requires an operations array."));
        return true;
    }

    // Flags
    bool bCompile = false;
    if (Payload->HasField(TEXT("compile")) &&
        !Payload->TryGetBoolField(TEXT("compile"), bCompile))
    {
        Ctx.SendError(TEXT("INVALID_COMPILE_FLAG"), TEXT("compile must be a boolean."));
        return true;
    }
    bool bSave = false;
    if (Payload->HasField(TEXT("save")) &&
        !Payload->TryGetBoolField(TEXT("save"), bSave))
    {
        Ctx.SendError(TEXT("INVALID_SAVE_FLAG"), TEXT("save must be a boolean."));
        return true;
    }

    // Resolve the blueprint asset (explicit path preferred, then candidates)
    FString NormalizedBlueprintPath;
    FString LoadError;
    TArray<FString> TriedCandidates;

    if (!BlueprintPath.IsEmpty())
    {
        TriedCandidates.Add(BlueprintPath);
        if (FindBlueprintNormalizedPath(BlueprintPath, NormalizedBlueprintPath))
        {
            // Resolved
        }
        else
        {
            LoadError = FString::Printf(TEXT("Blueprint not found for path %s"), *BlueprintPath);
        }
    }

    if (NormalizedBlueprintPath.IsEmpty() && CandidatePaths.Num() > 0)
    {
        for (const FString& Candidate : CandidatePaths)
        {
            TriedCandidates.Add(Candidate);
            FString CandidateNormalized;
            if (FindBlueprintNormalizedPath(Candidate, CandidateNormalized))
            {
                NormalizedBlueprintPath = CandidateNormalized;
                LoadError.Empty();
                break;
            }
            LoadError = FString::Printf(TEXT("Candidate not found: %s"), *Candidate);
        }
    }

    if (NormalizedBlueprintPath.IsEmpty())
    {
        TSharedPtr<FJsonObject> ErrPayload = MakeShared<FJsonObject>();
        if (TriedCandidates.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> TriedValues;
            for (const FString& C : TriedCandidates)
                TriedValues.Add(MakeShared<FJsonValueString>(C));
            ErrPayload->SetArrayField(TEXT("triedCandidates"), TriedValues);
        }
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadError.IsEmpty() ? TEXT("Blueprint not found") : *LoadError);
        return true;
    }

    if (OperationsArray->Num() == 0)
    {
        TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
        ResultPayload->SetStringField(TEXT("blueprintPath"), NormalizedBlueprintPath);
        ResultPayload->SetArrayField(TEXT("operations"), TArray<TSharedPtr<FJsonValue>>());
        Ctx.SendSuccess(ResultPayload);
        return true;
    }

    // Prevent concurrent SCS modifications against the same blueprint
    FBlueprintTracker& BpTracker = FPluginState::Get().Blueprints();
    const FString BusyKey = NormalizedBlueprintPath;
    if (!BusyKey.IsEmpty())
    {
        if (BpTracker.IsBusy(BusyKey))
        {
            Ctx.SendError(TEXT("BLUEPRINT_BUSY"),
                *FString::Printf(TEXT("Blueprint %s is busy with another modification."), *BusyKey));
            return true;
        }

        BpTracker.MarkBusy(BusyKey);
        BpTracker.SetCurrentBusyKey(BusyKey);
        BpTracker.SetBusyMarked(true);
        BpTracker.SetBusyScheduled(false);

        ON_SCOPE_EXIT
        {
            if (BpTracker.IsBusyMarked() && !BpTracker.IsBusyScheduled())
            {
                BpTracker.ClearCurrentBusyState();
            }
        };
    }

    // Shallow copy of operations for safe reference
    TArray<TSharedPtr<FJsonValue>> DeferredOps = *OperationsArray;
    bool bContainsImplicitCompile = false;

    // Lightweight validation of operations
    for (int32 Index = 0; Index < DeferredOps.Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& OperationValue = DeferredOps[Index];
        if (!OperationValue.IsValid() || OperationValue->Type != EJson::Object)
        {
            Ctx.SendError(TEXT("INVALID_OPERATION_PAYLOAD"),
                *FString::Printf(TEXT("Operation at index %d is not an object."), Index));
            return true;
        }
        const TSharedPtr<FJsonObject> OperationObject = OperationValue->AsObject();
        FString OperationType;
        if (!OperationObject->TryGetStringField(TEXT("type"), OperationType) ||
            OperationType.TrimStartAndEnd().IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_OPERATION_TYPE"),
                *FString::Printf(TEXT("Operation at index %d missing type."), Index));
            return true;
        }
        bContainsImplicitCompile |= OperationType.Equals(
            TEXT("add_component"), ESearchCase::IgnoreCase);
    }

    // Mark busy as scheduled (we will perform the work synchronously here)
    BpTracker.SetBusyScheduled(true);

    // Load the blueprint asset
    TSharedPtr<FJsonObject> CompletionResult = MakeShared<FJsonObject>();
    TArray<FString> LocalWarnings;
    TArray<TSharedPtr<FJsonValue>> FinalSummaries;

    FString LocalNormalized, LocalLoadError;
    UBlueprint* LocalBP = LoadBlueprintAsset(
        NormalizedBlueprintPath, LocalNormalized, LocalLoadError);
    if (!LocalBP)
    {
        CompletionResult->SetStringField(TEXT("error"), LocalLoadError);
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"), *LocalLoadError);
        BpTracker.ClearCurrentBusyState();
        return true;
    }

    BlueprintReinstancingGuard::FLiveInstanceSurvey ReinstancingSurvey;
    if (bCompile || bContainsImplicitCompile)
    {
        ReinstancingSurvey = BlueprintReinstancingGuard::SurveyLiveInstances(LocalBP);
        if (BlueprintReinstancingGuard::RefuseIfLiveInstancesWouldBeReinstanced(
                Ctx, LocalBP, ReinstancingSurvey, TEXT("blueprint.modify_scs")))
        {
            BpTracker.ClearCurrentBusyState();
            return true;
        }
    }

    USimpleConstructionScript* LocalSCS = LocalBP->SimpleConstructionScript;
    if (!LocalSCS)
    {
        Ctx.SendError(TEXT("SCS_UNAVAILABLE"), TEXT("SCS unavailable for blueprint"));
        BpTracker.ClearCurrentBusyState();
        return true;
    }

    // Apply operations directly
    FBlueprintCompileDiagnostics CompileDiagnostics;
    bool bCompileAttempted = false;
    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: blueprint.modify_scs")));
    LocalBP->Modify();
    LocalSCS->Modify();
    for (int32 Index = 0; Index < DeferredOps.Num(); ++Index)
    {
        const double OpStart = FPlatformTime::Seconds();
        const TSharedPtr<FJsonValue>& V = DeferredOps[Index];
        if (!V.IsValid() || V->Type != EJson::Object) continue;
        const TSharedPtr<FJsonObject> Op = V->AsObject();
        FString OpType;
        Op->TryGetStringField(TEXT("type"), OpType);
        const FString NormalizedType = OpType.ToLower();
        TSharedPtr<FJsonObject> OpSummary = MakeShared<FJsonObject>();
        OpSummary->SetNumberField(TEXT("index"), Index);
        OpSummary->SetStringField(TEXT("type"), NormalizedType);

        if (NormalizedType == TEXT("modify_component"))
        {
            FString ComponentName;
            Op->TryGetStringField(TEXT("componentName"), ComponentName);
            const TSharedPtr<FJsonValue> TransformVal = Op->TryGetField(TEXT("transform"));
            const TSharedPtr<FJsonObject> TransformObj =
                TransformVal.IsValid() && TransformVal->Type == EJson::Object
                    ? TransformVal->AsObject() : nullptr;
            if (!ComponentName.IsEmpty() && TransformObj.IsValid())
            {
                USCS_Node* Node = FindScsNodeByName(LocalSCS, ComponentName);
                if (Node && Node->ComponentTemplate &&
                    Node->ComponentTemplate->IsA<USceneComponent>())
                {
                    USceneComponent* SceneTemplate = Cast<USceneComponent>(Node->ComponentTemplate);
                    FVector Location = SceneTemplate->GetRelativeLocation();
                    FRotator Rotation = SceneTemplate->GetRelativeRotation();
                    FVector Scale = SceneTemplate->GetRelativeScale3D();
                    ReadVectorField(TransformObj, TEXT("location"), Location, Location);
                    ReadRotatorField(TransformObj, TEXT("rotation"), Rotation, Rotation);
                    ReadVectorField(TransformObj, TEXT("scale"), Scale, Scale);
                    SceneTemplate->SetRelativeLocation(Location);
                    SceneTemplate->SetRelativeRotation(Rotation);
                    SceneTemplate->SetRelativeScale3D(Scale);
                    OpSummary->SetBoolField(TEXT("success"), true);
                    OpSummary->SetStringField(TEXT("componentName"), ComponentName);
                }
                else
                {
                    OpSummary->SetBoolField(TEXT("success"), false);
                    OpSummary->SetStringField(TEXT("warning"),
                        TEXT("Component not found or template missing"));
                }
            }
            else
            {
                OpSummary->SetBoolField(TEXT("success"), false);
                OpSummary->SetStringField(TEXT("warning"),
                    TEXT("Missing component name or transform"));
            }
        }
        else if (NormalizedType == TEXT("add_component"))
        {
            FString ComponentName;
            Op->TryGetStringField(TEXT("componentName"), ComponentName);
            FString ComponentClassPath;
            Op->TryGetStringField(TEXT("componentClass"), ComponentClassPath);
            FString AttachToName;
            Op->TryGetStringField(TEXT("attachTo"), AttachToName);
            FSoftClassPath ComponentClassSoftPath(ComponentClassPath);
            UClass* ComponentClass = ComponentClassSoftPath.TryLoadClass<UActorComponent>();
            if (!ComponentClass)
                ComponentClass = FindObject<UClass>(nullptr, *ComponentClassPath);
            if (!ComponentClass)
            {
                const TArray<FString> Prefixes = {
                    TEXT("/Script/Engine."), TEXT("/Script/UMG."), TEXT("/Script/Paper2D.")};
                for (const FString& Prefix : Prefixes)
                {
                    const FString Guess = Prefix + ComponentClassPath;
                    UClass* TryClass = FindObject<UClass>(nullptr, *Guess);
                    if (!TryClass)
                        TryClass = StaticLoadClass(UActorComponent::StaticClass(), nullptr, *Guess);
                    if (TryClass) { ComponentClass = TryClass; break; }
                }
            }
            if (!ComponentClass)
            {
                OpSummary->SetBoolField(TEXT("success"), false);
                OpSummary->SetStringField(TEXT("warning"), TEXT("Component class not found"));
            }
            else
            {
                USCS_Node* ExistingNode = FindScsNodeByName(LocalSCS, ComponentName);
                if (ExistingNode)
                {
                    OpSummary->SetBoolField(TEXT("success"), true);
                    OpSummary->SetStringField(TEXT("componentName"), ComponentName);
                    OpSummary->SetStringField(TEXT("warning"), TEXT("Component already exists"));
                }
                else
                {
                    bool bAddedViaSubsystem = false;
                    FString AdditionMethodStr;
                    USubobjectDataSubsystem* SubobjectSub = nullptr;
                    if (GEngine)
                        SubobjectSub = GEngine->GetEngineSubsystem<USubobjectDataSubsystem>();
                    if (SubobjectSub)
                    {
                        TArray<FSubobjectDataHandle> ExistingHandles;
                        SubobjectSub->K2_GatherSubobjectDataForBlueprint(LocalBP, ExistingHandles);
                        FSubobjectDataHandle ParentHandle;
                        if (ExistingHandles.Num() > 0)
                        {
                            bool bFoundParentByName = false;
                            if (!AttachToName.TrimStartAndEnd().IsEmpty())
                            {
                                const UScriptStruct* HandleStruct = FSubobjectDataHandle::StaticStruct();
                                for (const FSubobjectDataHandle& H : ExistingHandles)
                                {
                                    if (!HandleStruct) continue;
                                    FString HText;
                                    HandleStruct->ExportText(HText, &H, nullptr, nullptr, PPF_None, nullptr);
                                    if (HText.Contains(AttachToName, ESearchCase::IgnoreCase))
                                    {
                                        ParentHandle = H;
                                        bFoundParentByName = true;
                                        break;
                                    }
                                }
                            }
                            if (!bFoundParentByName)
                                ParentHandle = ExistingHandles[0];
                        }

                        using namespace BlueprintComponentHandlerTraits;
                        constexpr bool bHasAddTwoArg = THasAddTwoArg<USubobjectDataSubsystem>::value;
                        constexpr bool bHandleHasIsValid = THandleHasIsValid<FSubobjectDataHandle>::value;
                        constexpr bool bHasRename = THasRename<USubobjectDataSubsystem>::value;

                        FSubobjectDataHandle NewHandle;
                        if constexpr (bHasAddTwoArg)
                        {
                            FAddNewSubobjectParams Params;
                            Params.ParentHandle = ParentHandle;
                            Params.NewClass = ComponentClass;
                            Params.BlueprintContext = LocalBP;
                            FText FailReason;
                            NewHandle = SubobjectSub->AddNewSubobject(Params, FailReason);
                            AdditionMethodStr = TEXT("SubobjectDataSubsystem.AddNewSubobject(WithFailReason)");

                            bool bHandleValid = true;
                            if constexpr (bHandleHasIsValid)
                                bHandleValid = NewHandle.IsValid();

                            if (bHandleValid)
                            {
                                if constexpr (bHasRename)
                                {
                                    FString UniqueName = ComponentName;
                                    FName TargetVarName = FName(*UniqueName);

                                    if (LocalBP->GeneratedClass)
                                    {
                                        bool bNameExists = false;
                                        for (TFieldIterator<FProperty> It(LocalBP->GeneratedClass); It; ++It)
                                        {
                                            if (It->GetFName() == TargetVarName) { bNameExists = true; break; }
                                        }

                                        FString GenVarName = UniqueName + TEXT("_GEN_VARIABLE");
                                        FName GenVarFName = FName(*GenVarName);
                                        for (TFieldIterator<FProperty> It(LocalBP->GeneratedClass); It; ++It)
                                        {
                                            if (It->GetFName() == GenVarFName) { bNameExists = true; break; }
                                        }

                                        if (bNameExists)
                                        {
                                            int32 Suffix = 1;
                                            while (Suffix < 1000)
                                            {
                                                UniqueName = FString::Printf(TEXT("%s_%d"), *ComponentName, Suffix);
                                                TargetVarName = FName(*UniqueName);
                                                bNameExists = false;
                                                for (TFieldIterator<FProperty> It(LocalBP->GeneratedClass); It; ++It)
                                                {
                                                    if (It->GetFName() == TargetVarName) { bNameExists = true; break; }
                                                }
                                                if (!bNameExists) break;
                                                Suffix++;
                                            }
                                            OpSummary->SetStringField(TEXT("originalName"), ComponentName);
                                            OpSummary->SetStringField(TEXT("renamedTo"), UniqueName);
                                        }
                                    }

                                    SubobjectSub->RenameSubobjectMemberVariable(LocalBP, NewHandle, TargetVarName);
                                }
                                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(LocalBP);
                                CompileDiagnostics = CompileBlueprintWithDiagnostics(LocalBP);
                                bCompileAttempted = true;
                                SaveLoadedAssetThrottled(LocalBP);
                                bAddedViaSubsystem = true;
                            }
                        }
                    }
                    if (bAddedViaSubsystem)
                    {
                        OpSummary->SetBoolField(TEXT("success"), true);
                        OpSummary->SetStringField(TEXT("componentName"), ComponentName);
                        if (!AdditionMethodStr.IsEmpty())
                            OpSummary->SetStringField(TEXT("additionMethod"), AdditionMethodStr);
                    }
                    else
                    {
                        USCS_Node* NewNode = LocalSCS->CreateNode(ComponentClass, *ComponentName);
                        if (NewNode)
                        {
                            if (!AttachToName.TrimStartAndEnd().IsEmpty())
                            {
                                if (USCS_Node* ParentNode = FindScsNodeByName(LocalSCS, AttachToName))
                                    ParentNode->AddChildNode(NewNode);
                                else
                                    LocalSCS->AddNode(NewNode);
                            }
                            else
                            {
                                LocalSCS->AddNode(NewNode);
                            }
                            OpSummary->SetBoolField(TEXT("success"), true);
                            OpSummary->SetStringField(TEXT("componentName"), ComponentName);
                        }
                        else
                        {
                            OpSummary->SetBoolField(TEXT("success"), false);
                            OpSummary->SetStringField(TEXT("warning"), TEXT("Failed to create SCS node"));
                        }
                    }
                }
            }
        }
        else if (NormalizedType == TEXT("remove_component"))
        {
            FString ComponentName;
            Op->TryGetStringField(TEXT("componentName"), ComponentName);
            bool bRemoved = false;
            USubobjectDataSubsystem* SubobjectSub = nullptr;
            if (GEngine)
                SubobjectSub = GEngine->GetEngineSubsystem<USubobjectDataSubsystem>();
            if (SubobjectSub)
            {
                TArray<FSubobjectDataHandle> ExistingHandles;
                SubobjectSub->K2_GatherSubobjectDataForBlueprint(LocalBP, ExistingHandles);
                FSubobjectDataHandle FoundHandle;
                bool bFound = false;
                const UScriptStruct* HandleStruct = FSubobjectDataHandle::StaticStruct();
                for (const FSubobjectDataHandle& H : ExistingHandles)
                {
                    if (!HandleStruct) continue;
                    FString HText;
                    HandleStruct->ExportText(HText, &H, nullptr, nullptr, PPF_None, nullptr);
                    if (HText.Contains(ComponentName, ESearchCase::IgnoreCase))
                    {
                        FoundHandle = H;
                        bFound = true;
                        break;
                    }
                }
                if (bFound)
                {
                    using namespace BlueprintComponentHandlerTraits;
                    constexpr bool bHasDelete = THasDeleteSubobject<USubobjectDataSubsystem>::value;
                    if constexpr (bHasDelete)
                    {
                        FSubobjectDataHandle ContextHandle =
                            ExistingHandles.Num() > 0 ? ExistingHandles[0] : FoundHandle;
                        SubobjectSub->DeleteSubobject(ContextHandle, FoundHandle, LocalBP);
                        bRemoved = true;
                    }
                }
            }
            if (bRemoved)
            {
                OpSummary->SetBoolField(TEXT("success"), true);
                OpSummary->SetStringField(TEXT("componentName"), ComponentName);
            }
            else
            {
                if (USCS_Node* TargetNode = FindScsNodeByName(LocalSCS, ComponentName))
                {
                    LocalSCS->RemoveNode(TargetNode);
                    OpSummary->SetBoolField(TEXT("success"), true);
                    OpSummary->SetStringField(TEXT("componentName"), ComponentName);
                }
                else
                {
                    OpSummary->SetBoolField(TEXT("success"), false);
                    OpSummary->SetStringField(TEXT("warning"), TEXT("Component not found; remove skipped"));
                }
            }
        }
        else if (NormalizedType == TEXT("attach_component"))
        {
            FString AttachComponentName;
            Op->TryGetStringField(TEXT("componentName"), AttachComponentName);
            FString ParentName;
            Op->TryGetStringField(TEXT("parentComponent"), ParentName);
            if (ParentName.IsEmpty())
                Op->TryGetStringField(TEXT("attachTo"), ParentName);
            bool bAttached = false;
            USubobjectDataSubsystem* SubobjectSub = nullptr;
            if (GEngine)
                SubobjectSub = GEngine->GetEngineSubsystem<USubobjectDataSubsystem>();
            if (SubobjectSub)
            {
                TArray<FSubobjectDataHandle> Handles;
                SubobjectSub->K2_GatherSubobjectDataForBlueprint(LocalBP, Handles);
                FSubobjectDataHandle ChildHandle, ParentHandle2;
                const UScriptStruct* HandleStruct = FSubobjectDataHandle::StaticStruct();
                for (const FSubobjectDataHandle& H : Handles)
                {
                    if (!HandleStruct) continue;
                    FString HText;
                    HandleStruct->ExportText(HText, &H, nullptr, nullptr, PPF_None, nullptr);
                    if (!AttachComponentName.IsEmpty() &&
                        HText.Contains(AttachComponentName, ESearchCase::IgnoreCase))
                        ChildHandle = H;
                    if (!ParentName.IsEmpty() &&
                        HText.Contains(ParentName, ESearchCase::IgnoreCase))
                        ParentHandle2 = H;
                }
                using namespace BlueprintComponentHandlerTraits;
                constexpr bool bHasAttach = THasAttach<USubobjectDataSubsystem>::value;
                if (ChildHandle.IsValid() && ParentHandle2.IsValid())
                {
                    if constexpr (bHasAttach)
                        bAttached = SubobjectSub->AttachSubobject(ParentHandle2, ChildHandle);
                }
            }
            if (bAttached)
            {
                OpSummary->SetBoolField(TEXT("success"), true);
                OpSummary->SetStringField(TEXT("componentName"), AttachComponentName);
                OpSummary->SetStringField(TEXT("attachedTo"), ParentName);
            }
            else
            {
                USCS_Node* ChildNode = FindScsNodeByName(LocalSCS, AttachComponentName);
                USCS_Node* ParentNode = FindScsNodeByName(LocalSCS, ParentName);
                if (ChildNode && ParentNode)
                {
                    ParentNode->AddChildNode(ChildNode);
                    OpSummary->SetBoolField(TEXT("success"), true);
                    OpSummary->SetStringField(TEXT("componentName"), AttachComponentName);
                    OpSummary->SetStringField(TEXT("attachedTo"), ParentName);
                }
                else
                {
                    OpSummary->SetBoolField(TEXT("success"), false);
                    OpSummary->SetStringField(TEXT("warning"), TEXT("Attach failed: child or parent not found"));
                }
            }
        }
        else
        {
            OpSummary->SetBoolField(TEXT("success"), false);
            OpSummary->SetStringField(TEXT("warning"), TEXT("Unknown operation type"));
        }

        const double OpElapsedMs = (FPlatformTime::Seconds() - OpStart) * 1000.0;
        OpSummary->SetNumberField(TEXT("durationMs"), OpElapsedMs);
        FinalSummaries.Add(MakeShared<FJsonValueObject>(OpSummary));
    }

    const bool bOk = FinalSummaries.Num() > 0;

    // Compile/save as requested
    bool bSaveResult = false;
    if (bSave && LocalBP)
    {
        // WasSavePersisted, not the raw call: the throttle skip and the transient-package
        // early-out used to return true, so a second edit inside the 0.5s window reported
        // saved:true with the change still only in memory.
        bSaveResult = WasSavePersisted(SaveLoadedAssetThrottled(LocalBP));
        if (!bSaveResult)
            LocalWarnings.Add(TEXT("Blueprint was not written to disk during apply (save throttled, refused, or failed); check output log and call asset.save."));
    }
    if (bCompile && LocalBP)
    {
        CompileDiagnostics = CompileBlueprintWithDiagnostics(LocalBP);
        bCompileAttempted = true;
    }

    TSharedPtr<FJsonObject> ResultPayload = MakeShared<FJsonObject>();
    ResultPayload->SetStringField(TEXT("blueprintPath"), NormalizedBlueprintPath);
    ResultPayload->SetArrayField(TEXT("operations"), FinalSummaries);
    ResultPayload->SetBoolField(TEXT("compiled"),
        bCompileAttempted && CompileDiagnostics.bCompiled);
    ResultPayload->SetBoolField(TEXT("saved"), bSave && bSaveResult);
    if (bCompileAttempted)
    {
        AddCompileDiagnosticsToJson(
            CompileDiagnostics, ResultPayload, TEXT("compileErrors"), TEXT("compileWarnings"));
        BlueprintReinstancingGuard::AddSurveyToJson(ReinstancingSurvey, ResultPayload);
    }
    if (LocalWarnings.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> WVals;
        for (const FString& W : LocalWarnings)
            WVals.Add(MakeShared<FJsonValueString>(W));
        ResultPayload->SetArrayField(TEXT("warnings"), WVals);
    }

    if (bOk)
        Ctx.SendSuccess(ResultPayload);
    else
        Ctx.SendError(TEXT("SCS_OPERATION_FAILED"),
            *FString::Printf(TEXT("Processed %d SCS operation(s) with failures."), FinalSummaries.Num()));

    // Release busy flag
    BpTracker.ClearCurrentBusyState();

    return true;
}
