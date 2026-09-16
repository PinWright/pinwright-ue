// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Formatter signature: convert a JSON result object into compact plain text.
// Returns true on success. On false, dispatcher falls back to the JSON envelope.
using FRpcFormatterFunc = bool(*)(const TSharedPtr<FJsonObject>& JsonResult, FString& OutText);

// Registration record paired with a specific RPC method name.
struct PINWRIGHT_API FFormatterRegistration
{
    FString MethodName;
    FRpcFormatterFunc Func = nullptr;
};

// Static collector mirrors FAutoRegisterHandler — formatter records accumulate
// before the dispatcher drains them at subsystem init.
struct PINWRIGHT_API FAutoRegisterFormatter
{
    FAutoRegisterFormatter(const TCHAR* Method, FRpcFormatterFunc Func);

    static TArray<FFormatterRegistration>& GetPendingRegistrations();
};

// Counter-based unique-name macro (keeps Unity-build safety same as REGISTER_RPC_HANDLER)
#define EARG_FMT_PP_CAT_IMPL(A, B) A##B
#define EARG_FMT_PP_CAT(A, B) EARG_FMT_PP_CAT_IMPL(A, B)
#define EARG_FMT_PP_CAT3(A, B, C) EARG_FMT_PP_CAT(EARG_FMT_PP_CAT(A, B), C)

#define REGISTER_RPC_FORMATTER_INNER(Method, Func, Id) \
    static FAutoRegisterFormatter EARG_FMT_PP_CAT3(AutoFmtReg_, Id, _)( \
        TEXT(Method), Func)

#define REGISTER_RPC_FORMATTER(Method, Func) \
    REGISTER_RPC_FORMATTER_INNER(Method, Func, __COUNTER__)
