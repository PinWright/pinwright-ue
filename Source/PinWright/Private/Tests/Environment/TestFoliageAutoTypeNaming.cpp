// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestFoliageAutoTypeNaming.cpp - regression coverage for
// B-add-instances-auto-foliage-type-name-mismatch.
//
// WHAT WAS WRONG. foliage.add_instances and foliage.paint both accept a STATIC MESH path and
// stand up a UFoliageType for it on the fly. Both built package "/Game/Foliage/Auto_<Mesh>" while
// naming the object inside it "<Mesh>". Every path lookup in the editor normalises a bare package
// handle to "Package.PackageStem" (EditorScriptingHelpers::ConvertAnyPathToObjectPath infers the
// object name from the package short name), so "/Game/Foliage/Auto_<Mesh>" - the form the content
// browser shows, the form asset.list rows carry, the form a caller writes by hand - normalised to
// "Auto_<Mesh>.Auto_<Mesh>" and resolved to NOTHING. Measured consequences: asset.exists answered
// false on an asset the level was actively drawing, asset.save and foliage.remove answered
// ASSET_NOT_FOUND, and foliage.get_instances answered success:true with an EMPTY instances[] over
// eight live instances.
//
// WHAT IS ASSERTED, AND WHY EACH FAILS ON THE PRE-FIX CODE.
//
//   Test 1 (round trip). The handle add_instances hands back is decomposed and its package stem
//   compared to its object name - the exact identity the defect broke - and then driven back
//   through asset.exists and foliage.get_instances in BOTH its object-path and its bare
//   package-handle form. On the pre-fix code the stem/name comparison is "Auto_Cylinder" vs
//   "Cylinder", asset.exists on the package handle is false, and get_instances on the package
//   handle is the empty-list success. Instance counts are cross-checked against the level's own
//   FFoliageInfo::Instances rather than against what was sent, so a read that agreed with a
//   broken write cannot satisfy them. The second add_instances call asserts the two calls agree
//   on one type - the pre-fix code re-ran NewObject over the live object every time.
//
//   Test 2 (refusal). A naming fix alone leaves the false-success mechanism intact for every
//   OTHER way a handle can go bad, which is why this is a separate test: an unresolvable filter,
//   and a filter that resolves to something that is not a UFoliageType, must both be refused
//   rather than answered with a clean empty read. Pre-fix both return success:true with
//   instances:[]. The unfiltered control call guards the opposite failure - a refusal that
//   over-reaches and starts rejecting legitimate reads.
//
// FIXTURE ISOLATION. The auto-type name is derived from the MESH basename, so any two tests
// driving the same mesh share one level-global type. Tests/World/TestEnvironmentHandlers.cpp and
// Tests/World/TestFoliageAddInstancesPrecedenceHonesty.cpp both drive BasicShapes/Cube; this file
// drives BasicShapes/Cylinder so its counts are absolute rather than relative to whatever ran
// before it. Both naming forms are cleaned up on entry and on exit, so a host carrying a
// pre-fix "/Game/Foliage/Auto_Cylinder" left by an older build cannot decide the outcome.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest -> the
// registered handler), the same entry the HTTP gateway uses, which also validates each payload
// against the declared ParamSpec.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FoliageType.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace FoliageAutoTypeNamingTestHelpers
{
    // Deliberately not Cube - see FIXTURE ISOLATION above.
    constexpr const TCHAR* AutoSourceMeshPath = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
    constexpr const TCHAR* AutoTypePackagePath = TEXT("/Game/Foliage/Auto_Cylinder");
    // Post-fix: the object takes the package stem.
    constexpr const TCHAR* AutoTypeObjectPath = TEXT("/Game/Foliage/Auto_Cylinder.Auto_Cylinder");
    // Pre-fix: the object took the bare mesh basename. Cleaned up too, so an asset left behind by
    // an older build cannot survive into this test and change what it measures.
    constexpr const TCHAR* LegacyAutoTypeObjectPath = TEXT("/Game/Foliage/Auto_Cylinder.Cylinder");

    // A column of the editor world away from the ones the other foliage test files use.
    constexpr double ColX = 512300.0;
    constexpr double ColY = 448700.0;
    constexpr double ColZ = 33000.0;

    inline UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    inline TSharedPtr<FJsonValue> TransformEntry(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), X);
        Location->SetNumberField(TEXT("y"), Y);
        Location->SetNumberField(TEXT("z"), Z);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetObjectField(TEXT("location"), Location);
        return MakeShared<FJsonValueObject>(Entry);
    }

    // Ground truth: what the level actually holds for a type, read off every foliage actor in the
    // world rather than off the response under test.
    inline int32 CountStoredInstances(UWorld* World, const UFoliageType* Type)
    {
        int32 Total = 0;
        if (!World || !Type)
        {
            return Total;
        }
        for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
        {
            if (AInstancedFoliageActor* Ifa = *It)
            {
                if (const FFoliageInfo* Info = Ifa->FindInfo(Type))
                {
                    Total += Info->Instances.Num();
                }
            }
        }
        return Total;
    }

    // Removes the auto type's instances under BOTH naming forms, then deletes the package. Scoped
    // by foliageTypePath rather than removeAll: a removeAll here would empty the host map's own
    // foliage, which is shared state this test does not own. A form that does not exist answers
    // ASSET_NOT_FOUND, which is the expected no-op.
    inline void DiscardAutoType(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink)
    {
        for (const TCHAR* ObjectPath : {AutoTypeObjectPath, LegacyAutoTypeObjectPath})
        {
            TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
            RemoveParams->SetStringField(TEXT("foliageTypePath"), ObjectPath);

            bool bRemoved = false;
            FString RemoveErrorCode;
            DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.remove"),
                TEXT("req-foliage-auto-name-cleanup"), RemoveParams, bRemoved, RemoveErrorCode);
        }
        CleanupTestAsset(FString(AutoTypePackagePath));
    }

    // Reads foliage.get_instances' count for one handle, reporting the failure itself when the
    // call did not succeed or the field is missing so a dropped field never reads as a zero that
    // silently satisfies a comparison. Returns -1 when nothing usable came back.
    inline int32 ReadInstanceCount(FAutomationTestBase& Test, FRpcDispatcher& Dispatcher,
        DispatcherTestHelpers::FSinkPtr& Sink, const FString& Handle, const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), Handle);

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.get_instances"),
            TEXT("req-foliage-auto-name-read"), Params, bSuccess, Result, ErrorCode);

        if (!Test.TestTrue(*FString::Printf(
                TEXT("foliage.get_instances succeeds for the %s form '%s' (error=%s)"),
                Label, *Handle, *ErrorCode), bSuccess) || !Result.IsValid())
        {
            return -1;
        }

        double Count = 0.0;
        if (!Test.TestTrue(*FString::Printf(TEXT("the %s read carries count"), Label),
                Result->TryGetNumberField(TEXT("count"), Count)))
        {
            return -1;
        }
        return static_cast<int32>(Count);
    }

    inline bool ProbeAssetExists(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
        const FString& AssetPath)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), AssetPath);

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("asset.exists"),
            TEXT("req-foliage-auto-name-exists"), Params, bSuccess, Result, ErrorCode);

        bool bExists = false;
        return bSuccess && Result.IsValid() && Result->TryGetBoolField(TEXT("exists"), bExists)
            && bExists;
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest below opens FoliageAutoTypeNamingTestHelpers inside its own body rather than at
// file scope: a file-scope using-directive would leak into every other test .cpp that Unity
// merges after this one into the same translation unit.

