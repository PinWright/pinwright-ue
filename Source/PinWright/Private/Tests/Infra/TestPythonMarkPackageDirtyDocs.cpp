// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-python-cannot-mark-package-dirty.
//
// The cost of this gap was never the missing code - it was the missing sentence.
// Several agent briefs told agents to work around a missing MarkPackageDirty() by
// "dirtying the package from python.execute", which is not possible on UE 5.8:
// UObjectBaseUtility::MarkPackageDirty (UObjectBaseUtility.h:527) and
// UPackage::SetDirtyFlag (Package.h:649) carry no UFUNCTION, and PyGenUtil::
// IsScriptExposedFunction (PyGenUtil.cpp:1615) exports only BlueprintCallable /
// BlueprintEvent functions. Scripts written to that advice silently did nothing.
//
// So the python overlay must keep saying four things, and this test fails if any of
// them is deleted: (1) the engine names people search for are absent on 5.8,
// (2) unreal.PinWrightPackageLibrary is the replacement, (3) there is a read-back
// call to assert the dirty actually took, and (4) the two engine functions that DO
// reach the flag exist, so nobody re-derives the wrong "there is no workaround at
// all" conclusion recorded in the ticket's first history entry.
//
// Rendered through WikiHandler::RenderPage - the same entry the HTTP gateway serves
// doc requests from - so this exercises the real overlay loader and renderer rather
// than a copy of the markdown.
#include "Misc/AutomationTest.h"
#include "PinWrightPackageLibrary.h"
#include "Tests/Infra/WikiDocTestHelpers.h"
#include "UObject/Class.h"

// ============================================================================
// The python namespace page documents the 5.8 gap and its supported replacement.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonMarkPackageDirtyDocTest,
    "PinWright.infra.wiki_handler.NamespacePage.PythonMarkPackageDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPythonMarkPackageDirtyDocTest::RunTest(const FString& Parameters)
{
    FString PythonText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("python"), PythonText))
    {
        return false;
    }

    // (1) The two engine spellings a reader will search for are named, so a grep for
    // either lands on the page that explains they do not exist here.
    TestTrue(TEXT("python page names the absent engine mark_package_dirty"),
        PythonText.Contains(TEXT("mark_package_dirty")));
    TestTrue(TEXT("python page names the absent engine set_dirty_flag"),
        PythonText.Contains(TEXT("set_dirty_flag")));

    // (2) The replacement, by its exact Python-visible name.
    TestTrue(TEXT("python page names unreal.PinWrightPackageLibrary as the replacement"),
        PythonText.Contains(TEXT("PinWrightPackageLibrary")));
    TestTrue(TEXT("python page names the reflected wrapper with an asset argument"),
        PythonText.Contains(TEXT("unreal.PinWrightPackageLibrary.mark_package_dirty(asset)")));
    TestTrue(TEXT("python page documents the blocker diagnostic that turns False into a reason"),
        PythonText.Contains(TEXT("describe_mark_dirty_blocker")));

    // (3) The read-back. A doc that showed only the mutation would teach the exact
    // trust-the-return-value habit that hid this class of bug.
    TestTrue(TEXT("python page shows the is_package_dirty_by_path read-back"),
        PythonText.Contains(TEXT("is_package_dirty_by_path")));

    TestTrue(TEXT("python page names the unavailable asset.mark_package_dirty engine call"),
        PythonText.Contains(TEXT("asset.mark_package_dirty()")));
    TestTrue(TEXT("python page explicitly rejects the unavailable engine call"),
        PythonText.Contains(TEXT("does not exist")));
    TestTrue(TEXT("python page routes callers to asset.mark_dirty"),
        PythonText.Contains(TEXT("asset.mark_dirty")));
    TestTrue(TEXT("python page shows a loaded asset dirty-state read-back"),
        PythonText.Contains(TEXT("is_package_dirty(asset)")));
    TestTrue(TEXT("python page names the bundled force-save path"),
        PythonText.Contains(TEXT("save_asset(asset_path, only_if_is_dirty=False)")));
    TestTrue(TEXT("python page requires a clean package after saving"),
        PythonText.Contains(TEXT("not lib.is_package_dirty(asset)")));

    TArray<FString> PythonLines;
    PythonText.ParseIntoArrayLines(PythonLines, false);
    bool bInCodeBlock = false;
    bool bLegacyGuidanceNamed = false;
    bool bLegacyGuidanceInCode = false;
    bool bLegacyGuidanceMarkedUnavailable = false;
    for (const FString& Line : PythonLines)
    {
        const FString Trimmed = Line.TrimStartAndEnd();
        if (Trimmed.StartsWith(TEXT("```")))
        {
            bInCodeBlock = !bInCodeBlock;
            continue;
        }
        if (Line.Contains(TEXT("asset.mark_package_dirty()")))
        {
            bLegacyGuidanceNamed = true;
            bLegacyGuidanceInCode |= bInCodeBlock;
            bLegacyGuidanceMarkedUnavailable |=
                Line.Contains(TEXT("does not exist")) ||
                Line.Contains(TEXT("not exposed")) ||
                Line.Contains(TEXT("unavailable"));
        }
    }
    TestTrue(TEXT("legacy asset.mark_package_dirty is explicitly marked unavailable"),
        bLegacyGuidanceNamed && bLegacyGuidanceMarkedUnavailable);
    TestFalse(TEXT("legacy asset.mark_package_dirty is not prescribed in executable Python"),
        bLegacyGuidanceInCode);

    // (4) The engine routes that do work. Recorded so the "no Python-side workaround
    // exists at all" conclusion is not re-derived by the next reader.
    TestTrue(TEXT("python page names the EditorAssetSubsystem.set_dirty_flag engine route"),
        PythonText.Contains(TEXT("EditorAssetSubsystem")));
    TestTrue(TEXT("python page names the SystemLibrary.transact_object engine route"),
        PythonText.Contains(TEXT("transact_object")));

    return true;
}

