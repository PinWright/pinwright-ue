// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for B-viewport-info-static-cast-crashes-editor-in-pie.
//
// THE DEFECT. Every handler that wanted the level-editor camera resolved it as
//
//     static_cast<FEditorViewportClient*>(GEditor->GetActiveViewport()->GetClient())
//
// FViewport::GetClient() returns an FViewportClient*, and while PIE runs inside the level
// viewport SLevelViewport::StartPlayInEditorSession swaps the active viewport for an
// FSceneViewport built over the UGameViewportClient
// (C:/UE_5.8/Engine/Source/Editor/LevelEditor/Private/SLevelViewport.cpp,
// `ActiveViewport = FSceneViewport::Create(PlayClient.Get(), ...)`). UGameViewportClient and
// FEditorViewportClient are SIBLINGS - they meet only at FCommonViewportClient - so the cast
// lands on the wrong branch, the `if (!Client)` guard beneath it is dead code (GetClient() is
// never null there), and FEditorViewportClient::GetViewLocation() then reads
// ViewTransformPerspective at an offset past the end of the game client's allocation. One
// read-only system.inspect.get_viewport_info call took the whole editor down with
// EXCEPTION_ACCESS_VIOLATION.
//
// WHY TWO TESTS. The crash itself needs a live PIE session inside the level viewport, which a
// synchronous automation test cannot stand up (and starting PIE mid-suite is what several other
// board tickets are about). So the coverage is split:
//
//  1. A SOURCE CONTRACT - the durable half. The fix is structural: nothing in the plugin may
//     downcast to an editor viewport client any more, because
//     EditorHandlerUtils::ResolveActiveLevelViewportClient() is the one sanctioned route and it
//     goes through FLevelEditorModule::GetFirstActiveViewport() ->
//     IAssetViewport::GetAssetViewportClient(), which the engine types for us. Reinstating any
//     of the seven removed casts fails this test by name and file.
//
//  2. A BEHAVIOURAL test of the property the crash violated: the resolver and the verb are
//     driven by the level-editor client, NOT by whatever client owns the active viewport, and
//     the response now says which viewport it measured. FViewport::SetPlayInEditorViewport() is
//     public, so the PIE discriminator can be exercised without a PIE session - the flag is set
//     and restored on the same stack, with nothing ticking in between. Note honestly what that
//     fixture can and cannot show: with no real PIE session the active viewport's client and the
//     level client are the same object (the engine's own check() in StartPlayInEditorSession
//     asserts exactly that outside PIE), so the camera-equality assertions are consistency
//     checks rather than crash reproductions. The assertion that fails on a revert is the
//     response shape: the pre-fix handler emitted no `pie`/`activeViewport` fields at all.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "HAL/FileManager.h"
#include "Handlers/Editor/EditorHandlerUtils.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UnrealClient.h"

// Uniquely named: Unity merges test TUs, so an anonymous-namespace helper here would ODR-clash
// with the identically shaped source-scanning helpers in the sibling Tests/*.cpp files.
namespace ViewportClientResolutionTestLocal
{
    // This file's own basename. It is excluded from the scan below because the forbidden
    // spellings appear here as pattern text; every other file in the tree is scanned.
    const TCHAR* ScannerFileName()
    {
        return TEXT("TestViewportClientResolution.cpp");
    }

    // One forbidden cast spelling. Whitespace-tolerant so `static_cast< FEditorViewportClient`
    // cannot slip past, `const` optional so a read-only spelling is caught too, and the C-style
    // form is listed because editor.set_camera carried
    // `(FEditorViewportClient *)GetActiveViewport()->GetClient()` - the same undefined behaviour
    // written differently. reinterpret_cast is deliberately NOT listed: the one site in the tree
    // (SentinelViewportClient() in Tests/Render/TestCaptureSubjectResolver.cpp) fabricates a
    // pointer that is only ever compared against null and never dereferenced, which is a different
    // thing entirely. A cast written through a `using` alias of the type evades this scan by
    // design - the scan is a tripwire on the idiom that actually recurred, not a type checker.
    struct FForbiddenCast
    {
        const TCHAR* Pattern;
        const TCHAR* Label;
    };

    const TArray<FForbiddenCast>& ForbiddenCasts()
    {
        static const TArray<FForbiddenCast> Casts = {
            {TEXT("static_cast\\s*<\\s*(const\\s+)?FEditorViewportClient"),
             TEXT("static_cast to FEditorViewportClient")},
            {TEXT("static_cast\\s*<\\s*(const\\s+)?FLevelEditorViewportClient"),
             TEXT("static_cast to FLevelEditorViewportClient")},
            {TEXT("\\(\\s*(const\\s+)?FEditorViewportClient\\s*\\*\\s*\\)"),
             TEXT("C-style cast to FEditorViewportClient*")},
            {TEXT("\\(\\s*(const\\s+)?FLevelEditorViewportClient\\s*\\*\\s*\\)"),
             TEXT("C-style cast to FLevelEditorViewportClient*")}
        };
        return Casts;
    }

