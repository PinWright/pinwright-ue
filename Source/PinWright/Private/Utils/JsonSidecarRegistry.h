// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UObject;
class UClass;

// Sibling to IrSidecarRegistry for JSON-builder sidecars. Where IrSidecarRegistry carries
// text-IR results ({Text, Warnings, bSuccess}), this registry carries a TSharedPtr<FJsonObject>
// produced by a per-type BuildFn, with an optional text-emitter twin written alongside the JSON
// and an optional diagnostic recorded when the builder returns null. Asset types whose dump is
// "class-default Properties + one JSON sidecar (± a text twin)" register here instead of growing
// the BuildAllFilesForAsset dispatch chain.
namespace JsonSidecarRegistry
{
    using FJsonSidecarClassThunk = UClass* (*)();
    using FJsonSidecarBuildFn = TSharedPtr<FJsonObject> (*)(UObject*);
    using FJsonSidecarTextEmitterFn = FString (*)(const TSharedPtr<FJsonObject>&);

    struct FJsonSidecarSpec
    {
        const TCHAR* Name = nullptr;
        const TCHAR* FileName = nullptr;
        FJsonSidecarClassThunk ClassFn = nullptr;
        FJsonSidecarBuildFn BuildFn = nullptr;
        const TCHAR* TextEmitterFileName = nullptr;
        FJsonSidecarTextEmitterFn TextEmitterFn = nullptr;
        const TCHAR* NullDiagnostic = nullptr;
        int32 Priority = 100;
    };

    class PINWRIGHT_API FAutoRegisterJsonSidecar
    {
    public:
        explicit FAutoRegisterJsonSidecar(FJsonSidecarSpec InSpec);
    };

    PINWRIGHT_API TArray<FJsonSidecarSpec> GetRegisteredJsonSidecars();
}

#define EARG_JSON_PP_CAT_IMPL(A, B) A##B
#define EARG_JSON_PP_CAT(A, B) EARG_JSON_PP_CAT_IMPL(A, B)

#define REGISTER_DUMP_JSON_SIDECAR_INNER(Id, ...) \
    static JsonSidecarRegistry::FAutoRegisterJsonSidecar \
        EARG_JSON_PP_CAT(GAutoRegisterJsonSidecar_, Id)( \
            JsonSidecarRegistry::FJsonSidecarSpec{ __VA_ARGS__ })

#define REGISTER_DUMP_JSON_SIDECAR(...) \
    REGISTER_DUMP_JSON_SIDECAR_INNER(__COUNTER__, __VA_ARGS__)
