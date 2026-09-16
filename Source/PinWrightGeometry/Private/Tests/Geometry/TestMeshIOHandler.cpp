// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the mesh import/export verbs geometry.export_obj / import_obj /
// export_stl / import_stl. Covers: OBJ round-trip (export a box's mesh to OBJ text,
// re-import into a new actor, assert vertex + triangle counts survive); ASCII STL
// round-trip (triangle count survives, 3 unwelded vertices per triangle); atomic
// collision/overwrite behavior for all three export formats; the missing-source and
// malformed-text typed rejections; and dispatcher-level unknown-arg rejection. Pure
// mesh data + file I/O under Saved/, so no RHI is needed.
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Containers/StringConv.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Geometry/GeometryTestHelpers.h"

#include "Editor.h"
#include "Engine/World.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#else
#include "HAL/PlatformFileManager.h"
#endif

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using GeometryTestHelpers::DestroyActorsWithLabel;

// File-local helpers. Uniquely named (MeshIO* prefix) so a Unity build merging this
// file's anonymous namespace with a sibling test's cannot ODR-collide.
namespace
{
    // Spawn a box DynamicMeshActor labeled Label (a known-good, closed manifold seed
    // carrying normals/UVs overlays). Returns true on success.
    bool MeshIOCreateBox(const FString& Label)
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("name"), Label);
        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("geometry.create_box"), Params, Capture))
        {
            return false;
        }
        return Capture.bSuccess;
    }

    // Delete a file the export verbs wrote (best-effort; keeps the disposable host tidy).
    void MeshIODeleteFile(const FString& Path)
    {
        if (!Path.IsEmpty())
        {
            IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true);
        }
    }

    bool MeshIOBytesEqual(const TArray<uint8>& A, const TArray<uint8>& B)
    {
        return A.Num() == B.Num()
            && (A.IsEmpty() || FMemory::Memcmp(A.GetData(), B.GetData(), A.Num()) == 0);
    }

    int32 MeshIOAtomicTempCount(const FString& FinalPath)
    {
        TArray<FString> TempNames;
        IFileManager::Get().FindFiles(TempNames, *(FinalPath + TEXT(".*.tmp")),
            /*Files=*/true, /*Directories=*/false);
        return TempNames.Num();
    }

    void MeshIODeleteAtomicTemps(const FString& FinalPath)
    {
        const FString Directory = FPaths::GetPath(FinalPath);
        TArray<FString> TempNames;
        IFileManager::Get().FindFiles(TempNames, *(FinalPath + TEXT(".*.tmp")),
            /*Files=*/true, /*Directories=*/false);
        for (const FString& TempName : TempNames)
        {
            MeshIODeleteFile(Directory / TempName);
        }
    }
}

