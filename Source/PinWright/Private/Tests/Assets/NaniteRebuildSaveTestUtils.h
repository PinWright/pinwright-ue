// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "Utils/AssetUtils.h"

namespace NaniteRebuildSaveTestUtils
{
    inline UStaticMesh* CreateFixture(const FString& PackagePath)
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

        FMeshDescription MeshDescription;
        FStaticMeshAttributes Attributes(MeshDescription);
        Attributes.Register();

        const FVertexID V0 = MeshDescription.CreateVertex();
        const FVertexID V1 = MeshDescription.CreateVertex();
        const FVertexID V2 = MeshDescription.CreateVertex();
        TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
        Positions[V0] = FVector3f(100.0f, 0.0f, 0.0f);
        Positions[V1] = FVector3f(0.0f, 100.0f, 0.0f);
        Positions[V2] = FVector3f(0.0f, 0.0f, 50.0f);

        const FVertexInstanceID VI0 = MeshDescription.CreateVertexInstance(V0);
        const FVertexInstanceID VI1 = MeshDescription.CreateVertexInstance(V1);
        const FVertexInstanceID VI2 = MeshDescription.CreateVertexInstance(V2);
        TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
        Normals[VI0] = FVector3f(0.0f, 0.0f, 1.0f);
        Normals[VI1] = FVector3f(0.0f, 0.0f, 1.0f);
        Normals[VI2] = FVector3f(0.0f, 0.0f, 1.0f);

        const FPolygonGroupID PolygonGroup = MeshDescription.CreatePolygonGroup();
        const FVertexInstanceID Triangle[3] = {VI0, VI1, VI2};
        MeshDescription.CreateTriangle(PolygonGroup, Triangle);

        UStaticMesh::FBuildMeshDescriptionsParams BuildParams;
        BuildParams.bFastBuild = true;
        if (!Mesh->BuildFromMeshDescriptions({&MeshDescription}, BuildParams))
        {
            return nullptr;
        }