    FString ResolveModuleSourceRoot()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid() ? (Plugin->GetBaseDir() / TEXT("Source")) : FString();
    }

    // Path relative to <Plugin>/Source, forward-slashed, so a failure names a file the reader can
    // open. Both sides go through ConvertRelativePathToFull first for the reason
    // TestErrorCodeRegistry documents: GetBaseDir() can hand back an engine-relative path while
    // FindFilesRecursive echoes the root it was given, and MakePathRelativeTo silently returns
    // its input unchanged when the two forms disagree.
    FString MakeSourceRelativePath(const FString& AnyPath, const FString& SourceDir)
    {
        FString Relative = FPaths::ConvertRelativePathToFull(AnyPath);
        const FString FullSourceDir = FPaths::ConvertRelativePathToFull(SourceDir) / TEXT("");
        FPaths::MakePathRelativeTo(Relative, *FullSourceDir);
        return Relative.Replace(TEXT("\\"), TEXT("/"));
    }

    int32 LineNumberAt(const FString& Text, int32 CharIndex)
    {
        int32 Line = 1;
        for (int32 i = 0; i < CharIndex && i < Text.Len(); ++i)
        {
            if (Text[i] == TEXT('\n'))
            {
                ++Line;
            }
        }
        return Line;
    }
}

// ============================================================================
// 1. Source contract: no raw downcast to an editor viewport client anywhere.
// ============================================================================
//
// The sanctioned resolution is EditorHandlerUtils::ResolveActiveLevelViewportClient(), which
// never touches FViewport::GetClient(). A new call site that needs the level-editor camera calls
// that; one that needs a DERIVED editor client (FLevelEditorViewportClient from an
// FEditorViewportClient the engine already typed) still may not spell it as a bare cast here -
// add the accessor to EditorHandlerUtils.h instead, so the unsafe idiom has exactly one place it
// could ever reappear and that place is reviewed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewportClientNoRawDowncastsTest,
    "PinWright.editor.viewport_client.NoRawViewportClientDowncasts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewportClientNoRawDowncastsTest::RunTest(const FString& Parameters)
{
    using namespace ViewportClientResolutionTestLocal;

    const FString SourceDir = ResolveModuleSourceRoot();
    if (!TestFalse(TEXT("PinWright source root resolves through IPluginManager"), SourceDir.IsEmpty()))
    {
        return false;
    }

    TArray<FString> SourceFiles;
    IFileManager::Get().FindFilesRecursive(SourceFiles, *SourceDir, TEXT("*.cpp"), true, false, false);
    IFileManager::Get().FindFilesRecursive(SourceFiles, *SourceDir, TEXT("*.h"), true, false, false);

    // A zero-file scan would pass vacuously, which is the failure mode this whole test exists to
    // avoid, so the file count is asserted before the findings are.
    TestTrue(TEXT("the scan actually read the plugin sources"), SourceFiles.Num() > 50);

    int32 Offences = 0;
    for (const FString& File : SourceFiles)
    {
        if (FPaths::GetCleanFilename(File).Equals(ScannerFileName(), ESearchCase::IgnoreCase))
        {
            continue;
        }

        FString RawContents;
        if (!FFileHelper::LoadFileToString(RawContents, *File))
        {
            continue;
        }
        // Whether a cast EXISTS is a fact about code, not about prose: the fix's own explanatory
        // comments name the pattern they removed, and scanning raw bytes would score those as
        // live call sites.
        const FString Contents = NeutralizeSourceText(RawContents);
        const FString Relative = MakeSourceRelativePath(File, SourceDir);

        for (const FForbiddenCast& Cast : ForbiddenCasts())
        {
            const FRegexPattern Pattern(Cast.Pattern);
            FRegexMatcher Matcher(Pattern, Contents);
            while (Matcher.FindNext())
            {
                ++Offences;
                AddError(FString::Printf(
                    TEXT("%s:%d - %s. FViewport::GetClient() hands back the GAME client while PIE ")
                    TEXT("runs in the level viewport, and that class is a sibling of ")
                    TEXT("FEditorViewportClient, so the cast reads past the end of the allocation ")
                    TEXT("and kills the editor. Use ")
                    TEXT("EditorHandlerUtils::ResolveActiveLevelViewportClient() instead."),
                    *Relative, LineNumberAt(Contents, Matcher.GetMatchBeginning()), Cast.Label));
            }
        }
    }

    TestEqual(TEXT("no raw downcast to an editor viewport client survives in the plugin"),
        Offences, 0);
    return true;
}