// ============================================================================
// OBJ round-trip: export a box's mesh to OBJ text and re-import it into a new
// actor; the vertex and triangle counts must survive the round-trip unchanged.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryExportImportObjRoundTripTest,
    "PinWright.geometry.export_obj.RoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryExportImportObjRoundTripTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping OBJ round-trip test"));
        return true;
    }

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Src = FString::Printf(TEXT("PW_MeshIOObjSrc_%s"), *Guid);
    const FString Dst = FString::Printf(TEXT("PW_MeshIOObjDst_%s"), *Guid);

    if (!TestTrue(TEXT("box seed created"), MeshIOCreateBox(Src)))
    {
        DestroyActorsWithLabel(Src);
        return true;
    }

    // Export to OBJ text.
    TSharedPtr<FJsonObject> ExportParams = MakeShared<FJsonObject>();
    ExportParams->SetStringField(TEXT("actorName"), Src);
    ExportParams->SetBoolField(TEXT("returnText"), true);

    FTestResponseCapture ExportCap;
    TestTrue(TEXT("export_obj handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.export_obj"), ExportParams, ExportCap));
    TestTrue(TEXT("export_obj succeeded"), ExportCap.bSuccess);

    FString ObjText;
    FString ExportPath;
    double SrcV = 0.0, SrcT = 0.0;
    if (ExportCap.bSuccess && ExportCap.Result.IsValid())
    {
        ExportCap.Result->TryGetStringField(TEXT("text"), ObjText);
        ExportCap.Result->TryGetStringField(TEXT("path"), ExportPath);
        ExportCap.Result->TryGetNumberField(TEXT("vertexCount"), SrcV);
        ExportCap.Result->TryGetNumberField(TEXT("triangleCount"), SrcT);
    }
    TestTrue(TEXT("export returned non-empty OBJ text"), !ObjText.IsEmpty());
    TestTrue(TEXT("OBJ text carries vertex lines"), ObjText.Contains(TEXT("v ")));
    TestTrue(TEXT("OBJ text carries face lines"), ObjText.Contains(TEXT("f ")));
    TestTrue(TEXT("box has triangles"), static_cast<int32>(SrcT) > 0);

    // UV and normal lines, checked against the OBJ FORMAT rather than against the
    // exporter's own output. The count comparison below is a symmetric round-trip: it
    // exports and re-imports with this plugin's own pair of handlers, so any channel the
    // exporter silently stops writing is a channel the importer silently stops reading,
    // and v/t counts still agree perfectly. `vt`/`vn` are the only evidence in this test
    // that anything beyond positions and topology crossed the wire at all.
    // MeshIOHandler.cpp emits these under `if (bHasUVs)` / `if (bHasNormals)`, and the
    // create_box fixture carries both, so their absence means a real regression.
    TestTrue(TEXT("OBJ text carries UV lines (the box fixture has UVs)"),
        ObjText.Contains(TEXT("vt ")));
    TestTrue(TEXT("OBJ text carries normal lines (the box fixture has normals)"),
        ObjText.Contains(TEXT("vn ")));

    // Re-import the OBJ text into a new actor.
    TSharedPtr<FJsonObject> ImportParams = MakeShared<FJsonObject>();
    ImportParams->SetStringField(TEXT("actorName"), Dst);
    ImportParams->SetStringField(TEXT("text"), ObjText);

    FTestResponseCapture ImportCap;
    TestTrue(TEXT("import_obj handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.import_obj"), ImportParams, ImportCap));
    TestTrue(TEXT("import_obj succeeded"), ImportCap.bSuccess);

    if (ImportCap.bSuccess && ImportCap.Result.IsValid())
    {
        double DstV = 0.0, DstT = 0.0;
        ImportCap.Result->TryGetNumberField(TEXT("vertexCount"), DstV);
        ImportCap.Result->TryGetNumberField(TEXT("triangleCount"), DstT);
        TestEqual(TEXT("OBJ round-trip preserves vertex count"),
            static_cast<int32>(DstV), static_cast<int32>(SrcV));
        TestEqual(TEXT("OBJ round-trip preserves triangle count"),
            static_cast<int32>(DstT), static_cast<int32>(SrcT));
    }

    MeshIODeleteFile(ExportPath);
    DestroyActorsWithLabel(Src);
    DestroyActorsWithLabel(Dst);
    return true;
}

// ============================================================================
// ASCII STL round-trip: export a box to ASCII STL text and re-import it. STL has
// no shared vertices, so the triangle count survives while the vertex count is
// 3 * triangleCount (no welding).
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryExportImportStlAsciiRoundTripTest,
    "PinWright.geometry.export_stl.AsciiRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryExportImportStlAsciiRoundTripTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping STL round-trip test"));
        return true;
    }

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Src = FString::Printf(TEXT("PW_MeshIOStlSrc_%s"), *Guid);
    const FString Dst = FString::Printf(TEXT("PW_MeshIOStlDst_%s"), *Guid);

    if (!TestTrue(TEXT("box seed created"), MeshIOCreateBox(Src)))
    {
        DestroyActorsWithLabel(Src);
        return true;
    }

    // Export to ASCII STL text (binary defaults to false).
    TSharedPtr<FJsonObject> ExportParams = MakeShared<FJsonObject>();
    ExportParams->SetStringField(TEXT("actorName"), Src);
    ExportParams->SetBoolField(TEXT("returnText"), true);

    FTestResponseCapture ExportCap;
    TestTrue(TEXT("export_stl handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.export_stl"), ExportParams, ExportCap));
    TestTrue(TEXT("export_stl succeeded"), ExportCap.bSuccess);

    FString StlText;
    FString ExportPath;
    FString Format;
    double SrcT = 0.0;
    if (ExportCap.bSuccess && ExportCap.Result.IsValid())
    {
        ExportCap.Result->TryGetStringField(TEXT("text"), StlText);
        ExportCap.Result->TryGetStringField(TEXT("path"), ExportPath);
        ExportCap.Result->TryGetStringField(TEXT("format"), Format);
        ExportCap.Result->TryGetNumberField(TEXT("triangleCount"), SrcT);
    }
    TestEqual(TEXT("default STL export is ASCII"), Format, FString(TEXT("ascii")));
    TestTrue(TEXT("export returned non-empty STL text"), !StlText.IsEmpty());
    TestTrue(TEXT("STL text carries facet lines"), StlText.Contains(TEXT("facet normal")));
    TestTrue(TEXT("box has triangles"), static_cast<int32>(SrcT) > 0);

    // Re-import the ASCII STL text into a new actor.
    TSharedPtr<FJsonObject> ImportParams = MakeShared<FJsonObject>();
    ImportParams->SetStringField(TEXT("actorName"), Dst);
    ImportParams->SetStringField(TEXT("text"), StlText);

    FTestResponseCapture ImportCap;
    TestTrue(TEXT("import_stl handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.import_stl"), ImportParams, ImportCap));
    TestTrue(TEXT("import_stl succeeded"), ImportCap.bSuccess);

    if (ImportCap.bSuccess && ImportCap.Result.IsValid())
    {
        double DstV = 0.0, DstT = 0.0;
        ImportCap.Result->TryGetNumberField(TEXT("vertexCount"), DstV);
        ImportCap.Result->TryGetNumberField(TEXT("triangleCount"), DstT);
        TestEqual(TEXT("STL round-trip preserves triangle count"),
            static_cast<int32>(DstT), static_cast<int32>(SrcT));
        TestEqual(TEXT("STL import emits 3 unwelded vertices per triangle"),
            static_cast<int32>(DstV), static_cast<int32>(SrcT) * 3);
    }

    MeshIODeleteFile(ExportPath);
    DestroyActorsWithLabel(Src);
    DestroyActorsWithLabel(Dst);
    return true;
}

