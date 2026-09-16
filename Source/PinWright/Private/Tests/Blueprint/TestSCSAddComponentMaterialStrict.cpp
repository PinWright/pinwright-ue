// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for the pre-flight material/mesh contract on
// blueprint.scs.add_component (B5).
//
// The defect: a bad materialPath used to return success:true with material_applied:false
// AFTER the SCS node had already been created, linked, compiled and saved. A bad path, an
// unloadable asset and a component class that can never hold a material were all
// indistinguishable, and the caller was left with a persisted half-applied Blueprint edit.
//
// GROUND TRUTH: the failure tests do not settle for the error code - they assert the SCS
// node count is UNCHANGED and the requested node does not exist. That is what proves the
// mutation was never committed, which is the actual defect. An implementation that added
// the node and then reported an error would pass a code-only check and fail these.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"


namespace
{
    // Engine material present on every UE install - the same probe the sibling
    // actor.spawn material tests use.
    const TCHAR* const ScsMatStrictProbeMaterial =
        TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");

    // Every helper here is uniquely named so a Unity merge cannot ODR-clash it with the
    // same-shaped fixture helpers in sibling Tests/Blueprint files.
    FString MakeScsMatStrictAssetPath(const FString& Prefix)
    {
        return FString::Printf(TEXT("/Game/__PW_GatewayTests/%s_%s"),
            *Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Cleanup for a transient fixture package (mirrors TestSCSDuplicateComponentHandler.cpp):
    // the handler compiles the Blueprint, so the object must leave the transient package and the
    // asset registry behind it.
    void CleanupScsMatStrictAsset(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);

        UPackage* ExistingPackage = FindPackage(nullptr, *PackagePath);
        UObject* ExistingObject = ExistingPackage
            ? FindObject<UObject>(ExistingPackage, *AssetName)
            : FindObject<UObject>(nullptr, *ObjectPath);

        if (ExistingPackage && ExistingPackage->HasAnyFlags(RF_Transient))
        {
            FAssetRegistryModule::PackageDeleted(ExistingPackage);
            ExistingPackage->ClearFlags(RF_Standalone | RF_Public);
            ExistingPackage->SetDirtyFlag(false);

            if (ExistingObject)
            {
                ExistingObject->ClearFlags(RF_Standalone | RF_Public);
                ExistingObject->RemoveFromRoot();
                ExistingObject->MarkAsGarbage();
            }

            ExistingPackage->MarkAsGarbage();
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
            return;
        }

        if (ExistingObject && !ExistingObject->IsAsset())
        {
            ExistingPackage = ExistingObject->GetOutermost();
            if (!ExistingPackage)
            {
                ExistingPackage = FindPackage(nullptr, *PackagePath);
            }
            if (ExistingPackage)
            {
                FAssetRegistryModule::PackageDeleted(ExistingPackage);
                ExistingPackage->ClearFlags(RF_Standalone | RF_Public);
                ExistingPackage->SetDirtyFlag(false);
                ExistingPackage->MarkAsGarbage();
            }

            ExistingObject->ClearFlags(RF_Standalone | RF_Public);
            ExistingObject->RemoveFromRoot();
            ExistingObject->MarkAsGarbage();

            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
            return;
        }

        // No force-delete tail. Every caller passes MakeScsMatStrictAssetPath(), which always
        // yields a /Game/__PW_GatewayTests/ path, so the guard that used to sit here
        // (`StartsWith("/Game/__PW_GatewayTests/") -> return`) always fired and the
        // UEditorAssetLibrary::DeleteAsset branches below it were unreachable.
    }

    // Empty AActor-derived fixture Blueprint in a transient package. Transient keeps the
    // handler's compile+MarkPackageDirty off disk while LoadBlueprintAsset still resolves
    // it by in-memory object path.
    UBlueprint* CreateScsMatStrictFixture(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        Package->SetFlags(RF_Transient);

        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
    }

    int32 ScsMatStrictNodeCount(const UBlueprint* Blueprint)
    {
        return (Blueprint && Blueprint->SimpleConstructionScript)
            ? Blueprint->SimpleConstructionScript->GetAllNodes().Num()
            : -1;
    }

    TSharedPtr<FJsonObject> MakeScsMatStrictPayload(const FString& PackagePath,
        const TCHAR* ComponentClass, const TCHAR* ComponentName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
        Payload->SetStringField(TEXT("componentClass"), ComponentClass);
        Payload->SetStringField(TEXT("componentName"), ComponentName);
        return Payload;
    }
}

// ============================================================================
// A loadable materialPath still applies: the strict pre-flight must not regress the
// working path, and a clean apply carries no warnings key.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScsAddComponentMaterialAppliedTest,
    "PinWright.blueprint.scs.add_component.MaterialApplied",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScsAddComponentMaterialAppliedTest::RunTest(const FString& Parameters)
{
    UMaterialInterface* Probe =
        LoadObject<UMaterialInterface>(nullptr, ScsMatStrictProbeMaterial);
    if (!Probe)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("WorldGridMaterial unavailable; skipping SCS material apply test."));
        return true;
    }

    const FString BPPath = MakeScsMatStrictAssetPath(TEXT("BP_ScsMatApplied"));
    ON_SCOPE_EXIT { CleanupScsMatStrictAsset(BPPath); };

    UBlueprint* Blueprint = CreateScsMatStrictFixture(BPPath);
    TestNotNull(TEXT("fixture blueprint created"), Blueprint);
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload =
        MakeScsMatStrictPayload(BPPath, TEXT("StaticMeshComponent"), TEXT("MatMesh"));
    Payload->SetStringField(TEXT("materialPath"), ScsMatStrictProbeMaterial);

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.scs.add_component handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.scs.add_component"), Payload, Capture));
    TestTrue(TEXT("add_component with a loadable materialPath succeeds"), Capture.bSuccess);

    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        bool bApplied = false;
        TestTrue(TEXT("response carries material_applied"),
            Capture.Result->TryGetBoolField(TEXT("material_applied"), bApplied));
        TestTrue(TEXT("material_applied is true"), bApplied);

        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        TestFalse(TEXT("a clean apply carries no warnings"),
            Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings));
    }

    // GROUND TRUTH: slot 0 of the created component TEMPLATE holds the probe material.
    USCS_Node* Node = Blueprint->SimpleConstructionScript->FindSCSNode(FName(TEXT("MatMesh")));
    TestNotNull(TEXT("component node was added"), Node);
    UStaticMeshComponent* Template =
        Node ? Cast<UStaticMeshComponent>(Node->ComponentTemplate) : nullptr;
    TestNotNull(TEXT("component template is a static mesh component"), Template);
    if (Template)
    {
        TestTrue(TEXT("template slot 0 holds the requested material"),
            Template->GetMaterial(0) == Probe);
    }
    return true;
}

