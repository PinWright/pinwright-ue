// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared JSON array-shape assertion helpers for the Niagara test cluster.
// Extracted from the per-test anonymous namespaces of TestNiagaraDumpBuilder.cpp,
// TestNiagaraHandlers.cpp, and TestNiagaraModelBuilder.cpp so Unity build can
// merge those .cpp files into a single TU without tripping C2084 on identical
// HasArrayField / ArrayFieldNum definitions.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Templates/SharedPointer.h"

namespace NiagaraJsonAssertionHelpers
{
    inline bool HasArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        return Object.IsValid() && Object->TryGetArrayField(FieldName, Values) && Values;
    }

    inline int32 ArrayFieldNum(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        return Object.IsValid() && Object->TryGetArrayField(FieldName, Values) && Values ? Values->Num() : 0;
    }

    // Returns true if CompileJson's "issues" array contains an object whose "code"
    // field equals ExpectedCode.
    inline bool IssuesContainCode(const TSharedPtr<FJsonObject>& CompileJson, const TCHAR* ExpectedCode)
    {
        if (!CompileJson.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
        if (!CompileJson->TryGetArrayField(TEXT("issues"), Issues) || !Issues)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Issues)
        {
            const TSharedPtr<FJsonObject> IssueObj = Value.IsValid() ? Value->AsObject() : nullptr;
            if (!IssueObj.IsValid())
            {
                continue;
            }
            FString Code;
            if (IssueObj->TryGetStringField(TEXT("code"), Code) && Code == ExpectedCode)
            {
                return true;
            }
        }
        return false;
    }
}
