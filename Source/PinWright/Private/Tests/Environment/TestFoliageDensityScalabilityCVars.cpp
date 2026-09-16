// Copyright (c) 2026 Alexander Penkin. MIT License.

// B-foliage-writes-vetoed-by-scalability-cvars.
//
// sg.FoliageQuality drives BOTH grass.densityScale and foliage.DensityScale
// (BaseScalability.ini [FoliageQuality@N]), and before this fix no foliage or landscape verb read
// either one. Two different lies came out of that:
//
//   * landscape.create_grass_type echoed back the density it had just written as if it were the
//     one the grass builder would use. FGrassBuilderBase multiplies it by grass.densityScale
//     (LandscapeGrass.cpp:2040-2041) and ULandscapeGrassType::bEnableDensityScaling - the only
//     opt-out - defaults TRUE, so at sg.FoliageQuality 1 the echo was 2.5x the effective value.
//     This is the spawn_sky_light scaled-intensity shape from the lighting sweep, one namespace
//     over.
//
//   * foliage.paint reported instancesPlaced, which is literally true about the editor-side
//     ledger and says nothing about the frame: on a type that opts in to density scaling,
//     UHierarchicalInstancedStaticMeshComponent drops a random fraction of those instances from
//     its cluster tree before anything is drawn. A caller who painted 1000 and saw 250 had no
//     route through the RPC surface to learn why.
//
// BOTH ASSERTIONS ARE DIFFERENTIAL. The same call is made twice against the same fixture with
// only the cvar moved, and the effective figure must differ between the runs while the requested
// one stays identical. A field echoed back from the request scores both runs alike, which is
// precisely the defect - so an implementation that "fixes" this by copying the requested value
// into the effective field fails here.
//
// COUNTERFACTUAL: before the fix neither response carried effectiveDensity / expectedDrawnInstances
// / the cvar block / cvarWarning at all, so every presence assertion below is red.
//
// CVAR HYGIENE. Both cvars are process-global and the suite runs against whatever map the editor
// opened, so each test restores its cvar in an ON_SCOPE_EXIT at the priority the variable already
// had (the engine's guard is >=, not >, so writing at a higher priority would leave the variable
// pinned above scalability for the rest of the run). A leaked cvar here would corrupt every test
// that runs after it in the same host - and, for foliage.DensityScale, would leave the engine's
// console-variable sink rebuilding every opted-in HISM cluster tree in the level.

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FoliageType.h"
#include "HAL/IConsoleManager.h"
#include "LandscapeGrassType.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace FoliageDensityScalabilityTestHelpers
{
    // The only content fixture, the same engine cube every other foliage test uses.
    constexpr const TCHAR* DensityCubeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    // A column of the editor world well away from the ones the sibling foliage and ground tests
    // paint into, so a failure is readable in the viewport.
    constexpr double DensityColX = 517400.0;
    constexpr double DensityColY = 463900.0;
    constexpr double DensityColZ = 19000.0;

    inline FString UniqueDensityName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("PWDensity_%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    inline FString FoliagePackagePathFor(const FString& Name)
    {
        return FString::Printf(TEXT("/Game/Foliage/%s"), *Name);
    }

    inline FString FoliageObjectPathFor(const FString& Name)
    {
        return FString::Printf(TEXT("/Game/Foliage/%s.%s"), *Name, *Name);
    }

    // landscape.create_grass_type hardcodes /Game/Landscape as its package root.
    inline FString GrassPackagePathFor(const FString& Name)
    {
        return FString::Printf(TEXT("/Game/Landscape/%s"), *Name);
    }

    inline TSharedPtr<FJsonValue> DensityLocationEntry(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return MakeShared<FJsonValueObject>(Obj);
    }

    // Type-scoped teardown: empties only this fixture's instances (a removeAll would wipe the host
    // map's own foliage) and then deletes the asset, which also clears the dirty /Game package a
    // later editor-wide save-all would otherwise flush into host Content.
    inline void DiscardDensityType(FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& Name)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), FoliageObjectPathFor(Name));

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.remove"),
            TEXT("req-foliage-density-cleanup"), Params, bSuccess, ErrorCode);

        CleanupTestAsset(FoliagePackagePathFor(Name));
    }

    // Reads the {cvar, found, value} block every verb in this family publishes and asserts the
    // measurement actually happened. Returns the measured value, or Unset when the block is
    // missing or unmeasured, so a caller can tell an absent field from a measured zero.
    inline double ReadMeasuredCVarValue(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result, const TCHAR* BlockField, const TCHAR* ExpectedCVar)
    {
        const double Unset = TNumericLimits<double>::Lowest();
        const TSharedPtr<FJsonObject>* Block = nullptr;
        if (!Test.TestTrue(*FString::Printf(TEXT("response carries %s"), BlockField),
                Result.IsValid() && Result->TryGetObjectField(BlockField, Block))
            || !Block || !(*Block).IsValid())
        {
            return Unset;
        }

        FString CVarName;
        (*Block)->TryGetStringField(TEXT("cvar"), CVarName);
        Test.TestEqual(TEXT("the reported cvar is the one the engine actually applies"),
            CVarName, FString(ExpectedCVar));

        bool bFound = false;
        (*Block)->TryGetBoolField(TEXT("found"), bFound);
        Test.TestTrue(TEXT("the cvar was measured, not assumed"), bFound);

        double Value = Unset;
        if (!bFound || !(*Block)->TryGetNumberField(TEXT("value"), Value))
        {
            return Unset;
        }
        return Value;
    }
}