// ============================================================================
// The replacement must remain a real reflected Python surface, not documentation
// for a C++-only helper.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonMarkPackageDirtySurfaceTest,
    "PinWright.infra.python.PackageDirtyLibrary.IsReflectedAndCallable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPythonMarkPackageDirtySurfaceTest::RunTest(const FString& Parameters)
{
    UClass* LibraryClass = UPinWrightPackageLibrary::StaticClass();
    if (!TestNotNull(TEXT("PinWright package library class is reflected"), LibraryClass))
    {
        return false;
    }

    const TArray<FName> PythonSurfaceFunctions = {
        TEXT("MarkPackageDirty"),
        TEXT("MarkActorPackageDirty"),
        TEXT("MarkPackageDirtyByPath"),
        TEXT("IsPackageDirty"),
        TEXT("IsPackageDirtyByPath"),
        TEXT("DescribeMarkDirtyBlocker"),
        TEXT("GetPackageName")};

    for (const FName FunctionName : PythonSurfaceFunctions)
    {
        UFunction* Function = LibraryClass->FindFunctionByName(FunctionName);
        const FString Label = FString::Printf(
            TEXT("%s is discoverable as a reflected function"), *FunctionName.ToString());
        TestNotNull(*Label, Function);
        if (Function)
        {
            TestTrue(*FString::Printf(
                TEXT("%s is BlueprintCallable for Python export"), *FunctionName.ToString()),
                Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
            TestTrue(*FString::Printf(
                TEXT("%s is static on the function library"), *FunctionName.ToString()),
                Function->HasAnyFunctionFlags(FUNC_Static));
        }
    }

    return true;
}

// ============================================================================
// The MCP twin: asset.mark_dirty's method page carries its overlay Notes.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetMarkDirtyMethodDocTest,
    "PinWright.infra.wiki_handler.MethodPage.AssetMarkDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetMarkDirtyMethodDocTest::RunTest(const FString& Parameters)
{
    FString MethodText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("asset.mark_dirty"), MethodText))
    {
        return false;
    }

    // "wasDirty" appears only in the `### asset.mark_dirty` overlay H3, never in the
    // registered macro summary, so this passes only when LoadMethodSection surfaces
    // the asset.md overlay section under `## Notes`.
    TestTrue(TEXT("asset.mark_dirty method page documents the wasDirty readback field"),
        MethodText.Contains(TEXT("wasDirty")));

    // Same argument for the python.md cross-link: overlay-only.
    TestTrue(TEXT("asset.mark_dirty method page cross-links the python overlay"),
        MethodText.Contains(TEXT("python.md")));

    // --- The canonical save recipe routes callers here instead of a force-save. ---
    FString RecipeText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("safe-mutation-save"), RecipeText))
    {
        return false;
    }

    TestTrue(TEXT("safe-mutation-save names asset.mark_dirty for the clean-package case"),
        RecipeText.Contains(TEXT("asset.mark_dirty")));
    TestTrue(TEXT("safe-mutation-save points Python callers at PinWrightPackageLibrary"),
        RecipeText.Contains(TEXT("PinWrightPackageLibrary")));
    TestTrue(TEXT("asset.mark_dirty page requires a clean package after saving"),
        MethodText.Contains(TEXT("isDirty:false")));
    TestTrue(TEXT("safe-mutation-save names the bundled Python force-save"),
        RecipeText.Contains(TEXT("save_asset(path, only_if_is_dirty=False)")));
    TestTrue(TEXT("safe-mutation-save requires Python dirty-state read-back"),
        RecipeText.Contains(TEXT("is_package_dirty(asset)")));

    return true;
}
