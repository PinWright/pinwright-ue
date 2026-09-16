// Copyright (c) 2026 Alexander Penkin. MIT License.

// THE DECISION PROCEDURE behind SpawnMaterialUtils::Apply's detect-and-warn (rather than
// guard-and-refuse) treatment of a construction-script-created mesh component.
//
// The engine's drag-a-material-onto-an-actor path refuses such a component outright
// (AttemptApplyObjToComponent, Editor/UnrealEd/Private/LevelEditorViewport.cpp:876). Copying
// that guard into the spawn verbs would remove the feature rather than fix it:
// UActorComponent::IsCreatedByConstructionScript answers true for SCS-created components as
// well as UserConstructionScript ones (ActorComponent.cpp:994-997), and on a classPath
// Blueprint spawn the mesh component is almost always the SCS one. So Apply only reports the
// condition - `materialOnConstructionScriptComponent` plus a warnings[] entry - and always
// performs the write.
//
// That choice is only correct if the override actually survives a construction-script rerun.
// This file settles it empirically instead of by reading engine source: it spawns a Blueprint
// whose mesh component comes from the SCS, then calls RerunConstructionScripts() - what
// PostEditMove and a Blueprint recompile both funnel into - and reads OverrideMaterials back
// off the live component. If the SURVIVES assertion below ever fails, the decision is wrong
// and the alternative design must be taken: a guard scoped to
// EComponentCreationMethod::UserConstructionScript ONLY (so SCS-created components are still
// served) plus a refusal on that narrow case. That is the correct narrow reading of
// LevelEditorViewport.cpp:876 in any case - the engine's own guard is broader than its stated
// rationale - and the reversal is one edit in SpawnMaterialUtils::Apply.
//
// The rename case is pinned here too. It was written expecting a DROP - instance data is
// matched by name and creation method, so a rename ought to orphan the delta - but measured on
// UE 5.8 the override survives a renaming recompile as well. The warning text was softened to
// match rather than left asserting fiction; see the block around the assertion for detail.
#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "Utils/ActorUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

// Named namespace (not anonymous) so a Unity merge of the Tests tree cannot ODR-clash these
// helpers with a sibling test file's file-local ones.
namespace SpawnMaterialCsTest
{
    // Engine fixtures present on every UE install, same ones the sibling
    // TestActorSpawnMaterialAssignment.cpp leans on.
    const TCHAR* const ProbeMaterialPath =
        TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");
    const TCHAR* const CubeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

