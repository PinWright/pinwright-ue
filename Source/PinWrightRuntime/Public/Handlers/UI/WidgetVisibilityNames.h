// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Components/SlateWrapperTypes.h"

// ESlateVisibility <-> canonical name. Single source for the runtime ui.* verbs and the editor
// module's WidgetAuthoringHelpers::TryParseVisibility / VisibilityToString, which delegate here.
namespace WidgetVisibilityNames
{
    // Strict: false (Out untouched) unless In is one of the five canonical names, case-insensitive.
    inline bool TryParse(const FString& In, ESlateVisibility& Out)
    {
        struct FEntry { const TCHAR* Name; ESlateVisibility Value; };
        static const FEntry Table[] = {
            { TEXT("Visible"),              ESlateVisibility::Visible },
            { TEXT("Collapsed"),            ESlateVisibility::Collapsed },
            { TEXT("Hidden"),               ESlateVisibility::Hidden },
            { TEXT("HitTestInvisible"),     ESlateVisibility::HitTestInvisible },
            { TEXT("SelfHitTestInvisible"), ESlateVisibility::SelfHitTestInvisible },
        };
        for (const FEntry& E : Table)
        {
            if (In.Equals(E.Name, ESearchCase::IgnoreCase))
            {
                Out = E.Value;
                return true;
            }
        }
        return false;
    }

    inline FString ToString(ESlateVisibility Visibility)
    {
        switch (Visibility)
        {
        case ESlateVisibility::Collapsed:
            return TEXT("Collapsed");
        case ESlateVisibility::Hidden:
            return TEXT("Hidden");
        case ESlateVisibility::HitTestInvisible:
            return TEXT("HitTestInvisible");
        case ESlateVisibility::SelfHitTestInvisible:
            return TEXT("SelfHitTestInvisible");
        case ESlateVisibility::Visible:
        default:
            return TEXT("Visible");
        }
    }
}
