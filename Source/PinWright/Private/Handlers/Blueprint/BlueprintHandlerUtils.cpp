// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintHandlerUtils.cpp - Implementation of shared Blueprint handler utilities
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Compat/EngineVersionCompat.h"
#include "Handlers/Blueprint/BlueprintEnumHelpers.h"


#include "PinWrightGlobals.h"
#include "PinWrightHelpers.h"
#include "PinWrightSubsystem.h"
#include "Utils/PropertyExport.h"
#include "Compiler/CodeNodeEmitter.h"
#include "Compiler/CodePinResolver.h"
#include "Compiler/BpirTypeSpec.h"
#include "Compiler/BpirTypeSpecParser.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Utils/JsonUtils.h"

// K2Node_FunctionEntry and K2Node_FunctionResult are not already included by the header
#if defined(__has_include)
#if __has_include("BlueprintGraph/K2Node_FunctionEntry.h")
#include "BlueprintGraph/K2Node_FunctionEntry.h"
#include "BlueprintGraph/K2Node_FunctionResult.h"
#elif __has_include("BlueprintGraph/Classes/K2Node_FunctionEntry.h")
#include "BlueprintGraph/Classes/K2Node_FunctionEntry.h"
#include "BlueprintGraph/Classes/K2Node_FunctionResult.h"
#elif __has_include("K2Node_FunctionEntry.h")
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#endif
#else
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#endif

#if defined(__has_include)
#if __has_include("BlueprintGraph/BlueprintMetadata.h")
#include "BlueprintGraph/BlueprintMetadata.h"
#elif __has_include("BlueprintMetadata.h")
#include "BlueprintMetadata.h"
#endif
#else
#include "BlueprintMetadata.h"
#endif

#include "K2Node.h"
#include "K2Node_Composite.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_InputKey.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Self.h"
#include "K2Node_Timeline.h"
#include "K2Node_Tunnel.h"
#include "K2Node_VariableGet.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "Engine/Engine.h"
#include "InputAction.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet/KismetTextLibrary.h"

#if defined(__has_include) && __has_include("K2Node_ComponentBoundEvent.h")
#include "K2Node_ComponentBoundEvent.h"
#endif

// Optional K2Node types for IsBlueprintEntryNode — same pattern as GraphWalker.cpp
#if __has_include("K2Node_InputAction.h")
#include "K2Node_InputAction.h"
#define MCP_BPUTILS_HAS_INPUT_ACTION 1
#else
#define MCP_BPUTILS_HAS_INPUT_ACTION 0
#endif
#if __has_include("K2Node_InputTouch.h")
#include "K2Node_InputTouch.h"
#define MCP_BPUTILS_HAS_INPUT_TOUCH 1
#else
#define MCP_BPUTILS_HAS_INPUT_TOUCH 0
#endif
#if __has_include("K2Node_ActorBoundEvent.h")
#include "K2Node_ActorBoundEvent.h"
#define MCP_BPUTILS_HAS_ACTOR_BOUND_EVENT 1
#else
#define MCP_BPUTILS_HAS_ACTOR_BOUND_EVENT 0
#endif
#if __has_include("K2Node_InputAxisEvent.h")
#include "K2Node_InputAxisEvent.h"
#define MCP_BPUTILS_HAS_INPUT_AXIS_EVENT 1
#else
#define MCP_BPUTILS_HAS_INPUT_AXIS_EVENT 0
#endif
#if __has_include("K2Node_InputAxisKeyEvent.h")
#include "K2Node_InputAxisKeyEvent.h"
#define MCP_BPUTILS_HAS_INPUT_AXIS_KEY_EVENT 1
#else
#define MCP_BPUTILS_HAS_INPUT_AXIS_KEY_EVENT 0
#endif

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/LinkerLoad.h"
#include "WidgetBlueprint.h"
#include "Handlers/UI/WidgetAuthoringUtils.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"

namespace BlueprintHandlerUtils
{

#if WITH_DEV_AUTOMATION_TESTS
static bool GForceParsedPinCreationFailureForTests = false;

void SetForceParsedPinCreationFailureForTests(bool bEnabled)
{
    GForceParsedPinCreationFailureForTests = bEnabled;
}
#endif

bool ArePinTypesEquivalent(const FEdGraphPinType& A, const FEdGraphPinType& B)
{
    return A == B;
}

FString JsonValueToString(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid()) return FString();
    switch (Value->Type)
    {
    case EJson::String: return Value->AsString();
    case EJson::Number: return LexToString(Value->AsNumber());
    case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
    case EJson::Null: return FString();
    default: break;
    }
    FString Serialized;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
    if (Value->Type == EJson::Object)
    {
        const TSharedPtr<FJsonObject> Obj = Value->AsObject();
        if (Obj.IsValid()) FJsonSerializer::Serialize(Obj.ToSharedRef(), *Writer, true);
    }
    else if (Value->Type == EJson::Array)
    {
        FJsonSerializer::Serialize(Value->AsArray(), *Writer, true);
    }
    else
    {
        Writer->WriteValue(Value->AsString());
    }
    Writer->Close();
    return Serialized;
}

FString ResolveExplicitBlueprintPath(const TSharedPtr<FJsonObject>& Payload, bool bNormalize)
{
    if (!Payload.IsValid())
    {
        return FString();
    }

    FString ResolvedPath;
    VisitBlueprintPathScalarFieldNames(
        EBlueprintPathParamAliasSet::ResolveExplicitBlueprintPath,
        [&Payload, bNormalize, &ResolvedPath](const TCHAR* FieldName)
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

            if (bNormalize)
            {
                FString Normalized;
                if (FindBlueprintNormalizedPath(Candidate, Normalized))
                {
                    Normalized = Normalized.TrimStartAndEnd();
                    if (!Normalized.IsEmpty())
                    {
                        ResolvedPath = Normalized;
                        return false;
                    }
                }
            }

            ResolvedPath = Candidate;
            return false;
        });

    return ResolvedPath;
}

bool ValidateNamedTypePinParamElements(
    const TArray<TSharedPtr<FJsonValue>>& In,
    const TCHAR* ParamName,
    FString& OutError)
{
    static const TArray<FString> AllowedKeys = { TEXT("name"), TEXT("type") };
    OutError.Reset();

    for (int32 Index = 0; Index < In.Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& Value = In[Index];
        const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(ObjectPtr) || !ObjectPtr || !ObjectPtr->IsValid())
        {
            OutError = FString::Printf(
                TEXT("%s[%d] must be an object with {name, type}"), ParamName, Index);
            return false;
        }

        TArray<FString> Unknown;
        if (!::RejectUnknownKeys(*ObjectPtr, AllowedKeys, Unknown, ERejectUnknownKeysMode::First, ESearchCase::IgnoreCase))
        {
            OutError = FString::Printf(
                TEXT("%s[%d].%s is not accepted. Valid keys: [name, type]"),
                ParamName, Index, *Unknown[0]);
            return false;
        }

        FString Name;
        FString Type;
        if (!(*ObjectPtr)->TryGetStringField(TEXT("name"), Name) || Name.TrimStartAndEnd().IsEmpty())
        {
            OutError = FString::Printf(TEXT("%s[%d].name must be a non-empty string"), ParamName, Index);
            return false;
        }
        if (!(*ObjectPtr)->TryGetStringField(TEXT("type"), Type) || Type.TrimStartAndEnd().IsEmpty())
        {
            OutError = FString::Printf(TEXT("%s[%d].type must be a non-empty string"), ParamName, Index);
            return false;
        }
    }
    return true;
}

bool ParseNamedTypePinParams(
    const TArray<TSharedPtr<FJsonValue>>& In,
    TArray<FParsedPinParam>& Out,
    EParsedPinParamMode Mode,
    FString& OutError,
    const TCHAR* StrictParamLabel)
{
    Out.Reset();
    OutError.Reset();

    for (const TSharedPtr<FJsonValue>& Value : In)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object)
        {
            continue;
        }

        const TSharedPtr<FJsonObject> Obj = Value->AsObject();
        if (!Obj.IsValid())
        {
            continue;
        }

        FParsedPinParam P;
        Obj->TryGetStringField(TEXT("name"), P.OriginalName);
        Obj->TryGetStringField(TEXT("type"), P.OriginalType);
        P.Name = P.OriginalName.TrimStartAndEnd();
        P.bParseOk = BpirTypeSpecParser::ParseTypeSpec(P.OriginalType, P.Spec, P.ParseErr, P.ParseErrCol);

        if (Mode == EParsedPinParamMode::Strict)
        {
            if (!P.bParseOk)
            {
                OutError = FString::Printf(
                    TEXT("Invalid %s '%s' type '%s': %s"),
                    StrictParamLabel,
                    *P.OriginalName,
                    *P.OriginalType,
                    *BpirTypeSpecParser::FormatTypeSpecErrorDetail(P.ParseErr, P.ParseErrCol));
                return false;
            }

            if (!ResolvesToConcretePin(P))
            {
                OutError = FString::Printf(
                    TEXT("Invalid %s '%s' type '%s'"),
                    StrictParamLabel,
                    *P.OriginalName,
                    *P.OriginalType);
                return false;
            }
        }

        Out.Add(MoveTemp(P));
    }

    return true;
}

FName ResolveMetadataKey(const FString& RawKey)
{
    if (RawKey.Equals(TEXT("displayname"), ESearchCase::IgnoreCase)) return FName(TEXT("DisplayName"));
    if (RawKey.Equals(TEXT("tooltip"), ESearchCase::IgnoreCase)) return FName(TEXT("ToolTip"));
    if (RawKey.Equals(TEXT("private"), ESearchCase::IgnoreCase))
    {
        return FBlueprintMetadata::MD_Private;
    }
    if (RawKey.Equals(TEXT("exposeonspawn"), ESearchCase::IgnoreCase))
    {
        return FBlueprintMetadata::MD_ExposeOnSpawn;
    }
    if (RawKey.Equals(TEXT("keywords"), ESearchCase::IgnoreCase))
    {
        return FBlueprintMetadata::MD_FunctionKeywords;
    }
    if (RawKey.Equals(TEXT("compactnodetitle"), ESearchCase::IgnoreCase))
    {
        return FBlueprintMetadata::MD_CompactNodeTitle;
    }
    if (RawKey.Equals(TEXT("callineditor"), ESearchCase::IgnoreCase))
    {
        return FBlueprintMetadata::MD_CallInEditor;
    }
    return FName(*RawKey);
}

FString DescribePinType(const FEdGraphPinType& PinType)
{
    FString BaseType = PinType.PinCategory.ToString();
    if (PinType.PinSubCategoryObject.IsValid())
    {
        if (const UObject* SubObj = PinType.PinSubCategoryObject.Get())
            BaseType = SubObj->GetName();
    }
    else if (PinType.PinSubCategory != NAME_None)
    {
        BaseType = PinType.PinSubCategory.ToString();
    }
    FString ContainerWrappedType = BaseType;
    switch (PinType.ContainerType)
    {
    case EPinContainerType::Array:
        ContainerWrappedType = FString::Printf(TEXT("Array<%s>"), *BaseType); break;
    case EPinContainerType::Set:
        ContainerWrappedType = FString::Printf(TEXT("Set<%s>"), *BaseType); break;
    case EPinContainerType::Map:
    {
        FString ValueType = PinType.PinValueType.TerminalCategory.ToString();
        if (PinType.PinValueType.TerminalSubCategoryObject.IsValid())
        {
            if (const UObject* ValueObj = PinType.PinValueType.TerminalSubCategoryObject.Get())
                ValueType = ValueObj->GetName();
        }
        else if (PinType.PinValueType.TerminalSubCategory != NAME_None)
        {
            ValueType = PinType.PinValueType.TerminalSubCategory.ToString();
        }
        ContainerWrappedType = FString::Printf(TEXT("Map<%s,%s>"), *BaseType, *ValueType);
        break;
    }
    default: break;
    }
    return ContainerWrappedType;
}

void AppendPinsJson(const TArray<TSharedPtr<FUserPinInfo>>& Pins, TArray<TSharedPtr<FJsonValue>>& Out)
{
    for (const TSharedPtr<FUserPinInfo>& PinInfo : Pins)
    {
        if (!PinInfo.IsValid()) continue;
        const FString PinName = PinInfo->PinName.ToString();
        if (PinName.IsEmpty()) continue;
        TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
        PinJson->SetStringField(TEXT("name"), PinName);
        PinJson->SetStringField(TEXT("type"), DescribePinType(PinInfo->PinType));
        Out.Add(MakeShared<FJsonValueObject>(PinJson));
    }
}

// Enum-pin helpers are implemented in BlueprintEnumHelpers.cpp; these
// namespace-scoped entry points forward to keep public callers (BpirCompiler,
// CodePinResolver) source-compatible with the BlueprintHandlerUtils:: qualifier.
FString StripEnumScope(const FString& InName)
{
    return BlueprintEnumHelpers::StripEnumScope(InName);
}

bool TryResolveEnumLiteralToValue(const UEnum* EnumType, const FString& InLiteral, int64& OutValue)
{
    return BlueprintEnumHelpers::TryResolveEnumLiteralToValue(EnumType, InLiteral, OutValue);
}

bool TryApplyEnumPinDefaultValue(
    const UEdGraphSchema* Schema,
    UEdGraphPin* Pin,
    const FString& RequestedValue,
    FString& OutAppliedLiteral,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    return BlueprintEnumHelpers::TryApplyEnumPinDefaultValue(
        Schema, Pin, RequestedValue, OutAppliedLiteral, OutErrorCode, OutErrorMessage);
}

