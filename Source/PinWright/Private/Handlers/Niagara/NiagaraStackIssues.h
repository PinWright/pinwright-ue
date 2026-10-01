// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"

namespace PinWrightNiagara
{
    // Which of an engine stack issue's fixes niagara.apply_issue_fix may execute. A non-empty
    // FixId names the fix; an empty one selects the issue's only applicable (Fix-style, bound)
    // fix. Returns nullptr with OutIndex set on success, otherwise the error code to send
    // (FIX_NOT_FOUND / FIX_IS_LINK / FIX_AMBIGUOUS) with OutIndex = INDEX_NONE.
    const TCHAR* SelectStackIssueFix(
        const UNiagaraStackEntry::FStackIssue& Issue,
        const FString& FixId,
        int32& OutIndex);
}