// ============================================================================
// An unloadable materialPath is MATERIAL_NOT_FOUND and NOTHING is added. The node-count
// assertion is the load-bearing one: the pre-fix handler created, compiled and saved the
// node before it ever looked at the material.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScsAddComponentMaterialNotFoundIsPreflightErrorTest,
    "PinWright.blueprint.scs.add_component.MaterialNotFoundIsPreflightError",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScsAddComponentMaterialNotFoundIsPreflightErrorTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeScsMatStrictAssetPath(TEXT("BP_ScsMatMissing"));
    ON_SCOPE_EXIT { CleanupScsMatStrictAsset(BPPath); };

    UBlueprint* Blueprint = CreateScsMatStrictFixture(BPPath);
    TestNotNull(TEXT("fixture blueprint created"), Blueprint);
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return false;
    }
    const int32 NodesBefore = ScsMatStrictNodeCount(Blueprint);

    TSharedPtr<FJsonObject> Payload =
        MakeScsMatStrictPayload(BPPath, TEXT("StaticMeshComponent"), TEXT("MissingMat"));
    Payload->SetStringField(TEXT("materialPath"),
        TEXT("/Game/PinWrightTests/DoesNotExist/M_NoSuchMaterial"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.scs.add_component handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.scs.add_component"), Payload, Capture));
    TestFalse(TEXT("an unloadable materialPath is not a success"), Capture.bSuccess);
    TestEqual(TEXT("unloadable materialPath yields MATERIAL_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("MATERIAL_NOT_FOUND")));

    TestEqual(TEXT("no SCS node was committed"),
        ScsMatStrictNodeCount(Blueprint), NodesBefore);
    TestNull(TEXT("the requested component does not exist"),
        Blueprint->SimpleConstructionScript->FindSCSNode(FName(TEXT("MissingMat"))));
    return true;
}

// ============================================================================
// A traversal materialPath is SECURITY_VIOLATION, and equally leaves the SCS untouched.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScsAddComponentMaterialTraversalIsSecurityViolationTest,
    "PinWright.blueprint.scs.add_component.MaterialTraversalIsSecurityViolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScsAddComponentMaterialTraversalIsSecurityViolationTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeScsMatStrictAssetPath(TEXT("BP_ScsMatTraversal"));
    ON_SCOPE_EXIT { CleanupScsMatStrictAsset(BPPath); };

    UBlueprint* Blueprint = CreateScsMatStrictFixture(BPPath);
    TestNotNull(TEXT("fixture blueprint created"), Blueprint);
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return false;
    }
    const int32 NodesBefore = ScsMatStrictNodeCount(Blueprint);

    TSharedPtr<FJsonObject> Payload =
        MakeScsMatStrictPayload(BPPath, TEXT("StaticMeshComponent"), TEXT("UnsafeMat"));
    Payload->SetStringField(TEXT("materialPath"),
        TEXT("/Game/../../Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.scs.add_component handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.scs.add_component"), Payload, Capture));
    TestFalse(TEXT("a traversal materialPath is not a success"), Capture.bSuccess);
    TestEqual(TEXT("traversal materialPath yields SECURITY_VIOLATION"),
        Capture.ErrorCode, FString(TEXT("SECURITY_VIOLATION")));

    TestEqual(TEXT("no SCS node was committed"),
        ScsMatStrictNodeCount(Blueprint), NodesBefore);
    TestNull(TEXT("the requested component does not exist"),
        Blueprint->SimpleConstructionScript->FindSCSNode(FName(TEXT("UnsafeMat"))));
    return true;
}