// Engine bug workaround: K2Node_VariableSet.cpp:450 calls LOCTEXT(...).ToString()
// directly without FText::Format, so {VariableName} is never substituted in the
// "UnableToSet_ReadOnly" message.  The sibling "NotWritable" case (line 446) does
// it correctly.  We cannot patch engine source from the plugin, so detect and repair
// the known broken string after FTokenizedMessage::ToText() produces it.
//
// Input:  "{VariableName} is private and not accessible in this context.  Set <ID>"
// Output: "<ID> is private and not accessible in this context.  Set <ID>"
FString RepairUnableToSetReadOnlyMessage(const FString& In)
{
    static const FString Placeholder = TEXT("{VariableName} ");
    if (!In.StartsWith(Placeholder))
    {
        return In;
    }

    // Find "Set " — the @@-expanded node title appended by FTokenizedMessage.
    // There may be one or more whitespace chars between the period and "Set".
    const FString SetToken = TEXT("Set ");
    const int32 SetIdx = In.Find(SetToken, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (SetIdx == INDEX_NONE)
    {
        return In;
    }

    const int32 IdentStart = SetIdx + SetToken.Len();
    if (IdentStart >= In.Len())
    {
        return In;
    }

    // Extract identifier: runs to first whitespace or end of string.
    int32 IdentEnd = IdentStart;
    while (IdentEnd < In.Len() && !FChar::IsWhitespace(In[IdentEnd]))
    {
        ++IdentEnd;
    }

    const FString Identifier = In.Mid(IdentStart, IdentEnd - IdentStart);
    if (Identifier.IsEmpty())
    {
        return In;
    }

    // Rebuild: identifier + everything after the literal "{VariableName}" token
    // (preserving the space that followed it, so the sentence reads
    // "<ID> is private..." rather than "<ID>is private...").
    static const FString PlaceholderToken = TEXT("{VariableName}");
    return Identifier + In.Mid(PlaceholderToken.Len());
}

FBlueprintCompileDiagnostics CompileBlueprintWithDiagnostics(UBlueprint* Blueprint)
{
    FBlueprintCompileDiagnostics Diagnostics;
    Diagnostics.Status = TEXT("Unknown");

    if (!Blueprint)
    {
        Diagnostics.Errors.Add(TEXT("Missing Blueprint"));
        Diagnostics.Status = TEXT("Error");
        return Diagnostics;
    }

    if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Blueprint))
    {
        WidgetAuthoringHelpers::EnsureAllWidgetVariableGuids(WidgetBP);
    }

    // Measured BEFORE the compile: the flush of the reinstancing queue destroys these
    // objects, so surveying afterwards would report the rebuilt set (or nothing) and
    // the caller would never learn that another world's placed actors were rebuilt.
    // This is the shared compile path, so every verb that reports through
    // AddCompileDiagnosticsToJson discloses it without a call-site change.
    Diagnostics.Reinstanced = BlueprintReinstancingGuard::SurveyLiveInstances(Blueprint);

    FCompilerResultsLog ResultsLog;
    ResultsLog.bSilentMode = true;
    ResultsLog.bLogDetailedResults = true;
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &ResultsLog);
    // Avoid GC on the compile stack, then restore cleanup at the next engine GC opportunity.
    if (GEngine)
    {
        GEngine->ForceGarbageCollection(true);
    }

    switch (Blueprint->Status)
    {
    case EBlueprintStatus::BS_UpToDate:
        Diagnostics.Status = TEXT("UpToDate");
        Diagnostics.bCompiled = true;
        break;
    case EBlueprintStatus::BS_UpToDateWithWarnings:
        Diagnostics.Status = TEXT("UpToDateWithWarnings");
        Diagnostics.bCompiled = true;
        break;
    case EBlueprintStatus::BS_Error:
        Diagnostics.Status = TEXT("Error");
        break;
    default:
        Diagnostics.Status = TEXT("Unknown");
        break;
    }

    for (const TSharedRef<FTokenizedMessage>& Message : ResultsLog.Messages)
    {
        switch (Message->GetSeverity())
        {
        case EMessageSeverity::Error:
            // Repair applies only to error-severity engine messages (see comment
            // on RepairUnableToSetReadOnlyMessage).
            Diagnostics.Errors.Add(RepairUnableToSetReadOnlyMessage(Message->ToText().ToString()));
            break;
        case EMessageSeverity::Warning:
        case EMessageSeverity::PerformanceWarning:
            Diagnostics.Warnings.Add(Message->ToText().ToString());
            break;
        default:
            break;
        }
    }

    return Diagnostics;
}

bool HasNewCompileErrorsBeyondBaseline(
    const TArray<FString>& BaselineErrors,
    const TArray<FString>& PostErrors)
{
    // Multiset (not set) comparison: an error counts as NEW when PostErrors
    // contains it MORE times than BaselineErrors did. Plain set membership would
    // mask a genuinely-new occurrence of a non-node-specific message (e.g. a
    // generic "In use pin ... no longer exists") whose text is byte-for-byte
    // identical to one that already existed once in the baseline — reporting a
    // real regression as preexistingErrorsOnly. Counting occurrences closes that
    // hole. An empty PostErrors falls straight through the loop to return false.
    TMap<FString, int32> RemainingBaseline;
    for (const FString& Error : BaselineErrors)
    {
        RemainingBaseline.FindOrAdd(Error)++;
    }

    for (const FString& Error : PostErrors)
    {
        int32& Remaining = RemainingBaseline.FindOrAdd(Error);
        if (Remaining <= 0)
        {
            return true;
        }
        --Remaining;
    }
    return false;
}

void AddCompileDiagnosticsToJson(
    const FBlueprintCompileDiagnostics& Diagnostics,
    const TSharedPtr<FJsonObject>& Out,
    const TCHAR* ErrorsFieldName,
    const TCHAR* WarningsFieldName)
{
    if (!Out.IsValid())
    {
        return;
    }

    Out->SetBoolField(TEXT("compiled"), Diagnostics.bCompiled);
    Out->SetStringField(TEXT("status"), Diagnostics.Status);

    TArray<TSharedPtr<FJsonValue>> ErrorsArray;
    for (const FString& Error : Diagnostics.Errors)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("message"), Error);
        ErrorsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Out->SetArrayField(ErrorsFieldName, ErrorsArray);

    TArray<TSharedPtr<FJsonValue>> WarningsArray;
    for (const FString& Warning : Diagnostics.Warnings)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("message"), Warning);
        WarningsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Out->SetArrayField(WarningsFieldName, WarningsArray);

    // Only present when the compile actually rebuilt live instances - a `reinstanced`
    // block in a response is therefore a positive signal, not a field to skip past.
    BlueprintReinstancingGuard::AddSurveyToJson(Diagnostics.Reinstanced, Out);
}

TArray<TSharedPtr<FJsonValue>> BuildBlueprintIntegrityFailuresJson(
    const TArray<FBlueprintIntegrityFailure>& Failures)
{
    TArray<TSharedPtr<FJsonValue>> FailArr;
    FailArr.Reserve(Failures.Num());
    for (const FBlueprintIntegrityFailure& Fail : Failures)
    {
        TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
        FailObj->SetStringField(TEXT("nodeGuid"), Fail.NodeGuid);
        FailObj->SetStringField(TEXT("nodeKind"), Fail.NodeKind);
        FailObj->SetStringField(TEXT("graphName"), Fail.GraphName);
        FailObj->SetStringField(TEXT("reason"), Fail.Reason);
        FailArr.Add(MakeShared<FJsonValueObject>(FailObj));
    }
    return FailArr;
}

bool TryResolveBlueprintOverride(
    UBlueprint* Blueprint,
    const FString& FunctionName,
    FBlueprintOverrideInfo& OutInfo,
    FString& OutErrorMessage)
{
    OutInfo = FBlueprintOverrideInfo();
    OutErrorMessage.Reset();

    if (!Blueprint)
    {
        OutErrorMessage = TEXT("Missing Blueprint");
        return false;
    }

    UFunction* OverrideFunction = nullptr;
    UClass* OverrideClass = FBlueprintEditorUtils::GetOverrideFunctionClass(
        Blueprint,
        FName(*FunctionName),
        &OverrideFunction);

    // Fallback: GetOverrideFunctionClass requires SkeletonGeneratedClass which may
    // be null for Widget Blueprints or freshly-created BPs that haven't been compiled.
    // Search the parent class hierarchy directly as a fallback.
    if ((!OverrideFunction || !OverrideClass) && Blueprint->ParentClass)
    {
        UFunction* ParentFunc = Blueprint->ParentClass->FindFunctionByName(FName(*FunctionName));
        if (ParentFunc)
        {
            OverrideFunction = ParentFunc;
            OverrideClass = CastChecked<UClass>(ParentFunc->GetOuter())->GetAuthoritativeClass();
        }
    }

    if (!OverrideFunction || !OverrideClass)
    {
        OutErrorMessage = FString::Printf(
            TEXT("No overridable parent function named '%s' was found"),
            *FunctionName);
        return false;
    }

    if (!UEdGraphSchema_K2::CanKismetOverrideFunction(OverrideFunction))
    {
        OutErrorMessage = FString::Printf(
            TEXT("Function '%s' cannot be overridden in Blueprint"),
            *FunctionName);
        return false;
    }

    OutInfo.Function = OverrideFunction;
    OutInfo.OverrideClass = OverrideClass;
    OutInfo.bCanPlaceAsEvent = UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(OverrideFunction);
    return true;
}

UEdGraph* FindFunctionGraphByName(
    UBlueprint* Blueprint,
    const FString& FunctionName,
    bool* bOutIsInterfaceOwned)
{
    if (bOutIsInterfaceOwned)
    {
        *bOutIsInterfaceOwned = false;
    }
    if (!Blueprint || FunctionName.TrimStartAndEnd().IsEmpty())
    {
        return nullptr;
    }

    const FString CleanName = FunctionName.TrimStartAndEnd();
    for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
    {
        for (UEdGraph* Graph : Interface.Graphs)
        {
            if (Graph && Graph->GetName().Equals(CleanName, ESearchCase::IgnoreCase))
            {
                if (bOutIsInterfaceOwned)
                {
                    *bOutIsInterfaceOwned = true;
                }
                return Graph;
            }
        }
    }

    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (Graph && Graph->GetName().Equals(CleanName, ESearchCase::IgnoreCase))
        {
            return Graph;
        }
    }
    return nullptr;
}

bool BuildNamedPinDescriptor(
    const FString& Name,
    const FBpirTypeSpec& TypeSpec,
    FNamedPinTypeDescriptor& OutDescriptor)
{
    const FString CleanName = Name.TrimStartAndEnd();
    if (CleanName.IsEmpty() || TypeSpec.IsEmpty())
    {
        return false;
    }

    OutDescriptor.Name = CleanName;
    OutDescriptor.Type = FEdGraphPinType{};
    if (!FCodePinResolver::ConvertTypeSpecToPinType(TypeSpec, OutDescriptor.Type))
    {
        return false;
    }
    // Reject wildcard fallback — caller would otherwise silently create an untyped pin.
    if (OutDescriptor.Type.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
    {
        return false;
    }
    return true;
}

bool GetFunctionSignatureDescriptors(
    UFunction* Function,
    TArray<FNamedPinTypeDescriptor>& OutInputs,
    TArray<FNamedPinTypeDescriptor>& OutOutputs,
    FString& OutErrorMessage)
{
    OutInputs.Reset();
    OutOutputs.Reset();
    OutErrorMessage.Reset();

    if (!Function)
    {
        OutErrorMessage = TEXT("Missing function");
        return false;
    }

    const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
    if (!Schema)
    {
        OutErrorMessage = TEXT("Missing K2 schema");
        return false;
    }

    for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It)
    {
        const FProperty* Property = *It;
        if (!Property)
        {
            continue;
        }

        FEdGraphPinType PinType;
        if (!Schema->ConvertPropertyToPinType(Property, PinType))
        {
            OutErrorMessage = FString::Printf(
                TEXT("Failed to convert function property '%s' to pin type"),
                *Property->GetName());
            return false;
        }

        FNamedPinTypeDescriptor Descriptor;
        Descriptor.Name = Property->GetName();
        Descriptor.Type = PinType;

        // CPF_ReferenceParm implies CPF_OutParm even for `const T&` (see ObjectMacros.h),
        // so the const-ref check is required to avoid misclassifying input refs as outputs.
        // Mirrors the canonical UE pattern in EdGraphSchema_K2.cpp / KismetCompiler.cpp.
        const bool bIsOutput =
            Property->HasAnyPropertyFlags(CPF_ReturnParm)
            || (Property->HasAnyPropertyFlags(CPF_OutParm) && !Property->HasAnyPropertyFlags(CPF_ConstParm));

        if (bIsOutput)
        {
            OutOutputs.Add(Descriptor);
        }
        else
        {
            OutInputs.Add(Descriptor);
        }
    }

    return true;
}

bool DoPinTypeDescriptorsMatch(
    const TArray<FNamedPinTypeDescriptor>& ExpectedInputs,
    const TArray<FNamedPinTypeDescriptor>& ExpectedOutputs,
    const TArray<FNamedPinTypeDescriptor>& ActualInputs,
    const TArray<FNamedPinTypeDescriptor>& ActualOutputs,
    FString& OutMismatchMessage)
{
    OutMismatchMessage.Reset();

    auto CompareLists = [&OutMismatchMessage](
        const TCHAR* Label,
        const TArray<FNamedPinTypeDescriptor>& Expected,
        const TArray<FNamedPinTypeDescriptor>& Actual) -> bool
    {
        if (Expected.Num() != Actual.Num())
        {
            OutMismatchMessage = FString::Printf(
                TEXT("%s count mismatch: expected %d, actual %d"),
                Label,
                Expected.Num(),
                Actual.Num());
            return false;
        }

        TSet<FName> ExpectedNames;
        ExpectedNames.Reserve(Expected.Num());
        for (const FNamedPinTypeDescriptor& ExpectedDescriptor : Expected)
        {
            const FName NameKey(*ExpectedDescriptor.Name);
            if (ExpectedNames.Contains(NameKey))
            {
                OutMismatchMessage = FString::Printf(
                    TEXT("%s duplicate expected name '%s'"),
                    Label,
                    *ExpectedDescriptor.Name);
                return false;
            }
            ExpectedNames.Add(NameKey);
        }

        TMap<FName, const FNamedPinTypeDescriptor*> ActualByName;
        ActualByName.Reserve(Actual.Num());
        for (const FNamedPinTypeDescriptor& ActualDescriptor : Actual)
        {
            const FName NameKey(*ActualDescriptor.Name);
            if (ActualByName.Contains(NameKey))
            {
                OutMismatchMessage = FString::Printf(
                    TEXT("%s duplicate actual name '%s'"),
                    Label,
                    *ActualDescriptor.Name);
                return false;
            }
            ActualByName.Add(NameKey, &ActualDescriptor);
        }

        for (const FNamedPinTypeDescriptor& ExpectedDescriptor : Expected)
        {
            const FNamedPinTypeDescriptor* const* ActualMatch =
                ActualByName.Find(FName(*ExpectedDescriptor.Name));
            if (!ActualMatch)
            {
                OutMismatchMessage = FString::Printf(
                    TEXT("%s name mismatch: expected '%s' was not found"),
                    Label,
                    *ExpectedDescriptor.Name);
                return false;
            }

            const FNamedPinTypeDescriptor& ActualDescriptor = **ActualMatch;
            if (!ArePinTypesEquivalent(ExpectedDescriptor.Type, ActualDescriptor.Type))
            {
                OutMismatchMessage = FString::Printf(
                    TEXT("%s type mismatch for '%s': expected '%s', actual '%s'"),
                    Label,
                    *ExpectedDescriptor.Name,
                    *DescribePinType(ExpectedDescriptor.Type),
                    *DescribePinType(ActualDescriptor.Type));
                return false;
            }
        }

        return true;
    };

    return CompareLists(TEXT("Input"), ExpectedInputs, ActualInputs)
        && CompareLists(TEXT("Output"), ExpectedOutputs, ActualOutputs);
}

