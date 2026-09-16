// Copyright (c) 2026 Alexander Penkin. MIT License.

// PCGSetNodeProperty.cpp - pcg.set_node_property
//
// The generic per-node settings editor. `pcg.add_node` resolves ANY UPCGSettings subclass, so the
// surface could add every node the engine defines while only three of them
// (add_slope_filter / add_noise_filter / set_self_pruning_settings) had a way to be configured.
// This writes one reflected property on a node's UPCGSettings object, which closes that gap for
// every subclass without a typed setter per class.
//
// REUSE, NOT A SECOND WRITER. A UPCGSettings object is a plain UObject, so nothing here needs its
// own reflection code: resolution is ResolvePropertyOnObject (the dotted-path dispatch primitive
// property.set uses), conversion is ApplyJsonValueToProperty, the change notification is
// PinWright::NotifyPropertyChanged, and the read-back is ExportPropertyToJsonValue. All four are
// the main module's shared primitives.
//
// WHY THE NOTIFICATION IS ADDRESSED TO THE SETTINGS OBJECT, NOT THE NODE. UPCGSettings's own
// PostEditChangeProperty is the only thing that broadcasts OnSettingsChangedDelegate
// (PCGSettings.cpp:744). UPCGNode::OnSettingsChanged forwards that as OnNodeChangedDelegate
// (PCGNode.cpp:869-881), UPCGGraph::OnNodeChanged turns it into NotifyGraphChanged
// (PCGGraph.cpp:2311), and that is the chain that dirties the components a later pcg.generate
// reads. A raw store with no notification - or one addressed to the UPCGNode, whose own
// PostEditChangeProperty only broadcasts EPCGChangeType::Cosmetic - leaves the new value sitting
// in memory, correct on read-back and invisible to generation. That exact shape is what
// B-property-set-object-hop-notification-noop measured on a PCG node.
//
// MEASURED, NOT ECHOED. `value` is read off the property AFTER the write and after the
// notification ran; `requestedValue` carries what the call asked for, separately. The two can
// legitimately disagree - a float property narrows the double the wire carried, an enum name that
// does not resolve leaves the field untouched, a PostEditChangeProperty override clamps - so
// `valueMatchesRequest` is a comparison of the two, omitted (with a warning) for shapes this verb
// cannot compare rather than guessed.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/ParamSpec.h"

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"

#include "Handlers/PCG/PCGHandlerHelpers.h"
#include "Utils/PropertyChangeNotify.h"
#include "Utils/PropertyExport.h"
#include "Utils/PropertyImport.h"
#include "Utils/PropertyInspection.h"

// Distinctive namespace: this module builds with unity on, so file-local helper names have to be
// unique across every .cpp folded into the same TU.
namespace PinWrightPCGSetNodeProperty
{
    enum class EValueAgreement : uint8
    {
        Match,
        Mismatch,
        // The requested and measured shapes are not comparable by any rule this verb models
        // (structs, arrays, object references, an enum ordinal whose name did not resolve).
        // Reported as an omitted field plus a warning, never as a false Match.
        NotComparable,
    };

    const UEnum* ResolveEnumForProperty(const FProperty* Property)
    {
        if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
        {
            return EnumProp->GetEnum();
        }
        if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
        {
            return ByteProp->Enum;
        }
        return nullptr;
    }

    // Exact double equality is the wrong test: the wire carries doubles and an FFloatProperty
    // stores float32, so a requested 0.1 reads back as 0.10000000149011612 and an exact compare
    // would report a mismatch on nearly every float write. The tolerance is relative so it holds
    // across magnitudes.
    bool NumbersAgreeWithinFloatPrecision(double A, double B)
    {
        const double Scale = FMath::Max(1.0, FMath::Max(FMath::Abs(A), FMath::Abs(B)));
        return FMath::Abs(A - B) <= 1.0e-6 * Scale;
    }

