// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// Shared response contract for asset mutators that accept more than one item. The handler owns
// preflight and the operation itself; this helper owns the part that used to diverge between
// handlers: preserving every input row, distinguishing not-attempted from runtime failure, and
// deriving aggregate counts from the completed rows rather than a filtered work array.

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace PinWrightAssetBatch
{
    enum class EItemState : uint8
    {
        Pending,
        Ready,
        Succeeded,
        Failed
    };

    struct FItemOutcome
    {
        TSharedPtr<FJsonObject> Data;
        EItemState State = EItemState::Pending;
    };

    class FAssetBatchResult
    {
    public:
        explicit FAssetBatchResult(bool bInAllowPartial)
            : bAllowPartial(bInAllowPartial)
        {
        }

        int32 AddInput(const TSharedPtr<FJsonValue>& Input)
        {
            const int32 Index = AddItem();
            if (Input.IsValid())
            {
                Items[Index].Data->SetField(TEXT("input"), Input);
            }
            else
            {
                Items[Index].Data->SetField(TEXT("input"), MakeShared<FJsonValueNull>());
            }
            return Index;
        }

        int32 AddItem()
        {
            FItemOutcome& Item = Items.AddDefaulted_GetRef();
            Item.Data = MakeShared<FJsonObject>();
            Item.Data->SetBoolField(TEXT("ok"), false);
            Item.Data->SetBoolField(TEXT("attempted"), false);
            Item.Data->SetStringField(TEXT("error"), FString());
            Item.Data->SetStringField(TEXT("code"), FString());
            return Items.Num() - 1;
        }

        TSharedPtr<FJsonObject> GetItem(int32 Index)
        {
            return Items.IsValidIndex(Index) ? Items[Index].Data : nullptr;
        }

        void MarkReady(int32 Index)
        {
            if (Items.IsValidIndex(Index) && Items[Index].State == EItemState::Pending)
            {
                Items[Index].State = EItemState::Ready;
            }
        }

        bool IsReady(int32 Index) const
        {
            return Items.IsValidIndex(Index) && Items[Index].State == EItemState::Ready;
        }

        bool IsSucceeded(int32 Index) const
        {
            return Items.IsValidIndex(Index) && Items[Index].State == EItemState::Succeeded;
        }

        void MarkSucceeded(int32 Index, bool bAttempted = true)
        {
            if (!Items.IsValidIndex(Index))
            {
                return;
            }

            FItemOutcome& Item = Items[Index];
            Item.State = EItemState::Succeeded;
            Item.Data->SetBoolField(TEXT("ok"), true);
            Item.Data->SetBoolField(TEXT("attempted"), bAttempted);
            Item.Data->SetStringField(TEXT("error"), FString());
            Item.Data->SetStringField(TEXT("code"), FString());
        }

        void FailPreflight(int32 Index, const FString& Code, const FString& Error)
        {
            SetFailure(Index, Code, Error, /*bAttempted=*/false);
            if (Items.IsValidIndex(Index))
            {
                bHasPreflightFailure = true;
            }
        }

        void FailRuntime(int32 Index, const FString& Code, const FString& Error)
        {
            SetFailure(Index, Code, Error, /*bAttempted=*/true);
        }

        bool HasPreflightFailure() const
        {
            return bHasPreflightFailure;
        }

        bool ShouldRefuseBeforeMutation() const
        {
            return !bAllowPartial && bHasPreflightFailure;
        }

        void RefuseReadyItems(const FString& Code, const FString& Error)
        {
            for (int32 Index = 0; Index < Items.Num(); ++Index)
            {
                if (Items[Index].State == EItemState::Ready)
                {
                    SetFailure(Index, Code, Error, /*bAttempted=*/false);
                }
            }
        }

        bool HasReadyItems() const
        {
            for (const FItemOutcome& Item : Items)
            {
                if (Item.State == EItemState::Ready)
                {
                    return true;
                }
            }
            return false;
        }

        int32 NumRequested() const
        {
            return Items.Num();
        }

        int32 NumSucceeded() const
        {
            int32 Count = 0;
            for (const FItemOutcome& Item : Items)
            {
                Count += Item.State == EItemState::Succeeded ? 1 : 0;
            }
            return Count;
        }

        int32 NumAttempted() const
        {
            int32 Count = 0;
            for (const FItemOutcome& Item : Items)
            {
                bool bAttempted = false;
                if (Item.Data.IsValid())
                {
                    Item.Data->TryGetBoolField(TEXT("attempted"), bAttempted);
                }
                Count += bAttempted ? 1 : 0;
            }
            return Count;
        }

        bool IsEnvelopeSuccess() const
        {
            const int32 Succeeded = NumSucceeded();
            const int32 Failed = Items.Num() - Succeeded;
            return Items.Num() > 0
                && (Failed == 0 || (bAllowPartial && Succeeded > 0));
        }

        TSharedPtr<FJsonObject> MakeResult() const
        {
            TArray<TSharedPtr<FJsonValue>> JsonItems;
            JsonItems.Reserve(Items.Num());
            for (const FItemOutcome& Item : Items)
            {
                JsonItems.Add(MakeShared<FJsonValueObject>(Item.Data));
            }

            const int32 Succeeded = NumSucceeded();
            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("success"), IsEnvelopeSuccess());
            Result->SetBoolField(TEXT("partial"), bAllowPartial);
            Result->SetNumberField(TEXT("requested"), Items.Num());
            Result->SetNumberField(TEXT("attempted"), NumAttempted());
            Result->SetNumberField(TEXT("succeeded"), Succeeded);
            Result->SetNumberField(TEXT("failed"), Items.Num() - Succeeded);
            Result->SetArrayField(TEXT("items"), JsonItems);
            return Result;
        }

    private:
        void SetFailure(int32 Index, const FString& Code, const FString& Error,
                        bool bAttempted)
        {
            if (!Items.IsValidIndex(Index))
            {
                return;
            }

            FItemOutcome& Item = Items[Index];
            Item.State = EItemState::Failed;
            Item.Data->SetBoolField(TEXT("ok"), false);
            Item.Data->SetBoolField(TEXT("attempted"), bAttempted);
            Item.Data->SetStringField(TEXT("error"), Error);
            Item.Data->SetStringField(TEXT("code"), Code);
        }

        TArray<FItemOutcome> Items;
        bool bAllowPartial = false;
        bool bHasPreflightFailure = false;
    };
}