FProperty* FindBlueprintProperty(UBlueprint* Blueprint, const FString& PropertyName)
{
    if (!Blueprint || PropertyName.TrimStartAndEnd().IsEmpty()) return nullptr;
    const FName PropFName(*PropertyName.TrimStartAndEnd());
    const TArray<UClass*> CandidateClasses = {
        Blueprint->GeneratedClass, Blueprint->SkeletonGeneratedClass, Blueprint->ParentClass };
    for (UClass* Candidate : CandidateClasses)
    {
        if (!Candidate) continue;
        if (FProperty* Found = Candidate->FindPropertyByName(PropFName)) return Found;
    }
    return nullptr;
}

FBlueprintGraphTargetParts ParseGraphTargetSpec(const FString& TargetSpec)
{
    const FString CleanTarget = TargetSpec.TrimStartAndEnd();
    FString Left, Right;
    if (CleanTarget.Split(TEXT("::"), &Left, &Right))
    {
        return { Left.TrimStartAndEnd(), Right.TrimStartAndEnd() };
    }
    if (CleanTarget.Split(TEXT("."), &Left, &Right))
    {
        return { Left.TrimStartAndEnd(), Right.TrimStartAndEnd() };
    }
    return { FString(), CleanTarget };
}

bool ResolveGraphVariableTarget(
    UBlueprint* Blueprint,
    const FBlueprintGraphTargetParts& TargetParts,
    UClass* InferredOwnerClass,
    UClass*& OutOwnerClass,
    FName& OutVariableName,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    OutOwnerClass = nullptr;
    OutVariableName = NAME_None;
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (TargetParts.MemberName.IsEmpty())
    {
        OutErrorCode = TEXT("INVALID_ARGUMENT");
        OutErrorMessage = TEXT("target required (variable name, optionally qualified as 'Class::Var')");
        return false;
    }

    OutVariableName = FName(*TargetParts.MemberName);
    if (!TargetParts.ClassName.IsEmpty())
    {
        OutOwnerClass = ResolveUClass(TargetParts.ClassName);
        if (!OutOwnerClass)
        {
            OutErrorCode = TEXT("CLASS_NOT_FOUND");
            OutErrorMessage = FString::Printf(TEXT("Class '%s' not found"), *TargetParts.ClassName);
            return false;
        }
    }
    else
    {
        OutOwnerClass = InferredOwnerClass;
    }

    return Blueprint != nullptr;
}

bool DoesGraphVariableExist(UBlueprint* Blueprint, UClass* OwnerClass, FName VariableName)
{
    if (VariableName == NAME_None) return false;
    if (OwnerClass) return OwnerClass->FindPropertyByName(VariableName) != nullptr;
    if (!Blueprint) return false;

    for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
    {
        if (VarDesc.VarName == VariableName) return true;
    }
    if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->FindPropertyByName(VariableName)) return true;
    if (Blueprint->SkeletonGeneratedClass && Blueprint->SkeletonGeneratedClass->FindPropertyByName(VariableName)) return true;
    return false;
}

UFunction* ResolveGraphCallableFunction(
    UBlueprint* Blueprint,
    const FBlueprintGraphTargetParts& TargetParts,
    UClass* PreferredClass,
    FString& OutErrorCode,
    FString& OutErrorMessage)
{
    OutErrorCode.Reset();
    OutErrorMessage.Reset();

    if (TargetParts.MemberName.IsEmpty())
    {
        OutErrorCode = TEXT("INVALID_ARGUMENT");
        OutErrorMessage = TEXT("target required (function name, optionally qualified as 'Class::Function')");
        return nullptr;
    }

    if (!TargetParts.ClassName.IsEmpty())
    {
        UClass* Class = ResolveUClass(TargetParts.ClassName);
        if (!Class)
        {
            OutErrorCode = TEXT("CLASS_NOT_FOUND");
            OutErrorMessage = FString::Printf(TEXT("Class '%s' not found"), *TargetParts.ClassName);
            return nullptr;
        }
        return Class->FindFunctionByName(FName(*TargetParts.MemberName));
    }

    if (PreferredClass)
    {
        if (UFunction* Func = PreferredClass->FindFunctionByName(FName(*TargetParts.MemberName)))
        {
            return Func;
        }
    }
    if (Blueprint && Blueprint->GeneratedClass)
    {
        if (UFunction* Func = Blueprint->GeneratedClass->FindFunctionByName(FName(*TargetParts.MemberName)))
        {
            return Func;
        }
    }
    if (UClass* KSL = UKismetSystemLibrary::StaticClass())
    {
        if (UFunction* Func = KSL->FindFunctionByName(FName(*TargetParts.MemberName))) return Func;
    }
    if (UClass* GPS = UGameplayStatics::StaticClass())
    {
        if (UFunction* Func = GPS->FindFunctionByName(FName(*TargetParts.MemberName))) return Func;
    }
    if (UClass* KML = UKismetMathLibrary::StaticClass())
    {
        if (UFunction* Func = KML->FindFunctionByName(FName(*TargetParts.MemberName))) return Func;
    }
    if (UClass* KSTR = UKismetStringLibrary::StaticClass())
    {
        if (UFunction* Func = KSTR->FindFunctionByName(FName(*TargetParts.MemberName))) return Func;
    }
    if (UClass* KTXT = UKismetTextLibrary::StaticClass())
    {
        if (UFunction* Func = KTXT->FindFunctionByName(FName(*TargetParts.MemberName))) return Func;
    }

    return nullptr;
}

bool CollectVariableMetadata(const UBlueprint* Blueprint, const FBPVariableDescription& VarDesc, TSharedPtr<FJsonObject>& OutMetadata)
{
    OutMetadata.Reset();
    if (Blueprint)
    {
        TSharedPtr<FJsonObject> MetaJson = MakeShared<FJsonObject>();
        bool bAny = false;
        UBlueprint* MutableBlueprint = const_cast<UBlueprint*>(Blueprint);
        if (FProperty* Property = FindBlueprintProperty(MutableBlueprint, VarDesc.VarName.ToString()))
        {
            if (const TMap<FName, FString>* MetaMap = Property->GetMetaDataMap())
            {
                for (const TPair<FName, FString>& Pair : *MetaMap)
                {
                    if (!Pair.Value.IsEmpty())
                    {
                        MetaJson->SetStringField(Pair.Key.ToString(), Pair.Value);
                        bAny = true;
                    }
                }
            }
        }
        if (bAny && MetaJson->Values.Num() > 0)
        {
            OutMetadata = MetaJson;
            return true;
        }
    }
    return false;
}

TSharedPtr<FJsonObject> BuildVariableJson(const UBlueprint* Blueprint, const FBPVariableDescription& VarDesc)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), VarDesc.VarName.ToString());
    Obj->SetStringField(TEXT("type"), DescribePinType(VarDesc.VarType));
    const bool bReplicated = (VarDesc.PropertyFlags & CPF_Net) != 0;
    const bool bReadOnly = (VarDesc.PropertyFlags & CPF_BlueprintReadOnly) != 0;
    const bool bEditable = (VarDesc.PropertyFlags & CPF_Edit) != 0;
    const bool bInstanceEditable = bEditable && ((VarDesc.PropertyFlags & CPF_DisableEditOnInstance) == 0);

    bool bPrivate = false;
    bool bExposeOnSpawn = false;
    if (Blueprint)
    {
        UBlueprint* MutableBlueprint = const_cast<UBlueprint*>(Blueprint);
        if (FProperty* Property = FindBlueprintProperty(MutableBlueprint, VarDesc.VarName.ToString()))
        {
            bPrivate = Property->GetBoolMetaData(FBlueprintMetadata::MD_Private);
            bExposeOnSpawn = Property->GetBoolMetaData(FBlueprintMetadata::MD_ExposeOnSpawn);
        }
    }

    Obj->SetBoolField(TEXT("replicated"), bReplicated);
    Obj->SetBoolField(TEXT("readOnly"), bReadOnly);
    Obj->SetBoolField(TEXT("editable"), bEditable);
    Obj->SetBoolField(TEXT("instanceEditable"), bInstanceEditable);
    Obj->SetBoolField(TEXT("private"), bPrivate);
    Obj->SetBoolField(TEXT("public"), !bPrivate);
    Obj->SetBoolField(TEXT("exposeOnSpawn"), bExposeOnSpawn);
    const FString CategoryStr = VarDesc.Category.ToString();
    if (!CategoryStr.IsEmpty()) Obj->SetStringField(TEXT("category"), CategoryStr);
    TSharedPtr<FJsonObject> Metadata;
    if (CollectVariableMetadata(Blueprint, VarDesc, Metadata))
        Obj->SetObjectField(TEXT("metadata"), Metadata);
    return Obj;
}

TArray<TSharedPtr<FJsonValue>> CollectBlueprintVariables(UBlueprint* Blueprint)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    if (!Blueprint) return Out;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
        Out.Add(MakeShared<FJsonValueObject>(BuildVariableJson(Blueprint, Var)));
    return Out;
}

TArray<TSharedPtr<FJsonValue>> CollectBlueprintFunctions(UBlueprint* Blueprint)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    if (!Blueprint) return Out;
    for (UEdGraph* Graph : Blueprint->FunctionGraphs)
    {
        if (!Graph) continue;
        TSharedPtr<FJsonObject> Fn = MakeShared<FJsonObject>();
        Fn->SetStringField(TEXT("name"), Graph->GetName());
        bool bIsPublic = true;
        bool bIsProtected = false;
        bool bIsPrivate = false;
        bool bIsPure = false;
        bool bIsConst = false;
        bool bCallInEditor = false;
        FString FunctionCategory;
        TArray<TSharedPtr<FJsonValue>> Inputs;
        TArray<TSharedPtr<FJsonValue>> Outputs;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
            {
                AppendPinsJson(EntryNode->UserDefinedPins, Inputs);
                const int32 FunctionFlags = EntryNode->GetFunctionFlags();
                bIsPublic = (FunctionFlags & FUNC_Public) != 0;
                bIsProtected = (FunctionFlags & FUNC_Protected) != 0;
                bIsPrivate = (FunctionFlags & FUNC_Private) != 0;
                bIsPure = (FunctionFlags & FUNC_BlueprintPure) != 0;
                bIsConst = (FunctionFlags & FUNC_Const) != 0;
                bCallInEditor = EntryNode->MetaData.bCallInEditor;
                // The function category set via blueprint.set_function_settings
                // (FBlueprintEditorUtils::SetBlueprintFunctionOrMacroCategory) lands in
                // this same MetaData struct as bCallInEditor; read it back so the summary
                // verbs are symmetric with the write surface. FText::ToString() on empty
                // text already yields "", and the emit below is gated on non-empty,
                // mirroring the variable-entry category pattern in BuildVariableJson above.
                FunctionCategory = EntryNode->MetaData.Category.ToString();
            }
            else if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
            {
                AppendPinsJson(ResultNode->UserDefinedPins, Outputs);
            }
        }
        Fn->SetBoolField(TEXT("public"), bIsPublic);
        Fn->SetBoolField(TEXT("protected"), bIsProtected);
        Fn->SetBoolField(TEXT("private"), bIsPrivate);
        Fn->SetBoolField(TEXT("pure"), bIsPure);
        Fn->SetBoolField(TEXT("const"), bIsConst);
        Fn->SetBoolField(TEXT("callInEditor"), bCallInEditor);
        if (!FunctionCategory.IsEmpty()) Fn->SetStringField(TEXT("category"), FunctionCategory);
        if (Inputs.Num() > 0) Fn->SetArrayField(TEXT("inputs"), Inputs);
        if (Outputs.Num() > 0) Fn->SetArrayField(TEXT("outputs"), Outputs);
        Out.Add(MakeShared<FJsonValueObject>(Fn));
    }
    return Out;
}

void CollectEventPins(UK2Node* Node, TArray<TSharedPtr<FJsonValue>>& Out)
{
    if (!Node) return;
    if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
        AppendPinsJson(CustomEvent->UserDefinedPins, Out);
    else if (UK2Node_FunctionEntry* FunctionEntry = Cast<UK2Node_FunctionEntry>(Node))
        AppendPinsJson(FunctionEntry->UserDefinedPins, Out);
}

TArray<TSharedPtr<FJsonValue>> CollectBlueprintEvents(UBlueprint* Blueprint)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    if (!Blueprint) return Out;
    auto AppendEvent = [&](const FString& EventName, const FString& EventType,
                           UK2Node* SourceNode, bool bIncludeExecOutputs = false)
    {
        TSharedPtr<FJsonObject> EventJson = MakeShared<FJsonObject>();
        EventJson->SetStringField(TEXT("name"), EventName);
        EventJson->SetStringField(TEXT("eventType"), EventType);
        // A freshly created Actor BP carries inert default event stubs
        // (ReceiveTick, ReceiveActorBeginOverlap) created in the Disabled state.
        // Without this flag they are indistinguishable from a live authored
        // handler, so a caller cannot tell which events the BP actually runs.
        // IsNodeEnabled() mirrors the structured nodeState.isEnabled signal the
        // graph-inspection family already exposes (BuildNodeStateJson).
        EventJson->SetBoolField(TEXT("enabled"), SourceNode ? SourceNode->IsNodeEnabled() : true);
        TArray<TSharedPtr<FJsonValue>> Params;
        CollectEventPins(SourceNode, Params);
        if (Params.Num() > 0) EventJson->SetArrayField(TEXT("parameters"), Params);
        if (bIncludeExecOutputs && SourceNode)
        {
            TArray<TSharedPtr<FJsonValue>> ExecOutputs;
            for (UEdGraphPin* Pin : SourceNode->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output
                    || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    continue;
                }
                for (UEdGraphPin* TargetPin : Pin->LinkedTo)
                {
                    UEdGraphNode* TargetNode = TargetPin ? TargetPin->GetOwningNode() : nullptr;
                    if (!TargetNode) continue;
                    TSharedPtr<FJsonObject> ExecOutput = MakeShared<FJsonObject>();
                    ExecOutput->SetStringField(TEXT("pin"), Pin->PinName.ToString());
                    ExecOutput->SetStringField(TEXT("targetNodeId"), TargetNode->NodeGuid.ToString());
                    ExecOutput->SetStringField(TEXT("targetNodeTitle"),
                        TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
                    ExecOutputs.Add(MakeShared<FJsonValueObject>(ExecOutput));
                }
            }
            EventJson->SetArrayField(TEXT("execOutputs"), ExecOutputs);
        }
        Out.Add(MakeShared<FJsonValueObject>(EventJson));
    };
    for (UEdGraph* Graph : Blueprint->UbergraphPages)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
                AppendEvent(CustomEvent->CustomFunctionName.ToString(), TEXT("custom"), CustomEvent);
