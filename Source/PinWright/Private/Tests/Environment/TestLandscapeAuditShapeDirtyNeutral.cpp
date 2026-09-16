// Copyright (c) 2026 Alexander Penkin. MIT License.

// landscape.audit_shape reads through FLandscapeEditDataInterface::GetHeightData, the same path
// that made landscape.get_heights dirty the map package (board B-landscape-get-heights-dirties-
// map): the interface calls Texture->Modify(bShouldDirtyPackage) with the flag defaulting true,
// and the heightmap texture is outered to the landscape proxy, so on a classic level the flag
// lands on the MAP package. A read verb that dirties destroys the only signal a host project has
// that a WRITE landed, because a save is gated on the dirty flag and an in-memory read-back
// cannot fail.
//
// Two tests, because the contract is PRESERVE and not clear. A "fix" that unconditionally
// cleared the flag would pass the clean-start assertion while silently throwing away an
// unrelated pending edit, which is a worse bug than the one being fixed - so the dirty-start
// case is the one that pins the contract down.
//
// Counterfactual: drop either MakeLandscapeEditInterfaceReadOnly or the
// FScopedPackageDirtyRestore guard from the verb and CleanLevelStaysClean fails on its first
// assertion.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestWorldUtils.h"
#include "UObject/Package.h"

namespace PinWrightAuditShapeDirtyTest
{
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

    // The audit over the whole fixture. A flat 8x8 landscape measures as Unrunnable, which is
    // the honest verdict and irrelevant here - what matters is that the verb reached the height
    // read, because a call that errored out early would never construct the edit interface and
    // the dirty assertions would pass for the wrong reason.
    void AuditShape(FAutomationTestBase& Test, const FString& Label)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("landscapeName"), Label);

        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("landscape.audit_shape handler is registered"),
            InvokeHandlerWithCapture(TEXT("landscape.audit_shape"), Payload, Capture));
        // A content defect is reported as success + pass:false, so bSuccess must hold here:
        // this verb never turns terrain shape into an RPC error.
        Test.TestTrue(TEXT("landscape.audit_shape returned success, not an RPC error"),
            Capture.bSuccess);
    }
}

// ============================================================================
// A shape audit on a CLEAN level leaves it clean.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeAuditShapeCleanLevelStaysCleanTest,
    "PinWright.landscape.audit_shape.CleanLevelStaysClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeAuditShapeCleanLevelStaysCleanTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("there is no editor world, so the dirty-flag assertions did not run"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_AuditShapeClean_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ALandscape* Landscape = PinWrightAuditShapeDirtyTest::CreateTinyLandscape(*this, World, Label);
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

    // Creating the fixture dirties packages, which is correct - clear the flags so the audit is
    // the only thing that could set them, and restore whatever was there before.
    const bool bLandscapePkgWasDirty = LandscapePkg->IsDirty();
    const bool bLevelPkgWasDirty = LevelPkg->IsDirty();
    ON_SCOPE_EXIT
    {
        LandscapePkg->SetDirtyFlag(bLandscapePkgWasDirty);
        LevelPkg->SetDirtyFlag(bLevelPkgWasDirty);
    };
    LandscapePkg->SetDirtyFlag(false);
    LevelPkg->SetDirtyFlag(false);

    PinWrightAuditShapeDirtyTest::AuditShape(*this, Label);

    TestFalse(TEXT("landscape.audit_shape leaves the landscape actor's package clean"),
        LandscapePkg->IsDirty());
    TestFalse(TEXT("landscape.audit_shape leaves the level package clean"),
        LevelPkg->IsDirty());
    return true;
}

// ============================================================================
// A shape audit on an ALREADY-DIRTY level leaves it dirty. Preserve, not clear:
// a guard that cleared unconditionally would throw away a pending edit.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeAuditShapeDirtyLevelStaysDirtyTest,
    "PinWright.landscape.audit_shape.DirtyLevelStaysDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeAuditShapeDirtyLevelStaysDirtyTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World || !World->PersistentLevel)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("there is no editor world, so the dirty-flag assertions did not run"));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = FString::Printf(TEXT("PW_AuditShapeDirty_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    ALandscape* Landscape = PinWrightAuditShapeDirtyTest::CreateTinyLandscape(*this, World, Label);
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

    PinWrightAuditShapeDirtyTest::AuditShape(*this, Label);

    TestTrue(TEXT("landscape.audit_shape does not clear a pre-existing dirty flag on the landscape package"),
        LandscapePkg->IsDirty());
    TestTrue(TEXT("landscape.audit_shape does not clear a pre-existing dirty flag on the level package"),
        LevelPkg->IsDirty());
    return true;
}
