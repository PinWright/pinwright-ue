// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/FormatterRegistration.h"

TArray<FFormatterRegistration>& FAutoRegisterFormatter::GetPendingRegistrations()
{
    // Function-local static — C++11 thread-safe initialization. Mirrors
    // FAutoRegisterHandler::GetPendingRegistrations() so both registration paths
    // have the same lifetime semantics.
    static TArray<FFormatterRegistration> Registrations;
    return Registrations;
}

FAutoRegisterFormatter::FAutoRegisterFormatter(const TCHAR* Method, FRpcFormatterFunc Func)
{
    FFormatterRegistration Reg;
    Reg.MethodName = Method;
    Reg.Func = Func;
    GetPendingRegistrations().Add(MoveTemp(Reg));
}