// ============================================================================
// materialPath on a component class that has no material slot is INVALID_PARAMS, not a
// silent material_applied:false. USceneComponent is a valid component (so it clears the
// existing IsChildOf(UActorComponent) gate) but is not a UPrimitiveComponent.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScsAddComponentMaterialOnNonPrimitiveIsInvalidParamsTest,
    "PinWright.blueprint.scs.add_component.MaterialOnNonPrimitiveIsInvalidParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScsAddComponentMaterialOnNonPrimitiveIsInvalidParamsTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeScsMatStrictAssetPath(TEXT("BP_ScsMatNonPrimitive"));
    ON_SCOPE_EXIT { CleanupScsMatStrictAsset(BPPath); };

    UBlueprint* Blueprint = CreateScsMatStrictFixture(BPPath);
    TestNotNull(TEXT("fixture blueprint created"), Blueprint);
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return false;
    }
    const int32 NodesBefore = ScsMatStrictNodeCount(Blueprint);

    TSharedPtr<FJsonObject> Payload =
        MakeScsMatStrictPayload(BPPath, TEXT("SceneComponent"), TEXT("NonPrimitiveMat"));
    Payload->SetStringField(TEXT("materialPath"), ScsMatStrictProbeMaterial);

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.scs.add_component handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.scs.add_component"), Payload, Capture));
    TestFalse(TEXT("materialPath on a non-primitive class is not a success"), Capture.bSuccess);
    TestEqual(TEXT("materialPath on a non-primitive class yields INVALID_PARAMS"),
        Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));

    TestEqual(TEXT("no SCS node was committed"),
        ScsMatStrictNodeCount(Blueprint), NodesBefore);
    TestNull(TEXT("the requested component does not exist"),
        Blueprint->SimpleConstructionScript->FindSCSNode(FName(TEXT("NonPrimitiveMat"))));
    return true;
}

