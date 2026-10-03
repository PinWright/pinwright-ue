// Copyright (c) 2026 Alexander Penkin. MIT License.

// Per-section / per-slot boundingBox on the shared StaticMesh builder (static_mesh.describe and
// static_mesh.json) and the opt-in static_mesh.describe islands[] readout.
//
// Fixture (synthetic, mesh-local units), three material slots:
//   - SlotB: a 2x2 quad at x 10..12, y 0..2, z 0 (two triangles).
//   - SlotA: one triangle touching the quad's right edge (x 12..14, y 0..2, z 0), and one lone
//            triangle far away at x -5..-4, y -5..-4, z 1.
//   - SlotC: no triangles.
// Every triangle corner gets its own UV, so the build splits every render vertex: connectivity
// through shared render indices would see four islands; welding by position must see two.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Handlers/Asset/StaticMeshDumpBuilder.h"
#include "Handlers/Asset/StaticMeshTextEmitter.h"
#include "MeshDescription.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace TestStaticMeshSpatialExtentHelpers
{
    UStaticMesh* CreateFixture(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        UStaticMesh* Mesh = NewObject<UStaticMesh>(Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)), RF_Public | RF_Standalone);
        if (!Mesh)
        {
            return nullptr;
        }
        Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, FName(TEXT("SlotA"))));
        Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, FName(TEXT("SlotB"))));
        Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, FName(TEXT("SlotC"))));

        FMeshDescription MeshDescription;
        FStaticMeshAttributes Attributes(MeshDescription);
        Attributes.Register();
        TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
        TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
        TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
        TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
        TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
        UVs.SetNumChannels(1);

        const FPolygonGroupID GroupA = MeshDescription.CreatePolygonGroup();
        const FPolygonGroupID GroupB = MeshDescription.CreatePolygonGroup();
        TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
        SlotNames[GroupA] = FName(TEXT("SlotA"));
        SlotNames[GroupB] = FName(TEXT("SlotB"));

        float NextU = 0.0f;
        const auto AddTriangle = [&](FPolygonGroupID Group, const FVector3f& P0, const FVector3f& P1, const FVector3f& P2)
        {
            const FVector3f Corners[3] = { P0, P1, P2 };
            FVertexInstanceID Instances[3];
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                const FVertexID Vertex = MeshDescription.CreateVertex();
                Positions[Vertex] = Corners[Corner];
                Instances[Corner] = MeshDescription.CreateVertexInstance(Vertex);
                Normals[Instances[Corner]] = FVector3f(0.0f, 0.0f, 1.0f);
                Tangents[Instances[Corner]] = FVector3f(1.0f, 0.0f, 0.0f);
                BinormalSigns[Instances[Corner]] = 1.0f;
                UVs.Set(Instances[Corner], 0, FVector2f(NextU, 0.0f));
                NextU += 0.05f;
            }
            MeshDescription.CreateTriangle(Group, Instances);
        };

        AddTriangle(GroupB, FVector3f(10, 0, 0), FVector3f(12, 0, 0), FVector3f(12, 2, 0));
        AddTriangle(GroupB, FVector3f(10, 0, 0), FVector3f(12, 2, 0), FVector3f(10, 2, 0));
        AddTriangle(GroupA, FVector3f(12, 0, 0), FVector3f(14, 0, 0), FVector3f(12, 2, 0));
        AddTriangle(GroupA, FVector3f(-5, -5, 1), FVector3f(-4, -5, 1), FVector3f(-5, -4, 1));

        UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
        BuildParams.bFastBuild = true;
        if (!Mesh->BuildFromMeshDescriptions({ &MeshDescription }, BuildParams))
        {
            return nullptr;
        }
        return Mesh;
    }

    TSharedPtr<FJsonObject> FindRow(const TArray<TSharedPtr<FJsonValue>>* Rows, const TCHAR* Field, int32 Value,
        int32 LodIndex = INDEX_NONE)
    {
        if (!Rows)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Row : *Rows)
        {
            const TSharedPtr<FJsonObject> Object = Row.IsValid() && Row->Type == EJson::Object ? Row->AsObject() : nullptr;
            double Number = -1.0;
            double Lod = -1.0;
            if (Object.IsValid() && Object->TryGetNumberField(Field, Number) && static_cast<int32>(Number) == Value
                && (LodIndex == INDEX_NONE
                    || (Object->TryGetNumberField(TEXT("lodIndex"), Lod) && static_cast<int32>(Lod) == LodIndex)))
            {
                return Object;
            }
        }
        return nullptr;
    }

    FVector ReadVector(const TSharedPtr<FJsonObject>& Box, const TCHAR* Field)
    {
        const TSharedPtr<FJsonObject>* V = nullptr;
        if (!Box.IsValid() || !Box->TryGetObjectField(Field, V))
        {
            return FVector(TNumericLimits<double>::Max());
        }
        return FVector((*V)->GetNumberField(TEXT("x")), (*V)->GetNumberField(TEXT("y")), (*V)->GetNumberField(TEXT("z")));
    }

    void ExpectBox(FAutomationTestBase& Test, const FString& What, const TSharedPtr<FJsonObject>& Row,
        const FVector& ExpectedMin, const FVector& ExpectedMax)
    {
        const TSharedPtr<FJsonObject>* Box = nullptr;
        if (!Test.TestTrue(What + TEXT(": boundingBox is an object"),
            Row.IsValid() && Row->TryGetObjectField(TEXT("boundingBox"), Box)))
        {
            return;
        }
        Test.TestTrue(What + TEXT(": min"), ReadVector(*Box, TEXT("min")).Equals(ExpectedMin, 1e-4));
        Test.TestTrue(What + TEXT(": max"), ReadVector(*Box, TEXT("max")).Equals(ExpectedMax, 1e-4));
        Test.TestTrue(What + TEXT(": size"), ReadVector(*Box, TEXT("size")).Equals(ExpectedMax - ExpectedMin, 1e-4));
        Test.TestTrue(What + TEXT(": center"),
            ReadVector(*Box, TEXT("center")).Equals((ExpectedMax + ExpectedMin) * 0.5, 1e-4));
    }

    // Fixture precondition shared by both tests: two LOD0 sections and no shared render vertex.
    bool CheckFixture(FAutomationTestBase& Test, const UStaticMesh* Mesh)
    {
        const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
        if (!Test.TestNotNull(TEXT("Fixture builds with render data"), RenderData)
            || !Test.TestTrue(TEXT("Fixture has LOD0"), RenderData->LODResources.Num() > 0))
        {
            return false;
        }
        const FStaticMeshLODResources& Lod0 = RenderData->LODResources[0];
        return Test.TestEqual(TEXT("Fixture LOD0 has two sections"), Lod0.Sections.Num(), 2)
            && Test.TestEqual(TEXT("Fixture LOD0 has four triangles"), Lod0.GetNumTriangles(), 4)
            && Test.TestEqual(TEXT("Fixture splits every render vertex (no shared index), so only a position weld connects"),
                static_cast<int32>(Lod0.VertexBuffers.PositionVertexBuffer.GetNumVertices()), 12);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshSpatialExtentSlotAndSectionBoxesTest,
    "PinWright.AssetDump.StaticMeshSpatialExtent.SlotAndSectionBoxes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticMeshSpatialExtentSlotAndSectionBoxesTest::RunTest(const FString& Parameters)
{
    using namespace TestStaticMeshSpatialExtentHelpers;
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SM_SpatialExtent_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TStrongObjectPtr<UStaticMesh> Mesh(CreateFixture(PackagePath));
    ON_SCOPE_EXIT
    {
        Mesh.Reset();
        CleanupTestAsset(PackagePath);
    };
    if (!CheckFixture(*this, Mesh.Get()))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Result = StaticMeshDumpBuilder::BuildStaticMeshJson(Mesh.Get());
    if (!TestTrue(TEXT("JSON is built"), Result.IsValid()))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* SlotUsage = nullptr;
    TestTrue(TEXT("slotUsage is an array"), Result->TryGetArrayField(TEXT("slotUsage"), SlotUsage));
    // SlotA spans the lone triangle and the quad-adjacent triangle; the whole-mesh bounds spans
    // the quad too, so this box is the per-slot reach the whole-mesh bounds cannot show.
    ExpectBox(*this, TEXT("SlotA"), FindRow(SlotUsage, TEXT("materialIndex"), 0),
        FVector(-5, -5, 0), FVector(14, 2, 1));
    ExpectBox(*this, TEXT("SlotB"), FindRow(SlotUsage, TEXT("materialIndex"), 1),
        FVector(10, 0, 0), FVector(12, 2, 0));
    const TSharedPtr<FJsonObject> SlotC = FindRow(SlotUsage, TEXT("materialIndex"), 2);
    if (TestTrue(TEXT("slotUsage has the empty SlotC row"), SlotC.IsValid()))
    {
        const TSharedPtr<FJsonValue> Box = SlotC->TryGetField(TEXT("boundingBox"));
        TestTrue(TEXT("SlotC boundingBox is present and null (no triangles)"),
            Box.IsValid() && Box->Type == EJson::Null);
    }

    const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
    TestTrue(TEXT("sections is an array"), Result->TryGetArrayField(TEXT("sections"), Sections));
    ExpectBox(*this, TEXT("SlotA section"), FindRow(Sections, TEXT("materialIndex"), 0, 0),
        FVector(-5, -5, 0), FVector(14, 2, 1));
    ExpectBox(*this, TEXT("SlotB section"), FindRow(Sections, TEXT("materialIndex"), 1, 0),
        FVector(10, 0, 0), FVector(12, 2, 0));

    const FString Text = StaticMeshTextEmitter::BuildText(Result);
    TestTrue(TEXT("text sidecar carries SlotA box"),
        Text.Contains(TEXT("boundingBox: min (x=-5, y=-5, z=0) max (x=14, y=2, z=1)")));
    TestTrue(TEXT("text sidecar carries SlotB box"),
        Text.Contains(TEXT("boundingBox: min (x=10, y=0, z=0) max (x=12, y=2, z=0)")));
    TestTrue(TEXT("text sidecar marks the empty slot's box None"), Text.Contains(TEXT("boundingBox: None")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshDescribeIslandsTest,
    "PinWright.static_mesh.describe.IslandsWeldByPosition",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticMeshDescribeIslandsTest::RunTest(const FString& Parameters)
{
    using namespace TestStaticMeshSpatialExtentHelpers;
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SM_Islands_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TStrongObjectPtr<UStaticMesh> Mesh(CreateFixture(PackagePath));
    ON_SCOPE_EXIT
    {
        Mesh.Reset();
        CleanupTestAsset(PackagePath);
    };
    if (!CheckFixture(*this, Mesh.Get()))
    {
        return false;
    }

    // Default: no islands (opt-in, not part of the dump shape).
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), Mesh->GetPathName());
        FTestResponseCapture Capture;
        TestTrue(TEXT("describe handler found"), InvokeHandlerWithCapture(TEXT("static_mesh.describe"), Payload, Capture));
        if (!TestTrue(TEXT("default describe succeeds"), Capture.bSuccess && Capture.Result.IsValid()))
        {
            return false;
        }
        TestFalse(TEXT("islands absent unless requested"), Capture.Result->HasField(TEXT("islands")));
        TestFalse(TEXT("islandCount absent unless requested"), Capture.Result->HasField(TEXT("islandCount")));
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), Mesh->GetPathName());
    Payload->SetBoolField(TEXT("includeIslands"), true);
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(TEXT("static_mesh.describe"), Payload, Capture);
    if (!TestTrue(FString::Printf(TEXT("includeIslands describe succeeds (errorCode='%s')"), *Capture.ErrorCode),
        Capture.bSuccess && Capture.Result.IsValid()))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Islands = nullptr;
    if (!TestTrue(TEXT("islands is an array"), Capture.Result->TryGetArrayField(TEXT("islands"), Islands)))
    {
        return false;
    }
    TestEqual(TEXT("islandCount: quad + touching triangle weld into one island, lone triangle is the other"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("islandCount"))), 2);
    TestFalse(TEXT("default maxIslands does not truncate two islands"),
        Capture.Result->GetBoolField(TEXT("islandsTruncated")));
    if (!TestEqual(TEXT("islands has two rows"), Islands->Num(), 2))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Big = FindRow(Islands, TEXT("index"), 0);
    const TSharedPtr<FJsonObject> Lone = FindRow(Islands, TEXT("index"), 1);
    if (!TestTrue(TEXT("island rows 0 and 1 exist"), Big.IsValid() && Lone.IsValid()))
    {
        return false;
    }
    TestEqual(TEXT("island 0 (largest first) has three triangles"),
        static_cast<int32>(Big->GetNumberField(TEXT("triangleCount"))), 3);
    TestEqual(TEXT("island 1 has one triangle"), static_cast<int32>(Lone->GetNumberField(TEXT("triangleCount"))), 1);
    ExpectBox(*this, TEXT("island 0"), Big, FVector(10, 0, 0), FVector(14, 2, 0));
    ExpectBox(*this, TEXT("island 1"), Lone, FVector(-5, -5, 1), FVector(-4, -4, 1));

    const TArray<TSharedPtr<FJsonValue>>* BigSlots = nullptr;
    if (TestTrue(TEXT("island 0 materialSlots is an array"), Big->TryGetArrayField(TEXT("materialSlots"), BigSlots))
        && TestEqual(TEXT("island 0 spans two slots"), BigSlots->Num(), 2))
    {
        TestEqual(TEXT("island 0 slot 0 is SlotA"), (*BigSlots)[0]->AsObject()->GetStringField(TEXT("materialSlotName")), FString(TEXT("SlotA")));
        TestEqual(TEXT("island 0 slot 1 is SlotB"), (*BigSlots)[1]->AsObject()->GetStringField(TEXT("materialSlotName")), FString(TEXT("SlotB")));
    }
    const TArray<TSharedPtr<FJsonValue>>* LoneSlots = nullptr;
    if (TestTrue(TEXT("island 1 materialSlots is an array"), Lone->TryGetArrayField(TEXT("materialSlots"), LoneSlots))
        && TestEqual(TEXT("island 1 spans one slot"), LoneSlots->Num(), 1))
    {
        TestEqual(TEXT("island 1 slot is SlotA"),
            static_cast<int32>((*LoneSlots)[0]->AsObject()->GetNumberField(TEXT("materialIndex"))), 0);
    }

    // maxIslands caps the rows, not the count: one row (the largest island), count still 2.
    Payload->SetNumberField(TEXT("maxIslands"), 1);
    FTestResponseCapture Capped;
    InvokeHandlerWithCapture(TEXT("static_mesh.describe"), Payload, Capped);
    if (!TestTrue(TEXT("maxIslands:1 describe succeeds"), Capped.bSuccess && Capped.Result.IsValid()))
    {
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* CappedIslands = nullptr;
    if (TestTrue(TEXT("capped islands is an array"), Capped.Result->TryGetArrayField(TEXT("islands"), CappedIslands)))
    {
        TestEqual(TEXT("maxIslands:1 serializes one row"), CappedIslands->Num(), 1);
        if (CappedIslands->Num() == 1)
        {
            TestEqual(TEXT("the kept row is the largest island"),
                static_cast<int32>((*CappedIslands)[0]->AsObject()->GetNumberField(TEXT("triangleCount"))), 3);
        }
    }
    TestEqual(TEXT("islandCount stays exact under the cap"),
        static_cast<int32>(Capped.Result->GetNumberField(TEXT("islandCount"))), 2);
    TestTrue(TEXT("islandsTruncated reports the cut"), Capped.Result->GetBoolField(TEXT("islandsTruncated")));
    TestEqual(TEXT("maxIslands is echoed as applied"),
        static_cast<int32>(Capped.Result->GetNumberField(TEXT("maxIslands"))), 1);
    return true;
}