// ============================================================================
// The handle foliage.add_instances publishes for an auto-created type resolves
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAutoTypeHandleRoundTripsTest,
    "PinWright.foliage.add_instances.AutoCreatedTypeHandleRoundTrips",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAutoTypeHandleRoundTripsTest::RunTest(const FString& Parameters)
{
    using namespace FoliageAutoTypeNamingTestHelpers;

    UWorld* World = EditorWorld();
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the auto-created "
                 "foliage type round-trip assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // An asset left by an older build under either naming form would decide the outcome, so the
    // fixture is cleared before the first call as well as after the last.
    DiscardAutoType(Dispatcher, Sink);
    ON_SCOPE_EXIT
    {
        DiscardAutoType(Dispatcher, Sink);
    };

    // ---- First scatter: the verb auto-creates the type and echoes its handle ----
    FString PublishedHandle;
    {
        TArray<TSharedPtr<FJsonValue>> Transforms;
        Transforms.Add(TransformEntry(ColX, ColY, ColZ));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), AutoSourceMeshPath);
        Params->SetArrayField(TEXT("transforms"), Transforms);

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
            TEXT("req-foliage-auto-name-add-1"), Params, bSuccess, Result, ErrorCode);

        if (!TestTrue(*FString::Printf(
                TEXT("foliage.add_instances auto-creates a type from a static mesh (error=%s)"),
                *ErrorCode), bSuccess) || !Result.IsValid())
        {
            return true;
        }
        if (!TestTrue(TEXT("the response echoes foliageTypePath"),
                Result->TryGetStringField(TEXT("foliageTypePath"), PublishedHandle)
                    && !PublishedHandle.IsEmpty()))
        {
            return true;
        }
    }

    // ---- The published handle is an object path whose stem agrees with its object name ----
    // This is the defect itself: the package was Auto_<Mesh> while the object was <Mesh>, so the
    // package handle every other surface uses resolved to nothing.
    FString PackageHandle;
    FString ObjectName;
    if (!TestTrue(*FString::Printf(
            TEXT("the published handle '%s' is an object path, not a bare package handle"),
            *PublishedHandle),
            PublishedHandle.Split(TEXT("."), &PackageHandle, &ObjectName,
                ESearchCase::CaseSensitive, ESearchDir::FromEnd)))
    {
        return true;
    }
    TestEqual(TEXT("the auto-created type's package stem and object name agree"),
        ObjectName, FPackageName::GetLongPackageAssetName(PackageHandle));
    TestEqual(TEXT("the auto-created type lands at the documented package path"),
        PackageHandle, FString(AutoTypePackagePath));

    // ---- Ground truth: what the level holds under that type ----
    UFoliageType* AutoType = LoadObject<UFoliageType>(nullptr, *PublishedHandle);
    if (!TestTrue(*FString::Printf(TEXT("the published handle '%s' loads as a UFoliageType"),
            *PublishedHandle), AutoType != nullptr))
    {
        return true;
    }
    TestEqual(TEXT("the level holds the one instance that was scattered"),
        CountStoredInstances(World, AutoType), 1);

    // ---- asset.exists answers true for BOTH forms of the same asset ----
    TestTrue(TEXT("asset.exists resolves the published object path"),
        ProbeAssetExists(Dispatcher, Sink, PublishedHandle));
    TestTrue(TEXT("asset.exists resolves the bare package handle of the same asset"),
        ProbeAssetExists(Dispatcher, Sink, PackageHandle));

    // ---- foliage.get_instances sees the instances through BOTH forms ----
    TestEqual(TEXT("get_instances on the published object path sees the scattered instance"),
        ReadInstanceCount(*this, Dispatcher, Sink, PublishedHandle, TEXT("object-path")), 1);
    TestEqual(TEXT("get_instances on the bare package handle sees the same instance"),
        ReadInstanceCount(*this, Dispatcher, Sink, PackageHandle, TEXT("package-handle")), 1);

    // ---- A second scatter reuses the SAME type and publishes the SAME handle ----
    {
        TArray<TSharedPtr<FJsonValue>> Transforms;
        Transforms.Add(TransformEntry(ColX + 400.0, ColY, ColZ));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), AutoSourceMeshPath);
        Params->SetArrayField(TEXT("transforms"), Transforms);

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
            TEXT("req-foliage-auto-name-add-2"), Params, bSuccess, Result, ErrorCode);

        TestTrue(*FString::Printf(TEXT("a repeat scatter on the same mesh succeeds (error=%s)"),
            *ErrorCode), bSuccess);
        if (bSuccess && Result.IsValid())
        {
            FString RepeatHandle;
            Result->TryGetStringField(TEXT("foliageTypePath"), RepeatHandle);
            TestEqual(TEXT("a repeat scatter publishes the same handle as the first"),
                RepeatHandle, PublishedHandle);
        }
    }

    // Re-read the type rather than reusing the pointer: the pre-fix create path ran NewObject over
    // the live object on every call, and this is where that would show as a lost instance.
    UFoliageType* ReloadedType = LoadObject<UFoliageType>(nullptr, *PublishedHandle);
    TestEqual(TEXT("both scatters landed on one shared type"),
        CountStoredInstances(World, ReloadedType), 2);
    TestEqual(TEXT("get_instances agrees with the level after the repeat scatter"),
        ReadInstanceCount(*this, Dispatcher, Sink, PublishedHandle, TEXT("object-path")), 2);

    return true;
}

