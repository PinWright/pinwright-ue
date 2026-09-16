// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the niagara.validate SubUV atlas check
// (ticket F-niagara-validate-subuv-atlas-vs-subimagesize).
//
// A sprite/mesh renderer's SubImageSize declares the frame grid the vertex factory slices the
// assigned texture into, and before this check nothing in the plugin joined that number to the
// texture actually sampled: a renderer set to 8x8 over a 6x6 atlas validated clean at strict,
// compiled clean, and rendered inter-cell gutter with a clipped fragment instead of a frame.
//
// Two fixtures, because the two measured signals catch different shapes:
//   * PACKED - a 48x48 sheet drawn as a full 6x6 of blobs with visible gutters, declared 8x8.
//     Every declared cell overlaps real content, so the trailing-empty pass sees nothing wrong;
//     only the gutter period reads the real 6x6. This is the reported asset's shape.
//   * PARTIAL - a 24x24 sheet whose flat content fills only the first 6 of 8 cells per axis.
//     No gutters at all, so no period is readable; only the trailing-empty pass sees it, and it
//     reports a warning because a partly-filled sheet is legitimate.
//
// Counterfactual: with the rule absent (EvaluateRendererSubUVAtlas removed) this test does not
// compile; with the gutter-period detection removed, the PACKED fixture's DetectedGrid stays
// (0,0), IsMismatch() is false and the reported defect produces no finding at all.
#include "Misc/AutomationTest.h"

#include "Handlers/Niagara/NiagaraSubUVAtlasCheck.h"

