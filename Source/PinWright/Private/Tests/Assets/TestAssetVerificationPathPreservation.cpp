// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board ticket B-asset-verification-clobbers-handler-assetpath.
//
// WriteMeasuredAssetVerification wrote assetPath unconditionally, and AddAssetVerification
// runs AFTER a handler has built its payload - so any handler that had already set assetPath
// had its value silently replaced by the bare package path. A census over Source found 322
// direct call sites, 50 of which set assetPath first, ~35 of them from GetPathName(): an
// object path downgraded to a package path with nothing in the response saying so. Which
// ordering a verb got was decided by accident, so the fix has to be structural rather than
// per-verb reordering.
//
// The rule now: the handler's assetPath is KEPT when it denotes the same package as the object
// that was verified, and REPLACED otherwise - with requestedAssetPath and
// assetPathSubstituted:true recording that it happened. These tests are failure-direction in
// both directions: restore the unconditional write and the first test sees the package path
// where it requires the handler's object path; drop the substitution signal and the second
// test finds no evidence that its foreign path was swapped out.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Utils/AssetUtils.h"

#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Materials/Material.h"

namespace AssetVerificationPathTestUtils
{
    inline FString MakeProbePackagePath()
    {
        return FString::Printf(TEXT("/Game/PinWrightTests/VerifyPath_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // A live, never-written /Game package holding a top-level object. Top-level is the case
    // that matters: ResolveVerificationAssetPath returns the bare package path for it, which
    // is precisely the value that used to overwrite the handler's object path. (A sub-object
    // asset already got GetPathName() from the helper, so it could not show the defect.)
    inline UObject* MakeTopLevelProbeAsset(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        // UObject::StaticClass() is abstract; allocating one in a package trips the engine
        // ensure at UObjectGlobals. This test cares about path preservation, not class.
        UObject* Asset = NewObject<UMaterial>(
            Package, *FPackageName::GetLongPackageAssetName(PackagePath),
            RF_Public | RF_Standalone);
        if (Asset)
        {
            Asset->AddToRoot();
        }
        return Asset;
    }
}

// ============================================================================
// A handler's own assetPath survives the helper that runs after it.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetVerificationKeepsHandlerAssetPathTest,
    "PinWright.core.asset_save_honesty.add_asset_verification.HandlerAssetPathSurvives",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetVerificationKeepsHandlerAssetPathTest::RunTest(const FString& Parameters)
{
    using namespace AssetVerificationPathTestUtils;

    const FString PackagePath = MakeProbePackagePath();
    UObject* Asset = MakeTopLevelProbeAsset(PackagePath);
    if (!TestNotNull(TEXT("built a top-level /Game probe asset"), Asset))
    {
        return false;
    }
    UPackage* Package = Asset->GetPackage();
    ON_SCOPE_EXIT
    {
        // Leave no dirty /Game package behind for the editor's save-on-exit prompt.
        if (Package)
        {
            Package->SetDirtyFlag(false);
        }
        Asset->RemoveFromRoot();
    };

    const FString ObjectPath = Asset->GetPathName();
    // Calibration: the two forms really are different here, or the test proves nothing.
    TestNotEqual(TEXT("the object path and the package path differ for a top-level asset"),
        ObjectPath, PackagePath);

    // The exact shape ~35 production call sites have: assetPath set from GetPathName(),
    // then the verification helper called.
    {
        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        Response->SetStringField(TEXT("assetPath"), ObjectPath);
        AddAssetVerification(Response, Asset);

        FString Reported;
        TestTrue(TEXT("assetPath is present"),
            Response->TryGetStringField(TEXT("assetPath"), Reported));
        TestEqual(TEXT("the handler's object path survives AddAssetVerification (it used to be "
                       "replaced by the bare package path)"),
            Reported, ObjectPath);
        TestFalse(TEXT("nothing was substituted, so no substitution signal is emitted"),
            Response->HasField(TEXT("assetPathSubstituted")));
        TestFalse(TEXT("and no requestedAssetPath is emitted either"),
            Response->HasField(TEXT("requestedAssetPath")));
    }

    // The package-path spelling is the same package, so it is equally preserved: the rule is
    // "same package", not "must be an object path".
    {
        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        Response->SetStringField(TEXT("assetPath"), PackagePath);
        AddAssetVerification(Response, Asset);
        TestEqual(TEXT("a package-path spelling of the same asset is preserved too"),
            Response->GetStringField(TEXT("assetPath")), PackagePath);
    }

    // A handler that sets nothing still gets the measured value, unchanged from before.
    {
        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        AddAssetVerification(Response, Asset);
        TestEqual(TEXT("an unset assetPath is filled in with the measured package path"),
            Response->GetStringField(TEXT("assetPath")), PackagePath);
        TestFalse(TEXT("and that is not reported as a substitution"),
            Response->HasField(TEXT("assetPathSubstituted")));
    }

    // The package handle is always available under its own key, whichever form assetPath took.
    {
        TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
        Response->SetStringField(TEXT("assetPath"), ObjectPath);
        AddAssetVerification(Response, Asset);
        FString PackageName;
        TestTrue(TEXT("packageName is always emitted"),
            Response->TryGetStringField(TEXT("packageName"), PackageName));
        TestEqual(TEXT("and it is the package handle, not the folder"), PackageName, PackagePath);
    }

    return true;
}

// ============================================================================
// A value naming a DIFFERENT asset is corrected - and the correction is visible.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetVerificationReplacesForeignAssetPathTest,
    "PinWright.core.asset_save_honesty.add_asset_verification.ForeignAssetPathIsReplacedVisibly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetVerificationReplacesForeignAssetPathTest::RunTest(const FString& Parameters)
{
    using namespace AssetVerificationPathTestUtils;

    const FString PackagePath = MakeProbePackagePath();
    UObject* Asset = MakeTopLevelProbeAsset(PackagePath);
    if (!TestNotNull(TEXT("built a top-level /Game probe asset"), Asset))
    {
        return false;
    }
    UPackage* Package = Asset->GetPackage();
    ON_SCOPE_EXIT
    {
        if (Package)
        {
            Package->SetDirtyFlag(false);
        }
        Asset->RemoveFromRoot();
    };

    // Preserving this blindly would be the opposite failure: echoing an unverified caller
    // string as though it were the measured handle.
    const FString ForeignPath = TEXT("/Game/PinWrightTests/SomeOtherAssetEntirely");
    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("assetPath"), ForeignPath);
    AddAssetVerification(Response, Asset);

    TestEqual(TEXT("a path naming a different package is replaced by the measured one"),
        Response->GetStringField(TEXT("assetPath")), PackagePath);

    bool bSubstituted = false;
    TestTrue(TEXT("the substitution is reported rather than silent"),
        Response->TryGetBoolField(TEXT("assetPathSubstituted"), bSubstituted));
    TestTrue(TEXT("and the flag says it happened"), bSubstituted);

    FString Requested;
    TestTrue(TEXT("the replaced value is preserved as requestedAssetPath"),
        Response->TryGetStringField(TEXT("requestedAssetPath"), Requested));
    TestEqual(TEXT("and it is what the handler had written"), Requested, ForeignPath);

    return true;
}
