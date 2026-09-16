// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintFunctionHandler.cpp - Migrated from PinWright_BlueprintHandlers.cpp
// Blueprint function management: add_function

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "Misc/ScopeExit.h"
#include "Utils/AssetUtils.h"
#include "Utils/PropertyUtils.h"

#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"

using namespace BlueprintHandlerUtils;

static UEdGraph* FindBlueprintFunctionGraph(UBlueprint* Blueprint, const FString& FunctionName, bool* bOutIsMacro = nullptr)
{
    if (!Blueprint || FunctionName.TrimStartAndEnd().IsEmpty())
    {
        return nullptr;
    }

    const FString CleanName = FunctionName.TrimStartAndEnd();
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetName().Equals(CleanName, ESearchCase::IgnoreCase))
        {
            if (bOutIsMacro) { *bOutIsMacro = false; }
            return Graph;
        }
    }
    for (UEdGraph* Graph : Blueprint->MacroGraphs)
    {
        if (Graph && Graph->GetName().Equals(CleanName, ESearchCase::IgnoreCase))
        {
            if (bOutIsMacro) { *bOutIsMacro = true; }
            return Graph;
        }
    }
    return nullptr;
}

static UK2Node_FunctionEntry* FindBlueprintFunctionEntryNode(UEdGraph* Graph)
{
    if (!Graph)
    {
        return nullptr;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
        {
            return Entry;
        }
    }
    return nullptr;
}

static void CollectPhysicalDataPinDescriptors(
    const UEdGraphNode* Node,
    EEdGraphPinDirection Direction,
    TArray<FNamedPinTypeDescriptor>& OutDescriptors)
{
    if (!Node)
    {
        return;
    }

    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin
            && !Pin->bOrphanedPin
            && Pin->Direction == Direction
            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
        {
            FNamedPinTypeDescriptor Descriptor;
            Descriptor.Name = Pin->PinName.ToString();
            Descriptor.Type = Pin->PinType;
            OutDescriptors.Add(MoveTemp(Descriptor));
        }
    }
}

static void AppendPinDescriptorsJson(
    const TArray<FNamedPinTypeDescriptor>& Descriptors,
    TArray<TSharedPtr<FJsonValue>>& Out)
{
    for (const FNamedPinTypeDescriptor& Descriptor : Descriptors)
    {
        TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
        PinJson->SetStringField(TEXT("name"), Descriptor.Name);
        PinJson->SetStringField(TEXT("type"), DescribePinType(Descriptor.Type));
        Out.Add(MakeShared<FJsonValueObject>(PinJson));
    }
}

static void AppendPhysicalExecPinNames(
    const UEdGraphNode* Node,
    EEdGraphPinDirection Direction,
    TArray<TSharedPtr<FJsonValue>>& Out)
{
    if (!Node)
    {
        return;
    }

    for (const UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin
            && !Pin->bOrphanedPin
            && Pin->Direction == Direction
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            Out.Add(MakeShared<FJsonValueString>(Pin->PinName.ToString()));
        }
    }
}

static void BuildExpectedPhysicalPins(
    const TArray<FParsedPinParam>& Params,
    TArray<FNamedPinTypeDescriptor>& OutDescriptors)
{
    for (const FParsedPinParam& Param : Params)
    {
        FNamedPinTypeDescriptor Descriptor;
        Descriptor.Name = Param.Name.TrimStartAndEnd();
        if (!BuildNamedPinDescriptor(Descriptor.Name, Param.Spec, Descriptor))
        {
            Descriptor.Type = FEdGraphPinType{};
            Descriptor.Type.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
        }
        OutDescriptors.Add(MoveTemp(Descriptor));
    }
}

