// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "UObject/UnrealType.h"

struct FFilteredPropertyCopyOptions
{
    EPropertyFlags RequiredAnyFlags = static_cast<EPropertyFlags>(CPF_Edit | CPF_BlueprintVisible);
    EPropertyFlags ExcludedAnyFlags = static_cast<EPropertyFlags>(CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient | CPF_TextExportTransient);
    TSet<FName> SkipPropertyNames;
};

PINWRIGHT_API TSharedPtr<FJsonObject> BuildSparsePropertyDiffJson(
    UObject* Instance,
    UObject* Baseline,
    const TSet<FName>& SkipProperties = TSet<FName>());

PINWRIGHT_API void CopyFilteredMatchingProperties(
    UObject* Source,
    UObject* Target,
    const FFilteredPropertyCopyOptions& Options = FFilteredPropertyCopyOptions());

PINWRIGHT_API void AddSceneAttachmentPropertySkips(FFilteredPropertyCopyOptions& Options);
