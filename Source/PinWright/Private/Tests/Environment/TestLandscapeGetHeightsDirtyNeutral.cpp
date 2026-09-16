// Copyright (c) 2026 Alexander Penkin. MIT License.

// Dirty-flag contract for landscape.get_heights — board B-landscape-get-heights-dirties-map.
//
// The defect: the READ counterpart to landscape.sculpt / landscape.edit left the map
// package dirty. Reproduced on the host project with a control — fresh boot,
// editor.list_dirty_packages count 0; exactly one 5x5 get_heights with
// includeSamples:true and no other RPC; count 1, and the entry is the open map's own package.
//
// Cause, in the engine: FLandscapeEditDataInterface is an edit interface even when only
// read from. GetHeightData -> GetHeightDataTempl -> GetHeightDataInternal ->
// GetHeightMapColor reaches GetTextureDataInfo (LandscapeEditInterface.cpp:3451-3457),
// whose FLandscapeTextureDataInfo ctor calls Texture->Modify(bShouldDirtyPackage)
// (:4028-4043) with the interface default bShouldDirtyPackage = true (:88-90). The
// heightmap texture is outered to the landscape proxy (LandscapeEdit.cpp:7954-7957), so
// the flag lands on the proxy's package — the MAP package on a classic level.
// bUploadTextureChangesToGPU=false never touched this; it only gates the GPU upload.
//
// Why it is worth two tests rather than one: the correct behaviour is PRESERVE, not
// clear. A "fix" that unconditionally cleared the flag after the read would pass a
// clean-start assertion and silently destroy an unrelated pending edit — so the
// dirty-start case is the one that pins the contract down. Both are asserted on the
// LANDSCAPE ACTOR's package and on the LEVEL's, which differ under OFPA and collapse to
// one package on a classic map.
//
// Counterfactual: revert MakeLandscapeEditInterfaceReadOnly + the
// FScopedPackageDirtyRestore guard in landscape.get_heights and CleanLevelStaysClean
// fails on the first assertion, exactly as the board's control run measured.
//
// Fixture scope: the landscape is created in code through the production
// landscape.create handler, under a uniquely-labelled GUID name, inside
// FScopedEditorWorldActorGuard (which destroys what the test spawned and restores the
// level's dirty flag). Nothing on disk and no host content is touched.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/Package.h"

namespace PinWrightGetHeightsDirtyTest
{
    // Creates a minimal landscape through the production landscape.create handler.
    // 1x1 components of 7 quads = 8x8 vertices, which is the smallest grid that still
    // supports the 5x5 region the board's repro used.
    ALandscape* CreateTinyLandscape(FAutomationTestBase& Test, UWorld* World, const FString& Label)
    {
        TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
        CreatePayload->SetStringField(TEXT("name"), Label);
        CreatePayload->SetNumberField(TEXT("componentsX"), 1);
        CreatePayload->SetNumberField(TEXT("componentsY"), 1);
        CreatePayload->SetNumberField(TEXT("quadsPerComponent"), 7);
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

    // The board's repro shape: one 5x5 region with includeSamples true.
    void ReadHeights(FAutomationTestBase& Test, const FString& Label)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("landscapeName"), Label);
        Payload->SetBoolField(TEXT("includeSamples"), true);
        TSharedPtr<FJsonObject> Region = MakeShared<FJsonObject>();
        Region->SetNumberField(TEXT("minX"), 0);
        Region->SetNumberField(TEXT("minY"), 0);
        Region->SetNumberField(TEXT("maxX"), 4);
        Region->SetNumberField(TEXT("maxY"), 4);
        Payload->SetObjectField(TEXT("region"), Region);

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("landscape.get_heights handler is registered"),
            InvokeHandlerWithCapture(TEXT("landscape.get_heights"), Payload, Capture));
        // A read that errored out early would never construct the edit interface, so the
        // dirty assertions below would pass for the wrong reason.
        Test.TestTrue(TEXT("landscape.get_heights succeeded"), Capture.bSuccess);
    }
}