// ============================================================================
// All mesh export formats share the same collision and atomic-publication contract.
// The locked-destination case exercises each production handler's publish-error mapping
// and preservation behavior. The shared writer's staged-source-lock test separately
// excludes an unsafe delete-before-move publisher.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryMeshExportAtomicOverwriteContractTest,
    "PinWright.geometry.MeshExport.AtomicOverwriteContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryMeshExportAtomicOverwriteContractTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping mesh export overwrite contract test"));
        return true;
    }

    struct FExportCase
    {
        const TCHAR* Name;
        const TCHAR* FileToken;
        const TCHAR* Handler;
        const TCHAR* Extension;
        bool bBinary;
    };

    const FExportCase ExportCases[] = {
        { TEXT("OBJ"), TEXT("obj"), TEXT("geometry.export_obj"), TEXT("obj"), false },
        { TEXT("ASCII STL"), TEXT("stl_ascii"), TEXT("geometry.export_stl"), TEXT("stl"), false },
        { TEXT("binary STL"), TEXT("stl_binary"), TEXT("geometry.export_stl"), TEXT("stl"), true }
    };

    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourceActor = FString::Printf(TEXT("PW_MeshIOAtomicSrc_%s"), *Guid);
    if (!TestTrue(TEXT("atomic export box seed created"), MeshIOCreateBox(SourceActor)))
    {
        DestroyActorsWithLabel(SourceActor);
        return true;
    }

    const TArray<uint8> SentinelBytes = {
        0x50, 0x57, 0x5F, 0x4D, 0x45, 0x53, 0x48, 0x5F,
        0x45, 0x58, 0x50, 0x4F, 0x52, 0x54, 0x5F, 0x53,
        0x45, 0x4E, 0x54, 0x49, 0x4E, 0x45, 0x4C
    };

    for (const FExportCase& ExportCase : ExportCases)
    {
        const FString Label = ExportCase.Name;
        const FString RelativePath = FString::Printf(
            TEXT("Saved/PinWright/Tests/MeshExportAtomic_%s_%s.%s"),
            *Guid, ExportCase.FileToken, ExportCase.Extension);
        const FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / RelativePath);
        const bool bIsStl = FCString::Strcmp(ExportCase.Handler, TEXT("geometry.export_stl")) == 0;

        auto MakeExportParams = [&]()
        {
            TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
            Params->SetStringField(TEXT("actorName"), SourceActor);
            Params->SetStringField(TEXT("filePath"), RelativePath);
            Params->SetBoolField(TEXT("returnText"), true);
            if (bIsStl)
            {
                Params->SetBoolField(TEXT("binary"), ExportCase.bBinary);
            }
            return Params;
        };

        MeshIODeleteFile(FullPath);
        MeshIODeleteAtomicTemps(FullPath);
        TestTrue(*FString::Printf(TEXT("%s: output directory created"), *Label),
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(FullPath), /*Tree=*/true));
        if (!TestTrue(*FString::Printf(TEXT("%s: sentinel seeded"), *Label),
                FFileHelper::SaveArrayToFile(SentinelBytes, *FullPath)))
        {
            MeshIODeleteFile(FullPath);
            continue;
        }

        FTestResponseCapture CollisionCapture;
        TestTrue(*FString::Printf(TEXT("%s: export handler registered"), *Label),
            InvokeHandlerWithCapture(ExportCase.Handler, MakeExportParams(), CollisionCapture));
        TestFalse(*FString::Printf(TEXT("%s: default export refuses collision"), *Label),
            CollisionCapture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s: collision -> ALREADY_EXISTS"), *Label),
            CollisionCapture.ErrorCode, FString(TEXT("ALREADY_EXISTS")));

        TArray<uint8> ActualBytes;
        TestTrue(*FString::Printf(TEXT("%s: sentinel remains readable after refusal"), *Label),
            FFileHelper::LoadFileToArray(ActualBytes, *FullPath));
        TestTrue(*FString::Printf(TEXT("%s: refusal preserves sentinel bytes"), *Label),
            MeshIOBytesEqual(ActualBytes, SentinelBytes));
        TestEqual(*FString::Printf(TEXT("%s: refusal cleans temporary file"), *Label),
            MeshIOAtomicTempCount(FullPath), 0);

        // Restore the fixture even when running against a regressed direct writer, so the
        // following assertions diagnose overwrite semantics independently.
        TestTrue(*FString::Printf(TEXT("%s: sentinel restored before overwrite"), *Label),
            FFileHelper::SaveArrayToFile(SentinelBytes, *FullPath));

        TSharedPtr<FJsonObject> OverwriteParams = MakeExportParams();
        OverwriteParams->SetBoolField(TEXT("overwrite"), true);
        FTestResponseCapture OverwriteCapture;
        TestTrue(*FString::Printf(TEXT("%s: overwrite handler registered"), *Label),
            InvokeHandlerWithCapture(ExportCase.Handler, OverwriteParams, OverwriteCapture));
        TestTrue(*FString::Printf(TEXT("%s: opted-in overwrite succeeds"), *Label),
            OverwriteCapture.bSuccess);

        ActualBytes.Reset();
        TestTrue(*FString::Printf(TEXT("%s: overwritten output is readable"), *Label),
            FFileHelper::LoadFileToArray(ActualBytes, *FullPath));
        TestFalse(*FString::Printf(TEXT("%s: overwrite replaces sentinel content"), *Label),
            MeshIOBytesEqual(ActualBytes, SentinelBytes));
        TestEqual(*FString::Printf(TEXT("%s: successful overwrite cleans temporary file"), *Label),
            MeshIOAtomicTempCount(FullPath), 0);

        if (OverwriteCapture.bSuccess && OverwriteCapture.Result.IsValid())
        {
            FString ReturnedPath;
            bool bReplaced = false;
            double VertexCount = 0.0;
            double TriangleCount = 0.0;
            TestTrue(*FString::Printf(TEXT("%s: response includes path"), *Label),
                OverwriteCapture.Result->TryGetStringField(TEXT("path"), ReturnedPath));
            TestEqual(*FString::Printf(TEXT("%s: response path is exact"), *Label),
                ReturnedPath, FullPath);
            TestTrue(*FString::Printf(TEXT("%s: response includes replaced"), *Label),
                OverwriteCapture.Result->TryGetBoolField(TEXT("replaced"), bReplaced));
            TestTrue(*FString::Printf(TEXT("%s: overwrite reports replaced=true"), *Label),
                bReplaced);
            TestTrue(*FString::Printf(TEXT("%s: response includes vertexCount"), *Label),
                OverwriteCapture.Result->TryGetNumberField(TEXT("vertexCount"), VertexCount));
            TestTrue(*FString::Printf(TEXT("%s: response includes triangleCount"), *Label),
                OverwriteCapture.Result->TryGetNumberField(TEXT("triangleCount"), TriangleCount));
            TestTrue(*FString::Printf(TEXT("%s: exported mesh has vertices"), *Label),
                VertexCount > 0.0);
            TestTrue(*FString::Printf(TEXT("%s: exported mesh has triangles"), *Label),
                TriangleCount > 0.0);

            if (ExportCase.bBinary)
            {
                FString Format;
                TestTrue(TEXT("binary STL: response includes format"),
                    OverwriteCapture.Result->TryGetStringField(TEXT("format"), Format));
                TestEqual(TEXT("binary STL: format remains binary"), Format, FString(TEXT("binary")));
                TestEqual(TEXT("binary STL: byte count matches STL triangle layout"),
                    ActualBytes.Num(), 84 + static_cast<int32>(TriangleCount) * 50);
                TestFalse(TEXT("binary STL: response does not inline text"),
                    OverwriteCapture.Result->HasField(TEXT("text")));
            }
            else
            {
                FString ReturnedText;
                TestTrue(*FString::Printf(TEXT("%s: response includes requested text"), *Label),
                    OverwriteCapture.Result->TryGetStringField(TEXT("text"), ReturnedText));
                const FTCHARToUTF8 Utf8(*ReturnedText);
                TArray<uint8> ReturnedTextBytes;
                ReturnedTextBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
                TestTrue(*FString::Printf(TEXT("%s: file is exact UTF-8 response text"), *Label),
                    MeshIOBytesEqual(ActualBytes, ReturnedTextBytes));

                if (bIsStl)
                {
                    FString Format;
                    TestTrue(TEXT("ASCII STL: response includes format"),
                        OverwriteCapture.Result->TryGetStringField(TEXT("format"), Format));
                    TestEqual(TEXT("ASCII STL: format remains ascii"), Format,
                        FString(TEXT("ascii")));
                }
            }
        }

        TestTrue(*FString::Printf(TEXT("%s: sentinel restored before publish failure"), *Label),
            FFileHelper::SaveArrayToFile(SentinelBytes, *FullPath));

