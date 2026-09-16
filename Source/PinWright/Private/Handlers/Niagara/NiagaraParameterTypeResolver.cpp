// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraParameterTypeResolver.h"

// Compat header, not the raw engine one: UE 5.3 does not define UE_VERSION_NEWER_THAN_OR_EQUAL.
#include "Compat/EngineVersionCompat.h"
// NiagaraTypeRegistry.h was split out of NiagaraTypes.h in UE 5.6; on UE 5.4 the class lives in
// NiagaraTypes.h, which the header above already includes.
#if __has_include("NiagaraTypeRegistry.h")
#include "NiagaraTypeRegistry.h"
#endif

namespace PinWrightNiagara
{
namespace
{
    struct FNiagaraTypeAliasRow
    {
        ENiagaraParameterValueKind Kind;
        // Canonical name first — the spelling niagara.inspect prints for this type — then the
        // short aliases the verbs have always accepted.
        TArray<const TCHAR*> Names;
    };

    const TArray<FNiagaraTypeAliasRow>& GetAliasRows()
    {
        static const TArray<FNiagaraTypeAliasRow> Rows =
        {
            {ENiagaraParameterValueKind::Float, {TEXT("NiagaraFloat"), TEXT("float"), TEXT("double")}},
            {ENiagaraParameterValueKind::Int, {TEXT("NiagaraInt32"), TEXT("int"), TEXT("int32")}},
            {ENiagaraParameterValueKind::Bool, {TEXT("NiagaraBool"), TEXT("bool"), TEXT("boolean")}},
            {ENiagaraParameterValueKind::Vec2, {TEXT("Vector2f"), TEXT("Vector2"), TEXT("Vec2")}},
            // "Vector" and "/Script/CoreUObject.Vector" name the double-precision core struct, not
            // Vector3f, but both verbs have answered them with the Vec3 definition since the first
            // release (a parameter store holds the single-precision type), and one of the two is
            // pinned by an existing round-trip test. Kept as aliases rather than let the registry
            // walk below answer them with the LWC type; the consequence, documented on the wiki
            // page, is that a genuinely double-precision Vector parameter is not addressable.
            {ENiagaraParameterValueKind::Vec3, {TEXT("Vector3f"), TEXT("Vector"), TEXT("Vector3"), TEXT("Vec3"), TEXT("/Script/CoreUObject.Vector")}},
            {ENiagaraParameterValueKind::Position, {TEXT("NiagaraPosition"), TEXT("Position")}},
            {ENiagaraParameterValueKind::LinearColor, {TEXT("LinearColor"), TEXT("Color")}},
            {ENiagaraParameterValueKind::NiagaraID, {TEXT("NiagaraID"), TEXT("ID")}},
            {ENiagaraParameterValueKind::NiagaraSpawnInfo, {TEXT("NiagaraSpawnInfo"), TEXT("SpawnInfo")}},
            {ENiagaraParameterValueKind::Half, {TEXT("NiagaraHalf"), TEXT("half")}},
            {ENiagaraParameterValueKind::HalfVec2, {TEXT("NiagaraHalfVector2"), TEXT("half2"), TEXT("HalfVec2")}},
            {ENiagaraParameterValueKind::HalfVec3, {TEXT("NiagaraHalfVector3"), TEXT("half3"), TEXT("HalfVec3")}},
            {ENiagaraParameterValueKind::HalfVec4, {TEXT("NiagaraHalfVector4"), TEXT("half4"), TEXT("HalfVec4")}},
        };
        return Rows;
    }

    FNiagaraTypeDefinition DefinitionForKind(ENiagaraParameterValueKind Kind)
    {
        switch (Kind)
        {
        case ENiagaraParameterValueKind::Float:            return FNiagaraTypeDefinition::GetFloatDef();
        case ENiagaraParameterValueKind::Int:              return FNiagaraTypeDefinition::GetIntDef();
        case ENiagaraParameterValueKind::Bool:             return FNiagaraTypeDefinition::GetBoolDef();
        case ENiagaraParameterValueKind::Vec2:             return FNiagaraTypeDefinition::GetVec2Def();
        case ENiagaraParameterValueKind::Vec3:             return FNiagaraTypeDefinition::GetVec3Def();
        case ENiagaraParameterValueKind::Position:         return FNiagaraTypeDefinition::GetPositionDef();
        case ENiagaraParameterValueKind::LinearColor:      return FNiagaraTypeDefinition::GetColorDef();
        case ENiagaraParameterValueKind::NiagaraID:        return FNiagaraTypeDefinition::GetIDDef();
        case ENiagaraParameterValueKind::NiagaraSpawnInfo: return FNiagaraTypeDefinition(FNiagaraSpawnInfo::StaticStruct());
        case ENiagaraParameterValueKind::Half:             return FNiagaraTypeDefinition::GetHalfDef();
        case ENiagaraParameterValueKind::HalfVec2:         return FNiagaraTypeDefinition::GetHalfVec2Def();
        case ENiagaraParameterValueKind::HalfVec3:         return FNiagaraTypeDefinition::GetHalfVec3Def();
        case ENiagaraParameterValueKind::HalfVec4:         return FNiagaraTypeDefinition::GetHalfVec4Def();
        default:                                           return FNiagaraTypeDefinition();
        }
    }

