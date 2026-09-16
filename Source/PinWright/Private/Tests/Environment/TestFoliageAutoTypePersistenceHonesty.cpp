// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestFoliageAutoTypePersistenceHonesty.cpp - regression coverage for
// B-foliage-auto-type-no-disk-write.
//
// WHAT WAS WRONG. foliage.paint and foliage.add_instances both accept a STATIC MESH path and stand
// a UFoliageType up for it on the fly, finishing at McpSafeAssetSave - which marks the package dirty
// and writes NOTHING (Utils/AssetUtils.h: "it deliberately does NOT write the .uasset ... so NOTHING
// it does makes an edit durable"). The only presence field either verb published was
// `existsAfter: true`, a LITERAL on a branch where the foliage actor pointer was already known
// non-null. So a caller scattered foliage, read the instances back, saw existsAfter:true, quit, and
// lost the type asset the instances referenced. No `saved`, no `pendingFlush`, no disk probe of any
// kind appeared on either verb, and neither of the plugin's two honest save reporters
// (SaveAssetToDiskReportingPresence, AddMarkDirtySaveReport) was wired in.
//
// WHAT IS ASSERTED, AND WHY IT FAILS ON THE PRE-FIX CODE. Every assertion compares the RESPONSE'S
// OWN CLAIM about on-disk presence against an independent IFileManager::FileSize probe of the
// .uasset, in BOTH directions:
//
//   Direction 1 (not saved). The first scatter creates the type. The probe finds no file, and the
//   response must say so - foliageType.existsOnDisk:false, saved:false, pendingFlush:true, plus a
//   warnings[] line naming the remedy. Pre-fix the response carries none of those fields at all and
//   its only presence claim is the literal existsAfter:true, so the "the response makes a measured
//   on-disk claim" assertions fail outright.
//
//   Direction 2 (saved). asset.save {force:true} lands the .uasset; a repeat scatter on the same
//   mesh REUSES that type. The probe now finds a file, and the response must flip to
//   foliageType.existsOnDisk:true / saved:true. This half is what makes the test discriminating
//   rather than a one-sided "always says false" check: a hardcoded false would pass Direction 1 and
//   fail here, exactly as the hardcoded true failed Direction 1.
//
//   Direction 3 (nothing was created). Passing a real UFoliageType path creates and dirties no
//   asset, so the save fields must be OMITTED rather than asserted about work this call did not do.
//
// A cross-field invariant is asserted on every response that carries the block: `saved` may never be
// true while `existsOnDisk` is false (durability implies presence), and pendingFlush is exactly
// !saved here because these verbs always request the save. That is what a future regression to a
// constant would have to violate.
//
// FIXTURE ISOLATION. The auto-type name is derived from the MESH basename, so any two tests driving
// the same mesh share one level-global type. Tests/World/TestEnvironmentHandlers.cpp and
// Tests/World/TestFoliageAddInstancesPrecedenceHonesty.cpp drive BasicShapes/Cube and
// Tests/Environment/TestFoliageAutoTypeNaming.cpp drives BasicShapes/Cylinder, so this file drives
// BasicShapes/Cone (add_instances) and BasicShapes/Plane (paint) and owns them alone. Both the
// current Auto_X.Auto_X form and the pre-B-add-instances-auto-foliage-type-name-mismatch Auto_X.X
// form are discarded on entry and on exit: an asset left on disk by an older build would satisfy
// Direction 1's probe and decide the outcome.
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
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

// Named (not anonymous) namespace: the plugin's tests share one module with Unity builds enabled,
// where same-named anonymous-namespace helpers collide across merged translation units.
namespace FoliageAutoTypePersistenceTestHelpers
{
    // Neither Cube nor Cylinder - see FIXTURE ISOLATION above.
    constexpr const TCHAR* AddInstancesMeshPath = TEXT("/Engine/BasicShapes/Cone.Cone");
    constexpr const TCHAR* AddInstancesTypePackage = TEXT("/Game/Foliage/Auto_Cone");
    constexpr const TCHAR* PaintMeshPath = TEXT("/Engine/BasicShapes/Plane.Plane");
    constexpr const TCHAR* PaintTypePackage = TEXT("/Game/Foliage/Auto_Plane");