    FString MakeFixturePath(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("/Game/__PW_GatewayTests/%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    FString MakeFixtureLabel(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Blueprint fixture carrying ONE SCS StaticMeshComponent with the engine cube, so the
    // spawned instance has a real material slot 0 whose owner reports
    // IsCreatedByConstructionScript() == true. It lives only in memory (never saved) and is
    // registered with the asset registry, because actor.spawn resolves classPath through
    // UEditorAssetLibrary::LoadAsset, which looks the path up in the registry and then calls
    // FAssetData::GetAsset for the in-memory object.
    //
    // The package must NOT be flagged RF_Transient the way the sibling
    // Tests/Blueprint/TestSCSDuplicateComponentHandler.cpp fixture is: that sibling hands the
    // UBlueprint* straight to the handler, while this one goes through LoadAsset, which
    // rejects any object whose outer package is transient - UObject::IsAsset() returns false
    // (Obj.cpp:2789) and LoadAssetFromData reports "'<path>' is not a valid asset."
    // (EditorAssetSubsystem.cpp:107-109). LoadAsset would then return null and actor.spawn's
    // ResolveClassByName fallback cannot resolve a bare /Game package path either, so the
    // spawn failed CLASS_NOT_FOUND. DiscardFixtureBlueprint below applies RF_Transient at
    // teardown instead, so a later editor.save_all still cannot write the fixture to disk.
    UBlueprint* CreateScsMeshBlueprint(const FString& PackagePath, FName ComponentName)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package || AssetName.IsEmpty())
        {
            return nullptr;
        }

        UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            Package,
            FName(*AssetName),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        if (!Blueprint || !Blueprint->SimpleConstructionScript)
        {
            return nullptr;
        }

        USCS_Node* Node = Blueprint->SimpleConstructionScript->CreateNode(
            UStaticMeshComponent::StaticClass(), ComponentName);
        if (!Node)
        {
            return nullptr;
        }
        Blueprint->SimpleConstructionScript->AddNode(Node);

        if (UStaticMeshComponent* Template = Cast<UStaticMeshComponent>(Node->ComponentTemplate))
        {
            if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubeMeshPath))
            {
                Template->SetStaticMesh(Cube);
            }
        }

        // The SCS node only reaches the generated class through a compile; without this the
        // spawned instance has no mesh component at all.
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        FAssetRegistryModule::AssetCreated(Blueprint);
        return Blueprint;
    }

    // Teardown for the never-saved fixture package: mark it transient, unregister, drop the
    // keep-alive flags, and collect. Mirrors the transient branch of CleanupScsDuplicateAsset -
    // a plain DeleteAsset would try to resolve an on-disk source that never existed. RF_Transient
    // is applied HERE rather than at creation (see CreateScsMeshBlueprint) so the package is a
    // valid asset while actor.spawn resolves it, yet still unsaveable from the moment the test
    // is done with it.
    void DiscardFixtureBlueprint(const FString& PackagePath)
    {
        UPackage* Package = FindPackage(nullptr, *PackagePath);
        if (!Package)
        {
            return;
        }
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        UObject* Asset = FindObject<UObject>(Package, *AssetName);

        Package->SetFlags(RF_Transient);
        FAssetRegistryModule::PackageDeleted(Package);
        Package->ClearFlags(RF_Standalone | RF_Public);
        Package->SetDirtyFlag(false);
        if (Asset)
        {
            Asset->ClearFlags(RF_Standalone | RF_Public);
            Asset->RemoveFromRoot();
            Asset->MarkAsGarbage();
        }
        Package->MarkAsGarbage();
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }

    UMeshComponent* FindMeshComponent(AActor* Actor)
    {
        if (!Actor)
        {
            return nullptr;
        }
        if (UMeshComponent* RootMesh = Cast<UMeshComponent>(Actor->GetRootComponent()))
        {
            return RootMesh;
        }
        return Actor->FindComponentByClass<UMeshComponent>();
    }

    // True when any warnings[] entry contains Substring. The warning text is user-facing and
    // documented, so the assertions below probe its substance rather than reusing the
    // implementation's own literal.
    bool WarningsMention(const TSharedPtr<FJsonObject>& Result, const TCHAR* Substring)
    {
        const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("warnings"), Warnings) || !Warnings)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Warnings)
        {
            FString Text;
            if (Value.IsValid() && Value->TryGetString(Text) && Text.Contains(Substring))
            {
                return true;
            }
        }
        return false;
    }
}

