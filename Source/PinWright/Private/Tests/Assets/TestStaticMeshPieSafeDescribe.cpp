// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Templates/UnrealTemplate.h"

#include "Handlers/Asset/StaticMeshDumpBuilder.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshDescribePieSafeNativeLodCountTest,
    "PinWright.static_mesh.describe.PieSafeNativeLodCount",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Counterfactual: routing this builder through UStaticMeshEditorSubsystem::GetLodCount would
// return -1 under the guarded flag below, failing both the native-count and positive assertions.
bool FStaticMeshDescribePieSafeNativeLodCountTest::RunTest(const FString& Parameters)
{
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("Engine cube fixture loads"), Mesh))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Result;
    {
        TGuardValue<bool> PieWorldFlagGuard(GIsPlayInEditorWorld, true);
        Result = StaticMeshDumpBuilder::BuildStaticMeshJson(Mesh);
    }

    if (!TestTrue(TEXT("Static-mesh summary is built while PIE flag is set"), Result.IsValid()))
    {
        return false;
    }

    double ReportedLodCount = -1.0;
    TestTrue(TEXT("Summary carries a LOD count"),
        Result->TryGetNumberField(TEXT("lods"), ReportedLodCount));
    TestEqual(TEXT("Summary uses the mesh's native LOD count during PIE"),
        static_cast<int32>(ReportedLodCount), Mesh->GetNumLODs());
    TestTrue(TEXT("Engine cube LOD count is not the subsystem's PIE sentinel"),
        ReportedLodCount > 0.0);
    return true;
}