    // A column of the editor world away from the ones the other foliage test files use.
    constexpr double ColX = 613400.0;
    constexpr double ColY = 527900.0;
    constexpr double ColZ = 41000.0;

    inline UWorld* EditorWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    // Removes the auto type's instances under BOTH naming forms, then deletes the package. Scoped by
    // foliageTypePath rather than removeAll: a removeAll here would empty the host map's own foliage,
    // which is shared state this test does not own. A form that does not exist answers
    // ASSET_NOT_FOUND, which is the expected no-op.
    inline void DiscardAutoType(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
        const FString& TypePackagePath, const FString& MeshPath)
    {
        const FString Stem = FPackageName::GetLongPackageAssetName(TypePackagePath);
        const FString MeshBaseName = FPaths::GetBaseFilename(MeshPath);
        const FString Forms[] = {
            FString::Printf(TEXT("%s.%s"), *TypePackagePath, *Stem),
            FString::Printf(TEXT("%s.%s"), *TypePackagePath, *MeshBaseName)
        };
        for (const FString& ObjectPath : Forms)
        {
            TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
            RemoveParams->SetStringField(TEXT("foliageTypePath"), ObjectPath);

            bool bRemoved = false;
            FString RemoveErrorCode;
            DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("foliage.remove"),
                TEXT("req-foliage-auto-persistence-cleanup"), RemoveParams, bRemoved, RemoveErrorCode);
        }
        CleanupTestAsset(TypePackagePath);
    }

    // The independent measurement every assertion in this file is compared against: is there a
    // .uasset on disk for the package this handle names, right now. Deliberately NOT the asset
    // registry and NOT the package's dirty flag - the defect being guarded is a response that
    // claimed presence without touching the filesystem, so the check has to touch the filesystem.
    inline bool ProbeUassetOnDisk(const FString& AssetPath)
    {
        const FString PackageFilename = PackageFilenameFromAssetPath(AssetPath);
        return !PackageFilename.IsEmpty() && IFileManager::Get().FileSize(*PackageFilename) > 0;
    }

    // Reads the response's own on-disk claim and its save verdict, asserting that each field is
    // PRESENT before comparing it. Presence is half the assertion: the pre-fix response emitted a
    // bare existsAfter:true and none of these, so an absent field must fail rather than default to
    // something that happens to agree with the probe.
    inline void AssertPersistenceMatchesDisk(FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Response, const FString& TypeHandle, const TCHAR* Label)
    {
        if (!Test.TestTrue(*FString::Printf(TEXT("[%s] the response object exists"), Label),
                Response.IsValid()))
        {
            return;
        }

        const bool bOnDisk = ProbeUassetOnDisk(TypeHandle);

        const TSharedPtr<FJsonObject>* TypeVerification = nullptr;
        if (!Test.TestTrue(*FString::Printf(
                TEXT("[%s] the response carries a measured foliageType verification block"), Label),
                Response->TryGetObjectField(TEXT("foliageType"), TypeVerification)
                    && TypeVerification && TypeVerification->IsValid()))
        {
            return;
        }

        // TestTrue over the comparison rather than TestEqual: FAutomationTestBase declares no bool
        // overload, so a bool pair binds to the int32 one and the failure message loses which side
        // was which. Printing both values is what makes a red actionable.
        bool bClaimedOnDisk = false;
        if (Test.TestTrue(*FString::Printf(TEXT("[%s] foliageType.existsOnDisk is published"), Label),
                (*TypeVerification)->TryGetBoolField(TEXT("existsOnDisk"), bClaimedOnDisk)))
        {
            Test.TestTrue(*FString::Printf(
                TEXT("[%s] foliageType.existsOnDisk (%s) agrees with the .uasset probe (%s) for '%s'"),
                Label, bClaimedOnDisk ? TEXT("true") : TEXT("false"),
                bOnDisk ? TEXT("true") : TEXT("false"), *TypeHandle),
                bClaimedOnDisk == bOnDisk);
        }

        bool bSaveRequested = false;
        Test.TestTrue(*FString::Printf(TEXT("[%s] saveRequested names the request separately"), Label),
            Response->TryGetBoolField(TEXT("saveRequested"), bSaveRequested) && bSaveRequested);

        bool bSaved = false;
        if (!Test.TestTrue(*FString::Printf(TEXT("[%s] saved is published"), Label),
                Response->TryGetBoolField(TEXT("saved"), bSaved)))
        {
            return;
        }

        // Durability implies presence. A saved:true over a package with no file is the exact claim
        // this ticket exists to prevent, in whichever direction it is reached.
        Test.TestTrue(*FString::Printf(
            TEXT("[%s] saved:true is never published over a package with no .uasset"), Label),
            !bSaved || bOnDisk);

        // These verbs always request the save, so pendingFlush is emitted exactly when saved is
        // false. Reading it through a defaulted false keeps an absent key meaning "not pending".
        bool bPendingFlush = false;
        Response->TryGetBoolField(TEXT("pendingFlush"), bPendingFlush);
        Test.TestTrue(*FString::Printf(
            TEXT("[%s] pendingFlush (%s) is exactly !saved (saved=%s)"), Label,
            bPendingFlush ? TEXT("true") : TEXT("false"), bSaved ? TEXT("true") : TEXT("false")),
            bPendingFlush == !bSaved);
    }

    // True when any warnings[] string mentions the type handle - the "measured and claimed disagree"
    // disclosure the not-yet-written branch owes the caller.
    inline bool WarningsMentionHandle(const TSharedPtr<FJsonObject>& Response, const FString& Handle)
    {
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (!Response.IsValid() || !Response->TryGetArrayField(TEXT("warnings"), Warnings) || !Warnings)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Warnings)
        {
            FString Text;
            if (Value.IsValid() && Value->TryGetString(Text) && Text.Contains(Handle))
            {
                return true;
            }
        }
        return false;
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

    inline TSharedPtr<FJsonValue> LocationEntry(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Location = MakeShared<FJsonObject>();
        Location->SetNumberField(TEXT("x"), X);
        Location->SetNumberField(TEXT("y"), Y);
        Location->SetNumberField(TEXT("z"), Z);
        return MakeShared<FJsonValueObject>(Location);
    }

    // asset.save with the throttle bypassed. The 0.5 s per-asset window is well inside the rate this
    // test issues calls at, and a throttle-skipped save would leave Direction 2 measuring Direction 1.
    inline bool ForceSaveType(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
        const FString& TypeHandle, FString& OutErrorCode)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("assetPath"), TypeHandle);
        Params->SetBoolField(TEXT("force"), true);

        bool bSuccess = false;
        TSharedPtr<FJsonObject> Result;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, TEXT("asset.save"),
            TEXT("req-foliage-auto-persistence-save"), Params, bSuccess, Result, OutErrorCode);
        return bSuccess;
    }
}

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;

