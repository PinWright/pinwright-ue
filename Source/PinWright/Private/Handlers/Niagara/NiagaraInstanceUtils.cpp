// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraInstanceUtils.h"

#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/UObjectIterator.h"

namespace PinWrightNiagara
{
    namespace
    {
        // UNiagaraComponent::DestroyInstance releases SystemInstanceController and always
        // broadcasts, so the controller must be read BEFORE the call. A component without one has
        // no running simulation to stop and must not be counted (NiagaraComponent.cpp:1942).
        int32 DestroyInstanceCountingLive(UNiagaraComponent& Component)
        {
            const bool bWasLive = Component.GetSystemInstanceController().IsValid();
            Component.DestroyInstance();
            return bWasLive ? 1 : 0;
        }
    }

    int32 KillSystemInstances(const UNiagaraSystem& System)
    {
        int32 Quiesced = 0;
        for (TObjectIterator<UNiagaraComponent> It; It; ++It)
        {
            UNiagaraComponent* Component = *It;
            if (Component && Component->GetAsset() == &System)
            {
                Quiesced += DestroyInstanceCountingLive(*Component);
            }
        }
        return Quiesced;
    }

    int32 CountLiveSystemInstances(const UNiagaraSystem& System)
    {
        int32 Live = 0;
        for (TObjectIterator<UNiagaraComponent> It; It; ++It)
        {
            const UNiagaraComponent* Component = *It;
            if (!IsValid(Component) || Component->IsTemplate())
            {
                continue;
            }
            if (Component->GetAsset() == &System && Component->GetSystemInstanceController().IsValid())
            {
                ++Live;
            }
        }
        return Live;
    }

    int32 KillSystemInstancesUsingEmitter(const UNiagaraEmitter& Emitter, const FGuid& VersionGuid)
    {
        // UNiagaraSystem::UsesEmitter only walks the already-resolved emitter handles, so this
        // does not force a load on systems the iterator happens to reach.
        const FVersionedNiagaraEmitter VersionedEmitter(const_cast<UNiagaraEmitter*>(&Emitter), VersionGuid);
        int32 Quiesced = 0;
        for (TObjectIterator<UNiagaraComponent> It; It; ++It)
        {
            UNiagaraComponent* Component = *It;
            if (!Component)
            {
                continue;
            }
            const UNiagaraSystem* System = Component->GetAsset();
            if (System && System->UsesEmitter(VersionedEmitter))
            {
                Quiesced += DestroyInstanceCountingLive(*Component);
            }
        }
        return Quiesced;
    }

    bool IsSupportedRuntimeParameterType(const FString& ParameterType)
    {
        return ParameterType.Equals(TEXT("Float"), ESearchCase::IgnoreCase)
            || ParameterType.Equals(TEXT("Vector"), ESearchCase::IgnoreCase)
            || ParameterType.Equals(TEXT("Color"), ESearchCase::IgnoreCase)
            || ParameterType.Equals(TEXT("Bool"), ESearchCase::IgnoreCase);
    }

    EParameterLookup ClassifyComponentParameter(const UNiagaraComponent* Component, FName ParamName, const FString& ParameterType)
    {
        // Map the handler's parameterType string onto the Niagara typedef the value would be
        // written as. Vector covers both Vec3 and Position (User.Velocity etc.); accept either.
        FNiagaraTypeDefinition TypeDef;
        bool bAlsoAcceptPosition = false;
        if (ParameterType.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
        {
            TypeDef = FNiagaraTypeDefinition::GetFloatDef();
        }
        else if (ParameterType.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
        {
            TypeDef = FNiagaraTypeDefinition::GetVec3Def();
            bAlsoAcceptPosition = true;
        }
        else if (ParameterType.Equals(TEXT("Color"), ESearchCase::IgnoreCase))
        {
            TypeDef = FNiagaraTypeDefinition::GetColorDef();
        }
        else if (ParameterType.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
        {
            TypeDef = FNiagaraTypeDefinition::GetBoolDef();
        }
        else
        {
            return EParameterLookup::InvalidType;
        }

        // Fail closed (treat as NotFound) when the component or its asset is missing.
        if (!Component)
        {
            return EParameterLookup::NotFound;
        }
        UNiagaraSystem* System = Component->GetAsset();
        if (!System)
        {
            return EParameterLookup::NotFound;
        }

        // GetExposedParameters() is the FNiagaraUserRedirectionParameterStore; its
        // FindParameterVariable override resolves the "User." prefix the same way the runtime
        // setters do, so a User.-prefixed name validates correctly.
        const FNiagaraUserRedirectionParameterStore& UserStore = System->GetExposedParameters();
        if (UserStore.FindParameterVariable(FNiagaraVariable(TypeDef, ParamName)) != nullptr)
        {
            return EParameterLookup::Found;
        }
        if (bAlsoAcceptPosition &&
            UserStore.FindParameterVariable(FNiagaraVariable(FNiagaraTypeDefinition::GetPositionDef(), ParamName)) != nullptr)
        {
            return EParameterLookup::Found;
        }
        return EParameterLookup::NotFound;
    }
}
