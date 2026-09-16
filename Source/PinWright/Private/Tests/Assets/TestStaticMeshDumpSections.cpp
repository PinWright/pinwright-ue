// Copyright (c) 2026 Alexander Penkin. MIT License.

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

namespace
{
    TSharedPtr<FJsonObject> StaticMeshSectionsTest_FindObject(
        const TArray<TSharedPtr<FJsonValue>>* Values,
        const TCHAR* IndexField,
        int32 ExpectedIndex,
        const TCHAR* SecondIndexField = nullptr,
        int32 ExpectedSecondIndex = INDEX_NONE)
    {
        if (!Values)
        {
            return nullptr;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const TSharedPtr<FJsonObject> Object = Value.IsValid() && Value->Type == EJson::Object
                ? Value->AsObject() : nullptr;
            double Index = -1.0;
            double SecondIndex = -1.0;
            if (Object.IsValid()
                && Object->TryGetNumberField(IndexField, Index)
                && static_cast<int32>(Index) == ExpectedIndex
                && (!SecondIndexField
                    || (Object->TryGetNumberField(SecondIndexField, SecondIndex)
                        && static_cast<int32>(SecondIndex) == ExpectedSecondIndex)))
            {
                return Object;
            }
        }
        return nullptr;
    }

    void StaticMeshSectionsTest_AssertSection(
        FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Json,
        const FStaticMeshSection& Expected)
    {
        double Number = -1.0;
        Test.TestTrue(TEXT("section.materialIndex is a number"),
            Json->TryGetNumberField(TEXT("materialIndex"), Number));
        Test.TestEqual(TEXT("section.materialIndex matches render data"),
            static_cast<int32>(Number), Expected.MaterialIndex);
        Test.TestTrue(TEXT("section.firstIndex is a number"),
            Json->TryGetNumberField(TEXT("firstIndex"), Number));
        Test.TestEqual(TEXT("section.firstIndex matches render data"),
            static_cast<uint32>(Number), Expected.FirstIndex);
        Test.TestTrue(TEXT("section.numTriangles is a number"),
            Json->TryGetNumberField(TEXT("numTriangles"), Number));
        Test.TestEqual(TEXT("section.numTriangles matches render data"),
            static_cast<uint32>(Number), Expected.NumTriangles);
        Test.TestTrue(TEXT("section.minVertexIndex is a number"),
            Json->TryGetNumberField(TEXT("minVertexIndex"), Number));
        Test.TestEqual(TEXT("section.minVertexIndex matches render data"),
            static_cast<uint32>(Number), Expected.MinVertexIndex);
        Test.TestTrue(TEXT("section.maxVertexIndex is a number"),
            Json->TryGetNumberField(TEXT("maxVertexIndex"), Number));
        Test.TestEqual(TEXT("section.maxVertexIndex matches render data"),
            static_cast<uint32>(Number), Expected.MaxVertexIndex);

        bool bValue = false;
        Test.TestTrue(TEXT("section.bEnableCollision is a boolean"),
            Json->TryGetBoolField(TEXT("bEnableCollision"), bValue));
        Test.TestEqual(TEXT("section.bEnableCollision matches render data"),
            bValue, Expected.bEnableCollision);
        Test.TestTrue(TEXT("section.bCastShadow is a boolean"),
            Json->TryGetBoolField(TEXT("bCastShadow"), bValue));
        Test.TestEqual(TEXT("section.bCastShadow matches render data"),
            bValue, Expected.bCastShadow);
    }

