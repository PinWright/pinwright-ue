// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UEnvQuery;

namespace EnvQueryDumpBuilder
{
    // Walks a UEnvQuery's Options array and serializes the authored topology that asset.dump's
    // generic property walk cannot reach. UEnvQuery::Options is a plain
    // TArray<TObjectPtr<UEnvQueryOption>> (no Instanced specifier), so the dumper emits each
    // option as a bare object-ref path string and stops — the nested generator class, tests,
    // and per-test purpose / filter / scoring fields never appear. This builder expands them:
    // per option the generator class, and per test the type, purpose, filterType (+ float
    // bounds / bool match), scoringEquation (+ factor), and comment — so the dump is reviewable
    // instead of an opaque Options object-ref.
    //
    // Returns nullptr when the asset is null (RunRegisteredJsonSidecars records the
    // NullDiagnostic and skips the sidecar).
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildEnvQueryJson(const UEnvQuery* Query);
}
