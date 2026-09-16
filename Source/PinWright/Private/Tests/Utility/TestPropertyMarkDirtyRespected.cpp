// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestPropertyMarkDirtyRespected.cpp
// Regression tests for B-property-set-markdirty-false-still-dirties.
//
// property.set and property.reset accept `markDirty` (default true), documented as
// "pass false for transient checks". Pre-fix that parameter gated ONLY the final
// MarkPackageDirty() call, while RootObject->Modify() ran earlier and unconditionally
// (UObject::Modify defaults bAlwaysMarkDirty=true, and with no open transaction it falls
// straight through to MarkPackageDirty), and PostEditChange() could re-dirty afterwards.
// Net effect: markDirty:false left the package DIRTY while the response still claimed
// markedDirty:false, so a transient probe value rode a later blanket editor.save_all onto
// disk.
//
// Post-fix contract exercised here:
//   1. markDirty:false against a CLEAN package leaves it clean.
//   2. markDirty:true leaves it dirty (counterfactual — proves these tests can tell the
//      two apart; a "fix" that simply never dirties anything must not pass the suite).
//   3. markDirty:false against an ALREADY-DIRTY package leaves it dirty — the restore may
//      only ever return a package to clean if it was clean to begin with, never eat
//      pre-existing user dirt.
//   4. The same clean-package guarantee holds for property.reset.
//   5. property.set's response `markedDirty` field reports OBSERVED package state, not the
//      request parameter.
//   6. property.reset reports the SAME `markedDirty` field, also from observed state, in both
//      directions (E-property-reset-no-markeddirty). Pre-fix the field was absent entirely:
//      the behaviour was correct on both verbs but only property.set said so, leaving the
//      markDirty guarantee uncheckable from a reset response and pushing callers onto
//      editor.list_dirty_packages — a second, process-wide, race-prone call.
//
// Why the fixture is a real on-disk asset and not a transient one: the sibling
// TestPropertySetReportsMarkDirtyNotSaved.cpp builds its fixture in GetTransientPackage(),
// and UObjectBaseUtility::MarkPackageDirty() early-returns for anything under an
// RF_Transient outer. No dirty flag is ever set there — with or without the fix — so a
// transient fixture makes every assertion below vacuously true. These tests therefore
// create a real /Game asset, SAVE it to disk so it starts clean, and assert on
// UPackage::IsDirty() directly rather than on response JSON alone.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Sound/SoundMix.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

// Named (not anonymous) namespace so a Unity merge of this TU with a sibling test cannot
// produce an ODR collision on the shared helper names, matching the plugin convention for
// test-side helpers.
namespace PropertyMarkDirtyRespectedTestHelpers
{
    // USoundMix is the fixture asset type: a plain UObject with no editor graph or factory
    // subobjects (so bare CreatePackage + NewObject + SaveLoadedAsset is a known-good
    // creation path, the same shape TestAssetReloadHandler.cpp uses for USoundClass), and
    // it exposes FadeInTime as a direct top-level scalar UPROPERTY(EditAnywhere, float).
    // A direct scalar keeps property RESOLUTION orthogonal to the dirty-flag behavior under
    // test. Its PostEditChangeProperty only touches the transient bChanged flag when the
    // changed member is not SoundClassEffects, so it contributes no dirt of its own.
    constexpr const TCHAR* FixtureFolder = TEXT("/Game/PinWrightTests/Property");

    // Seed value persisted to disk. Deliberately different from the USoundMix CDO default
    // (0.2f) so the property.reset case performs a REAL value change rather than a no-op
    // that could pass without the handler ever touching the dirty flag.
    constexpr float SeedFadeInTime = 0.5f;

    struct FCleanAssetFixture
    {
        FString PackagePath;
        FString ObjectPath;
        UPackage* Package = nullptr;
        USoundMix* Asset = nullptr;
        // Absolute .uasset path plus the mtime and size measured immediately after the
        // fixture save and BEFORE any handler runs. These three are the disk baseline: every
        // other assertion in this file reads UPackage::IsDirty(), an IN-MEMORY flag, so a
        // "fix" that made the package clean by SAVING it would satisfy all of them while
        // writing the transient probe value into the asset.
        FString Filename;
        FDateTime OnDiskTimeStamp = FDateTime::MinValue();
        int64 OnDiskSize = 0;
        // True only when the asset was created, written to disk, and the package was
        // observed CLEAN afterwards. Every assertion downstream is meaningless otherwise.
        bool bReady = false;
    };