// Each RunTest below opens FoliageAutoTypePersistenceTestHelpers inside its own body rather than at
// file scope: a file-scope using-directive would leak into every other test .cpp that Unity merges
// after this one into the same translation unit.

// ============================================================================
// foliage.add_instances reports the auto-created type's on-disk presence as a
// MEASUREMENT, in both directions
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliageAddInstancesAutoTypePersistenceMeasuredTest,
    "PinWright.foliage.add_instances.AutoCreatedTypePersistenceIsMeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliageAddInstancesAutoTypePersistenceMeasuredTest::RunTest(const FString& Parameters)
{
    using namespace FoliageAutoTypePersistenceTestHelpers;

    if (!EditorWorld())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the auto-created foliage "
                 "type persistence assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    // An asset left on disk by an older build would satisfy Direction 1's probe, so the fixture is
    // cleared before the first call as well as after the last.
    DiscardAutoType(Dispatcher, Sink, AddInstancesTypePackage, AddInstancesMeshPath);
    ON_SCOPE_EXIT
    {
        DiscardAutoType(Dispatcher, Sink, AddInstancesTypePackage, AddInstancesMeshPath);
    };

    // ---- Direction 1: the type has just been created and NOTHING wrote it ----
    FString TypeHandle;
    {
        TArray<TSharedPtr<FJsonValue>> Transforms;
        Transforms.Add(TransformEntry(ColX, ColY, ColZ));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), AddInstancesMeshPath);
        Params->SetArrayField(TEXT("transforms"), Transforms);

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
            TEXT("req-foliage-auto-persistence-add-1"), Params, bSuccess, Result, ErrorCode);

        if (!TestTrue(*FString::Printf(
                TEXT("foliage.add_instances auto-creates a type from a static mesh (error=%s)"),
                *ErrorCode), bSuccess) || !Result.IsValid())
        {
            return true;
        }
        if (!TestTrue(TEXT("the response echoes foliageTypePath"),
                Result->TryGetStringField(TEXT("foliageTypePath"), TypeHandle)
                    && !TypeHandle.IsEmpty()))
        {
            return true;
        }

        // The probe the pre-fix response never made. A fresh auto-create has no .uasset.
        TestFalse(TEXT("the freshly auto-created type has no .uasset on disk"),
            ProbeUassetOnDisk(TypeHandle));
        AssertPersistenceMatchesDisk(*this, Result, TypeHandle, TEXT("fresh auto-create"));

        bool bSaved = true;
        Result->TryGetBoolField(TEXT("saved"), bSaved);
        TestFalse(TEXT("a mark-dirty-only auto-create reports saved:false"), bSaved);

        bool bMarkedForSave = false;
        TestTrue(TEXT("markedForSave tells a deferred write apart from a save never asked for"),
            Result->TryGetBoolField(TEXT("markedForSave"), bMarkedForSave) && bMarkedForSave);

        // The disclosure: measured presence and the response's own upbeat top-level fields disagree,
        // and the response says so rather than leaving the caller to find out after a restart.
        TestTrue(TEXT("warnings[] names the not-yet-written type and its remedy"),
            WarningsMentionHandle(Result, TypeHandle));
    }

    // ---- Direction 2: the same type, now actually on disk ----
    // Without this half the assertions above would also pass a hardcoded false.
    {
        FString SaveErrorCode;
        // Called before the message is built: formatting the code into the message as a sibling
        // argument would read it before the call that fills it.
        const bool bSaveDispatched = ForceSaveType(Dispatcher, Sink, TypeHandle, SaveErrorCode);
        if (!TestTrue(*FString::Printf(TEXT("asset.save force-writes the auto-created type (error=%s)"),
                *SaveErrorCode), bSaveDispatched))
        {
            return true;
        }
        if (!TestTrue(TEXT("the .uasset is on disk after asset.save"), ProbeUassetOnDisk(TypeHandle)))
        {
            return true;
        }

        TArray<TSharedPtr<FJsonValue>> Transforms;
        Transforms.Add(TransformEntry(ColX + 400.0, ColY, ColZ));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), AddInstancesMeshPath);
        Params->SetArrayField(TEXT("transforms"), Transforms);

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
            TEXT("req-foliage-auto-persistence-add-2"), Params, bSuccess, Result, ErrorCode);

        if (!TestTrue(*FString::Printf(
                TEXT("a repeat scatter reuses the saved auto type (error=%s)"), *ErrorCode),
                bSuccess) || !Result.IsValid())
        {
            return true;
        }

        AssertPersistenceMatchesDisk(*this, Result, TypeHandle, TEXT("reused saved type"));

        bool bSaved = false;
        TestTrue(TEXT("a reused, already-written type reports saved:true"),
            Result->TryGetBoolField(TEXT("saved"), bSaved) && bSaved);
        TestFalse(TEXT("a durable type publishes no warning about being unwritten"),
            WarningsMentionHandle(Result, TypeHandle));
    }

    // ---- Direction 3: nothing was created, so nothing is claimed ----
    // Passing the resolved UFoliageType path creates and dirties no asset. OMIT rather than assert.
    {
        TArray<TSharedPtr<FJsonValue>> Transforms;
        Transforms.Add(TransformEntry(ColX + 800.0, ColY, ColZ));

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("foliageTypePath"), TypeHandle);
        Params->SetArrayField(TEXT("transforms"), Transforms);

        bool bSuccess = false;
        FString ErrorCode;
        TSharedPtr<FJsonObject> Result;
        Dispatch(Dispatcher, Sink, TEXT("foliage.add_instances"),
            TEXT("req-foliage-auto-persistence-add-3"), Params, bSuccess, Result, ErrorCode);

        if (TestTrue(*FString::Printf(
                TEXT("a scatter onto an existing foliage type succeeds (error=%s)"), *ErrorCode),
                bSuccess) && Result.IsValid())
        {
            bool bIgnored = false;
            TestFalse(TEXT("no saveRequested is published when no asset was created"),
                Result->TryGetBoolField(TEXT("saveRequested"), bIgnored));
            const TSharedPtr<FJsonObject>* TypeVerification = nullptr;
            TestFalse(TEXT("no foliageType verification block is published when no asset was created"),
                Result->TryGetObjectField(TEXT("foliageType"), TypeVerification));
        }
    }

    return true;
}

