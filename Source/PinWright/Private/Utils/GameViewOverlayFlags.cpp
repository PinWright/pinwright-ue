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
        { TEXT("editor"),           &FGameViewOverlayShowFlags::bEditor },
        { TEXT("selection"),        &FGameViewOverlayShowFlags::bSelection },
        { TEXT("grid"),             &FGameViewOverlayShowFlags::bGrid },
        { TEXT("volumes"),          &FGameViewOverlayShowFlags::bVolumes },
        { TEXT("lightRadius"),      &FGameViewOverlayShowFlags::bLightRadius },
        { TEXT("audioRadius"),      &FGameViewOverlayShowFlags::bAudioRadius },
        { TEXT("navigation"),       &FGameViewOverlayShowFlags::bNavigation },
    };
}

FGameViewOverlayShowFlags PinWrightReadGameViewOverlayFlags(const FEngineShowFlags& Flags)
{
    FGameViewOverlayShowFlags State;
    State.bSplines = Flags.Splines != 0;
    State.bEditor = Flags.Editor != 0;
    State.bSelection = Flags.Selection != 0;
    State.bGrid = Flags.Grid != 0;
    State.bVolumes = Flags.Volumes != 0;
    State.bLightRadius = Flags.LightRadius != 0;
    State.bAudioRadius = Flags.AudioRadius != 0;
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

const TCHAR* PinWrightGameViewOverlayRemedy()
{
    return TEXT("Toggling game view does not clear it: once a game flag set exists, "
                "editor.set_game_view restores that stored set, flag included. Clear the flag on the "
                "level viewport itself (its Show menu; `navigation` is also the P key) - no PinWright "
                "verb writes a viewport show flag. For a scripted shot, system.console_command "
                "\"ShowFlag.<Name> 0\" (for example ShowFlag.Splines 0) forces it off in every view; "
                "it is process-global, viewport.showFlagOverrides on a capture reports it, and "
                "\"ShowFlag.<Name> 2\" restores it afterwards. A capture's overlay flags include "
                "that override; editor.set_game_view reads the viewport's own flags, which still "
                "read set.");
}
