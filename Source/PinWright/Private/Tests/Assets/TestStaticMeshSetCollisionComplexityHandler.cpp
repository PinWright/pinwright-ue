// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for the typed collision-complexity writer used by level authoring.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"

#include "BodySetupEnums.h"
#include "Engine/StaticMesh.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshSetCollisionComplexityTest,
    "PinWright.static_mesh.set_collision_complexity.UpdatesBodySetup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticMeshSetCollisionComplexityTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SM_CollisionComplexity_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    UPackage* Package = CreatePackage(*PackagePath);
    UStaticMesh* Mesh = NewObject<UStaticMesh>(
        Package, FName(*AssetName), RF_Public | RF_Standalone);

    TestNotNull(TEXT("Temporary StaticMesh created"), Mesh);
    if (!Mesh)
    {
        return false;
    }

    Mesh->AddToRoot();
    ON_SCOPE_EXIT
    {
        Package->SetDirtyFlag(false);
        Mesh->RemoveFromRoot();
        CleanupTestAsset(PackagePath);
    };

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Mesh->GetPathName());
    Payload->SetStringField(TEXT("complexity"), TEXT("complex_as_simple"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("setter is registered"), InvokeHandlerWithCapture(
        TEXT("static_mesh.set_collision_complexity"), Payload, Capture));
    TestTrue(TEXT("setter succeeds"), Capture.bSuccess);
    TestNotNull(TEXT("setter creates a BodySetup"), Mesh->GetBodySetup());
    if (!Capture.bSuccess || !Capture.Result.IsValid() || !Mesh->GetBodySetup())
    {
        return false;
    }

    TestEqual(TEXT("ground truth BodySetup flag is complex-as-simple"),
        static_cast<ECollisionTraceFlag>(Mesh->GetBodySetup()->CollisionTraceFlag),
        ECollisionTraceFlag::CTF_UseComplexAsSimple);
    FString TraceFlag;
    TestTrue(TEXT("response echoes collisionTraceFlag"),
        Capture.Result->TryGetStringField(TEXT("collisionTraceFlag"), TraceFlag));
    TestEqual(TEXT("response reads back the applied raw flag"),
        TraceFlag, FString(TEXT("CTF_UseComplexAsSimple")));
    bool bChanged = false;
    TestTrue(TEXT("response reports changed"),
        Capture.Result->TryGetBoolField(TEXT("changed"), bChanged) && bChanged);
    bool bSaved = true;
    TestTrue(TEXT("response carries saved field"),
        Capture.Result->TryGetBoolField(TEXT("saved"), bSaved));
    TestFalse(TEXT("save:false reports saved:false"), bSaved);

    // Invalid input must fail before touching the existing flag.
    Payload->SetStringField(TEXT("complexity"), TEXT("per_poly_maybe"));
    FTestResponseCapture InvalidCapture;
    TestTrue(TEXT("setter handles invalid mode"), InvokeHandlerWithCapture(
        TEXT("static_mesh.set_collision_complexity"), Payload, InvalidCapture));
    TestFalse(TEXT("invalid mode is rejected"), InvalidCapture.bSuccess);
    TestEqual(TEXT("invalid mode returns a stable error code"),
        InvalidCapture.ErrorCode, FString(TEXT("INVALID_COLLISION_COMPLEXITY")));
    TestEqual(TEXT("invalid mode leaves BodySetup unchanged"),
        static_cast<ECollisionTraceFlag>(Mesh->GetBodySetup()->CollisionTraceFlag),
        ECollisionTraceFlag::CTF_UseComplexAsSimple);

    return true;
}
