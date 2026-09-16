// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/GameViewOverlayFlags.h"

#include "Dom/JsonObject.h"
#include "ShowFlags.h"

namespace
{
    // ONE table, walked by both the JSON writer and the "what is still drawing" description, so a
    // flag can never be published under one name and warned about under another - or, worse, be
    // published and left out of the warning.
    struct FPinWrightGameViewOverlayEntry
    {
        const TCHAR* Name;
        bool FGameViewOverlayShowFlags::* Member;
    };

    const FPinWrightGameViewOverlayEntry PinWrightGameViewOverlayEntries[] = {
        { TEXT("splines"),          &FGameViewOverlayShowFlags::bSplines },
        { TEXT("billboardSprites"), &FGameViewOverlayShowFlags::bBillboardSprites },
        { TEXT("selection"),        &FGameViewOverlayShowFlags::bSelection },
        { TEXT("selectionOutline"), &FGameViewOverlayShowFlags::bSelectionOutline },
        { TEXT("grid"),             &FGameViewOverlayShowFlags::bGrid },
        { TEXT("volumes"),          &FGameViewOverlayShowFlags::bVolumes },
        { TEXT("lightRadius"),      &FGameViewOverlayShowFlags::bLightRadius },
        { TEXT("audioRadius"),      &FGameViewOverlayShowFlags::bAudioRadius },
        { TEXT("modeWidgets"),      &FGameViewOverlayShowFlags::bModeWidgets },
        { TEXT("navigation"),       &FGameViewOverlayShowFlags::bNavigation },
    };
}

FGameViewOverlayShowFlags PinWrightReadGameViewOverlayFlags(const FEngineShowFlags& Flags)
{
    FGameViewOverlayShowFlags State;
    State.bSplines = Flags.Splines != 0;
    State.bBillboardSprites = Flags.BillboardSprites != 0;
    State.bSelection = Flags.Selection != 0;
    State.bSelectionOutline = Flags.SelectionOutline != 0;
    State.bGrid = Flags.Grid != 0;
    State.bVolumes = Flags.Volumes != 0;
    State.bLightRadius = Flags.LightRadius != 0;
    State.bAudioRadius = Flags.AudioRadius != 0;
    State.bModeWidgets = Flags.ModeWidgets != 0;
    State.bNavigation = Flags.Navigation != 0;
    State.bGame = Flags.Game != 0;
    return State;
}

void PinWrightAddGameViewOverlayFlags(const TSharedPtr<FJsonObject>& Out,
    const FGameViewOverlayShowFlags& State)
{
    for (const FPinWrightGameViewOverlayEntry& Entry : PinWrightGameViewOverlayEntries)
    {
        Out->SetBoolField(Entry.Name, State.*Entry.Member);
    }
    // Outside the table on purpose: it is not an overlay, so it must not reach the warning.
    Out->SetBoolField(TEXT("game"), State.bGame);
}

FString PinWrightDescribeVisibleGameViewOverlays(const FGameViewOverlayShowFlags& State)
{
    FString Names;
    for (const FPinWrightGameViewOverlayEntry& Entry : PinWrightGameViewOverlayEntries)
    {
        if (State.*Entry.Member)
        {
            if (!Names.IsEmpty())
            {
                Names += TEXT(", ");
            }
            Names += Entry.Name;
        }
    }
    return Names;
}
