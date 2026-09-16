// Copyright (c) 2026 Alexander Penkin. MIT License.

// Region honesty for landscape.get_heights — board
// B-landscape-get-heights-fabricates-out-of-extent-region.
//
// The defect: a region that reached past the landscape was answered as though it had not.
// LandscapeHeightStats::ResolveHeightRegion clamps all four coordinates into the extent, so a
// request lying WHOLLY past the far edge collapsed onto the single edge pixel and came back
// success:true with one sample. The engine then found no ULandscapeComponent for that vertex,
// filled the buffer by interpolation (FLandscapeEditDataInterface::CalcMissingValues) and left
// its four int32& out-params at the INT_MAX / INT_MIN seeds it opens with
// (LandscapeEditInterface.cpp:863, narrowed only inside `if (Component)` at :908-918, clamped
// back at :1291-1294 where Max(X1, INT_MAX) is still INT_MAX). The handler echoed those
// out-params, so `region` was published as {2147483647, 2147483647, -2147483648, -2147483648}
// and the neutral fill value 32768 as world Z 0 — a number that reads like flat ground at sea
// level and is in fact the absence of a measurement.
//
// A region PARTLY past the edge was the quieter half: it clamps to a real sub-rectangle, finds
// its components, and returns correct heights — with nothing anywhere in the response saying the
// window the caller asked for is not the window they got.
//
// Why the straddling case is the one that has to be asserted: a region wholly INSIDE the
// landscape behaves identically before and after the fix, so a test written on one passes today
// and measures nothing. Both cases below are written against the landscape's edge on purpose.
//
// Counterfactual: revert the bOverlapsExtent test in ResolveHeightRegion and RefusedWhollyOutside
// fails (the call succeeds with a one-pixel fabricated reading); revert the requestedRegion /
// warnings / omittedSampleCount reporting in the handler and StraddlingRegionReportsTheClamp
// fails on the first missing field.
//
// Fixture scope: the landscape is created in code through the production landscape.create
// handler, under a uniquely-labelled GUID name, inside FScopedEditorWorldActorGuard (which
// destroys what the test spawned and restores the level's dirty flag). Nothing on disk and no
// host content is touched.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "Misc/Guid.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

namespace PinWrightGetHeightsRegionTest
{
    // 1x1 components of 7 quads = 8x8 vertices, so the landscape's heightmap-pixel extent is
    // exactly [0,0]..[7,7] and any coordinate above 7 is off the far edge.
    constexpr int32 ExtentMax = 7;

    ALandscape* CreateTinyLandscape(FAutomationTestBase& Test, UWorld* World, const FString& Label)
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), Label);
        CreatePayload->SetNumberField(TEXT("componentsX"), 1);
        CreatePayload->SetNumberField(TEXT("componentsY"), 1);
        CreatePayload->SetNumberField(TEXT("quadsPerComponent"), ExtentMax);
        CreatePayload->SetNumberField(TEXT("sectionsPerComponent"), 1);

        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        if (!InvokeHandlerWithSharedCapture(TEXT("landscape.create"), CreatePayload, Capture))
        {
            Test.AddError(TEXT("landscape.create handler is not registered"));
            return nullptr;
        }
        PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/30.0);
        // The landscape IS the fixture: a create that did not succeed is a failure, not a skip.
        Test.TestTrue(TEXT("landscape.create fixture succeeded"), Capture->bSuccess);
        if (!Capture->bSuccess)
        {
            return nullptr;
        }

        for (TActorIterator<ALandscape> It(World); It; ++It)
        {
            if (It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase))
            {
                return *It;
            }
        }
        Test.AddError(TEXT("landscape.create reported success but no actor with that label exists"));
        return nullptr;
    }

    void ReadRegion(const FString& Label, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY,
        FTestResponseCapture& OutCapture, FAutomationTestBase& Test)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("landscapeName"), Label);
        TSharedPtr<FJsonObject> Region = MakeShared<FJsonObject>();
        Region->SetNumberField(TEXT("minX"), MinX);
        Region->SetNumberField(TEXT("minY"), MinY);
        Region->SetNumberField(TEXT("maxX"), MaxX);
        Region->SetNumberField(TEXT("maxY"), MaxY);
        Payload->SetObjectField(TEXT("region"), Region);

        Test.TestTrue(TEXT("landscape.get_heights handler is registered"),
            InvokeHandlerWithCapture(TEXT("landscape.get_heights"), Payload, OutCapture));
    }

    // Reads one coordinate out of a response sub-object, recording a failure when either the
    // object or the field is missing rather than silently substituting a default that would
    // make the assertion below pass for the wrong reason.
    bool GetRegionCoord(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Result,
        const TCHAR* ObjectField, const TCHAR* Coord, int32& OutValue)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Result.IsValid() || !Result->TryGetObjectField(ObjectField, Obj) || !Obj || !Obj->IsValid())
        {
            Test.AddError(FString::Printf(TEXT("response has no '%s' object"), ObjectField));
            return false;
        }
        if (!(*Obj)->TryGetNumberField(Coord, OutValue))
        {
            Test.AddError(FString::Printf(TEXT("'%s' has no '%s' field"), ObjectField, Coord));
            return false;
        }
        return true;
    }
}