#if MCP_BPUTILS_HAS_INPUT_ACTION
            else if (UK2Node_InputAction* InputActionNode = Cast<UK2Node_InputAction>(Node))
                AppendEvent(InputActionNode->InputActionName.ToString(),
                    InputActionNode->GetClass()->GetName(), InputActionNode, true);
#endif
#if MCP_BPUTILS_HAS_INPUT_TOUCH
            else if (UK2Node_InputTouch* InputTouchNode = Cast<UK2Node_InputTouch>(Node))
                AppendEvent(TEXT("Touch"), InputTouchNode->GetClass()->GetName(), InputTouchNode, true);
#endif
#if MCP_BPUTILS_HAS_INPUT_AXIS_EVENT
            else if (UK2Node_InputAxisEvent* InputAxisNode = Cast<UK2Node_InputAxisEvent>(Node))
                AppendEvent(InputAxisNode->InputAxisName.ToString(),
                    InputAxisNode->GetClass()->GetName(), InputAxisNode, true);
#endif
#if MCP_BPUTILS_HAS_INPUT_AXIS_KEY_EVENT
            else if (UK2Node_InputAxisKeyEvent* InputAxisKeyNode = Cast<UK2Node_InputAxisKeyEvent>(Node))
                AppendEvent(InputAxisKeyNode->AxisKey.GetFName().ToString(),
                    InputAxisKeyNode->GetClass()->GetName(), InputAxisKeyNode, true);
#endif
            else if (UK2Node_InputKey* InputKeyNode = Cast<UK2Node_InputKey>(Node))
                AppendEvent(InputKeyNode->InputKey.GetFName().ToString(),
                    InputKeyNode->GetClass()->GetName(), InputKeyNode, true);
            else if (Node && Node->GetClass()->GetFName() == TEXT("K2Node_EnhancedInputAction"))
            {
                const UInputAction* InputAction = FCodeNodeEmitter::GetEnhancedInputAction(Node);
                AppendEvent(InputAction ? InputAction->GetPathName() : TEXT("<UNBOUND>"),
                    Node->GetClass()->GetName(), CastChecked<UK2Node>(Node), true);
            }
            else if (UK2Node_Event* K2Event = Cast<UK2Node_Event>(Node))
                AppendEvent(K2Event->GetFunctionName().ToString(), K2Event->GetClass()->GetName(), K2Event);
        }
    }
    return Out;
}

TSharedPtr<FJsonObject> FindNamedEntry(const TArray<TSharedPtr<FJsonValue>>& Array, const FString& FieldName, const FString& DesiredValue)
{
    for (const TSharedPtr<FJsonValue>& Value : Array)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object) continue;
        const TSharedPtr<FJsonObject> Obj = Value->AsObject();
        if (!Obj.IsValid()) continue;
        FString FieldValue;
        if (Obj->TryGetStringField(FieldName, FieldValue) &&
            FieldValue.Equals(DesiredValue, ESearchCase::IgnoreCase))
            return Obj;
    }
    return nullptr;
}

TSharedPtr<FJsonObject> EnsureBlueprintEntry(const FString& Key)
{
    FBlueprintTracker& Tracker = FPluginState::Get().Blueprints();
    TSharedPtr<FJsonObject> Existing = Tracker.FindBlueprintEntry(Key);
    if (Existing.IsValid())
    {
        return Existing;
    }
    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("blueprintPath"), Key);
    Entry->SetArrayField(TEXT("variables"), TArray<TSharedPtr<FJsonValue>>());
    Entry->SetArrayField(TEXT("functions"), TArray<TSharedPtr<FJsonValue>>());
    Entry->SetArrayField(TEXT("events"), TArray<TSharedPtr<FJsonValue>>());
    Entry->SetObjectField(TEXT("defaults"), MakeShared<FJsonObject>());
    Entry->SetObjectField(TEXT("metadata"), MakeShared<FJsonObject>());
    Tracker.RegisterBlueprint(Key, Entry);
    return Entry;
}

// Build the per-variable `defaults` map for blueprint.get by exporting each
// member variable's value off the generated-class CDO — the same surface
// property.get reads with includeDefault:true (defaultSource "class_cdo"). The
// snapshot here always carries `defaults`, so the handler's registry merge
// (which only folds in the always-empty registry `defaults` when the snapshot
// lacks one) leaves these live CDO values in place. Variables whose property
// has not yet been compiled onto the generated class (no FProperty on the CDO)
// are skipped, so a freshly added-but-uncompiled variable simply does not
// appear rather than reporting a wrong value.
static TSharedPtr<FJsonObject> BuildBlueprintDefaults(UBlueprint* Blueprint)
{
    TSharedPtr<FJsonObject> Defaults = MakeShared<FJsonObject>();
    if (!Blueprint) return Defaults;

    UClass* GeneratedClass = Blueprint->GeneratedClass;
    if (!GeneratedClass) return Defaults;
    UObject* CDO = GeneratedClass->GetDefaultObject();
    if (!CDO) return Defaults;

    for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
    {
        const FName VarName = VarDesc.VarName;
        if (VarName.IsNone()) continue;
        // Resolve against GeneratedClass directly rather than the module-public
        // FindBlueprintProperty helper: that helper falls back to the skeleton and
        // parent classes, whose FProperty would not have a meaningful value on the
        // generated CDO we export from here, and would surface inherited/uncompiled
        // vars the helper's contract omits. Restricting to GeneratedClass is what
        // keeps the uncompiled-omit behavior above correct.
        FProperty* Property = FindFProperty<FProperty>(GeneratedClass, VarName);
        if (!Property) continue;
        if (TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(CDO, Property))
            Defaults->SetField(VarName.ToString(), Value);
    }
    return Defaults;
}

TSharedPtr<FJsonObject> BuildBlueprintSnapshot(UBlueprint* Blueprint, const FString& NormalizedPath)
{
    if (!Blueprint) return MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
    Snapshot->SetStringField(TEXT("blueprintPath"), NormalizedPath);
    Snapshot->SetStringField(TEXT("resolvedPath"), NormalizedPath);
    Snapshot->SetStringField(TEXT("assetPath"), Blueprint->GetPathName());
    Snapshot->SetArrayField(TEXT("variables"), CollectBlueprintVariables(Blueprint));
    Snapshot->SetArrayField(TEXT("functions"), CollectBlueprintFunctions(Blueprint));
    Snapshot->SetArrayField(TEXT("events"), CollectBlueprintEvents(Blueprint));
    Snapshot->SetObjectField(TEXT("defaults"), BuildBlueprintDefaults(Blueprint));
    TSharedPtr<FJsonObject> MetadataRoot = MakeShared<FJsonObject>();
    for (const FBPVariableDescription& VarDesc : Blueprint->NewVariables)
    {
        TSharedPtr<FJsonObject> MetaJson;
        if (CollectVariableMetadata(Blueprint, VarDesc, MetaJson) && MetaJson.IsValid())
            MetadataRoot->SetObjectField(VarDesc.VarName.ToString(), MetaJson);
    }
    if (MetadataRoot->Values.Num() > 0)
        Snapshot->SetObjectField(TEXT("metadata"), MetadataRoot);
    return Snapshot;
}

bool MakePinTypeFromBpirText(FStringView Source, FEdGraphPinType& OutPinType)
{
    FBpirTypeSpec Spec;
    FString ParseErr;
    int32 ParseErrCol = INDEX_NONE;
    if (BpirTypeSpecParser::ParseTypeSpec(FString(Source), Spec, ParseErr, ParseErrCol)
        && FCodePinResolver::ConvertTypeSpecToPinType(Spec, OutPinType))
    {
        return true;
    }
    // On any parse or resolution failure, hand back a PC_Wildcard pin so UE's
    // graph schema can retype it later rather than rejecting outright.
    OutPinType = FEdGraphPinType{};
    OutPinType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
    return false;
}

bool AddUserDefinedPin(UK2Node* Node, const FString& PinName, const FBpirTypeSpec& TypeSpec, EEdGraphPinDirection Direction)
{
    if (!Node) return false;
    const FString CleanName = PinName.TrimStartAndEnd();
    if (CleanName.IsEmpty()) return false;
    // A failed conversion still yields a pin, just with category PC_Wildcard
    // so UE's graph schema can retype it later.
    FEdGraphPinType PinTypeDesc;
    if (!FCodePinResolver::ConvertTypeSpecToPinType(TypeSpec, PinTypeDesc))
    {
        PinTypeDesc = FEdGraphPinType{};
        PinTypeDesc.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
    }
    const FName PinFName(*CleanName);
    UEdGraphPin* CreatedPin = nullptr;
    if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
        CreatedPin = EntryNode->CreateUserDefinedPin(PinFName, PinTypeDesc, Direction);
    else if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(Node))
        CreatedPin = ResultNode->CreateUserDefinedPin(PinFName, PinTypeDesc, Direction);
    else if (UK2Node_CustomEvent* CustomEventNode = Cast<UK2Node_CustomEvent>(Node))
        CreatedPin = CustomEventNode->CreateUserDefinedPin(PinFName, PinTypeDesc, Direction);
    else if (UK2Node_Tunnel* TunnelNode = Cast<UK2Node_Tunnel>(Node))
        CreatedPin = TunnelNode->CreateUserDefinedPin(PinFName, PinTypeDesc, Direction);
    if (!CreatedPin)
    {
        // Log the canonical BPIR-text form of the TypeSpec to make the failure diagnosable.
        const FString TypeText = BpirTypeSpecParser::TypeSpecToBpirText(TypeSpec);
        UE_LOG(LogPinWrightSubsystem, Warning, TEXT("AddUserDefinedPin: CreateUserDefinedPin failed for pin '%s' type '%s'"), *CleanName, *TypeText);
    }
    return CreatedPin != nullptr;
}

UK2Node_FunctionEntry* FindGraphFunctionEntryNode(UEdGraph* Graph)
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

EAddDispatcherResult AddDispatcherWithSignatureGraph(
    UBlueprint* Blueprint,
    FName DispatcherName,
    const TArray<FParsedPinParam>& SignatureParams,
    UEdGraph** OutSignatureGraph,
    FString* OutFailedParamName)
{
    if (OutSignatureGraph)
    {
        *OutSignatureGraph = nullptr;
    }
    if (!Blueprint)
    {
        return EAddDispatcherResult::GraphUnavailable;
    }

    FEdGraphPinType DelegateType;
    DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
    if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, DispatcherName, DelegateType))
    {
        return EAddDispatcherResult::MemberExists;
    }

    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
    UEdGraph* SignatureGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint,
        DispatcherName,
        UEdGraph::StaticClass(),
        UEdGraphSchema_K2::StaticClass());
    if (!SignatureGraph || !K2Schema)
    {
        // Roll back the member variable so a failed setup leaves the Blueprint unchanged.
        FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, DispatcherName);
        return EAddDispatcherResult::GraphUnavailable;
    }

    SignatureGraph->bEditable = false;
    K2Schema->CreateDefaultNodesForGraph(*SignatureGraph);
    K2Schema->CreateFunctionGraphTerminators(*SignatureGraph, static_cast<UClass*>(nullptr));
    K2Schema->AddExtraFunctionFlags(SignatureGraph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
    K2Schema->MarkFunctionEntryAsEditable(SignatureGraph, true);

    UK2Node_FunctionEntry* EntryNode = FindGraphFunctionEntryNode(SignatureGraph);
    if (!EntryNode)
    {
        FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, DispatcherName);
        return EAddDispatcherResult::GraphUnavailable;
    }

    for (const FParsedPinParam& Param : SignatureParams)
    {
        if (!AddUserDefinedPin(EntryNode, Param.Name, Param.Spec, EGPD_Output))
        {
            if (OutFailedParamName)
            {
                *OutFailedParamName = Param.Name;
            }
            FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, DispatcherName);
            return EAddDispatcherResult::ParamFailed;
        }
    }

    EntryNode->ReconstructNode();
    Blueprint->DelegateSignatureGraphs.Add(SignatureGraph);
    if (OutSignatureGraph)
    {
        *OutSignatureGraph = SignatureGraph;
    }
    return EAddDispatcherResult::Success;
}

TArray<TSharedPtr<FJsonValue>> ReadPinParamArrayField(
    const TSharedPtr<FJsonObject>& Payload,
    const TCHAR* FieldName)
{
    if (!Payload.IsValid())
    {
        return TArray<TSharedPtr<FJsonValue>>();
    }
    const TArray<TSharedPtr<FJsonValue>>* Field = nullptr;
    Payload->TryGetArrayField(FieldName, Field);
    return (Field && Field->Num() > 0) ? *Field : TArray<TSharedPtr<FJsonValue>>();
}

const TCHAR* GetAcceptedPinTypeFormsText()
{
    // One canonical accepted-type-forms catalog. Every TYPE_NOT_FOUND message that
    // lists what a pin/variable type may look like appends this so the vocabulary
    // never drifts between handlers.
    static const TCHAR* const AcceptedForms =
        TEXT("Accepted forms: primitives (bool, int, int64, float, double, byte, string, name, text); ")
        TEXT("builtin structs (vector, rotator, transform, FLinearColor); ")
        TEXT("struct short names (EditorReplay or FEditorReplay); ")
        TEXT("class short names (MyWidget or UMyWidget or MyWidget*); ")
        TEXT("full paths (/Script/Module.Type or /Game/Path/BP_Asset); ")
        TEXT("wrappers (array<T>, set<T>, map<K,V>, object<T>, struct<T>, enum<T>, class<T>, softobject<T>, softclass<T>, interface<T>)");
    return AcceptedForms;
}

bool ResolvesToConcretePin(const FParsedPinParam& Param)
{
    // BuildNamedPinDescriptor returns false when ConvertTypeSpecToPinType yields
    // PC_Wildcard — the same "did this resolve to a concrete pin?" check add_variable
    // and Strict-mode parsing make. The descriptor name does not affect resolution.
    FNamedPinTypeDescriptor Descriptor;
    return BuildNamedPinDescriptor(Param.Name, Param.Spec, Descriptor);
}