// ============================================================================
// foliage.paint - the ledger count and what the renderer draws are separated
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliagePaintDensityScaleIsDisclosedTest,
    "PinWright.foliage.paint.LedgerCountIsSeparatedFromWhatRenders",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliagePaintDensityScaleIsDisclosedTest::RunTest(const FString& Parameters)
{
    using namespace FoliageDensityScalabilityTestHelpers;

    IConsoleVariable* DensityCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("foliage.DensityScale"));
    if (!DensityCVar)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("foliage.DensityScale is not in this host's console registry, so the render-time "
                 "cull this test drives cannot be produced."));
        return true;
    }

    if (!GEditor || !GEditor->GetEditorWorldContext().World())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("no editor world, so foliage.paint has nowhere to place its instances."));
        return true;
    }

    if (!LoadObject<UObject>(nullptr, DensityCubeMeshPath))
    {
        AddError(FString::Printf(TEXT("required fixture mesh %s did not load"), DensityCubeMeshPath));
        return true;
    }

    const float OriginalScale = DensityCVar->GetFloat();
    // Write at the priority the variable ALREADY has: the engine's guard is >=, not >, so a Code
    // write would leave foliage.DensityScale outranking scalability for the rest of the run.
    const EConsoleVariableFlags CVarSetBy =
        static_cast<EConsoleVariableFlags>(DensityCVar->GetFlags() & ECVF_SetByMask);
    ON_SCOPE_EXIT
    {
        DensityCVar->Set(OriginalScale, CVarSetBy);
    };

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    const FString TypeName = UniqueDensityName(TEXT("PaintScale"));
    ON_SCOPE_EXIT { DiscardDensityType(Dispatcher, Sink, TypeName); };

    // enableDensityScaling is itself new: UFoliageType::bEnableDensityScaling defaults FALSE and
    // was unreachable through this surface, so a caller could neither opt a detail mesh in nor
    // discover which regime a type was in. Routed through the real dispatcher, so an undeclared
    // parameter is refused UNKNOWN_PARAMS here rather than silently ignored.
    {
        TSharedPtr<FJsonObject> TypeParams = MakeShared<FJsonObject>();
        TypeParams->SetStringField(TEXT("name"), TypeName);
        TypeParams->SetStringField(TEXT("meshPath"), DensityCubeMeshPath);
        TypeParams->SetNumberField(TEXT("density"), 100.0);
        TypeParams->SetBoolField(TEXT("enableDensityScaling"), true);

        bool bTypeSuccess = false;
        FString TypeErrorCode;
        TSharedPtr<FJsonObject> TypeResult;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.add_type"),
            TEXT("req-foliage-density-add-type"), TypeParams, bTypeSuccess, TypeResult,
            TypeErrorCode);
        if (!TestTrue(*FString::Printf(TEXT("foliage.add_type accepts enableDensityScaling "
                                            "(error=%s)"), *TypeErrorCode), bTypeSuccess))
        {
            return false;
        }

        bool bReportedOptIn = false;
        TestTrue(TEXT("foliage.add_type reports densityScalingEnabled"),
            TypeResult.IsValid()
                && TypeResult->TryGetBoolField(TEXT("densityScalingEnabled"), bReportedOptIn));
        TestTrue(TEXT("the requested opt-in reached the asset"), bReportedOptIn);

        // Re-read off the asset rather than off the response: a handler that reported the flag
        // without writing it satisfies the line above and fails this one.
        const UFoliageType* WrittenType =
            LoadObject<UFoliageType>(nullptr, *FoliageObjectPathFor(TypeName));
        if (TestNotNull(TEXT("the foliage type asset is addressable"), WrittenType))
        {
            TestTrue(TEXT("bEnableDensityScaling is written on the asset, not just echoed"),
                WrittenType->bEnableDensityScaling != 0);
        }
    }

    constexpr int32 InstanceCount = 8;
    TArray<TSharedPtr<FJsonValue>> Locations;
    for (int32 Index = 0; Index < InstanceCount; ++Index)
    {
        Locations.Add(DensityLocationEntry(
            DensityColX + Index * 250.0, DensityColY, DensityColZ));
    }

    auto MakePaintParams = [&]()
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), FoliageObjectPathFor(TypeName));
        Params->SetArrayField(TEXT("locations"), Locations);
        return Params;
    };

    // ---- Case 1: the renderer culls. This is the assertion the old response could not carry.
    constexpr float CulledScale = 0.25f;
    DensityCVar->Set(CulledScale, CVarSetBy);
    if (!FMath::IsNearlyEqual(DensityCVar->GetFloat(), CulledScale))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("foliage.DensityScale would not take 0.25 on this host, so the render-time cull "
                 "could not be staged."));
        return true;
    }

    bool bCulledSuccess = false;
    FString CulledErrorCode;
    TSharedPtr<FJsonObject> Culled;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.paint"),
        TEXT("req-foliage-density-paint-culled"), MakePaintParams(), bCulledSuccess, Culled,
        CulledErrorCode);
    if (!TestTrue(*FString::Printf(TEXT("foliage.paint still succeeds while the renderer culls "
                                        "(error=%s)"), *CulledErrorCode), bCulledSuccess)
        || !Culled.IsValid())
    {
        return false;
    }

    double CulledPlaced = 0.0;
    TestTrue(TEXT("the culled response carries instancesPlaced"),
        Culled->TryGetNumberField(TEXT("instancesPlaced"), CulledPlaced));
    TestEqual(TEXT("instancesPlaced still reports the ledger write that did happen"),
        static_cast<int32>(CulledPlaced), InstanceCount);

    bool bCulledOptIn = false;
    TestTrue(TEXT("the culled response carries densityScalingEnabled"),
        Culled->TryGetBoolField(TEXT("densityScalingEnabled"), bCulledOptIn));
    TestTrue(TEXT("the painted type is reported as opted in to density scaling"), bCulledOptIn);

    TestEqual(TEXT("foliage.DensityScale is measured and published as 0.25"),
        static_cast<float>(ReadMeasuredCVarValue(*this, Culled,
            TEXT("foliageDensityScaleCVar"), TEXT("foliage.DensityScale"))),
        CulledScale);

    double CulledScaleField = 0.0;
    TestTrue(TEXT("the culled response carries effectiveDensityScale"),
        Culled->TryGetNumberField(TEXT("effectiveDensityScale"), CulledScaleField));
    TestEqual(TEXT("effectiveDensityScale is the measured cvar, clamped as the engine clamps it"),
        static_cast<float>(CulledScaleField), CulledScale);

    double CulledDrawn = 0.0;
    TestTrue(TEXT("the culled response carries expectedDrawnInstances"),
        Culled->TryGetNumberField(TEXT("expectedDrawnInstances"), CulledDrawn));
    TestEqual(TEXT("expectedDrawnInstances is the ledger count scaled by the measured cvar"),
        static_cast<int32>(CulledDrawn), FMath::RoundToInt(InstanceCount * CulledScale));

    FString CulledWarning;
    Culled->TryGetStringField(TEXT("cvarWarning"), CulledWarning);
    TestTrue(TEXT("a culled paint names foliage.DensityScale in cvarWarning"),
        CulledWarning.Contains(TEXT("foliage.DensityScale")));
    TestTrue(TEXT("the warning names the remedy the caller can act on"),
        CulledWarning.Contains(TEXT("sg.FoliageQuality")));

    // ---- Case 2: same verb, same fixture, same type - only the cvar moved.
    DensityCVar->Set(1.0f, CVarSetBy);
    if (!FMath::IsNearlyEqual(DensityCVar->GetFloat(), 1.0f))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("foliage.DensityScale would not take 1.0 on this host, so the unculled half of "
                 "the differential is absent."));
        return true;
    }

    bool bFullSuccess = false;
    FString FullErrorCode;
    TSharedPtr<FJsonObject> Full;
    DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.paint"),
        TEXT("req-foliage-density-paint-full"), MakePaintParams(), bFullSuccess, Full,
        FullErrorCode);
    if (!TestTrue(*FString::Printf(TEXT("foliage.paint succeeds with the scale at 1 (error=%s)"),
            *FullErrorCode), bFullSuccess)
        || !Full.IsValid())
    {
        return false;
    }

    double FullPlaced = 0.0;
    Full->TryGetNumberField(TEXT("instancesPlaced"), FullPlaced);
    TestEqual(TEXT("instancesPlaced is the same in both runs - only the cvar moved"),
        static_cast<int32>(FullPlaced), static_cast<int32>(CulledPlaced));

    double FullDrawn = 0.0;
    TestTrue(TEXT("the unculled response carries expectedDrawnInstances"),
        Full->TryGetNumberField(TEXT("expectedDrawnInstances"), FullDrawn));
    TestEqual(TEXT("every instance is expected to be drawn once the scale is 1"),
        static_cast<int32>(FullDrawn), static_cast<int32>(FullPlaced));

    TestFalse(TEXT("an unculled paint publishes no cvarWarning"),
        Full->HasField(TEXT("cvarWarning")));

    // THE DIFFERENTIAL. A field echoed back from instancesPlaced scores both runs alike, which is
    // exactly the defect this ticket is about.
    TestTrue(TEXT("expectedDrawnInstances differs between the culled and the unculled run"),
        static_cast<int32>(CulledDrawn) != static_cast<int32>(FullDrawn));

    return true;
}