// ============================================================================
// A region that lies ENTIRELY past the landscape is refused. It used to clamp onto the nearest
// edge pixel and be answered as a one-sample success whose heights the engine had invented.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeGetHeightsRefusedWhollyOutsideTest,
    "PinWright.landscape.get_heights.RefusedWhollyOutside",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeGetHeightsRefusedWhollyOutsideTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor has no editor world with a persistent level, so no landscape fixture can be created."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_GetHeightsOutside_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    if (!PinWrightGetHeightsRegionTest::CreateTinyLandscape(*this, World, Label))
    {
        return false;
    }

    // Every coordinate is past the extent max of 7, so there is no overlap at all.
    FTestResponseCapture Capture;
    PinWrightGetHeightsRegionTest::ReadRegion(Label, 20, 20, 40, 40, Capture, *this);

    TestFalse(TEXT("a region entirely outside the landscape is not answered as a successful read"),
        Capture.bSuccess);
    if (Capture.bSuccess)
    {
        // Name what came back instead, because the shape of the lie is the point of the ticket.
        int32 SampleCount = 0;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetNumberField(TEXT("sampleCount"), SampleCount);
        }
        AddError(FString::Printf(
            TEXT("get_heights reported success with sampleCount=%d for a region no part of which exists"),
            SampleCount));
        return false;
    }

    TestEqual(TEXT("the refusal is INVALID_ARGUMENT"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    // The caller has to be able to re-ask, so the extent it overshot is in the message.
    TestTrue(TEXT("the refusal names the landscape extent it lies outside"),
        Capture.Message.Contains(TEXT("extent")));

    return true;
}

// ============================================================================
// A region that STRADDLES the edge is read over its in-extent part only, and says so: the
// requested rectangle, the rectangle actually read, and the count of samples that were asked for
// and not measured all reach the caller. Before the fix the clamped rectangle was substituted
// silently and the response carried no warnings array at all.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeGetHeightsStraddlingRegionReportsClampTest,
    "PinWright.landscape.get_heights.StraddlingRegionReportsClamp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeGetHeightsStraddlingRegionReportsClampTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightGetHeightsRegionTest;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor has no editor world with a persistent level, so no landscape fixture can be created."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_GetHeightsStraddle_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    if (!CreateTinyLandscape(*this, World, Label))
    {
        return false;
    }

    // [4,4]..[20,20] on an extent of [0,0]..[7,7]: the low corner is inside, the high corner is
    // 13 pixels past the far edge. 17x17 = 289 samples asked for, 4x4 = 16 of them exist.
    constexpr int32 AskedMin = 4;
    constexpr int32 AskedMax = 20;
    constexpr int32 AskedSamples = (AskedMax - AskedMin + 1) * (AskedMax - AskedMin + 1);
    constexpr int32 ReadableSamples = (ExtentMax - AskedMin + 1) * (ExtentMax - AskedMin + 1);

    FTestResponseCapture Capture;
    ReadRegion(Label, AskedMin, AskedMin, AskedMax, AskedMax, Capture, *this);

    TestTrue(TEXT("a region overlapping the landscape is still read"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("get_heights failed: %s / %s"), *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    // The rectangle the caller asked for is echoed back unclamped — the half of the answer that
    // makes the clamp visible at all.
    int32 RequestedMaxX = 0;
    if (GetRegionCoord(*this, Capture.Result, TEXT("requestedRegion"), TEXT("maxX"), RequestedMaxX))
    {
        TestEqual(TEXT("requestedRegion echoes the unclamped request"), RequestedMaxX, AskedMax);
    }

    // ...and "region" is the rectangle that was actually read, which is not the same thing.
    int32 ReadMaxX = 0;
    if (GetRegionCoord(*this, Capture.Result, TEXT("region"), TEXT("maxX"), ReadMaxX))
    {
        TestEqual(TEXT("region is the sampled rectangle, clamped to the extent"), ReadMaxX, ExtentMax);
    }

    // The aggregates describe only what exists: sampleCount is the in-extent count, never the
    // count that was asked for.
    int32 SampleCount = 0;
    TestTrue(TEXT("response carries sampleCount"),
        Capture.Result->TryGetNumberField(TEXT("sampleCount"), SampleCount));
    TestEqual(TEXT("only the in-extent samples are counted"), SampleCount, ReadableSamples);

    // The count of what was asked for and not measured, so a caller assembling a heightmap can
    // see the hole rather than infer it from two rectangles.
    double OmittedSamples = 0.0;
    if (TestTrue(TEXT("response carries omittedSampleCount"),
            Capture.Result->TryGetNumberField(TEXT("omittedSampleCount"), OmittedSamples)))
    {
        TestEqual(TEXT("every sample past the edge is counted as omitted"),
            (int32)OmittedSamples, AskedSamples - ReadableSamples);
    }

    // And the clamp is stated in words, the way the sibling paint verb already states it.
    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    if (TestTrue(TEXT("a clamped read carries a warnings array"),
            Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings))
    {
        TestTrue(TEXT("the warnings array is not empty"), Warnings->Num() > 0);
    }

    return true;
}

// ============================================================================
// The control, and the trap the ticket names: a region wholly INSIDE the landscape must still be
// a plain successful read with nothing omitted and nothing warned about. This is the assertion
// that would catch the fix over-correcting into refusing ordinary requests — and, on its own, it
// is also the test that would have passed before the fix and measured nothing.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeGetHeightsInteriorRegionIsUnaffectedTest,
    "PinWright.landscape.get_heights.InteriorRegionIsUnaffected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeGetHeightsInteriorRegionIsUnaffectedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightGetHeightsRegionTest;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor has no editor world with a persistent level, so no landscape fixture can be created."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_GetHeightsInterior_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    if (!CreateTinyLandscape(*this, World, Label))
    {
        return false;
    }

    FTestResponseCapture Capture;
    ReadRegion(Label, 1, 1, 5, 5, Capture, *this);

    TestTrue(TEXT("an interior region reads successfully"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        AddError(FString::Printf(TEXT("get_heights failed: %s / %s"), *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    int32 ReadMaxX = 0;
    if (GetRegionCoord(*this, Capture.Result, TEXT("region"), TEXT("maxX"), ReadMaxX))
    {
        TestEqual(TEXT("an interior region is read exactly as asked"), ReadMaxX, 5);
    }

    int32 SampleCount = 0;
    Capture.Result->TryGetNumberField(TEXT("sampleCount"), SampleCount);
    TestEqual(TEXT("every sample asked for was measured"), SampleCount, 25);

    double OmittedSamples = -1.0;
    Capture.Result->TryGetNumberField(TEXT("omittedSampleCount"), OmittedSamples);
    TestEqual(TEXT("nothing was omitted"), OmittedSamples, 0.0);

    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    TestFalse(TEXT("an unclamped, fully-measured read warns about nothing"),
        Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings));

    return true;
}