bool FindFirstPinParamWildcardFallback(
    const TArray<FParsedPinParam>& Params,
    const TCHAR* ParamLabel,
    FString& OutErrorMessage)
{
    for (const FParsedPinParam& P : Params)
    {
        if (!P.bParseOk)
        {
            // Token never parsed (e.g. the documented-but-unsupported
            // 'class:/Script/X.Y' path form). Surface the structured parse detail.
            OutErrorMessage = FString::Printf(
                TEXT("Could not resolve %s '%s' type '%s': %s. %s"),
                ParamLabel, *P.OriginalName, *P.OriginalType,
                *BpirTypeSpecParser::FormatTypeSpecErrorDetail(P.ParseErr, P.ParseErrCol),
                GetAcceptedPinTypeFormsText());
            return true;
        }

        // Parsed, but does it resolve to a concrete pin type? (catches e.g. a bare
        // unknown identifier that ConvertTypeSpecToPinType leaves as PC_Wildcard.)
        if (!ResolvesToConcretePin(P))
        {
            OutErrorMessage = FString::Printf(
                TEXT("Could not resolve %s '%s' type '%s'. %s"),
                ParamLabel, *P.OriginalName, *P.OriginalType, GetAcceptedPinTypeFormsText());
            return true;
        }
    }

    return false;
}

bool RejectWildcardPinParams(
    FHandlerContext& Ctx,
    const TArray<FParsedPinParam>& Inputs,
    const TArray<FParsedPinParam>& Outputs)
{
    // Reject any input/output token that would silently become a wildcard pin (e.g.
    // the documented-but-unsupported 'class:/Script/X.Y' form) instead of returning
    // success with a malformed pin that only fails at a later compile. Mirrors
    // blueprint.add_variable's loud TYPE_NOT_FOUND rejection.
    FString WildcardError;
    if (FindFirstPinParamWildcardFallback(Inputs, TEXT("input"), WildcardError)
        || FindFirstPinParamWildcardFallback(Outputs, TEXT("output"), WildcardError))
    {
        Ctx.SendError(TEXT("TYPE_NOT_FOUND"), *WildcardError);
        return true;
    }
    return false;
}

bool AddParsedPinParamsToNodes(
    UK2Node_FunctionEntry* EntryNode,
    UK2Node_FunctionResult* ResultNode,
    const TArray<FParsedPinParam>& ParsedInputs,
    const TArray<FParsedPinParam>& ParsedOutputs,
    const TCHAR* LogContext,
    FString& OutError)
{
    OutError.Reset();
    if (!EntryNode)
    {
        OutError = TEXT("Function entry node is unavailable.");
        return false;
    }
    if (ParsedOutputs.Num() > 0 && !ResultNode)
    {
        OutError = TEXT("Function result node is unavailable.");
        return false;
    }
#if WITH_DEV_AUTOMATION_TESTS
    if (GForceParsedPinCreationFailureForTests)
    {
        OutError = TEXT("Pin creation failure injected by automation test.");
        return false;
    }
#endif

    for (const FParsedPinParam& P : ParsedInputs)
    {
        if (!P.bParseOk)
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("%s: could not parse input param type '%s' (%s); falling back to wildcard pin"),
                LogContext, *P.OriginalType, *P.ParseErr);
        }
        if (!AddUserDefinedPin(EntryNode, P.Name, P.Spec, EGPD_Output))
        {
            OutError = FString::Printf(TEXT("Failed to create function input pin '%s'."), *P.Name);
            return false;
        }
    }

    for (const FParsedPinParam& P : ParsedOutputs)
    {
        if (!P.bParseOk)
        {
            UE_LOG(LogPinWrightSubsystem, Warning,
                TEXT("%s: could not parse output param type '%s' (%s); falling back to wildcard pin"),
                LogContext, *P.OriginalType, *P.ParseErr);
        }
        if (!AddUserDefinedPin(ResultNode, P.Name, P.Spec, EGPD_Input))
        {
            OutError = FString::Printf(TEXT("Failed to create function output pin '%s'."), *P.Name);
            return false;
        }
    }

    EntryNode->ReconstructNode();
    if (ResultNode)
    {
        ResultNode->ReconstructNode();
    }
    return true;
}

UFunction* ResolveFunction(UBlueprint* Blueprint, const FString& FunctionName)
{
    if (!Blueprint || FunctionName.TrimStartAndEnd().IsEmpty()) return nullptr;
    const FString CleanFunc = FunctionName.TrimStartAndEnd();
    UFunction* Found = FindObject<UFunction>(nullptr, *CleanFunc);
    if (Found) return Found;
    const FName FuncFName(*CleanFunc);
    const TArray<UClass*> CandidateClasses = {
        Blueprint->GeneratedClass, Blueprint->SkeletonGeneratedClass, Blueprint->ParentClass };
    for (UClass* Candidate : CandidateClasses)
    {
        if (Candidate)
        {
            UFunction* CandidateFunc = Candidate->FindFunctionByName(FuncFName);
            if (CandidateFunc) return CandidateFunc;
        }
    }
    int32 DotIndex = INDEX_NONE;
    if (CleanFunc.FindChar('.', DotIndex))
    {
        const FString ClassPath = CleanFunc.Left(DotIndex);
        const FString FuncSegment = CleanFunc.Mid(DotIndex + 1);
        if (!ClassPath.IsEmpty() && !FuncSegment.IsEmpty())
        {
            if (UClass* ExplicitClass = FindObject<UClass>(nullptr, *ClassPath))
            {
                UFunction* ExplicitFunc = ExplicitClass->FindFunctionByName(FName(*FuncSegment));
                if (ExplicitFunc) return ExplicitFunc;
            }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Graph node pin/link helper functions
// ---------------------------------------------------------------------------

bool HasConnectedExecPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
    if (!Node) return false;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
            && Pin->Direction == Direction && Pin->LinkedTo.Num() > 0)
        {
            return true;
        }
    }
    return false;
}

UEdGraphPin* FindOutputPin(UEdGraphNode* Node, const FName& PinName)
{
    if (!Node) return nullptr;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output)
        {
            if (!PinName.IsNone()) { if (Pin->PinName == PinName) return Pin; }
            else return Pin;
        }
    }
    return nullptr;
}

UEdGraphPin* FindInputPin(UEdGraphNode* Node, const FName& PinName)
{
    if (!Node) return nullptr;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == PinName) return Pin;
    }
    return nullptr;
}

FString ResolveBlueprintPath(FHandlerContext& Ctx)
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    if (!Payload.IsValid()) return FString();

    auto* Subsystem = Ctx.GetSubsystem();

    FString ResolvedPath;
    VisitBlueprintPathScalarFieldNames(
        EBlueprintPathParamAliasSet::ResolveBlueprintPath,
        [&Payload, Subsystem, &ResolvedPath](const TCHAR* FieldName)
        {
            FString Req;
            if (!Payload->TryGetStringField(FieldName, Req) || Req.TrimStartAndEnd().IsEmpty())
            {
                return true;
            }

            FString Norm;
            if (Subsystem && FindBlueprintNormalizedPath(Req, Norm) && !Norm.TrimStartAndEnd().IsEmpty())
            {
                ResolvedPath = Norm;
                return false;
            }
            ResolvedPath = Req;
            return false;
        });
    if (!ResolvedPath.IsEmpty())
    {
        return ResolvedPath;
    }

    // Blueprint candidate arrays
    VisitBlueprintPathCandidateArrayFieldNames(
        [&Payload, Subsystem, &ResolvedPath](const TCHAR* CandidateField)
        {
            const TArray<TSharedPtr<FJsonValue>>* CandidateArray = nullptr;
            if (!Payload->TryGetArrayField(CandidateField, CandidateArray) ||
                !CandidateArray ||
                CandidateArray->Num() == 0)
            {
                return true;
            }

            for (const TSharedPtr<FJsonValue>& V : *CandidateArray)
            {
                if (!V.IsValid() || V->Type != EJson::String) continue;
                FString Candidate = V->AsString();
                if (Candidate.TrimStartAndEnd().IsEmpty()) continue;
                FString Norm;
                if (Subsystem && FindBlueprintNormalizedPath(Candidate, Norm))
                {
                    ResolvedPath = !Norm.TrimStartAndEnd().IsEmpty() ? Norm : Candidate;
                    return false;
                }
            }
            return true;
        });
    if (!ResolvedPath.IsEmpty())
    {
        return ResolvedPath;
    }

    return FString();
}

void CollectExecChainNodes(const TArray<UEdGraphNode*>& RootNodes, TSet<UEdGraphNode*>& OutNodes)
{
    TArray<UEdGraphNode*> WorkQueue;
    for (UEdGraphNode* Root : RootNodes)
    {
        if (!Root) continue;
        if (OutNodes.Contains(Root)) continue;
        OutNodes.Add(Root);
        WorkQueue.Add(Root);
    }

    while (WorkQueue.Num() > 0)
    {
        UEdGraphNode* Current = WorkQueue.Pop(EAllowShrinking::No);
        for (UEdGraphPin* Pin : Current->Pins)
        {
            if (Pin->Direction != EGPD_Output) continue;
            if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                if (!LinkedPin) continue;
                UEdGraphNode* Downstream = LinkedPin->GetOwningNode();
                if (Downstream && !OutNodes.Contains(Downstream))
                {
                    OutNodes.Add(Downstream);
                    WorkQueue.Add(Downstream);
                }
            }
        }
    }
}

int32 CascadeRemoveStaleCreateDelegates(UBlueprint* Blueprint, const TSet<FName>& RemovedFunctionNames)
{
    if (!Blueprint || RemovedFunctionNames.Num() == 0) return 0;

    TArray<UEdGraph*> Graphs;
    Graphs.Append(Blueprint->UbergraphPages);
    Graphs.Append(Blueprint->FunctionGraphs);

    TArray<UK2Node_CreateDelegate*> NodesToRemove;
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(Node);
            if (!CreateDelegate) continue;
            if (CreateDelegate->SelectedFunctionName != NAME_None
                && RemovedFunctionNames.Contains(CreateDelegate->SelectedFunctionName))
            {
                NodesToRemove.Add(CreateDelegate);
            }
        }
    }

    for (UK2Node_CreateDelegate* Node : NodesToRemove)
    {
        FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
    }

    return NodesToRemove.Num();
}

int32 ScrubStaleUFunctionsFromClass(UBlueprint* Blueprint, const TSet<FName>& WipedFunctionNames)
{
    if (!Blueprint || WipedFunctionNames.Num() == 0) return 0;

    int32 TotalScrubbed = 0;
    auto ScrubOn = [&WipedFunctionNames, &TotalScrubbed](UClass* Class)
    {
        if (!Class) return;

        TArray<UFunction*> ToScrub;
        for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            UFunction* Func = *It;
            if (!Func) continue;
            if (Func->HasAnyFunctionFlags(FUNC_Native)) continue; // never touch C++
            if (!WipedFunctionNames.Contains(Func->GetFName())) continue;
            ToScrub.Add(Func);
        }

        for (UFunction* Stale : ToScrub)
        {
            // Mirrors engine's FBlueprintEditorUtils::RemoveStaleFunctions per-entry:
            // unlink from Class->Children, drop from FuncMap, invalidate the export,
            // then rename into transient so serialization can't pick it up.
            // The naive version (only FuncMap.Remove + Rename) leaves the UFunction
            // linked in Class->Children, so TFieldIterator still yields it and the
            // integrity gate's orphan walk flags it post-scrub.
            UField* Prev = nullptr;
            UField* Curr = Class->Children;
            while (Curr && Curr != Stale)
            {
                Prev = Curr;
                Curr = Curr->Next;
            }
            if (Curr == Stale)
            {
                if (Prev) Prev->Next = Stale->Next;
                else Class->Children = Stale->Next;
                Stale->Next = nullptr;
            }

            Class->RemoveFunctionFromFunctionMap(Stale);
            FLinkerLoad::InvalidateExport(Stale);

            const FName UniqueName = MakeUniqueObjectName(
                GetTransientPackage(),
                UFunction::StaticClass(),
                *FString::Printf(TEXT("BPIR_TRASH_%s"), *Stale->GetName()));
            Stale->Rename(*UniqueName.ToString(), GetTransientPackage(),
                REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);

            ++TotalScrubbed;
        }

        if (ToScrub.Num() > 0)
        {
            Class->ClearFunctionMapsCaches();
            Class->Bind();
            Class->StaticLink(/*bRelinkExistingProperties=*/true);
        }
    };

    ScrubOn(Blueprint->SkeletonGeneratedClass);
    ScrubOn(Blueprint->GeneratedClass);
    return TotalScrubbed;
}

// Standalone stale-GUID check for a UK2Node_CreateDelegate. Detects the case where
// the node's SelectedFunctionName resolves against the scope class (i.e. a function
// by that name exists), but the node's stored SelectedFunctionGuid disagrees with
// the scope class's canonical GUID for that function. This is the condition that
// crashes ReplaceConvertibleDelegates on cold reload even when the engine's IsValid
// accepts the node.
//
// Independent of delegate-pin connectivity: runs purely on
// (SelectedFunctionName, SelectedFunctionGuid, ScopeClass), so it fires even for
// disconnected delegate pins that would early-out of the engine-style IsValid.
static bool HasStaleCreateDelegateGuid(const UK2Node_CreateDelegate* CreateDelegate, FString* OutMsg, bool bDontUseSkeletalClassForSelf)
{
    if (!CreateDelegate) return false;
    if (CreateDelegate->SelectedFunctionName == NAME_None) return false;
    if (!CreateDelegate->SelectedFunctionGuid.IsValid()) return false;

    UClass* ScopeClass = CreateDelegate->GetScopeClass(bDontUseSkeletalClassForSelf);
    if (!ScopeClass) return false;

    // Resolve by name+GUID to confirm the node targets a real function on the scope.
    // ResolveMember rewrites its in-memory GUID copy after a name match, so the check
    // that matters is against the scope class's canonical GUID map, not against what
    // ResolveMember returns.
    FMemberReference MemberReference;
    MemberReference.SetDirect(CreateDelegate->SelectedFunctionName, CreateDelegate->SelectedFunctionGuid, ScopeClass, false);
    const UFunction* FoundFunction = MemberReference.ResolveMember<UFunction>();
    if (!FoundFunction) return false;

    FGuid CanonicalGuid;
    const bool bFoundCanonical = FBlueprintEditorUtils::GetFunctionGuidFromClassByFieldName(
        ScopeClass, FoundFunction->GetFName(), CanonicalGuid);
    if (!bFoundCanonical || !CanonicalGuid.IsValid()) return false;

    if (CanonicalGuid == CreateDelegate->SelectedFunctionGuid) return false;

    if (OutMsg)
    {
        *OutMsg = FString::Printf(
            TEXT("stale GUID for '%s' — stored=%s canonical=%s"),
            *CreateDelegate->SelectedFunctionName.ToString(),
            *CreateDelegate->SelectedFunctionGuid.ToString(EGuidFormats::Digits),
            *CanonicalGuid.ToString(EGuidFormats::Digits));
    }
    return true;
}

