// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the foliage.add_instances precedence-elision defect.
//
// foliage.add_instances accepts two mutually exclusive position sources: `transforms`
// (full per-instance transform) and the legacy `locations` (positions only). When a
// caller supplied both, `transforms` won - the documented contract, unchanged here -
// but every `locations` entry was discarded with no field, no count and no warning in
// the response. A caller could hand over N positions and get a success back that was
// indistinguishable from one where those positions had been placed: exactly the
// silent-elision failure mode the handler's skipped[]/skippedCount machinery already
// exists to close for unusable entries.
//
// The fix is detection only. Precedence still resolves to `transforms`, and placement
// is byte-for-byte what it was; the ignored positions are now routed through the same
// NoteSkipped seam the parser uses (so they land in skipped[]/skippedCount with a
// reason naming the precedence rule) plus two summary fields - `ignoredLocationCount`
// (the exact total, which survives the 32-entry skipped[] detail cap) and one
// `warnings[]` string. Both summary fields are absent unless the collision happened.
//
// The three cases below are the decision procedure for that contract:
//   (a) both supplied  -> instances_count still tracks `transforms` (precedence
//       unchanged), ignoredLocationCount equals the locations length, the drops are
//       counted in skippedCount, and warnings[] is non-empty.
//   (b) transforms only -> NEITHER new key is present. This is the backward-compat
//       guard: it fails if the fields are ever emitted unconditionally, which would
//       change every existing response.
//   (c) locations only  -> the legacy fallback still places the positions, with no
//       new keys. Guards against the detection branch swallowing the fallback.
//
// Requests route through the real production dispatcher (FRpcDispatcher::ProcessRequest
// -> the registered foliage.add_instances handler), the same entry the HTTP gateway
// uses, not a copy of the handler logic. No fixture content is needed: the handler
// auto-creates a UFoliageType from the engine's BasicShapes/Cube, and each test removes
// that type's instances (type-scoped, so unrelated foliage in the host map is not
// touched) and deletes the generated asset on the way out.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "Dispatch/RpcDispatcher.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity
// builds enabled, where same-named anonymous-namespace helpers collide across merged
// translation units.
namespace FoliageAddInstancesPrecedenceTestHelpers
{
    // add_instances converts a static-mesh path into a UFoliageType on the fly, building
    // package "/Game/Foliage/Auto_<BaseName>" with the object inside it taking the same stem
    // - so the loadable object path is "Auto_Cube.Auto_Cube", while asset deletion takes the
    // package path. The object used to be named "<BaseName>", which left the package handle
    // resolving to nothing (B-add-instances-auto-foliage-type-name-mismatch); the constants
    // below are the post-fix names.
    constexpr const TCHAR* MeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");
    constexpr const TCHAR* AutoTypePackagePath = TEXT("/Game/Foliage/Auto_Cube");
    constexpr const TCHAR* AutoTypeObjectPath = TEXT("/Game/Foliage/Auto_Cube.Auto_Cube");

