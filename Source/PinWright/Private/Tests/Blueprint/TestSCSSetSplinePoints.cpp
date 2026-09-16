// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for F-scs-spline-points.
//
// blueprint.scs.set_spline_points must author the point data of a USplineComponent that
// lives in a Blueprint's SCS class template — the gap that previously forced a
// python.execute SubobjectDataSubsystem fallback because every spline.* point setter is
// actor-scoped and blueprint.scs.add_component only creates the component, never its points.
//
// This builds a transient Blueprint whose SCS holds a spline component, dispatches the verb
// with three local-space points + closedLoop, and asserts on the ACTUAL component template
// (not just the response echo) that the points landed, the closed-loop flag flipped, and the
// reparam table was rebuilt (GetSplineLength > 0 — the UpdateSpline recompute). Reverting the
// handler makes InvokeHandlerWithCapture fail to find the method; dropping the UpdateSpline
// call leaves GetSplineLength at 0. Either failure trips this test.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SplineComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace
{
    FString MakeScsSplineAssetPath()
    {
        return FString::Printf(TEXT("/Game/__PW_GatewayTests/BP_ScsSetSplinePoints_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    void CleanupScsSplineAsset(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* ExistingPackage = FindPackage(nullptr, *PackagePath);
        UObject* ExistingObject = ExistingPackage
            ? FindObject<UObject>(ExistingPackage, *AssetName)
            : nullptr;

        if (ExistingPackage)
        {
            FAssetRegistryModule::PackageDeleted(ExistingPackage);
            ExistingPackage->ClearFlags(RF_Standalone | RF_Public);
            ExistingPackage->SetDirtyFlag(false);
            ExistingPackage->MarkAsGarbage();
        }
        if (ExistingObject)
        {
            ExistingObject->ClearFlags(RF_Standalone | RF_Public);
            ExistingObject->RemoveFromRoot();
            ExistingObject->MarkAsGarbage();
        }
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }

    // Re-resolve the spline template on the Blueprint's SCS by variable name.
    USplineComponent* FindScsSplineTemplate(UBlueprint* Blueprint, const FString& ComponentName)
    {
        if (!Blueprint || !Blueprint->SimpleConstructionScript)
        {
            return nullptr;
        }
        for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
        {
            if (Node && Node->GetVariableName().IsValid() &&
                Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
            {
                return Cast<USplineComponent>(Node->ComponentTemplate);
            }
        }
        return nullptr;
    }

    TSharedPtr<FJsonValue> MakeXYZ(double X, double Y, double Z)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetNumberField(TEXT("x"), X);
        Obj->SetNumberField(TEXT("y"), Y);
        Obj->SetNumberField(TEXT("z"), Z);
        return MakeShared<FJsonValueObject>(Obj);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScsSetSplinePointsAuthorsTemplateTest,
    "PinWright.blueprint.scs.set_spline_points.AuthorsSCSTemplate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FBlueprintScsSetSplinePointsAuthorsTemplateTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeScsSplineAssetPath();
    ON_SCOPE_EXIT { CleanupScsSplineAsset(BPPath); };

    const FString AssetName = FPackageName::GetLongPackageAssetName(BPPath);
    UPackage* Package = CreatePackage(*BPPath);
    if (!TestNotNull(TEXT("package allocated"), Package))
    {
        return false;
    }
    Package->SetFlags(RF_Transient);

    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        FName(*AssetName),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("blueprint created"), Blueprint) || !Blueprint->SimpleConstructionScript)
    {
        return false;
    }

    // Add a spline component to the SCS class template (the state after
    // blueprint.scs.add_component).
    const FString ComponentName = TEXT("OpeningSpline");
    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    USCS_Node* SplineNode = SCS->CreateNode(USplineComponent::StaticClass(), *ComponentName);
    if (!TestNotNull(TEXT("spline SCS node created"), SplineNode))
    {
        return false;
    }
    SCS->AddNode(SplineNode);

    // Author 3 distinct local-space points + close the loop via the verb under test.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), BPPath);
    Payload->SetStringField(TEXT("componentName"), ComponentName);
    Payload->SetStringField(TEXT("pointType"), TEXT("Linear"));
    Payload->SetBoolField(TEXT("closedLoop"), true);
    TArray<TSharedPtr<FJsonValue>> Points;
    Points.Add(MakeXYZ(0.0, 0.0, 0.0));
    Points.Add(MakeXYZ(200.0, 0.0, 0.0));
    Points.Add(MakeXYZ(200.0, 200.0, 0.0));
    Payload->SetArrayField(TEXT("points"), Points);

    FTestResponseCapture Capture;
    // Reverting the handler removes the registration -> InvokeHandlerWithCapture returns false.
    if (!TestTrue(TEXT("blueprint.scs.set_spline_points handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.scs.set_spline_points"), Payload, Capture)))
    {
        return false;
    }
    TestTrue(TEXT("handler succeeded"), Capture.bSuccess);
    TestEqual(TEXT("no error code"), Capture.ErrorCode, FString());

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        double EchoedCount = 0.0;
        Capture.Result->TryGetNumberField(TEXT("pointCount"), EchoedCount);
        TestEqual(TEXT("response echoes pointCount 3"), (int32)EchoedCount, 3);

        double EchoedLength = 0.0;
        Capture.Result->TryGetNumberField(TEXT("splineLength"), EchoedLength);
        TestTrue(TEXT("response echoes a positive splineLength"), EchoedLength > 0.0);
    }

    // Ground truth: inspect the actual SCS component template, not just the echo.
    USplineComponent* Template = FindScsSplineTemplate(Blueprint, ComponentName);
    if (!TestNotNull(TEXT("spline template still resolvable"), Template))
    {
        return false;
    }
    TestEqual(TEXT("template holds the 3 authored points"),
        Template->GetNumberOfSplinePoints(), 3);
    TestTrue(TEXT("template closed-loop flag set"), Template->IsClosedLoop());
    // UpdateSpline recompute proof: the reparam table only yields a non-zero length after
    // UpdateSpline runs over the authored points.
    TestTrue(TEXT("template spline length rebuilt (>0) by UpdateSpline"),
        Template->GetSplineLength() > 0.0);

    return true;
}
