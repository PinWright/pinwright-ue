// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// THE RUNTIME READER OF FParamSpec::NestedKeys - the fourth pass of
// FRpcDispatcher::ValidateHandlerParams, and the first thing in this plugin that validates a key
// one level below the top of the payload.
//
// WHAT WAS UNCHECKED. The first three passes (missing-required, unknown-name, declared-type) all
// iterate `for (const auto& Field : Params->Values)` - strictly one level. A key nested inside an
// object- or array-typed parameter was therefore validated by NOTHING: not by the dispatcher, and
// not by PinWright.infra.declared_params.HandlersOnlyReadDeclaredParams, whose exclusion of nested
// reads is deliberate and correct (a nested key is not a top-level wire name, so diffing it against
// RPC_PARAMS reports every one of them as undeclared - the failure that produced 15 false
// level.structure.* pairs). 317 of 1,220 verbs declare an object/array parameter and 50 of them
// read 216 nested keys in the handler body; a caller who sent a plausible nested key the verb did
// not read got `success` and a silently discarded input. Board
// B-declared-param-guard-blind-to-nested-keys, option C.
//
// WHY A DECLARED SCHEMA HAD TO COME FIRST. The nested schema lived in the parameter's DESCRIPTION -
// prose, shaped like JSON, read by humans and by the wiki. That is what
// PinWright.infra.declared_params.NestedKeysMatchTheirParameterDescriptions diffs against, and it is
// the right source for a TEST that can afford to under-match. It is the wrong source for a GATE: a
// refusal built by parsing prose refuses real callers whenever the prose is loose, and every
// description in the registry was written with no gate reading it. FParamSpec::NestedKeys is that
// schema made machine-readable, declared beside the parameter it belongs to.
//
// OPT-IN, AND THE DEFAULT IS THE OLD BEHAVIOUR. An empty NestedKeys means "this parameter's nested
// surface is not declared yet", and nothing about that parameter is refused. Closing an object is a
// COMPATIBILITY BREAK - a caller sending a stray nested key gets `success` today and a refusal
// afterwards - so it lands one parameter at a time, with the description updated in the same
// commit, never as a blanket sweep across the 317. A verb that has not adopted must be, and is,
// bit-for-bit unaffected by this pass.
//
// ONE LEVEL DOWN, NOT RECURSIVE. The gate checks the immediate keys of the declared object (or of
// each object element of a declared array) and stops. Deeper levels are a different schema and
// would need their own declaration; guessing at them is how a gate starts refusing shapes nobody
// declared. This also keeps the map-shaped payloads honest:
// material.authoring.create_material_instance's `parameters` has a CLOSED set of four bucket names
// one level down and caller-chosen parameter names below that, so exactly one level is the level
// that can be closed. Its sibling set_material_instance_parameters, whose top-level `scalar` /
// `vector` / `texture` / `staticSwitch` maps are keyed by caller-chosen names all the way down,
// declares nothing here and must not.
//
// CASE-INSENSITIVE, TO MATCH THE ACCESSOR. Comparison is TArray<FString>::Contains, i.e. FString
// operator==, which is case-insensitive - the same predicate the top-level unknown-name pass uses
// (TSet<FString>::Contains) and, more importantly, the same one FJsonObject's own field lookup
// uses: TMap<FString, ...> hashes with FCrc::Strihash_DEPRECATED, so Obj->TryGetObjectField("scalar")
// really does find a `"Scalar"` the caller sent. A case-SENSITIVE allow-list would refuse a key the
// reader then honours - the gate promising a refusal the accessor contradicts.
//
// A NON-OBJECT ELEMENT IS NOT THIS PASS'S FAULT TO REPORT. An array element that is a bare string,
// or a declared-object slot holding a scalar, is a SHAPE fault: the declared-type pass ahead of this
// one owns the top-level shape, and an element type inside an array has no declaration at all yet.
// Skipping them keeps this pass answering exactly one question - "is this key one you accept?".

#include "CoreMinimal.h"
#include "Compat/JsonKeyCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace PinWrightNestedParams
{
    // Every key of Obj that AllowedNestedKeys does not name, formatted as one fault per key and
    // appended to OutFaults. PathPrefix is the caller-facing spelling of the owning slot
    // ("grid", or "states[0]"), so the message names the key the way the caller wrote it.
    inline void PinWrightCollectUnknownKeysOfObject(const FString& PathPrefix,
                                                    const TSharedPtr<FJsonObject>& Obj,
                                                    const TArray<FString>& AllowedNestedKeys,
                                                    TArray<FString>& OutFaults)
    {
        if (!Obj.IsValid())
        {
            return;
        }

        for (const auto& Pair : Obj->Values)
        {
            const FString Key = EARGCompat::JsonKeyToString(Pair.Key);
            if (AllowedNestedKeys.Contains(Key))
            {
                continue;
            }

            OutFaults.Add(FString::Printf(
                TEXT("'%s.%s' is not a key of '%s'. Valid keys: [%s]"),
                *PathPrefix, *Key, *PathPrefix, *FString::Join(AllowedNestedKeys, TEXT(", "))));
        }
    }

    // The whole gate for one (wire name, received value, declared nested schema) triple.
    // Object -> its own keys; array -> the keys of each object element, indexed in the message;
    // anything else -> nothing (see the header comment).
    inline void PinWrightCollectUnknownNestedKeys(const FString& WireName,
                                                  const TSharedPtr<FJsonValue>& Value,
                                                  const TArray<FString>& AllowedNestedKeys,
                                                  TArray<FString>& OutFaults)
    {
        if (AllowedNestedKeys.Num() == 0 || !Value.IsValid())
        {
            return;
        }

        const TSharedPtr<FJsonObject>* AsObject = nullptr;
        if (Value->TryGetObject(AsObject) && AsObject)
        {
            PinWrightCollectUnknownKeysOfObject(WireName, *AsObject, AllowedNestedKeys, OutFaults);
            return;
        }

        const TArray<TSharedPtr<FJsonValue>>* AsArray = nullptr;
        if (Value->TryGetArray(AsArray) && AsArray)
        {
            for (int32 Index = 0; Index < AsArray->Num(); ++Index)
            {
                const TSharedPtr<FJsonValue>& Element = (*AsArray)[Index];
                const TSharedPtr<FJsonObject>* ElementObject = nullptr;
                if (Element.IsValid() && Element->TryGetObject(ElementObject) && ElementObject)
                {
                    PinWrightCollectUnknownKeysOfObject(
                        FString::Printf(TEXT("%s[%d]"), *WireName, Index),
                        *ElementObject, AllowedNestedKeys, OutFaults);
                }
            }
        }
    }
}
