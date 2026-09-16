// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"

#if defined(__has_include) && __has_include("PCGGraph.h")

#include "Compat/EngineVersionCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/JsonUtils.h"
#include "UObject/UObjectGlobals.h"

#include "PCGGraph.h"
#include "Handlers/PCG/PCGHandlerHelpers.h"

// UPCGGraph exposes graph-level user parameters (the "Graph Parameters" panel in
// the PCG Graph editor) as an FInstancedPropertyBag. These handlers are the
// asset-authoring CRUD surface for that bag — add a typed parameter (with an
// optional initial value), list the parameters back, and remove one — so an
// agent can build a parameterized/overridable PCG graph without python.execute.
// FInstancedPropertyBag / FPropertyBagPropertyDesc / the EPropertyBag* enums come
// transitively from PCGGraph.h (which includes StructUtils/PropertyBag.h), so no
// version-fragile direct include of PropertyBag.h is needed across UE 5.3-5.7.

namespace
{
    // Maps a caller-facing type string to the settable scalar bag types. Only the
    // scalars this handler can round-trip through value get/set are accepted; other
    // property-bag kinds (struct/object/enum/text/...) are intentionally out of scope
    // for the first CRUD slice and rejected with INVALID_PARAM_TYPE.
    bool ParseScalarBagType(const FString& TypeStr, EPropertyBagPropertyType& Out)
    {
        const FString T = TypeStr.TrimStartAndEnd().ToLower();
        if (T == TEXT("bool"))                     { Out = EPropertyBagPropertyType::Bool;   return true; }
        if (T == TEXT("int") || T == TEXT("int32")){ Out = EPropertyBagPropertyType::Int32;  return true; }
        if (T == TEXT("int64"))                    { Out = EPropertyBagPropertyType::Int64;  return true; }
        if (T == TEXT("float"))                    { Out = EPropertyBagPropertyType::Float;  return true; }
        if (T == TEXT("double"))                   { Out = EPropertyBagPropertyType::Double; return true; }
        if (T == TEXT("name"))                     { Out = EPropertyBagPropertyType::Name;   return true; }
        if (T == TEXT("string"))                   { Out = EPropertyBagPropertyType::String; return true; }
        return false;
    }

    // Canonical lower-case label for any bag property type — used by list so a graph
    // whose parameters were authored elsewhere (e.g. an object/struct param) still
    // reports a meaningful type, even when this handler cannot set its value.
    FString BagTypeToString(EPropertyBagPropertyType Type)
    {
        switch (Type)
        {
            case EPropertyBagPropertyType::Bool:       return TEXT("bool");
            case EPropertyBagPropertyType::Byte:       return TEXT("byte");
            case EPropertyBagPropertyType::Int32:      return TEXT("int");
            case EPropertyBagPropertyType::Int64:      return TEXT("int64");
            // The unsigned bag types were added to EPropertyBagPropertyType in UE 5.4.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
            case EPropertyBagPropertyType::UInt32:     return TEXT("uint32");
            case EPropertyBagPropertyType::UInt64:     return TEXT("uint64");
#endif
            case EPropertyBagPropertyType::Float:      return TEXT("float");
            case EPropertyBagPropertyType::Double:     return TEXT("double");
            case EPropertyBagPropertyType::Name:       return TEXT("name");
            case EPropertyBagPropertyType::String:     return TEXT("string");
            case EPropertyBagPropertyType::Text:       return TEXT("text");
            case EPropertyBagPropertyType::Enum:       return TEXT("enum");
            case EPropertyBagPropertyType::Struct:     return TEXT("struct");
            case EPropertyBagPropertyType::Object:     return TEXT("object");
            case EPropertyBagPropertyType::SoftObject: return TEXT("softobject");
            case EPropertyBagPropertyType::Class:      return TEXT("class");
            case EPropertyBagPropertyType::SoftClass:  return TEXT("softclass");
            default:                                   return TEXT("unknown");
        }
    }

