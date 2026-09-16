// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Shared shape for IR pin resolvers (MGIR, AGIR) that run the same
// parse-reference -> symbol-lookup -> wire-pin triplet against per-IR node
// types. Concrete resolvers still own their own ParseReference (text grammars
// differ) and Wire* (target API differs); the base unifies the error envelope
// and the symbol-map lookup so callers handling multiple IR families see one
// return shape. TResolvedPin is the per-IR resolved-pin payload; the base does
// not touch it (it is named only for documentation and future helpers).
template <typename TNode, typename TResolvedPin>
class TIrPinResolverBase
{
public:
    // Error envelope shared across IR families. Empty ErrorCode == success.
    struct FWireResult
    {
        FString ErrorCode;
        FString ErrorMessage;

        bool IsSuccess() const { return ErrorCode.IsEmpty(); }
    };

protected:
    static FWireResult MakeError(const TCHAR* Code, FString Message)
    {
        FWireResult Result;
        Result.ErrorCode = Code;
        Result.ErrorMessage = MoveTemp(Message);
        return Result;
    }

    static TNode* LookupSymbol(const TMap<FString, TNode*>& Symbols, const FString& Name)
    {
        TNode* const* Found = Symbols.Find(Name);
        return Found ? *Found : nullptr;
    }

    static FWireResult SymbolNotFound(const TCHAR* Code, const FString& Name)
    {
        return MakeError(Code, FString::Printf(TEXT("Symbol '%%%s' was not found."), *Name));
    }
};
