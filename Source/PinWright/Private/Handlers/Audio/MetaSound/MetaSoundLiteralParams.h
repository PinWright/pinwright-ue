// Copyright (c) 2026 Alexander Penkin. MIT License.

// The wire contract for "a MetaSound literal supplied as RPC params", shared by every verb
// that writes one: set_metasound_default, set_metasound_variable_default,
// add_metasound_variable, set_metasound_node_input_default.
//
// Kept out of MetaSoundLiteralFromTypeName.h so that header stays free of the handler layer:
// the resolver there answers "what literal does this name/path mean" and returns facts, this
// header turns params into a literal and facts into wire errors. One writer for the param
// names, the error codes and the recovery text means four verbs cannot disagree about what
// "no value" or "wrong class" means to a caller.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/Audio/MetaSound/MetaSoundLiteralFromTypeName.h"

#if MCP_HAS_METASOUND_LITERAL_HELPER

namespace PinWright::MetaSound
{
    /** Outcome of turning the optional typed value params into a literal. */
    enum class EMetaSoundLiteralParamOutcome : uint8
    {
        Ok,        // OutLiteral is populated
        NoValue,   // no value param present (and no type default was allowed / the type is unknown)
        ErrorSent  // a value param was present but unusable; the wire error is already sent
    };

    // Sends the error matching a non-Ok MakeObjectLiteralForMetaSoundType outcome. ResolvedObject
    // is the object that WAS found (WrongClass only); ExpectedClassPath is the class the data type
    // wants, empty when the registry has none — which is itself the answer: the type takes no object.
    // Never call with Result == Ok.
    inline void SendMetaSoundObjectLiteralError(
        FHandlerContext& Ctx,
        EMetaSoundObjectLiteralResult Result,
        const FString& ObjectPath,
        const FString& DataTypeName,
        const UObject* ResolvedObject,
        const FString& ExpectedClassPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectValue"), ObjectPath);
        Payload->SetStringField(TEXT("dataType"), DataTypeName);
        if (!ExpectedClassPath.IsEmpty())
        {
            Payload->SetStringField(TEXT("expectedClass"), ExpectedClassPath);
        }
        if (ResolvedObject)
        {
            Payload->SetStringField(TEXT("resolvedClass"), ResolvedObject->GetClass()->GetPathName());
            Payload->SetStringField(TEXT("resolvedObject"), ResolvedObject->GetPathName());
        }

        switch (Result)
        {
        case EMetaSoundObjectLiteralResult::PathRejected:
            Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_PATH,
                FString::Printf(TEXT("objectValue '%s' is not a usable asset path; nothing was written. Pass a /Game/... path to the asset to bind — assetPath names the MetaSound being edited, not the object."),
                    *ObjectPath),
                Payload);
            return;

        case EMetaSoundObjectLiteralResult::ObjectNotFound:
            Ctx.SendError(ErrorCodes::ERR_OBJECT_NOT_FOUND,
                FString::Printf(TEXT("objectValue '%s' resolves to no loadable object; nothing was written. Confirm the path with asset.exists / asset.list."),
                    *ObjectPath),
                Payload);
            return;

        case EMetaSoundObjectLiteralResult::WrongClass:
            Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_TYPE,
                ExpectedClassPath.IsEmpty()
                    ? FString::Printf(TEXT("MetaSound data type '%s' accepts no object literal, so objectValue '%s' cannot be bound to it; nothing was written. Use floatValue / intValue / boolValue / stringValue for this type."),
                        *DataTypeName, *ObjectPath)
                    : FString::Printf(TEXT("objectValue '%s' is a %s, but MetaSound data type '%s' requires %s; nothing was written."),
                        *ObjectPath,
                        ResolvedObject ? *ResolvedObject->GetClass()->GetPathName() : TEXT("different class"),
                        *DataTypeName,
                        *ExpectedClassPath),
                Payload);
            return;