    // Creates a uniquely named USoundMix under /Game/PinWrightTests/Property, saves it to
    // disk, and asserts the resulting package starts CLEAN. The clean-start assertion is a
    // hard gate on purpose: a fixture that starts dirty would make "still clean" and
    // "became dirty" indistinguishable, so it must fail loudly here instead of silently
    // passing later.
    FCleanAssetFixture MakeSavedCleanFixture(FAutomationTestBase& Test, const TCHAR* NamePrefix)
    {
        FCleanAssetFixture Fixture;

        const FString AssetName = FString::Printf(
            TEXT("%s_%s"), NamePrefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        Fixture.PackagePath = FString::Printf(TEXT("%s/%s"), FixtureFolder, *AssetName);
        Fixture.ObjectPath = FString::Printf(TEXT("%s.%s"), *Fixture.PackagePath, *AssetName);

        Fixture.Package = CreatePackage(*Fixture.PackagePath);
        if (!Test.TestNotNull(TEXT("fixture package created"), Fixture.Package))
        {
            return Fixture;
        }

        Fixture.Asset = NewObject<USoundMix>(
            Fixture.Package, *AssetName, RF_Public | RF_Standalone);
        if (!Test.TestNotNull(TEXT("fixture USoundMix created"), Fixture.Asset))
        {
            return Fixture;
        }
        FAssetRegistryModule::AssetCreated(Fixture.Asset);

        Fixture.Asset->FadeInTime = SeedFadeInTime;
        Fixture.Package->MarkPackageDirty();

        const bool bSaved =
            UEditorAssetLibrary::SaveLoadedAsset(Fixture.Asset, /*bOnlyIfIsDirty=*/false);
        Test.TestTrue(TEXT("fixture saved to disk via SaveLoadedAsset"), bSaved);

        // Hard disk-presence proof: the dirty flag only means something for a package that
        // really was persisted.
        Fixture.Filename = FPackageName::LongPackageNameToFilename(
            Fixture.PackagePath, FPackageName::GetAssetPackageExtension());
        const FString& Filename = Fixture.Filename;
        const int64 OnDiskSize = IFileManager::Get().FileSize(*Filename);
        Test.TestTrue(TEXT("fixture .uasset is on disk after save"), OnDiskSize > 0);

        // The disk baseline every later TestAssetUntouchedOnDisk() call compares against.
        // Captured here, after the only save this file is ever allowed to perform.
        Fixture.OnDiskSize = OnDiskSize;
        Fixture.OnDiskTimeStamp = IFileManager::Get().GetTimeStamp(*Filename);

        // The gate. A transient-package fixture would trivially satisfy this (and every
        // later dirty assertion) because MarkPackageDirty early-returns under RF_Transient;
        // a real package that saved successfully must genuinely be clean here.
        const bool bDirtyAfterSave = Fixture.Package->IsDirty();
        Test.TestFalse(TEXT("fixture package starts CLEAN after save"), bDirtyAfterSave);

        Fixture.bReady = bSaved && OnDiskSize > 0 && !bDirtyAfterSave;
        return Fixture;
    }

    // Builds a property.set payload for the fixture's scalar property.
    TSharedPtr<FJsonObject> MakeSetPayload(
        const FCleanAssetFixture& Fixture, double Value, bool bMarkDirty)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Fixture.ObjectPath);
        Payload->SetStringField(TEXT("propertyName"), TEXT("FadeInTime"));
        Payload->SetNumberField(TEXT("value"), Value);
        Payload->SetBoolField(TEXT("markDirty"), bMarkDirty);
        return Payload;
    }

    // Builds a property.reset payload for the same scalar property.
    TSharedPtr<FJsonObject> MakeResetPayload(const FCleanAssetFixture& Fixture, bool bMarkDirty)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Fixture.ObjectPath);
        Payload->SetStringField(TEXT("propertyName"), TEXT("FadeInTime"));
        Payload->SetBoolField(TEXT("markDirty"), bMarkDirty);
        return Payload;
    }

    // The read-back assertion this file exists to protect, applied to whichever verb ran.
    //
    // Two claims, and the second is the load-bearing one: the field must be PRESENT (pre-fix
    // property.reset omitted it, so a caller had nothing to read), and its value must equal
    // the package state the test observed for itself. Equality is what pins the field to
    // observed state rather than to the markDirty request — a handler that echoed the
    // parameter would pass the presence check and fail this one on the pre-existing-dirt case.
    void TestMarkedDirtyMatchesObservedState(FAutomationTestBase& Test, const TCHAR* Label,
        const TSharedPtr<FJsonObject>& Result, bool bObservedPackageDirty)
    {
        // Seeded to the WRONG answer so a TryGetBoolField that fails without writing the out
        // param cannot leave a value that accidentally satisfies the equality check below.
        bool bMarkedDirty = !bObservedPackageDirty;
        Test.TestTrue(*FString::Printf(TEXT("%s: response carries markedDirty"), Label),
            Result->TryGetBoolField(TEXT("markedDirty"), bMarkedDirty));
        Test.TestEqual(
            *FString::Printf(TEXT("%s: markedDirty agrees with observed package state"), Label),
            bMarkedDirty, bObservedPackageDirty);
    }

    // Re-measures the fixture's .uasset and asserts the handler wrote NOTHING to disk.
    //
    // This is the check the rest of the file structurally cannot make: every other assertion
    // here reads UPackage::IsDirty(), an in-memory flag, and the project's standing rule is
    // "verify a write against disk, not against the object you just wrote" — a read-back
    // through the already-loaded object never consults the file, so it confirms a write that
    // may never have happened (and, symmetrically, cannot see one that did). Deleting the
    // handler's SetDirtyFlag(false) restore is caught by the IsDirty() assertions; SUBSTITUTING
    // it with a save (the plausible wrong fix — "make it clean by saving it") is caught only
    // here, because a saved package is clean, the value is applied, and markedDirty is false.
    // A stale mtime is the tell; size is carried too so the check survives a filesystem whose
    // timestamp granularity could round a fast rewrite back onto the original stamp.
    void TestAssetUntouchedOnDisk(
        FAutomationTestBase& Test, const FCleanAssetFixture& Fixture, const TCHAR* Stage)
    {
        const FDateTime NowTimeStamp = IFileManager::Get().GetTimeStamp(*Fixture.Filename);
        const int64 NowSize = IFileManager::Get().FileSize(*Fixture.Filename);

        Test.TestTrue(*FString::Printf(
            TEXT("%s: .uasset mtime unchanged on disk — the handler saved nothing (%s -> %s)"),
            Stage, *Fixture.OnDiskTimeStamp.ToIso8601(), *NowTimeStamp.ToIso8601()),
            NowTimeStamp == Fixture.OnDiskTimeStamp);

        Test.TestTrue(*FString::Printf(
            TEXT("%s: .uasset size unchanged on disk — the handler saved nothing (%lld -> %lld)"),
            Stage, Fixture.OnDiskSize, NowSize),
            NowSize == Fixture.OnDiskSize);
    }

    // The 4 bytes UE's tagged-property serializer writes for a float into an uncooked
    // .uasset (little-endian on Win64). Editor packages are not compressed, which is what
    // makes a raw byte probe of the file meaningful at all.
    TArray<uint8> FloatBytePattern(float Value)
    {
        TArray<uint8> Pattern;
        Pattern.SetNumUninitialized(sizeof(float));
        FMemory::Memcpy(Pattern.GetData(), &Value, sizeof(float));
        return Pattern;
    }

    bool BytesContainPattern(const TArray<uint8>& Haystack, const TArray<uint8>& Needle)
    {
        if (Needle.Num() <= 0 || Haystack.Num() < Needle.Num())
        {
            return false;
        }
        for (int32 Index = 0; Index <= Haystack.Num() - Needle.Num(); ++Index)
        {
            if (FMemory::Memcmp(Haystack.GetData() + Index, Needle.GetData(), Needle.Num()) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // Byte-level counterpart to TestAssetUntouchedOnDisk: proves the .uasset still holds the
    // SEEDED FadeInTime and not the value the handler just applied in memory. mtime+size alone
    // could in principle miss a same-size rewrite that landed inside one timestamp tick; the
    // bytes cannot. Skipped (with the read itself asserted) if the file cannot be read.
    void TestOnDiskValueStillSeeded(
        FAutomationTestBase& Test, const FCleanAssetFixture& Fixture, float AppliedValue)
    {
        TArray<uint8> FileBytes;
        const bool bRead = FFileHelper::LoadFileToArray(FileBytes, *Fixture.Filename);
        Test.TestTrue(TEXT("fixture .uasset re-read from disk for the byte check"), bRead);
        if (!bRead)
        {
            return;
        }

        // Guard against a vacuous byte check: the two patterns must actually differ.
        Test.TestTrue(TEXT("applied value differs from the seeded value (byte check is real)"),
            AppliedValue != SeedFadeInTime);

        Test.TestTrue(
            TEXT("on-disk .uasset still holds the SEEDED FadeInTime bytes"),
            BytesContainPattern(FileBytes, FloatBytePattern(SeedFadeInTime)));

        // The harm the ticket describes, stated directly: a transient probe value must never
        // reach the file. Pre-fix (or under a save-based "fix") the applied value is on disk.
        Test.TestFalse(
            TEXT("on-disk .uasset does NOT hold the applied FadeInTime bytes"),
            BytesContainPattern(FileBytes, FloatBytePattern(AppliedValue)));
    }

    // Invokes a property.* handler and asserts it was found and succeeded.
    bool InvokePropertyMutator(FAutomationTestBase& Test, const FString& Method,
        const TSharedPtr<FJsonObject>& Payload, FTestResponseCapture& Capture)
    {
        Test.TestTrue(*FString::Printf(TEXT("%s handler found"), *Method),
            InvokeHandlerWithCapture(Method, Payload, Capture));
        Test.TestTrue(*FString::Printf(TEXT("%s succeeded"), *Method), Capture.bSuccess);
        Test.TestTrue(*FString::Printf(TEXT("%s returned a payload"), *Method),
            Capture.Result.IsValid());
        return Capture.bSuccess && Capture.Result.IsValid();
    }
}

using namespace PropertyMarkDirtyRespectedTestHelpers;

// ===========================================================================
// property.set — markDirty:false must leave a CLEAN package clean.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetMarkDirtyFalseLeavesPackageCleanTest,
    "PinWright.property.set.MarkDirtyFalseLeavesPackageClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetMarkDirtyFalseLeavesPackageCleanTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor unavailable (asset fixture requires the editor); "
                "skipping MarkDirtyFalseLeavesPackageClean."));
        return true;
    }

    FCleanAssetFixture Fixture = MakeSavedCleanFixture(*this, TEXT("SM_SetClean"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Fixture.PackagePath);
    };
    if (!Fixture.bReady)
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!InvokePropertyMutator(*this, TEXT("property.set"),
            MakeSetPayload(Fixture, 0.75, /*bMarkDirty=*/false), Capture))
    {
        return false;
    }

    // The in-memory write must still have happened — markDirty only governs persistence
    // bookkeeping, never whether the value is applied.
    TestEqual(TEXT("markDirty:false still applies the in-memory write"),
        Fixture.Asset->FadeInTime, 0.75f);

    // THE contract assertion. Pre-fix this fails: RootObject->Modify() dirties the package
    // before the markDirty gate is ever consulted.
    const bool bPackageDirty = Fixture.Package->IsDirty();
    TestFalse(TEXT("markDirty:false leaves a clean package CLEAN"), bPackageDirty);

    bool bMarkedDirty = true;
    TestTrue(TEXT("response carries markedDirty"),
        Capture.Result->TryGetBoolField(TEXT("markedDirty"), bMarkedDirty));
    TestFalse(TEXT("response reports markedDirty:false"), bMarkedDirty);

    // Pins the other half of the contract: markedDirty is DERIVED from observed package
    // state, not echoed from the request parameter. Pre-fix the package is dirty while the
    // field says false, so the two disagree.
    TestEqual(TEXT("response markedDirty agrees with observed package state"),
        bMarkedDirty, bPackageDirty);

    // Disk truth. IsDirty() above is an in-memory flag and cannot distinguish "left clean"
    // from "made clean by saving"; only the file can.
    TestAssetUntouchedOnDisk(*this, Fixture, TEXT("property.set markDirty:false"));
    TestOnDiskValueStillSeeded(*this, Fixture, Fixture.Asset->FadeInTime);

    return true;
}

// ===========================================================================
// property.set — markDirty:true must dirty the package (counterfactual).
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetMarkDirtyTrueDirtiesPackageTest,
    "PinWright.property.set.MarkDirtyTrueDirtiesPackage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetMarkDirtyTrueDirtiesPackageTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor unavailable (asset fixture requires the editor); "
                "skipping MarkDirtyTrueDirtiesPackage."));
        return true;
    }

    FCleanAssetFixture Fixture = MakeSavedCleanFixture(*this, TEXT("SM_SetDirty"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Fixture.PackagePath);
    };
    if (!Fixture.bReady)
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!InvokePropertyMutator(*this, TEXT("property.set"),
            MakeSetPayload(Fixture, 0.875, /*bMarkDirty=*/true), Capture))
    {
        return false;
    }

    TestEqual(TEXT("markDirty:true applies the in-memory write"),
        Fixture.Asset->FadeInTime, 0.875f);

    // Without this test a "fix" that simply never dirties anything would pass the suite.
    const bool bPackageDirty = Fixture.Package->IsDirty();
    TestTrue(TEXT("markDirty:true leaves the package DIRTY"), bPackageDirty);

    bool bMarkedDirty = false;
    TestTrue(TEXT("response carries markedDirty"),
        Capture.Result->TryGetBoolField(TEXT("markedDirty"), bMarkedDirty));
    TestTrue(TEXT("response reports markedDirty:true"), bMarkedDirty);
    TestEqual(TEXT("response markedDirty agrees with observed package state"),
        bMarkedDirty, bPackageDirty);

    // Dirty is the whole point of markDirty:true — but dirty still means "not yet written".
    // property.set is a mark-dirty-only mutator (persist with editor.save_all / asset.save),
    // so even here the .uasset must be byte-identical and still hold the SEEDED 0.5f, not the
    // 0.875f just applied in memory. This is the exact case the ticket describes: a save-based
    // "fix" leaves every in-memory assertion above green with the probe value on disk.
    TestAssetUntouchedOnDisk(*this, Fixture, TEXT("property.set markDirty:true"));
    TestOnDiskValueStillSeeded(*this, Fixture, Fixture.Asset->FadeInTime);

    return true;
}

// ===========================================================================
// property.set — markDirty:false must NOT eat pre-existing dirt.
// ===========================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertySetMarkDirtyFalsePreservesPreexistingDirtTest,
    "PinWright.property.set.MarkDirtyFalsePreservesPreexistingDirt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertySetMarkDirtyFalsePreservesPreexistingDirtTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor unavailable (asset fixture requires the editor); "
                "skipping MarkDirtyFalsePreservesPreexistingDirt."));
        return true;
    }

    FCleanAssetFixture Fixture = MakeSavedCleanFixture(*this, TEXT("SM_SetPreDirty"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Fixture.PackagePath);
    };
    if (!Fixture.bReady)
    {
        return false;
    }

    // Dirt that this handler did not create — stands in for an unsaved user edit or a
    // concurrent editor action. The markDirty:false restore must leave it alone.
    Fixture.Package->MarkPackageDirty();
    TestTrue(TEXT("package is dirty before the markDirty:false call"),
        Fixture.Package->IsDirty());
    if (!Fixture.Package->IsDirty())
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!InvokePropertyMutator(*this, TEXT("property.set"),
            MakeSetPayload(Fixture, 0.625, /*bMarkDirty=*/false), Capture))
    {
        return false;
    }

    // The restore may only return a package to CLEAN if it was clean to begin with.
    const bool bPackageDirty = Fixture.Package->IsDirty();
    TestTrue(TEXT("markDirty:false preserves pre-existing dirt"), bPackageDirty);

    // Observed-state reporting: the package IS dirty here, so markedDirty must say true
    // even though the caller passed markDirty:false. Pre-fix the field echoes the
    // parameter and reports false.
    bool bMarkedDirty = false;
    TestTrue(TEXT("response carries markedDirty"),
        Capture.Result->TryGetBoolField(TEXT("markedDirty"), bMarkedDirty));
    TestEqual(TEXT("response markedDirty agrees with observed package state"),
        bMarkedDirty, bPackageDirty);

    // Preserving pre-existing dirt must not be achieved (or accompanied) by writing the file.
    TestAssetUntouchedOnDisk(*this, Fixture, TEXT("property.set markDirty:false over dirt"));
    TestOnDiskValueStillSeeded(*this, Fixture, Fixture.Asset->FadeInTime);

    return true;
}

// ===========================================================================
// property.reset — markDirty:false must leave a CLEAN package clean.
// ===========================================================================
// On UE 5.8 this exercises the MCP_HAS_PROPERTY_VISITOR branch of property.reset (the 5.4
// FOverridableManager branch and the 5.3 plain-copy branch are not compiled here); all
// three branches share the same Modify() -> PostEditChange() -> dirty-decision ordering
// that the fix corrects.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyResetMarkDirtyFalseLeavesPackageCleanTest,
    "PinWright.property.reset.MarkDirtyFalseLeavesPackageClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyResetMarkDirtyFalseLeavesPackageCleanTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor unavailable (asset fixture requires the editor); "
                "skipping property.reset MarkDirtyFalseLeavesPackageClean."));
        return true;
    }

    // The fixture is created with FadeInTime already seeded to a non-default value and
    // saved in that state, so the package is clean AND the property is overridden — the
    // "set to non-default with markDirty:true, then save so it is clean again" precondition
    // collapsed into the fixture build.
    FCleanAssetFixture Fixture = MakeSavedCleanFixture(*this, TEXT("SM_ResetClean"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Fixture.PackagePath);
    };
    if (!Fixture.bReady)
    {
        return false;
    }

    // Guard against a vacuous pass: if the seeded value already equalled the class default
    // the reset would be a value-copy no-op and could stay clean without the fix.
    const float ClassDefaultFadeInTime = GetDefault<USoundMix>()->FadeInTime;
    const bool bSeedDiffersFromDefault =
        !FMath::IsNearlyEqual(Fixture.Asset->FadeInTime, ClassDefaultFadeInTime);
    TestTrue(TEXT("seeded value differs from the class default (reset is a real change)"),
        bSeedDiffersFromDefault);
    if (!bSeedDiffersFromDefault)
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!InvokePropertyMutator(*this, TEXT("property.reset"),
            MakeResetPayload(Fixture, /*bMarkDirty=*/false), Capture))
    {
        return false;
    }

    // The reset really happened — otherwise "still clean" proves nothing.
    TestEqual(TEXT("property.reset restored the class default value"),
        Fixture.Asset->FadeInTime, ClassDefaultFadeInTime);

    bool bWasOverridden = false;
    TestTrue(TEXT("response carries wasOverridden"),
        Capture.Result->TryGetBoolField(TEXT("wasOverridden"), bWasOverridden));
    TestTrue(TEXT("response reports the property WAS overridden before the reset"),
        bWasOverridden);

    // THE contract assertion. Pre-fix this fails: property.reset's Modify() dirties the
    // package regardless of markDirty.
    const bool bPackageDirty = Fixture.Package->IsDirty();
    TestFalse(TEXT("property.reset markDirty:false leaves a clean package CLEAN"),
        bPackageDirty);

    // The read-back half (E-property-reset-no-markeddirty). The behaviour above was already
    // correct before that ticket; what was missing was any way to see it from the response,
    // which is what made the guarantee uncheckable without a second RPC.
    TestMarkedDirtyMatchesObservedState(*this, TEXT("property.reset markDirty:false"),
        Capture.Result, bPackageDirty);

    bool bApplied = false;
    TestTrue(TEXT("response carries applied"),
        Capture.Result->TryGetBoolField(TEXT("applied"), bApplied));
    TestTrue(TEXT("response reports applied:true"), bApplied);

    // Mark-dirty-only, exactly like property.set: applied/markedDirty never imply a write.
    bool bSaved = false;
    const bool bHasSaved = Capture.Result->TryGetBoolField(TEXT("saved"), bSaved);
    TestFalse(TEXT("property.reset must not claim saved:true (mark-dirty-only)"),
        bHasSaved && bSaved);

    // Disk truth, same reasoning as the property.set cases: IsDirty() above cannot tell
    // "left clean" from "made clean by saving", and property.reset is mark-dirty-only too.
    TestAssetUntouchedOnDisk(*this, Fixture, TEXT("property.reset markDirty:false"));

    // Only mtime+size here, no applied-value byte probe: property.reset's applied value IS
    // the class default, and a class default is exactly the kind of literal that can occur
    // incidentally elsewhere in a package's bytes — an absent-pattern assertion on it could
    // fail for a reason unrelated to persistence. The seeded value must still be there
    // though, which is the half of the byte check that carries no such ambiguity.
    TArray<uint8> FileBytes;
    if (TestTrue(TEXT("fixture .uasset re-read from disk for the byte check"),
            FFileHelper::LoadFileToArray(FileBytes, *Fixture.Filename)))
    {
        TestTrue(TEXT("on-disk .uasset still holds the SEEDED FadeInTime bytes after reset"),
            BytesContainPattern(FileBytes, FloatBytePattern(SeedFadeInTime)));
    }

    return true;
}

// ===========================================================================
// property.reset — markDirty:true must dirty the package AND say so (counterfactual).
// ===========================================================================
// The other polarity of the sibling test above. Without it, a "fix" that stamps a constant
// false — or one that never dirties anything — satisfies every markedDirty assertion in the
// reset half of this file.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyResetMarkDirtyTrueDirtiesPackageTest,
    "PinWright.property.reset.MarkDirtyTrueDirtiesPackage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyResetMarkDirtyTrueDirtiesPackageTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor unavailable (asset fixture requires the editor); "
                "skipping property.reset MarkDirtyTrueDirtiesPackage."));
        return true;
    }

    FCleanAssetFixture Fixture = MakeSavedCleanFixture(*this, TEXT("SM_ResetDirty"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Fixture.PackagePath);
    };
    if (!Fixture.bReady)
    {
        return false;
    }

    const float ClassDefaultFadeInTime = GetDefault<USoundMix>()->FadeInTime;
    const bool bSeedDiffersFromDefault =
        !FMath::IsNearlyEqual(Fixture.Asset->FadeInTime, ClassDefaultFadeInTime);
    TestTrue(TEXT("seeded value differs from the class default (reset is a real change)"),
        bSeedDiffersFromDefault);
    if (!bSeedDiffersFromDefault)
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!InvokePropertyMutator(*this, TEXT("property.reset"),
            MakeResetPayload(Fixture, /*bMarkDirty=*/true), Capture))
    {
        return false;
    }

    TestEqual(TEXT("property.reset restored the class default value"),
        Fixture.Asset->FadeInTime, ClassDefaultFadeInTime);

    // property.reset genuinely dirties — this is the fact markedDirty has to report.
    const bool bPackageDirty = Fixture.Package->IsDirty();
    TestTrue(TEXT("property.reset markDirty:true leaves the package DIRTY"), bPackageDirty);

    TestMarkedDirtyMatchesObservedState(*this, TEXT("property.reset markDirty:true"),
        Capture.Result, bPackageDirty);

    // Dirty still means "not yet written": property.reset never saves.
    TestAssetUntouchedOnDisk(*this, Fixture, TEXT("property.reset markDirty:true"));

    return true;
}

// ===========================================================================
// property.reset — markDirty:false over PRE-EXISTING dirt must report markedDirty:true.
// ===========================================================================
// The discriminating case, and the reason markedDirty is worth reporting at all: the request
// says false and the honest answer is true. A handler that echoed its markDirty parameter —
// which is what property.set did before B-property-set-markdirty-false-still-dirties — passes
// both tests above and fails only here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyResetMarkDirtyFalsePreservesPreexistingDirtTest,
    "PinWright.property.reset.MarkDirtyFalsePreservesPreexistingDirt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyResetMarkDirtyFalsePreservesPreexistingDirtTest::RunTest(const FString& Parameters)
{
    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor unavailable (asset fixture requires the editor); "
                "skipping property.reset MarkDirtyFalsePreservesPreexistingDirt."));
        return true;
    }

    FCleanAssetFixture Fixture = MakeSavedCleanFixture(*this, TEXT("SM_ResetPreDirty"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(Fixture.PackagePath);
    };
    if (!Fixture.bReady)
    {
        return false;
    }

    // Dirt this handler did not create — an unsaved user edit or a concurrent editor action.
    Fixture.Package->MarkPackageDirty();
    TestTrue(TEXT("package is dirty before the markDirty:false reset"),
        Fixture.Package->IsDirty());
    if (!Fixture.Package->IsDirty())
    {
        return false;
    }

    FTestResponseCapture Capture;
    if (!InvokePropertyMutator(*this, TEXT("property.reset"),
            MakeResetPayload(Fixture, /*bMarkDirty=*/false), Capture))
    {
        return false;
    }

    const bool bPackageDirty = Fixture.Package->IsDirty();
    TestTrue(TEXT("property.reset markDirty:false preserves pre-existing dirt"), bPackageDirty);

    // markDirty:false, markedDirty:true — the contradiction a request-echo cannot produce.
    TestMarkedDirtyMatchesObservedState(*this, TEXT("property.reset markDirty:false over dirt"),
        Capture.Result, bPackageDirty);

    TestAssetUntouchedOnDisk(*this, Fixture, TEXT("property.reset markDirty:false over dirt"));

    return true;
}