    // Reads the current scalar value of a bag property onto Entry["value"]. No-op for
    // container/non-scalar kinds, which list by name+type only.
    void AddScalarValueField(const FInstancedPropertyBag& Bag,
        const FPropertyBagPropertyDesc& Desc, const TSharedPtr<FJsonObject>& Entry)
    {
        const FName PName = Desc.Name;
        switch (Desc.ValueType)
        {
            case EPropertyBagPropertyType::Bool:
                if (auto R = Bag.GetValueBool(PName);   R.IsValid()) Entry->SetBoolField(TEXT("value"), R.GetValue());
                break;
            case EPropertyBagPropertyType::Int32:
                if (auto R = Bag.GetValueInt32(PName);  R.IsValid()) Entry->SetNumberField(TEXT("value"), R.GetValue());
                break;
            case EPropertyBagPropertyType::Int64:
                if (auto R = Bag.GetValueInt64(PName);  R.IsValid()) Entry->SetNumberField(TEXT("value"), static_cast<double>(R.GetValue()));
                break;
            case EPropertyBagPropertyType::Float:
                if (auto R = Bag.GetValueFloat(PName);  R.IsValid()) Entry->SetNumberField(TEXT("value"), R.GetValue());
                break;
            case EPropertyBagPropertyType::Double:
                if (auto R = Bag.GetValueDouble(PName); R.IsValid()) Entry->SetNumberField(TEXT("value"), R.GetValue());
                break;
            case EPropertyBagPropertyType::Name:
                if (auto R = Bag.GetValueName(PName);   R.IsValid()) Entry->SetStringField(TEXT("value"), R.GetValue().ToString());
                break;
            case EPropertyBagPropertyType::String:
                if (auto R = Bag.GetValueString(PName); R.IsValid()) Entry->SetStringField(TEXT("value"), R.GetValue());
                break;
            default:
                break;
        }
    }

    FString CoerceValueToString(const TSharedPtr<FJsonValue>& ValueJson);

    struct FParsedScalarValue
    {
        bool bBool = false;
        int64 Integer = 0;
        double Number = 0.0;
        FString String;
    };

    // Parse the optional value before entering MutateUserParameters. The strict JsonUtils
    // parsers consume the whole string, reject non-finite numbers, and enforce the target
    // integer range, so malformed scalar text cannot create or replace a bag property.
    bool ParseScalarValue(EPropertyBagPropertyType Type,
        const TSharedPtr<FJsonValue>& ValueJson, FParsedScalarValue& Out, FString& OutError)
    {
        OutError.Empty();
        if (!ValueJson.IsValid())
        {
            OutError = TEXT("A scalar value is required");
            return false;
        }

        switch (Type)
        {
            case EPropertyBagPropertyType::Bool:
                return TryParseStrictJsonBoolean(ValueJson, Out.bBool, OutError);

            case EPropertyBagPropertyType::Int32:
                return TryParseStrictJsonInteger(
                    ValueJson,
                    static_cast<int64>(TNumericLimits<int32>::Min()),
                    static_cast<int64>(TNumericLimits<int32>::Max()),
                    Out.Integer,
                    OutError);

            case EPropertyBagPropertyType::Int64:
                return TryParseStrictJsonInteger(
                    ValueJson,
                    TNumericLimits<int64>::Min(),
                    TNumericLimits<int64>::Max(),
                    Out.Integer,
                    OutError);

            case EPropertyBagPropertyType::Float:
                if (!TryParseStrictJsonNumber(ValueJson, Out.Number, OutError))
                {
                    return false;
                }
                if (!FMath::IsFinite(static_cast<float>(Out.Number)))
                {
                    OutError = TEXT("Value is outside the finite float range");
                    return false;
                }
                Out.Number = static_cast<float>(Out.Number);
                return true;

            case EPropertyBagPropertyType::Double:
                return TryParseStrictJsonNumber(ValueJson, Out.Number, OutError);

            case EPropertyBagPropertyType::Name:
            case EPropertyBagPropertyType::String:
                if (ValueJson->Type != EJson::String
                    && ValueJson->Type != EJson::Number
                    && ValueJson->Type != EJson::Boolean)
                {
                    OutError = TEXT("Expected a scalar JSON value");
                    return false;
                }
                Out.String = CoerceValueToString(ValueJson);
                return true;

            default:
                OutError = TEXT("Unsupported scalar property type");
                return false;
        }
    }

    // Assigns a pre-parsed scalar value onto a freshly-added bag property. The property was
    // created with a matching type immediately before this call, so a Success result is
    // expected; any other result is surfaced as SET_FAILED by the caller.
    EPropertyBagResult SetScalarValue(FInstancedPropertyBag& Bag, const FName PName,
        EPropertyBagPropertyType Type, const FParsedScalarValue& Value)
    {
        switch (Type)
        {
            case EPropertyBagPropertyType::Bool:   return Bag.SetValueBool(PName, Value.bBool);
            case EPropertyBagPropertyType::Int32:  return Bag.SetValueInt32(PName, static_cast<int32>(Value.Integer));
            case EPropertyBagPropertyType::Int64:  return Bag.SetValueInt64(PName, Value.Integer);
            case EPropertyBagPropertyType::Float:  return Bag.SetValueFloat(PName, static_cast<float>(Value.Number));
            case EPropertyBagPropertyType::Double: return Bag.SetValueDouble(PName, Value.Number);
            case EPropertyBagPropertyType::Name:   return Bag.SetValueName(PName, FName(*Value.String));
            case EPropertyBagPropertyType::String: return Bag.SetValueString(PName, Value.String);
            default:                               return EPropertyBagResult::TypeMismatch;
        }
    }

