// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Utils/AssetDumpWriter.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"


#include "Tests/Widget/WidgetTestFixtures.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "WidgetBlueprint.h"
#include "Tests/TestUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace
{
    bool HasPngMagic(const TArray<uint8>& Bytes)
    {
        // Standard PNG signature: 89 50 4E 47 0D 0A 1A 0A
        static const uint8 Magic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
        if (Bytes.Num() < 8) return false;
        for (int32 I = 0; I < 8; ++I)
        {
            if (Bytes[I] != Magic[I]) return false;
        }
        return true;
    }

    bool LoadFileBytes(const FString& Path, TArray<uint8>& OutBytes)
    {
        return FFileHelper::LoadFileToArray(OutBytes, *Path);
    }

    bool ContainsPngError(const TArray<TPair<FString, FString>>& FileErrors)
    {
        for (const TPair<FString, FString>& Pair : FileErrors)
        {
            if (Pair.Key == FString(DumpFileNames::WidgetPreviewPng))
            {
                return true;
            }
        }
        return false;
    }

    bool ContainsPngWritten(const TArray<FString>& WrittenPaths)
    {
        for (const FString& Path : WrittenPaths)
        {
            if (Path.EndsWith(DumpFileNames::WidgetPreviewPng))
            {
                return true;
            }
        }
        return false;
    }

    void CleanupDumpRoot(const FString& Root)
    {
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    }

    void CloseAndCleanup(UWidgetBlueprint* WBP, const FString& AssetPath)
    {
        if (GEditor)
        {
            if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                if (WBP)
                {
                    AssetEditorSubsystem->CloseAllEditorsForAsset(WBP);
                }
            }
        }
        if (WBP)
        {
            if (UPackage* Package = WBP->GetOutermost())
            {
                Package->SetDirtyFlag(false);
            }
        }
        CleanupTestAsset(AssetPath);
    }
}

// Counterfactual: if the new dispatch in BuildAllFilesForAsset's WBP branch is
// reverted to skip the screenshot regardless of the flag, this test fails because
// preview.png never appears in WrittenPaths.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWidgetScreenshotOptInProducesPngTest,
    "PinWright.asset.dump.WidgetScreenshot.OptInProducesPng",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWidgetScreenshotOptInProducesPngTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(
        TEXT("WBP_AssetDumpScreenshotOptIn"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(AssetPath);
    if (!TestNotNull(TEXT("widget blueprint allocated"), WBP))
    {
        return false;
    }
    if (!TestNotNull(TEXT("preview label added"),
            WidgetTestFixtures::AddSizedPreviewLabel(WBP)))
    {
        CloseAndCleanup(WBP, AssetPath);
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(WBP);

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpWidgetScreenshot") / Suffix;

    ON_SCOPE_EXIT
    {
        CleanupDumpRoot(ScratchRoot);
        CloseAndCleanup(WBP, AssetPath);
    };

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"),
        *AssetPath, *FPackageName::GetLongPackageAssetName(AssetPath));
    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false,
            /*bIncludeWidgetScreenshot=*/true);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());
    TestTrue(TEXT("preview.png in WrittenPaths"), ContainsPngWritten(Result.WrittenPaths));
    TestFalse(TEXT("no preview.png error recorded"), ContainsPngError(Result.FileErrors));

    const FString PngPath = Result.DumpDir / DumpFileNames::WidgetPreviewPng;
    TestTrue(TEXT("preview.png exists on disk"),
        IFileManager::Get().FileExists(*PngPath));

    TArray<uint8> PngBytes;
    TestTrue(TEXT("preview.png readable"), LoadFileBytes(PngPath, PngBytes));
    TestTrue(TEXT("preview.png non-empty"), PngBytes.Num() > 0);
    TestTrue(TEXT("preview.png has PNG magic"), HasPngMagic(PngBytes));

    return true;
}

// Counterfactual: if dispatch happens unconditionally (ignoring the
// includeWidgetScreenshot flag), this test fails because preview.png appears in
// WrittenPaths despite the default-args call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWidgetScreenshotDefaultOmitsPngTest,
    "PinWright.asset.dump.WidgetScreenshot.DefaultOmitsPng",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWidgetScreenshotDefaultOmitsPngTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(
        TEXT("WBP_AssetDumpScreenshotDefault"));
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(AssetPath);
    if (!TestNotNull(TEXT("widget blueprint allocated"), WBP))
    {
        return false;
    }
    WidgetTestFixtures::AddSizedPreviewLabel(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpWidgetScreenshot") / Suffix;

    ON_SCOPE_EXIT
    {
        CleanupDumpRoot(ScratchRoot);
        CloseAndCleanup(WBP, AssetPath);
    };

    // Default args: bIncludeWidgetScreenshot defaults to false.
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"),
        *AssetPath, *FPackageName::GetLongPackageAssetName(AssetPath));
    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot);

    TestTrue(TEXT("Dump succeeds"), Result.ErrorCode.IsEmpty());
    TestFalse(TEXT("preview.png NOT in WrittenPaths"),
        ContainsPngWritten(Result.WrittenPaths));
    TestFalse(TEXT("no preview.png error recorded"),
        ContainsPngError(Result.FileErrors));

    const FString PngPath = Result.DumpDir / DumpFileNames::WidgetPreviewPng;
    TestFalse(TEXT("preview.png does not exist on disk"),
        IFileManager::Get().FileExists(*PngPath));

    return true;
}

