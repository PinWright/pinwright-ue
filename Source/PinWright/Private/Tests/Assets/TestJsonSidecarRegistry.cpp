// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-asset-dump-registry-driven-dispatch.
//
// The migration moved the per-type JSON-builder branches out of
// BuildAllFilesForAsset's inline if/else chain into JsonSidecarRegistry.
// The registry is now the single source of truth for which asset class
// emits which JSON sidecar (plus optional text twin and null diagnostic).
// This test asserts the migrated registrations are present and that a
// registered build function produces a valid sidecar on a transient asset.
//
// Counterfactual: if a REGISTER_DUMP_JSON_SIDECAR(...) line is dropped
// from a builder .cpp (e.g. StaticMeshDumpBuilder.cpp), the corresponding
// sidecar silently vanishes from every dump — this test catches that
// without a full asset.dump_folder integration run.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/JsonSidecarRegistry.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/Package.h"

namespace
{
    const JsonSidecarRegistry::FJsonSidecarSpec* FindSpecByFileName(
        const TArray<JsonSidecarRegistry::FJsonSidecarSpec>& Specs, const TCHAR* FileName)
    {
        for (const JsonSidecarRegistry::FJsonSidecarSpec& Spec : Specs)
        {
            if (Spec.FileName && FCString::Strcmp(Spec.FileName, FileName) == 0)
            {
                return &Spec;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FJsonSidecar_RegisteredAndDispatched,
    "PinWright.asset.dump.JsonSidecarRegistry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FJsonSidecar_RegisteredAndDispatched::RunTest(const FString& Parameters)
{
    const TArray<JsonSidecarRegistry::FJsonSidecarSpec> Specs = JsonSidecarRegistry::GetRegisteredJsonSidecars();

    // StaticMesh: JSON sidecar paired with a text twin, no null diagnostic.
    const JsonSidecarRegistry::FJsonSidecarSpec* StaticMeshSpec =
        FindSpecByFileName(Specs, DumpFileNames::StaticMesh);
    TestNotNull(TEXT("Registry contains a static_mesh.json sidecar entry"), StaticMeshSpec);
    if (!StaticMeshSpec)
    {
        return false;
    }
    TestTrue(TEXT("static_mesh spec discriminates on UStaticMesh"),
        StaticMeshSpec->ClassFn && StaticMeshSpec->ClassFn() == UStaticMesh::StaticClass());
    TestTrue(TEXT("static_mesh spec carries a text-emitter twin"),
        StaticMeshSpec->TextEmitterFileName != nullptr && StaticMeshSpec->TextEmitterFn != nullptr);

    // MaterialInstance: registry candidate that emits a null diagnostic when the
    // builder returns null (preserves the inline branch's diagnostic message).
    const JsonSidecarRegistry::FJsonSidecarSpec* MICSpec =
        FindSpecByFileName(Specs, DumpFileNames::MaterialInstance);
    TestNotNull(TEXT("Registry contains a material_instance.json sidecar entry"), MICSpec);
    if (MICSpec)
    {
        TestTrue(TEXT("material_instance spec discriminates on UMaterialInstanceConstant"),
            MICSpec->ClassFn && MICSpec->ClassFn() == UMaterialInstanceConstant::StaticClass());
        TestNotNull(TEXT("material_instance spec carries a null diagnostic"), MICSpec->NullDiagnostic);
    }

    // Invoke the registered build function directly to confirm it produces a valid
    // JSON object without the dispatch orchestrator. Assert the pointer first: a bare
    // `if (BuildFn)` guard would silently skip the whole block — and the test would
    // still pass — for a spec registered with a null builder, which is exactly the
    // "sidecar silently vanishes from every dump" failure this file exists to catch.
    TestTrue(TEXT("static_mesh spec carries a build function"),
        StaticMeshSpec->BuildFn != nullptr);
    if (StaticMeshSpec->BuildFn)
    {
        UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage());
        TestNotNull(TEXT("Transient UStaticMesh created"), Mesh);
        if (Mesh)
        {
            const TSharedPtr<FJsonObject> Json = StaticMeshSpec->BuildFn(Mesh);
            TestTrue(TEXT("static_mesh build function produced a valid JSON object"), Json.IsValid());
        }
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
