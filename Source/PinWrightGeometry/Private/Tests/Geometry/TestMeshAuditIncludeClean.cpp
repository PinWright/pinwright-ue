// Copyright (c) 2026 Alexander Penkin. MIT License.

// geometry.audit_static_meshes includeClean (B-mesh-audit-includeclean-emits-nothing).
//
// Two defects, two tests. The handler test pins the wire: with includeClean the response
// carries `cleanAssets` and `cleanChecks` even when both are empty - the old response dropped
// the key whenever no asset was clean, which is byte-identical to the flag being ignored. The
// pure test pins the measurement channel: a check that came back clean still yields its
// measurements, on an asset whose OTHER check flagged too.
#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/MeshAuditUtils.h"
#include "State/JobRegistry.h"
#include "State/PluginState.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace TestMeshAuditIncludeCleanHelpers
{
    // Runs the verb over one well-formed path the registry has nothing at, which folds in as
    // an unrunnable asset: a sweep with findings and no clean asset, the reported case. The
    // job completes inside the handler call, so its result is read straight off the registry.
    TSharedPtr<FJsonObject> RunMissingAssetSweep(FAutomationTestBase& Test, bool bIncludeClean)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Assets;
        Assets.Add(MakeShared<FJsonValueString>(
            TEXT("/Game/PinWrightTests/MeshAuditIncludeClean/SM_NotThere")));
        Payload->SetArrayField(TEXT("assets"), Assets);
        Payload->SetBoolField(TEXT("includeClean"), bIncludeClean);

        FTestResponseCapture Capture;
        if (!Test.TestTrue(TEXT("geometry.audit_static_meshes is registered"),
                InvokeHandlerWithCapture(TEXT("geometry.audit_static_meshes"), Payload, Capture))
            || !Test.TestTrue(TEXT("the sweep starts"), Capture.bSuccess && Capture.Result.IsValid()))
        {
            return nullptr;
        }
        FString TicketId;
        Capture.Result->TryGetStringField(TEXT("ticket_id"), TicketId);
        FJobTicket Ticket;
        if (!Test.TestTrue(TEXT("the sweep's ticket is registered"),
                FPluginState::Get().GetJobRegistry().Get(TicketId, Ticket))
            || !Test.TestEqual(TEXT("the sweep completed inside the call"),
                Ticket.Status, FString(TEXT("completed"))))
        {
            return nullptr;
        }
        return Ticket.Result;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditIncludeCleanKeysTest,
    "PinWright.Geometry.MeshAudit.IncludeCleanEmitsEmptyCleanKeys",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditIncludeCleanKeysTest::RunTest(const FString& Parameters)
{
    const TSharedPtr<FJsonObject> With =
        TestMeshAuditIncludeCleanHelpers::RunMissingAssetSweep(*this, true);
    if (!With.IsValid())
    {
        return false;
    }
    // Precondition: this is the reported shape - findings present, nothing clean.
    const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
    TestTrue(TEXT("fixture sweep produced findings"),
        With->TryGetArrayField(TEXT("findings"), Findings) && Findings->Num() > 0);

    const TArray<TSharedPtr<FJsonValue>>* CleanAssets = nullptr;
    TestTrue(TEXT("includeClean emits cleanAssets even when no asset is clean"),
        With->TryGetArrayField(TEXT("cleanAssets"), CleanAssets));
    TestTrue(TEXT("and it is empty"), CleanAssets && CleanAssets->Num() == 0);
    const TArray<TSharedPtr<FJsonValue>>* CleanChecks = nullptr;
    TestTrue(TEXT("includeClean emits cleanChecks even when no check is clean"),
        With->TryGetArrayField(TEXT("cleanChecks"), CleanChecks));
    TestTrue(TEXT("and it is empty"), CleanChecks && CleanChecks->Num() == 0);
    bool bTruncated = true;
    TestTrue(TEXT("includeClean reports cleanChecksTruncated:false"),
        With->TryGetBoolField(TEXT("cleanChecksTruncated"), bTruncated) && !bTruncated);

    const TSharedPtr<FJsonObject> Without =
        TestMeshAuditIncludeCleanHelpers::RunMissingAssetSweep(*this, false);
    if (!Without.IsValid())
    {
        return false;
    }
    TestFalse(TEXT("without includeClean there is no cleanAssets key"),
        Without->HasField(TEXT("cleanAssets")));
    TestFalse(TEXT("without includeClean there is no cleanChecks key"),
        Without->HasField(TEXT("cleanChecks")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMeshAuditIncludeCleanMeasurementsTest,
    "PinWright.Geometry.MeshAudit.IncludeCleanCarriesCleanCheckMeasurements",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMeshAuditIncludeCleanMeasurementsTest::RunTest(const FString& Parameters)
{
    // Two parallel unit squares one unit apart, in separate components: z_fighting runs and is
    // clean. empty_mesh is selected too and is also clean. Neither yields a finding, so before
    // the fix neither yielded a single number.
    auto Square = [](int32 FirstId, int32 Component, double Z)
    {
        TArray<MeshAudit::FZFightTriangle> Out;
        const FVector3d Corners[] = {FVector3d(0, 0, Z), FVector3d(1, 0, Z),
                                     FVector3d(1, 1, Z), FVector3d(0, 1, Z)};
        const int32 Tris[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (int32 T = 0; T < 2; ++T)
        {
            MeshAudit::FZFightTriangle Tri;
            Tri.TriangleID = FirstId + T;
            Tri.ComponentIndex = Component;
            Tri.V0 = Corners[Tris[T][0]];
            Tri.V1 = Corners[Tris[T][1]];
            Tri.V2 = Corners[Tris[T][2]];
            const FVector3d Cross = FVector3d::CrossProduct(Tri.V1 - Tri.V0, Tri.V2 - Tri.V0);
            Tri.Area = 0.5 * Cross.Length();
            Tri.Normal = Cross.GetSafeNormal();
            Tri.BoundsMin = FVector3d(0, 0, Z);
            Tri.BoundsMax = FVector3d(1, 1, Z);
            Out.Add(Tri);
        }
        return Out;
    };

    MeshAudit::FAssetMeasurement Clean;
    Clean.bMeasured = true;
    Clean.ZFightTriangles = Square(0, 0, 0.0);
    Clean.ZFightTriangles.Append(Square(2, 1, 1.0));
    Clean.Health.TriangleCount = Clean.ZFightTriangles.Num();
    Clean.Health.VertexCount = 8;

    // The same squares coincident: z_fighting flags, empty_mesh stays clean on that asset.
    MeshAudit::FAssetMeasurement Fighting = Clean;
    Fighting.ZFightTriangles = Square(0, 0, 0.0);
    Fighting.ZFightTriangles.Append(Square(2, 1, 0.0));

    MeshAudit::FConfig Config;
    Config.SelectedChecks = MeshAudit::CheckBit(MeshAudit::ECheck::ZFighting)
        | MeshAudit::CheckBit(MeshAudit::ECheck::EmptyMesh);
    Config.MaxCleanAssets = 500;

    MeshAudit::FReport Report;
    MeshAudit::EvaluateAsset(TEXT("/Game/T/Clean.Clean"), TEXT("Clean"), Clean, Config, Report);
    MeshAudit::EvaluateAsset(TEXT("/Game/T/Fight.Fight"), TEXT("Fight"), Fighting, Config, Report);

    TestEqual(TEXT("precondition: the coincident asset drew exactly one finding"),
        Report.Findings.Num(), 1);
    TestEqual(TEXT("only the fully clean asset is in cleanAssets"), Report.CleanAssets.Num(), 1);

    // Clean asset: both checks; fighting asset: empty_mesh only.
    TestEqual(TEXT("three clean (asset, check) rows"), Report.CleanChecks.Num(), 3);
    bool bCleanZFightMeasured = false;
    bool bFlaggedAssetCleanCheckKept = false;
    for (const MeshAudit::FCleanCheck& Row : Report.CleanChecks)
    {
        TestTrue(TEXT("every clean row carries measurements"),
            Row.Measurements.IsValid() && Row.Measurements->Values.Num() > 0);
        if (Row.AssetName == TEXT("Clean") && Row.Check == MeshAudit::ECheck::ZFighting)
        {
            double Candidates = -1.0;
            bCleanZFightMeasured = Row.Measurements.IsValid()
                && Row.Measurements->TryGetNumberField(TEXT("candidatePairCount"), Candidates)
                && Row.Measurements->HasField(TEXT("broadPhaseWork"));
        }
        if (Row.AssetName == TEXT("Fight"))
        {
            bFlaggedAssetCleanCheckKept = Row.Check == MeshAudit::ECheck::EmptyMesh;
        }
    }
    TestTrue(TEXT("a clean z_fighting result still reports its broad-phase measurements"),
        bCleanZFightMeasured);
    TestTrue(TEXT("an asset with a finding still reports its clean checks"),
        bFlaggedAssetCleanCheckKept);

    MeshAudit::FConfig NoClean = Config;
    NoClean.MaxCleanAssets = 0;
    MeshAudit::FReport Quiet;
    MeshAudit::EvaluateAsset(TEXT("/Game/T/Clean.Clean"), TEXT("Clean"), Clean, NoClean, Quiet);
    TestEqual(TEXT("without includeClean nothing is collected"), Quiet.CleanChecks.Num(), 0);

    // Clean rows are capped at maxFindings, and the clip is reported rather than silent.
    TestFalse(TEXT("the uncapped run was not truncated"), Report.bCleanChecksTruncated);
    MeshAudit::FConfig Capped = Config;
    Capped.MaxFindings = 2;
    MeshAudit::FReport Clipped;
    MeshAudit::EvaluateAsset(TEXT("/Game/T/Clean.Clean"), TEXT("Clean"), Clean, Capped, Clipped);
    MeshAudit::EvaluateAsset(TEXT("/Game/T/Clean2.Clean2"), TEXT("Clean2"), Clean, Capped, Clipped);
    TestEqual(TEXT("clean rows stop at maxFindings"), Clipped.CleanChecks.Num(), 2);
    TestTrue(TEXT("and the clip is flagged"), Clipped.bCleanChecksTruncated);
    TestTrue(TEXT("a clipped clean channel does not fail the sweep"),
        Clipped.DerivePass(TEXT("error")));
    return true;
}