#if PLATFORM_WINDOWS
        // Access 0 plus read/write sharing allows a direct CREATE_ALWAYS writer to open
        // the target, while withholding FILE_SHARE_DELETE makes the real atomic publisher
        // fail. This proves handler-level WRITE_FAILED propagation and preservation; the
        // utility regression's staged-source lock is what rules out delete-before-move.
        const HANDLE LockedDestination = CreateFileW(*FullPath, /*dwDesiredAccess=*/0,
            FILE_SHARE_READ | FILE_SHARE_WRITE, /*lpSecurityAttributes=*/nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, /*hTemplateFile=*/nullptr);
        const bool bLocked = LockedDestination != INVALID_HANDLE_VALUE;
#else
        // The real atomic publisher refuses a read-only destination (the MoveFileExW rule),
        // which proves the same handler-level WRITE_FAILED propagation and preservation.
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        const bool bLocked = PlatformFile.SetReadOnly(*FullPath, true);
#endif
        TestTrue(*FString::Printf(TEXT("%s: destination locked against replacement"), *Label),
            bLocked);
        if (bLocked)
        {
            TSharedPtr<FJsonObject> FailureParams = MakeExportParams();
            FailureParams->SetBoolField(TEXT("overwrite"), true);
            FTestResponseCapture FailureCapture;
            TestTrue(*FString::Printf(TEXT("%s: locked export handler registered"), *Label),
                InvokeHandlerWithCapture(ExportCase.Handler, FailureParams, FailureCapture));
            TestFalse(*FString::Printf(TEXT("%s: locked publish fails"), *Label),
                FailureCapture.bSuccess);
            TestEqual(*FString::Printf(TEXT("%s: publish failure -> WRITE_FAILED"), *Label),
                FailureCapture.ErrorCode, FString(TEXT("WRITE_FAILED")));
#if PLATFORM_WINDOWS
            CloseHandle(LockedDestination);
#else
            PlatformFile.SetReadOnly(*FullPath, false);
#endif

            ActualBytes.Reset();
            TestTrue(*FString::Printf(TEXT("%s: prior output readable after publish failure"), *Label),
                FFileHelper::LoadFileToArray(ActualBytes, *FullPath));
            TestTrue(*FString::Printf(TEXT("%s: publish failure preserves sentinel bytes"), *Label),
                MeshIOBytesEqual(ActualBytes, SentinelBytes));
            TestEqual(*FString::Printf(TEXT("%s: publish failure cleans temporary file"), *Label),
                MeshIOAtomicTempCount(FullPath), 0);
        }

        MeshIODeleteFile(FullPath);
        TSharedPtr<FJsonObject> FreshParams = MakeExportParams();
        FreshParams->SetBoolField(TEXT("overwrite"), true);
        FTestResponseCapture FreshCapture;
        TestTrue(*FString::Printf(TEXT("%s: fresh export handler registered"), *Label),
            InvokeHandlerWithCapture(ExportCase.Handler, FreshParams, FreshCapture));
        TestTrue(*FString::Printf(TEXT("%s: fresh export succeeds"), *Label),
            FreshCapture.bSuccess);
        if (FreshCapture.bSuccess && FreshCapture.Result.IsValid())
        {
            bool bReplaced = true;
            TestTrue(*FString::Printf(TEXT("%s: fresh response includes replaced"), *Label),
                FreshCapture.Result->TryGetBoolField(TEXT("replaced"), bReplaced));
            TestFalse(*FString::Printf(TEXT("%s: fresh export reports replaced=false"), *Label),
                bReplaced);
        }

        MeshIODeleteFile(FullPath);
        MeshIODeleteAtomicTemps(FullPath);
    }

    DestroyActorsWithLabel(SourceActor);
    return true;
}

