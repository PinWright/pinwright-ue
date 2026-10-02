// Copyright (c) 2026 Alexander Penkin. MIT License.

// FDriveInput::PressKey with modifiers sends a real chord (board B-drive-key-shift-f1-arrives-bare):
// the modifier's own key-down before the key and its key-up after, with the modifier set on the
// key event's flags and, on Linux, held in the platform modifier state Slate's GetModifierKeys()
// reads. Before the fix only the event flags carried it, so a PIE viewport that reads held-key
// state saw Shift+F1 as a bare F1. A front-of-queue Slate input preprocessor records and consumes
// every key event, so nothing in the editor acts on the injected keys.
//
// The platform half is host-dependent: under -RenderOffScreen the Linux platform application is
// FNullApplication (LinuxPlatformApplicationMisc.cpp CreateApplication), whose GetModifierKeys()
// is always empty, and Windows keeps its modifier state private. RealKeyDowns asserts on every
// host that the reported flag matches what the platform answered; PlatformStateHeld requires the
// hold where the engine runs an SDL platform application and skips with the marker elsewhere.

#include "Misc/AutomationTest.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tests/TestSkipReporting.h"

#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Handlers/Drive/DriveInput.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Misc/ScopeExit.h"

namespace DriveInputModifierChordTest
{
    struct FRecordedKey
    {
        FKey Key;
        bool bDown = false;
        FModifierKeysState EventModifiers;
        FModifierKeysState PlatformModifiers;
    };

    class FRecordKeysProcessor : public IInputProcessor
    {
    public:
        virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

        virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
        {
            Keys.Add({ Event.GetKey(), true, Event.GetModifierKeys(), SlateApp.GetModifierKeys() });
            return true;
        }

        virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
        {
            Keys.Add({ Event.GetKey(), false, Event.GetModifierKeys(), SlateApp.GetModifierKeys() });
            return true;
        }

        TArray<FRecordedKey> Keys;
    };

    bool IsHeld(const FModifierKeysState& State, EDriveModifierKeys Modifier)
    {
        switch (Modifier)
        {
            case EDriveModifierKeys::Shift: return State.IsShiftDown();
            case EDriveModifierKeys::Ctrl:  return State.IsControlDown();
            case EDriveModifierKeys::Alt:   return State.IsAltDown();
            default:                        return false;
        }
    }

