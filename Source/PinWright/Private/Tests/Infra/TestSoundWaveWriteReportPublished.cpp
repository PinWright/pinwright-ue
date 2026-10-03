// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract test: every handler that writes a USoundWave through PwCreateSoundWaveAsset publishes
// the FPwSoundWaveWriteReport it gets back (board B-render-metasound-drops-write-report).
//
// The report is a required out-parameter (PwAudioExport.h) so a call site cannot forget to
// MEASURE routing and property preservation; nothing stopped a call site from measuring them and
// then not publishing them, which is what audio.synth.render_metasound shipped: routing never in
// the response, and a non-empty property diff computed and discarded beside verification.pass.
//
// The rule, per handler source file: the number of PwCreateSoundWaveAsset( calls must not exceed
// the number of PwAddSoundWaveWriteReport( calls plus PW_SOUNDWAVE_WRITE_REPORT_BY_HAND markers.
// The marker is the opt-out for a verb that consumes the report by hand (audio.music.export_stems
// aggregates it across rows) and must sit at that call site with its reason. The two calls are
// counted in code only (NeutralizeSourceText blanks comments, so prose quoting a call does not
// count); the marker is counted in the RAW text, because it is written as a comment.
//
// Known limitation, shared with Tests/Core/TestNoParamHandlersReadNoArgs.cpp: a text heuristic at
// file granularity. A file with two writers and one publish fails correctly; a publish routed
// through a helper defined in another file would be missed.

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Tests/TestUtils.h"

namespace SoundWaveWriteReportScan
{
    int32 CountAnywhere(const FString& Contents, const TCHAR* Needle)
    {
        int32 Count = 0;
        for (int32 At = Contents.Find(Needle, ESearchCase::CaseSensitive); At != INDEX_NONE;
             At = Contents.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
        {
            ++Count;
        }
        return Count;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSoundWaveWriteReportPublishedTest,
    "PinWright.infra.contract.SoundWaveWriteReport.EveryWriterPublishesIt",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSoundWaveWriteReportPublishedTest::RunTest(const FString& Parameters)
{
    using namespace SoundWaveWriteReportScan;

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!TestTrue(TEXT("PinWright plugin resolves"), Plugin.IsValid()))
    {
        return false;
    }
    const FString HandlersDir = Plugin->GetBaseDir() / TEXT("Source") / TEXT("PinWright") /
        TEXT("Private") / TEXT("Handlers");
    if (!TestTrue(TEXT("handler source dir exists"), IFileManager::Get().DirectoryExists(*HandlersDir)))
    {
        return false;
    }

    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *HandlersDir, TEXT("*.cpp"), true, false);

    int32 WriterCalls = 0;
    for (const FString& File : Files)
    {
        FString Contents;
        if (!FFileHelper::LoadFileToString(Contents, *File))
        {
            AddError(FString::Printf(TEXT("could not read %s; an unread file is an unchecked one"),
                *File));
            continue;
        }
        const FString Code = NeutralizeSourceText(Contents);
        const int32 Writes = CountAnywhere(Code, TEXT("PwCreateSoundWaveAsset("));
        if (Writes == 0)
        {
            continue;
        }
        WriterCalls += Writes;
        const int32 Publishes = CountAnywhere(Code, TEXT("PwAddSoundWaveWriteReport(")) +
            CountAnywhere(Contents, TEXT("PW_SOUNDWAVE_WRITE_REPORT_BY_HAND"));
        TestTrue(FString::Printf(
            TEXT("%s: %d PwCreateSoundWaveAsset call(s) but only %d PwAddSoundWaveWriteReport call(s) "
                 "/ PW_SOUNDWAVE_WRITE_REPORT_BY_HAND marker(s) - publish the write report"),
            *FPaths::GetCleanFilename(File), Writes, Publishes), Publishes >= Writes);
    }

    // Non-vacuous: a moved directory or a renamed writer would otherwise pass with nothing checked.
    TestTrue(FString::Printf(TEXT("found the SoundWave writer call sites (%d)"), WriterCalls),
        WriterCalls >= 4);
    return true;
}
