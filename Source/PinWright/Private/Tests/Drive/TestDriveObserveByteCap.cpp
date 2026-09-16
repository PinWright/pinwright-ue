// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the drive.observe element-list byte-aware truncation
// (E-drive-observe-element-list-no-projection-spills). Each drive element's `handle` is the full
// Slate/UMG widget-tree path, so interactables_only + max_elements bound element COUNT but not
// payload BYTES: a handful of deeply-nested editor_chrome elements still overflow the 10000-char
// inline budget and spill to Saved/PinWright/HttpResponses. The fix adds a byte budget (max_bytes)
// that caps the total serialized element size, reusing the existing omitted_count machinery.
//
// This exercises the production filter FDriveHandlerCommon::FilterObservationElements plus the
// FDriveJson::WriteObservation serialization seam with an in-code synthetic observation whose
// elements carry long verbose handles (no live viewport, no example asset), so it fails if the
// byte cap is reverted (the "capped" observation balloons back over budget). It also pins the
// backward-compatible no-op: MaxBytes=0 drops nothing.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Handlers/Drive/DriveHandlerCommon.h"
#include "Handlers/Drive/DriveJson.h"
#include "Handlers/Drive/DriveTypes.h"
#include "Utils/HttpResponseSpill.h"

namespace
{
    // A synthetic editor_chrome-style element whose Handle is a long widget-tree path - the field
    // that dominates a real drive.observe payload. Index keeps each handle/label distinct and the
    // path ~30 segments deep, matching the deeply-nested editor Slate paths in the ticket.
    FDriveElement MakeVerboseElement(int32 Index)
    {
        FString Handle;
        for (int32 Seg = 0; Seg < 30; ++Seg)
        {
            Handle += FString::Printf(TEXT("SDockingTabStack%d[%d]/"), Seg, Index % 4);
        }
        Handle += FString::Printf(TEXT("SButton[%d]"), Index);

        FDriveElement Element;
        Element.Handle = Handle;
        Element.Path = Handle;
        Element.Type = TEXT("SButton");
        Element.Label = FString::Printf(TEXT("Button%d"), Index);
        Element.bInteractable = true;
        Element.Surface = EDriveSurface::EditorChrome;
        return Element;
    }

    TArray<FDriveElement> MakeVerboseElements(int32 Count)
    {
        TArray<FDriveElement> Elements;
        Elements.Reserve(Count);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Elements.Add(MakeVerboseElement(Index));
        }
        return Elements;
    }

    // Serialize an observation exactly as callers receive it (WriteObservation -> JSON string) and
    // return the wire-string length, so the assertions measure the real payload, not an estimate.
    int32 SerializedLen(const FDriveObservation& Obs)
    {
        const TSharedPtr<FJsonObject> Root = FDriveJson::WriteObservation(Obs);
        if (!Root.IsValid())
        {
            return -1;
        }
        FString Out;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
        return Out.Len();
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveObserveElementByteCapTest,
    "PinWright.drive.observe.ElementListByteCapBoundsPayload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveObserveElementByteCapTest::RunTest(const FString& Parameters)
{
    constexpr int32 ElementCount = 60;
    constexpr int32 ByteBudget = 8000;

    // The inline display budget drive.observe overflows in the field report; the byte cap must
    // bring a capped observation under it. Derived from the single-source spill threshold rather
    // than hardcoded, so this assertion tracks the real clamp if DefaultThresholdCharacters moves.
    const int32 InlineBudget = HttpResponseSpill::GetDefaultThresholdCharacters();

    // --- Baseline: no byte cap (MaxBytes=0) keeps every element and overflows the inline budget. ---
    // This is the reported defect: even with the count caps, the verbose handle list spills.
    {
        FDriveObservation Obs;
        Obs.Surface = EDriveSurface::EditorChrome;
        Obs.Elements = MakeVerboseElements(ElementCount);
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(
            Obs.Elements, /*bInteractablesOnly=*/false, /*MaxElements=*/0, /*MaxBytes=*/0, Omitted);
        Obs.OmittedCount = Omitted;

        TestEqual(TEXT("no byte cap keeps every element"), Obs.Elements.Num(), ElementCount);
        TestEqual(TEXT("no byte cap omits nothing"), Omitted, 0);
        TestTrue(TEXT("uncapped verbose element list overflows the inline budget (the reported spill)"),
            SerializedLen(Obs) > InlineBudget);
    }

    // --- Byte cap: MaxBytes bounds the serialized element payload and reports the drop. ---
    {
        FDriveObservation Obs;
        Obs.Surface = EDriveSurface::EditorChrome;
        Obs.Elements = MakeVerboseElements(ElementCount);
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(
            Obs.Elements, /*bInteractablesOnly=*/false, /*MaxElements=*/0, /*MaxBytes=*/ByteBudget, Omitted);
        Obs.OmittedCount = Omitted;

        TestTrue(TEXT("byte cap drops elements"), Obs.Elements.Num() < ElementCount);
        TestTrue(TEXT("byte cap keeps at least one element"), Obs.Elements.Num() >= 1);
        TestEqual(TEXT("omitted_count accounts for every dropped element"),
            Omitted, ElementCount - Obs.Elements.Num());

        // The serialized observation must stay within the budget plus the fixed root overhead
        // (surface/root_name/frame/timestamp/omitted_count) - a small constant. This is the
        // property the fix guarantees and that a revert (no byte cap) would violate.
        const int32 CappedLen = SerializedLen(Obs);
        TestTrue(TEXT("capped observation serializes within budget + root overhead"),
            CappedLen <= ByteBudget + 512);
        TestTrue(TEXT("capped observation now fits the inline budget"), CappedLen <= InlineBudget);
    }

    // --- Guard: a single over-budget element is still returned (never an empty list). ---
    {
        TArray<FDriveElement> Elements;
        Elements.Add(MakeVerboseElement(0));
        int32 Omitted = -1;
        FDriveHandlerCommon::FilterObservationElements(
            Elements, /*bInteractablesOnly=*/false, /*MaxElements=*/0, /*MaxBytes=*/1, Omitted);
        TestEqual(TEXT("a lone over-budget element is kept, not dropped"), Elements.Num(), 1);
        TestEqual(TEXT("keeping the lone element omits nothing"), Omitted, 0);
    }

    return true;
}