// ============================================================================
// landscape.create_grass_type - the echoed density is not the effective one
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLandscapeCreateGrassTypeEffectiveDensityTest,
    "PinWright.landscape.create_grass_type.EffectiveDensityFollowsCVarScale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLandscapeCreateGrassTypeEffectiveDensityTest::RunTest(const FString& Parameters)
{
    using namespace FoliageDensityScalabilityTestHelpers;

    IConsoleVariable* GrassCVar =
        IConsoleManager::Get().FindConsoleVariable(TEXT("grass.densityScale"));
    if (!GrassCVar)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-absent"),
            TEXT("grass.densityScale is not in this host's console registry, so the builder "
                 "rescale this test drives cannot be produced."));
        return true;
    }

    if (!LoadObject<UObject>(nullptr, DensityCubeMeshPath))
    {
        AddError(FString::Printf(TEXT("required fixture mesh %s did not load"), DensityCubeMeshPath));
        return true;
    }

    const float OriginalScale = GrassCVar->GetFloat();
    const EConsoleVariableFlags CVarSetBy =
        static_cast<EConsoleVariableFlags>(GrassCVar->GetFlags() & ECVF_SetByMask);

    const FString ScaledName = UniqueDensityName(TEXT("GrassScaled"));
    const FString UnscaledName = UniqueDensityName(TEXT("GrassUnscaled"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(GrassPackagePathFor(ScaledName));
        CleanupTestAsset(GrassPackagePathFor(UnscaledName));
        GrassCVar->Set(OriginalScale, CVarSetBy);
    };

    constexpr double RequestedDensity = 10.0;

    // The verb finishes inside an AsyncTask(GameThread) lambda, so responses arrive through a
    // shared-owned capture that a late completion cannot write through after this frame.
    auto CallCreateGrassType = [&](const FString& AssetName,
        const TSharedRef<FTestResponseCapture>& Capture) -> bool
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), AssetName);
        Params->SetStringField(TEXT("meshPath"), DensityCubeMeshPath);
        Params->SetNumberField(TEXT("density"), RequestedDensity);

        if (!TestTrue(TEXT("landscape.create_grass_type handler registered"),
                InvokeHandlerWithSharedCapture(TEXT("landscape.create_grass_type"), Params, Capture)))
        {
            return false;
        }
        PumpUntilCaptured(*Capture, /*TimeoutSeconds=*/15.0);
        return TestTrue(TEXT("the async create_grass_type response reached the capture"),
            Capture->bWasCalled) && Capture->bSuccess && Capture->Result.IsValid();
    };

    // ---- Case 1: the grass builder rescales.
    constexpr float ScaledValue = 0.5f;
    GrassCVar->Set(ScaledValue, CVarSetBy);
    if (!FMath::IsNearlyEqual(GrassCVar->GetFloat(), ScaledValue))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("grass.densityScale would not take 0.5 on this host, so the builder rescale "
                 "could not be staged."));
        return true;
    }

    TSharedRef<FTestResponseCapture> Scaled = MakeShared<FTestResponseCapture>();
    if (!CallCreateGrassType(ScaledName, Scaled))
    {
        AddError(TEXT("landscape.create_grass_type did not produce a success to assert on"));
        return false;
    }

    double ScaledDensity = 0.0;
    TestTrue(TEXT("the scaled response carries density"),
        Scaled->Result->TryGetNumberField(TEXT("density"), ScaledDensity));
    TestEqual(TEXT("density reports the value stored on the asset"),
        static_cast<float>(ScaledDensity), static_cast<float>(RequestedDensity));

    bool bScaledOptIn = false;
    TestTrue(TEXT("the scaled response carries densityScalingEnabled"),
        Scaled->Result->TryGetBoolField(TEXT("densityScalingEnabled"), bScaledOptIn));
    // ULandscapeGrassType's constructor sets it true, so a default-created grass type IS scaled.
    TestTrue(TEXT("a grass type created with engine defaults is subject to the scale"),
        bScaledOptIn);

    TestEqual(TEXT("grass.densityScale is measured and published as 0.5"),
        static_cast<float>(ReadMeasuredCVarValue(*this, Scaled->Result,
            TEXT("grassDensityScaleCVar"), TEXT("grass.densityScale"))),
        ScaledValue);

    double ScaledEffective = 0.0;
    TestTrue(TEXT("the scaled response carries effectiveDensity"),
        Scaled->Result->TryGetNumberField(TEXT("effectiveDensity"), ScaledEffective));
    TestEqual(TEXT("effectiveDensity is the stored density times the measured scale"),
        static_cast<float>(ScaledEffective),
        static_cast<float>(RequestedDensity * ScaledValue));

    FString ScaledWarning;
    Scaled->Result->TryGetStringField(TEXT("cvarWarning"), ScaledWarning);
    TestTrue(TEXT("a rescaled create names grass.densityScale in cvarWarning"),
        ScaledWarning.Contains(TEXT("grass.densityScale")));
    TestTrue(TEXT("the warning names the remedy the caller can act on"),
        ScaledWarning.Contains(TEXT("sg.FoliageQuality")));

    // Re-read off the asset: the stored density must be the UNSCALED one, or the verb has quietly
    // pre-multiplied the caller's value into the asset instead of reporting the scale.
    const ULandscapeGrassType* StoredType = LoadObject<ULandscapeGrassType>(nullptr,
        *FString::Printf(TEXT("/Game/Landscape/%s.%s"), *ScaledName, *ScaledName));
    if (TestNotNull(TEXT("the grass type asset is addressable"), StoredType)
        && StoredType->GrassVarieties.Num() > 0)
    {
        TestEqual(TEXT("the asset stores the requested density, unscaled"),
            StoredType->GrassVarieties[0].GrassDensity.Default,
            static_cast<float>(RequestedDensity));
    }

    // ---- Case 2: same call, same mesh, scale restored to 1.
    GrassCVar->Set(1.0f, CVarSetBy);
    if (!FMath::IsNearlyEqual(GrassCVar->GetFloat(), 1.0f))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("cvar-pinned"),
            TEXT("grass.densityScale would not take 1.0 on this host, so the unscaled half of the "
                 "differential is absent."));
        return true;
    }

    TSharedRef<FTestResponseCapture> Unscaled = MakeShared<FTestResponseCapture>();
    if (!CallCreateGrassType(UnscaledName, Unscaled))
    {
        AddError(TEXT("landscape.create_grass_type did not produce a second success to assert on"));
        return false;
    }

    double UnscaledDensity = 0.0;
    Unscaled->Result->TryGetNumberField(TEXT("density"), UnscaledDensity);
    TestEqual(TEXT("density is the same in both runs - only the cvar moved"),
        static_cast<float>(UnscaledDensity), static_cast<float>(ScaledDensity));

    double UnscaledEffective = 0.0;
    TestTrue(TEXT("the unscaled response carries effectiveDensity"),
        Unscaled->Result->TryGetNumberField(TEXT("effectiveDensity"), UnscaledEffective));
    TestEqual(TEXT("effectiveDensity equals the stored density once the scale is 1"),
        static_cast<float>(UnscaledEffective), static_cast<float>(RequestedDensity));

    TestFalse(TEXT("an unscaled create publishes no cvarWarning"),
        Unscaled->Result->HasField(TEXT("cvarWarning")));

    // THE DIFFERENTIAL: a field echoed back from `density` scores both runs alike.
    TestTrue(TEXT("effectiveDensity differs between the scaled and the unscaled run"),
        !FMath::IsNearlyEqual(static_cast<float>(ScaledEffective),
            static_cast<float>(UnscaledEffective)));

    return true;
}