// ============================================================================
// Exporting from a non-existent actor is a typed ACTOR_NOT_FOUND rejection.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryExportObjMissingSourceTest,
    "PinWright.geometry.export_obj.MissingSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryExportObjMissingSourceTest::RunTest(const FString& Parameters)
{
    if (!GEditor || !IsValid(GEditor->GetEditorWorldContext().World()))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping export missing-source test"));
        return true;
    }

    const FString Missing = FString::Printf(TEXT("PW_MeshIONoSuchActor_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), Missing);

    FTestResponseCapture Capture;
    TestTrue(TEXT("export_obj handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.export_obj"), Params, Capture));
    TestFalse(TEXT("missing source is rejected, not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("missing source -> ACTOR_NOT_FOUND"),
        Capture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));
    return true;
}

// ============================================================================
// Importing malformed OBJ text is a typed PARSE_FAILED rejection (no fake actor).
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryImportObjMalformedTest,
    "PinWright.geometry.import_obj.MalformedRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryImportObjMalformedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), TEXT("PW_MeshIOObjMalformedProbe"));
    Params->SetStringField(TEXT("text"), TEXT("this is not a valid obj file at all\njust prose"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("import_obj handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.import_obj"), Params, Capture));
    TestFalse(TEXT("malformed OBJ is rejected, not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("malformed OBJ -> PARSE_FAILED"),
        Capture.ErrorCode, FString(TEXT("PARSE_FAILED")));

    // Nothing should have been spawned; belt-and-braces cleanup if a regression spawns.
    DestroyActorsWithLabel(TEXT("PW_MeshIOObjMalformedProbe"));
    return true;
}