// ============================================================================
// A read on a CLEAN level leaves it clean. This is the board's control run,
// expressed as an assertion.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeGetHeightsCleanLevelStaysCleanTest,
    "PinWright.landscape.get_heights.CleanLevelStaysClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeGetHeightsCleanLevelStaysCleanTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping get_heights clean-stays-clean test"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_GetHeightsClean_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ALandscape* Landscape = PinWrightGetHeightsDirtyTest::CreateTinyLandscape(*this, World, Label);
    if (!Landscape)
    {
        return false;
    }

    UPackage* LandscapePkg = Landscape->GetPackage();
    UPackage* LevelPkg = World->PersistentLevel->GetPackage();
    TestNotNull(TEXT("landscape has a package"), LandscapePkg);
    TestNotNull(TEXT("persistent level has a package"), LevelPkg);
    if (!LandscapePkg || !LevelPkg)
    {
        return false;
    }

    // Creating the fixture dirties packages, which is correct — clear the flags so the
    // read is the only thing that could set them, and restore whatever was there before.
    const bool bLandscapePkgWasDirty = LandscapePkg->IsDirty();
    const bool bLevelPkgWasDirty = LevelPkg->IsDirty();
    ON_SCOPE_EXIT
    {
        LandscapePkg->SetDirtyFlag(bLandscapePkgWasDirty);
        LevelPkg->SetDirtyFlag(bLevelPkgWasDirty);
    };
    LandscapePkg->SetDirtyFlag(false);
    LevelPkg->SetDirtyFlag(false);

    PinWrightGetHeightsDirtyTest::ReadHeights(*this, Label);

    TestFalse(TEXT("landscape.get_heights leaves the landscape actor's package clean"),
        LandscapePkg->IsDirty());
    TestFalse(TEXT("landscape.get_heights leaves the level package clean"),
        LevelPkg->IsDirty());

    return true;
}

// ============================================================================
// A read on an ALREADY-DIRTY level leaves it dirty. Preserve, not clear: a guard
// that clears unconditionally would throw away a pending edit, which is a worse
// bug than the one being fixed.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeGetHeightsDirtyLevelStaysDirtyTest,
    "PinWright.landscape.get_heights.DirtyLevelStaysDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeGetHeightsDirtyLevelStaysDirtyTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("Editor world not available — skipping get_heights dirty-stays-dirty test"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_GetHeightsDirty_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ALandscape* Landscape = PinWrightGetHeightsDirtyTest::CreateTinyLandscape(*this, World, Label);
    if (!Landscape)
    {
        return false;
    }

    UPackage* LandscapePkg = Landscape->GetPackage();
    UPackage* LevelPkg = World->PersistentLevel->GetPackage();
    TestNotNull(TEXT("landscape has a package"), LandscapePkg);
    TestNotNull(TEXT("persistent level has a package"), LevelPkg);
    if (!LandscapePkg || !LevelPkg)
    {
        return false;
    }

    const bool bLandscapePkgWasDirty = LandscapePkg->IsDirty();
    const bool bLevelPkgWasDirty = LevelPkg->IsDirty();
    ON_SCOPE_EXIT
    {
        LandscapePkg->SetDirtyFlag(bLandscapePkgWasDirty);
        LevelPkg->SetDirtyFlag(bLevelPkgWasDirty);
    };
    LandscapePkg->SetDirtyFlag(true);
    LevelPkg->SetDirtyFlag(true);

    PinWrightGetHeightsDirtyTest::ReadHeights(*this, Label);

    TestTrue(TEXT("landscape.get_heights does not clear a pre-existing dirty flag on the landscape package"),
        LandscapePkg->IsDirty());
    TestTrue(TEXT("landscape.get_heights does not clear a pre-existing dirty flag on the level package"),
        LevelPkg->IsDirty());

    return true;
}
