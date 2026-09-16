// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for the drive.observe screenshot file-delivery mode
// (F-drive-observe-screenshot-inline-base64). screenshot_mode="file" must route the encoded
// PNG to a written path and OMIT the inline base64 (so the observation payload stays small
// and the marked image is directly Read-able), while the default inline mode keeps base64
// and emits no path. Exercises the production delivery seam
// FDriveSetOfMarkRenderer::DeliverScreenshotBytes plus the FDriveJson::WriteObservation
// serialization contract with an in-code synthetic byte buffer (no live viewport, no example
// asset), so it fails if either the file-write branch or the path-vs-base64 serialization is
// reverted. The live-capture path (CaptureAnnotated) needs a real viewport and is not
// exercised here.

#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"

#include "Handlers/Drive/DriveSetOfMarkRenderer.h"
#include "Handlers/Drive/DriveJson.h"
#include "Handlers/Drive/DriveTypes.h"

namespace
{
    // A short, non-empty stand-in for encoded PNG bytes. Delivery never decodes it, so real
    // PNG structure is unnecessary; the leading bytes are the PNG signature for realism.
    TArray<uint8> MakeFakePngBytes()
    {
        const uint8 Raw[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3, 4, 5, 6, 7, 8 };
        TArray<uint8> Data;
        Data.Append(Raw, UE_ARRAY_COUNT(Raw));
        return Data;
    }

    // Serialize an observation carrying only Screenshot and hand back its nested `screenshot`
    // object, so the test asserts the exact wire shape callers receive.
    TSharedPtr<FJsonObject> SerializeScreenshotObject(const FDriveScreenshot& Screenshot)
    {
        FDriveObservation Obs;
        Obs.Surface = EDriveSurface::Game;
        Obs.Screenshot = Screenshot;

        TSharedPtr<FJsonObject> Root = FDriveJson::WriteObservation(Obs);
        if (!Root.IsValid())
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* ScreenshotObj = nullptr;
        if (Root->TryGetObjectField(TEXT("screenshot"), ScreenshotObj) && ScreenshotObj)
        {
            return *ScreenshotObj;
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveScreenshotFileDeliveryTest,
    "PinWright.drive.somrender.FileDeliveryReturnsPathNotBase64",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FDriveScreenshotFileDeliveryTest::RunTest(const FString& Parameters)
{
    const TArray<uint8> Png = MakeFakePngBytes();

    // --- File mode: bytes go to disk; Path is set and inline Base64 is omitted. ---
    FDriveScreenshot FileShot;
    FString FileErr;
    const bool bFileOk =
        FDriveSetOfMarkRenderer::DeliverScreenshotBytes(Png, /*bWriteToFile=*/true, FileShot, FileErr);
    TestTrue(TEXT("file delivery succeeds"), bFileOk);
    TestTrue(TEXT("file delivery sets no error"), FileErr.IsEmpty());
    TestFalse(TEXT("file mode populates a path"), FileShot.Path.IsEmpty());
    TestTrue(TEXT("file mode omits inline base64"), FileShot.Base64.IsEmpty());

    // The written file exists and holds exactly the supplied bytes.
    if (!FileShot.Path.IsEmpty())
    {
        TestTrue(TEXT("written screenshot file exists"), FPaths::FileExists(FileShot.Path));
        TArray<uint8> ReadBack;
        const bool bRead = FFileHelper::LoadFileToArray(ReadBack, *FileShot.Path);
        TestTrue(TEXT("written file is readable"), bRead);
        TestTrue(TEXT("written bytes round-trip exactly"), ReadBack == Png);
    }

    // Serialization: file mode emits `path` and no `base64`.
    const TSharedPtr<FJsonObject> FileJson = SerializeScreenshotObject(FileShot);
    if (TestNotNull(TEXT("file-mode screenshot serializes"), FileJson.Get()))
    {
        TestTrue(TEXT("json carries path in file mode"), FileJson->HasField(TEXT("path")));
        TestFalse(TEXT("json omits base64 in file mode"), FileJson->HasField(TEXT("base64")));
        TestEqual(TEXT("json path matches written path"),
            FileJson->GetStringField(TEXT("path")), FileShot.Path);
    }

    // Clean up the artifact this test wrote.
    if (!FileShot.Path.IsEmpty())
    {
        IFileManager::Get().Delete(*FileShot.Path);
    }

    // --- Inline mode (default): Base64 encodes the bytes; Path stays empty. ---
    FDriveScreenshot InlineShot;
    FString InlineErr;
    const bool bInlineOk =
        FDriveSetOfMarkRenderer::DeliverScreenshotBytes(Png, /*bWriteToFile=*/false, InlineShot, InlineErr);
    TestTrue(TEXT("inline delivery succeeds"), bInlineOk);
    TestTrue(TEXT("inline delivery sets no error"), InlineErr.IsEmpty());
    TestEqual(TEXT("inline base64 encodes the bytes"), InlineShot.Base64, FBase64::Encode(Png));
    TestTrue(TEXT("inline mode sets no path"), InlineShot.Path.IsEmpty());

    // Serialization: inline mode emits `base64` and no `path`.
    const TSharedPtr<FJsonObject> InlineJson = SerializeScreenshotObject(InlineShot);
    if (TestNotNull(TEXT("inline-mode screenshot serializes"), InlineJson.Get()))
    {
        TestTrue(TEXT("json carries base64 in inline mode"), InlineJson->HasField(TEXT("base64")));
        TestFalse(TEXT("json omits path in inline mode"), InlineJson->HasField(TEXT("path")));
    }

    return true;
}