    // One `transforms` entry carrying only the required location, in its object form.
    inline TSharedPtr<FJsonValue> MakeTransformEntry(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), X);
        Location->SetNumberField(TEXT("y"), Y);
        Location->SetNumberField(TEXT("z"), Z);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetObjectField(TEXT("location"), Location);
        return MakeShared<FJsonValueObject>(Entry);
    }

    // One legacy `locations` entry: a bare {x,y,z} object.
    inline TSharedPtr<FJsonValue> MakeLocationEntry(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("x"), X);
        Entry->SetNumberField(TEXT("y"), Y);
        Entry->SetNumberField(TEXT("z"), Z);
        return MakeShared<FJsonValueObject>(Entry);
    }

    // Removes only the instances of the auto-created type, then the type asset itself.
    // Scoped by foliageTypePath rather than removeAll so a host map's own foliage is not
    // wiped by a test run; the asset must go last, while nothing still references it.
    inline void Cleanup(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink)
    {
        TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
        RemovePayload->SetStringField(TEXT("foliageTypePath"), AutoTypeObjectPath);

        bool bSuccess = false;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.remove"),
            TEXT("req-foliage-add-precedence-cleanup"), RemovePayload, bSuccess, ErrorCode);

        CleanupTestAsset(FString(AutoTypePackagePath));
    }

    // Reads an integer response field, reporting a failure when the key is missing so a
    // dropped field never reads as a zero that silently satisfies a comparison.
    inline int32 RequireNumberField(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
    {
        double Value = 0.0;
        const bool bPresent = Result.IsValid() && Result->TryGetNumberField(Field, Value);
        Test.TestTrue(*FString::Printf(TEXT("response carries %s"), Field), bPresent);
        return bPresent ? static_cast<int32>(Value) : -1;
    }

    // True when warnings[] carries a line about the transforms-over-locations precedence rule.
    //
    // The cases below used to assert warnings[] was ABSENT, which was a proxy for "no precedence
    // warning" and stopped being one the moment an unrelated writer joined the same array: the
    // auto-created foliage type is dirty-in-memory and correctly says so on every call. Asserting
    // on the subject keeps case (a)'s discriminating power -- a handler that dropped the
    // precedence warning still fails there -- without this test failing on someone else's true
    // statement. Matched on the same token case (a) asserts the skipped[] reason carries.
    inline bool PrecedenceWarningPresent(const TSharedPtr<FJsonObject>& Result)
    {
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("warnings"), Warnings) || !Warnings)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Entry : *Warnings)
        {
            if (Entry.IsValid() && Entry->AsString().Contains(TEXT("precedence")))
            {
                return true;
            }
        }
        return false;
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest below opens FoliageAddInstancesPrecedenceTestHelpers inside its own body
// rather than at file scope: a file-scope using-directive would leak into every other
// test .cpp that Unity merges after this one into the same translation unit.

