// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Dispatch/SafePoint.h"
#include "Dispatch/WorldPrecondition.h"

namespace
{
    bool IsReadOnlySafePointProbe(const FString& Method)
    {
        // These entries are deliberately tick-gated because their analysis
        // stack is unsafe, but they do not mutate a world or project asset.
        static const TSet<FString> ReadOnlyMethods =
        {
            TEXT("audio.analysis.analyze"),
            TEXT("audio.analysis.audit_folder"),
            TEXT("audio.analysis.compare"),
            TEXT("audio.analysis.decompose"),
            TEXT("audio.analysis.to_recipe")
        };
        return ReadOnlyMethods.Contains(Method);
    }
}

TArray<FHandlerRegistration>& FAutoRegisterHandler::GetPendingRegistrations()
{
    // Function-local static -- thread-safe initialization (C++11 guarantee)
    static TArray<FHandlerRegistration> Registrations;
    return Registrations;
}

FAutoRegisterHandler::FAutoRegisterHandler(
    const TCHAR* Method, const TCHAR* Category,
    const TCHAR* Summary, TArray<FParamSpec> Params,
    FRpcHandlerFunc Func, const bool bExplicitMutating)
{
    FHandlerRegistration Reg;
    Reg.MethodName = Method;
    Reg.Category = Category;
    Reg.Summary = Summary;
    Reg.Params = MoveTemp(Params);
    Reg.bMutating = bExplicitMutating ||
        (PinWrightSafePoint::IsTickUnsafeMethod(Reg.MethodName) &&
            !IsReadOnlySafePointProbe(Reg.MethodName));
    Reg.Func = Func;
    GetPendingRegistrations().Add(MoveTemp(Reg));
}