// ---- blueprint.add_function ----
REGISTER_RPC_HANDLER("blueprint.add_function", "blueprint", "Create a new UFunction graph on a Blueprint with caller-specified input/output pins. To author the body, follow with blueprint.compile_bpir or blueprint.graph.create_node calls scoped to the new graph. Set 'override' to true to override a BlueprintNativeEvent / BlueprintImplementableEvent inherited from the parent.",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path.")),
        RPC_PARAM_REQ_ALIAS("functionName", "string", "Function identifier (also accepted as 'name' or 'memberName').", "memberName"),
        RPC_PARAM_OPT("inputs", "array", "Array of {name, type} pin definitions for inputs; type accepts the same tokens as blueprint.add_variable's variableType."),
        RPC_PARAM_OPT("outputs", "array", "Array of {name, type} pin definitions for outputs."),
        RPC_PARAM_OPT("override", "boolean", "When true, creates an override of a parent BlueprintNativeEvent / BlueprintImplementableEvent with matching name; defaults to false."),
        RPC_PARAM_DEF("isPublic", "boolean", "Marks the function callable from outside the BP class when true; defaults to false (private).", "false")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.add_function requires a blueprint path."));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    // Accept 'functionName', 'name', or 'memberName'
    FString FuncName;
    if (!Payload->TryGetStringField(TEXT("functionName"), FuncName) || FuncName.IsEmpty())
    {
        if (!Payload->TryGetStringField(TEXT("name"), FuncName) || FuncName.IsEmpty())
            Payload->TryGetStringField(TEXT("memberName"), FuncName);
    }
    if (FuncName.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("functionName, name, or memberName required. Example: {\"functionName\": \"MyFunction\"}"));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* InputsField = nullptr;
    Payload->TryGetArrayField(TEXT("inputs"), InputsField);
    const TArray<TSharedPtr<FJsonValue>>* OutputsField = nullptr;
    Payload->TryGetArrayField(TEXT("outputs"), OutputsField);
    TArray<TSharedPtr<FJsonValue>> Inputs =
        (InputsField && InputsField->Num() > 0) ? *InputsField : TArray<TSharedPtr<FJsonValue>>();
    TArray<TSharedPtr<FJsonValue>> Outputs =
        (OutputsField && OutputsField->Num() > 0) ? *OutputsField : TArray<TSharedPtr<FJsonValue>>();
    const bool bOverride = GetJsonBoolField(Payload, TEXT("override"), false);
    const bool bIsPublic = Payload->HasField(TEXT("isPublic"))
        ? GetJsonBoolField(Payload, TEXT("isPublic")) : false;

    FString PinShapeError;
    if (!ValidateNamedTypePinParamElements(Inputs, TEXT("inputs"), PinShapeError)
        || !ValidateNamedTypePinParamElements(Outputs, TEXT("outputs"), PinShapeError))
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), *PinShapeError);
        return true;
    }

    if (FPluginState::Get().Blueprints().IsBusy(Path))
    {
        Ctx.SendError(TEXT("BLUEPRINT_BUSY"), TEXT("Blueprint is busy"));
        return true;
    }

    FPluginState::Get().Blueprints().MarkBusy(Path);
    ON_SCOPE_EXIT
    {
        if (FPluginState::Get().Blueprints().IsBusy(Path))
            FPluginState::Get().Blueprints().ClearBusy(Path);
    };

    auto* Subsystem = Ctx.GetSubsystem();
    FString Normalized, LoadErr;
    UBlueprint* Blueprint = LoadBlueprintAsset(Path, Normalized, LoadErr);
    const FString RegistryKey = !Normalized.IsEmpty() ? Normalized : Path;
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
        return true;
    }
    const bool bEffectiveIsPublic = bIsPublic || Blueprint->BlueprintType == BPTYPE_Interface;

    BlueprintHandlerUtils::FBlueprintOverrideInfo OverrideInfo;
    FString OverrideError;
    if (bOverride
        && !BlueprintHandlerUtils::TryResolveBlueprintOverride(
            Blueprint,
            FuncName,
            OverrideInfo,
            OverrideError))
    {
        Ctx.SendError(TEXT("INVALID_OVERRIDE"), *OverrideError);
        return true;
    }

    TArray<FParsedPinParam> ParsedInputs;
    TArray<FParsedPinParam> ParsedOutputs;
    FString ParamParseError;
    ParseNamedTypePinParams(Inputs, ParsedInputs, EParsedPinParamMode::AllowWildcardFallback, ParamParseError, TEXT("input param"));
    ParseNamedTypePinParams(Outputs, ParsedOutputs, EParsedPinParamMode::AllowWildcardFallback, ParamParseError, TEXT("output param"));

    // New-function path: reject any input/output token that would silently become a
    // wildcard pin (e.g. the documented-but-unsupported 'class:/Script/X.Y' form)
    // instead of returning success with a malformed pin that only fails at a later
    // compile. The override path below has its own stricter signature-match check.
    if (!bOverride && RejectWildcardPinParams(Ctx, ParsedInputs, ParsedOutputs))
    {
        return true;
    }

    if (bOverride && (Inputs.Num() > 0 || Outputs.Num() > 0))
    {
        TArray<FNamedPinTypeDescriptor> ExpectedInputs;
        TArray<FNamedPinTypeDescriptor> ExpectedOutputs;

        for (const FParsedPinParam& P : ParsedInputs)
        {
            if (!P.bParseOk)
            {
                // Surface the structured parse error detail so callers can fix the input.
                Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                    *FString::Printf(TEXT("Invalid override input signature: type '%s' — %s"),
                        *P.OriginalType, *BpirTypeSpecParser::FormatTypeSpecErrorDetail(P.ParseErr, P.ParseErrCol)));
                return true;
            }

            FNamedPinTypeDescriptor Descriptor;
            if (!BuildNamedPinDescriptor(P.Name, P.Spec, Descriptor))
            {
                // BuildNamedPinDescriptor failed on a successfully-parsed spec (e.g.
                // unresolved enum/struct) — no parse detail to surface here.
                Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Invalid override input signature"));
                return true;
            }
            ExpectedInputs.Add(Descriptor);
        }

        for (const FParsedPinParam& P : ParsedOutputs)
        {
            if (!P.bParseOk)
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                    *FString::Printf(TEXT("Invalid override output signature: type '%s' — %s"),
                        *P.OriginalType, *BpirTypeSpecParser::FormatTypeSpecErrorDetail(P.ParseErr, P.ParseErrCol)));
                return true;
            }

            FNamedPinTypeDescriptor Descriptor;
            if (!BuildNamedPinDescriptor(P.Name, P.Spec, Descriptor))
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Invalid override output signature"));
                return true;
            }
            ExpectedOutputs.Add(Descriptor);
        }

        TArray<FNamedPinTypeDescriptor> ActualInputs;
        TArray<FNamedPinTypeDescriptor> ActualOutputs;
        FString SignatureError;
        if (!GetFunctionSignatureDescriptors(
            OverrideInfo.Function,
            ActualInputs,
            ActualOutputs,
            SignatureError))
        {
            Ctx.SendError(TEXT("INVALID_OVERRIDE"), *SignatureError);
            return true;
        }

        FString MismatchMessage;
        if (!DoPinTypeDescriptorsMatch(
            ExpectedInputs,
            ExpectedOutputs,
            ActualInputs,
            ActualOutputs,
            MismatchMessage))
        {
            Ctx.SendError(TEXT("INVALID_OVERRIDE"), *MismatchMessage);
            return true;
        }
    }

    UEdGraph* ExistingGraph = BlueprintHandlerUtils::FindFunctionGraphByName(Blueprint, FuncName);

    if (bOverride && OverrideInfo.bCanPlaceAsEvent)
    {
        if (UK2Node_Event* ExistingOverrideEvent = FBlueprintEditorUtils::FindOverrideForFunction(
            Blueprint,
            OverrideInfo.OverrideClass,
            OverrideInfo.Function->GetFName()))
        {
            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetBoolField(TEXT("success"), true);
            Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
            Resp->SetStringField(TEXT("functionName"), FuncName);
            Resp->SetBoolField(TEXT("override"), true);
            Resp->SetBoolField(TEXT("eventOverride"), true);
            Resp->SetStringField(TEXT("note"), TEXT("Override already exists"));
            Ctx.SendSuccess(Resp);
            return true;
        }
    }

    if (ExistingGraph)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
        Resp->SetStringField(TEXT("functionName"), ExistingGraph->GetName());
        if (bOverride)
        {
            Resp->SetBoolField(TEXT("override"), true);
            Resp->SetBoolField(TEXT("eventOverride"), false);
        }
        Resp->SetStringField(TEXT("note"), TEXT("Function already exists"));
        Ctx.SendSuccess(Resp);
        return true;
    }

    TUniquePtr<FScopedTransaction> FunctionTransaction;
    UEdGraph* NewGraph = nullptr;
    bool bFunctionGraphCommitted = false;
    ON_SCOPE_EXIT
    {
        if (!bFunctionGraphCommitted && FunctionTransaction)
        {
            if (Blueprint && NewGraph && Blueprint->FunctionGraphs.Contains(NewGraph))
            {
                Blueprint->Modify();
                FBlueprintEditorUtils::RemoveGraph(Blueprint, NewGraph);
            }
            FunctionTransaction->Cancel();
        }
    };

    if (bOverride && OverrideInfo.bCanPlaceAsEvent)
    {
        UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
        if (!EventGraph)
        {
            Ctx.SendError(TEXT("GRAPH_UNAVAILABLE"), TEXT("Failed to find event graph for override"));
            return true;
        }

        int32 NodePosY = 0;
        UK2Node_Event* NewEventNode = FKismetEditorUtilities::AddDefaultEventNode(
            Blueprint,
            EventGraph,
            OverrideInfo.Function->GetFName(),
            OverrideInfo.OverrideClass,
            NodePosY);
        if (!NewEventNode)
        {
            Ctx.SendError(TEXT("GRAPH_UNAVAILABLE"), TEXT("Failed to create override event node"));
            return true;
        }
    }
    else
    {
        FunctionTransaction = MakeUnique<FScopedTransaction>(
            FText::FromString(FString::Printf(TEXT("Add Function '%s'"), *FuncName)));
        Blueprint->Modify();

        NewGraph = FBlueprintEditorUtils::CreateNewGraph(
            Blueprint, FName(*FuncName), UEdGraph::StaticClass(),
            UEdGraphSchema_K2::StaticClass());
        if (!NewGraph)
        {
            Ctx.SendError(TEXT("GRAPH_UNAVAILABLE"), TEXT("Failed to create function graph"));
            return true;
        }

        if (bOverride)
        {
            FBlueprintEditorUtils::AddFunctionGraph(
                Blueprint, NewGraph, /*bIsUserCreated=*/false, OverrideInfo.OverrideClass);
        }
        else
        {
            FBlueprintEditorUtils::CreateFunctionGraph<UFunction>(
                Blueprint, NewGraph, /*bIsUserCreated=*/true, nullptr);
            if (!Blueprint->FunctionGraphs.Contains(NewGraph))
            {
                FBlueprintEditorUtils::AddFunctionGraph<UClass>(
                    Blueprint, NewGraph, /*bIsUserCreated=*/true, nullptr);
            }
        }
    }

    if (bOverride && OverrideInfo.bCanPlaceAsEvent)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        const FBlueprintCompileDiagnostics Diagnostics = CompileBlueprintWithDiagnostics(Blueprint);
        const bool bSaved =
            Diagnostics.bCompiled && WasSavePersisted(SaveLoadedAssetThrottled(Blueprint));

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), Diagnostics.bCompiled);
        Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
        Resp->SetStringField(TEXT("functionName"), FuncName);
        Resp->SetBoolField(TEXT("saved"), bSaved);
        Resp->SetBoolField(TEXT("override"), true);
        Resp->SetBoolField(TEXT("eventOverride"), true);
        AddCompileDiagnosticsToJson(Diagnostics, Resp);
        AddAssetVerification(Resp, Blueprint);
        Ctx.SendSuccess(Resp);
        return true;
    }

    // Find entry and result nodes.
    TArray<UK2Node_FunctionEntry*> EntryNodes;
    TArray<UK2Node_FunctionResult*> ResultNodes;
    for (UEdGraphNode* Node : NewGraph->Nodes)
    {
        if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(Node))
        {
            EntryNodes.Add(AsEntry);
            continue;
        }
        if (UK2Node_FunctionResult* AsResult = Cast<UK2Node_FunctionResult>(Node))
            ResultNodes.Add(AsResult);
    }

    UK2Node_FunctionEntry* EntryNode = EntryNodes.Num() > 0 ? EntryNodes[0] : nullptr;
    UK2Node_FunctionResult* ResultNode = ResultNodes.Num() > 0 ? ResultNodes[0] : nullptr;

    // Remove duplicate entry/result nodes
    if (EntryNodes.Num() > 1 || ResultNodes.Num() > 1)
    {
        NewGraph->Modify();
        for (int32 EntryIdx = 1; EntryIdx < EntryNodes.Num(); ++EntryIdx)
        {
            if (UK2Node_FunctionEntry* ExtraEntry = EntryNodes[EntryIdx])
            {
                ExtraEntry->Modify();
                ExtraEntry->DestroyNode();
            }
        }
        for (int32 ResultIdx = 1; ResultIdx < ResultNodes.Num(); ++ResultIdx)
        {
            if (UK2Node_FunctionResult* ExtraResult = ResultNodes[ResultIdx])
            {
                ExtraResult->Modify();
                ExtraResult->DestroyNode();
            }
        }
        // Refresh surviving pointers
        EntryNode = nullptr;
        ResultNode = nullptr;
        for (UEdGraphNode* Node : NewGraph->Nodes)
        {
            if (!EntryNode)
            {
                EntryNode = Cast<UK2Node_FunctionEntry>(Node);
                if (EntryNode) continue;
            }
            if (!ResultNode)
                ResultNode = Cast<UK2Node_FunctionResult>(Node);
            if (EntryNode && ResultNode) break;
        }
    }

    if (!EntryNode)
    {
        Ctx.SendError(TEXT("GRAPH_UNAVAILABLE"), TEXT("Failed to find function entry node"));
        return true;
    }

    EntryNode->Modify();
    if (ParsedOutputs.Num() > 0 && !ResultNode)
    {
        ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
        if (!ResultNode)
        {
            Ctx.SendError(TEXT("GRAPH_UNAVAILABLE"), TEXT("Failed to create function result node"));
            return true;
        }
    }
    if (ResultNode)
    {
        ResultNode->Modify();
    }

    int32 ExtraFlags = EntryNode->GetExtraFlags();
    ExtraFlags &= ~FUNC_AccessSpecifiers;
    ExtraFlags |= bEffectiveIsPublic ? FUNC_Public : FUNC_Private;
    EntryNode->SetExtraFlags(ExtraFlags);

    // Add input parameters — feed the already-parsed specs. Parse misses log
    // then hand a default (wildcard) spec to AddUserDefinedPin.
    for (const FParsedPinParam& P : ParsedInputs)
    {
        if (!P.bParseOk)
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("blueprint.add_function: could not parse input param type '%s' (%s); falling back to wildcard pin"),
                *P.OriginalType, *P.ParseErr);
        }
        const FName CleanPinName(*P.Name.TrimStartAndEnd());
        if ((!bOverride || !EntryNode->FindPin(CleanPinName, EGPD_Output))
            && !AddUserDefinedPin(EntryNode, P.Name, P.Spec, EGPD_Output))
        {
            Ctx.SendError(TEXT("PIN_CREATION_FAILED"),
                *FString::Printf(TEXT("Failed to create function input pin '%s'."), *P.Name));
            return true;
        }
    }

    // Add output parameters
    for (const FParsedPinParam& P : ParsedOutputs)
    {
        if (!P.bParseOk)
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("blueprint.add_function: could not parse output param type '%s' (%s); falling back to wildcard pin"),
                *P.OriginalType, *P.ParseErr);
        }
        const FName CleanPinName(*P.Name.TrimStartAndEnd());
        if ((!bOverride || !ResultNode->FindPin(CleanPinName, EGPD_Input))
            && !AddUserDefinedPin(ResultNode, P.Name, P.Spec, EGPD_Input))
        {
            Ctx.SendError(TEXT("PIN_CREATION_FAILED"),
                *FString::Printf(TEXT("Failed to create function output pin '%s'."), *P.Name));
            return true;
        }
    }

    EntryNode->ReconstructNode();
    if (ResultNode)
    {
        ResultNode->ReconstructNode();
    }

    TArray<FNamedPinTypeDescriptor> ExpectedPhysicalInputs;
    TArray<FNamedPinTypeDescriptor> ExpectedPhysicalOutputs;
    if (bOverride)
    {
        FString SignatureError;
        if (!GetFunctionSignatureDescriptors(
            OverrideInfo.Function,
            ExpectedPhysicalInputs,
            ExpectedPhysicalOutputs,
            SignatureError))
        {
            Ctx.SendError(TEXT("INVALID_OVERRIDE"), *SignatureError);
            return true;
        }
    }
    else
    {
        BuildExpectedPhysicalPins(ParsedInputs, ExpectedPhysicalInputs);
        BuildExpectedPhysicalPins(ParsedOutputs, ExpectedPhysicalOutputs);
    }

    TArray<FNamedPinTypeDescriptor> ActualPhysicalInputs;
    TArray<FNamedPinTypeDescriptor> ActualPhysicalOutputs;
    CollectPhysicalDataPinDescriptors(EntryNode, EGPD_Output, ActualPhysicalInputs);
    CollectPhysicalDataPinDescriptors(ResultNode, EGPD_Input, ActualPhysicalOutputs);

    FString PhysicalPinError;
    if (!DoPinTypeDescriptorsMatch(
        ExpectedPhysicalInputs,
        ExpectedPhysicalOutputs,
        ActualPhysicalInputs,
        ActualPhysicalOutputs,
        PhysicalPinError))
    {
        Ctx.SendError(TEXT("PIN_CREATION_FAILED"), *PhysicalPinError);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> MeasuredInputs;
    TArray<TSharedPtr<FJsonValue>> MeasuredOutputs;
    AppendPinDescriptorsJson(ActualPhysicalInputs, MeasuredInputs);
    AppendPinDescriptorsJson(ActualPhysicalOutputs, MeasuredOutputs);

    bFunctionGraphCommitted = true;
    FunctionTransaction.Reset();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const FBlueprintCompileDiagnostics CompileDiagnostics = CompileBlueprintWithDiagnostics(Blueprint);
    // WasSavePersisted, not the raw call: the throttle skip and the transient-package
    // early-out used to return true, so a second edit inside the 0.5s window reported
    // saved:true with the change still only in memory.
    const bool bSaved = CompileDiagnostics.bCompiled
        && WasSavePersisted(SaveLoadedAssetThrottled(Blueprint));

    // Update registry
    TSharedPtr<FJsonObject> Entry = EnsureBlueprintEntry(RegistryKey);
    TArray<TSharedPtr<FJsonValue>> Funcs =
        Entry->HasField(TEXT("functions"))
            ? Entry->GetArrayField(TEXT("functions"))
            : TArray<TSharedPtr<FJsonValue>>();
    bool bFound = false;
    for (const TSharedPtr<FJsonValue>& Value : Funcs)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object) continue;
        const TSharedPtr<FJsonObject> Obj = Value->AsObject();
        if (!Obj.IsValid()) continue;
        FString Existing;
        if (Obj->TryGetStringField(TEXT("name"), Existing) &&
            Existing.Equals(FuncName, ESearchCase::IgnoreCase))
        {
            Obj->SetBoolField(TEXT("public"), bEffectiveIsPublic);
            if (MeasuredInputs.Num() > 0) Obj->SetArrayField(TEXT("inputs"), MeasuredInputs);
            else Obj->RemoveField(TEXT("inputs"));
            if (MeasuredOutputs.Num() > 0) Obj->SetArrayField(TEXT("outputs"), MeasuredOutputs);
            else Obj->RemoveField(TEXT("outputs"));
            bFound = true;
            break;
        }
    }

    if (!bFound)
    {
        TSharedPtr<FJsonObject> Rec = MakeShared<FJsonObject>();
        Rec->SetStringField(TEXT("name"), FuncName);
        Rec->SetBoolField(TEXT("public"), bEffectiveIsPublic);
        if (MeasuredInputs.Num() > 0) Rec->SetArrayField(TEXT("inputs"), MeasuredInputs);
        if (MeasuredOutputs.Num() > 0) Rec->SetArrayField(TEXT("outputs"), MeasuredOutputs);
        Funcs.Add(MakeShared<FJsonValueObject>(Rec));
    }

    Entry->SetArrayField(TEXT("functions"), Funcs);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), CompileDiagnostics.bCompiled);
    Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
    Resp->SetStringField(TEXT("functionName"), FuncName);
    Resp->SetBoolField(TEXT("public"), bEffectiveIsPublic);
    Resp->SetBoolField(TEXT("override"), bOverride);
    if (bOverride)
    {
        Resp->SetBoolField(TEXT("eventOverride"), false);
    }
    Resp->SetBoolField(TEXT("saved"), bSaved);
    if (MeasuredInputs.Num() > 0) Resp->SetArrayField(TEXT("inputs"), MeasuredInputs);
    if (MeasuredOutputs.Num() > 0) Resp->SetArrayField(TEXT("outputs"), MeasuredOutputs);
    AddCompileDiagnosticsToJson(CompileDiagnostics, Resp);
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- blueprint.add_macro ----
REGISTER_RPC_HANDLER("blueprint.add_macro", "blueprint", "Create a new Blueprint macro graph with caller-specified tunnel input/output pins. To author the body, follow with blueprint.graph.create_node calls scoped to the returned graphName.",
    RPC_PARAMS(
        BlueprintPathParamReq(
            TEXT("path"),
            TEXT("path"),
            TEXT("Blueprint asset path (also accepted as 'blueprintPath')."),
            EBlueprintPathParamAliasSet::ResolveExplicitBlueprintPath),
        RPC_PARAM_REQ_ALIAS("macroName", "string", "Macro graph name (also accepted as 'name').", "name"),
        RPC_PARAM_OPT("inputs", "array", "Array of {name, type} pin definitions. Pins are added to the entry tunnel as outputs."),
        RPC_PARAM_OPT("outputs", "array", "Array of {name, type} pin definitions. Pins are added to the exit tunnel as inputs."),
        RPC_PARAM_OPT("execExits", "array", "Optional array of exit exec pin names. When present, adds entry exec output 'execute' and exit exec inputs named exactly from this array.")
    ))
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    const FString Path = ResolveExplicitBlueprintPath(Payload);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.add_macro requires a blueprint path."));
        return true;
    }

    FString MacroName;
    if (!Payload->TryGetStringField(TEXT("macroName"), MacroName) || MacroName.IsEmpty())
    {
        Payload->TryGetStringField(TEXT("name"), MacroName);
    }
    MacroName = MacroName.TrimStartAndEnd();
    if (MacroName.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("macroName or name required. Example: {\"macroName\": \"MyMacro\"}"));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* InputsField = nullptr;
    Payload->TryGetArrayField(TEXT("inputs"), InputsField);
    const TArray<TSharedPtr<FJsonValue>>* OutputsField = nullptr;
    Payload->TryGetArrayField(TEXT("outputs"), OutputsField);
    const TArray<TSharedPtr<FJsonValue>>* ExecExitsField = nullptr;
    const bool bHasExecExits = Payload->TryGetArrayField(TEXT("execExits"), ExecExitsField);

    TArray<TSharedPtr<FJsonValue>> Inputs =
        (InputsField && InputsField->Num() > 0) ? *InputsField : TArray<TSharedPtr<FJsonValue>>();
    TArray<TSharedPtr<FJsonValue>> Outputs =
        (OutputsField && OutputsField->Num() > 0) ? *OutputsField : TArray<TSharedPtr<FJsonValue>>();

    TArray<FParsedPinParam> ParsedInputs;
    TArray<FParsedPinParam> ParsedOutputs;
    FString ParamParseError;
    // Unlike blueprint.add_function / networking.create_rpc_function (which reject an
    // unresolved type via RejectWildcardPinParams), add_macro intentionally keeps the
    // wildcard fallback: macro tunnel pins legitimately support PC_Wildcard so a generic
    // macro can operate over arbitrary types, so an unresolved type is NOT an error here.
    ParseNamedTypePinParams(Inputs, ParsedInputs, EParsedPinParamMode::AllowWildcardFallback, ParamParseError, TEXT("input param"));
    ParseNamedTypePinParams(Outputs, ParsedOutputs, EParsedPinParamMode::AllowWildcardFallback, ParamParseError, TEXT("output param"));

    TArray<FString> ExecExitNames;
    if (bHasExecExits && ExecExitsField)
    {
        for (const TSharedPtr<FJsonValue>& Value : *ExecExitsField)
        {
            if (!Value.IsValid() || Value->Type != EJson::String)
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("execExits must be an array of strings."));
                return true;
            }
            const FString ExecName = Value->AsString();
            if (ExecName.TrimStartAndEnd().IsEmpty())
            {
                Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("execExits cannot contain empty names."));
                return true;
            }
            ExecExitNames.Add(ExecName);
        }
    }

    if (FPluginState::Get().Blueprints().IsBusy(Path))
    {
        Ctx.SendError(TEXT("BLUEPRINT_BUSY"), TEXT("Blueprint is busy"));
        return true;
    }

    FPluginState::Get().Blueprints().MarkBusy(Path);
    ON_SCOPE_EXIT
    {
        if (FPluginState::Get().Blueprints().IsBusy(Path))
            FPluginState::Get().Blueprints().ClearBusy(Path);
    };

    FString Normalized, LoadErr;
    UBlueprint* Blueprint = LoadBlueprintAsset(Path, Normalized, LoadErr);
    const FString RegistryKey = !Normalized.IsEmpty() ? Normalized : Path;
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
        return true;
    }

    for (UEdGraph* ExistingGraph : Blueprint->MacroGraphs)
    {
        if (ExistingGraph && ExistingGraph->GetName().Equals(MacroName, ESearchCase::IgnoreCase))
        {
            UK2Node_Tunnel* EntryTunnel = nullptr;
            UK2Node_Tunnel* ExitTunnel = nullptr;
            FindMacroTunnelPair(ExistingGraph, EntryTunnel, ExitTunnel);

            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetBoolField(TEXT("success"), true);
            Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
            Resp->SetStringField(TEXT("macroName"), ExistingGraph->GetName());
            Resp->SetStringField(TEXT("graphName"), ExistingGraph->GetName());
            if (EntryTunnel) Resp->SetStringField(TEXT("entryNodeId"), EntryTunnel->NodeGuid.ToString());
            if (ExitTunnel) Resp->SetStringField(TEXT("exitNodeId"), ExitTunnel->NodeGuid.ToString());
            Resp->SetBoolField(TEXT("saved"), false);
            Resp->SetStringField(TEXT("note"), TEXT("Macro already exists"));
            AddAssetVerification(Resp, Blueprint);
            Ctx.SendSuccess(Resp);
            return true;
        }
    }

    TUniquePtr<FScopedTransaction> MacroTransaction = MakeUnique<FScopedTransaction>(
        FText::FromString(FString::Printf(TEXT("Add Macro '%s'"), *MacroName)));
    UEdGraph* MacroGraph = nullptr;
    bool bMacroCommitted = false;
    ON_SCOPE_EXIT
    {
        if (!bMacroCommitted && MacroTransaction)
        {
            if (Blueprint && MacroGraph && Blueprint->MacroGraphs.Contains(MacroGraph))
            {
                Blueprint->Modify();
                FBlueprintEditorUtils::RemoveGraph(Blueprint, MacroGraph);
            }
            MacroTransaction->Cancel();
        }
    };

    Blueprint->Modify();

    MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint,
        FName(*MacroName),
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    if (!MacroGraph)
    {
        Ctx.SendError(TEXT("GRAPH_UNAVAILABLE"), TEXT("Failed to create macro graph"));
        return true;
    }
    MacroGraph->Modify();

    FBlueprintEditorUtils::AddMacroGraph(
        Blueprint, MacroGraph, /*bIsUserCreated=*/true, static_cast<UClass*>(nullptr));

    UK2Node_Tunnel* EntryTunnel = nullptr;
    UK2Node_Tunnel* ExitTunnel = nullptr;
    if (!FindMacroTunnelPair(MacroGraph, EntryTunnel, ExitTunnel))
    {
        Ctx.SendError(TEXT("GRAPH_UNAVAILABLE"), TEXT("Failed to find macro entry/exit tunnel nodes"));
        return true;
    }
    EntryTunnel->Modify();
    ExitTunnel->Modify();

    if (bHasExecExits)
    {
        FEdGraphPinType ExecPinType;
        ExecPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
        if (!EntryTunnel->CreateUserDefinedPin(FName(TEXT("execute")), ExecPinType, EGPD_Output))
        {
            Ctx.SendError(TEXT("PIN_CREATION_FAILED"),
                TEXT("Failed to create macro entry exec pin 'execute'."));
            return true;
        }
        for (const FString& ExecName : ExecExitNames)
        {
            if (!ExitTunnel->CreateUserDefinedPin(FName(*ExecName), ExecPinType, EGPD_Input))
            {
                Ctx.SendError(TEXT("PIN_CREATION_FAILED"),
                    *FString::Printf(TEXT("Failed to create macro exit exec pin '%s'."), *ExecName));
                return true;
            }
        }
    }

    for (const FParsedPinParam& P : ParsedInputs)
    {
        if (!P.bParseOk)
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("blueprint.add_macro: could not parse input param type '%s' (%s); falling back to wildcard pin"),
                *P.OriginalType, *P.ParseErr);
        }
        if (!AddUserDefinedPin(EntryTunnel, P.Name, P.Spec, EGPD_Output))
        {
            Ctx.SendError(TEXT("PIN_CREATION_FAILED"),
                *FString::Printf(TEXT("Failed to create macro input pin '%s'."), *P.Name));
            return true;
        }
    }

    for (const FParsedPinParam& P : ParsedOutputs)
    {
        if (!P.bParseOk)
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("blueprint.add_macro: could not parse output param type '%s' (%s); falling back to wildcard pin"),
                *P.OriginalType, *P.ParseErr);
        }
        if (!AddUserDefinedPin(ExitTunnel, P.Name, P.Spec, EGPD_Input))
        {
            Ctx.SendError(TEXT("PIN_CREATION_FAILED"),
                *FString::Printf(TEXT("Failed to create macro output pin '%s'."), *P.Name));
            return true;
        }
    }

    EntryTunnel->ReconstructNode();
    ExitTunnel->ReconstructNode();

    TArray<FNamedPinTypeDescriptor> ExpectedPhysicalInputs;
    TArray<FNamedPinTypeDescriptor> ExpectedPhysicalOutputs;
    BuildExpectedPhysicalPins(ParsedInputs, ExpectedPhysicalInputs);
    BuildExpectedPhysicalPins(ParsedOutputs, ExpectedPhysicalOutputs);

    TArray<FNamedPinTypeDescriptor> ActualPhysicalInputs;
    TArray<FNamedPinTypeDescriptor> ActualPhysicalOutputs;
    CollectPhysicalDataPinDescriptors(EntryTunnel, EGPD_Output, ActualPhysicalInputs);
    CollectPhysicalDataPinDescriptors(ExitTunnel, EGPD_Input, ActualPhysicalOutputs);

    FString PhysicalPinError;
    if (!DoPinTypeDescriptorsMatch(
        ExpectedPhysicalInputs,
        ExpectedPhysicalOutputs,
        ActualPhysicalInputs,
        ActualPhysicalOutputs,
        PhysicalPinError))
    {
        Ctx.SendError(TEXT("PIN_CREATION_FAILED"), *PhysicalPinError);
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> MeasuredInputs;
    TArray<TSharedPtr<FJsonValue>> MeasuredOutputs;
    TArray<TSharedPtr<FJsonValue>> MeasuredExecExits;
    AppendPinDescriptorsJson(ActualPhysicalInputs, MeasuredInputs);
    AppendPinDescriptorsJson(ActualPhysicalOutputs, MeasuredOutputs);
    AppendPhysicalExecPinNames(ExitTunnel, EGPD_Input, MeasuredExecExits);

    bMacroCommitted = true;
    MacroTransaction.Reset();

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const FBlueprintCompileDiagnostics CompileDiagnostics = CompileBlueprintWithDiagnostics(Blueprint);
    // WasSavePersisted, not the raw call: the throttle skip and the transient-package
    // early-out used to return true, so a second edit inside the 0.5s window reported
    // saved:true with the change still only in memory.
    const bool bSaved = CompileDiagnostics.bCompiled
        && WasSavePersisted(SaveLoadedAssetThrottled(Blueprint));
    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), CompileDiagnostics.bCompiled);
    Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
    Resp->SetStringField(TEXT("macroName"), MacroGraph->GetName());
    Resp->SetStringField(TEXT("graphName"), MacroGraph->GetName());
    Resp->SetStringField(TEXT("entryNodeId"), EntryTunnel->NodeGuid.ToString());
    Resp->SetStringField(TEXT("exitNodeId"), ExitTunnel->NodeGuid.ToString());
    Resp->SetBoolField(TEXT("saved"), bSaved);
    if (MeasuredInputs.Num() > 0) Resp->SetArrayField(TEXT("inputs"), MeasuredInputs);
    if (MeasuredOutputs.Num() > 0) Resp->SetArrayField(TEXT("outputs"), MeasuredOutputs);
    if (bHasExecExits)
    {
        Resp->SetArrayField(TEXT("execExits"), MeasuredExecExits);
    }
    AddCompileDiagnosticsToJson(CompileDiagnostics, Resp);
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- blueprint.set_function_settings ----
REGISTER_RPC_HANDLER("blueprint.set_function_settings", "blueprint", "Set Blueprint function settings (access, pure/const/exec, call-in-editor, category)",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ_ALIAS("functionName", "string", "Function name (also accepts 'name' or 'memberName')", "memberName"),
        RPC_PARAM_OPT("access", "string", "Access specifier: public, protected, or private"),
        RPC_PARAM_OPT("isPublic", "boolean", "Whether function is public"),
        RPC_PARAM_OPT("isProtected", "boolean", "Whether function is protected"),
        RPC_PARAM_OPT("isPrivate", "boolean", "Whether function is private"),
        RPC_PARAM_OPT("isPure", "boolean", "Whether function is pure"),
        RPC_PARAM_OPT("isConst", "boolean", "Whether function is const"),
        RPC_PARAM_OPT("isExec", "boolean", "Whether function is Exec"),
        RPC_PARAM_OPT("callInEditor", "boolean", "Whether function can be called in editor"),
        RPC_PARAM_OPT("category", "string", "Function category")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.set_function_settings requires a blueprint path."));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    FString FuncName;
    if (!Payload->TryGetStringField(TEXT("functionName"), FuncName) || FuncName.IsEmpty())
    {
        if (!Payload->TryGetStringField(TEXT("name"), FuncName) || FuncName.IsEmpty())
        {
            Payload->TryGetStringField(TEXT("memberName"), FuncName);
        }
    }
    if (FuncName.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("functionName, name, or memberName required"));
        return true;
    }

    auto ReadOptionalBool = [&Payload](std::initializer_list<const TCHAR*> Keys) -> TOptional<bool>
    {
        for (const TCHAR* Key : Keys)
        {
            if (Payload->HasField(Key))
            {
                return GetJsonBoolField(Payload, Key, false);
            }
        }
        return TOptional<bool>();
    };

    TOptional<int32> AccessSetting;
    FString Access;
    if (Payload->TryGetStringField(TEXT("access"), Access))
    {
        const FString AccessLower = Access.TrimStartAndEnd().ToLower();
        if (AccessLower == TEXT("public"))
        {
            AccessSetting = FUNC_Public;
        }
        else if (AccessLower == TEXT("protected"))
        {
            AccessSetting = FUNC_Protected;
        }
        else if (AccessLower == TEXT("private"))
        {
            AccessSetting = FUNC_Private;
        }
        else
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("access must be one of: public, protected, private"));
            return true;
        }
    }

    const TOptional<bool> IsPublicSetting = ReadOptionalBool({ TEXT("isPublic"), TEXT("public") });
    const TOptional<bool> IsProtectedSetting = ReadOptionalBool({ TEXT("isProtected"), TEXT("protected") });
    const TOptional<bool> IsPrivateSetting = ReadOptionalBool({ TEXT("isPrivate"), TEXT("private") });

    int32 AccessSpecCount = 0;
    int32 AccessSpecFlag = FUNC_Public;
    if (IsPublicSetting.IsSet() && IsPublicSetting.GetValue())
    {
        AccessSpecCount++;
        AccessSpecFlag = FUNC_Public;
    }
    if (IsProtectedSetting.IsSet() && IsProtectedSetting.GetValue())
    {
        AccessSpecCount++;
        AccessSpecFlag = FUNC_Protected;
    }
    if (IsPrivateSetting.IsSet() && IsPrivateSetting.GetValue())
    {
        AccessSpecCount++;
        AccessSpecFlag = FUNC_Private;
    }

    if (AccessSpecCount > 1)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("Only one of isPublic, isProtected, or isPrivate can be true."));
        return true;
    }

    if (AccessSpecCount == 1)
    {
        if (AccessSetting.IsSet() && AccessSetting.GetValue() != AccessSpecFlag)
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("Conflicting access settings between access and isPublic/isProtected/isPrivate."));
            return true;
        }
        AccessSetting = AccessSpecFlag;
    }

    const TOptional<bool> IsPureSetting = ReadOptionalBool({ TEXT("isPure"), TEXT("pure") });
    const TOptional<bool> IsConstSetting = ReadOptionalBool({ TEXT("isConst"), TEXT("const") });
    const TOptional<bool> IsExecSetting = ReadOptionalBool({ TEXT("isExec"), TEXT("exec") });
    const TOptional<bool> CallInEditorSetting = ReadOptionalBool({ TEXT("callInEditor"), TEXT("isCallInEditor") });

    FString Category;
    const bool bHasCategory = Payload->TryGetStringField(TEXT("category"), Category);

    const bool bHasAnySetting =
        AccessSetting.IsSet() || IsPureSetting.IsSet() || IsConstSetting.IsSet() ||
        IsExecSetting.IsSet() || CallInEditorSetting.IsSet() || bHasCategory;
    if (!bHasAnySetting)
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("No function settings were provided."));
        return true;
    }

    FString Normalized, LoadErr;
    UBlueprint* Blueprint = LoadBlueprintAsset(Path, Normalized, LoadErr);
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
        return true;
    }

    const FString RegistryKey = !Normalized.IsEmpty() ? Normalized : Path;
    UEdGraph* FunctionGraph = FindBlueprintFunctionGraph(Blueprint, FuncName);
    if (!FunctionGraph)
    {
        Ctx.SendError(TEXT("NOT_FOUND"), TEXT("Function graph not found"));
        return true;
    }

    UK2Node_FunctionEntry* EntryNode = FindBlueprintFunctionEntryNode(FunctionGraph);
    const bool bRequiresEntryNode =
        AccessSetting.IsSet() || IsPureSetting.IsSet() || IsConstSetting.IsSet() ||
        IsExecSetting.IsSet() || CallInEditorSetting.IsSet();
    if (bRequiresEntryNode && !EntryNode)
    {
        Ctx.SendError(TEXT("GRAPH_UNAVAILABLE"), TEXT("Function entry node not found"));
        return true;
    }

    Blueprint->Modify();
    FunctionGraph->Modify();

    TArray<FString> AppliedSettings;

    if (EntryNode)
    {
        EntryNode->Modify();
        int32 ExtraFlags = EntryNode->GetExtraFlags();

        if (AccessSetting.IsSet())
        {
            ExtraFlags &= ~FUNC_AccessSpecifiers;
            ExtraFlags |= AccessSetting.GetValue();
            AppliedSettings.Add(TEXT("access"));
        }

        if (IsPureSetting.IsSet())
        {
            if (IsPureSetting.GetValue())
            {
                ExtraFlags |= FUNC_BlueprintPure;
            }
            else
            {
                ExtraFlags &= ~FUNC_BlueprintPure;
            }
            AppliedSettings.Add(TEXT("pure"));
        }

        if (IsConstSetting.IsSet())
        {
            if (IsConstSetting.GetValue())
            {
                ExtraFlags |= FUNC_Const;
            }
            else
            {
                ExtraFlags &= ~FUNC_Const;
            }
            AppliedSettings.Add(TEXT("const"));
        }

        if (IsExecSetting.IsSet())
        {
            if (IsExecSetting.GetValue())
            {
                ExtraFlags |= FUNC_Exec;
            }
            else
            {
                ExtraFlags &= ~FUNC_Exec;
            }
            AppliedSettings.Add(TEXT("exec"));
        }

        EntryNode->SetExtraFlags(ExtraFlags);

        if (CallInEditorSetting.IsSet())
        {
            EntryNode->MetaData.bCallInEditor = CallInEditorSetting.GetValue();
            AppliedSettings.Add(TEXT("callInEditor"));
        }
    }

    if (bHasCategory)
    {
        // A function/macro category is an editor-only organizational label, not
        // persisted localizable game/UI text — see MakeBlueprintCategoryText.
        const FText CategoryText = BlueprintHandlerUtils::MakeBlueprintCategoryText(Category);
        FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory(
            FunctionGraph, CategoryText, true);
        AppliedSettings.Add(TEXT("category"));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const FBlueprintCompileDiagnostics CompileDiagnostics = CompileBlueprintWithDiagnostics(Blueprint);
    // WasSavePersisted, not the raw call: the throttle skip and the transient-package
    // early-out used to return true, so a second edit inside the 0.5s window reported
    // saved:true with the change still only in memory.
    const bool bSaved = WasSavePersisted(SaveLoadedAssetThrottled(Blueprint));

    const TSharedPtr<FJsonObject> Snapshot = BuildBlueprintSnapshot(Blueprint, RegistryKey);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
    Resp->SetStringField(TEXT("functionName"), FunctionGraph->GetName());
    Resp->SetBoolField(TEXT("saved"), bSaved);
    TArray<TSharedPtr<FJsonValue>> AppliedArray;
    for (const FString& Setting : AppliedSettings)
    {
        AppliedArray.Add(MakeShared<FJsonValueString>(Setting));
    }
    Resp->SetArrayField(TEXT("appliedSettings"), AppliedArray);
    if (Snapshot.IsValid())
    {
        Resp->SetObjectField(TEXT("blueprint"), Snapshot);
        if (Snapshot->HasField(TEXT("functions")))
        {
            if (const TSharedPtr<FJsonObject> FunctionJson = FindNamedEntry(
                    Snapshot->GetArrayField(TEXT("functions")), TEXT("name"), FunctionGraph->GetName()))
            {
                Resp->SetObjectField(TEXT("function"), FunctionJson);
            }
        }
    }
    AddCompileDiagnosticsToJson(CompileDiagnostics, Resp);
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}

