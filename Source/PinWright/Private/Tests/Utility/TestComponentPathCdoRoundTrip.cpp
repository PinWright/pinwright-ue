// Copyright (c) 2026 Alexander Penkin. MIT License.

// blueprint.scs.get and asset.dump report component object-refs as
// "<BP>.Default__<BP>_C:<Sub>". That shape has to resolve back through
// PinWrightRpc::ComponentPath::Resolve, which is what every vehicle.* write verb
// uses for its componentPath, or the path a read RPC hands back cannot be pasted
// into a write RPC.

#include "Misc/AutomationTest.h"

#include "Components/ActorComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "Utils/ComponentPathUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComponentPathCdoDefaultShapeRoundTripTest,
    "PinWright.utils.component_path.CdoDefaultShapeRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FComponentPathCdoDefaultShapeRoundTripTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = FString::Printf(TEXT("/Game/__McpTest__/T_CdoPath_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::DigitsLower).Left(8));
    const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);

    UPackage* Package = CreatePackage(*AssetPath);
    if (!TestNotNull(TEXT("fixture package created"), Package))
    {
        return false;
    }

    // AStaticMeshActor parent: its component is a NATIVE default subobject, so it lives on
    // the CDO where GetDefaultSubobjectByName can see it. An SCS-added component is outered
    // to the generated class instead and would not exercise this resolver path at all.
    UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
        AStaticMeshActor::StaticClass(),
        Package,
        FName(*AssetName),
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("fixture blueprint created"), BP) || !BP->GeneratedClass)
    {
        CleanupTestAsset(AssetPath);
        return false;
    }

    UObject* CDO = BP->GeneratedClass->GetDefaultObject();
    if (!TestNotNull(TEXT("generated class CDO available"), CDO))
    {
        CleanupTestAsset(AssetPath);
        return false;
    }

    // Read the component name off the CDO instead of hard-coding the engine's
    // "StaticMeshComponent0", which is an engine implementation detail.
    TArray<UObject*> Subobjects;
    CDO->GetDefaultSubobjects(Subobjects);
    UActorComponent* ExpectedComponent = nullptr;
    for (UObject* Subobject : Subobjects)
    {
        if (UActorComponent* AsComponent = Cast<UActorComponent>(Subobject))
        {
            ExpectedComponent = AsComponent;
            break;
        }
    }
    if (!TestNotNull(TEXT("parent class contributes a native default component"), ExpectedComponent))
    {
        CleanupTestAsset(AssetPath);
        return false;
    }
    const FString ComponentName = ExpectedComponent->GetName();

    FString ClassShapeError;
    UActorComponent* FromClassShape = PinWrightRpc::ComponentPath::Resolve(
        FString::Printf(TEXT("%s.%s_C:%s"), *AssetPath, *AssetName, *ComponentName),
        ClassShapeError);
    TestNotNull(TEXT("generated-class shape resolves"), FromClassShape);

    FString AssetShapeError;
    UActorComponent* FromAssetShape = PinWrightRpc::ComponentPath::Resolve(
        FString::Printf(TEXT("%s:%s"), *AssetPath, *ComponentName),
        AssetShapeError);
    TestNotNull(TEXT("blueprint-asset shape resolves"), FromAssetShape);

    FString CdoShapeError;
    UActorComponent* FromCdoShape = PinWrightRpc::ComponentPath::Resolve(
        FString::Printf(TEXT("%s.Default__%s_C:%s"), *AssetPath, *AssetName, *ComponentName),
        CdoShapeError);
    TestNotNull(TEXT("CDO-instance shape resolves"), FromCdoShape);
    TestEqual(TEXT("CDO-instance shape reports no error"), CdoShapeError, FString());
    TestTrue(TEXT("CDO-instance shape resolves to the same component as the generated-class shape"),
        FromCdoShape != nullptr && FromCdoShape == FromClassShape);
    TestTrue(TEXT("CDO-instance shape resolves to the same component as the blueprint-asset shape"),
        FromCdoShape != nullptr && FromCdoShape == FromAssetShape);

    // The CDO redirect must not widen the resolver: an unknown subobject on the same CDO
    // still has to miss.
    FString MissingError;
    TestNull(TEXT("unknown subobject on the CDO shape still misses"),
        PinWrightRpc::ComponentPath::Resolve(
            FString::Printf(TEXT("%s.Default__%s_C:NoSuchComponent"), *AssetPath, *AssetName),
            MissingError));
    TestEqual(TEXT("unknown subobject reports COMPONENT_NOT_FOUND"),
        MissingError, FString(TEXT("COMPONENT_NOT_FOUND")));

    CleanupTestAsset(AssetPath);
    return true;
}