    // Normalizes a JSON value into the string form the scalar setters parse. Accepts a
    // JSON string, number, or bool so callers can send `value: 0.25` or `value: "0.25"`.
    FString CoerceValueToString(const TSharedPtr<FJsonValue>& ValueJson)
    {
        if (!ValueJson.IsValid())
        {
            return FString();
        }
        switch (ValueJson->Type)
        {
            case EJson::String:  return ValueJson->AsString();
            case EJson::Number:  return LexToString(ValueJson->AsNumber());
            case EJson::Boolean: return ValueJson->AsBool() ? TEXT("true") : TEXT("false");
            default:             return FString();
        }
    }

    // ---- Property-bag / graph-parameter API compat (UE 5.3-5.8) --------------------
    // UE 5.6 reshaped this surface: FInstancedPropertyBag::AddProperty and
    // RemovePropertyByName started returning EPropertyBagAlterationResult (both were void
    // before) and SanitizePropertyName was introduced; UPCGGraph::UpdateUserParametersStruct
    // only exists on 5.5+. The three helpers below give the handlers one vocabulary so the
    // observable behaviour — sanitized storage name, loud failure when a mutation did not
    // take — is identical on every supported engine.

    // Mirrors FInstancedPropertyBag::SanitizePropertyName: every character of the bag's
    // InvalidNameCharacters set folds to '_' and an empty name becomes "Property". Pre-5.6
    // bags store the name verbatim, so the fold has to happen here or the stored / echoed /
    // removed names would disagree with the 5.6+ behaviour the handlers document.
    FName SanitizeBagName(const FString& Name)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        return FInstancedPropertyBag::SanitizePropertyName(Name, TEXT('_'));
#else
        if (Name.IsEmpty())
        {
            return FName(TEXT("Property"));
        }
        // Same character set as UE::StructUtils::Private::Constants::InvalidNameCharacters.
        static const TCHAR InvalidNameCharacters[] = TEXT(" \"',/.:|&!?~\\\n\r\t@#(){}[]<>=;^%$`+*");
        FString Sanitized = Name;
        for (const TCHAR* Char = InvalidNameCharacters; *Char != TEXT('\0'); ++Char)
        {
            Sanitized.ReplaceCharInline(*Char, TEXT('_'));
        }
        return FName(Sanitized);
#endif
    }

    // Applies a mutation to the graph's user-parameter bag. 5.5+ routes through
    // UpdateUserParametersStruct (which propagates the change to graph instances and fires
    // OnGraphParametersChanged when the callback returns); older engines only expose the raw
    // bag, so the mutation is applied directly to it.
    bool MutateUserParameters(UPCGGraph* Graph, TFunctionRef<void(FInstancedPropertyBag&)> Fn)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        Graph->UpdateUserParametersStruct(Fn);
        return true;
#else
        FInstancedPropertyBag* Bag = Graph->GetMutableUserParametersStruct_Unsafe();
        if (!Bag)
        {
            return false;
        }
        Fn(*Bag);
        return true;
#endif
    }

    // Adds (upserts) a scalar property. Pre-5.6 AddProperty returns void and cannot report
    // failure, so the descriptor is read back out of the bag to confirm the add took.
    bool AddBagProperty(FInstancedPropertyBag& Bag, const FName PName,
        const EPropertyBagPropertyType Type, FString& OutDetail)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        const EPropertyBagAlterationResult Result = Bag.AddProperty(PName, Type);
        if (Result != EPropertyBagAlterationResult::Success)
        {
            OutDetail = FString::Printf(TEXT("alteration result %d"), static_cast<int32>(Result));
            return false;
        }
        return true;
#else
        Bag.AddProperty(PName, Type);
        if (!Bag.FindPropertyDescByName(PName))
        {
            OutDetail = TEXT("the property was not created");
            return false;
        }
        return true;
#endif
    }

    // Removes a property. Same void-vs-result split as AddBagProperty: pre-5.6 the removal
    // is confirmed by the descriptor being gone.
    bool RemoveBagProperty(FInstancedPropertyBag& Bag, const FName PName, FString& OutDetail)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        const EPropertyBagAlterationResult Result = Bag.RemovePropertyByName(PName);
        if (Result != EPropertyBagAlterationResult::Success)
        {
            OutDetail = FString::Printf(TEXT("alteration result %d"), static_cast<int32>(Result));
            return false;
        }
        return true;
