// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared text-emitter formatting helpers for the Asset/ handler cluster.
// Extracted from per-emitter anonymous namespaces of StaticMeshTextEmitter.cpp
// and TextureTextEmitter.cpp so Unity build can merge those TUs without tripping
// C2084 on identical FormatNumber / AppendIndent / AppendScalar / AppendOptionalString /
// AppendOptionalNumber definitions.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/SharedPointer.h"

namespace AssetTextEmitterHelpers
{
    inline FString FormatNumber(double Value)
    {
        const double Rounded = FMath::RoundToDouble(Value);
        if (FMath::IsNearlyEqual(Value, Rounded, KINDA_SMALL_NUMBER))
        {
            return FString::Printf(TEXT("%lld"), static_cast<int64>(Rounded));
        }
        return FString::SanitizeFloat(Value);
    }

    inline void AppendIndent(FString& Out, int32 Indent)
    {
        for (int32 Index = 0; Index < Indent; ++Index)
        {
            Out += TEXT("  ");
        }
    }

    inline void AppendScalar(FString& Out, int32 Indent, const TCHAR* Field, const FString& Value)
    {
        AppendIndent(Out, Indent);
        Out += Field;
        Out += TEXT(": ");
        Out += Value;
        Out += TEXT("\n");
    }

    inline void AppendOptionalString(FString& Out, int32 Indent, const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        FString Value;
        if (Object.IsValid() && Object->TryGetStringField(Field, Value))
        {
            AppendScalar(Out, Indent, Field, Value.IsEmpty() ? TEXT("None") : Value);
        }
    }

    inline void AppendOptionalNumber(FString& Out, int32 Indent, const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
    {
        double Value = 0.0;
        if (Object.IsValid() && Object->TryGetNumberField(Field, Value))
        {
            AppendScalar(Out, Indent, Field, FormatNumber(Value));
        }
    }
}