// ============================================================================
// The mesh slot converged on the same contract: an unloadable meshPath is MESH_NOT_FOUND
// and a class that cannot hold a mesh is INVALID_PARAMS, both before any mutation.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScsAddComponentMeshPreflightTest,
    "PinWright.blueprint.scs.add_component.MeshPreflight",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScsAddComponentMeshPreflightTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeScsMatStrictAssetPath(TEXT("BP_ScsMeshPreflight"));
    ON_SCOPE_EXIT { CleanupScsMatStrictAsset(BPPath); };

    UBlueprint* Blueprint = CreateScsMatStrictFixture(BPPath);
    TestNotNull(TEXT("fixture blueprint created"), Blueprint);
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return false;
    }
    const int32 NodesBefore = ScsMatStrictNodeCount(Blueprint);

    {
        TSharedPtr<FJsonObject> Payload =
            MakeScsMatStrictPayload(BPPath, TEXT("StaticMeshComponent"), TEXT("MissingMesh"));
        Payload->SetStringField(TEXT("meshPath"),
            TEXT("/Game/PinWrightTests/DoesNotExist/SM_NoSuchMesh"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.scs.add_component handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.scs.add_component"), Payload, Capture));
        TestFalse(TEXT("an unloadable meshPath is not a success"), Capture.bSuccess);
        TestEqual(TEXT("unloadable meshPath yields MESH_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("MESH_NOT_FOUND")));
    }

    {
        TSharedPtr<FJsonObject> Payload =
            MakeScsMatStrictPayload(BPPath, TEXT("SceneComponent"), TEXT("NonMeshMesh"));
        Payload->SetStringField(TEXT("meshPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.scs.add_component handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.scs.add_component"), Payload, Capture));
        TestFalse(TEXT("meshPath on a non-mesh class is not a success"), Capture.bSuccess);
        TestEqual(TEXT("meshPath on a non-mesh class yields INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    }

    TestEqual(TEXT("neither rejected call committed an SCS node"),
        ScsMatStrictNodeCount(Blueprint), NodesBefore);
    TestNull(TEXT("MissingMesh does not exist"),
        Blueprint->SimpleConstructionScript->FindSCSNode(FName(TEXT("MissingMesh"))));
    TestNull(TEXT("NonMeshMesh does not exist"),
        Blueprint->SimpleConstructionScript->FindSCSNode(FName(TEXT("NonMeshMesh"))));
    return true;
}

// ============================================================================
// BACKWARD COMPAT GUARD. A call that supplies neither materialPath nor meshPath must
// behave exactly as it did before the strict pre-flight: success, both applied flags
// false, and NO warnings key. This is what bounds the breaking change to bad-path calls.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FScsAddComponentNoMaterialIsUnchangedTest,
    "PinWright.blueprint.scs.add_component.NoMaterialIsUnchanged",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FScsAddComponentNoMaterialIsUnchangedTest::RunTest(const FString& Parameters)
{
    const FString BPPath = MakeScsMatStrictAssetPath(TEXT("BP_ScsNoMaterial"));
    ON_SCOPE_EXIT { CleanupScsMatStrictAsset(BPPath); };

    UBlueprint* Blueprint = CreateScsMatStrictFixture(BPPath);
    TestNotNull(TEXT("fixture blueprint created"), Blueprint);
    if (!Blueprint || !Blueprint->SimpleConstructionScript)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload =
        MakeScsMatStrictPayload(BPPath, TEXT("SceneComponent"), TEXT("PlainScene"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.scs.add_component handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.scs.add_component"), Payload, Capture));
    TestTrue(TEXT("add_component without material/mesh still succeeds"), Capture.bSuccess);

    if (Capture.Result.IsValid())
    {
        bool bMaterialApplied = true;
        TestTrue(TEXT("response still carries material_applied"),
            Capture.Result->TryGetBoolField(TEXT("material_applied"), bMaterialApplied));
        TestFalse(TEXT("material_applied is false"), bMaterialApplied);

        bool bMeshApplied = true;
        TestTrue(TEXT("response still carries mesh_applied"),
            Capture.Result->TryGetBoolField(TEXT("mesh_applied"), bMeshApplied));
        TestFalse(TEXT("mesh_applied is false"), bMeshApplied);

        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        TestFalse(TEXT("no warnings key is added when nothing was requested"),
            Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings));
    }

    TestNotNull(TEXT("the component was added"),
        Blueprint->SimpleConstructionScript->FindSCSNode(FName(TEXT("PlainScene"))));
    return true;
}
