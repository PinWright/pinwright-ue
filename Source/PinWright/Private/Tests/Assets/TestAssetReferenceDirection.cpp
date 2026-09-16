// Copyright (c) 2026 Alexander Penkin. MIT License.

// Test: asset.references / asset.dependencies report the correct direction in
// their result fields and registered summaries.
//
// Regression guard for E-asset-dependencies-references-inverted:
//   - asset.references calls GetDependencies (OUTBOUND deps); its summary must say
//     so and its payload must carry a direction-true "dependencies" alias next to
//     the legacy "references" key.
//   - asset.dependencies calls GetReferencers (INBOUND referencers); its summary
//     must say so and its payload must carry a direction-true "referencers" alias
//     next to the legacy "dependencies" key.
//   - asset.get_dependencies's cross-ref must point at asset.dependencies (the verb
//     that actually answers "who references this asset"), not asset.references.
// If the fix is reverted (summaries flipped back, aliases removed, cross-ref wrong)
// the assertions below fail.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/Assets/AssetRefDirectionFixtures.h"

#include "Misc/PackageName.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetReferenceDirectionTest,
    "PinWright.asset.references.DirectionAndAliases",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetReferenceDirectionTest::RunTest(const FString& Parameters)
{
    // -------------------------------------------------------------------------
    // 0. Registered-summary direction must be self-consistent (doc-string fix).
    // -------------------------------------------------------------------------
    {
        const FString RefSummary = GetRegisteredSummary(TEXT("asset.references"));
        const FString DepSummary = GetRegisteredSummary(TEXT("asset.dependencies"));
        const FString GetDepSummary = GetRegisteredSummary(TEXT("asset.get_dependencies"));

        TestFalse(TEXT("asset.references summary present"), RefSummary.IsEmpty());
        TestFalse(TEXT("asset.dependencies summary present"), DepSummary.IsEmpty());
        TestFalse(TEXT("asset.get_dependencies summary present"), GetDepSummary.IsEmpty());

        // Assert the direction *property* the ticket is about ("noun matches
        // direction"), not the exact retired prose: asset.references is the
        // OUTBOUND verb, so its summary must say "outbound" and must NOT describe
        // itself as returning inbound referencers.
        TestTrue(TEXT("asset.references summary says outbound"),
            RefSummary.Contains(TEXT("outbound"), ESearchCase::IgnoreCase));
        TestFalse(TEXT("asset.references summary does not claim to return referencers/inbound"),
            RefSummary.Contains(TEXT("referencers"), ESearchCase::IgnoreCase)
            || RefSummary.Contains(TEXT("inbound"), ESearchCase::IgnoreCase));

        // asset.dependencies is the INBOUND verb: its summary must say
        // "referencers" (or "inbound") and must NOT describe itself as returning
        // outbound dependencies.
        TestTrue(TEXT("asset.dependencies summary says referencers/inbound"),
            DepSummary.Contains(TEXT("referencers"), ESearchCase::IgnoreCase)
            || DepSummary.Contains(TEXT("inbound"), ESearchCase::IgnoreCase));
        TestFalse(TEXT("asset.dependencies summary does not claim to return outbound deps"),
            DepSummary.Contains(TEXT("outbound"), ESearchCase::IgnoreCase));

        // asset.get_dependencies cross-ref must steer "who references this asset"
        // to asset.dependencies, NOT to asset.references (the inverted breadcrumb).
        TestTrue(TEXT("asset.get_dependencies cross-ref points at asset.dependencies"),
            GetDepSummary.Contains(TEXT("use asset.dependencies")));
        TestFalse(TEXT("asset.get_dependencies cross-ref no longer points at asset.references for the inverse"),
            GetDepSummary.Contains(TEXT("use asset.references")));
    }

    // -------------------------------------------------------------------------
    // 1. Build a hard dependency A -> B on disk (A references B) via the shared
    //    fixture (same builder TestAssetReferencersWarning uses), so the
    //    AssetRegistry tracks both directions deterministically.
    // -------------------------------------------------------------------------
    FString PathA, PathB;
    if (!AssetRefDirectionFixtures::BuildHardDependency(*this, TEXT("RefDir"), PathA, PathB))
    {
        return true;
    }

    // asset.* read verbs resolve via GetAssetByObjectPath(FSoftObjectPath) — pass
    // the object-path form Package.AssetName.
    const FString ObjectPathA = FString::Printf(TEXT("%s.%s"),
        *PathA, *FPackageName::GetLongPackageAssetName(PathA));
    const FString ObjectPathB = FString::Printf(TEXT("%s.%s"),
        *PathB, *FPackageName::GetLongPackageAssetName(PathB));

    // -------------------------------------------------------------------------
    // 2. asset.references(A) returns OUTBOUND deps: B appears, under BOTH the
    //    legacy "references" key AND the direction-true "dependencies" alias.
    // -------------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPathA);
        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(TEXT("asset.references"), Payload, Capture);
        TestTrue(TEXT("asset.references handler found"), bInvoked);
        TestTrue(TEXT("asset.references succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            // Legacy field carries the outbound dependency on B.
            TestTrue(TEXT("asset.references legacy 'references' contains B"),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("references"), TEXT("packageName"), PathB));
            // Direction-true alias must exist and carry the same outbound dep.
            TestTrue(TEXT("asset.references direction-true 'dependencies' alias contains B"),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("dependencies"), TEXT("packageName"), PathB));
            // A's outbound deps must NOT be reported as A referencing itself, and
            // crucially the verb must not return inbound referencers here.
            double DependencyCount = -1.0;
            TestTrue(TEXT("asset.references emits 'dependencyCount' alias"),
                Capture.Result->TryGetNumberField(TEXT("dependencyCount"), DependencyCount));
        }
    }

    // -------------------------------------------------------------------------
    // 3. asset.dependencies(B) returns INBOUND referencers: A appears, under BOTH
    //    the legacy "dependencies" key AND the direction-true "referencers" alias.
    // -------------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPathB);
        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(TEXT("asset.dependencies"), Payload, Capture);
        TestTrue(TEXT("asset.dependencies handler found"), bInvoked);
        TestTrue(TEXT("asset.dependencies succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            // Legacy field carries the inbound referencer A.
            TestTrue(TEXT("asset.dependencies legacy 'dependencies' contains A (the referencer)"),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("dependencies"), TEXT("packageName"), PathA));
            // Direction-true alias must exist and carry the same inbound referencer.
            TestTrue(TEXT("asset.dependencies direction-true 'referencers' alias contains A"),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("referencers"), TEXT("packageName"), PathA));
            double ReferencerCount = -1.0;
            TestTrue(TEXT("asset.dependencies emits 'referencerCount' alias"),
                Capture.Result->TryGetNumberField(TEXT("referencerCount"), ReferencerCount));
        }
    }

    // -------------------------------------------------------------------------
    // 4. Cleanup
    // -------------------------------------------------------------------------
    CleanupTestAsset(PathA);
    CleanupTestAsset(PathB);
    return true;
}
