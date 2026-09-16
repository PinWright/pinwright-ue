// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-create-blend-space-axis-config-dropped-on-57.
//
// animation.create_blend_space advertises minX/maxX/gridX (and minY/maxY/gridY)
// and parses them, but on UE 5.7 the old code wrote them only inside
// #if MCP_HAS_BLENDSPACE_BASE — a branch that is dead on 5.7 (UBlendSpaceBase /
// BlendSpaceBase.h are gone), so the asset was created with default 0..1 bounds
// and the call returned success carrying only a warning string. The fix writes
// the axis range/grid to UBlendSpace::BlendParameters via FProperty reflection
// (mirroring create_blend_space_1d/_2d).
//
// This routes the real RPC through FRpcDispatcher::ProcessRequest so it exercises
// the production handler + ApplyBlendSpaceConfiguration, then reads the created
// asset's axis values back via UBlendSpace::GetBlendParameter (the const accessor
// available on 5.7). Counterfactual: revert ApplyBlendSpaceConfiguration to the
// #if-gated UBlendSpaceBase write and on 5.7 the read-back stays at the default
// 0..1 / GridNum 4, failing the TestEqual asserts below.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dispatch/RpcDispatcher.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

using DispatcherTestHelpers::Dispatch;
using DispatcherTestHelpers::MakeDispatcher;

namespace
{
const TCHAR* MannequinAnimBPPathForBlendSpaceAxis =
    TEXT("/Game/Characters/Mannequins/Animations/ABP_Manny.ABP_Manny");

// Resolve a usable skeleton the same way the sibling anim tests do: load the
// host's Mannequin AnimBP (ABP_Manny) and reuse its TargetSkeleton. Host
// absence is gated at the call site via PINWRIGHT_SKIP_IF_FIXTURE_MISSING, so
// a nullptr here means the package exists but failed to load — the caller then
// HARD-FAILS (a broken fixture is never a fake pass).
USkeleton* LoadFixtureSkeletonForBlendSpaceAxis()
{
    UAnimBlueprint* Fixture =
        LoadObject<UAnimBlueprint>(nullptr, MannequinAnimBPPathForBlendSpaceAxis);
    return Fixture ? Fixture->TargetSkeleton : nullptr;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCreateBlendSpaceAppliesAxisConfigTest,
    "PinWright.animation.CreateBlendSpaceAppliesAxisConfig",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCreateBlendSpaceAppliesAxisConfigTest::RunTest(const FString& Parameters)
{
    PINWRIGHT_SKIP_IF_FIXTURE_MISSING(MannequinAnimBPPathForBlendSpaceAxis);
    USkeleton* Skeleton = LoadFixtureSkeletonForBlendSpaceAxis();
    if (!TestNotNull(TEXT("Mannequin AnimBP skeleton fixture loaded"), Skeleton)) return false;

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SavePath = TEXT("/Game/PinWrightTests");
    const FString Name = FString::Printf(TEXT("BS_AxisCfg_%s"), *Guid);
    const FString PackagePath = FString::Printf(TEXT("%s/%s"), *SavePath, *Name);
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *Name);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PackagePath);
    };

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // 1D blend space with a non-default Speed axis 0..600 and 8 grid divisions.
    const double RequestedMin = 0.0;
    const double RequestedMax = 600.0;
    const int32 RequestedGrid = 8;

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("name"), Name);
    Params->SetStringField(TEXT("skeletonPath"), Skeleton->GetPathName());
    Params->SetStringField(TEXT("savePath"), SavePath);
    Params->SetNumberField(TEXT("dimensions"), 1);
    Params->SetNumberField(TEXT("minX"), RequestedMin);
    Params->SetNumberField(TEXT("maxX"), RequestedMax);
    Params->SetNumberField(TEXT("gridX"), RequestedGrid);

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("animation.create_blend_space"),
        TEXT("req-bs-axis-config"), Params, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("create_blend_space succeeded"), bSuccess);
    TestEqual(TEXT("no error code"), ErrorCode, FString());
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    // The fix reports axis configuration as applied; the old dead-branch path
    // instead attached a "BlendSpaceBase headers unavailable" warning.
    bool bAxisConfigured = false;
    Result->TryGetBoolField(TEXT("axisConfigured"), bAxisConfigured);
    TestTrue(TEXT("response reports axisConfigured=true"), bAxisConfigured);
    TestFalse(TEXT("response carries no axis-skip warning"),
        Result->HasField(TEXT("warning")));

    // Read the axis range/grid back off the actual created asset.
    UBlendSpace* BlendSpace = LoadObject<UBlendSpace>(nullptr, *ObjectPath);
    if (!TestNotNull(TEXT("created blend space loads back"), BlendSpace))
    {
        return true;
    }

    const FBlendParameter& Axis0 = BlendSpace->GetBlendParameter(0);
    // Counterfactual: the pre-fix code left these at the default 0..1 / GridNum 4
    // on UE 5.7, so these three asserts pin the accepted-then-dropped defect.
    TestEqual(TEXT("axis 0 min applied"),
        static_cast<double>(Axis0.Min), RequestedMin);
    TestEqual(TEXT("axis 0 max applied"),
        static_cast<double>(Axis0.Max), RequestedMax);
    TestEqual(TEXT("axis 0 grid divisions applied"),
        Axis0.GridNum, RequestedGrid);

    return true;
}
