// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Utils/JsonUtils.h"

// THE RUNTIME READER OF FParamSpec::Type.
//
// Until this header existed, `Type` (ParamSpec.h:19) was declared on every parameter in the
// registry, rendered into every wiki page as a contract, and read by exactly two pieces of
// production code, both of which only PRINT it: Catalog/MarkdownHelpers.cpp's param bullet and
// the "(type: %s)" substring of MISSING_REQUIRED_PARAM. The dispatcher validated parameter NAMES
// and never their JSON shape, so a caller who spelled a key right and shaped it wrong got a
// success payload built on a coerced or defaulted value - board B-param-type-never-validated.
//
// WHAT IS REFUSED, AND WHY IT IS NOT A JSON-SCHEMA GATE. The direction of the mismatch decides.
// Refusing every deviation would break every working caller that sends `"limit": "100"` or
// `"force": "true"` - routine LLM-client output, and LOSSLESS: the accessor recovers the intended
// value exactly. What is refused is the LOSSY set, where UE's FJsonValue accessors silently
// return the zero value and log to the editor log the caller never sees
// (Json/Private/Dom/JsonValue.cpp - AsString/AsNumber/AsBool call ErrorMessage() then return ""/0/false):
//
//   declared          accepted                                   refused (lossy)
//   string            string, number, bool                       array, object
//   number            number, bool, numeric string               non-numeric string, array, object
//   integer           finite in-range integral number/string     fractions, bool, array, object
//   boolean/bool      bool, number, boolean-spelled string       other strings, array, object
//   object            object, array (measured, see below)        every scalar
//   array             array                                      everything else
//   any               anything (except null, see below)          -
//
// `object` ALSO ACCEPTS AN ARRAY, and that row is a measurement of this registry rather than a
// reading of JSON. 205 declarations across 36 handler files spell a vector-shaped slot
// (`location`, `rotation`, `extent`, `scale`, `center`, `offset`, `translation`, `point`, `force`,
// `deltaWorld`, `additionalOffset`, `min`, `max`) as `object`, while the path that reads them -
// FHandlerContext::GetVector / GetRotator -> ExtractVectorField -> ReadVectorFieldImpl
// (Utils/JsonUtils.cpp:16-47, whose own comment reads "Supports object form {x, y, z} or array
// form [x, y, z]") - accepts BOTH shapes at 111 confirmed call sites. In this tree `object` is
// therefore not a promise of "JSON object only"; it is a systematically under-specified spelling of
// "structured value", and refusing `[0, 0, 100]` for a location would break a working, documented
// wire convention on ~200 slots with nothing in the suite to catch it. No confirmed instance on
// board B-param-type-never-validated needs the array-where-object case refused: all fourteen are
// null, non-numeric-string->number, array->string, scalar->array, or string->boolean. The residual
// this leaves - an `object` slot that genuinely means object-only still silently drops an array
// through GetObject()'s nullptr - is recorded on that ticket, and the honest fix is to correct
// those 205 declarations to `object|array` verb by verb against their read path, not to tighten
// this row first and find out which callers broke.
//
// A numeric string is tested with FString::IsNumeric, which is not an arbitrary choice: it is the
// exact predicate TJsonValueString::TryGetNumber gates on (Json/Public/Dom/JsonValue.h:451-462,
// `if (Value.IsNumeric()) { OutDouble = Atod(...) }`). Using the accessor's own predicate is what
// makes "the gate accepted it" and "the accessor recovers it" one fact instead of two that can
// drift. Two near-misses, both rejected on purpose: LexTryParseString is built on Atod alone and
// answers TRUE for "2s" (Atod stops at the 's' and returns 2.0) - the precise value that collapses
// sequencer.set_sub_section_range's range to (0,0); and exponent notation ("1e5") is NOT numeric by
// this predicate, so accepting it would be the gate promising a recovery the accessor cannot make.
//
// NULL IS ALWAYS REFUSED, for every declared type including `any`. UE 5.8's FJsonObject::HasField
// (Json/Private/Dom/JsonObject.cpp) tests only that the shared pointer is valid and returns TRUE
// for an EJson::Null value, and FJsonValueNull overrides no TryGet*, so `{"preserveProperties": null}`
// cleared the dispatcher's required-param gate and landed in the handler as `false`. That is the
// most plausible wrong value on the whole wire: a client whose serializer emits nulls for unset
// optionals turns "I did not set this" into "I explicitly set this to the destructive value" on
// every optional parameter in the registry. Refusing is the only honest answer - "absent" is
// spelled by omitting the key, and a caller that means null on an `any` slot has no way to
// distinguish itself from that serializer, so neither can this gate.
//
// GRAMMAR. Mirrors IsSupportedTypeExpr in Tests/Infra/TestContractConsistency.cpp (the
// registry-wide test that has always defended this vocabulary): a `|`-separated union over the
// eleven atomic tokens {string, number, integer, boolean, bool, object, array, any,
// path, classref, filepath}, case- and whitespace-insensitive. `bool` normalizes to `boolean`;
// `integer` remains distinct and delegates to Utils/JsonUtils.h's strict int32 parser so a
// fractional or out-of-range value cannot be accepted by the wire gate and then truncated.
//
// ===========================================================================================
// THE THREE PATH-SHAPED TOKENS, AND THE ONE RULE THEY EXIST TO CARRY
// ===========================================================================================
//
// WHAT IS LETHAL. FPackageName / CreatePackage logs at **Fatal** when a package name contains `//`
// (CoreUObject/Private/UObject/UObjectGlobals.cpp:1094-1096 on UE 5.8). Fatal is not compiled out
// in any configuration: it ends the PROCESS and every unsaved package in it, and no `if (!Result)`
// after the call is ever reached. CreatePackage is not the only door and in most files not even the
// first one - StaticLoadObjectInternal calls ResolveName2(..., Create=true) (:1427), which calls
// CreatePackage on the partial name (:1310) - so ANY load on unvalidated caller text is the same
// editor kill. FindObject is safe (Create=false).
//
// IT IS LETHAL WITHOUT A LEADING SLASH AND WITHOUT A DOT. ResolveName2 returns immediately (:1241)
// when there is no delimiter, and StaticLoadObjectInternal (:1474-1482) then re-enters itself with
// `InName + "." + GetShortName(InName)` - the second pass has the dot. `LoadObject(nullptr,
// TEXT("A//B"))` is an editor kill.
//
// WHY THE RULE LIVES HERE AND NOT AT ~385 CALL SITES. Guarding the loads would be wrong twice: it
// would not prevent the 386th, and it would add another spelling to a validation layer that already
// has ten overlapping helpers. `Type` is declared on 5,418 params across 1,188 verbs and is already
// read at ONE choke point per wire name, with alias resolution handled. Declaring what an input IS
// closes the class for every future verb for free.
//
//   declared    accepted shapes                       extra rule
//   path        exactly what `string` accepts         a string containing `//` is REFUSED
//   classref    exactly what `string` accepts         a string containing `//` is REFUSED
//   filepath    exactly what `string` accepts         NONE - see below
//
// THE SHAPE ROW IS "EXACTLY WHAT string ACCEPTS" ON PURPOSE. Retyping ~780 existing `string`
// declarations must not change one shape verdict, or the sweep breaks working callers on a
// dimension nobody was reviewing. The three tokens differ from `string` only in the rule column.
//
// `Contains("//")` IS THE WHOLE RULE, AND NOTHING WIDER IS CORRECT. FPackageName::IsValidLongPackageName
// refuses a leading-slash-less short name AND refuses `.` (it is in INVALID_LONGPACKAGE_CHARACTERS,
// NameTypes.h:197). Using it on a class reference would refuse `PointLight`, `/Script/UMG.UserWidget`
// and `/Game/BP/BP_X.BP_X_C` - roughly half the shapes ClassUtils::ResolveUClass documents, across
// ~45 verbs. `classref` exists precisely so a class slot gets the lethal rule and nothing else.
//
// `filepath` CARRIES NO `//` RULE, AND THAT IS THE POINT OF HAVING IT. A UNC path normalises to
// `//server/share`, so typing a disk path `path` starts refusing valid input. `filePath`,
// `outputPath`, `sourcePath` and `destinationPath` are disk paths and belong here - which one a
// parameter is, is a per-parameter judgement, not a find-replace.
//
// AN ARRAY OF PATHS COMPOSES AS A UNION. `path|array` accepts a single path string or an array of
// them, and the rule reaches the STRING ELEMENTS of the array (naming the index in the refusal).
// That is what makes the array-of-paths slots (`assetPaths`) expressible without a twelfth token.
// The rule reads only string values: a number cannot contain `//`.
//
// WHAT IT STILL DOES NOT SEE. A path nested one level down - inside an object parameter, or
// inside an array ELEMENT object - is not covered, and cannot be until a nested key can carry
// a type of its own: FParamSpec::NestedKeys is an untyped allow-list, and several real cases
// (material `texture: {ParamName: AssetPath}`) put the path in the map VALUE rather than the
// key, where a key-shaped rule could not reach it either. Confirmed reachers:
// foliageTypes[].meshPath, nodes[].texturePath, stems[].assetPath, captures[].attribute.
// Nor can this gate see a class reference inside IR source text, which is a substring of a `text`
// param that must stay typed `string`.
//
// BOTH ARE CLOSED AT THE POINT OF USE, by PinWrightGuardedLoad::LoadObjectChecked
// (Utils/GuardedLoad.h) - a guard keyed on where the load happens rather than on the input's
// shape, so no input shape can bypass it. That is defence in depth BELOW this gate and never a
// reason to retype a top-level path slot back to `string`: this refuses the whole request before
// any work happens, with the offending value quoted; the load guard refuses one load, deep inside
// a verb that may already have created something.
//
// FAIL-OPEN ON AN UNPARSEABLE DECLARATION. A Type string that is empty, or that names a token
// outside the grammar, accepts every value. The gate must never turn a MIS-DECLARED parameter
// into a live rejection; TestContractConsistency's ParamTypes.ValidTypeNames is what holds the
// declarations to the grammar, and it fails loudly and separately when one drifts.

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