// Local reimplementation of UK2Node_CreateDelegate::IsValid. The engine method exists
// but is not exported from the BlueprintGraph module in UE 5.6 (no BLUEPRINTGRAPH_API
// on the declaration), so calling it directly produces an unresolved-external link
// error. All the building-block accessors it uses internally ARE exported, so we
// replicate the same checks here.
static bool IsCreateDelegateNodeValid(const UK2Node_CreateDelegate* CreateDelegate, FString* OutMsg, bool bDontUseSkeletalClassForSelf)
{
    if (!CreateDelegate)
    {
        if (OutMsg) *OutMsg = TEXT("Null node");
        return false;
    }

    const FName FunctionName = CreateDelegate->GetFunctionName();
    if (FunctionName == NAME_None)
    {
        if (OutMsg) *OutMsg = TEXT("No function/event specified.");
        return false;
    }

    const UEdGraphPin* DelegatePin = CreateDelegate->GetDelegateOutPin();
    if (!DelegatePin)
    {
        if (OutMsg) *OutMsg = TEXT("Malformed node - there's no delegate output pin.");
        return false;
    }

    const UFunction* Signature = CreateDelegate->GetDelegateSignature();
    if (!Signature)
    {
        if (OutMsg) *OutMsg = TEXT("Unable to determine expected signature - is the delegate pin connected?");
        return false;
    }

    for (int32 PinIter = 1; PinIter < DelegatePin->LinkedTo.Num(); ++PinIter)
    {
        const UEdGraphPin* OtherPin = DelegatePin->LinkedTo[PinIter];
        const UFunction* OtherSignature = OtherPin
            ? FMemberReference::ResolveSimpleMemberReference<UFunction>(OtherPin->PinType.PinSubCategoryMemberReference)
            : nullptr;
        if (!OtherSignature || !Signature->IsSignatureCompatibleWith(OtherSignature))
        {
            if (OutMsg) *OutMsg = TEXT("A connected delegate's signature is incompatible.");
            return false;
        }
    }

    UClass* ScopeClass = CreateDelegate->GetScopeClass(bDontUseSkeletalClassForSelf);
    if (!ScopeClass)
    {
        if (OutMsg) *OutMsg = FString::Printf(TEXT("Unable to determine context for '%s'."), *FunctionName.ToString());
        return false;
    }

    FMemberReference MemberReference;
    MemberReference.SetDirect(CreateDelegate->SelectedFunctionName, CreateDelegate->SelectedFunctionGuid, ScopeClass, false);
    const UFunction* FoundFunction = MemberReference.ResolveMember<UFunction>();
    if (!FoundFunction)
    {
        if (OutMsg) *OutMsg = FString::Printf(TEXT("Unable to find selected function/event '%s'."), *FunctionName.ToString());
        return false;
    }

    // Check for stale GUID: if the node's stored GUID is non-zero and the scope
    // class has a canonical GUID for this function, they must agree. ResolveMember
    // resolves by name first and silently rewrites its in-memory GUID copy without
    // comparing against the canonical identity, so a name-matching-but-GUID-stale
    // node passes the ResolveMember check but crashes ReplaceConvertibleDelegates
    // on cold reload when scope-class resolution differs.
    if (CreateDelegate->SelectedFunctionGuid.IsValid())
    {
        FGuid CanonicalGuid;
        const bool bFoundCanonical = FBlueprintEditorUtils::GetFunctionGuidFromClassByFieldName(
            ScopeClass, FoundFunction->GetFName(), CanonicalGuid);
        if (bFoundCanonical && CanonicalGuid.IsValid() && CanonicalGuid != CreateDelegate->SelectedFunctionGuid)
        {
            if (OutMsg) *OutMsg = FString::Printf(
                TEXT("stale GUID for '%s' — stored=%s canonical=%s"),
                *FunctionName.ToString(),
                *CreateDelegate->SelectedFunctionGuid.ToString(EGuidFormats::Digits),
                *CanonicalGuid.ToString(EGuidFormats::Digits));
            return false;
        }
    }

    if (!Signature->IsSignatureCompatibleWith(FoundFunction))
    {
        if (OutMsg) *OutMsg = FString::Printf(TEXT("Function/event '%s' does not match the necessary signature."), *FunctionName.ToString());
        return false;
    }
    if (!UEdGraphSchema_K2::FunctionCanBeUsedInDelegate(FoundFunction))
    {
        if (OutMsg) *OutMsg = TEXT("The selected function/event is not bindable.");
        return false;
    }

    return true;
}

bool ValidateBlueprintGraphIntegrity(UBlueprint* Blueprint, TArray<FBlueprintIntegrityFailure>& OutFailures)
{
    if (!Blueprint) return true;

    UClass* GeneratedClass = Blueprint->GeneratedClass;

    const TArray<UEdGraph*> Graphs = CollectAllBlueprintGraphsRecursive(Blueprint);

    // Graph node set used by the LinkedTo walk to confirm that every linked pin's
    // owner still lives in this blueprint's graph world.
    TSet<const UEdGraphNode*> KnownNodes;
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node) KnownNodes.Add(Node);
        }
    }

    const int32 InitialFailureCount = OutFailures.Num();

    auto RecordFailure = [&OutFailures](UEdGraphNode* Node, const FString& Reason)
    {
        FBlueprintIntegrityFailure Failure;
        Failure.NodeGuid = Node ? Node->NodeGuid.ToString(EGuidFormats::Digits) : FString();
        Failure.NodeKind = Node ? Node->GetClass()->GetName() : TEXT("<null>");
        if (Node)
        {
            if (UEdGraph* G = Node->GetGraph())
            {
                Failure.GraphName = G->GetName();
            }
        }
        Failure.Reason = Reason;
        OutFailures.Add(MoveTemp(Failure));
    };

    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node) continue;

            // --- 1. K2Node_CreateDelegate: strict resolve against generated class ---
            if (UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(Node))
            {
                // Skip freshly-placed unwired nodes — engine treats these as transient OK.
                // A CreateDelegate with an empty SelectedFunctionName and no delegate-pin
                // connections is the "user just dragged this in" state; it cannot crash
                // ReplaceConvertibleDelegates because the compiler skips nodes with no
                // resolvable binding target.
                const UEdGraphPin* DelegateOutPin = CreateDelegate->GetDelegateOutPin();
                const bool bFullyUnwired =
                    CreateDelegate->SelectedFunctionName == NAME_None
                    && (!DelegateOutPin || DelegateOutPin->LinkedTo.Num() == 0);
                if (!bFullyUnwired)
                {
                    // Stale-GUID check runs independently of delegate-pin connectivity.
                    // The engine-style IsValid short-circuits when the delegate pin is
                    // unconnected (GetDelegateSignature returns null), which would hide
                    // a genuine stored-vs-canonical GUID mismatch. Run this check first
                    // so staleness is always reported.
                    FString StaleMsg;
                    bool bStaleRecorded = false;
                    if (HasStaleCreateDelegateGuid(CreateDelegate, &StaleMsg, /*bDontUseSkeletalClassForSelf=*/true))
                    {
                        RecordFailure(Node, FString::Printf(
                            TEXT("K2Node_CreateDelegate SelectedFunctionName='%s' fails generated-class resolve: %s"),
                            *CreateDelegate->SelectedFunctionName.ToString(), *StaleMsg));
                        bStaleRecorded = true;
                    }

                    FString Msg;
                    if (!IsCreateDelegateNodeValid(CreateDelegate, &Msg, /*bDontUseSkeletalClassForSelf=*/true))
                    {
                        // Skip the duplicate report when the failure is the same staleness
                        // already recorded above.
                        if (!(bStaleRecorded && Msg.Contains(TEXT("stale GUID"))))
                        {
                            RecordFailure(Node, FString::Printf(
                                TEXT("K2Node_CreateDelegate SelectedFunctionName='%s' fails generated-class resolve: %s"),
                                *CreateDelegate->SelectedFunctionName.ToString(), *Msg));
                        }
                    }
                }
            }
#if defined(__has_include) && __has_include("K2Node_ComponentBoundEvent.h")
            else if (UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Node))
            {
                // --- 2. K2Node_ComponentBoundEvent: resolve component + delegate property ---
                // ComponentPropertyName → FObjectProperty on the generated class pointing at
                // the component/widget type. DelegatePropertyName → FMulticastDelegateProperty
                // on that property's class.
                const FName ComponentName = BoundEvent->ComponentPropertyName;
                const FName DelegateName = BoundEvent->DelegatePropertyName;

                if (ComponentName == NAME_None)
                {
                    RecordFailure(Node, TEXT("K2Node_ComponentBoundEvent has empty ComponentPropertyName"));
                }
                else if (DelegateName == NAME_None)
                {
                    RecordFailure(Node, TEXT("K2Node_ComponentBoundEvent has empty DelegatePropertyName"));
                }
                else if (GeneratedClass)
                {
                    FObjectProperty* CompProp = FindFProperty<FObjectProperty>(GeneratedClass, ComponentName);
                    if (!CompProp)
                    {
                        RecordFailure(Node, FString::Printf(
                            TEXT("K2Node_ComponentBoundEvent ComponentPropertyName='%s' does not resolve to an FObjectProperty on generated class"),
                            *ComponentName.ToString()));
                    }
                    else if (UClass* CompClass = CompProp->PropertyClass)
                    {
                        FMulticastDelegateProperty* DelProp = FindFProperty<FMulticastDelegateProperty>(CompClass, DelegateName);
                        if (!DelProp)
                        {
                            // The declared property type can be a widened base - e.g. a C++
                            // BindWidgetOptional typed TObjectPtr<UUserWidget> whose instance is a
                            // BP widget declaring the dispatcher. The engine resolves the delegate
                            // on the node's serialized DelegateOwnerClass
                            // (K2Node_ComponentBoundEvent::GetTargetDelegateProperty), so mirror
                            // that before declaring the asset broken.
                            if (UClass* OwnerClass = BoundEvent->DelegateOwnerClass)
                            {
                                DelProp = FindFProperty<FMulticastDelegateProperty>(OwnerClass, DelegateName);
                            }
                        }
                        if (!DelProp)
                        {
                            RecordFailure(Node, FString::Printf(
                                TEXT("K2Node_ComponentBoundEvent DelegatePropertyName='%s' does not resolve to an FMulticastDelegateProperty on component class '%s' (or the node's DelegateOwnerClass)"),
                                *DelegateName.ToString(), *CompClass->GetName()));
                        }
                    }
                }
            }
#endif // K2Node_ComponentBoundEvent

            // --- 3. LinkedTo integrity walk on every pin of every node ---
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin) continue;
                for (int32 LinkIdx = 0; LinkIdx < Pin->LinkedTo.Num(); ++LinkIdx)
                {
                    UEdGraphPin* LinkedPin = Pin->LinkedTo[LinkIdx];
                    if (!LinkedPin)
                    {
                        RecordFailure(Node, FString::Printf(
                            TEXT("Pin '%s' has null LinkedTo[%d]"),
                            *Pin->PinName.ToString(), LinkIdx));
                        continue;
                    }
                    UEdGraphNode* LinkedOwner = LinkedPin->GetOwningNodeUnchecked();
                    if (!LinkedOwner)
                    {
                        RecordFailure(Node, FString::Printf(
                            TEXT("Pin '%s' LinkedTo[%d] has null owning node"),
                            *Pin->PinName.ToString(), LinkIdx));
                        continue;
                    }
                    if (!KnownNodes.Contains(LinkedOwner))
                    {
                        RecordFailure(Node, FString::Printf(
                            TEXT("Pin '%s' LinkedTo[%d] points at node '%s' (kind %s) not in this blueprint's graph set"),
                            *Pin->PinName.ToString(), LinkIdx,
                            *LinkedOwner->NodeGuid.ToString(EGuidFormats::Digits),
                            *LinkedOwner->GetClass()->GetName()));
                        continue;
                    }
                    if (!LinkedOwner->Pins.Contains(LinkedPin))
                    {
                        RecordFailure(Node, FString::Printf(
                            TEXT("Pin '%s' LinkedTo[%d] points at pin '%s' that is not in its owning node's Pins array"),
                            *Pin->PinName.ToString(), LinkIdx, *LinkedPin->PinName.ToString()));
                    }
                }
            }
        }
    }

    // --- 4. UFunction field-table sanity walk ---
    // Narrow check: flag null entries and FNames that have become invalid (i.e. the
    // cold-reload crash signature `FunctionName = Illegal name (block index out of range)`).
    // The broader "no backing graph / event / parent / interface" heuristic that used
    // to live here was removed: it was structurally wrong because the engine synthesizes
    // UFunctions on the class for many node types (AsyncAction proxy callbacks,
    // ComponentBoundEvent delegate signatures, ExecuteUbergraph_*, input event sigs,
    // and a long tail of K2Node-specific patterns). Trying to enumerate every legitimate
    // pattern from the outside produced a growing whitelist and kept false-positive-blocking
    // valid authoring (see `B-integrity-gate-false-positive-asyncaction-proxies`).
    //
    // We now trust UE's own CompileBlueprint for class-layer validity. Corruption prevention
    // lives at the source: Phase 0c calls `ScrubStaleUFunctionsFromClass` to remove orphans
    // when nodes are deleted (P0-10), and `RefreshBpirDelegateNodes` keeps CreateDelegate
    // GUIDs in sync (P0-8). These source-level fixes are what actually prevent the
    // cold-reload crash vector; the in-memory FName check below is a last-resort sanity
    // probe for cases where those source fixes are bypassed.
    auto CheckFieldTableSanity = [&OutFailures](UClass* Class, const TCHAR* ClassKind)
    {
        if (!Class) return;

        for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            UFunction* Func = *It;

            FBlueprintIntegrityFailure Failure;
            Failure.NodeKind = TEXT("UFunction");
            Failure.GraphName = ClassKind;

            if (!Func)
            {
                Failure.Reason = FString::Printf(
                    TEXT("null UFunction entry in %s field chain (cold-reload crash vector)"), ClassKind);
                OutFailures.Add(MoveTemp(Failure));
                continue;
            }

            if (!Func->GetFName().IsValid())
            {
                Failure.Reason = FString::Printf(
                    TEXT("%s has UFunction with invalid FName (cold-reload crash vector — TFieldIterator trips)"),
                    ClassKind);
                OutFailures.Add(MoveTemp(Failure));
            }
        }
    };

    CheckFieldTableSanity(Blueprint->SkeletonGeneratedClass, TEXT("SkeletonGeneratedClass"));
    CheckFieldTableSanity(Blueprint->GeneratedClass, TEXT("GeneratedClass"));

    if (UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(Blueprint))
    {
        if (WidgetBP->WidgetTree)
        {
            TArray<UPanelWidget*> Panels;
            WidgetBP->WidgetTree->ForEachWidget([&Panels](UWidget* Widget)
            {
                if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
                {
                    Panels.Add(Panel);
                }
            });

            auto RecordPanelSlotFailure = [&OutFailures](UPanelWidget* Panel, int32 SlotIndex, const FString& Reason)
            {
                FBlueprintIntegrityFailure Failure;
                Failure.NodeKind = TEXT("UPanelSlot");
                Failure.GraphName = TEXT("WidgetTree");
                Failure.Reason = FString::Printf(TEXT("Panel '%s' slot[%d]: %s"),
                    Panel ? *Panel->GetName() : TEXT("<null>"),
                    SlotIndex,
                    *Reason);
                OutFailures.Add(MoveTemp(Failure));
            };

            for (UPanelWidget* Panel : Panels)
            {
                if (!Panel)
                {
                    continue;
                }

                const TArray<UPanelSlot*>& Slots = Panel->GetSlots();
                for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
                {
                    FString InvalidReason;
                    if (WidgetAuthoringHelpers::TryGetInvalidPanelSlotReason(Panel, SlotIndex, nullptr, InvalidReason))
                    {
                        RecordPanelSlotFailure(Panel, SlotIndex, InvalidReason);
                    }
                }
            }
        }
    }

    return OutFailures.Num() == InitialFailureCount;
}

