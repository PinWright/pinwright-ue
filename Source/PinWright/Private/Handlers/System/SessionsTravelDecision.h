// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/PackageName.h"
#include "Templates/Function.h"

namespace PinWrightSessionTravel
{
    struct FServerTravelPendingState
    {
        FString URL;
        bool bSeamlessTravelInProgress = false;

        bool IsPending() const
        {
            return !URL.IsEmpty() || bSeamlessTravelInProgress;
        }
    };

    struct FServerTravelOutcome
    {
        bool bAccepted = false;
        bool bQueued = false;
        bool bCompleted = false;

        bool IsRefused() const
        {
            return !bAccepted || !bQueued;
        }
    };

    enum class EServerTravelCompletionState : uint8
    {
        Waiting,
        Completed,
        ContextLost,
    };

    struct FServerTravelCompletionObservation
    {
        FName InitiatingContextHandle;
        FName ObservedContextHandle;
        uint32 InitialWorldUniqueID = 0;
        uint32 ObservedWorldUniqueID = 0;
        FString RequestedMapPackagePath;
        FString ObservedMapPackagePath;
        bool bWorldAvailable = false;
        bool bTravelPending = false;
    };

    inline EServerTravelCompletionState EvaluateServerTravelCompletionState(
        const FServerTravelCompletionObservation& Observation)
    {
        if (Observation.InitiatingContextHandle.IsNone()
            || Observation.ObservedContextHandle != Observation.InitiatingContextHandle)
        {
            return EServerTravelCompletionState::ContextLost;
        }

        if (!Observation.bWorldAvailable
            || Observation.ObservedWorldUniqueID == Observation.InitialWorldUniqueID
            || Observation.bTravelPending)
        {
            return EServerTravelCompletionState::Waiting;
        }

        return Observation.ObservedMapPackagePath.Equals(
                Observation.RequestedMapPackagePath, ESearchCase::IgnoreCase)
            ? EServerTravelCompletionState::Completed
            : EServerTravelCompletionState::Waiting;
    }

    struct FServerTravelTerminalResponseGate
    {
        bool TryRespond(TFunctionRef<void()> Response)
        {
            if (bResponseSent)
            {
                return false;
            }

            bResponseSent = true;
            Response();
            return true;
        }

    private:
        bool bResponseSent = false;
    };

    inline FString ExtractTravelDestination(const FString& URL)
    {
        int32 DelimiterIndex = URL.Len();
        int32 CandidateIndex = INDEX_NONE;
        if (URL.FindChar(TEXT('?'), CandidateIndex))
        {
            DelimiterIndex = FMath::Min(DelimiterIndex, CandidateIndex);
        }
        if (URL.FindChar(TEXT('#'), CandidateIndex))
        {
            DelimiterIndex = FMath::Min(DelimiterIndex, CandidateIndex);
        }
        return FPackageName::ObjectPathToPackageName(URL.Left(DelimiterIndex));
    }

    inline FServerTravelOutcome EvaluateServerTravelOutcome(
        const bool bServerTravelAccepted,
        const FServerTravelPendingState& PendingBefore,
        const FServerTravelPendingState& PendingAfter,
        const FString& RequestedURL,
        const bool bDestinationObserved)
    {
        const bool bRequestedURLPending = !PendingAfter.URL.IsEmpty()
            && ExtractTravelDestination(PendingAfter.URL).Equals(
                ExtractTravelDestination(RequestedURL), ESearchCase::IgnoreCase);
        const bool bNewSeamlessTravelStarted = !PendingBefore.bSeamlessTravelInProgress
            && PendingAfter.bSeamlessTravelInProgress;
        const bool bNewRequestedTravelQueued = bServerTravelAccepted
            && !PendingBefore.IsPending()
            && (bRequestedURLPending || bNewSeamlessTravelStarted);
        return {
            bServerTravelAccepted,
            bNewRequestedTravelQueued,
            bNewRequestedTravelQueued && bDestinationObserved,
        };
    }
}