// ============================================================================
// Importing malformed STL text is a typed PARSE_FAILED rejection.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryImportStlMalformedTest,
    "PinWright.geometry.import_stl.MalformedRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryImportStlMalformedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), TEXT("PW_MeshIOStlMalformedProbe"));
    Params->SetStringField(TEXT("text"), TEXT("not an stl, no vertices here"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("import_stl handler registered"),
        InvokeHandlerWithCapture(TEXT("geometry.import_stl"), Params, Capture));
    TestFalse(TEXT("malformed STL is rejected, not a fake success"), Capture.bSuccess);
    TestEqual(TEXT("malformed STL -> PARSE_FAILED"),
        Capture.ErrorCode, FString(TEXT("PARSE_FAILED")));

    DestroyActorsWithLabel(TEXT("PW_MeshIOStlMalformedProbe"));
    return true;
}

// ============================================================================
// An unknown wire parameter is rejected by the dispatcher's param validation
// (UNKNOWN_PARAMS), which lives in ValidateHandlerParams upstream of the handler
// body. Required actorName is supplied so validation reaches the unknown-param branch.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryExportObjUnknownArgTest,
    "PinWright.geometry.export_obj.UnknownArgRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryExportObjUnknownArgTest::RunTest(const FString& Parameters)
{
    DispatcherTestHelpers::FSinkPtr Sink;
    FRpcDispatcher Dispatcher;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("actorName"), TEXT("PW_MeshIOUnknownArgProbe"));
    Params->SetBoolField(TEXT("bogusParam"), true);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("geometry.export_obj"),
        TEXT("req-meshio-unknown-arg"), Params, bSuccess, ErrorCode);

    TestFalse(TEXT("unknown parameter is rejected, not a fake success"), bSuccess);
    TestEqual(TEXT("unknown parameter -> UNKNOWN_PARAMS"),
        ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));
    return true;
}

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif
