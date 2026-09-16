// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-asset-reload-from-disk.
//
// asset.reload force-evicts a loaded asset package and re-serializes it from the
// on-disk .uasset, so a subsequent read reflects the persisted bytes rather than the
// live in-memory UObject. This is the read-side verification capability the corruption
// tickets kept SKIP'ing on: before it, the only in-session "confirm the saved asset is
// clean" path was asset.save -> blueprint.decompile, which re-reads the SAME in-memory
// object (asset.validate has the same blind spot — its LoadAsset returns the resident
// UObject and never evicts).
//
// Coverage:
//   1. asset.reload is registered with category "asset".
//   2. asset.reload with no assetPath -> INVALID_ARGUMENT (RequireAssetPathRaw).
//   3. asset.reload with a nonexistent path -> ASSET_NOT_FOUND.
//   4. From-disk semantics: create a SoundClass with Volume=0.5, SAVE it to disk, then
//      mutate the RESIDENT object's Volume to 0.123 in memory WITHOUT saving, then
//      asset.reload. The reloaded object must read back the on-disk 0.5, proving the
//      package was evicted and re-read from disk (the disk bytes win). If asset.reload
//      were reverted to a validate-style re-LoadAsset (no eviction) or removed entirely,
//      the in-memory 0.123 would survive and this assertion fails.
//   5. Crash regression (board B-asset-reload-blueprint-package-crash /
//      B-asset-reload-access-violation-kills-editor): the handler must answer
//      SYNCHRONOUSLY, on the caller's own stack. It used to hand its body to
//      AsyncTask(ENamedThreads::GameThread, ...), which ran UPackageTools::ReloadPackages
//      outside FRpcDispatcher's bProcessingRequest reentrancy guard and outside the
//      tick-unsafe gate, on whatever named-thread pump drained it. ReloadPackages pumps
//      while it works (FlushAsyncLoading, a global component reregister, two
//      CollectGarbage passes) and holds a raw UObject* snapshot of the whole object graph
//      across those pumps, so another RPC drained into the middle of one freed objects
//      the snapshot still pointed at — an access violation walking a UFunction's bytecode
//      in FPropertyProxyArchive::operator<<, taking the shared editor with it.
//      Asserted below by checking bWasCalled BEFORE any pump: a re-introduced AsyncTask
//      continuation leaves the capture unpopulated at that point and fails here.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "Sound/SoundClass.h"