        case EMetaSoundObjectLiteralResult::Ok:
        default:
            // Not an error path; callers must branch on Ok before reaching here.
            Ctx.SendError(ErrorCodes::ERR_INTERNAL_ERROR,
                TEXT("SendMetaSoundObjectLiteralError called for a successful object-literal resolution"),
                Payload);
            return;
        }
    }

    // Sends the §3 "that type name does not exist" rejection: names the common set, and carries
    // near matches measured from the live registry rather than a hardcoded list that would drift.
    // ParamName is the caller-facing parameter that carried the bad name (inputType / variableType).
    inline void SendMetaSoundUnknownTypeError(FHandlerContext& Ctx, const FString& ParamName, const FString& TypeName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(ParamName, TypeName);
        TArray<TSharedPtr<FJsonValue>> SuggestionsJson;
        for (const FString& Suggestion : SuggestMetaSoundDataTypeNames(TypeName))
        {
            SuggestionsJson.Add(MakeShared<FJsonValueString>(Suggestion));
        }
        Payload->SetArrayField(TEXT("suggestions"), SuggestionsJson);
        Ctx.SendError(ErrorCodes::ERR_INVALID_TYPE,
            FString::Printf(TEXT("'%s' names no registered MetaSound data type; nothing was written. Common names: Float, Int, Bool, String, Audio, Trigger, Time, WaveAsset. Any registered data type is accepted — see the suggestions field."),
                *TypeName),
            Payload);
    }

    // Sends the error matching a non-Ok MakeArrayLiteralForMetaSoundType outcome. Never call with
    // Result == Ok. The payload names the failing ENTRY, not just the call, because an array
    // rejection a caller cannot locate is one they have to bisect by hand.
    inline void SendMetaSoundArrayLiteralError(
        FHandlerContext& Ctx,
        const FMetaSoundArrayLiteralOutcome& Outcome,
        const FString& DataTypeName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("dataType"), DataTypeName);
        if (!Outcome.ElementTypeName.IsEmpty())
        {
            Payload->SetStringField(TEXT("elementType"), Outcome.ElementTypeName);
        }
        if (Outcome.FailedIndex != INDEX_NONE)
        {
            Payload->SetNumberField(TEXT("entryIndex"), Outcome.FailedIndex);
            Payload->SetStringField(TEXT("entry"), Outcome.FailedEntryText);
        }
        if (!Outcome.ExpectedEntryKind.IsEmpty())
        {
            Payload->SetStringField(TEXT("expectedEntryKind"), Outcome.ExpectedEntryKind);
        }
        if (!Outcome.ExpectedClassPath.IsEmpty())
        {
            Payload->SetStringField(TEXT("expectedClass"), Outcome.ExpectedClassPath);
        }
        if (Outcome.ResolvedObject)
        {
            Payload->SetStringField(TEXT("resolvedClass"), Outcome.ResolvedObject->GetClass()->GetPathName());
            Payload->SetStringField(TEXT("resolvedObject"), Outcome.ResolvedObject->GetPathName());
        }

        switch (Outcome.Result)
        {
        case EMetaSoundArrayLiteralResult::NotAnArrayType:
            Ctx.SendError(ErrorCodes::ERR_INVALID_TYPE,
                FString::Printf(TEXT("MetaSound data type '%s' is not an array type, so arrayValue cannot be bound to it; nothing was written. Use floatValue / intValue / boolValue / stringValue / objectValue for a scalar type."),
                    *DataTypeName),
                Payload);
            return;

        case EMetaSoundArrayLiteralResult::UnsupportedArrayShape:
            Ctx.SendError(ErrorCodes::ERR_INVALID_TYPE,
                FString::Printf(TEXT("MetaSound array data type '%s' declares no literal shape that can be built from JSON; nothing was written. Check the type name against audio.authoring.describe_metasound."),
                    *DataTypeName),
                Payload);
            return;

        case EMetaSoundArrayLiteralResult::ElementTypeMismatch:
            Ctx.SendError(ErrorCodes::ERR_INVALID_VALUE,
                FString::Printf(TEXT("arrayValue entry %d must be a %s for a '%s' array, but it is '%s'; nothing was written. Every entry carries one %s."),
                    Outcome.FailedIndex, *Outcome.ExpectedEntryKind, *DataTypeName,
                    *Outcome.FailedEntryText, *Outcome.ExpectedEntryKind),
                Payload);
            return;

        case EMetaSoundArrayLiteralResult::ElementNotFound:
            Ctx.SendError(ErrorCodes::ERR_OBJECT_NOT_FOUND,
                FString::Printf(TEXT("arrayValue entry %d ('%s') resolves to no loadable object; nothing was written. Confirm the path with asset.exists / asset.list."),
                    Outcome.FailedIndex, *Outcome.FailedEntryText),
                Payload);
            return;

        case EMetaSoundArrayLiteralResult::ElementWrongClass:
            Ctx.SendError(ErrorCodes::ERR_INVALID_ASSET_TYPE,
                Outcome.ExpectedClassPath.IsEmpty()
                    ? FString::Printf(TEXT("arrayValue entry %d ('%s') cannot be bound: the array's element data type '%s' accepts no object literal; nothing was written."),
                        Outcome.FailedIndex, *Outcome.FailedEntryText, *Outcome.ElementTypeName)
                    : FString::Printf(TEXT("arrayValue entry %d ('%s') is a %s, but the array's element data type '%s' requires %s; nothing was written."),
                        Outcome.FailedIndex, *Outcome.FailedEntryText,
                        Outcome.ResolvedObject ? *Outcome.ResolvedObject->GetClass()->GetPathName() : TEXT("different class"),
                        *Outcome.ElementTypeName, *Outcome.ExpectedClassPath),
                Payload);
            return;

        case EMetaSoundArrayLiteralResult::Ok:
        default:
            Ctx.SendError(ErrorCodes::ERR_INTERNAL_ERROR,
                TEXT("SendMetaSoundArrayLiteralError called for a successful array-literal build"),
                Payload);
            return;
        }
    }

    // Sends the "this target takes a list, not a scalar" rejection. Reached only when the target
    // is array-typed and the caller supplied a scalar value param instead of arrayValue — the
    // exact case that used to be answered with "Use floatValue / intValue / boolValue /
    // stringValue for this type", advice no scalar param can satisfy for an array
    // (F-metasound-array-inputs-cannot-be-populated).
    inline void SendMetaSoundArrayValueRequiredError(FHandlerContext& Ctx,
        const FString& DataTypeName, const FString& ElementTypeName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("dataType"), DataTypeName);
        if (!ElementTypeName.IsEmpty())
        {
            Payload->SetStringField(TEXT("elementType"), ElementTypeName);
        }
        Ctx.SendError(ErrorCodes::ERR_INVALID_TYPE,
            FString::Printf(TEXT("MetaSound data type '%s' is an array type, which no scalar value param can express; nothing was written. Pass arrayValue as a JSON array whose entries are %s values."),
                *DataTypeName,
                ElementTypeName.IsEmpty() ? TEXT("element-typed") : *ElementTypeName),
            Payload);
    }

    // Sends the §3 "you supplied no value" rejection. There is deliberately no default default:
    // set_metasound_default used to fall back to a float zero, which wrote a value the caller
    // never asked for into whatever type the target actually was and reported success.
    inline void SendMetaSoundMissingValueError(FHandlerContext& Ctx, const FString& TargetDescription,
        const FString& DataTypeName)
    {
        // An array-typed target must be told about arrayValue and NOT about the scalar params:
        // naming five params none of which can carry a list is the advice that made array state
        // look unwritable (F-metasound-array-inputs-cannot-be-populated).
        const bool bIsArrayTarget = IsMetaSoundArrayDataType(DataTypeName);
        Ctx.SendError(ErrorCodes::ERR_MISSING_VALUE,
            FString::Printf(TEXT("No value supplied for %s (type '%s'); nothing was written. %s"),
                *TargetDescription, *DataTypeName,
                bIsArrayTarget
                    ? TEXT("This target is array-typed: provide arrayValue as a JSON array.")
                    : TEXT("Provide exactly one of: floatValue, intValue, boolValue, stringValue, objectValue.")));
    }

    // Build a FMetasoundFrontendLiteral from the optional typed value params
    // (arrayValue, floatValue / intValue / boolValue / stringValue / objectValue, alias
    // objectPath).
    //
    // DataTypeName is the target vertex/variable's declared MetaSound data type; it drives
    // objectValue class validation, decides whether the target takes arrayValue or a scalar, and,
    // when bAllowTypeDefault is true, supplies the default literal for a call carrying no value at
    // all. Only create-time verbs pass true — for a *set* verb an omitted value is a caller
    // mistake, not a request to reset.
    //
    // ARRAY AND SCALAR ARE EXCLUSIVE, decided by the TARGET's declared type rather than by which
    // param arrived: an array-typed target takes arrayValue and refuses every scalar param, and a
    // scalar-typed target refuses arrayValue. Both refusals name the parameter that would work.
    // Before this, an array type had no reachable value param at all — objectValue was rejected by
    // the registry (an array entry sets only bIsProxyArrayParsable) with advice to use the scalar
    // params, none of which can carry a list, so the whole Array.* node family was unreachable
    // (F-metasound-array-inputs-cannot-be-populated).
    //
    // Within the scalar family the first matching param wins in the order above, so a call
    // carrying two value params is not rejected; it is resolved deterministically.
    inline EMetaSoundLiteralParamOutcome BuildMetaSoundLiteralFromParams(
        FHandlerContext& Ctx,
        const FString& DataTypeName,
        bool bAllowTypeDefault,
        FMetasoundFrontendLiteral& OutLiteral)
    {
        TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
        if (!Payload.IsValid())
        {
            return EMetaSoundLiteralParamOutcome::NoValue;
        }

        static const TCHAR* const ScalarValueKeys[] = {
            TEXT("floatValue"), TEXT("intValue"), TEXT("boolValue"),
            TEXT("stringValue"), TEXT("objectValue"), TEXT("objectPath")
        };

        if (IsMetaSoundArrayDataType(DataTypeName))
        {
            if (const TArray<TSharedPtr<FJsonValue>>* Entries = Ctx.GetArray(TEXT("arrayValue")))
            {
                FMetaSoundArrayLiteralOutcome ArrayOutcome;
                if (MakeArrayLiteralForMetaSoundType(*Entries, DataTypeName, OutLiteral, ArrayOutcome))
                {
                    return EMetaSoundLiteralParamOutcome::Ok;
                }
                SendMetaSoundArrayLiteralError(Ctx, ArrayOutcome, DataTypeName);
                return EMetaSoundLiteralParamOutcome::ErrorSent;
            }

            // arrayValue present but not a JSON array, or a scalar param supplied instead:
            // both are the same caller mistake and both get told which param does work.
            if (Payload->HasField(TEXT("arrayValue")))
            {
                FMetaSoundArrayLiteralOutcome ShapeOutcome;
                ShapeOutcome.Result = EMetaSoundArrayLiteralResult::ElementTypeMismatch;
                ShapeOutcome.FailedIndex = 0;
                ShapeOutcome.ElementTypeName = GetMetaSoundArrayElementTypeName(DataTypeName);
                ShapeOutcome.ExpectedEntryKind = TEXT("JSON array");
                ShapeOutcome.FailedEntryText = TEXT("a non-array value");
                SendMetaSoundArrayLiteralError(Ctx, ShapeOutcome, DataTypeName);
                return EMetaSoundLiteralParamOutcome::ErrorSent;
            }
            for (const TCHAR* ScalarKey : ScalarValueKeys)
            {
                if (Payload->HasField(ScalarKey))
                {
                    SendMetaSoundArrayValueRequiredError(Ctx, DataTypeName,
                        GetMetaSoundArrayElementTypeName(DataTypeName));
                    return EMetaSoundLiteralParamOutcome::ErrorSent;
                }
            }

            if (bAllowTypeDefault && MakeDefaultLiteralForMetaSoundType(DataTypeName, OutLiteral))
            {
                return EMetaSoundLiteralParamOutcome::Ok;
            }
            return EMetaSoundLiteralParamOutcome::NoValue;
        }

        // arrayValue on a scalar-typed target: refuse rather than ignore, so a caller who put the
        // list on the wrong verb is told, not silently given the scalar branch below.
        if (Payload->HasField(TEXT("arrayValue")))
        {
            FMetaSoundArrayLiteralOutcome NotArrayOutcome;
            NotArrayOutcome.Result = EMetaSoundArrayLiteralResult::NotAnArrayType;
            SendMetaSoundArrayLiteralError(Ctx, NotArrayOutcome, DataTypeName);
            return EMetaSoundLiteralParamOutcome::ErrorSent;
        }

        if (Payload->HasField(TEXT("floatValue")))
        {
            OutLiteral.Set(static_cast<float>(Ctx.GetNumber(TEXT("floatValue"), 0.0)));
            return EMetaSoundLiteralParamOutcome::Ok;
        }
        if (Payload->HasField(TEXT("intValue")))
        {
            OutLiteral.Set(Ctx.GetInt(TEXT("intValue"), 0));
            return EMetaSoundLiteralParamOutcome::Ok;
        }
        if (Payload->HasField(TEXT("boolValue")))
        {
            OutLiteral.Set(Ctx.GetBool(TEXT("boolValue"), false));
            return EMetaSoundLiteralParamOutcome::Ok;
        }
        if (Payload->HasField(TEXT("stringValue")))
        {
            OutLiteral.Set(Ctx.GetString(TEXT("stringValue")));
            return EMetaSoundLiteralParamOutcome::Ok;
        }
        if (Payload->HasField(TEXT("objectValue")) || Payload->HasField(TEXT("objectPath")))
        {
            const FString ObjectPath = Ctx.GetStringFirstOf({ TEXT("objectValue"), TEXT("objectPath") });
            UObject* ResolvedObject = nullptr;
            FString ExpectedClassPath;
            const EMetaSoundObjectLiteralResult ObjectResult = MakeObjectLiteralForMetaSoundType(
                ObjectPath, DataTypeName, OutLiteral, ResolvedObject, ExpectedClassPath);
            if (ObjectResult == EMetaSoundObjectLiteralResult::Ok)
            {
                return EMetaSoundLiteralParamOutcome::Ok;
            }
            SendMetaSoundObjectLiteralError(Ctx, ObjectResult, ObjectPath, DataTypeName,
                ResolvedObject, ExpectedClassPath);
            return EMetaSoundLiteralParamOutcome::ErrorSent;
        }

        if (bAllowTypeDefault && MakeDefaultLiteralForMetaSoundType(DataTypeName, OutLiteral))
        {
            return EMetaSoundLiteralParamOutcome::Ok;
        }
        return EMetaSoundLiteralParamOutcome::NoValue;
    }
}

#endif // MCP_HAS_METASOUND_LITERAL_HELPER
