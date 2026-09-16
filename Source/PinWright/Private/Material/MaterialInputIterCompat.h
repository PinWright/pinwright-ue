// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// ForEachExpressionInput — version-compat wrapper that hides the FExpressionInputIterator
// guard that would otherwise repeat in every caller.
//
// FExpressionInputIterator was added to Materials/MaterialExpression.h in UE 5.5.
// On 5.4 the iterator does not exist, and the GetInput(Index) loop the iterator is built on
// is NOT a safe fallback: 5.4's UMaterialExpression::GetInput(Index) indexes its input array
// unchecked and asserts (Array.h "(Index >= 0) & (Index < ArrayNum)") once Index passes the
// last input, rather than returning null like 5.5+. So on 5.4 we iterate the bounded
// GetInputsView() array instead, which can never run off the end.
//
// Usage:
//   ForEachExpressionInput(Expr, [&](FExpressionInput* Input, int32 Index) -> bool
//   {
//       // ... process Input / Index ...
//       return false; // continue; return true to break early
//   });
//
// The lambda must return bool: false == continue, true == break early.

#include "CoreMinimal.h"
#include "Misc/EngineVersionComparison.h"
#include "Materials/MaterialExpression.h"
#include "MaterialExpressionIO.h"

template <typename Fn>
void ForEachExpressionInput(UMaterialExpression* Expression, Fn Func)
{
    if (!Expression)
    {
        return;
    }

#if UE_VERSION_OLDER_THAN(5, 5, 0)
    // 5.4: no FExpressionInputIterator, and GetInput(Index) asserts past the last input.
    // Iterate the bounded input array view; GetInputsView() exists on 5.4.
    const TArrayView<FExpressionInput*> Inputs = Expression->GetInputsView();
    for (int32 Index = 0; Index < Inputs.Num(); ++Index)
    {
        FExpressionInput* Input = Inputs[Index];
        if (!Input)
        {
            continue;
        }
        if (Func(Input, Index))
        {
            break;
        }
    }
#else
    for (FExpressionInputIterator It{ Expression }; It; ++It)
    {
        if (Func(It.Input, It.Index))
        {
            break;
        }
    }
#endif
}