#include "Tests/TestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAssetReloadHandlerTest,
    "PinWright.asset.reload",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetReloadHandlerTest::RunTest(const FString& Parameters)
{
    // ------------------------------------------------------------------
    // 1. asset.reload must be registered with category "asset".
    // ------------------------------------------------------------------
    {
        bool bFound = false;
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName == TEXT("asset.reload"))
            {
                bFound = true;
                TestEqual(TEXT("asset.reload category"), Reg.Category, TEXT("asset"));
                TestTrue(TEXT("asset.reload func pointer non-null"), Reg.Func != nullptr);
                break;
            }
        }
        TestTrue(TEXT("asset.reload is registered"), bFound);
    }

    // ------------------------------------------------------------------
    // 2. asset.reload with no assetPath -> INVALID_ARGUMENT.
    // ------------------------------------------------------------------
    {
        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        TestTrue(TEXT("asset.reload handler found (missing param)"),
            InvokeHandlerWithSharedCapture(TEXT("asset.reload"), Params, Capture));
        PumpUntilCaptured(*Capture, 5.0);
        TestTrue(TEXT("asset.reload missing assetPath responded"), Capture->bWasCalled);
        TestFalse(TEXT("asset.reload missing assetPath is not a success"), Capture->bSuccess);
        TestEqual(TEXT("asset.reload missing assetPath error code"),
            Capture->ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    }

    // ------------------------------------------------------------------
    // 3. asset.reload with a nonexistent asset path -> ASSET_NOT_FOUND.
    // ------------------------------------------------------------------
    {
        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), TEXT("/Game/Test/DoesNotExistAsset_XYZZY_Reload"));
        TestTrue(TEXT("asset.reload handler found (not found)"),
            InvokeHandlerWithSharedCapture(TEXT("asset.reload"), Params, Capture));

        // Coverage item 5, checked BEFORE the pump: the existence probe lives past the
        // point where the old AsyncTask continuation began, so a re-introduced marshal
        // leaves this unpopulated on the caller's stack.
        TestTrue(TEXT("asset.reload answers on the caller's stack (no AsyncTask continuation)"),
            Capture->bWasCalled);

        PumpUntilCaptured(*Capture, 5.0);
        TestTrue(TEXT("asset.reload not_found responded"), Capture->bWasCalled);
        TestFalse(TEXT("asset.reload not_found is not a success"), Capture->bSuccess);
        TestEqual(TEXT("asset.reload not_found error code"),
            Capture->ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    }

    // ------------------------------------------------------------------
    // 4. From-disk semantics: reload discards an unsaved in-memory edit and
    //    reads back the on-disk value.
    // ------------------------------------------------------------------
    {
        const FString AssetName = FString::Printf(
            TEXT("SC_Reload_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PkgPath = FString::Printf(TEXT("/Game/__PW_GatewayTests/%s"), *AssetName);
        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PkgPath, *AssetName);

        UPackage* Pkg = CreatePackage(*PkgPath);
        if (TestNotNull(TEXT("test package created"), Pkg))
        {
            USoundClass* SC = NewObject<USoundClass>(
                Pkg, *AssetName, RF_Public | RF_Standalone);
            if (TestNotNull(TEXT("SoundClass created"), SC))
            {
                FAssetRegistryModule::AssetCreated(SC);

                // The value we persist to disk.
                SC->Properties.Volume = 0.5f;
                Pkg->MarkPackageDirty();

                const bool bSaved =
                    UEditorAssetLibrary::SaveLoadedAsset(SC, /*bOnlyIfIsDirty=*/false);
                TestTrue(TEXT("SoundClass saved via SaveLoadedAsset"), bSaved);

                // Hard disk-presence proof — reload can only re-read a real .uasset.
                const FString Filename = FPackageName::LongPackageNameToFilename(
                    PkgPath, FPackageName::GetAssetPackageExtension());
                const int64 OnDiskSize = IFileManager::Get().FileSize(*Filename);
                TestTrue(TEXT("SoundClass .uasset is on disk after save"), OnDiskSize > 0);

                if (OnDiskSize > 0)
                {
                    // Mutate the RESIDENT object in memory only — do NOT save. A blind
                    // re-LoadAsset (validate's behavior) would keep this value.
                    SC->Properties.Volume = 0.123f;

                    TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
                    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
                    Params->SetStringField(TEXT("assetPath"), ObjectPath);
                    TestTrue(TEXT("asset.reload handler found (round-trip)"),
                        InvokeHandlerWithSharedCapture(TEXT("asset.reload"), Params, Capture));

                    // Coverage item 5 on the REAL eviction path: UPackageTools::ReloadPackages
                    // must have run and answered before the handler returned, so the whole
                    // reload sits inside the dispatcher's reentrancy guard instead of on a
                    // detached named-thread continuation.
                    TestTrue(TEXT("asset.reload runs ReloadPackages on the caller's stack"),
                        Capture->bWasCalled);

                    PumpUntilCaptured(*Capture, 20.0);

                    TestTrue(TEXT("asset.reload round-trip responded"), Capture->bWasCalled);
                    TestTrue(TEXT("asset.reload round-trip succeeded"), Capture->bSuccess);
                    if (Capture->bSuccess && Capture->Result.IsValid())
                    {
                        bool bReloaded = false;
                        Capture->Result->TryGetBoolField(TEXT("reloaded"), bReloaded);
                        TestTrue(TEXT("asset.reload reports reloaded:true"), bReloaded);

                        bool bWasLoaded = false;
                        Capture->Result->TryGetBoolField(TEXT("wasLoaded"), bWasLoaded);
                        TestTrue(TEXT("asset.reload reports wasLoaded:true (package was resident)"),
                            bWasLoaded);
                    }

                    // Re-resolve — the pre-reload SC pointer is stale after eviction.
                    USoundClass* ReloadedSC =
                        Cast<USoundClass>(UEditorAssetLibrary::LoadAsset(ObjectPath));
                    if (TestNotNull(TEXT("reloaded SoundClass resolves"), ReloadedSC))
                    {
                        // The on-disk value (0.5) must win over the discarded in-memory
                        // edit (0.123). This is the core proof of an evict + re-read.
                        TestEqual(TEXT("reload reverted the in-memory edit to the on-disk value"),
                            ReloadedSC->Properties.Volume, 0.5f);
                    }
                }
            }
        }
        CleanupTestAsset(PkgPath);
    }

    return true;
}
