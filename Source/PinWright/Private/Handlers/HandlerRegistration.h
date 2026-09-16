// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once
#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"

class FHandlerContext;

// Handler function signature
using FRpcHandlerFunc = bool(*)(FHandlerContext& Ctx);

// Registration record
struct PINWRIGHT_API FHandlerRegistration
{
    FString MethodName;               // "actor.spawn"
    FString Category;                 // "actor"
    FString Summary;                  // "Spawn an actor in the world"
    TArray<FParamSpec> Params;        // Parameter schemas
    bool bMutating = false;           // Shared dispatcher effect policy
    FRpcHandlerFunc Func;
};

// Static collector -- accumulates registrations before subsystem Initialize()
struct PINWRIGHT_API FAutoRegisterHandler
{
    FAutoRegisterHandler(const TCHAR* Method, const TCHAR* Category,
                         const TCHAR* Summary, TArray<FParamSpec> Params,
                         FRpcHandlerFunc Func, bool bExplicitMutating = false);

    // Returns the global pending registrations array
    static TArray<FHandlerRegistration>& GetPendingRegistrations();
};

// Macro for handler files -- uses __COUNTER__ (not __LINE__) for Unity build safety.
// Keep token pasting local so registration works in all translation units (including tests).
#define EARG_PP_CAT_IMPL(A, B) A##B
#define EARG_PP_CAT(A, B) EARG_PP_CAT_IMPL(A, B)
#define EARG_PP_CAT3(A, B, C) EARG_PP_CAT(EARG_PP_CAT(A, B), C)

#define REGISTER_RPC_HANDLER_INNER(Method, Category, Summary, Params, Id) \
    static bool EARG_PP_CAT3(AutoHandler_, Id, _)(FHandlerContext& Ctx); \
    static FAutoRegisterHandler EARG_PP_CAT3(AutoReg_, Id, _)( \
        TEXT(Method), TEXT(Category), TEXT(Summary), \
        Params, \
        &EARG_PP_CAT3(AutoHandler_, Id, _), false); \
    static bool EARG_PP_CAT3(AutoHandler_, Id, _)(FHandlerContext& Ctx)

#define REGISTER_RPC_HANDLER(Method, Category, Summary, Params) \
    REGISTER_RPC_HANDLER_INNER(Method, Category, Summary, Params, __COUNTER__)

// Explicit world-mutating registration. The ordinary macro intentionally does
// not infer effects from a method name; tick-unsafe table entries remain
// authoritative for that safety class, while handlers outside it opt in here.
#define REGISTER_RPC_MUTATING_HANDLER_INNER(Method, Category, Summary, Params, Id) \
    static bool EARG_PP_CAT3(AutoMutatingHandler_, Id, _)(FHandlerContext& Ctx); \
    static FAutoRegisterHandler EARG_PP_CAT3(AutoMutatingReg_, Id, _)( \
        TEXT(Method), TEXT(Category), TEXT(Summary), \
        Params, \
        &EARG_PP_CAT3(AutoMutatingHandler_, Id, _), true); \
    static bool EARG_PP_CAT3(AutoMutatingHandler_, Id, _)(FHandlerContext& Ctx)

#define REGISTER_RPC_MUTATING_HANDLER(Method, Category, Summary, Params) \
    REGISTER_RPC_MUTATING_HANDLER_INNER(Method, Category, Summary, Params, __COUNTER__)