// Counterfactual: if the new dispatch silently swallows the capture failure
// (returns without RecordAspectDiagnostic), this test fails because FileErrors
// stays empty even though the capture could not produce a PNG.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpWidgetScreenshotFailureRecordsDiagnosticTest,
    "PinWright.asset.dump.WidgetScreenshot.FailureRecordsDiagnostic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpWidgetScreenshotFailureRecordsDiagnosticTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = WidgetTestFixtures::MakeWidgetDesignerScreenshotAssetPath(
        TEXT("WBP_AssetDumpScreenshotFailure"));
    // Build a normal compilable WBP and then pre-open it with FSimpleAssetEditor.
    // The generic editor's toolkit FName is "GenericAssetEditor", which does not match
    // the "WidgetBlueprintEditor" name FWidgetGeometryResolver::FindWidgetBlueprintEditor
    // requires, so CapturePreviewToPng bails out at EDITOR_NOT_FOUND — the deterministic
    // failure path AssetDumpHandler's diagnostic dispatch must surface.
    UWidgetBlueprint* WBP = WidgetTestFixtures::MakeWidgetDesignerScreenshotBlueprint(AssetPath);
    if (!TestNotNull(TEXT("widget blueprint allocated"), WBP))
    {
        return false;
    }
    FKismetEditorUtilities::CompileBlueprint(WBP);
    // Claim the asset's editor slot with a generic editor BEFORE invoking the dump —
    // OpenEditorForAsset early-returns when an editor is already open for the asset,
    // so the WBP editor never spawns and the lookup deterministically fails.
    WidgetTestFixtures::PreemptWidgetBlueprintEditorWithGenericEditor(WBP);

    const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString ScratchRoot = FPaths::ProjectIntermediateDir()
        / TEXT("AssetDumpWidgetScreenshot") / Suffix;

    ON_SCOPE_EXIT
    {
        CleanupDumpRoot(ScratchRoot);
        CloseAndCleanup(WBP, AssetPath);
    };

    const FString ObjectPath = FString::Printf(TEXT("%s.%s"),
        *AssetPath, *FPackageName::GetLongPackageAssetName(AssetPath));
    AssetDumpHandler::FDumpSingleResult Result =
        AssetDumpHandler::DumpSingleAsset(ObjectPath, ScratchRoot, /*bDiff=*/false,
            /*bIncludeWidgetScreenshot=*/true);

    TestTrue(TEXT("Dump succeeds at top level"), Result.ErrorCode.IsEmpty());
    TestFalse(TEXT("preview.png NOT in WrittenPaths"),
        ContainsPngWritten(Result.WrittenPaths));
    TestTrue(TEXT("preview.png error recorded"), ContainsPngError(Result.FileErrors));

    // Other text aspects must still land — capture failure never blocks them.
    bool bSawTreeXml = false;
    bool bSawProperties = false;
    bool bSawMeta = false;
    for (const FString& Path : Result.WrittenPaths)
    {
        if (Path.EndsWith(DumpFileNames::TreeXml))    { bSawTreeXml = true; }
        if (Path.EndsWith(DumpFileNames::Properties)) { bSawProperties = true; }
        if (Path.EndsWith(DumpFileNames::Meta))       { bSawMeta = true; }
    }
    TestTrue(TEXT("tree.xml still written"), bSawTreeXml);
    TestTrue(TEXT("properties.json still written"), bSawProperties);
    TestTrue(TEXT("meta.json still written"), bSawMeta);

    // Confirm the diagnostic carries a non-empty reason — empty would let consumers
    // miss why the file is absent.
    bool bReasonNonEmpty = false;
    for (const TPair<FString, FString>& Pair : Result.FileErrors)
    {
        if (Pair.Key == FString(DumpFileNames::WidgetPreviewPng))
        {
            bReasonNonEmpty = !Pair.Value.IsEmpty();
            break;
        }
    }
    TestTrue(TEXT("preview.png diagnostic has non-empty reason"), bReasonNonEmpty);

    return true;
}