// ============================================================================
// foliage.get_instances refuses a filter it cannot resolve instead of answering
// success with an empty list
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageGetInstancesUnresolvableFilterRefusedTest,
    "PinWright.foliage.get_instances.UnresolvableTypeFilterIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageGetInstancesUnresolvableFilterRefusedTest::RunTest(const FString& Parameters)
{
    using namespace FoliageAutoTypeNamingTestHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // ---- Case 1: a path that names no asset at all ----
    // The fresh GUID keeps the path absent regardless of host content state.
    {
        const FString MissingType = FString::Printf(
            TEXT("/Game/Foliage/DoesNotExist_FoliageGetInstances_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), MissingType);

        bool bSuccess = true;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("foliage.get_instances"),
            TEXT("req-foliage-get-missing-type"), Params, bSuccess, Result, ErrorCode);

        TestFalse(TEXT("foliage.get_instances does not report success for a filter that names "
                       "no asset"), bSuccess);
        TestEqual(TEXT("an unresolvable foliage type filter returns ASSET_NOT_FOUND"),
            ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
        // The pre-fix arm answered success:true WITH an instances[] - so the absence of that
        // field is what separates a refusal from the empty-list false success.
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        TestFalse(TEXT("the refusal carries no instances[] that could be read as a clean zero"),
            Result.IsValid() && Result->TryGetArrayField(TEXT("instances"), Rows));
    }

    // ---- Case 2: a path that resolves, but not to a UFoliageType ----
    // The registry probe the pre-fix code gated on answers "exists" here, and the read then fell
    // through to the same silent zero.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), AutoSourceMeshPath);

        bool bSuccess = true;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("foliage.get_instances"),
            TEXT("req-foliage-get-wrong-type"), Params, bSuccess, Result, ErrorCode);

        TestFalse(TEXT("foliage.get_instances does not report success for a filter naming a "
                       "static mesh rather than a foliage type"), bSuccess);
        TestEqual(TEXT("a wrong-type foliage type filter returns ASSET_NOT_FOUND"),
            ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    }

    // ---- Control: the refusal does not reach an unfiltered read ----
    // Without this, a handler that refused everything would pass both cases above.
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("foliage.get_instances"),
            TEXT("req-foliage-get-unfiltered-control"), Params, bSuccess, Result, ErrorCode);

        TestTrue(*FString::Printf(TEXT("an unfiltered foliage.get_instances still succeeds "
                                       "(error=%s)"), *ErrorCode), bSuccess);
    }

    return true;
}