    EValueAgreement CompareRequestedToMeasured(const TSharedPtr<FJsonValue>& Requested,
                                               const TSharedPtr<FJsonValue>& Measured,
                                               const FProperty* Property)
    {
        if (!Requested.IsValid() || !Measured.IsValid())
        {
            return EValueAgreement::NotComparable;
        }

        const UEnum* PropertyEnum = ResolveEnumForProperty(Property);

        if (Requested->Type == EJson::Number && Measured->Type == EJson::Number)
        {
            return NumbersAgreeWithinFloatPrecision(Requested->AsNumber(), Measured->AsNumber())
                ? EValueAgreement::Match : EValueAgreement::Mismatch;
        }

        if (Requested->Type == EJson::String && Measured->Type == EJson::String)
        {
            // For an enum, compare the VALUES the two names resolve to: UEnum name lookup is
            // case-insensitive, so a caller who wrote "voronoi2d" got exactly what it asked for
            // even though the read-back spells the canonical "Voronoi2D".
            if (PropertyEnum)
            {
                const int64 RequestedEnum = PropertyEnum->GetValueByNameString(Requested->AsString());
                const int64 MeasuredEnum = PropertyEnum->GetValueByNameString(Measured->AsString());
                if (RequestedEnum != INDEX_NONE && MeasuredEnum != INDEX_NONE)
                {
                    return RequestedEnum == MeasuredEnum
                        ? EValueAgreement::Match : EValueAgreement::Mismatch;
                }
            }
            return Requested->AsString().Equals(Measured->AsString(), ESearchCase::CaseSensitive)
                ? EValueAgreement::Match : EValueAgreement::Mismatch;
        }

        if (Requested->Type == EJson::Boolean && Measured->Type == EJson::Boolean)
        {
            return Requested->AsBool() == Measured->AsBool()
                ? EValueAgreement::Match : EValueAgreement::Mismatch;
        }

        // An enum written by ordinal reads back as its symbolic NAME (PropertyExport.cpp emits the
        // name whenever the value resolves), so the two sides are one value in two spellings.
        if (Requested->Type == EJson::Number && Measured->Type == EJson::String && PropertyEnum)
        {
            const int64 MeasuredEnum = PropertyEnum->GetValueByNameString(Measured->AsString());
            if (MeasuredEnum != INDEX_NONE)
            {
                return NumbersAgreeWithinFloatPrecision(Requested->AsNumber(), static_cast<double>(MeasuredEnum))
                    ? EValueAgreement::Match : EValueAgreement::Mismatch;
            }
        }

        return EValueAgreement::NotComparable;
    }

    // Short human-readable rendering for a warning line. Structured values are named rather than
    // serialized: the response already carries both of them verbatim.
    FString DescribeJsonValue(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("<absent>");
        }
        switch (Value->Type)
        {
        case EJson::String:  return FString::Printf(TEXT("\"%s\""), *Value->AsString());
        case EJson::Number:  return FString::SanitizeFloat(Value->AsNumber());
        case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
        case EJson::Null:    return TEXT("null");
        default:             return TEXT("<structured value>");
        }
    }
}

REGISTER_RPC_HANDLER("pcg.set_node_property", "pcg",
    "Write one reflected property on a PCG node's UPCGSettings object - the generic form of the "
    "per-class typed setters, so any UPCGSettings subclass is configurable by property name. "
    "`value` is MEASURED: it is read back off the property after the write and after the change "
    "notification ran, `requestedValue` carries what the call asked for separately, and "
    "`valueMatchesRequest` compares the two (omitted, with a warning, for shapes that cannot be "
    "compared). The write runs inside an FScopedTransaction so editor undo restores it, and fires "
    "the settings object's PostEditChangeProperty - the notification that makes a later "
    "pcg.generate see the new value.",
    RPC_PARAMS(
        RPC_PARAM_REQ("graphPath", "path", "PCG graph asset path."),
        RPC_PARAM_REQ("nodeId", "string", "Node id as reported by pcg.inspect (UPCGNode::GetName())."),
        RPC_PARAM_REQ_ALIAS("property", "string",
            "UPCGSettings property name. A dotted path reaches into a nested struct member, "
            "e.g. Parameters.PruningType on a UPCGSelfPruningSettings node.", "propertyName"),
        RPC_PARAM_REQ("value", "any",
            "Value to write. Enums accept the symbolic name or the ordinal; numbers, strings, "
            "bools and struct objects follow the same conversion rules as property.set.")
    ))
{
    using namespace PinWrightPCGSetNodeProperty;

    FString GraphPath;
    if (!Ctx.RequireString(TEXT("graphPath"), GraphPath)) return true;

    FString NodeId;
    if (!Ctx.RequireString(TEXT("nodeId"), NodeId)) return true;

    const FString PropertyName =
        Ctx.GetStringFirstOf({TEXT("property"), TEXT("propertyName")}).TrimStartAndEnd();
    if (PropertyName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PROPERTY,
            TEXT("pcg.set_node_property requires a non-empty 'property'."));
        return true;
    }

    const TSharedPtr<FJsonValue> RequestedValue = Ctx.GetJsonValueFirstOf({TEXT("value")});
    if (!RequestedValue.IsValid() || RequestedValue->Type == EJson::Null)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_VALUE,
            TEXT("pcg.set_node_property requires a non-null 'value'."));
        return true;
    }

    UPCGGraph* Graph = PinWrightPCG::LoadGraphOrError(Ctx, GraphPath);
    if (!Graph) return true;

    // Implicit endpoints are resolved so the refusal below can name them, matching
    // pcg.remove_node's vocabulary; a caller that mistypes a user node still gets NODE_NOT_FOUND.
    UPCGNode* Node = PinWrightPCG::FindNodeByNameIncludingImplicit(Graph, NodeId);
    if (!Node)
    {
        Ctx.SendError(ErrorCodes::ERR_NODE_NOT_FOUND,
            FString::Printf(TEXT("Could not find node '%s' in graph %s."), *NodeId, *GraphPath));
        return true;
    }

    if (Node == Graph->GetInputNode() || Node == Graph->GetOutputNode())
    {
        Ctx.SendError(ErrorCodes::ERR_IMMUTABLE_NODE,
            FString::Printf(
                TEXT("Cannot write settings on implicit input/output node: %s"), *NodeId));
        return true;
    }

    UPCGSettings* Settings = Node->GetSettings();
    if (!Settings)
    {
        Ctx.SendError(ErrorCodes::ERR_NO_NODE_SETTINGS,
            FString::Printf(TEXT("Node '%s' carries no UPCGSettings object to write to."), *NodeId));
        return true;
    }

    void* Container = nullptr;
    FString ResolveError;
    FProperty* Property = ResolvePropertyOnObject(Settings, PropertyName, Container, ResolveError);
    if (!Property || !Container)
    {
        Ctx.SendError(ErrorCodes::ERR_PROPERTY_NOT_FOUND,
            FString::Printf(TEXT("%s on node '%s' (settings class %s)."),
                *ResolveError, *NodeId, *Settings->GetClass()->GetPathName()));
        return true;
    }

    // The top-level UPROPERTY of the settings object that contains the leaf. Engine overrides
    // split on BOTH names - GetPropertyName() reads the leaf, GetMemberPropertyName() reads this -
    // so a nested write that names only the leaf misses every member-matched branch
    // (Utils/PropertyChangeNotify.h). Null for a single-segment write, where FPropertyChangedEvent
    // already sets MemberProperty = Property.
    FProperty* MemberProperty = nullptr;
    int32 FirstDotIndex = INDEX_NONE;
    if (PropertyName.FindChar(TEXT('.'), FirstDotIndex))
    {
        MemberProperty = FindFProperty<FProperty>(
            Settings->GetClass(), FName(*PropertyName.Left(FirstDotIndex)));
    }

    FString ConversionError;
    bool bApplied = false;
    bool bChangeNotified = false;
    {
        // ApplyJsonValueToProperty stores straight into the property's memory, so the pre-write
        // value only survives an undo if Modify() records it into an ACTIVE transaction.
        FScopedTransaction Transaction(
            NSLOCTEXT("PinWright", "PcgSetNodeProperty", "PCG Set Node Property"));
        Settings->Modify();

        bApplied = ApplyJsonValueToProperty(Container, Property, RequestedValue, ConversionError);
        if (!bApplied)
        {
            // Nothing was written; do not leave a no-op entry on the editor's undo stack.
            Transaction.Cancel();
        }
        else
        {
            bChangeNotified = PinWright::NotifyPropertyChanged(
                Settings, Property, MemberProperty, EPropertyChangeType::ValueSet);
        }
    }

    if (!bApplied)
    {
        Ctx.SendError(ErrorCodes::ERR_PROPERTY_CONVERSION_FAILED, ConversionError);
        return true;
    }

    Settings->MarkPackageDirty();
    Graph->MarkPackageDirty();

    // Re-resolved rather than reusing the pre-write container: the read-back has to describe the
    // object as it stands after PostEditChangeProperty ran, and an override is free to have
    // rebuilt whatever the nested container pointed into.
    void* ReadbackContainer = nullptr;
    FString ReadbackError;
    FProperty* ReadbackProperty = IsValid(Settings)
        ? ResolvePropertyOnObject(Settings, PropertyName, ReadbackContainer, ReadbackError)
        : nullptr;
    const TSharedPtr<FJsonValue> MeasuredValue = (ReadbackProperty && ReadbackContainer)
        ? ExportPropertyToJsonValue(ReadbackContainer, ReadbackProperty)
        : nullptr;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Result->SetStringField(TEXT("nodeId"), NodeId);
    Result->SetStringField(TEXT("property"), PropertyName);
    Result->SetStringField(TEXT("propertyType"), GetPropertyCppTypeWithParams(Property));
    Result->SetStringField(TEXT("settingsClass"), Settings->GetClass()->GetPathName());
    // The object the write actually landed on. pcg.inspect reports only the settings CLASS path,
    // so this is the only place a caller learns the sub-object path to hand to property.get.
    Result->SetStringField(TEXT("settingsPath"), Settings->GetPathName());
    Result->SetField(TEXT("requestedValue"), RequestedValue);
    // The engine's change path RAN. It does not claim anything downstream recomputed.
    Result->SetBoolField(TEXT("changeNotified"), bChangeNotified);

    const UPackage* SettingsPackage = Settings->GetOutermost();
    Result->SetBoolField(TEXT("markedDirty"), SettingsPackage && SettingsPackage->IsDirty());

    TArray<TSharedPtr<FJsonValue>> Warnings;

    if (MeasuredValue.IsValid())
    {
        Result->SetField(TEXT("value"), MeasuredValue);

        const EValueAgreement Agreement =
            CompareRequestedToMeasured(RequestedValue, MeasuredValue, ReadbackProperty);
        if (Agreement != EValueAgreement::NotComparable)
        {
            Result->SetBoolField(TEXT("valueMatchesRequest"), Agreement == EValueAgreement::Match);
        }

        if (Agreement == EValueAgreement::Mismatch)
        {
            Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("'%s' reads %s after the write, not the requested %s. The store succeeded, so "
                     "the difference came from the property's own type (a narrowing conversion) or "
                     "from the settings class's PostEditChangeProperty clamping/overriding it. "
                     "'value' is what the graph will generate with."),
                *PropertyName, *DescribeJsonValue(MeasuredValue), *DescribeJsonValue(RequestedValue))));
        }
        else if (Agreement == EValueAgreement::NotComparable)
        {
            Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("'valueMatchesRequest' is omitted for '%s': the requested and read-back shapes "
                     "are not comparable by any rule this verb models, so no agreement claim is "
                     "made. Both values are reported - compare them yourself."),
                *PropertyName)));
        }
    }
    else
    {
        // Omitted rather than echoed: publishing 'requestedValue' as 'value' here would be exactly
        // the false-success this verb exists to avoid.
        const FString ReadbackDetail = ReadbackError.IsEmpty()
            ? FString(TEXT("."))
            : FString::Printf(TEXT(": %s"), *ReadbackError);
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("The write was applied but '%s' could not be read back, so 'value' and "
                 "'valueMatchesRequest' are omitted rather than echoed from the request%s"),
            *PropertyName, *ReadbackDetail)));
    }

    if (Graph->GetOutermost() != SettingsPackage)
    {
        Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("Node '%s' uses a settings object stored outside the graph asset (%s). The write "
                 "landed on that shared object, so every node and graph instancing it now reads "
                 "the new value."),
            *NodeId, *Settings->GetPathName())));
    }

    if (Warnings.Num() > 0)
    {
        Result->SetArrayField(TEXT("warnings"), Warnings);
    }

    Ctx.SendSuccess(Result);
    return true;
}

#endif // __has_include("PCGGraph.h")