FName GetEffectiveFunctionNameForRemoval(UEdGraphNode* Node)
{
    if (!Node) return NAME_None;
    if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
    {
        return CustomEvent->CustomFunctionName;
    }
    if (UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Node))
    {
        return BoundEvent->CustomFunctionName;
    }
    if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
    {
        return EventNode->EventReference.GetMemberName();
    }
    if (UK2Node_FunctionEntry* FuncEntry = Cast<UK2Node_FunctionEntry>(Node))
    {
        if (UEdGraph* OwnerGraph = FuncEntry->GetGraph())
        {
            return OwnerGraph->GetFName();
        }
    }
    return NAME_None;
}

void RefreshBpirDelegateNodes(UBlueprint* Blueprint, const TArray<FGuid>& /*CreatedNodeGUIDs*/)
{
    // Scope: ALL UK2Node_CreateDelegate nodes in the BP, not just the transaction's
    // freshly-created set. The post-BPIR full compile (`CompileBlueprintWithDiagnostics`)
    // can shift canonical custom-event GUIDs for *every* CreateDelegate in the BP — not
    // just the ones this transaction emitted. Narrow scope was wrong: on a production BP
    // with N pre-existing CreateDelegates (e.g. a front-end menu widget), the gate's P0-5 stale-GUID
    // check flagged N-1 false positives because their skel-time GUIDs no longer matched
    // the post-compile canonical GUIDs. Refreshing everything once is cheap and keeps
    // the gate's view of state consistent.
    //
    // Safety on truly-stale nodes (genuine broken refs from a prior deletion): HandleAnyChange
    // leaves such nodes with an unresolvable name + invalid GUID — the gate's ResolveMember
    // check still catches them via "Unable to find selected function/event".
    if (!Blueprint) return;
    TArray<UEdGraph*> Graphs;
    Graphs.Append(Blueprint->UbergraphPages);
    Graphs.Append(Blueprint->FunctionGraphs);

    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph) continue;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UK2Node_CreateDelegate* CreateDelegate = Cast<UK2Node_CreateDelegate>(Node);
            if (!CreateDelegate) continue;

            // Clear the stale GUID so HandleAnyChange re-resolves against the now-final
            // class state. The resolver fills MemberGuid from FMemberReference::ResolveMember's
            // canonical-GUID lookup, which walks the Blueprint's ubergraph CustomEvent
            // NodeGuids — same path as GetFunctionGuidFromClassByFieldName uses in the gate.
            CreateDelegate->SelectedFunctionGuid.Invalidate();
            CreateDelegate->HandleAnyChange(/*bForceModify=*/false);
        }
    }
}

// Shape-based entry test, mirroring the engine's own compile-time root set:
// UE::KismetCompiler::Private::GatherRootSet (Engine/Source/Editor/KismetCompiler/Private/
// KismetCompiler.cpp), as reached from FKismetCompilerContext::ExpansionStep with
// bIncludeNodesThatCouldBeExpandedToRootSet = true. That is the PRE-expansion pass, so it
// runs over the same authored graph this file's reachability walk sees, and a node the
// compiler roots there is by definition not dead.
//
// The load-bearing clause is the last one: an impure UK2Node with NO input pins at all is a
// root by SHAPE, not by class. That is exactly what an event-shaped node looks like before
// ExpandNode() turns it into real bindings, and it is why UK2Node_EnhancedInputAction is
// covered — a plain UK2Node living in the EnhancedInput plugin's uncooked-only
// InputBlueprintNodes module, which this file must not name or link (the plugin ships for
// UE 5.3-5.8 and the module is optional). Every future node class of that shape is covered
// for free; a class-name seed list has to be extended again each time, which is the defect
// this replaces (B-orphan-sweep-treats-enhanced-input-graph-as-dead).
//
// An impure node that HAS input pins — a stranded PrintString, an unwired Branch, a
// FunctionResult — still fails the test, so genuine orphans keep being reported. Only the
// engine's own "cannot be pruned" set is excluded.
//
// Deliberate shared-entry divergence: UK2Node_Timeline is root-set by TYPE in the engine
// clause but remains excluded from this shared entry predicate. It has input pins, so the
// shape clause never reaches it, and PinWright resolves auto-play timelines through
// CollectLatentExecRootNodes instead (a fallback used only by get_execution_flow, so that a
// timeline whose Play input IS wired is not listed as a duplicate root). The orphan sweep
// adds the engine's Timeline root through IsOrphanSweepRootNode below without changing the
// entry semantics consumed by execution-flow discovery or BPIR emission.
static bool IsEngineCompileRootSetNode(UEdGraphNode* Node)
{
    UK2Node* K2Node = Cast<UK2Node>(Node);
    if (!K2Node)
    {
        return false;
    }

    // Explicit opt-in from the node class itself (AnimGraph roots today; the extension
    // point any plugin node uses to declare "never prune me").
    if (K2Node->IsNodeRootSet())
    {
        return true;
    }

    if (K2Node->IsNodePure())
    {
        return false;
    }

    for (UEdGraphPin* Pin : K2Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Input)
        {
            return false;
        }
    }

    return true;
}

bool IsBlueprintEntryNode(UEdGraphNode* Node)
{
    if (!Node)
    {
        return false;
    }

    if (Node->IsA<UK2Node_Event>()
        || Node->IsA<UK2Node_CustomEvent>()
        || Node->IsA<UK2Node_FunctionEntry>()
        || Node->IsA<UK2Node_InputKey>()
#if defined(__has_include) && __has_include("K2Node_ComponentBoundEvent.h")
        || Node->IsA<UK2Node_ComponentBoundEvent>()
#endif
#if MCP_BPUTILS_HAS_INPUT_ACTION
        || Node->IsA<UK2Node_InputAction>()
#endif
#if MCP_BPUTILS_HAS_INPUT_TOUCH
        || Node->IsA<UK2Node_InputTouch>()
#endif
#if MCP_BPUTILS_HAS_ACTOR_BOUND_EVENT
        || Node->IsA<UK2Node_ActorBoundEvent>()
#endif
#if MCP_BPUTILS_HAS_INPUT_AXIS_EVENT
        || Node->IsA<UK2Node_InputAxisEvent>()
#endif
#if MCP_BPUTILS_HAS_INPUT_AXIS_KEY_EVENT
        || Node->IsA<UK2Node_InputAxisKeyEvent>()
#endif
        )
    {
        return true;
    }

    // Macro graph entry tunnels: output-only tunnel nodes are entry points.
    // Must come after all the IsA<> checks above because UK2Node_MacroInstance
    // derives from UK2Node_Tunnel — we only want bare tunnels here.
    if (IsMacroEntryTunnel(Node))
    {
        return true;
    }

    // General, class-agnostic backstop: anything the Kismet compiler itself roots.
    // The named classes above are kept as a fast path and as documentation of the
    // common cases; this clause is what covers node classes this module cannot name.
    if (IsEngineCompileRootSetNode(Node))
    {
        return true;
    }

    return false;
}

// The orphan sweep follows the engine's retention roots, including Timelines rooted by type,
// without promoting Timelines to shared Blueprint entry points.
static bool IsOrphanSweepRootNode(UEdGraphNode* Node)
{
    return IsBlueprintEntryNode(Node)
        || (Node && Node->IsA<UK2Node_Timeline>());
}

bool IsMacroEntryTunnel(const UEdGraphNode* Node)
{
    // Bare output-only tunnel: macro graph entry. Must reject UK2Node_MacroInstance
    // (which derives from UK2Node_Tunnel but represents a callsite, not an entry).
    const UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
    return Tunnel
        && !Node->IsA<UK2Node_MacroInstance>()
        && Tunnel->bCanHaveOutputs
        && !Tunnel->bCanHaveInputs;
}

bool IsBlueprintOrphanExitTunnel(const UEdGraphNode* Node)
{
    const UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
    return Tunnel
        && !Node->IsA<UK2Node_MacroInstance>()
        && Tunnel->bCanHaveInputs
        && !Tunnel->bCanHaveOutputs;
}

bool FBlueprintOrphanReachability::IsStructuralNode(UEdGraphNode* Node) const
{
    return !Node
        || Node->IsA<UEdGraphNode_Comment>()
        || IsBlueprintEntryNode(Node)
        || IsBlueprintOrphanExitTunnel(Node);
}

bool FBlueprintOrphanReachability::IsOrphan(
    UEdGraphNode* Node,
    bool bIncludeDataOnly) const
{
    if (IsStructuralNode(Node))
    {
        return false;
    }

    if (HasExecPins(Node))
    {
        return !IsExecReachable(Node);
    }

    return bIncludeDataOnly
        && !IsStandalonePure(Node)
        && !IsDataReachable(Node);
}

bool FindMacroTunnelPair(UEdGraph* Graph, UK2Node_Tunnel*& OutEntryTunnel, UK2Node_Tunnel*& OutExitTunnel)
{
    OutEntryTunnel = nullptr;
    OutExitTunnel = nullptr;
    if (!Graph)
    {
        return false;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (IsMacroEntryTunnel(Node))
        {
            OutEntryTunnel = Cast<UK2Node_Tunnel>(Node);
        }
        else if (IsBlueprintOrphanExitTunnel(Node))
        {
            OutExitTunnel = Cast<UK2Node_Tunnel>(Node);
        }
    }

    return OutEntryTunnel && OutExitTunnel;
}

TArray<UEdGraph*> CollectAllBlueprintGraphsRecursive(UBlueprint* Blueprint)
{
    TArray<UEdGraph*> Result;
    if (!Blueprint)
    {
        return Result;
    }

    TSet<const UEdGraph*> Seen;
    TArray<UEdGraph*> Stack;

    // Seed the stack in top-level list order so the output is:
    // Ubergraph subtrees, then Function subtrees, Macro subtrees, Delegate subtrees.
    // Push in reverse order so the first list pops first (LIFO → DFS left-to-right).
    auto Seed = [&](const TArray<UEdGraph*>& List)
    {
        for (int32 i = List.Num() - 1; i >= 0; --i)
        {
            if (List[i] && !Seen.Contains(List[i]))
            {
                Stack.Add(List[i]);
            }
        }
    };
    Seed(Blueprint->DelegateSignatureGraphs);
    Seed(Blueprint->MacroGraphs);
    Seed(Blueprint->FunctionGraphs);
    Seed(Blueprint->UbergraphPages);

    while (Stack.Num() > 0)
    {
        UEdGraph* Graph = Stack.Pop(EAllowShrinking::No);
        if (!Graph || Seen.Contains(Graph))
        {
            continue;
        }
        Seen.Add(Graph);
        Result.Add(Graph);

        // Push direct children in reverse order for left-to-right DFS.
        // GetAllChildrenGraphs returns direct children only; we recurse via the stack.
        TArray<UEdGraph*> Children;
        Graph->GetAllChildrenGraphs(Children);
        for (int32 i = Children.Num() - 1; i >= 0; --i)
        {
            if (Children[i] && !Seen.Contains(Children[i]))
            {
                Stack.Add(Children[i]);
            }
        }
    }

    return Result;
}

void CollectEntryNodesRecursive(UEdGraph* Root, TArray<UEdGraphNode*>& OutEntries)
{
    if (!Root)
    {
        return;
    }

    TSet<const UEdGraph*> Seen;
    TArray<UEdGraph*> Stack;
    Stack.Add(Root);

    while (Stack.Num() > 0)
    {
        UEdGraph* Graph = Stack.Pop(EAllowShrinking::No);
        if (!Graph || Seen.Contains(Graph))
        {
            continue;
        }
        Seen.Add(Graph);

        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && IsBlueprintEntryNode(Node))
            {
                OutEntries.Add(Node);
            }
        }

        TArray<UEdGraph*> Children;
        Graph->GetAllChildrenGraphs(Children);
        for (int32 i = Children.Num() - 1; i >= 0; --i)
        {
            if (Children[i] && !Seen.Contains(Children[i]))
            {
                Stack.Add(Children[i]);
            }
        }
    }
}

void CollectBlueprintOrphanGraphFamily(UEdGraph* Root, TArray<UEdGraph*>& OutGraphs)
{
    TSet<const UEdGraph*> Seen;
    for (UEdGraph* Existing : OutGraphs)
    {
        if (Existing)
        {
            Seen.Add(Existing);
        }
    }

    if (!Root)
    {
        return;
    }

    TArray<UEdGraph*> Stack;
    Stack.Add(Root);
    while (Stack.Num() > 0)
    {
        UEdGraph* Graph = Stack.Pop(EAllowShrinking::No);
        if (!Graph || Seen.Contains(Graph))
        {
            continue;
        }
        Seen.Add(Graph);
        OutGraphs.Add(Graph);

        TArray<UEdGraph*> Children;
        Graph->GetAllChildrenGraphs(Children);
        for (int32 Index = Children.Num() - 1; Index >= 0; --Index)
        {
            if (Children[Index] && !Seen.Contains(Children[Index]))
            {
                Stack.Add(Children[Index]);
            }
        }
    }
}

void CollectLatentExecRootNodes(UEdGraph* Graph, TArray<UEdGraphNode*>& OutRoots)
{
    if (!Graph)
    {
        return;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node || IsBlueprintEntryNode(Node))
        {
            continue;
        }

        // A latent exec root drives the graph from an exec OUTPUT pin while having no
        // wired exec INPUT. Auto-play timelines are the canonical case (Play/Stop exec
        // inputs unconnected, Update/Finished/Impact exec outputs drive the chain); a
        // timeline that IS called from upstream has a connected Play input and is
        // therefore reachable from its real entry — excluded here on purpose. Both halves
        // scan ALL exec pins of the direction (a Timeline has several per direction), so
        // HasConnectedExecPin is used because it checks every exec pin, not only the first.
        if (HasConnectedExecPin(Node, EGPD_Output) && !HasConnectedExecPin(Node, EGPD_Input))
        {
            OutRoots.Add(Node);
        }
    }
}

