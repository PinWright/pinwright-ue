// Copyright (c) 2026 Alexander Penkin. MIT License.

// Behavioural regression guard for B-force-delete-nulls-referencers.
//
// The defect: asset.delete routed every path through UEditorAssetLibrary::DeleteAsset,
// which is ObjectTools::ForceDeleteObjects with bShowConfirmation=false. That runs
// ForceReplaceReferences(nullptr, ...) BEFORE the engine decides whether the package may
// go - a walk over every live UObject that replaces each pointer to the doomed asset with
// null and calls MarkPackageDirty() on every object it touched (UE 5.8 ObjectTools.cpp
// :1470). It is not transacted and the replace archive issues no Modify(), so it cannot be
// undone; when CleanupAfterSuccessfulDelete then declines the package the destruction is
// left unpaired - the .uasset survives and only the pointers to it are gone.
//
// What this test judges on is the SIDE EFFECT, not the response text: A's package must
// still be clean after a refused delete of B. Before the fix, ForceReplaceReferences
// dirties it (and B is deleted outright, because the nulling removed the very reference the
// cull would have seen), so both halves of case 1 fail. The response fields are asserted
// too, but a fix that only renamed fields would still fail the dirty-flag assertion.
//
// Case 2 is the other direction: the gate must not have turned asset.delete into a verb
// that refuses everything. A is referenced by nothing, so it still deletes.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Dispatch/ScopedUnattendedRpc.h"
#include "Tests/TestUtils.h"
#include "Tests/Assets/AssetRefDirectionFixtures.h"
#include "Utils/AssetUtils.h"