    bool TryResolveAlias(const FString& TypeName, FNiagaraResolvedParameterType& OutType)
    {
        for (const FNiagaraTypeAliasRow& Row : GetAliasRows())
        {
            for (const TCHAR* Name : Row.Names)
            {
                if (TypeName.Equals(Name, ESearchCase::IgnoreCase))
                {
                    OutType.Kind = Row.Kind;
                    OutType.Definition = DefinitionForKind(Row.Kind);
                    return OutType.Definition.IsValid();
                }
            }
        }
        return false;
    }

    // Match priority, best first. An identity-name match always beats a storage-struct match,
    // because an enum type's storage struct is FNiagaraInt32 and would otherwise answer for the
    // int type (and for every other enum). Within a tier, a plain definition beats a flagged one
    // (TF_Static, TF_SerializedAsLWC): the registry holds IntDef and IntDef.ToStaticDef() under
    // the same name, and the plain one is what a parameter store holds.
    enum class ERegistryMatchTier : uint8
    {
        IdentityName = 0,
        IdentityNameFlagged = 1,
        StructPathOrName = 2,
        StructPathOrNameFlagged = 3,
        None = 4,
    };

    bool TryResolveRegistered(const FString& TypeName, FNiagaraTypeDefinition& OutType)
    {
#if !UE_VERSION_OLDER_THAN(5, 4, 0)
        // FNiagaraTypeRegistry::ProcessRegistryQueue() (deferred type-registration flush) was
        // added in UE 5.4; on 5.3 types register immediately so there is no queue to drain.
        FNiagaraTypeRegistry::ProcessRegistryQueue();
#endif
        ERegistryMatchTier BestTier = ERegistryMatchTier::None;
        for (const FNiagaraTypeDefinition& Candidate : FNiagaraTypeRegistry::GetRegisteredTypes())
        {
            UScriptStruct* Struct = Candidate.GetScriptStruct();
            if (!Struct)
            {
                // Data interfaces and UObject-typed parameters. The parameter-store edit path
                // writes struct payloads only, so refusing here is what produces
                // INVALID_PARAMETER_TYPE rather than a struct write against a null struct.
                continue;
            }

            const bool bFlagged = Candidate.GetFlags() != 0;
            ERegistryMatchTier Tier = ERegistryMatchTier::None;
            if (Candidate.GetName().Equals(TypeName, ESearchCase::IgnoreCase))
            {
                Tier = bFlagged ? ERegistryMatchTier::IdentityNameFlagged : ERegistryMatchTier::IdentityName;
            }
            else if (!Candidate.IsEnum()
                && (Struct->GetName().Equals(TypeName, ESearchCase::IgnoreCase)
                    || Struct->GetPathName().Equals(TypeName, ESearchCase::IgnoreCase)))
            {
                Tier = bFlagged ? ERegistryMatchTier::StructPathOrNameFlagged : ERegistryMatchTier::StructPathOrName;
            }

            if (Tier < BestTier)
            {
                BestTier = Tier;
                OutType = Candidate;
                if (BestTier == ERegistryMatchTier::IdentityName)
                {
                    break;
                }
            }
        }
        return BestTier != ERegistryMatchTier::None;
    }
}

bool ResolveNiagaraParameterType(const FString& TypeName, FNiagaraResolvedParameterType& OutType)
{
    if (TypeName.IsEmpty())
    {
        return false;
    }
    if (TryResolveAlias(TypeName, OutType))
    {
        return true;
    }

    FNiagaraTypeDefinition Registered;
    if (!TryResolveRegistered(TypeName, Registered))
    {
        return false;
    }
    OutType.Definition = Registered;
    OutType.Kind = ENiagaraParameterValueKind::ScriptStruct;
    return true;
}

FString DescribeNiagaraType(const FNiagaraTypeDefinition& Type)
{
    if (!Type.IsValid())
    {
        return TEXT("<invalid type>");
    }

    FString Path;
    if (const UEnum* Enum = Type.GetEnum())
    {
        Path = Enum->GetPathName();
    }
    else if (const UClass* Class = Type.GetClass())
    {
        Path = Class->GetPathName();
    }
    else if (const UStruct* Struct = Type.GetStruct())
    {
        Path = Struct->GetPathName();
    }

    return Path.IsEmpty()
        ? Type.GetName()
        : FString::Printf(TEXT("%s (%s)"), *Type.GetName(), *Path);
}

const TArray<FString>& GetNiagaraParameterTypeAliases()
{
    static const TArray<FString> Aliases = []()
    {
        TArray<FString> Names;
        for (const FNiagaraTypeAliasRow& Row : GetAliasRows())
        {
            for (const TCHAR* Name : Row.Names)
            {
                Names.Add(Name);
            }
        }
        return Names;
    }();
    return Aliases;
}
}