static TSet<FBlueprintOrphanNodeKey> BuildExecReachabilityKeySet(
    const TArray<UEdGraph*>& Graphs)
{
    TSet<FBlueprintOrphanNodeKey> Reachable;

    auto MarkNode = [&Reachable](UEdGraphNode* Node, TArray<UEdGraphNode*>& Queue)
    {
        if (!Node || !Node->NodeGuid.IsValid())
        {
            return;
        }
        const FBlueprintOrphanNodeKey Key{Node->GetGraph(), Node->NodeGuid};
        bool bAlreadyInSet = false;
        Reachable.Add(Key, &bAlreadyInSet);
        if (!bAlreadyInSet)
        {
            Queue.Add(Node);
        }
    };

    TSet<const UEdGraph*> SeenGraphs;
    for (UEdGraph* Graph : Graphs)
    {
        if (!Graph || SeenGraphs.Contains(Graph))
        {
            continue;
        }
        SeenGraphs.Add(Graph);

        const int32 NodeCount = Graph->Nodes.Num();
        TArray<UEdGraphNode*> Queue;
        Queue.Reserve(NodeCount);

        // Seed: shared entry nodes, Timelines rooted by the engine, and composites whose
        // BoundGraph has an entry-point node.
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }
            if (IsOrphanSweepRootNode(Node))
            {
                MarkNode(Node, Queue);
            }
            else if (UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
            {
                TArray<UEdGraphNode*> BoundEntries;
                CollectEntryNodesRecursive(Composite->BoundGraph, BoundEntries);
                if (BoundEntries.Num() > 0)
                {
                    MarkNode(Composite, Queue);
                }
            }
        }

        // BFS over output exec edges
        while (Queue.Num() > 0)
        {
            UEdGraphNode* Current = Queue.Pop(EAllowShrinking::No);
            for (UEdGraphPin* Pin : Current->Pins)
            {
                if (!Pin) continue;
                if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
                if (Pin->Direction != EGPD_Output) continue;

                for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
                {
                    if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
                    MarkNode(LinkedPin->GetOwningNode(), Queue);
                }
            }
        }
    }

    return Reachable;
}

TSet<FGuid> BuildExecReachabilitySet(const TArray<UEdGraph*>& Graphs)
{
    TSet<FGuid> Reachable;
    for (const FBlueprintOrphanNodeKey& Key : BuildExecReachabilityKeySet(Graphs))
    {
        Reachable.Add(Key.NodeGuid);
    }
    return Reachable;
}

static void MarkDataReachableNode(
    UEdGraphNode* Node,
    TSet<FBlueprintOrphanNodeKey>& Reachable,
    TArray<UEdGraphNode*>& Queue)
{
    if (!Node || !Node->NodeGuid.IsValid())
    {
        return;
    }

    const FBlueprintOrphanNodeKey Key{Node->GetGraph(), Node->NodeGuid};
    bool bAlreadyInSet = false;
    Reachable.Add(Key, &bAlreadyInSet);
    if (!bAlreadyInSet)
    {
        Queue.Add(Node);
    }
}

FBlueprintOrphanReachability BuildBlueprintOrphanReachability(
    const TArray<UEdGraph*>& Graphs,
    bool bIncludeDataOnly)
{
    FBlueprintOrphanReachability Model;
    TArray<UEdGraph*> UniqueGraphs;
    TSet<const UEdGraph*> SeenGraphs;
    for (UEdGraph* Graph : Graphs)
    {
        if (Graph && !SeenGraphs.Contains(Graph))
        {
            SeenGraphs.Add(Graph);
            UniqueGraphs.Add(Graph);
        }
    }

    bool bNeedsDataReachability = false;
    for (UEdGraph* Graph : UniqueGraphs)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node || !Node->NodeGuid.IsValid())
            {
                continue;
            }

            const FBlueprintOrphanNodeKey Key{Graph, Node->NodeGuid};
            FBlueprintOrphanNodeShape Shape;
            for (const UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin)
                {
                    continue;
                }
                if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                {
                    Shape.bHasExecPins = true;
                }
                else if (Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
                {
                    Shape.bHasLinkedDataOutput = true;
                }
            }
            Model.NodeShapes.Add(Key, Shape);
            bNeedsDataReachability |= !Shape.bHasExecPins && Shape.bHasLinkedDataOutput;

            // This is the only BPIR output-policy exception: a pure node with no data
            // consumer is emitted as a standalone statement for round-trip preservation.
            // Classify it while the node shape is already cached, so the model does not
            // need a second full graph walk.
            if (!Model.IsStructuralNode(Node)
                && !Node->IsA<UK2Node_Self>()
                && !Node->IsA<UK2Node_VariableGet>()
                && !Node->IsA<UK2Node_Knot>()
                && !Shape.bHasExecPins
                && !Shape.bHasLinkedDataOutput)
            {
                Model.StandalonePureNodes.Add(Key);
            }

        }
    }

    Model.ExecReachableNodes = BuildExecReachabilityKeySet(UniqueGraphs);
    Model.DataReachableNodes = Model.ExecReachableNodes;

    // A pure node with a linked data output is the only shape that can be affected by the
    // backward closure. Avoid the extra walk when the caller requested data-only handling
    // but this graph family has no such candidate (including BPIR's standalone-pure case).
    if (!bIncludeDataOnly || !bNeedsDataReachability)
    {
        return Model;
    }

    TArray<UEdGraphNode*> Queue;
    Queue.Reserve(Model.DataReachableNodes.Num());

    for (UEdGraph* Graph : UniqueGraphs)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (!Node)
            {
                continue;
            }

            const FBlueprintOrphanNodeKey Key{Graph, Node->NodeGuid};
            if (Model.DataReachableNodes.Contains(Key))
            {
                Queue.Add(Node);
            }
            // A bound graph's exit tunnel is a live data sink: whatever feeds it is consumed
            // by the owning composite, even when no exec edge reaches it. A
            // UK2Node_MathExpression subgraph is entirely pure, so exec seeding stops at the
            // entry tunnel and the backward walk below would otherwise report every generated
            // expression node — and the exit tunnel itself — as an orphan.
            else if (IsBlueprintOrphanExitTunnel(Node))
            {
                MarkDataReachableNode(Node, Model.DataReachableNodes, Queue);
            }
        }
    }

    while (Queue.Num() > 0)
    {
        UEdGraphNode* Current = Queue.Pop(EAllowShrinking::No);
        for (UEdGraphPin* Pin : Current->Pins)
        {
            if (!Pin) continue;
            if (Pin->Direction != EGPD_Input) continue;
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

            for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
            {
                if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
                MarkDataReachableNode(
                    LinkedPin->GetOwningNode(), Model.DataReachableNodes, Queue);
            }
        }
    }

    return Model;
}

FBlueprintOrphanReachability BuildBlueprintOrphanReachability(
    UEdGraph* Graph,
    bool bIncludeDataOnly)
{
    TArray<UEdGraph*> SingleGraph;
    if (Graph)
    {
        SingleGraph.Add(Graph);
    }
    return BuildBlueprintOrphanReachability(SingleGraph, bIncludeDataOnly);
}

static void ScanGraphForBlueprintOrphans(
    UEdGraph* Graph,
    bool bIncludeDataOnly,
    const FString& SourceGraphName,
    const FBlueprintOrphanReachability& OrphanModel,
    TArray<FBlueprintOrphanNodeInfo>& OutOrphans)
{
    if (!Graph)
    {
        return;
    }

    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node) continue;
        // Structural nodes are graph boundaries/connectors, not sweepable candidates.
        // UK2Node_Tunnel::DestroyNode()
        // nulls the twin's back-pointer, so deleting a composite's exit tunnel leaves the
        // surviving UK2Node_Composite with OutputSourceNode == nullptr. That state compiles and
        // saves clean, then kills the editor on the package's next load: regenerate-on-load
        // reaches UK2Node_MathExpression::ReconstructNode() -> RebuildExpression() ->
        // ClearExpression() -> GetExitNode(), whose check(OutputSourceNode) is a fatal assert
        // with no recovery path. Delete the owning composite instead — its DestroyNode() tears
        // the bound graph down as a unit.
        if (OrphanModel.IsOrphan(Node, bIncludeDataOnly))
        {
            FBlueprintOrphanNodeInfo Info;
            Info.NodeGuid = Node->NodeGuid;
            Info.NodeType = Node->GetClass()->GetName();
            Info.Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
            Info.SourceGraphName = SourceGraphName;
            Info.bHasExecPins = OrphanModel.HasExecPins(Node);
            Info.WeakNodePtr = Node;
            OutOrphans.Add(MoveTemp(Info));
        }
    }
}

TArray<FBlueprintOrphanNodeInfo> FindBlueprintOrphanNodes(
    UBlueprint* Blueprint,
    bool bIncludeDataOnly,
    UEdGraph* TargetGraph,
    bool bIncludeNestedGraphs)
{
    TArray<FBlueprintOrphanNodeInfo> Orphans;
    if (!Blueprint)
    {
        return Orphans;
    }

    if (TargetGraph)
    {
        TArray<UEdGraph*> GraphsToScan;
        if (bIncludeNestedGraphs)
        {
            CollectBlueprintOrphanGraphFamily(TargetGraph, GraphsToScan);
        }
        else
        {
            GraphsToScan.Add(TargetGraph);
        }
        const FBlueprintOrphanReachability OrphanModel =
            BuildBlueprintOrphanReachability(GraphsToScan, bIncludeDataOnly);
        for (UEdGraph* Graph : GraphsToScan)
        {
            if (!Graph) continue;
            ScanGraphForBlueprintOrphans(
                Graph, bIncludeDataOnly, Graph->GetName(), OrphanModel, Orphans);
        }
        return Orphans;
    }

    const TArray<UEdGraph*> GraphsToScan = CollectAllBlueprintGraphsRecursive(Blueprint);
    const FBlueprintOrphanReachability OrphanModel =
        BuildBlueprintOrphanReachability(GraphsToScan, bIncludeDataOnly);
    for (UEdGraph* Graph : GraphsToScan)
    {
        if (!Graph) continue;
        ScanGraphForBlueprintOrphans(
            Graph, bIncludeDataOnly, Graph->GetName(), OrphanModel, Orphans);
    }
    return Orphans;
}

TSet<FGuid> SnapshotBlueprintOrphanGuids(
    UBlueprint* Blueprint,
    bool bIncludeDataOnly,
    UEdGraph* TargetGraph)
{
    TSet<FGuid> Snapshot;
    for (const FBlueprintOrphanNodeInfo& Info : FindBlueprintOrphanNodes(Blueprint, bIncludeDataOnly, TargetGraph))
    {
        if (Info.NodeGuid.IsValid())
        {
            Snapshot.Add(Info.NodeGuid);
        }
    }
    return Snapshot;
}

FBlueprintOrphanDeltaCleanupResult CleanupNewBlueprintOrphans(
    UBlueprint* Blueprint,
    const TSet<FGuid>& BeforeOrphanGuids,
    bool bCleanupNewOrphans,
    bool bIncludeDataOnly,
    UEdGraph* TargetGraph)
{
    FBlueprintOrphanDeltaCleanupResult Result;
    Result.bCleanupNewOrphans = bCleanupNewOrphans;

    TArray<FBlueprintOrphanNodeInfo> AfterOrphans =
        FindBlueprintOrphanNodes(Blueprint, bIncludeDataOnly, TargetGraph);
    for (const FBlueprintOrphanNodeInfo& Info : AfterOrphans)
    {
        if (Info.NodeGuid.IsValid() && !BeforeOrphanGuids.Contains(Info.NodeGuid))
        {
            Result.NewOrphans.Add(Info);
        }
    }

    if (!bCleanupNewOrphans || !Blueprint)
    {
        return Result;
    }

    for (const FBlueprintOrphanNodeInfo& Info : Result.NewOrphans)
    {
        if (!Info.WeakNodePtr.IsValid())
        {
            continue;
        }

        UEdGraphNode* Node = Info.WeakNodePtr.Get();
        if (UEdGraph* Graph = Node->GetGraph())
        {
            Graph->Modify();
        }
        FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
        Result.DeletedNewOrphans.Add(Info);
    }

    return Result;
}

TArray<TSharedPtr<FJsonValue>> BuildOrphanNodeInfoJsonArray(
    const TArray<FBlueprintOrphanNodeInfo>& Orphans)
{
    TArray<TSharedPtr<FJsonValue>> OrphanArray;
    OrphanArray.Reserve(Orphans.Num());

    for (const FBlueprintOrphanNodeInfo& Info : Orphans)
    {
        TSharedPtr<FJsonObject> OrphanObj = MakeShared<FJsonObject>();
        OrphanObj->SetStringField(TEXT("nodeId"), Info.NodeGuid.ToString());
        OrphanObj->SetStringField(TEXT("nodeType"), Info.NodeType);
        OrphanObj->SetStringField(TEXT("title"), Info.Title);
        OrphanObj->SetStringField(TEXT("graphName"), Info.SourceGraphName);
        OrphanObj->SetBoolField(TEXT("hasExecPins"), Info.bHasExecPins);
        OrphanArray.Add(MakeShared<FJsonValueObject>(OrphanObj));
    }

    return OrphanArray;
}

void AddOrphanDeltaCleanupResultToJson(
    const FBlueprintOrphanDeltaCleanupResult& CleanupResult,
    const TSharedPtr<FJsonObject>& Out)
{
    if (!Out.IsValid())
    {
        return;
    }

    Out->SetBoolField(TEXT("cleanupNewOrphans"), CleanupResult.bCleanupNewOrphans);
    Out->SetArrayField(TEXT("newOrphanedNodes"), BuildOrphanNodeInfoJsonArray(CleanupResult.NewOrphans));
    Out->SetNumberField(TEXT("newOrphanedCount"), CleanupResult.NewOrphans.Num());
    Out->SetArrayField(TEXT("deletedNewOrphans"), BuildOrphanNodeInfoJsonArray(CleanupResult.DeletedNewOrphans));
    Out->SetNumberField(TEXT("deletedNewOrphanCount"), CleanupResult.DeletedNewOrphans.Num());
}

FText MakeBlueprintCategoryText(const FString& Category)
{
    return Category.IsEmpty() ? FText::GetEmpty() : FText::FromString(Category);
}

} // namespace BlueprintHandlerUtils