namespace PinWrightParamTypes
{
    enum class EDeclaredTypeVerdict : uint8
    {
        Accepted,
        // EJson::Null. Reported separately from Mismatch so the refusal message can say
        // "omit the parameter" rather than "send a different shape".
        ReceivedNull,
        Mismatch,
    };

    // Wire-facing name of a JSON value's type, for the refusal message. Deliberately the JSON
    // vocabulary the caller sent, not UE's EJson spelling.
    inline FString PinWrightDescribeJsonValueType(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("null");
        }
        switch (Value->Type)
        {
            case EJson::None:    return TEXT("null");
            case EJson::Null:    return TEXT("null");
            case EJson::String:  return TEXT("string");
            case EJson::Number:  return TEXT("number");
            case EJson::Boolean: return TEXT("boolean");
            case EJson::Array:   return TEXT("array");
            case EJson::Object:  return TEXT("object");
            default:             return TEXT("unknown");
        }
    }

    // A string the number accessor will actually recover. This is FString::IsNumeric and nothing
    // else on purpose: it is the exact predicate TJsonValueString::TryGetNumber gates on, so the
    // gate cannot promise a recovery the accessor then declines. Non-empty, optional leading sign,
    // digits with at most one '.', NO exponent and NO surrounding whitespace. Rejects "", " 42 ",
    // "1e5", "2s", "50%", "100cm", "1,000", "auto" - every spelling in the ticket's coercion matrix
    // that reaches a handler as 0.
    inline bool PinWrightIsStrictNumericLiteral(const FString& Raw)
    {
        return Raw.IsNumeric();
    }

    // A string FCString::ToBool round-trips to the value the caller plainly meant. ToBool answers
    // true for "true"/"yes"/"on"/a nonzero integer and FALSE for everything else, so "y",
    // "enabled", "always" and "default" all land as false with no signal - those are refused here.
    // Deliberately NOT trimmed, because FToBoolHelper::FromCStringWide (Core/Private/Misc/CString.cpp:123)
    // does not trim either: it Stricmps the WHOLE string against True/Yes/On/False/No/Off and falls
    // through to Atoi for everything else, so " true " is Atoi(" true ") == 0 == false. Accepting a
    // padded spelling here would be the gate promising a value the accessor does not produce.
    inline bool PinWrightIsBooleanLiteral(const FString& Raw)
    {
        return Raw.Equals(TEXT("true"), ESearchCase::IgnoreCase)
            || Raw.Equals(TEXT("false"), ESearchCase::IgnoreCase)
            || Raw.Equals(TEXT("yes"), ESearchCase::IgnoreCase)
            || Raw.Equals(TEXT("no"), ESearchCase::IgnoreCase)
            || Raw.Equals(TEXT("on"), ESearchCase::IgnoreCase)
            || Raw.Equals(TEXT("off"), ESearchCase::IgnoreCase)
            || PinWrightIsStrictNumericLiteral(Raw);
    }

    // True when Atom is one of the eleven grammar tokens.
    inline bool PinWrightIsKnownTypeAtom(const FString& Atom)
    {
        return Atom == TEXT("string") || Atom == TEXT("number") || Atom == TEXT("integer")
            || Atom == TEXT("boolean") || Atom == TEXT("bool") || Atom == TEXT("object")
            || Atom == TEXT("array") || Atom == TEXT("any")
            || Atom == TEXT("path") || Atom == TEXT("classref") || Atom == TEXT("filepath");
    }

    // True for the two tokens that carry the doubled-slash refusal. `filepath` is deliberately NOT
    // one of them: a UNC path normalises to `//server/share`.
    inline bool PinWrightAtomCarriesPathSeparatorRule(const FString& Atom)
    {
        return Atom == TEXT("path") || Atom == TEXT("classref");
    }

    // Does this declared type expression put its slot under the doubled-slash refusal? True when
    // ANY member of the union is `path` or `classref`, so `path|array` (an array-of-paths slot)
    // carries the rule as well as a bare `path`.
    //
    // Unlike the shape check, an unrecognized member does NOT disable the rule. The shape check
    // fails open because half-enforcing a union would refuse shapes the unreadable half may have
    // named; there is no such trade here, because the only thing this rule refuses is a value that
    // ends the editor process. A typo elsewhere in the union must not buy a `//` a way through.
    inline bool PinWrightTypeExprCarriesPathSeparatorRule(const FString& TypeExpr)
    {
        TArray<FString> Atoms;
        TypeExpr.ParseIntoArray(Atoms, TEXT("|"), true);
        for (FString& Atom : Atoms)
        {
            if (PinWrightAtomCarriesPathSeparatorRule(Atom.TrimStartAndEnd().ToLower()))
            {
                return true;
            }
        }
        return false;
    }

    // The single lethal property, spelled once. Every shape a path slot can hold - a bare short
    // name, a package path, an object path, a subobject path, a `_C` path, a plugin mount - is
    // answered correctly by this and by nothing wider (see the header comment).
    inline bool PinWrightPathValueReachesCreatePackageFatal(const FString& Raw)
    {
        return Raw.Contains(TEXT("//"));
    }

    // The refusal text for one offending value. `Label` is the caller-facing spelling of the slot
    // ("assetPath", or "assetPaths[2]" for an array element), so the message names the slot the way
    // the caller wrote it and QUOTES what they sent - other tests assert on the quoted value.
    inline FString PinWrightMakePathSeparatorFault(const FString& Label, const FString& Raw)
    {
        return FString::Printf(
            TEXT("'%s' contains a doubled slash: \"%s\". A path may not contain '//' - it reaches ")
            TEXT("CreatePackage, which logs Fatal and ends the editor process rather than ")
            TEXT("returning an error. Send the path with single separators."),
            *Label, *Raw);
    }

    // Collect the doubled-slash faults of ONE value that sits in a path-ruled slot: the string
    // itself, or the string elements of an array (indexed in the label). Non-string values are not
    // this rule's business - a number cannot contain '//', and an object one level down has no
    // per-key type to check against yet (see the header comment).
    inline void PinWrightCollectPathSeparatorFaultsOfValue(const FString& Label,
                                                           const TSharedPtr<FJsonValue>& Value,
                                                           TArray<FString>& OutFaults)
    {
        if (!Value.IsValid())
        {
            return;
        }

        if (Value->Type == EJson::String)
        {
            FString Raw;
            if (Value->TryGetString(Raw) && PinWrightPathValueReachesCreatePackageFatal(Raw))
            {
                OutFaults.Add(PinWrightMakePathSeparatorFault(Label, Raw));
            }
            return;
        }

        const TArray<TSharedPtr<FJsonValue>>* AsArray = nullptr;
        if (Value->TryGetArray(AsArray) && AsArray)
        {
            for (int32 Index = 0; Index < AsArray->Num(); ++Index)
            {
                const TSharedPtr<FJsonValue>& Element = (*AsArray)[Index];
                if (!Element.IsValid() || Element->Type != EJson::String)
                {
                    continue;
                }
                FString Raw;
                if (Element->TryGetString(Raw) && PinWrightPathValueReachesCreatePackageFatal(Raw))
                {
                    OutFaults.Add(PinWrightMakePathSeparatorFault(
                        FString::Printf(TEXT("%s[%d]"), *Label, Index), Raw));
                }
            }
        }
    }

    // The whole top-level path rule for one (wire name, declared type expression, received value).
    inline void PinWrightCollectPathSeparatorFaults(const FString& WireName,
                                                    const FString& TypeExpr,
                                                    const TSharedPtr<FJsonValue>& Value,
                                                    TArray<FString>& OutFaults)
    {
        if (!PinWrightTypeExprCarriesPathSeparatorRule(TypeExpr))
        {
            return;
        }
        PinWrightCollectPathSeparatorFaultsOfValue(WireName, Value, OutFaults);
    }

    // Does one normalized atomic token accept this non-null value?
    inline bool PinWrightAtomAcceptsValue(const FString& Atom, const TSharedPtr<FJsonValue>& Value)
    {
        const EJson Actual = Value->Type;

        if (Atom == TEXT("any"))
        {
            return true;
        }
        // The three path-shaped tokens accept exactly what `string` accepts, and differ from it
        // only in the doubled-slash rule applied by the pass after this one. Retyping an existing
        // `string` declaration must not change one shape verdict.
        if (Atom == TEXT("string") || Atom == TEXT("path") || Atom == TEXT("classref")
            || Atom == TEXT("filepath"))
        {
            return Actual == EJson::String || Actual == EJson::Number || Actual == EJson::Boolean;
        }
        if (Atom == TEXT("number"))
        {
            if (Actual == EJson::Number || Actual == EJson::Boolean)
            {
                return true;
            }
            FString AsText;
            return Actual == EJson::String && Value->TryGetString(AsText)
                && PinWrightIsStrictNumericLiteral(AsText);
        }
        if (Atom == TEXT("integer"))
        {
            int64 ParsedValue = 0;
            FString ParseError;
            return TryParseStrictJsonInteger(
                Value,
                static_cast<int64>(TNumericLimits<int32>::Min()),
                static_cast<int64>(TNumericLimits<int32>::Max()),
                ParsedValue,
                ParseError);
        }
        if (Atom == TEXT("boolean") || Atom == TEXT("bool"))
        {
            if (Actual == EJson::Boolean || Actual == EJson::Number)
            {
                return true;
            }
            FString AsText;
            return Actual == EJson::String && Value->TryGetString(AsText)
                && PinWrightIsBooleanLiteral(AsText);
        }
        if (Atom == TEXT("object"))
        {
            // Array included by measurement, not by JSON semantics - see the header comment.
            return Actual == EJson::Object || Actual == EJson::Array;
        }
        if (Atom == TEXT("array"))
        {
            return Actual == EJson::Array;
        }

        // Outside the grammar: fail open (see the header comment).
        return true;
    }

    // The whole gate for one (declared type expression, received value) pair.
    inline EDeclaredTypeVerdict PinWrightCheckDeclaredType(const FString& TypeExpr,
                                                           const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid() || Value->Type == EJson::Null || Value->Type == EJson::None)
        {
            return EDeclaredTypeVerdict::ReceivedNull;
        }

        TArray<FString> Atoms;
        TypeExpr.ParseIntoArray(Atoms, TEXT("|"), true);
        if (Atoms.Num() == 0)
        {
            return EDeclaredTypeVerdict::Accepted;
        }

        // One unrecognized member makes the whole expression unusable, not just that member:
        // a union is a promise about the WHOLE slot, and half-enforcing it would refuse shapes
        // the unreadable half may well have named.
        for (FString& Atom : Atoms)
        {
            Atom = Atom.TrimStartAndEnd().ToLower();
            if (!PinWrightIsKnownTypeAtom(Atom))
            {
                return EDeclaredTypeVerdict::Accepted;
            }
        }

        for (const FString& Atom : Atoms)
        {
            if (PinWrightAtomAcceptsValue(Atom, Value))
            {
                return EDeclaredTypeVerdict::Accepted;
            }
        }

        return EDeclaredTypeVerdict::Mismatch;
    }
}