// ---- blueprint.remove_function ----
REGISTER_RPC_HANDLER("blueprint.remove_function", "blueprint", "Remove a function or macro graph from a Blueprint",
    RPC_PARAMS(
        BlueprintPathParamReq(TEXT("path"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_REQ_ALIAS("functionName", "string", "Name of the function or macro graph to remove (also accepts 'name' or 'memberName')", "memberName")
    ))
{
    FString Path = ResolveBlueprintPath(Ctx);
    if (Path.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_BLUEPRINT_PATH"), TEXT("blueprint.remove_function requires a blueprint path."));
        return true;
    }

    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();

    FString FuncName;
    if (!Payload->TryGetStringField(TEXT("functionName"), FuncName) || FuncName.IsEmpty())
    {
        if (!Payload->TryGetStringField(TEXT("name"), FuncName) || FuncName.IsEmpty())
            Payload->TryGetStringField(TEXT("memberName"), FuncName);
    }
    if (FuncName.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("functionName, name, or memberName required."));
        return true;
    }

    FString Normalized, LoadErr;
    UBlueprint* Blueprint = LoadBlueprintAsset(Path, Normalized, LoadErr);
    const FString RegistryKey = !Normalized.IsEmpty() ? Normalized : Path;
    if (!Blueprint)
    {
        Ctx.SendError(TEXT("BLUEPRINT_NOT_FOUND"),
            LoadErr.IsEmpty() ? TEXT("Failed to load blueprint") : *LoadErr);
        return true;
    }

    bool bIsMacro = false;
    UEdGraph* FunctionGraph = FindBlueprintFunctionGraph(Blueprint, FuncName, &bIsMacro);
    if (!FunctionGraph)
    {
        // Before returning idempotent success, scan the event graph for a matching
        // UK2Node_CustomEvent — callers often use remove_function for custom events too.
        TArray<UEdGraphNode*> EventRootNodes;
        if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
        {
            for (UEdGraphNode* Node : EventGraph->Nodes)
            {
                UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node);
                if (CustomEvent && CustomEvent->CustomFunctionName.ToString().Equals(
                        FuncName, ESearchCase::IgnoreCase))
                {
                    EventRootNodes.Add(CustomEvent);
                }
            }
        }

        if (EventRootNodes.Num() > 0)
        {
            TSet<UEdGraphNode*> AllNodesToRemove;
            CollectExecChainNodes(EventRootNodes, AllNodesToRemove);

            // Capture function names BEFORE removal — pointers may be invalid after RemoveNode.
            TSet<FName> RemovedFunctionNames;
            for (UEdGraphNode* RootNode : EventRootNodes)
            {
                const FName Name = GetEffectiveFunctionNameForRemoval(RootNode);
                if (Name != NAME_None)
                {
                    RemovedFunctionNames.Add(Name);
                }
            }

            FScopedTransaction Transaction(FText::FromString(FString::Printf(
                TEXT("Remove Custom Event '%s'"), *FuncName)));
            Blueprint->Modify();
            for (UEdGraphNode* Node : AllNodesToRemove)
            {
                FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
            }
            const int32 CascadedRemoved =
                CascadeRemoveStaleCreateDelegates(Blueprint, RemovedFunctionNames);
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            const FBlueprintCompileDiagnostics CompileDiagnostics = CompileBlueprintWithDiagnostics(Blueprint);
            // WasSavePersisted, not the raw call: the throttle skip and the
            // transient-package early-out used to return true, so a second edit
            // inside the 0.5s window reported saved:true with the change still
            // only in memory.
            const bool bSaved = WasSavePersisted(SaveLoadedAssetThrottled(Blueprint));

            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetBoolField(TEXT("success"), true);
            Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
            Resp->SetStringField(TEXT("functionName"), FuncName);
            Resp->SetStringField(TEXT("kind"), TEXT("custom_event"));
            Resp->SetStringField(TEXT("graphKind"), TEXT("event"));
            Resp->SetNumberField(TEXT("removedNodeCount"),
                static_cast<double>(AllNodesToRemove.Num()));
            Resp->SetNumberField(TEXT("cascadedCreateDelegatesRemoved"),
                static_cast<double>(CascadedRemoved));
            Resp->SetBoolField(TEXT("saved"), bSaved);
            AddCompileDiagnosticsToJson(CompileDiagnostics, Resp);
            AddAssetVerification(Resp, Blueprint);
            Ctx.SendSuccess(Resp);
            return true;
        }

        // Idempotent: function or macro doesn't exist, return success with note
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetBoolField(TEXT("success"), true);
        Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
        Resp->SetStringField(TEXT("functionName"), FuncName);
        Resp->SetStringField(TEXT("note"), TEXT("Function or macro not found — already removed or never existed"));
        Ctx.SendSuccess(Resp);
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(FString::Printf(
        TEXT("Remove Function '%s'"), *FuncName)));
    Blueprint->Modify();
    FBlueprintEditorUtils::RemoveGraph(Blueprint, FunctionGraph);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    const FBlueprintCompileDiagnostics CompileDiagnostics = CompileBlueprintWithDiagnostics(Blueprint);
    // WasSavePersisted, not the raw call: the throttle skip and the transient-package
    // early-out used to return true, so a second edit inside the 0.5s window reported
    // saved:true with the change still only in memory.
    const bool bSaved = WasSavePersisted(SaveLoadedAssetThrottled(Blueprint));

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("success"), true);
    Resp->SetStringField(TEXT("blueprintPath"), RegistryKey);
    Resp->SetStringField(TEXT("functionName"), FuncName);
    Resp->SetStringField(TEXT("graphKind"), bIsMacro ? TEXT("macro") : TEXT("function"));
    Resp->SetBoolField(TEXT("saved"), bSaved);
    AddCompileDiagnosticsToJson(CompileDiagnostics, Resp);
    AddAssetVerification(Resp, Blueprint);
    Ctx.SendSuccess(Resp);
    return true;
}