// ============================================================================
// 2. Behaviour: the camera is the level-editor client's, and the response says
//    which viewport it measured.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FViewportInfoCameraFromLevelClientTest,
    "PinWright.system.inspect.get_viewport_info.CameraComesFromTheLevelViewportClient",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FViewportInfoCameraFromLevelClientTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("system.inspect.get_viewport_info handler registered"),
        IsHandlerRegistered(TEXT("system.inspect.get_viewport_info")));

    FViewport* ActiveViewport = GEditor ? GEditor->GetActiveViewport() : nullptr;
    FEditorViewportClient* LevelClient = EditorHandlerUtils::ResolveActiveLevelViewportClient();
    if (!ActiveViewport || !LevelClient)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-level-viewport"),
            FString::Printf(TEXT("activeViewport=%s levelViewportClient=%s; the camera fields ")
                TEXT("and the PIE discriminator are unobservable without a live level viewport."),
                ActiveViewport ? TEXT("present") : TEXT("null"),
                LevelClient ? TEXT("present") : TEXT("null")));
        return true;
    }

    // The flag PIE stamps on the viewport it swaps in. Set and restored on this stack; no engine
    // tick runs in between, so nothing else observes the forced value.
    const bool bOriginalPieFlag = ActiveViewport->IsPlayInEditorViewport();
    ON_SCOPE_EXIT
    {
        ActiveViewport->SetPlayInEditorViewport(bOriginalPieFlag);
    };

    const bool PieFlags[] = {false, true};
    for (const bool bPieFlag : PieFlags)
    {
        const TCHAR* const Phase = bPieFlag ? TEXT("pie-flag-set") : TEXT("pie-flag-clear");
        ActiveViewport->SetPlayInEditorViewport(bPieFlag);

        TestTrue(*FString::Printf(TEXT("%s: the discriminator reports the viewport's own flag"), Phase),
            EditorHandlerUtils::IsActiveViewportPlayInEditor() == bPieFlag);
        // The property the crash violated: resolution does not follow the active viewport.
        TestTrue(*FString::Printf(TEXT("%s: the resolver still returns the level-editor client"), Phase),
            EditorHandlerUtils::ResolveActiveLevelViewportClient() == LevelClient);

        FTestResponseCapture Capture;
        TestTrue(*FString::Printf(TEXT("%s: handler found"), Phase),
            InvokeHandlerWithCapture(TEXT("system.inspect.get_viewport_info"),
                MakeShared<FJsonObject>(), Capture));
        TestTrue(*FString::Printf(TEXT("%s: handler reports success"), Phase), Capture.bSuccess);
        if (!Capture.Result.IsValid())
        {
            AddError(FString::Printf(TEXT("%s: handler sent no result object"), Phase));
            continue;
        }

        bool bReportedPie = !bPieFlag;
        TestTrue(*FString::Printf(TEXT("%s: response carries pie"), Phase),
            Capture.Result->TryGetBoolField(TEXT("pie"), bReportedPie));
        TestTrue(*FString::Printf(TEXT("%s: pie matches the measured viewport"), Phase),
            bReportedPie == bPieFlag);

        FString ReportedViewport;
        TestTrue(*FString::Printf(TEXT("%s: response names the active viewport"), Phase),
            Capture.Result->TryGetStringField(TEXT("activeViewport"), ReportedViewport));
        TestEqual(*FString::Printf(TEXT("%s: activeViewport names the right viewport"), Phase),
            ReportedViewport,
            bPieFlag ? FString(TEXT("pieGameViewport")) : FString(TEXT("levelEditorViewport")));

        const TSharedPtr<FJsonObject>* CameraLocation = nullptr;
        if (TestTrue(*FString::Printf(TEXT("%s: response carries cameraLocation"), Phase),
                Capture.Result->TryGetObjectField(TEXT("cameraLocation"), CameraLocation))
            && CameraLocation != nullptr && (*CameraLocation).IsValid())
        {
            const FVector Expected = LevelClient->GetViewLocation();
            TestEqual(*FString::Printf(TEXT("%s: cameraLocation.x is the editor camera's"), Phase),
                (*CameraLocation)->GetNumberField(TEXT("x")), Expected.X);
            TestEqual(*FString::Printf(TEXT("%s: cameraLocation.y is the editor camera's"), Phase),
                (*CameraLocation)->GetNumberField(TEXT("y")), Expected.Y);
            TestEqual(*FString::Printf(TEXT("%s: cameraLocation.z is the editor camera's"), Phase),
                (*CameraLocation)->GetNumberField(TEXT("z")), Expected.Z);
        }

        double ReportedFov = 0.0;
        TestTrue(*FString::Printf(TEXT("%s: response carries fov"), Phase),
            Capture.Result->TryGetNumberField(TEXT("fov"), ReportedFov));
        TestEqual(*FString::Printf(TEXT("%s: fov is the editor camera's"), Phase),
            ReportedFov, static_cast<double>(LevelClient->ViewFOV));
    }

    return true;
}
