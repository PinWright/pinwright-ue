// Copyright (c) 2026 Alexander Penkin. MIT License.

// The single wire-`type` resolver for the Niagara parameter-store verbs.
//
// niagara.set_parameter / niagara.add_parameter used to answer the question "what type is this?"
// twice, in two files, from two disagreeing tables: NiagaraEditTypes.cpp decided the VALUE SHAPE
// from the raw string, and NiagaraEditHandler.cpp decided the TYPE DEFINITION from a second table
// plus a registry walk. Neither table carried the canonical names niagara.inspect prints, so
// `type: "NiagaraInt32"` — the exact string the documented discovery route hands back — was first
// told it needed a JSON object (the shape branch fell through to "registered script struct") and
// then refused PARAMETER_TYPE_MISMATCH against its own spelling (the message printed the raw
// request text on both sides of "not"). Resolving once, here, is what makes the accepted set and
// the shape rule the same fact.
//
// WHY THE REGISTRY WALK IS NOT ENOUGH ON ITS OWN. FNiagaraTypeDefinition::GetStruct() of an ENUM
// type returns FNiagaraInt32 (that is the storage the enum uses), so matching a request against
// candidate.GetScriptStruct()->GetName() makes EVERY registered enum type answer to
// "NiagaraInt32" and to "/Script/Niagara.NiagaraInt32". Which one wins is registration order —
// i.e. which content enums this project happens to have loaded — so the same request resolves
// differently in two editors. The alias table below answers the built-in spellings outright, and
// the walk that follows it prefers a candidate's own identity name over its storage struct and
// never lets an enum answer for a struct name.

#pragma once

#include "CoreMinimal.h"
#include "NiagaraTypes.h"

namespace PinWrightNiagara
{
    // What a resolved parameter type expects on the wire, and which parameter-store setter writes
    // it. One enumerator per FNiagaraParameterStore::SetParameterValue overload the safe edit path
    // uses, plus ScriptStruct for everything written field-by-field through FStructOnScope.
    enum class ENiagaraParameterValueKind : uint8
    {
        Float,
        Int,
        Bool,
        Vec2,
        Vec3,
        Position,
        LinearColor,
        NiagaraID,
        NiagaraSpawnInfo,
        Half,
        HalfVec2,
        HalfVec3,
        HalfVec4,
        // Resolved to a registered script struct with no dedicated setter: written by copying
        // JSON fields onto the struct, so the value must be a JSON object.
        ScriptStruct,
    };

    struct FNiagaraResolvedParameterType
    {
        FNiagaraTypeDefinition Definition;
        ENiagaraParameterValueKind Kind = ENiagaraParameterValueKind::ScriptStruct;
    };

    // Resolve one wire `type` spelling. Accepts the short aliases the verbs have always taken
    // ("int32", "vec3", "color", ...), the canonical struct names niagara.inspect prints
    // ("NiagaraInt32", "Vector3f", "NiagaraHalfVector2", ...), and any other registered Niagara
    // type by its own name or by its struct path ("/Script/Niagara.NiagaraInt32"), matched
    // case-insensitively.
    //
    // Returns false for a type the parameter-store edit path cannot write: data interfaces and
    // UObject-typed parameters (no script struct), and anything unregistered. Callers report that
    // as INVALID_PARAMETER_TYPE.
    bool ResolveNiagaraParameterType(const FString& TypeName, FNiagaraResolvedParameterType& OutType);

    // "<name> (<path>)", e.g. "NiagaraInt32 (/Script/Niagara.NiagaraInt32)". Two distinct types
    // can share a display name (an enum and the struct it is stored as; a content struct and a
    // core one), so a type-mismatch message that prints only names can read as "X is not X".
    // The path is what makes the two sides of such a message distinguishable.
    FString DescribeNiagaraType(const FNiagaraTypeDefinition& Type);

    // The alias spellings ResolveNiagaraParameterType answers directly, canonical name first for
    // each type. Documented on the niagara wiki page and asserted by the parameter-type tests, so
    // the accepted set cannot drift from what is published.
    const TArray<FString>& GetNiagaraParameterTypeAliases();
}