#include "EditorAssetLibrary.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDeleteRefusesReferencedTest,
    "PinWright.asset.delete.RefusesReferencedInsteadOfNullingReferences",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDeleteRefusesReferencedTest::RunTest(const FString& Parameters)
{
    // The refused delete logs the engine's own "Could not delete" warning.
    bSuppressLogErrors = true;

    FString PathA, PathB;
    if (!AssetRefDirectionFixtures::BuildHardDependency(*this, TEXT("DeleteGate"), PathA, PathB))
    {
        return true;
    }

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PathA);
        CleanupTestAsset(PathB);
    };

    UPackage* PackageA = FindPackage(nullptr, *PathA);
    if (!TestNotNull(TEXT("referencing package A is loaded"), PackageA))
    {
        return true;
    }
    // The fixture saves A last, so it starts clean. If a sibling left it dirty the
    // dirty-flag assertion below would measure nothing, so it is skipped rather than
    // asserted on a bad precondition.
    const bool bAWasCleanBeforeDelete = !PackageA->IsDirty();
    TestTrue(TEXT("referencing package A starts clean (precondition)"), bAWasCleanBeforeDelete);

    // -------------------------------------------------------------------------
    // Case 1: deleting B, which A references, refuses and changes nothing.
    // -------------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), PathB);

        FTestResponseCapture Capture;
        bool bInvoked = false;
        {
            // InvokeHandlerWithCapture calls the body directly, outside the dispatcher's
            // own scope, so the engine's delete-path dialogs are unguarded without this.
            FScopedUnattendedRpc UnattendedScope;
            bInvoked = InvokeHandlerWithCapture(TEXT("asset.delete"), Payload, Capture);
        }
        if (!TestTrue(TEXT("asset.delete handler found"), bInvoked)
            || !TestTrue(TEXT("asset.delete response carries a result"), Capture.Result.IsValid()))
        {
            return true;
        }

        // The asset must survive. Before the fix the force path nulled A's reference first,
        // which is exactly what let the delete through.
        TestTrue(TEXT("a referenced asset is not deleted"),
            UEditorAssetLibrary::DoesAssetExist(PathB));

        // The behavioural core: nothing was touched. ForceReplaceReferences marks every
        // package it wrote into dirty, so a clean A proves the walk never ran.
        if (bAWasCleanBeforeDelete)
        {
            TestFalse(TEXT("the referencing package was not dirtied by the refused delete"),
                PackageA->IsDirty());
        }

        bool bSuccess = true;
        Capture.Result->TryGetBoolField(TEXT("success"), bSuccess);
        TestFalse(TEXT("a refused delete does not report success"), bSuccess);

        FString TopLevelCode;
        Capture.Result->TryGetStringField(TEXT("errorCode"), TopLevelCode);
        TestEqual(TEXT("the response names ASSET_IN_USE"), TopLevelCode, FString(TEXT("ASSET_IN_USE")));

        const TSharedPtr<FJsonObject> Entry = JsonArrayFindObjectByStringField(
            Capture.Result, TEXT("results"), TEXT("path"), PathB);
        if (TestTrue(TEXT("results[] carries an entry for the requested path"), Entry.IsValid()))
        {
            bool bRefused = false;
            Entry->TryGetBoolField(TEXT("refused"), bRefused);
            TestTrue(TEXT("the entry is marked refused"), bRefused);

            bool bReferencesPreserved = false;
            Entry->TryGetBoolField(TEXT("referencesPreserved"), bReferencesPreserved);
            TestTrue(TEXT("the entry states that references were preserved"), bReferencesPreserved);

            FString EntryCode;
            Entry->TryGetStringField(TEXT("errorCode"), EntryCode);
            TestEqual(TEXT("the entry names ASSET_IN_USE"), EntryCode, FString(TEXT("ASSET_IN_USE")));

            // The referencer that blocked it must be named, or the caller cannot act.
            const TArray<TSharedPtr<FJsonValue>>* Referencers = nullptr;
            bool bNamesA = false;
            if (Entry->TryGetArrayField(TEXT("referencers"), Referencers) && Referencers)
            {
                for (const TSharedPtr<FJsonValue>& Val : *Referencers)
                {
                    if (Val.IsValid() && Val->AsString() == PathA)
                    {
                        bNamesA = true;
                        break;
                    }
                }
            }
            TestTrue(TEXT("the refusal names the referencing package"), bNamesA);
        }
    }

    // -------------------------------------------------------------------------
    // Case 2: the unreferenced path still deletes. A references B, but nothing
    // references A, so the gate must let it through.
    // -------------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), PathA);

        FTestResponseCapture Capture;
        bool bInvoked = false;
        {
            FScopedUnattendedRpc UnattendedScope;
            bInvoked = InvokeHandlerWithCapture(TEXT("asset.delete"), Payload, Capture);
        }
        if (!TestTrue(TEXT("asset.delete handler found"), bInvoked)
            || !TestTrue(TEXT("asset.delete response carries a result"), Capture.Result.IsValid()))
        {
            return true;
        }

        // The disk, not UEditorAssetLibrary::DoesAssetExist. That call reads the asset registry
        // with in-memory objects INCLUDED, and a deleted object outlives its own delete: the
        // engine clears its asset flags, removes the row and the file, then leaves the UObject
        // for a GC the transaction buffer can veto. The registry resolves that leftover and
        // answers "still there" for an asset whose row and file are both already gone - measured
        // on UE 5.4 running the full suite, where the buffer is deep enough to hold it. The
        // .uasset is the fact this delete is supposed to change; the response's own reconciled
        // verdict is asserted just below and is the other half of the measurement.
        TestFalse(TEXT("an unreferenced asset is deleted"),
            DoesPackageFileExistOnDisk(PathA));

        bool bSuccess = false;
        Capture.Result->TryGetBoolField(TEXT("success"), bSuccess);
        TestTrue(TEXT("deleting an unreferenced asset reports success"), bSuccess);

        const TSharedPtr<FJsonObject> Entry = JsonArrayFindObjectByStringField(
            Capture.Result, TEXT("results"), TEXT("path"), PathA);
        if (TestTrue(TEXT("results[] carries an entry for the deleted path"), Entry.IsValid()))
        {
            // Absent reads as false: the field is emitted only on the refusal branch.
            bool bRefused = false;
            Entry->TryGetBoolField(TEXT("refused"), bRefused);
            TestFalse(TEXT("a clean delete is not marked refused"), bRefused);

            bool bDeleted = false;
            Entry->TryGetBoolField(TEXT("deleted"), bDeleted);
            TestTrue(TEXT("the entry reports the delete"), bDeleted);
        }
    }

    return true;
}