        Mesh->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Mesh);
        return Mesh;
    }

    class FScopedFixture
    {
    public:
        FScopedFixture(const TCHAR* Prefix, const TCHAR* Suffix)
            : PackagePath(FString::Printf(TEXT("/Game/PinWrightTests/SM_%sNaniteSave_%s_%s"),
                Prefix, Suffix, *FGuid::NewGuid().ToString(EGuidFormats::Digits)))
            , Mesh(CreateFixture(PackagePath))
        {
            if (Mesh)
            {
                Mesh->AddToRoot();
            }
        }

        ~FScopedFixture()
        {
            if (Mesh)
            {
                if (Mesh->IsCompiling())
                {
                    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
                }
                if (UPackage* Package = Mesh->GetOutermost())
                {
                    Package->SetDirtyFlag(false);
                }
                Mesh->RemoveFromRoot();
            }
            CleanupTestAsset(PackagePath);
        }

        FScopedFixture(const FScopedFixture&) = delete;
        FScopedFixture& operator=(const FScopedFixture&) = delete;

        FString PackagePath;
        UStaticMesh* Mesh = nullptr;
    };

    struct FPackageFileBaseline
    {
        FString Filename;
        int64 SizeBytes = 0;
        TArray<uint8> Bytes;
    };

    inline bool SaveFixtureBaseline(
        FAutomationTestBase& Test,
        FScopedFixture& Fixture,
        FPackageFileBaseline& OutBaseline)
    {
        if (!Fixture.Mesh)
        {
            return false;
        }

        FStaticMeshCompilingManager::Get().FinishCompilation({Fixture.Mesh});
        FString SavedPackageName;
        int64 ReportedSize = 0;
        EAssetSaveState SaveState = EAssetSaveState::NotRequested;
        const bool bSaved = SaveAssetToDiskReportingPresence(
            Fixture.Mesh, /*bForce=*/true, &SavedPackageName, &ReportedSize, &SaveState);
        Test.TestTrue(TEXT("save:false fixture baseline saved to disk"), bSaved);
        Test.TestEqual(TEXT("baseline save names the fixture package"),
            SavedPackageName, Fixture.PackagePath);

        OutBaseline.Filename = PackageFilenameFromAssetPath(Fixture.PackagePath);
        Test.TestFalse(TEXT("baseline fixture resolves to a .uasset filename"),
            OutBaseline.Filename.IsEmpty());
        OutBaseline.SizeBytes = IFileManager::Get().FileSize(*OutBaseline.Filename);
        Test.TestTrue(TEXT("baseline fixture .uasset is non-empty"),
            OutBaseline.SizeBytes > 0);
        Test.TestEqual(TEXT("baseline save reports the measured file size"),
            ReportedSize, OutBaseline.SizeBytes);
        Test.TestFalse(TEXT("baseline save leaves the fixture package clean"),
            Fixture.Mesh->GetOutermost()->IsDirty());

        const bool bReadBaseline = FFileHelper::LoadFileToArray(
            OutBaseline.Bytes, *OutBaseline.Filename);
        Test.TestTrue(TEXT("baseline fixture bytes can be read"), bReadBaseline);
        return bSaved && OutBaseline.SizeBytes > 0 && bReadBaseline;
    }

    inline bool ReadBool(
        const TSharedPtr<FJsonObject>& Result,
        const TCHAR* Field,
        bool& OutValue)
    {
        return Result.IsValid() && Result->TryGetBoolField(Field, OutValue);
    }

    inline FString ReadString(const TSharedPtr<FJsonObject>& Result, const TCHAR* Field)
    {
        FString Value;
        return Result.IsValid() && Result->TryGetStringField(Field, Value)
            ? Value
            : FString(TEXT("<absent>"));
    }

    inline void TestDurablePersistenceResult(
        FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result,
        const FScopedFixture& Fixture)
    {
        bool bSaveRequested = false;
        Test.TestTrue(TEXT("omitted save defaults to saveRequested:true"),
            ReadBool(Result, TEXT("saveRequested"), bSaveRequested) && bSaveRequested);
        bool bSaved = false;
        Test.TestTrue(TEXT("default save reports saved:true"),
            ReadBool(Result, TEXT("saved"), bSaved) && bSaved);
        Test.TestFalse(TEXT("a durable save has no pendingFlush field"),
            Result->HasField(TEXT("pendingFlush")));
        Test.TestEqual(TEXT("a durable save reports written"),
            ReadString(Result, TEXT("saveState")), FString(TEXT("written")));
        FString SaveDetail;
        Test.TestTrue(TEXT("a durable save carries a non-empty saveDetail"),
            Result->TryGetStringField(TEXT("saveDetail"), SaveDetail) && !SaveDetail.IsEmpty());
        Test.TestEqual(TEXT("the save result names the fixture package"),
            ReadString(Result, TEXT("package")), Fixture.PackagePath);
        Test.TestFalse(TEXT("a durable save does not mark sizeBytes stale"),
            Result->HasField(TEXT("sizeBytesIsStale")));

        double ReportedSize = 0.0;
        Test.TestTrue(TEXT("default-save result carries sizeBytes"),
            Result->TryGetNumberField(TEXT("sizeBytes"), ReportedSize));
        const FString Filename = PackageFilenameFromAssetPath(Fixture.PackagePath);
        Test.TestFalse(TEXT("default-save fixture resolves to a .uasset filename"),
            Filename.IsEmpty());
        const int64 DiskSize = IFileManager::Get().FileSize(*Filename);
        Test.TestTrue(TEXT("default-save fixture .uasset exists and is non-empty"), DiskSize > 0);
        Test.TestEqual(TEXT("default-save result reports the measured file size"),
            ReportedSize, static_cast<double>(DiskSize));
        Test.TestFalse(TEXT("default-save fixture package is clean"),
            Fixture.Mesh->GetOutermost()->IsDirty());
    }

    inline void TestOptOutPersistenceResult(
        FAutomationTestBase& Test,
        const TSharedPtr<FJsonObject>& Result,
        const FScopedFixture& Fixture,
        const FPackageFileBaseline& Baseline)
    {
        bool bSaveRequested = true;
        Test.TestTrue(TEXT("save:false result carries saveRequested:false"),
            ReadBool(Result, TEXT("saveRequested"), bSaveRequested) && !bSaveRequested);
        bool bSaved = true;
        Test.TestTrue(TEXT("save:false result carries saved:false"),
            ReadBool(Result, TEXT("saved"), bSaved) && !bSaved);
        Test.TestFalse(TEXT("save:false has no pendingFlush field"),
            Result->HasField(TEXT("pendingFlush")));
        Test.TestEqual(TEXT("save:false reports notRequested"),
            ReadString(Result, TEXT("saveState")), FString(TEXT("notRequested")));
        FString SaveDetail;
        Test.TestTrue(TEXT("save:false carries a non-empty saveDetail"),
            Result->TryGetStringField(TEXT("saveDetail"), SaveDetail) && !SaveDetail.IsEmpty());
        Test.TestEqual(TEXT("save:false result names the existing fixture package"),
            ReadString(Result, TEXT("package")), Fixture.PackagePath);

        double ReportedSize = -1.0;
        Test.TestTrue(TEXT("save:false result carries sizeBytes"),
            Result->TryGetNumberField(TEXT("sizeBytes"), ReportedSize));
        Test.TestEqual(TEXT("save:false reports the pre-existing on-disk size"),
            ReportedSize, static_cast<double>(Baseline.SizeBytes));
        bool bSizeIsStale = false;
        Test.TestTrue(TEXT("save:false marks the pre-existing sizeBytes as stale"),
            ReadBool(Result, TEXT("sizeBytesIsStale"), bSizeIsStale) && bSizeIsStale);
        Test.TestEqual(TEXT("save:false leaves the baseline file size unchanged"),
            IFileManager::Get().FileSize(*Baseline.Filename), Baseline.SizeBytes);

        TArray<uint8> BytesAfter;
        const bool bReadAfter = FFileHelper::LoadFileToArray(BytesAfter, *Baseline.Filename);
        Test.TestTrue(TEXT("save:false fixture bytes can be re-read"), bReadAfter);
        if (bReadAfter)
        {
            Test.TestTrue(TEXT("save:false leaves the existing .uasset bytes unchanged"),
                BytesAfter == Baseline.Bytes);
        }
        Test.TestTrue(TEXT("save:false leaves the fixture package dirty"),
            Fixture.Mesh->GetOutermost()->IsDirty());
    }
}
