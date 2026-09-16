// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UUserDefinedStruct;
struct FStructVariableDescription;

namespace UserDefinedStructDumpBuilder
{
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildUserDefinedStructFieldJson(const FStructVariableDescription& VarDesc);
    PINWRIGHT_API TSharedPtr<FJsonObject> BuildUserDefinedStructJson(const UUserDefinedStruct* Struct);
}