    UStaticMesh* StaticMeshSectionsTest_CreateMultiSectionFixture(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        UStaticMesh* Mesh = NewObject<UStaticMesh>(
            Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            RF_Public | RF_Standalone);
        if (!Mesh)
        {
            return nullptr;
        }

        Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, FName(TEXT("SlotA"))));
        Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, FName(TEXT("SlotB"))));

        FMeshDescription MeshDescription;
        FStaticMeshAttributes Attributes(MeshDescription);
        Attributes.Register();
        TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
        TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
        TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
        TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
        TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
        UVs.SetNumChannels(2);

        const FPolygonGroupID GroupA = MeshDescription.CreatePolygonGroup();
        const FPolygonGroupID GroupB = MeshDescription.CreatePolygonGroup();
        TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
        SlotNames[GroupA] = FName(TEXT("SlotA"));
        SlotNames[GroupB] = FName(TEXT("SlotB"));

        const auto AddTriangle = [&](FPolygonGroupID Group, float X)
        {
            FVertexID Vertices[3];
            FVertexInstanceID Instances[3];
            for (int32 Corner = 0; Corner < 3; ++Corner)
            {
                Vertices[Corner] = MeshDescription.CreateVertex();
                Instances[Corner] = MeshDescription.CreateVertexInstance(Vertices[Corner]);
                Normals[Instances[Corner]] = FVector3f(0.0f, 0.0f, 1.0f);
                Tangents[Instances[Corner]] = FVector3f(1.0f, 0.0f, 0.0f);
                BinormalSigns[Instances[Corner]] = 1.0f;
            }
            Positions[Vertices[0]] = FVector3f(X, 0.0f, 0.0f);
            Positions[Vertices[1]] = FVector3f(X + 1.0f, 0.0f, 0.0f);
            Positions[Vertices[2]] = FVector3f(X, 1.0f, 0.0f);
            UVs.Set(Instances[0], 0, FVector2f(0.0f, 0.0f));
            UVs.Set(Instances[1], 0, FVector2f(1.0f, 0.0f));
            UVs.Set(Instances[2], 0, FVector2f(0.0f, 1.0f));
            UVs.Set(Instances[0], 1, FVector2f(0.0f, 0.0f));
            UVs.Set(Instances[1], 1, FVector2f(0.5f, 0.0f));
            UVs.Set(Instances[2], 1, FVector2f(0.0f, 0.5f));
            MeshDescription.CreateTriangle(Group, Instances);
        };

        AddTriangle(GroupA, 0.0f);
        AddTriangle(GroupB, 2.0f);
        AddTriangle(GroupB, 4.0f);

        UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
        BuildParams.bFastBuild = true;
        if (!Mesh->BuildFromMeshDescriptions({ &MeshDescription }, BuildParams))
        {
            return nullptr;
        }
        Mesh->SetLightMapCoordinateIndex(1);
        return Mesh;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshDumpCubeSectionTest,
    "PinWright.AssetDump.StaticMeshSections.Cube",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticMeshDumpCubeSectionTest::RunTest(const FString& Parameters)
{
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!TestNotNull(TEXT("Engine cube fixture loads"), Mesh))
    {
        return false;
    }

    const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
    if (!TestNotNull(TEXT("Engine cube has render data"), RenderData)
        || !TestTrue(TEXT("Engine cube has LOD0"), RenderData->LODResources.Num() > 0)
        || !TestEqual(TEXT("Engine cube LOD0 has one section"), RenderData->LODResources[0].Sections.Num(), 1))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Result = StaticMeshDumpBuilder::BuildStaticMeshJson(Mesh);
    if (!TestTrue(TEXT("Static-mesh JSON is built"), Result.IsValid()))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
    TestTrue(TEXT("sections is an array"), Result->TryGetArrayField(TEXT("sections"), Sections));
    const TSharedPtr<FJsonObject> Lod0Section = StaticMeshSectionsTest_FindObject(
        Sections, TEXT("lodIndex"), 0, TEXT("index"), 0);
    if (!TestTrue(TEXT("sections contains LOD0 section 0"), Lod0Section.IsValid()))
    {
        return false;
    }
    StaticMeshSectionsTest_AssertSection(*this, Lod0Section, RenderData->LODResources[0].Sections[0]);

    const TArray<TSharedPtr<FJsonValue>>* SlotUsage = nullptr;
    TestTrue(TEXT("slotUsage is an array"), Result->TryGetArrayField(TEXT("slotUsage"), SlotUsage));
    const TSharedPtr<FJsonObject> Slot0 = StaticMeshSectionsTest_FindObject(
        SlotUsage, TEXT("materialIndex"), 0);
    if (TestTrue(TEXT("slotUsage contains material slot 0"), Slot0.IsValid()))
    {
        TestEqual(TEXT("cube slot 0 owns every LOD0 triangle"),
            static_cast<int32>(Slot0->GetNumberField(TEXT("lod0TriangleCount"))),
            static_cast<int32>(RenderData->LODResources[0].Sections[0].NumTriangles));
        TestTrue(TEXT("cube slot 0 fraction is one"),
            FMath::IsNearlyEqual(Slot0->GetNumberField(TEXT("lod0TriangleFraction")), 1.0));
    }

    const TArray<TSharedPtr<FJsonValue>>* UvChannelsByLod = nullptr;
    const int32 ExpectedUvChannels = Mesh->GetNumUVChannels(0);
    TestTrue(TEXT("Engine cube has at least one usable UV channel"), ExpectedUvChannels > 0);
    if (TestTrue(TEXT("uvChannelsByLod is an array"),
        Result->TryGetArrayField(TEXT("uvChannelsByLod"), UvChannelsByLod))
        && TestTrue(TEXT("uvChannelsByLod contains LOD0"), UvChannelsByLod->Num() > 0))
    {
        TestEqual(TEXT("LOD0 UV-channel count matches the engine asset API"),
            static_cast<int32>((*UvChannelsByLod)[0]->AsNumber()),
            ExpectedUvChannels);
    }

    TestEqual(TEXT("lightMapCoordinateIndex matches the asset"),
        static_cast<int32>(Result->GetNumberField(TEXT("lightMapCoordinateIndex"))),
        Mesh->GetLightMapCoordinateIndex());

    const FString Text = StaticMeshTextEmitter::BuildText(Result);
    TestTrue(TEXT("text sidecar carries sections"), Text.Contains(TEXT("sections {")));
    TestTrue(TEXT("text sidecar carries slotUsage"), Text.Contains(TEXT("slotUsage {")));
    TestTrue(TEXT("text sidecar carries uvChannelsByLod"), Text.Contains(TEXT("uvChannelsByLod:")));
    TestTrue(TEXT("text sidecar carries lightMapCoordinateIndex"),
        Text.Contains(TEXT("lightMapCoordinateIndex:")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FStaticMeshDumpMultiSectionTest,
    "PinWright.AssetDump.StaticMeshSections.MultiSection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FStaticMeshDumpMultiSectionTest::RunTest(const FString& Parameters)
{
    const FString PackagePath = FString::Printf(TEXT("/Game/PinWrightTests/SM_DumpSections_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UStaticMesh* Mesh = StaticMeshSectionsTest_CreateMultiSectionFixture(PackagePath);
    if (Mesh)
    {
        Mesh->AddToRoot();
    }
    ON_SCOPE_EXIT
    {
        if (Mesh)
        {
            Mesh->RemoveFromRoot();
        }
        CleanupTestAsset(PackagePath);
    };

    if (!TestNotNull(TEXT("Two-slot, three-triangle fixture builds"), Mesh))
    {
        return false;
    }
    TestEqual(TEXT("Fixture has two usable UV channels according to the engine asset API"),
        Mesh->GetNumUVChannels(0), 2);

    const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
    if (!TestNotNull(TEXT("Fixture has render data"), RenderData)
        || !TestTrue(TEXT("Fixture has LOD0"), RenderData->LODResources.Num() > 0)
        || !TestEqual(TEXT("Fixture LOD0 has two sections"), RenderData->LODResources[0].Sections.Num(), 2))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Result = StaticMeshDumpBuilder::BuildStaticMeshJson(Mesh);
    if (!TestTrue(TEXT("Fixture JSON is built"), Result.IsValid()))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
    TestTrue(TEXT("sections is an array"), Result->TryGetArrayField(TEXT("sections"), Sections));
    TestEqual(TEXT("Fixture emits both LOD0 sections"), Sections ? Sections->Num() : 0, 2);
    for (int32 SectionIndex = 0; SectionIndex < 2; ++SectionIndex)
    {
        const TSharedPtr<FJsonObject> Section = StaticMeshSectionsTest_FindObject(
            Sections, TEXT("lodIndex"), 0, TEXT("index"), SectionIndex);
        if (TestTrue(FString::Printf(TEXT("sections contains LOD0 section %d"), SectionIndex),
            Section.IsValid()))
        {
            StaticMeshSectionsTest_AssertSection(
                *this, Section, RenderData->LODResources[0].Sections[SectionIndex]);
            const int32 MaterialIndex = RenderData->LODResources[0].Sections[SectionIndex].MaterialIndex;
            if (!TestTrue(TEXT("section material index resolves to a StaticMaterials slot"),
                Mesh->GetStaticMaterials().IsValidIndex(MaterialIndex)))
            {
                continue;
            }
            TestEqual(TEXT("section carries its material slot name"),
                Section->GetStringField(TEXT("materialSlotName")),
                Mesh->GetStaticMaterials()[MaterialIndex].MaterialSlotName.ToString());
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* SlotUsage = nullptr;
    TestTrue(TEXT("slotUsage is an array"), Result->TryGetArrayField(TEXT("slotUsage"), SlotUsage));
    TestEqual(TEXT("slotUsage has one row per material slot"), SlotUsage ? SlotUsage->Num() : 0, 2);
    for (int32 MaterialIndex = 0; MaterialIndex < 2; ++MaterialIndex)
    {
        const TSharedPtr<FJsonObject> Slot = StaticMeshSectionsTest_FindObject(
            SlotUsage, TEXT("materialIndex"), MaterialIndex);
        if (!TestTrue(FString::Printf(TEXT("slotUsage contains material %d"), MaterialIndex),
            Slot.IsValid()))
        {
            continue;
        }
        const int32 ExpectedTriangles = MaterialIndex == 0 ? 1 : 2;
        TestEqual(TEXT("slot usage aggregates LOD0 section triangles"),
            static_cast<int32>(Slot->GetNumberField(TEXT("lod0TriangleCount"))), ExpectedTriangles);
        TestTrue(TEXT("slot usage fraction uses all LOD0 triangles as denominator"),
            FMath::IsNearlyEqual(Slot->GetNumberField(TEXT("lod0TriangleFraction")),
                static_cast<double>(ExpectedTriangles) / 3.0));
    }

    const TArray<TSharedPtr<FJsonValue>>* UvChannelsByLod = nullptr;
    if (TestTrue(TEXT("uvChannelsByLod is an array"),
        Result->TryGetArrayField(TEXT("uvChannelsByLod"), UvChannelsByLod))
        && TestTrue(TEXT("uvChannelsByLod contains LOD0"), UvChannelsByLod->Num() > 0))
    {
        TestEqual(TEXT("Fixture preserves its two UV channels"),
            static_cast<int32>((*UvChannelsByLod)[0]->AsNumber()), 2);
    }
    TestEqual(TEXT("Fixture reports its light-map coordinate index"),
        static_cast<int32>(Result->GetNumberField(TEXT("lightMapCoordinateIndex"))), 1);

    const FString Text = StaticMeshTextEmitter::BuildText(Result);
    TestTrue(TEXT("text sidecar carries SlotA"), Text.Contains(TEXT("materialSlotName: SlotA")));
    TestTrue(TEXT("text sidecar carries SlotB"), Text.Contains(TEXT("materialSlotName: SlotB")));
    TestTrue(TEXT("text sidecar carries the one-triangle slot count"),
        Text.Contains(TEXT("lod0TriangleCount: 1")));
    TestTrue(TEXT("text sidecar carries the two-triangle slot count"),
        Text.Contains(TEXT("lod0TriangleCount: 2")));
    TestTrue(TEXT("text sidecar carries the one-third slot fraction"),
        Text.Contains(FString::Printf(TEXT("lod0TriangleFraction: %s"),
            *FString::SanitizeFloat(1.0 / 3.0))));
    TestTrue(TEXT("text sidecar carries the two-thirds slot fraction"),
        Text.Contains(FString::Printf(TEXT("lod0TriangleFraction: %s"),
            *FString::SanitizeFloat(2.0 / 3.0))));
    TestTrue(TEXT("text sidecar carries the two-channel LOD0 readout"),
        Text.Contains(TEXT("uvChannelsByLod: [2]")));
    TestTrue(TEXT("text sidecar carries light-map channel 1"),
        Text.Contains(TEXT("lightMapCoordinateIndex: 1")));
    return true;
}