    // Whether the engine runs a platform application that answers GetModifierKeys() from SDL:
    // the same test FLinuxPlatformApplicationMisc::CreateApplication makes.
    bool PlatformCanHoldModifiers()
    {
#if PLATFORM_LINUX
        return !FParse::Param(FCommandLine::Get(), TEXT("RenderOffScreen"));
#else
        return false;
#endif
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveInputModifierChordRealKeyDownsTest,
    "PinWright.drive.input.ModifierChordRealKeyDowns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveInputModifierChordRealKeyDownsTest::RunTest(const FString& Parameters)
{
    using namespace DriveInputModifierChordTest;

    if (!FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Slate is not initialized; the chord cannot be injected."));
        return false;
    }
    FSlateApplication& SlateApp = FSlateApplication::Get();
    const TSharedRef<FRecordKeysProcessor> Recorder = MakeShared<FRecordKeysProcessor>();
    if (!TestTrue(TEXT("recording preprocessor registers"), SlateApp.RegisterInputPreProcessor(Recorder, 0)))
    {
        return false;
    }
    ON_SCOPE_EXIT { SlateApp.UnregisterInputPreProcessor(Recorder); };

    struct FCase { EDriveModifierKeys Modifier; FKey ModifierKey; FKey Key; };
    const FCase Cases[] = {
        { EDriveModifierKeys::Shift, EKeys::LeftShift,   EKeys::F1 },
        { EDriveModifierKeys::Ctrl,  EKeys::LeftControl, EKeys::F2 },
        { EDriveModifierKeys::Alt,   EKeys::LeftAlt,     EKeys::F3 },
    };

    for (const FCase& Case : Cases)
    {
        const FString Label = FString::Printf(TEXT("%s+%s"), *Case.ModifierKey.ToString(), *Case.Key.ToString());
        Recorder->Keys.Reset();
        bool bPlatformHeld = true;
        TestTrue(Label + TEXT(": injected"), FDriveInput::PressKey(Case.Key, Case.Modifier, EDriveKeyAction::Press, &bPlatformHeld));

        // Modifier down, key down, key up, modifier up: the order a keyboard sends a chord in.
        const TArray<FRecordedKey>& Keys = Recorder->Keys;
        if (!TestEqual(Label + TEXT(": four key edges"), Keys.Num(), 4))
        {
            continue;
        }
        TestTrue(Label + TEXT(": modifier key-down first"), Keys[0].Key == Case.ModifierKey && Keys[0].bDown);
        TestTrue(Label + TEXT(": key-down second"), Keys[1].Key == Case.Key && Keys[1].bDown);
        TestTrue(Label + TEXT(": key-up third"), Keys[2].Key == Case.Key && !Keys[2].bDown);
        TestTrue(Label + TEXT(": modifier key-up last"), Keys[3].Key == Case.ModifierKey && !Keys[3].bDown);

        TestTrue(Label + TEXT(": key-down event carries the modifier"), IsHeld(Keys[1].EventModifiers, Case.Modifier));
        TestTrue(Label + TEXT(": modifier key-down reports itself held"), IsHeld(Keys[0].EventModifiers, Case.Modifier));
        TestFalse(Label + TEXT(": modifier key-up reports itself released"), IsHeld(Keys[3].EventModifiers, Case.Modifier));
        // Honest on every host: the reported flag is what the platform answered at the key-down.
        TestEqual(Label + TEXT(": reported platform hold matches the platform's answer at the key-down"),
            bPlatformHeld, IsHeld(Keys[1].PlatformModifiers, Case.Modifier));
        TestFalse(Label + TEXT(": platform modifier state released after the chord"),
            IsHeld(SlateApp.GetModifierKeys(), Case.Modifier));
    }

    // Split edges: action down leaves the modifier held, action up releases it after the key.
    Recorder->Keys.Reset();
    FDriveInput::PressKey(EKeys::F4, EDriveModifierKeys::Ctrl | EDriveModifierKeys::Shift, EDriveKeyAction::Down);
    if (TestEqual(TEXT("down edge: ctrl, shift, key"), Recorder->Keys.Num(), 3))
    {
        TestTrue(TEXT("down edge: ctrl first"), Recorder->Keys[0].Key == EKeys::LeftControl && Recorder->Keys[0].bDown);
        TestTrue(TEXT("down edge: shift second"), Recorder->Keys[1].Key == EKeys::LeftShift && Recorder->Keys[1].bDown);
        TestTrue(TEXT("down edge: key last"), Recorder->Keys[2].Key == EKeys::F4 && Recorder->Keys[2].bDown);
    }
    Recorder->Keys.Reset();
    FDriveInput::PressKey(EKeys::F4, EDriveModifierKeys::Ctrl | EDriveModifierKeys::Shift, EDriveKeyAction::Up);
    if (TestEqual(TEXT("up edge: key, shift, ctrl"), Recorder->Keys.Num(), 3))
    {
        TestTrue(TEXT("up edge: key first"), Recorder->Keys[0].Key == EKeys::F4 && !Recorder->Keys[0].bDown);
        TestTrue(TEXT("up edge: shift second"), Recorder->Keys[1].Key == EKeys::LeftShift && !Recorder->Keys[1].bDown);
        TestTrue(TEXT("up edge: ctrl last"), Recorder->Keys[2].Key == EKeys::LeftControl && !Recorder->Keys[2].bDown);
    }
    TestFalse(TEXT("up edge: platform shift released"), SlateApp.GetModifierKeys().IsShiftDown());
    TestFalse(TEXT("up edge: platform ctrl released"), SlateApp.GetModifierKeys().IsControlDown());

    // No modifiers: a bare key is exactly one down and one up.
    Recorder->Keys.Reset();
    FDriveInput::PressKey(EKeys::F5);
    TestEqual(TEXT("bare key: two edges"), Recorder->Keys.Num(), 2);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveInputModifierChordPlatformStateHeldTest,
    "PinWright.drive.input.ModifierChordPlatformStateHeld",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveInputModifierChordPlatformStateHeldTest::RunTest(const FString& Parameters)
{
    using namespace DriveInputModifierChordTest;

    if (!PlatformCanHoldModifiers())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("platform-modifier-state-unavailable"),
            TEXT("the platform application does not answer GetModifierKeys() from SDL (Linux -RenderOffScreen runs FNullApplication; Windows keeps its state private); run windowed on Linux to measure the platform hold"));
        return true;
    }
    if (!FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Slate is not initialized; the chord cannot be injected."));
        return false;
    }
    FSlateApplication& SlateApp = FSlateApplication::Get();
    const TSharedRef<FRecordKeysProcessor> Recorder = MakeShared<FRecordKeysProcessor>();
    if (!TestTrue(TEXT("recording preprocessor registers"), SlateApp.RegisterInputPreProcessor(Recorder, 0)))
    {
        return false;
    }
    ON_SCOPE_EXIT { SlateApp.UnregisterInputPreProcessor(Recorder); };

    const EDriveModifierKeys Modifiers[] = { EDriveModifierKeys::Shift, EDriveModifierKeys::Ctrl, EDriveModifierKeys::Alt };
    for (const EDriveModifierKeys Modifier : Modifiers)
    {
        const FString Label = FString::Printf(TEXT("modifier %d"), static_cast<int32>(Modifier));
        Recorder->Keys.Reset();
        bool bPlatformHeld = false;
        FDriveInput::PressKey(EKeys::F1, Modifier, EDriveKeyAction::Press, &bPlatformHeld);
        TestTrue(Label + TEXT(": reported held"), bPlatformHeld);
        if (TestEqual(Label + TEXT(": four key edges"), Recorder->Keys.Num(), 4))
        {
            TestTrue(Label + TEXT(": platform state held during the key-down"), IsHeld(Recorder->Keys[1].PlatformModifiers, Modifier));
        }
        TestFalse(Label + TEXT(": platform state released after the chord"), IsHeld(SlateApp.GetModifierKeys(), Modifier));
    }

    // Split edges: the hold outlives the down edge and ends with the up edge.
    FDriveInput::PressKey(EKeys::F4, EDriveModifierKeys::Shift, EDriveKeyAction::Down);
    TestTrue(TEXT("down edge: platform shift stays held"), SlateApp.GetModifierKeys().IsShiftDown());
    FDriveInput::PressKey(EKeys::F4, EDriveModifierKeys::Shift, EDriveKeyAction::Up);
    TestFalse(TEXT("up edge: platform shift released"), SlateApp.GetModifierKeys().IsShiftDown());

    return true;
}
