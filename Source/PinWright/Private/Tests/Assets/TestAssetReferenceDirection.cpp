// Copyright (c) 2026 Alexander Penkin. MIT License.

// Test: asset.references / asset.dependencies report the correct direction in
// their result fields and registered summaries.
//
// Regression guard for E-asset-dependencies-references-inverted and
// B-asset-references-returns-dependencies-not-referencers:
//   - asset.references reports BOTH directions under explicit keys: OUTBOUND
//     "dependencies" (GetDependencies) and INBOUND "referencers" (GetReferencers).
//     The legacy "references" key carries the referencers; it used to repeat the
//     outbound list byte-for-byte, so a pre-delete "who uses this?" check read the
//     asset's own dependencies instead.
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

#include "AssetRegistry/AssetRegistryModule.h"
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

        // asset.references reports both directions, so its summary must name
        // both (B-asset-references-returns-dependencies-not-referencers retired
        // the outbound-only wording).
        TestTrue(TEXT("asset.references summary names the outbound dependencies"),
            RefSummary.Contains(TEXT("outbound"), ESearchCase::IgnoreCase));
        TestTrue(TEXT("asset.references summary names the referencers"),
            RefSummary.Contains(TEXT("referencers"), ESearchCase::IgnoreCase));

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

    // Fixture precondition, read straight from the registry: the A -> B edge
    // exists in both directions, so a wrong verb answer below is the verb's fault.
    {
        IAssetRegistry& Registry =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TArray<FName> RegDepsOfA, RegRefsOfB;
        Registry.GetDependencies(FName(*PathA), RegDepsOfA);
        Registry.GetReferencers(FName(*PathB), RegRefsOfB);
        if (!TestTrue(TEXT("fixture: registry records A depends on B"), RegDepsOfA.Contains(FName(*PathB)))
            || !TestTrue(TEXT("fixture: registry records B is referenced by A"), RegRefsOfB.Contains(FName(*PathA))))
        {
            CleanupTestAsset(PathA);
            CleanupTestAsset(PathB);
            return true;
        }
    }

    // -------------------------------------------------------------------------
    // 2. asset.references reports each direction under its own key. Query both
    //    ends of the A -> B edge: on A, B is a dependency and NOT a referencer;
    //    on B, A is a referencer (also under legacy "references") and NOT a
    //    dependency. Before the fix "references" == "dependencies" on every
    //    asset, so B's "references" lacked A and A's "references" held B.
    // -------------------------------------------------------------------------
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPathA);
        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(TEXT("asset.references"), Payload, Capture);
        TestTrue(TEXT("asset.references(A) handler found"), bInvoked);
        TestTrue(TEXT("asset.references(A) succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            TestTrue(TEXT("asset.references(A) 'dependencies' contains B"),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("dependencies"), TEXT("packageName"), PathB));
            TestFalse(TEXT("asset.references(A) 'referencers' does not contain B (B does not reference A)"),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("referencers"), TEXT("packageName"), PathB));
            TestFalse(TEXT("asset.references(A) legacy 'references' does not contain B (it is not the outbound list)"),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("references"), TEXT("packageName"), PathB));
            double DependencyCount = -1.0;
            TestTrue(TEXT("asset.references(A) emits 'dependencyCount'"),
                Capture.Result->TryGetNumberField(TEXT("dependencyCount"), DependencyCount));
        }
    }
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPathB);
        FTestResponseCapture Capture;
        const bool bInvoked = InvokeHandlerWithCapture(TEXT("asset.references"), Payload, Capture);
        TestTrue(TEXT("asset.references(B) handler found"), bInvoked);
        TestTrue(TEXT("asset.references(B) succeeded"), Capture.bSuccess);

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            TestTrue(TEXT("asset.references(B) 'referencers' contains A"),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("referencers"), TEXT("packageName"), PathA));
            TestTrue(TEXT("asset.references(B) legacy 'references' contains A"),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("references"), TEXT("packageName"), PathA));
            TestFalse(TEXT("asset.references(B) 'dependencies' does not contain A (B does not depend on A)"),
                JsonArrayHasObjectWithStringField(Capture.Result, TEXT("dependencies"), TEXT("packageName"), PathA));
            double ReferencerCount = -1.0, ReferenceCount = -1.0;
            TestTrue(TEXT("asset.references(B) emits 'referencerCount'"),
                Capture.Result->TryGetNumberField(TEXT("referencerCount"), ReferencerCount));
            TestTrue(TEXT("asset.references(B) emits 'referenceCount'"),
                Capture.Result->TryGetNumberField(TEXT("referenceCount"), ReferenceCount));
            TestTrue(TEXT("asset.references(B) referencerCount >= 1"), ReferencerCount >= 1.0);
            TestEqual(TEXT("asset.references(B) referenceCount == referencerCount"), ReferenceCount, ReferencerCount);
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