// ============================================================================
// actor.spawn {classPath: <BP>, materialPath}: the write lands on the SCS-created component,
// is reported (not refused), and SURVIVES RerunConstructionScripts().
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnMaterialSurvivesConstructionScriptTest,
    "PinWright.actor.spawn.MaterialSurvivesConstructionScriptRerun",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnMaterialSurvivesConstructionScriptTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping construction-script material test."));
        return true;
    }
    UMaterialInterface* Probe =
        LoadObject<UMaterialInterface>(nullptr, SpawnMaterialCsTest::ProbeMaterialPath);
    if (!Probe)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("WorldGridMaterial unavailable; skipping construction-script material test."));
        return true;
    }

    // The fixture Blueprint is created, compiled and recompiled-with-a-rename in memory, so the
    // Kismet compiler and the editor asset subsystem can log noise at Error around an asset that
    // has no on-disk source. Every verdict below is asserted explicitly off the response and off
    // the live component, never off the log, so suppression cannot hide a real verdict.
    bSuppressLogErrors = true;

    const FString BPPath = SpawnMaterialCsTest::MakeFixturePath(TEXT("BP_SpawnMatCS"));
    const FName OriginalComponentName(TEXT("PWFixtureMesh"));
    const FName RenamedComponentName(TEXT("PWFixtureMeshRenamed"));

    // Declaration order matters: ON_SCOPE_EXIT runs LAST, the world guard's destructor FIRST,
    // so the spawned instance is destroyed before its Blueprint class is discarded.
    ON_SCOPE_EXIT { SpawnMaterialCsTest::DiscardFixtureBlueprint(BPPath); };
    FScopedEditorWorldActorGuard WorldGuard;

    UBlueprint* Blueprint = SpawnMaterialCsTest::CreateScsMeshBlueprint(BPPath, OriginalComponentName);
    TestNotNull(TEXT("fixture Blueprint with an SCS StaticMeshComponent created"), Blueprint);
    if (!Blueprint || !Blueprint->GeneratedClass)
    {
        return false;
    }

    const FString Label = SpawnMaterialCsTest::MakeFixtureLabel(TEXT("PW_SpawnMatCS"));
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("classPath"), BPPath);
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetStringField(TEXT("materialPath"), SpawnMaterialCsTest::ProbeMaterialPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn"), Payload, Capture));
    TestTrue(TEXT("actor.spawn of a Blueprint with materialPath succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    // NO GUARD, NO REFUSAL: the write went through on a construction-script component. An
    // IsCreatedByConstructionScript()-based refusal would fail exactly here, on the ordinary
    // classPath Blueprint spawn that is the feature's main use.
    bool bApplied = false;
    Capture.Result->TryGetBoolField(TEXT("material_applied"), bApplied);
    TestTrue(TEXT("material_applied is true on a construction-script component"), bApplied);

    bool bOnConstructionScript = false;
    TestTrue(TEXT("response carries materialOnConstructionScriptComponent"),
        Capture.Result->TryGetBoolField(TEXT("materialOnConstructionScriptComponent"),
            bOnConstructionScript));
    TestTrue(TEXT("materialOnConstructionScriptComponent is true for an SCS component"),
        bOnConstructionScript);

    TestTrue(TEXT("the warning names the construction script"),
        SpawnMaterialCsTest::WarningsMention(Capture.Result, TEXT("construction script")));
    TestTrue(TEXT("the warning steers to the durable blueprint.scs.add_component form"),
        SpawnMaterialCsTest::WarningsMention(Capture.Result, TEXT("blueprint.scs.add_component")));

    AActor* Spawned = McpActorUtils::FindActorByName(nullptr, Label);
    TestNotNull(TEXT("spawned Blueprint instance found by label"), Spawned);
    UMeshComponent* MeshComp = SpawnMaterialCsTest::FindMeshComponent(Spawned);
    TestNotNull(TEXT("spawned instance has a mesh component"), MeshComp);
    if (!Spawned || !MeshComp)
    {
        return false;
    }

    // The premise of the whole section: the receiving component really is construction-script
    // created, so this test is exercising the case the engine's drag-drop guard would refuse.
    TestTrue(TEXT("the receiving component is construction-script created"),
        MeshComp->IsCreatedByConstructionScript());
    TestTrue(TEXT("slot 0 holds the requested material before the rerun"),
        MeshComp->GetMaterial(0) == Probe);

    // ---- THE DECISION ------------------------------------------------------
    // RerunConstructionScripts destroys and rebuilds every construction-script component, then
    // re-applies an FComponentInstanceDataCache (ActorConstruction.cpp:927-929 after the SCS,
    // :960-962 after the user CS). OverrideMaterials is expected to ride through because it is
    // CPF_Edit and non-transient, so ComponentInstanceDataCache.cpp:53-66 does not skip it.
    Spawned->RerunConstructionScripts();

    UMeshComponent* AfterRerun = SpawnMaterialCsTest::FindMeshComponent(Spawned);
    TestNotNull(TEXT("mesh component still present after RerunConstructionScripts"), AfterRerun);
    if (AfterRerun)
    {
        // IF THIS FAILS, the detect-and-warn design is wrong: the override does NOT survive,
        // and SpawnMaterialUtils::Apply must instead refuse the write when
        // MeshComp->CreationMethod == EComponentCreationMethod::UserConstructionScript (that
        // narrow case only - SCS-created components must keep working), returning a typed
        // error, and the `materialOnConstructionScriptComponent` reporting becomes that
        // refusal's signal. Do not "fix" this by deleting the assertion.
        TestTrue(TEXT("DECISION: the spawn-time material override survives RerunConstructionScripts"),
            AfterRerun->GetMaterial(0) == Probe);
    }

    // ---- THE FAILURE MODE THE WARNING USED TO CLAIM ------------------------
    // This block was written expecting the override to be DROPPED here, on the reasoning that
    // component instance data is matched by name + creation method, so a rename should orphan
    // the cached delta. Measured on UE 5.8, that is NOT what happens: the override survives a
    // recompile that renames the component (the rename itself is confirmed by the guard below,
    // so this is not a no-op rename reading as survival).
    //
    // Per this file's own instruction - "if it fails, the warning overstates the risk and must
    // be softened rather than left as fiction" - the user-facing warning in
    // SpawnMaterialUtils::MakeConstructionScriptWarning was softened to match, and the
    // assertion below now pins the measured behaviour instead of the assumed behaviour.
    // It is still a real assertion: if a future engine version starts dropping the override on
    // rename, this fails and the warning must be restored to its stronger form.
    USCS_Node* Node = Blueprint->SimpleConstructionScript
        ? Blueprint->SimpleConstructionScript->FindSCSNode(OriginalComponentName)
        : nullptr;
    TestNotNull(TEXT("fixture SCS node found for rename"), Node);
    if (!Node)
    {
        return false;
    }
    FBlueprintEditorUtils::RenameComponentMemberVariable(Blueprint, Node, RenamedComponentName);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    // A recompile reinstances every placed instance, so the pre-compile AActor* is stale -
    // the surviving instance must be re-found by label.
    AActor* Reinstanced = McpActorUtils::FindActorByName(nullptr, Label);
    TestNotNull(TEXT("instance found again after the renaming recompile"), Reinstanced);
    if (!Reinstanced)
    {
        return false;
    }
    Reinstanced->RerunConstructionScripts();

    UMeshComponent* RenamedComp = SpawnMaterialCsTest::FindMeshComponent(Reinstanced);
    TestNotNull(TEXT("renamed mesh component present after the recompile"), RenamedComp);
    if (RenamedComp)
    {
        // Guard the verdict on the rename having actually happened; otherwise a no-op rename
        // would read as "the override survived" and silently invert the meaning below.
        TestTrue(TEXT("the component was really renamed by the recompile"),
            RenamedComp->GetName().Contains(RenamedComponentName.ToString()));
        TestTrue(TEXT("the per-instance override also survives a recompile that renames the component"),
            RenamedComp->GetMaterial(0) == Probe);
    }
    return true;
}

// ============================================================================
// Contract: the new field is ADDITIVE. actor.spawn_shape spawns an AStaticMeshActor whose mesh
// component is native (CreationMethod == Native), so nothing construction-script related may
// appear in its response - existing callers must see byte-identical JSON.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSpawnShapeOmitsConstructionScriptFieldTest,
    "PinWright.actor.spawn_shape.OmitsConstructionScriptMaterialField",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSpawnShapeOmitsConstructionScriptFieldTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping spawn_shape additive-field test."));
        return true;
    }
    if (!LoadObject<UMaterialInterface>(nullptr, SpawnMaterialCsTest::ProbeMaterialPath))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-missing"),
            TEXT("WorldGridMaterial unavailable; skipping spawn_shape additive-field test."));
        return true;
    }

    FScopedEditorWorldActorGuard WorldGuard;
    const FString Label = SpawnMaterialCsTest::MakeFixtureLabel(TEXT("PW_ShapeMatNoCS"));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("shape"), TEXT("CUBE"));
    Payload->SetStringField(TEXT("actorName"), Label);
    Payload->SetStringField(TEXT("materialPath"), SpawnMaterialCsTest::ProbeMaterialPath);

    FTestResponseCapture Capture;
    TestTrue(TEXT("actor.spawn_shape is registered"),
        InvokeHandlerWithCapture(TEXT("actor.spawn_shape"), Payload, Capture));
    TestTrue(TEXT("actor.spawn_shape with materialPath succeeds"), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bOnConstructionScript = false;
    TestFalse(TEXT("no materialOnConstructionScriptComponent field on a native mesh component"),
        Capture.Result->TryGetBoolField(TEXT("materialOnConstructionScriptComponent"),
            bOnConstructionScript));

    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    TestFalse(TEXT("a clean native-component apply carries no warnings"),
        Capture.Result->TryGetArrayField(TEXT("warnings"), Warnings));
    return true;
}