#else
        Bag.RemovePropertyByName(PName);
        if (Bag.FindPropertyDescByName(PName))
        {
            OutDetail = TEXT("the property is still present after removal");
            return false;
        }
        return true;
#endif
    }
}

REGISTER_RPC_HANDLER("pcg.add_graph_parameter", "pcg",
    "Add (upsert) a graph-level user parameter to a UPCGGraph with an optional initial value. "
    "type is one of bool|int|int64|float|double|name|string.",
    RPC_PARAMS(
        RPC_PARAM_REQ("graphPath", "path", "PCG graph asset path."),
        RPC_PARAM_REQ("name", "string", "Parameter name (as shown in the Graph Parameters panel)."),
        RPC_PARAM_REQ("type", "string", "Scalar type: bool|int|int64|float|double|name|string."),
        RPC_PARAM_OPT("value", "string", "Initial value; parsed per type. Omit to default-initialize.")
    ))
{
    FString GraphPath;
    if (!Ctx.RequireString(TEXT("graphPath"), GraphPath)) return true;

    FString RawName;
    if (!Ctx.RequireString(TEXT("name"), RawName)) return true;

    FString TypeStr;
    if (!Ctx.RequireString(TEXT("type"), TypeStr)) return true;

    UPCGGraph* Graph = PinWrightPCG::LoadGraphOrError(Ctx, GraphPath);
    if (!Graph) return true;

    const FString ParamName = RawName.TrimStartAndEnd();
    if (ParamName.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_NAME, TEXT("Parameter name cannot be empty."));
        return true;
    }

    EPropertyBagPropertyType BagType;
    if (!ParseScalarBagType(TypeStr, BagType))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAM_TYPE,
            FString::Printf(TEXT("Unsupported parameter type '%s'. Supported: bool, int, int64, float, double, name, string."),
                *TypeStr));
        return true;
    }

    const TSharedPtr<FJsonValue> ValueJson = Ctx.GetJsonValueFirstOf({TEXT("value")});
    const bool bHasValue = ValueJson.IsValid() && ValueJson->Type != EJson::Null;
    FParsedScalarValue ParsedValue;
    FString ParseError;
    if (bHasValue && !ParseScalarValue(BagType, ValueJson, ParsedValue, ParseError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_VALUE,
            FString::Printf(TEXT("Invalid value for graph parameter '%s': %s"),
                *ParamName, *ParseError));
        return true;
    }

    // FInstancedPropertyBag stores a NEW property under SanitizePropertyName(name):
    // any character in InvalidNameCharacters (space, ',', '.', '/', '|', '+', '*', ...)
    // is folded to '_', and the up-front StructUtils.EnforceValidPropertyName rejection
    // defaults OFF. Canonicalize to that stored name BEFORE Add/Set — otherwise
    // SetScalarValue(raw) would miss the just-added descriptor (spuriously reporting
    // SET_FAILED for a param that WAS created), and the echoed name / list / remove
    // would all disagree with what the bag actually holds. ParamName is non-empty here
    // (checked above), so this never hits the empty -> "Property" fallback.
    const FName PName = SanitizeBagName(ParamName);

    // MutateUserParameters is the sanctioned mutation entry point: it hands us the real
    // (not a copy) user-parameter bag and, on 5.5+, propagates the change to child
    // instances + fires OnGraphParametersChanged when the callback returns. AddProperty
    // overwrites an existing property of the same name, giving idempotent upsert semantics
    // under client retry.
    bool bAdded = false;
    FString AddDetail;
    EPropertyBagResult SetResult = EPropertyBagResult::Success;
    const bool bMutated = MutateUserParameters(Graph, [&](FInstancedPropertyBag& Bag)
    {
        bAdded = AddBagProperty(Bag, PName, BagType, AddDetail);
        if (bAdded && bHasValue)
        {
            SetResult = SetScalarValue(Bag, PName, BagType, ParsedValue);
        }
    });

    if (!bMutated || !bAdded)
    {
        Ctx.SendError(ErrorCodes::ERR_ADD_FAILED,
            FString::Printf(TEXT("Failed to add graph parameter '%s' (%s)."),
                *ParamName, bMutated ? *AddDetail : TEXT("the graph exposes no user-parameter bag")));
        return true;
    }
    if (bHasValue && SetResult != EPropertyBagResult::Success)
    {
        Ctx.SendError(ErrorCodes::ERR_SET_FAILED,
            FString::Printf(TEXT("Parameter '%s' was added but its value could not be set (result %d)."),
                *ParamName, static_cast<int32>(SetResult)));
        return true;
    }

    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Result->SetStringField(TEXT("name"), PName.ToString());
    Result->SetStringField(TEXT("type"), BagTypeToString(BagType));
    Result->SetBoolField(TEXT("valueSet"), bHasValue);
    if (const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct())
    {
        if (const FPropertyBagPropertyDesc* Desc = Bag->FindPropertyDescByName(PName))
        {
            AddScalarValueField(*Bag, *Desc, Result);
        }
    }
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("pcg.list_graph_parameters", "pcg",
    "List the graph-level user parameters of a UPCGGraph, each with name, type, and "
    "(for scalar types) current value.",
    RPC_PARAMS(
        RPC_PARAM_REQ("graphPath", "path", "PCG graph asset path.")
    ))
{
    FString GraphPath;
    if (!Ctx.RequireString(TEXT("graphPath"), GraphPath)) return true;

    UPCGGraph* Graph = PinWrightPCG::LoadGraphOrError(Ctx, GraphPath);
    if (!Graph) return true;

    TArray<TSharedPtr<FJsonValue>> Params;
    if (const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct())
    {
        // GetPropertyBagStruct() is null while the bag holds no properties — an
        // empty graph legitimately lists zero parameters.
        if (const UPropertyBag* BagStruct = Bag->GetPropertyBagStruct())
        {
            for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Desc.Name.ToString());
                Entry->SetStringField(TEXT("type"), BagTypeToString(Desc.ValueType));
                AddScalarValueField(*Bag, Desc, Entry);
                Params.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Result->SetNumberField(TEXT("count"), Params.Num());
    Result->SetArrayField(TEXT("parameters"), Params);
    Ctx.SendSuccess(Result);
    return true;
}

REGISTER_RPC_HANDLER("pcg.remove_graph_parameter", "pcg",
    "Remove a graph-level user parameter from a UPCGGraph by name.",
    RPC_PARAMS(
        RPC_PARAM_REQ("graphPath", "path", "PCG graph asset path."),
        RPC_PARAM_REQ("name", "string", "Parameter name to remove.")
    ))
{
    FString GraphPath;
    if (!Ctx.RequireString(TEXT("graphPath"), GraphPath)) return true;

    FString RawName;
    if (!Ctx.RequireString(TEXT("name"), RawName)) return true;

    UPCGGraph* Graph = PinWrightPCG::LoadGraphOrError(Ctx, GraphPath);
    if (!Graph) return true;

    // Bag descriptors are always stored under SanitizePropertyName (see
    // add_graph_parameter), so the lookup key must be sanitized the same way for the
    // round-trip to close: removing by the name that was added ("My Density") must
    // resolve to the stored descriptor ("My_Density"). An empty name stays NAME_None
    // so it falls through to PARAMETER_NOT_FOUND rather than the SanitizePropertyName
    // empty -> "Property" fallback (which could delete an unrelated "Property" param).
    const FString TrimmedName = RawName.TrimStartAndEnd();
    const FName PName = TrimmedName.IsEmpty()
        ? FName()
        : SanitizeBagName(TrimmedName);

    // Fail loud on a missing parameter rather than reporting a fake success — the
    // caller passed a name that isn't there, and RemovePropertyByName would otherwise
    // return SourcePropertyNotFound with no other signal.
    const FInstancedPropertyBag* ReadBag = Graph->GetUserParametersStruct();
    if (!ReadBag || !ReadBag->FindPropertyDescByName(PName))
    {
        Ctx.SendError(ErrorCodes::ERR_PARAMETER_NOT_FOUND,
            FString::Printf(TEXT("Graph parameter '%s' does not exist on %s."),
                *RawName, *Graph->GetPathName()));
        return true;
    }

    bool bRemoved = false;
    FString RemoveDetail;
    const bool bMutated = MutateUserParameters(Graph, [&](FInstancedPropertyBag& Bag)
    {
        bRemoved = RemoveBagProperty(Bag, PName, RemoveDetail);
    });

    if (!bMutated || !bRemoved)
    {
        Ctx.SendError(ErrorCodes::ERR_REMOVE_FAILED,
            FString::Printf(TEXT("Failed to remove graph parameter '%s' (%s)."),
                *RawName, bMutated ? *RemoveDetail : TEXT("the graph exposes no user-parameter bag")));
        return true;
    }

    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("graphPath"), Graph->GetPathName());
    Result->SetStringField(TEXT("removed"), PName.ToString());
    Ctx.SendSuccess(Result);
    return true;
}

#endif // __has_include("PCGGraph.h")