// Case (a): both arrays supplied. `transforms` still wins, and the discarded
// `locations` positions are now reported instead of vanishing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddInstancesReportsIgnoredLocationsTest,
    "PinWright.foliage.add_instances.ReportsIgnoredLocations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddInstancesReportsIgnoredLocationsTest::RunTest(const FString& Parameters)
{
    using namespace FoliageAddInstancesPrecedenceTestHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);
    ON_SCOPE_EXIT { Cleanup(Dispatcher, Sink); };

    // Deliberately different lengths so an accidental echo of the wrong array's count
    // cannot pass: 2 transforms placed, 3 locations dropped.
    TArray<TSharedPtr<FJsonValue>> Transforms;
    Transforms.Add(MakeTransformEntry(0.0, 0.0, 0.0));
    Transforms.Add(MakeTransformEntry(100.0, 0.0, 0.0));

    TArray<TSharedPtr<FJsonValue>> Locations;
    Locations.Add(MakeLocationEntry(500.0, 0.0, 0.0));
    Locations.Add(MakeLocationEntry(600.0, 0.0, 0.0));
    Locations.Add(MakeLocationEntry(700.0, 0.0, 0.0));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), MeshPath);
    Params->SetArrayField(TEXT("transforms"), Transforms);
    Params->SetArrayField(TEXT("locations"), Locations);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
        TEXT("req-foliage-add-precedence-both"), Params, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("foliage.add_instances with both arrays succeeds"), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        // Nothing was placed (e.g. the engine mesh did not load in this host), so every
        // assertion below would be vacuous rather than discriminating.
        return false;
    }

    // Precedence is unchanged: placement still comes from `transforms` alone.
    TestEqual(TEXT("instances_count still reflects transforms only"),
        RequireNumberField(*this, Result, TEXT("instances_count")), 2);

    // The exact drop total, uncapped.
    TestEqual(TEXT("ignoredLocationCount equals the supplied locations length"),
        RequireNumberField(*this, Result, TEXT("ignoredLocationCount")), 3);

    // The drops go through the pre-existing partial-success mechanism rather than a
    // second parallel one, so skippedCount accounts for them too - which restores the
    // documented instances_count + skippedCount == entries sent identity.
    TestEqual(TEXT("ignored locations are counted in skippedCount"),
        RequireNumberField(*this, Result, TEXT("skippedCount")), 3);

    const TArray<TSharedPtr<FJsonValue>>* SkippedArray = nullptr;
    if (TestTrue(TEXT("response carries the skipped[] detail array"),
            Result->TryGetArrayField(TEXT("skipped"), SkippedArray) && SkippedArray))
    {
        TestEqual(TEXT("every ignored location has a skipped[] row"), SkippedArray->Num(), 3);
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if (SkippedArray->Num() > 0 && (*SkippedArray)[0]->TryGetObject(Row) && Row)
        {
            FString Reason;
            (*Row)->TryGetStringField(TEXT("reason"), Reason);
            TestTrue(TEXT("the skipped[] reason names the precedence rule"),
                Reason.Contains(TEXT("precedence")));
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    TestTrue(TEXT("response carries a non-empty warnings[] array"),
        Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings && Warnings->Num() > 0);

    return true;
}

// Case (b): `transforms` only. Backward-compat guard - the detection fields must not
// appear, so responses that never hit the collision stay byte-identical.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddInstancesTransformsOnlyOmitsFieldsTest,
    "PinWright.foliage.add_instances.TransformsOnlyOmitsPrecedenceFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddInstancesTransformsOnlyOmitsFieldsTest::RunTest(const FString& Parameters)
{
    using namespace FoliageAddInstancesPrecedenceTestHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);
    ON_SCOPE_EXIT { Cleanup(Dispatcher, Sink); };

    TArray<TSharedPtr<FJsonValue>> Transforms;
    Transforms.Add(MakeTransformEntry(200.0, 0.0, 0.0));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), MeshPath);
    Params->SetArrayField(TEXT("transforms"), Transforms);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
        TEXT("req-foliage-add-precedence-transforms-only"), Params, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("foliage.add_instances with transforms only succeeds"), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("instances_count reflects the single transform"),
        RequireNumberField(*this, Result, TEXT("instances_count")), 1);
    TestFalse(TEXT("ignoredLocationCount is absent when no locations were supplied"),
        Result->HasField(TEXT("ignoredLocationCount")));
    // Scoped to THIS test's subject rather than asserting the array is absent. warnings[] is a
    // shared channel: the auto-created foliage type is dirty-in-memory and correctly says so on
    // every call, which is a true statement about durability and nothing to do with precedence.
    TestFalse(TEXT("no precedence warning when no locations were supplied"),
        PrecedenceWarningPresent(Result));
    TestEqual(TEXT("a clean transforms-only batch skips nothing"),
        RequireNumberField(*this, Result, TEXT("skippedCount")), 0);

    return true;
}

// Case (c): `locations` only. The legacy fallback is untouched - it still places the
// positions and emits none of the detection fields.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddInstancesLocationsOnlyUnchangedTest,
    "PinWright.foliage.add_instances.LocationsOnlyUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddInstancesLocationsOnlyUnchangedTest::RunTest(const FString& Parameters)
{
    using namespace FoliageAddInstancesPrecedenceTestHelpers;

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);
    ON_SCOPE_EXIT { Cleanup(Dispatcher, Sink); };

    TArray<TSharedPtr<FJsonValue>> Locations;
    Locations.Add(MakeLocationEntry(300.0, 0.0, 0.0));
    Locations.Add(MakeLocationEntry(400.0, 0.0, 0.0));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), MeshPath);
    Params->SetArrayField(TEXT("locations"), Locations);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
        TEXT("req-foliage-add-precedence-locations-only"), Params, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("foliage.add_instances with locations only succeeds"), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return false;
    }

    // The fallback ran: both legacy positions were placed, none were treated as ignored.
    TestEqual(TEXT("instances_count reflects the legacy locations"),
        RequireNumberField(*this, Result, TEXT("instances_count")), 2);
    TestFalse(TEXT("ignoredLocationCount is absent when locations were actually used"),
        Result->HasField(TEXT("ignoredLocationCount")));
    TestFalse(TEXT("no precedence warning when locations were actually used"),
        PrecedenceWarningPresent(Result));
    TestEqual(TEXT("a clean locations-only batch skips nothing"),
        RequireNumberField(*this, Result, TEXT("skippedCount")), 0);

    return true;
}
