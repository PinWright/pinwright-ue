// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared param-spec lookup helper for alias-registration tests. Extracted from
// anonymous namespaces previously duplicated across the param-alias regression
// tests (TestAssetPathParamAlias.cpp, TestGenerateLodsPathParamAlias.cpp, and
// siblings). Required because the plugin's tests share a single module with Unity
// builds: identically-named anonymous-namespace helpers in two .cpp files produce
// ODR / redefinition errors when Unity merges them into one translation unit
// (observed on UE 5.3, where these two TUs landed in the same Unity chunk).
//
// Conventions match Tests/Infra/DispatcherTestHelpers.h: named namespace + inline
// functions, no module API macro.

#include "CoreMinimal.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"

namespace ParamSpecTestHelpers
{
    // Look up a registered param spec by method + param name across the pending
    // auto-registrations. Returns nullptr if the method or param is not declared.
    inline const FParamSpec* FindParamSpec(const FString& Method, const FString& ParamName)
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (!Reg.MethodName.Equals(Method))
            {
                continue;
            }
            for (const FParamSpec& Spec : Reg.Params)
            {
                if (Spec.Name.Equals(ParamName))
                {
                    return &Spec;
                }
            }
        }
        return nullptr;
    }

    // Every wire name the dispatcher accepts for one registration: the canonical Name, the
    // untyped Aliases, and each TypedAliases[].Name. Mirrors AddKnownParamNames /
    // CollectParamNames in Dispatch/RpcDispatcher.cpp -- the three sources that build the
    // KnownParams set ValidateHandlerParams rejects against. Empty when the method is not
    // registered (an integration sub-module whose engine plugin is disabled on this host).
    inline TSet<FString> CollectAcceptedParamNames(const FString& Method)
    {
        TSet<FString> Accepted;
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (!Reg.MethodName.Equals(Method))
            {
                continue;
            }
            for (const FParamSpec& Spec : Reg.Params)
            {
                Accepted.Add(Spec.Name);
                for (const FString& Alias : Spec.Aliases)
                {
                    Accepted.Add(Alias);
                }
                for (const FParamAliasSpec& Alias : Spec.TypedAliases)
                {
                    Accepted.Add(Alias.Name);
                }
            }
        }
        return Accepted;
    }

    // True when a request naming WireName clears the dispatcher's declared-parameter gate for
    // Method. Unlike FindParamSpec this resolves aliases, so it is the honest answer to "may a
    // caller pass this key". Use it whenever a handler body reads a key the test cares about:
    // Tests/TestUtils.h's InvokeHandler never runs that gate, so a body-level read proves the
    // handler USES the key and says nothing about whether a caller can SUPPLY it.
    inline bool IsParamAccepted(const FString& Method, const FString& WireName)
    {
        return CollectAcceptedParamNames(Method).Contains(WireName);
    }
}
