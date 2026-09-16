// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

class UObject;
class UClass;

namespace IrSidecarRegistry
{
    struct FIrSidecarResult
    {
        FString Text;
        TArray<FString> Warnings;
        bool bSuccess = false;
    };

    using FIrSidecarClassThunk = UClass* (*)();
    using FIrSidecarBuildFn = FIrSidecarResult (*)(UObject*);

    struct FIrSidecarSpec
    {
        const TCHAR* Name = nullptr;
        const TCHAR* FileName = nullptr;
        FIrSidecarClassThunk ClassFn = nullptr;
        FIrSidecarBuildFn BuildFn = nullptr;
        int32 Priority = 100;
    };

    class PINWRIGHT_API FAutoRegisterIrSidecar
    {
    public:
        explicit FAutoRegisterIrSidecar(FIrSidecarSpec InSpec);
    };

    PINWRIGHT_API TArray<FIrSidecarSpec> GetRegisteredIrSidecars();
}

#define EARG_IR_PP_CAT_IMPL(A, B) A##B
#define EARG_IR_PP_CAT(A, B) EARG_IR_PP_CAT_IMPL(A, B)

#define REGISTER_DECOMPILE_IR_INNER(SpecName, FileNameValue, ClassThunkValue, BuildFnValue, PriorityValue, Id) \
    static IrSidecarRegistry::FAutoRegisterIrSidecar \
        EARG_IR_PP_CAT(GAutoRegisterIrSidecar_, Id)( \
            IrSidecarRegistry::FIrSidecarSpec{ \
                SpecName, FileNameValue, ClassThunkValue, BuildFnValue, PriorityValue \
            })

#define REGISTER_DECOMPILE_IR(SpecName, FileNameValue, ClassThunkValue, BuildFnValue, PriorityValue) \
    REGISTER_DECOMPILE_IR_INNER(SpecName, FileNameValue, ClassThunkValue, BuildFnValue, PriorityValue, __COUNTER__)