#include "Engine/Texture2D.h"
#include "NiagaraSpriteRendererProperties.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    // 24 divides by both 8 and 6, so a cell boundary of either grid lands on a whole pixel.
    constexpr int32 SubUVPartialFixtureSize = 24;
    // 6 of the 8 declared columns/rows, in pixels: 6 * (24 / 8).
    constexpr int32 SubUVPartialPopulatedExtent = 18;

    // 48 = 6 cells of 8 px, and also divides by 8 (6 px cells), so both grids stay whole-pixel.
    constexpr int32 SubUVPackedFixtureSize = 48;
    constexpr int32 SubUVPackedCellSize = 8;

    UTexture2D* MakeSubUVSourceTexture(int32 Size, const TArray<uint8>& Bytes)
    {
        UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage());
        if (Texture)
        {
            Texture->Source.Init(Size, Size, 1, 1, TSF_BGRA8, Bytes.GetData());
        }
        return Texture;
    }

    // A 24x24 BGRA8 atlas. When bPopulateAll is false only the top-left 18x18 carries content,
    // which is exactly the first 6 cells of an 8x8 grid on each axis - and, being flat, it has no
    // gutters for the period detector to find.
    UTexture2D* MakeSubUVPartialAtlasTexture(bool bPopulateAll)
    {
        TArray<uint8> Bytes;
        Bytes.Reserve(SubUVPartialFixtureSize * SubUVPartialFixtureSize * 4);
        for (int32 Y = 0; Y < SubUVPartialFixtureSize; ++Y)
        {
            for (int32 X = 0; X < SubUVPartialFixtureSize; ++X)
            {
                const bool bHasContent = bPopulateAll
                    || (X < SubUVPartialPopulatedExtent && Y < SubUVPartialPopulatedExtent);
                const uint8 Value = bHasContent ? 200 : 0;
                // Source byte order is B,G,R,A.
                Bytes.Add(Value);
                Bytes.Add(Value);
                Bytes.Add(Value);
                Bytes.Add(255);
            }
        }
        return MakeSubUVSourceTexture(SubUVPartialFixtureSize, Bytes);
    }

    // A 48x48 BGRA8 atlas laid out as a 6x6 of 8 px cells, each carrying a 6x6 blob with a
    // one-pixel dark border - so the inter-cell lines are the only dark columns and rows.
    // DrawnCellRows = 6 fills every cell, which is what makes it the fixture the trailing-empty
    // pass cannot see; a smaller value leaves whole trailing cell rows blank, the ordinary shape
    // of a sheet holding fewer frames than it has cells.
    UTexture2D* MakeSubUVPackedAtlasTexture(int32 DrawnCellRows)
    {
        TArray<uint8> Bytes;
        Bytes.Reserve(SubUVPackedFixtureSize * SubUVPackedFixtureSize * 4);
        for (int32 Y = 0; Y < SubUVPackedFixtureSize; ++Y)
        {
            const int32 CellY = Y % SubUVPackedCellSize;
            const bool bDrawnRow = (Y / SubUVPackedCellSize) < DrawnCellRows;
            for (int32 X = 0; X < SubUVPackedFixtureSize; ++X)
            {
                const int32 CellX = X % SubUVPackedCellSize;
                const bool bBlob = bDrawnRow && CellX >= 1 && CellX <= 6 && CellY >= 1 && CellY <= 6;
                const uint8 Value = bBlob ? 200 : 0;
                Bytes.Add(Value);
                Bytes.Add(Value);
                Bytes.Add(Value);
                Bytes.Add(255);
            }
        }
        return MakeSubUVSourceTexture(SubUVPackedFixtureSize, Bytes);
    }

    // A sprite renderer declaring Columns x Rows and binding Atlas through the renderer's own
    // material texture parameter - the route the check resolves first, because a MID built from
    // that binding overrides whatever the material asset samples.
    UNiagaraSpriteRendererProperties* MakeSubUVFixtureRenderer(UTexture2D* Atlas, int32 Columns, int32 Rows)
    {
        UNiagaraSpriteRendererProperties* Renderer = NewObject<UNiagaraSpriteRendererProperties>(
            GetTransientPackage(),
            UNiagaraSpriteRendererProperties::StaticClass(),
            NAME_None,
            RF_Transient | RF_Transactional);
        if (!Renderer)
        {
            return nullptr;
        }

        Renderer->SubImageSize = FVector2D(static_cast<double>(Columns), static_cast<double>(Rows));
        if (Atlas)
        {
            FNiagaraRendererMaterialTextureParameter TextureParameter;
            TextureParameter.MaterialParameterName = TEXT("SubUVTexture");
            TextureParameter.Texture = Atlas;
            Renderer->MaterialParameters.TextureParameters.Add(TextureParameter);
        }
        return Renderer;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSubUVAtlasCheckTest,
    "PinWright.niagara.validate.SubUVAtlasMismatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSubUVAtlasCheckTest::RunTest(const FString& Parameters)
{
    // --- PACKED 6x6 declared 8x8: the reported defect, invisible to every signal but the period. ---
    UTexture2D* PackedAtlas = MakeSubUVPackedAtlasTexture(/*DrawnCellRows=*/6);
    if (!TestNotNull(TEXT("Constructed packed 6x6 atlas"), PackedAtlas))
    {
        return true;
    }

    UNiagaraSpriteRendererProperties* PackedRenderer = MakeSubUVFixtureRenderer(PackedAtlas, 8, 8);
    if (!TestNotNull(TEXT("Constructed 8x8 sprite renderer over the packed atlas"), PackedRenderer))
    {
        return true;
    }

    PinWrightNiagara::FSubUVRendererAtlasFinding Packed;
    if (!TestTrue(TEXT("8x8 renderer is in scope for the SubUV atlas check"),
            PinWrightNiagara::EvaluateRendererSubUVAtlas(*PackedRenderer, Packed)))
    {
        return true;
    }

    TestEqual(TEXT("declared columns"), Packed.DeclaredSubImageSize.X, 8);
    TestEqual(TEXT("declared rows"), Packed.DeclaredSubImageSize.Y, 8);
    TestTrue(TEXT("texture resolved through the renderer's own texture parameter"),
        Packed.TextureSource == PinWrightNiagara::ESubUVTextureSource::RendererTextureParameter);
    TestEqual(TEXT("resolved texture path"), Packed.TexturePath, PackedAtlas->GetPathName());
    TestEqual(TEXT("texture width read off the source"), Packed.TextureWidth, SubUVPackedFixtureSize);
    TestTrue(FString::Printf(TEXT("packed atlas was measurable (reason='%s')"), *Packed.UnverifiedReason),
        Packed.UnverifiedReason.IsEmpty());

    // Every weak signal is silent on this fixture - which is exactly why the period detector had
    // to exist. 48 divides by 8; the transient texture's name spells no grid; and every declared
    // 8x8 cell overlaps a blob, so nothing is trailing-empty.
    TestFalse(TEXT("48 divides evenly by 8, so divisibility stays quiet"), Packed.bDimensionsIndivisible);
    TestEqual(TEXT("no name-encoded grid on the fixture"), Packed.NameEncodedGrid.X, 0);
    TestEqual(TEXT("packed atlas has no empty declared cell"), Packed.EmptyTileCount, 0);
    TestEqual(TEXT("trailing-empty pass sees the declared columns"), Packed.PopulatedGrid.X, 8);
    TestEqual(TEXT("trailing-empty pass sees the declared rows"), Packed.PopulatedGrid.Y, 8);
    TestFalse(TEXT("trailing-empty pass finds nothing on a packed sheet"), Packed.HasTrailingEmptyMismatch());

    // The gutter period is the signal that catches it. Without this code the whole finding is absent.
    TestEqual(FString::Printf(TEXT("detected columns (reason='%s')"), *Packed.DetectionReason),
        Packed.DetectedGrid.X, 6);
    TestEqual(TEXT("detected rows"), Packed.DetectedGrid.Y, 6);
    TestTrue(TEXT("column gutter confidence is high"), Packed.DetectedColumnConfidence >= 0.9);
    TestTrue(TEXT("row gutter confidence is high"), Packed.DetectedRowConfidence >= 0.9);
    TestTrue(TEXT("the detected period disagrees with SubImageSize"), Packed.HasDetectedGridMismatch());
    TestTrue(TEXT("the renderer is reported as a mismatch"), Packed.IsMismatch());

    const FString PackedMessage = PinWrightNiagara::DescribeSubUVAtlasFinding(Packed);
    TestTrue(TEXT("message names the declared grid"), PackedMessage.Contains(TEXT("SubImageSize 8 x 8")));
    TestTrue(TEXT("message names the measured period"), PackedMessage.Contains(TEXT("cell period measures 6 x 6")));

    // --- Same packed atlas declared 6x6: the period agrees, so nothing is reported. ---
    UNiagaraSpriteRendererProperties* CorrectRenderer = MakeSubUVFixtureRenderer(PackedAtlas, 6, 6);
    if (TestNotNull(TEXT("Constructed 6x6 sprite renderer over the packed atlas"), CorrectRenderer))
    {
        PinWrightNiagara::FSubUVRendererAtlasFinding Correct;
        if (TestTrue(TEXT("6x6 renderer is in scope"),
                PinWrightNiagara::EvaluateRendererSubUVAtlas(*CorrectRenderer, Correct)))
        {
            TestEqual(TEXT("correct renderer detected columns"), Correct.DetectedGrid.X, 6);
            TestEqual(TEXT("correct renderer detected rows"), Correct.DetectedGrid.Y, 6);
            TestFalse(TEXT("a grid that matches its atlas raises no finding"), Correct.IsMismatch());
            TestTrue(TEXT("correct renderer is not unverified"), Correct.UnverifiedReason.IsEmpty());
        }
    }

    // --- A correct 6x6 grid whose last cell row is simply not drawn (30 frames in 36 cells). ---
    // This is the shape that must NOT be an error: the period still reads 6x6, and the only
    // signal that fires is the trailing-empty one, which is a warning precisely because a sheet
    // holding fewer frames than it has cells is sound content.
    UTexture2D* UnderfilledAtlas = MakeSubUVPackedAtlasTexture(/*DrawnCellRows=*/5);
    UNiagaraSpriteRendererProperties* UnderfilledRenderer = MakeSubUVFixtureRenderer(UnderfilledAtlas, 6, 6);
    if (TestNotNull(TEXT("Constructed underfilled 6x6 atlas"), UnderfilledAtlas)
        && TestNotNull(TEXT("Constructed 6x6 renderer over the underfilled atlas"), UnderfilledRenderer))
    {
        PinWrightNiagara::FSubUVRendererAtlasFinding Underfilled;
        if (TestTrue(TEXT("underfilled renderer is in scope"),
                PinWrightNiagara::EvaluateRendererSubUVAtlas(*UnderfilledRenderer, Underfilled)))
        {
            TestEqual(FString::Printf(TEXT("a blank trailing cell row does not move the period (reason='%s')"),
                    *Underfilled.DetectionReason),
                Underfilled.DetectedGrid.X, 6);
            TestEqual(TEXT("underfilled detected rows"), Underfilled.DetectedGrid.Y, 6);
            TestFalse(TEXT("no error-grade finding on a legitimately underfilled sheet"),
                Underfilled.HasDetectedGridMismatch());
            TestEqual(TEXT("the blank trailing cell row is still reported"), Underfilled.PopulatedGrid.Y, 5);
            TestTrue(TEXT("and it fires the warning-grade signal only"), Underfilled.HasTrailingEmptyMismatch());
        }
    }

    // --- PARTIAL sheet declared 8x8: only the trailing-empty pass sees it, and only as a warning. ---
    UTexture2D* PartialAtlas = MakeSubUVPartialAtlasTexture(/*bPopulateAll=*/false);
    UNiagaraSpriteRendererProperties* PartialRenderer = MakeSubUVFixtureRenderer(PartialAtlas, 8, 8);
    if (TestNotNull(TEXT("Constructed partially-populated atlas"), PartialAtlas)
        && TestNotNull(TEXT("Constructed 8x8 renderer over the partial atlas"), PartialRenderer))
    {
        PinWrightNiagara::FSubUVRendererAtlasFinding Partial;
        if (TestTrue(TEXT("partial renderer is in scope"),
                PinWrightNiagara::EvaluateRendererSubUVAtlas(*PartialRenderer, Partial)))
        {
            TestEqual(TEXT("partial populated columns"), Partial.PopulatedGrid.X, 6);
            TestEqual(TEXT("partial populated rows"), Partial.PopulatedGrid.Y, 6);
            TestEqual(TEXT("partial empty tile count"), Partial.EmptyTileCount, 64 - 36);
            TestTrue(TEXT("the trailing-empty signal fires"), Partial.HasTrailingEmptyMismatch());
            // A flat block has no inter-cell lines, so no period is readable - and that must not
            // be reported as a detected 8x8 either.
            TestEqual(TEXT("no period is readable from a gutterless block"), Partial.DetectedGrid.X, 0);
            TestFalse(TEXT("trailing-empty alone never proves a different cell size"),
                Partial.HasDetectedGridMismatch());
            TestTrue(TEXT("a detection reason is recorded rather than left silent"),
                !Partial.DetectionReason.IsEmpty());
        }
    }

    // --- A fully-populated flat sheet: no signal fires at all. ---
    UTexture2D* FullAtlas = MakeSubUVPartialAtlasTexture(/*bPopulateAll=*/true);
    UNiagaraSpriteRendererProperties* HealthyRenderer = MakeSubUVFixtureRenderer(FullAtlas, 8, 8);
    if (TestNotNull(TEXT("Constructed fully-populated atlas"), FullAtlas)
        && TestNotNull(TEXT("Constructed healthy 8x8 renderer"), HealthyRenderer))
    {
        PinWrightNiagara::FSubUVRendererAtlasFinding Healthy;
        if (TestTrue(TEXT("healthy renderer is in scope"),
                PinWrightNiagara::EvaluateRendererSubUVAtlas(*HealthyRenderer, Healthy)))
        {
            TestEqual(TEXT("healthy populated columns"), Healthy.PopulatedGrid.X, 8);
            TestEqual(TEXT("healthy empty tile count"), Healthy.EmptyTileCount, 0);
            TestEqual(TEXT("no spurious period on a flat sheet"), Healthy.DetectedGrid.X, 0);
            TestFalse(TEXT("a flat sheet raises no finding"), Healthy.IsMismatch());
            TestTrue(TEXT("healthy renderer is not unverified"), Healthy.UnverifiedReason.IsEmpty());
        }
    }

    // --- A renderer with no SubUV grid is out of scope entirely. ---
    UNiagaraSpriteRendererProperties* PlainRenderer = MakeSubUVFixtureRenderer(FullAtlas, 1, 1);
    if (TestNotNull(TEXT("Constructed 1x1 renderer"), PlainRenderer))
    {
        PinWrightNiagara::FSubUVRendererAtlasFinding Plain;
        TestFalse(TEXT("SubImageSize (1,1) is not a SubUV renderer"),
            PinWrightNiagara::EvaluateRendererSubUVAtlas(*PlainRenderer, Plain));
    }

    // --- A renderer whose texture cannot be resolved is reported, never silently passed. ---
    UNiagaraSpriteRendererProperties* UnboundRenderer = MakeSubUVFixtureRenderer(nullptr, 4, 4);
    if (TestNotNull(TEXT("Constructed unbound 4x4 renderer"), UnboundRenderer))
    {
        PinWrightNiagara::FSubUVRendererAtlasFinding Unbound;
        if (TestTrue(TEXT("unbound renderer is in scope"),
                PinWrightNiagara::EvaluateRendererSubUVAtlas(*UnboundRenderer, Unbound)))
        {
            TestTrue(TEXT("an unresolvable texture yields a reason, not silence"),
                !Unbound.UnverifiedReason.IsEmpty());
            TestFalse(TEXT("an unverified renderer is not reported as a mismatch"), Unbound.IsMismatch());
        }
    }

    // --- The period detector, driven directly on synthetic profiles. ---
    {
        // Six cells of eight samples, each with a dark sample at either end: the shape a gutter
        // grid makes in a 1-D energy profile.
        TArray<double> Periodic;
        for (int32 Index = 0; Index < 48; ++Index)
        {
            const int32 Cell = Index % 8;
            Periodic.Add((Cell >= 1 && Cell <= 6) ? 150.0 : 0.0);
        }
        double Confidence = 0.0;
        TestEqual(TEXT("a six-cell profile reads as six divisions"),
            PinWrightNiagara::DetectAxisDivisions(Periodic, 16, Confidence), 6);
        TestTrue(TEXT("black gutters give full confidence"), Confidence >= 0.99);

        // A flat profile has no period at any division.
        TArray<double> Flat;
        Flat.Init(150.0, 48);
        double FlatConfidence = 0.0;
        TestEqual(TEXT("a flat profile reads as no divisions"),
            PinWrightNiagara::DetectAxisDivisions(Flat, 16, FlatConfidence), 0);
    }

    // --- Name heuristic: a grid spelled in the texture name, and the shapes that are not one. ---
    FIntPoint NameGrid = FIntPoint::ZeroValue;
    TestTrue(TEXT("'T_Smoke_8x8' spells a grid"),
        PinWrightNiagara::ParseGridFromAssetName(TEXT("T_Smoke_8x8"), NameGrid));
    TestEqual(TEXT("name grid columns"), NameGrid.X, 8);
    TestEqual(TEXT("name grid rows"), NameGrid.Y, 8);

    TestTrue(TEXT("'T_Fire_subUV_6X6_02' spells a grid with a capital X"),
        PinWrightNiagara::ParseGridFromAssetName(TEXT("T_Fire_subUV_6X6_02"), NameGrid));
    TestEqual(TEXT("capital-X grid columns"), NameGrid.X, 6);

    TestFalse(TEXT("'T_Fire_subUV_01' spells no grid"),
        PinWrightNiagara::ParseGridFromAssetName(TEXT("T_Fire_subUV_01"), NameGrid));
    TestFalse(TEXT("a pixel size in a name is not read as a frame grid"),
        PinWrightNiagara::ParseGridFromAssetName(TEXT("T_Sky_1024x1024"), NameGrid));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