// ============================================================================
// foliage.paint makes the same measured claim over the same shared helper
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoliagePaintAutoTypePersistenceMeasuredTest,
    "PinWright.foliage.paint.AutoCreatedTypePersistenceIsMeasured",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FFoliagePaintAutoTypePersistenceMeasuredTest::RunTest(const FString& Parameters)
{
    using namespace FoliageAutoTypePersistenceTestHelpers;

    if (!EditorWorld())
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no_editor_world"),
            TEXT("GEditor->GetEditorWorldContext().World() returned null; the painted auto-created "
                 "foliage type persistence assertions were stepped over."));
        return true;
    }

    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    DiscardAutoType(Dispatcher, Sink, PaintTypePackage, PaintMeshPath);
    ON_SCOPE_EXIT
    {
        DiscardAutoType(Dispatcher, Sink, PaintTypePackage, PaintMeshPath);
    };

    // No `surface`, so the verb writes the literal z. Ground projection is irrelevant here - what is
    // under test is what the response says about the type asset it just created.
    TArray<TSharedPtr<FJsonValue>> Locations;
    Locations.Add(LocationEntry(ColX, ColY + 900.0, ColZ));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("foliageTypePath"), PaintMeshPath);
    Params->SetArrayField(TEXT("locations"), Locations);

    bool bSuccess = false;
    FString ErrorCode;
    TSharedPtr<FJsonObject> Result;
    Dispatch(Dispatcher, Sink, TEXT("foliage.paint"),
        TEXT("req-foliage-paint-auto-persistence"), Params, bSuccess, Result, ErrorCode);

    if (!TestTrue(*FString::Printf(
            TEXT("foliage.paint auto-creates a type from a static mesh (error=%s)"), *ErrorCode),
            bSuccess) || !Result.IsValid())
    {
        return true;
    }

    // The handle the response publishes is what the disk probe is run against - not a path this
    // test reconstructs - so a verb that echoed one string and measured another cannot pass.
    FString TypeHandle;
    if (!TestTrue(TEXT("foliage.paint echoes the resolved foliageTypePath"),
            Result->TryGetStringField(TEXT("foliageTypePath"), TypeHandle) && !TypeHandle.IsEmpty()))
    {
        return true;
    }
    TestEqual(TEXT("the painted auto type lands at the documented package path"),
        FPackageName::ObjectPathToPackageName(TypeHandle), FString(PaintTypePackage));

    TestFalse(TEXT("the freshly auto-created painted type has no .uasset on disk"),
        ProbeUassetOnDisk(TypeHandle));
    AssertPersistenceMatchesDisk(*this, Result, TypeHandle, TEXT("painted auto-create"));

    bool bSaved = true;
    Result->TryGetBoolField(TEXT("saved"), bSaved);
    TestFalse(TEXT("a painted mark-dirty-only auto-create reports saved:false"), bSaved);
    TestTrue(TEXT("warnings[] names the not-yet-written painted type and its remedy"),
        WarningsMentionHandle(Result, TypeHandle));

    return true;
}
