// Copyright (c) 2026 Alexander Penkin. MIT License.

// level.describe_offline: reads a saved .umap's actors without loading it.
//
// The fixture is a real map saved under the scratch root. The reading is checked against the
// LIVE actors through the same ActorDescribeBuilder asset.dump uses, so "offline matches loaded"
// is the assertion rather than a hand-written expectation; and the no-load guarantee is checked
// on a byte copy of that file that has never existed in memory, so a verb that loaded it would
// leave its package behind.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Tests/AutomationSuiteMaintenance.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "Utils/ActorDescribeBuilder.h"

#include "Components/StaticMeshComponent.h"
#include "Curves/CurveFloat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/ArchiveProxy.h"
#include "Serialization/MemoryWriter.h"
#include "UObject/ObjectResource.h"
#include "UObject/Package.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace TestLevelDescribeOfflineHelpers
{
    // A map with two labelled probes - a root actor carrying folder / tags / non-unit scale, and
    // a child attached to it with a relative offset, so the world transform must be composed
    // through the attachment chain - saved to a .umap under the scratch root.
    struct FSavedMap
    {
        FString PackageName;
        FString Filename;
        UPackage* Package = nullptr;
        UWorld* World = nullptr;
        AStaticMeshActor* Parent = nullptr;
        AStaticMeshActor* Child = nullptr;

        bool Create(FAutomationTestBase& Test)
        {
            const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
            const FString WorldName = FString::Printf(TEXT("LDO_%s"), *Stamp);
            PackageName = FString::Printf(TEXT("%s/LDO_%s/%s"),
                PinWrightSuiteMaintenance::ScratchRootPackagePath(), *Stamp, *WorldName);
            if (!Test.TestTrue(TEXT("fixture map path resolves to a .umap filename"),
                    FPackageName::TryConvertLongPackageNameToFilename(
                        PackageName, Filename, FPackageName::GetMapPackageExtension())))
            {
                return false;
            }
            Filename = FPaths::ConvertRelativePathToFull(Filename);

            Package = CreatePackage(*PackageName);
            World = Package
                ? UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, FName(*WorldName), Package, true)
                : nullptr;
            if (!Test.TestNotNull(TEXT("fixture world created"), World))
            {
                return false;
            }
            // Preconditions the expectations below rest on: actors embedded in the .umap, and
            // folders stored as the FolderPath name rather than as actor-folder objects.
            if (!Test.TestFalse(TEXT("fixture level embeds its actors"), World->PersistentLevel->IsUsingExternalActors())
                || !Test.TestFalse(TEXT("fixture level stores folders by name"), World->PersistentLevel->IsUsingActorFolders()))
            {
                return false;
            }

            Parent = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(),
                FTransform(FRotator(10.0, 20.0, 30.0), FVector(100.0, -200.0, 300.0), FVector(1.0, 2.0, 3.0)));
            Child = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform::Identity);
            if (!Test.TestTrue(TEXT("fixture probes spawned"), Parent && Child))
            {
                return false;
            }
            Parent->SetActorLabel(TEXT("OfflineParent"));
            Parent->SetFolderPath(TEXT("Probes/Sub"));
            Parent->Tags = {TEXT("beta"), TEXT("alpha")};
            Child->SetActorLabel(TEXT("OfflineChild"));
            Child->AttachToActor(Parent, FAttachmentTransformRules::KeepRelativeTransform);
            Child->SetActorRelativeLocation(FVector(50.0, 0.0, 0.0));
            if (!Test.TestTrue(TEXT("child probe is attached, so its world transform is a composition"),
                    Child->GetAttachParentActor() == Parent
                    && !Child->GetActorLocation().Equals(FVector(50.0, 0.0, 0.0), 1.0)))
            {
                return false;
            }

            // UWorld::CreateWorld sets only RF_Transactional; a saved map's world is public and
            // standalone (UEditorEngine::OnPreSaveWorld sets both), and SavePackage refuses an asset
            // without the TopLevelFlags. DestroyWorld clears RF_Standalone again on teardown.
            World->SetFlags(RF_Public | RF_Standalone);
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Standalone;
            SaveArgs.SaveFlags = SAVE_NoError;
            return Test.TestTrue(TEXT("fixture map saved"), UPackage::SavePackage(Package, World, *Filename, SaveArgs))
                && Test.TestTrue(TEXT("fixture .umap exists on disk"), IFileManager::Get().FileExists(*Filename));
        }

        ~FSavedMap()
        {
            if (World)
            {
                World->DestroyWorld(/*bInformEngineOfWorld=*/false);
            }
            if (Package)
            {
                Package->SetDirtyFlag(false);
            }
            if (!Filename.IsEmpty())
            {
                IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Filename), /*RequireExists=*/false, /*Tree=*/true);
            }
        }
    };

    TSharedPtr<FJsonObject> Describe(FAutomationTestBase& Test, const FString& LevelPath, FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("levelPath"), LevelPath);
        Test.TestTrue(TEXT("level.describe_offline registered"),
            InvokeHandlerWithCapture(TEXT("level.describe_offline"), Payload, Capture));
        return Capture.Result;
    }

    TSharedPtr<FJsonObject> FindActorByName(const TSharedPtr<FJsonObject>& Result, const FString& Name)
    {
        const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
        if (Result.IsValid() && Result->TryGetArrayField(TEXT("actors"), Actors))
        {
            for (const TSharedPtr<FJsonValue>& Value : *Actors)
            {
                const TSharedPtr<FJsonObject> Actor = Value->AsObject();
                if (Actor.IsValid() && Actor->GetStringField(TEXT("name")) == Name)
                {
                    return Actor;
                }
            }
        }
        return nullptr;
    }

    FVector ReadVector(const TSharedPtr<FJsonObject>& Transform, const TCHAR* Key, const TCHAR* X, const TCHAR* Y, const TCHAR* Z)
    {
        const TSharedPtr<FJsonObject> V = Transform->GetObjectField(Key);
        return FVector(V->GetNumberField(X), V->GetNumberField(Y), V->GetNumberField(Z));
    }

    void ExpectMatchesLive(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Offline, AActor* Actor)
    {
        const FString What = Actor->GetActorLabel();
        if (!Test.TestTrue(FString::Printf(TEXT("%s is in the offline read"), *What), Offline.IsValid()))
        {
            return;
        }
        const TSharedPtr<FJsonObject> Live = ActorDescribeBuilder::BuildActorJson(Actor, TEXT("embedded"));
        for (const TCHAR* Key : {TEXT("schema"), TEXT("storage"), TEXT("name"), TEXT("label"), TEXT("path"),
                 TEXT("class"), TEXT("level"), TEXT("folder"), TEXT("guid")})
        {
            Test.TestEqual(FString::Printf(TEXT("%s offline '%s' equals the loaded describe"), *What, Key),
                Offline->GetStringField(Key), Live->GetStringField(Key));
        }
        TArray<FString> OfflineTags;
        TArray<FString> LiveTags;
        Offline->TryGetStringArrayField(TEXT("tags"), OfflineTags);
        Live->TryGetStringArrayField(TEXT("tags"), LiveTags);
        Test.TestEqual(FString::Printf(TEXT("%s offline tags equal the loaded describe"), *What), OfflineTags, LiveTags);

        const TSharedPtr<FJsonObject> OfflineTransform = Offline->GetObjectField(TEXT("transform"));
        const TSharedPtr<FJsonObject> LiveTransform = Live->GetObjectField(TEXT("transform"));
        Test.TestTrue(FString::Printf(TEXT("%s offline location equals the loaded world location"), *What),
            ReadVector(OfflineTransform, TEXT("location"), TEXT("x"), TEXT("y"), TEXT("z")).Equals(
                ReadVector(LiveTransform, TEXT("location"), TEXT("x"), TEXT("y"), TEXT("z")), 0.01));
        Test.TestTrue(FString::Printf(TEXT("%s offline rotation equals the loaded world rotation"), *What),
            ReadVector(OfflineTransform, TEXT("rotation"), TEXT("pitch"), TEXT("yaw"), TEXT("roll")).Equals(
                ReadVector(LiveTransform, TEXT("rotation"), TEXT("pitch"), TEXT("yaw"), TEXT("roll")), 0.01));
        Test.TestTrue(FString::Printf(TEXT("%s offline scale equals the loaded world scale"), *What),
            ReadVector(OfflineTransform, TEXT("scale"), TEXT("x"), TEXT("y"), TEXT("z")).Equals(
                ReadVector(LiveTransform, TEXT("scale"), TEXT("x"), TEXT("y"), TEXT("z")), 0.0001));
        Test.TestTrue(FString::Printf(TEXT("%s transform is exact (native class: every default is readable)"), *What),
            Offline->GetBoolField(TEXT("transformExact")));
        bool bTagsExact = false;
        Test.TestTrue(FString::Printf(TEXT("%s tags are exact (native class: an unsaved Tags is the class default)"), *What),
            Offline->TryGetBoolField(TEXT("tagsExact"), bTagsExact) && bTagsExact);
    }

    // Bytes one export-table record takes in this file: FObjectExport is fixed-size for a given
    // file version, with its names written as (index, number) the way a linker writes them.
    int64 ExportRecordSize(const FPackageFileSummary& Summary)
    {
        struct FIndexNameWriter : FArchiveProxy
        {
            using FArchiveProxy::FArchiveProxy;
            virtual FArchive& operator<<(FName& Value) override
            {
                int32 Index = 0;
                int32 Number = 0;
                InnerArchive << Index << Number;
                return *this;
            }
        };
        TArray<uint8> Record;
        FMemoryWriter Writer(Record);
        Writer.SetUEVer(Summary.GetFileVersionUE());
        FIndexNameWriter Proxy(Writer);
        FObjectExport Export;
        Proxy << Export;
        return Record.Num();
    }

    template <typename T>
    void Poke(TArray<uint8>& Bytes, int64 Offset, T Value)
    {
        FMemory::Memcpy(Bytes.GetData() + Offset, &Value, sizeof(T));
    }

    template <typename T>
    T Peek(const TArray<uint8>& Bytes, int64 Offset)
    {
        T Value;
        FMemory::Memcpy(&Value, Bytes.GetData() + Offset, sizeof(T));
        return Value;
    }

    int32 CountLiveActors(const UWorld* World)
    {
        int32 Count = 0;
        for (const AActor* Actor : World->PersistentLevel->Actors)
        {
            Count += (Actor && !Actor->HasAnyFlags(RF_Transient)) ? 1 : 0; // transient actors are not saved
        }
        return Count;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelDescribeOfflineMatchesLoadedTest,
    "PinWright.level.describe_offline.MatchesLoadedActorDescribe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelDescribeOfflineMatchesLoadedTest::RunTest(const FString& Parameters)
{
    using namespace TestLevelDescribeOfflineHelpers;

    FSavedMap Map;
    if (!Map.Create(*this))
    {
        return false;
    }

    FTestResponseCapture Capture;
    const TSharedPtr<FJsonObject> Result = Describe(*this, Map.PackageName, Capture);
    if (!TestTrue(FString::Printf(TEXT("describe_offline succeeds (%s: %s)"), *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess && Result.IsValid()))
    {
        return false;
    }

    TestEqual(TEXT("levelPath echoes the package"), Result->GetStringField(TEXT("levelPath")), Map.PackageName);
    TestEqual(TEXT("worldPath is the saved world"), Result->GetStringField(TEXT("worldPath")), Map.World->GetPathName());
    TestEqual(TEXT("every embedded actor of the saved level is listed"),
        static_cast<int32>(Result->GetNumberField(TEXT("embeddedActorCount"))), CountLiveActors(Map.World));
    TestFalse(TEXT("an embedded-actor level does not report external actors"), Result->GetBoolField(TEXT("usesExternalActors")));
    TestEqual(TEXT("no external actor references"),
        static_cast<int32>(Result->GetNumberField(TEXT("externalActorReferenceCount"))), 0);

    for (AActor* Actor : Map.World->PersistentLevel->Actors)
    {
        if (Actor && !Actor->HasAnyFlags(RF_Transient))
        {
            const TSharedPtr<FJsonObject> Offline = FindActorByName(Result, Actor->GetName());
            TestTrue(FString::Printf(TEXT("%s is listed with the loaded path and class"), *Actor->GetName()),
                Offline.IsValid()
                && Offline->GetStringField(TEXT("path")) == Actor->GetPathName()
                && Offline->GetStringField(TEXT("class")) == Actor->GetClass()->GetPathName());
        }
    }
    ExpectMatchesLive(*this, FindActorByName(Result, Map.Parent->GetName()), Map.Parent);
    ExpectMatchesLive(*this, FindActorByName(Result, Map.Child->GetName()), Map.Child);

    // Provenance: a clean saved package raises no warning; a dirty loaded copy says the read is
    // of the file, not of the editor's state.
    TestFalse(TEXT("a clean package reports no unsaved changes"),
        Result->GetObjectField(TEXT("source"))->GetBoolField(TEXT("unsavedChanges")));
    Map.Package->SetDirtyFlag(true);
    FTestResponseCapture DirtyCapture;
    const TSharedPtr<FJsonObject> DirtyResult = Describe(*this, Map.PackageName, DirtyCapture);
    TArray<FString> Warnings;
    TestTrue(TEXT("a dirty loaded copy is reported as unsaved changes with a warning"),
        DirtyCapture.bSuccess && DirtyResult.IsValid()
        && DirtyResult->GetObjectField(TEXT("source"))->GetBoolField(TEXT("unsavedChanges"))
        && DirtyResult->TryGetStringArrayField(TEXT("warnings"), Warnings)
        && Warnings.ContainsByPredicate([](const FString& W) { return W.Contains(TEXT("unsaved changes")); }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelDescribeOfflineNeverLoadsTest,
    "PinWright.level.describe_offline.NeverLoadsTheMapOrTouchesTheEditorWorld",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelDescribeOfflineNeverLoadsTest::RunTest(const FString& Parameters)
{
    using namespace TestLevelDescribeOfflineHelpers;

    if (!GEditor)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("editor-required"),
            TEXT("The no-load guarantee is measured against the editor world."));
        return true;
    }

    FSavedMap Map;
    if (!Map.Create(*this))
    {
        return false;
    }

    // A byte copy of the saved map under a name no object has ever had.
    const FString CopyPackageName = Map.PackageName + TEXT("_Copy");
    const FString CopyFilename = FPaths::ChangeExtension(Map.Filename, TEXT("")) + TEXT("_Copy")
        + FPackageName::GetMapPackageExtension();
    if (!TestEqual(TEXT("copied the fixture .umap"),
            IFileManager::Get().Copy(*CopyFilename, *Map.Filename), static_cast<uint32>(COPY_OK))
        || !TestNull(TEXT("precondition: the copy's package is not in memory"), FindPackage(nullptr, *CopyPackageName)))
    {
        return false;
    }

    UWorld* EditorWorldBefore = GEditor->GetEditorWorldContext().World();
    FTestResponseCapture Capture;
    const TSharedPtr<FJsonObject> Result = Describe(*this, CopyPackageName, Capture);
    if (!TestTrue(FString::Printf(TEXT("describe_offline succeeds on the never-loaded copy (%s: %s)"),
            *Capture.ErrorCode, *Capture.Message), Capture.bSuccess && Result.IsValid()))
    {
        return false;
    }

    TestNull(TEXT("the map package was never created in memory"), FindPackage(nullptr, *CopyPackageName));
    TestTrue(TEXT("the active editor world is unchanged"), GEditor->GetEditorWorldContext().World() == EditorWorldBefore);
    // It did read the file: the same actors, addressed under the copy's package.
    TestEqual(TEXT("the copy lists every actor of the saved map"),
        static_cast<int32>(Result->GetNumberField(TEXT("embeddedActorCount"))), CountLiveActors(Map.World));
    const TSharedPtr<FJsonObject> Parent = FindActorByName(Result, Map.Parent->GetName());
    TestTrue(TEXT("actor paths are addressed in the requested package"),
        Parent.IsValid() && Parent->GetStringField(TEXT("path")).StartsWith(CopyPackageName + TEXT(".")));
    TestTrue(TEXT("the label came off the file"),
        Parent.IsValid() && Parent->GetStringField(TEXT("label")) == TEXT("OfflineParent"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelDescribeOfflineRefusalsTest,
    "PinWright.level.describe_offline.RefusesMissingAndNonMapPackages",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelDescribeOfflineRefusalsTest::RunTest(const FString& Parameters)
{
    using namespace TestLevelDescribeOfflineHelpers;

    const FString Stamp = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FTestResponseCapture Missing;
    Describe(*this, FString::Printf(TEXT("%s/LDO_Missing_%s"), PinWrightSuiteMaintenance::ScratchRootPackagePath(), *Stamp), Missing);
    TestFalse(TEXT("a map that does not exist is an error, not an empty level"), Missing.bSuccess);
    TestEqual(TEXT("missing map is ASSET_NOT_FOUND"), Missing.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));

    // A saved non-map package at the requested path.
    const FString AssetName = FString::Printf(TEXT("LDO_Curve_%s"), *Stamp);
    const FString PackageName = FString::Printf(TEXT("%s/%s"), PinWrightSuiteMaintenance::ScratchRootPackagePath(), *AssetName);
    FString Filename;
    if (!TestTrue(TEXT("curve fixture path resolves"), FPackageName::TryConvertLongPackageNameToFilename(
            PackageName, Filename, FPackageName::GetAssetPackageExtension())))
    {
        return false;
    }
    UPackage* Package = CreatePackage(*PackageName);
    UCurveFloat* Curve = NewObject<UCurveFloat>(Package, FName(*AssetName), RF_Public | RF_Standalone);
    ON_SCOPE_EXIT { CleanupTestAsset(PackageName); };
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    if (!TestTrue(TEXT("curve fixture saved"), UPackage::SavePackage(Package, Curve, *Filename, SaveArgs)))
    {
        return false;
    }

    FTestResponseCapture NotAMap;
    Describe(*this, PackageName, NotAMap);
    TestFalse(TEXT("a non-map package is refused"), NotAMap.bSuccess);
    TestEqual(TEXT("non-map package is NOT_A_MAP"), NotAMap.ErrorCode, FString(TEXT("NOT_A_MAP")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLevelDescribeOfflineMalformedTest,
    "PinWright.level.describe_offline.MalformedFilesAreRefusedNotCrashed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FLevelDescribeOfflineMalformedTest::RunTest(const FString& Parameters)
{
    using namespace TestLevelDescribeOfflineHelpers;

    FSavedMap Map;
    if (!Map.Create(*this))
    {
        return false;
    }
    TArray<uint8> Bytes;
    FPackageFileSummary Summary;
    {
        TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Map.Filename));
        if (Reader)
        {
            *Reader << Summary;
        }
    }
    // Preconditions: the header tables sit in the order the truncations below rely on.
    if (!TestTrue(TEXT("fixture bytes read"), FFileHelper::LoadFileToArray(Bytes, *Map.Filename))
        || !TestTrue(TEXT("fixture header is names < imports < exports < EOF"),
            Summary.Tag == PACKAGE_FILE_TAG && Summary.NameOffset > 32
            && Summary.ImportOffset > Summary.NameOffset + 16 && Summary.ExportOffset > Summary.ImportOffset
            && Summary.ExportOffset < Bytes.Num()))
    {
        return false;
    }
    // The first name is an ANSI entry: int32 length (with its NUL), then the characters.
    const int32 FirstNameLength = Peek<int32>(Bytes, Summary.NameOffset);
    // Each export record: Class, Super, Template, Outer (4 x int32), ObjectName (index, number),
    // ObjectFlags, SerialSize (int64 at +28), SerialOffset (int64 at +36). Checked against every
    // record so a layout drift fails here rather than corrupting the wrong bytes.
    const int64 RecordSize = ExportRecordSize(Summary);
    bool bExportLayoutKnown = RecordSize >= 44 && Summary.ExportCount > 0
        && Summary.ExportOffset + RecordSize * Summary.ExportCount <= Bytes.Num();
    for (int32 Index = 0; bExportLayoutKnown && Index < Summary.ExportCount; ++Index)
    {
        const int64 Record = Summary.ExportOffset + RecordSize * Index;
        const int64 SerialSize = Peek<int64>(Bytes, Record + 28);
        const int64 SerialOffset = Peek<int64>(Bytes, Record + 36);
        bExportLayoutKnown = SerialSize >= 0 && SerialOffset > 0 && SerialOffset + SerialSize <= Bytes.Num();
    }
    if (!TestTrue(TEXT("fixture's first name is a terminated ANSI entry"),
            FirstNameLength > 1 && Summary.NameOffset + 4 + FirstNameLength <= Bytes.Num()
            && Bytes[Summary.NameOffset + 4 + FirstNameLength - 1] == 0)
        || !TestTrue(TEXT("fixture's export records have the expected layout"), bExportLayoutKnown))
    {
        return false;
    }

    struct FCase { const TCHAR* Suffix; TArray<uint8> Content; };
    TArray<FCase> Cases;
    Cases.Add({TEXT("_Garbage"), TArray<uint8>()});
    Cases.Last().Content.Init('x', 64);
    // Offsets in the summary now point past the end of the file: the reader must refuse before
    // seeking there (the file reader's Seek asserts on an out-of-range position).
    Cases.Add({TEXT("_CutInNames"), TArray<uint8>(Bytes.GetData(), Summary.NameOffset + 16)});
    Cases.Add({TEXT("_ExportsPastEof"), TArray<uint8>(Bytes.GetData(), Summary.ExportOffset - 1)});
    // Export table offset is exactly EOF: a legal seek, then every export read runs off the end.
    Cases.Add({TEXT("_ExportsAtEof"), TArray<uint8>(Bytes.GetData(), Summary.ExportOffset)});
    // Table offsets intact, the first name's length corrupt: FNameEntrySerialized would leave its
    // buffer unfilled and FName(Entry) would Strlen garbage into its max-length checkf.
    Cases.Add({TEXT("_NameLengthCorrupt"), Bytes});
    Poke<int32>(Cases.Last().Content, Summary.NameOffset, 0x7FFFFFFF);
    // The first name's terminator overwritten: Strlen would run on past the entry.
    Cases.Add({TEXT("_NameUnterminated"), Bytes});
    Cases.Last().Content[Summary.NameOffset + 4 + FirstNameLength - 1] = 'x';
    // Every export's data range near INT64_MAX: offset + size overflows int64, and a guard that
    // adds them first lets Seek reach the file reader's past-EOF checkf.
    Cases.Add({TEXT("_ExportRangeOverflows"), Bytes});
    // Every export's ObjectName with a negative number: "_-N" never re-parses as a number, so the
    // name-table reader must refuse it as corrupt rather than build a name that cannot round-trip.
    Cases.Add({TEXT("_NegativeNameNumber"), Bytes});
    for (int32 Index = 0; Index < Summary.ExportCount; ++Index)
    {
        const int64 Record = Summary.ExportOffset + RecordSize * Index;
        Poke<int64>(Cases[Cases.Num() - 2].Content, Record + 28, 16);
        Poke<int64>(Cases[Cases.Num() - 2].Content, Record + 36, MAX_int64 - 8);
        Poke<int32>(Cases.Last().Content, Record + 20, -1);
    }

    // The crafted files go under /Temp (mounted on Saved/), never under a content root: the asset
    // registry neither watches nor scans read-only roots (AssetRegistry.cpp QueryRootContentPaths
    // with bIncludeReadOnlyRoots=false for its watchers and searches; ScanPathsSynchronous refuses
    // any /Temp path outright), and its own package reader
    // (PackageReader.cpp SerializeNameMap) has the same unterminated-name exposure the verb closes.
    // So neither a mid-test rescan nor the next editor start can read one, even if this test crashes.
    const FString CraftedDir = FString::Printf(TEXT("/Temp/PinWright/TestTemp/%s"), *FPaths::GetBaseFilename(Map.Filename));
    FString CraftedFileDir;
    if (!TestTrue(TEXT("/Temp resolves to a directory under Saved/, outside every scanned content root"),
            FPackageName::TryConvertLongPackageNameToFilename(CraftedDir, CraftedFileDir)
            && FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(CraftedFileDir),
                FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()))))
    {
        return false;
    }
    CraftedFileDir = FPaths::ConvertRelativePathToFull(CraftedFileDir);
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*CraftedFileDir, /*RequireExists=*/false, /*Tree=*/true); };

    for (const FCase& Case : Cases)
    {
        const FString PackageName = CraftedDir + TEXT("/LDO") + Case.Suffix;
        const FString Filename = CraftedFileDir / (FString(TEXT("LDO")) + Case.Suffix + FPackageName::GetMapPackageExtension());
        if (!TestTrue(FString::Printf(TEXT("%s written"), Case.Suffix), FFileHelper::SaveArrayToFile(Case.Content, *Filename)))
        {
            continue;
        }
        FTestResponseCapture Capture;
        Describe(*this, PackageName, Capture);
        TestFalse(FString::Printf(TEXT("%s is not reported as a level"), Case.Suffix), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("%s is PARSE_FAILED"), Case.Suffix), Capture.ErrorCode, FString(TEXT("PARSE_FAILED")));
        IFileManager::Get().Delete(*Filename, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
    }
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
