// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Tests/AutomationSuiteMaintenance.h"

#if WITH_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PinWrightSettings.h"
#include "ShaderCompiler.h"
#include "SoundWaveCompiler.h"
#include "Tests/TestUtils.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogPinWrightSuiteMaintenance, Log, All);

namespace PinWrightSuiteMaintenance
{
namespace
{
    constexpr int32 UnsetInterval = MIN_int32;
    constexpr float UnsetFraction = -1.0f;

    int32 GTestGcEvery = UnsetInterval;
    FAutoConsoleVariableRef CVarTestGcEvery(
        TEXT("pinwright.TestGcEvery"),
        GTestGcEvery,
        TEXT("Reset the PinWright automation suite every N completed tests; values <= 0 disable it."));

    float GTestMemoryWatermark = UnsetFraction;
    FAutoConsoleVariableRef CVarTestMemoryWatermark(
        TEXT("pinwright.TestMemoryWatermark"),
        GTestMemoryWatermark,
        TEXT("Force a PinWright suite reset once the working set reaches this fraction of ")
        TEXT("physical RAM; values <= 0 disable the watermark trigger."));

    float GTestMemoryHardFraction = UnsetFraction;
    FAutoConsoleVariableRef CVarTestMemoryHardFraction(
        TEXT("pinwright.TestMemoryHardFraction"),
        GTestMemoryHardFraction,
        TEXT("Log PINWRIGHT_MEMORY_WATERMARK_EXCEEDED when the working set is still at this ")
        TEXT("fraction of physical RAM after a suite reset; values <= 0 disable the escalation."));

    const TCHAR* const GScratchRootPackagePath = TEXT("/Game/PinWrightTests");

    FDelegateHandle TestEndHandle;

    // Last PinWright test id seen by OnTestEnd. The escalation names it because the reset runs
    // BETWEEN tests: without it the log line points at nothing and the reader has to guess which
    // test's teardown left the memory behind.
    FString GLastCompletedTestId;

    bool GMemorySampleOverrideEnabled = false;
    FMemorySample GMemorySampleOverride;

    int32 GetCommandLineInterval()
    {
        static bool bParsed = false;
        static int32 CommandLineInterval = UnsetInterval;

        if (!bParsed)
        {
            bParsed = true;
            FParse::Value(
                FCommandLine::Get(), TEXT("PinWrightTestGcEvery="), CommandLineInterval);
        }

        return CommandLineInterval;
    }

    float GetCommandLineFraction(const TCHAR* Switch, float& CachedValue, bool& bParsed)
    {
        if (!bParsed)
        {
            bParsed = true;
            FParse::Value(FCommandLine::Get(), Switch, CachedValue);
        }

        return CachedValue;
    }

    float GetCommandLineWatermark()
    {
        static bool bParsed = false;
        static float CommandLineWatermark = UnsetFraction;
        return GetCommandLineFraction(
            TEXT("PinWrightTestMemoryWatermark="), CommandLineWatermark, bParsed);
    }

    float GetCommandLineHardFraction()
    {
        static bool bParsed = false;
        static float CommandLineHardFraction = UnsetFraction;
        return GetCommandLineFraction(
            TEXT("PinWrightTestMemoryHardFraction="), CommandLineHardFraction, bParsed);
    }

    double BytesToGiB(uint64 Bytes)
    {
        return static_cast<double>(Bytes) / (1024.0 * 1024.0 * 1024.0);
    }

    const TCHAR* TriggerName(EResetTrigger Trigger)
    {
        switch (Trigger)
        {
        case EResetTrigger::Count:     return TEXT("count");
        case EResetTrigger::Watermark: return TEXT("watermark");
        default:                       return TEXT("manual");
        }
    }

    void OnTestEnd(FAutomationTestBase* Test)
    {
        if (Test == nullptr
            || !Test->GetTestFullName().StartsWith(TEXT("PinWright."), ESearchCase::CaseSensitive))
        {
            return;
        }

        GLastCompletedTestId = Test->GetTestFullName();

        EResetTrigger Trigger = EResetTrigger::Count;
        if (AdvanceAndShouldReset(&Trigger))
        {
            RunResetNow(Trigger);
        }
    }
}

FStats& GetStats()
{
    static FStats Stats;
    return Stats;
}

int32& IntervalOverrideForTests()
{
    static int32 Override = 0;
    return Override;
}

float& WatermarkOverrideForTests()
{
    static float Override = UnsetFraction;
    return Override;
}

float& HardFractionOverrideForTests()
{
    static float Override = UnsetFraction;
    return Override;
}

FScopedInterval::FScopedInterval(int32 InInterval)
    : PreviousInterval(IntervalOverrideForTests())
{
    IntervalOverrideForTests() = InInterval;
}

FScopedInterval::~FScopedInterval()
{
    IntervalOverrideForTests() = PreviousInterval;
}

FScopedWatermark::FScopedWatermark(float InWatermark, float InHardFraction)
    : PreviousWatermark(WatermarkOverrideForTests())
    , PreviousHardFraction(HardFractionOverrideForTests())
{
    WatermarkOverrideForTests() = InWatermark;
    HardFractionOverrideForTests() = InHardFraction;
}

FScopedWatermark::~FScopedWatermark()
{
    WatermarkOverrideForTests() = PreviousWatermark;
    HardFractionOverrideForTests() = PreviousHardFraction;
}

FScopedMemorySample::FScopedMemorySample(uint64 InUsedPhysical, uint64 InTotalPhysical)
    : bPreviousEnabled(GMemorySampleOverrideEnabled)
    , PreviousSample(GMemorySampleOverride)
{
    GMemorySampleOverride.UsedPhysical = InUsedPhysical;
    GMemorySampleOverride.TotalPhysical = InTotalPhysical;
    GMemorySampleOverrideEnabled = true;
}

FScopedMemorySample::~FScopedMemorySample()
{
    GMemorySampleOverride = PreviousSample;
    GMemorySampleOverrideEnabled = bPreviousEnabled;
}

FMemorySample SampleMemory()
{
    if (GMemorySampleOverrideEnabled)
    {
        return GMemorySampleOverride;
    }

    const FPlatformMemoryStats PlatformStats = FPlatformMemory::GetStats();

    FMemorySample Sample;
    Sample.UsedPhysical = PlatformStats.UsedPhysical;
    Sample.TotalPhysical = PlatformStats.TotalPhysical;
    return Sample;
}

int32 ResolveInterval()
{
    const int32 Override = IntervalOverrideForTests();
    if (Override > 0)
    {
        return Override;
    }

    if (GTestGcEvery != UnsetInterval)
    {
        return GTestGcEvery;
    }

    const int32 CommandLineInterval = GetCommandLineInterval();
    if (CommandLineInterval != UnsetInterval)
    {
        return CommandLineInterval;
    }

    return GetDefault<UPinWrightSettings>()->TestSuiteResetIntervalTests;
}

float ResolveWatermark()
{
    const float Override = WatermarkOverrideForTests();
    if (Override >= 0.0f)
    {
        return Override;
    }

    if (GTestMemoryWatermark >= 0.0f)
    {
        return GTestMemoryWatermark;
    }

    const float CommandLineWatermark = GetCommandLineWatermark();
    if (CommandLineWatermark >= 0.0f)
    {
        return CommandLineWatermark;
    }

    return GetDefault<UPinWrightSettings>()->TestSuiteResetMemoryWatermark;
}

float ResolveHardFraction()
{
    const float Override = HardFractionOverrideForTests();
    if (Override >= 0.0f)
    {
        return Override;
    }

    if (GTestMemoryHardFraction >= 0.0f)
    {
        return GTestMemoryHardFraction;
    }

    const float CommandLineHardFraction = GetCommandLineHardFraction();
    if (CommandLineHardFraction >= 0.0f)
    {
        return CommandLineHardFraction;
    }

    return GetDefault<UPinWrightSettings>()->TestSuiteMemoryHardFraction;
}

bool ShouldEscalateAfterReset(
    uint64 UsedPhysicalBytes, uint64 TotalPhysicalBytes, float HardFraction)
{
    return HardFraction > 0.0f
        && TotalPhysicalBytes > 0
        && static_cast<double>(UsedPhysicalBytes)
            >= static_cast<double>(TotalPhysicalBytes) * static_cast<double>(HardFraction);
}

bool AdvanceAndShouldReset(EResetTrigger* OutTrigger)
{
    FStats& Stats = GetStats();
    ++Stats.TestsEnded;
    ++Stats.TestsSinceReset;

    const int32 Interval = ResolveInterval();
    const bool bCountDue = Interval > 0 && Stats.TestsSinceReset >= Interval;

    // Same predicate the asset.dump_folder sweep schedules its release step with: fire on the
    // count, or on the working-set watermark once a minimum gap of tests has passed. Sharing it
    // keeps one rule for "this long-running loop has accumulated enough to be worth reclaiming",
    // and its min-gap floor is what stops a watermark the reset cannot get back under from
    // turning into one full collect per test.
    const FMemorySample Memory = SampleMemory();
    const bool bDue = AssetDumpHandler::ShouldRunDumpReleaseStep(
        Stats.TestsSinceReset,
        Interval,
        Memory.UsedPhysical,
        Memory.TotalPhysical,
        ResolveWatermark());

    if (OutTrigger != nullptr)
    {
        *OutTrigger = bCountDue ? EResetTrigger::Count : EResetTrigger::Watermark;
    }

    return bDue;
}

void RunResetNow(EResetTrigger Trigger)
{
    const FMemorySample Before = SampleMemory();

    FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
    FSoundWaveCompilingManager::Get().FinishAllCompilation();

    if (GShaderCompilingManager && GShaderCompilingManager->IsCompiling())
    {
        GShaderCompilingManager->FinishAllCompilation();
    }

    if (GEditor && !GIsTransacting && !GUndo)
    {
        GEditor->ResetTransaction(
            NSLOCTEXT("PinWright", "SuiteMaintenanceReset", "PinWright suite maintenance"));
    }

    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

    const FMemorySample After = SampleMemory();

    FStats& Stats = GetStats();
    ++Stats.Resets;
    Stats.LastResetAtTest = Stats.TestsEnded;
    Stats.TestsSinceReset = 0;
    if (Trigger == EResetTrigger::Watermark)
    {
        ++Stats.WatermarkResets;
    }

    // Display, not Verbose: a reset that never happened and a reset that happened and reclaimed
    // nothing look identical in a suite log unless every one of them is on the record.
    UE_LOG(
        LogPinWrightSuiteMaintenance,
        Display,
        TEXT("PinWright suite maintenance reset %d (trigger=%s) after test %d: usedPhysical ")
        TEXT("%.2f -> %.2f GiB of %.2f GiB (freed %.2f GiB)."),
        Stats.Resets,
        TriggerName(Trigger),
        Stats.TestsEnded,
        BytesToGiB(Before.UsedPhysical),
        BytesToGiB(After.UsedPhysical),
        BytesToGiB(After.TotalPhysical),
        BytesToGiB(Before.UsedPhysical) - BytesToGiB(After.UsedPhysical));

    if (ShouldEscalateAfterReset(After.UsedPhysical, After.TotalPhysical, ResolveHardFraction()))
    {
        ++Stats.HardFractionEscalations;

        // Attribution is by log marker, never by AddError. This runs inside
        // FAutomationTestFramework::InternalStopTest, AFTER the just-finished test's success
        // state was frozen (AutomationTest.cpp:1384) but BEFORE the output device is detached
        // (:1407), so the line does land as an Error EVENT on that test's report while being
        // unable to turn its Result={Success} into a failure. That is deliberate: the run is
        // classified from the log by check_suite_log.py, which keys on the token below.
        UE_LOG(
            LogPinWrightSuiteMaintenance,
            Error,
            TEXT("PINWRIGHT_MEMORY_WATERMARK_EXCEEDED reset=%d lastTest=%s usedPhysical ")
            TEXT("%.2f -> %.2f GiB of %.2f GiB total, still at or above the hard fraction ")
            TEXT("%.2f after a full collect."),
            Stats.Resets,
            GLastCompletedTestId.IsEmpty() ? TEXT("<none>") : *GLastCompletedTestId,
            BytesToGiB(Before.UsedPhysical),
            BytesToGiB(After.UsedPhysical),
            BytesToGiB(After.TotalPhysical),
            ResolveHardFraction());
    }
}

bool IsRegistered()
{
    return TestEndHandle.IsValid();
}

void Register()
{
    if (!TestEndHandle.IsValid())
    {
        TestEndHandle = FAutomationTestFramework::Get().OnTestEndEvent.AddStatic(&OnTestEnd);
    }
}

void Unregister()
{
    if (TestEndHandle.IsValid())
    {
        FAutomationTestFramework::Get().OnTestEndEvent.Remove(TestEndHandle);
        TestEndHandle.Reset();
    }
}

const TCHAR* ScratchRootPackagePath()
{
    return GScratchRootPackagePath;
}

bool IsScratchPackageName(const FString& PackageName)
{
    const FString Root(GScratchRootPackagePath);
    return PackageName.Equals(Root, ESearchCase::IgnoreCase)
        || PackageName.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase);
}

FString ScratchRootContentDir()
{
    FString Dir;
    if (!FPackageName::TryConvertLongPackageNameToFilename(FString(GScratchRootPackagePath), Dir))
    {
        return FString();
    }
    return FPaths::ConvertRelativePathToFull(Dir);
}

bool ScratchRootExistsOnDisk()
{
    const FString RootDir = ScratchRootContentDir();
    return !RootDir.IsEmpty() && IFileManager::Get().DirectoryExists(*RootDir);
}

int32 SweepScratchRoot(TArray<FString>* OutRemainingFiles, const FString& SubDirectory)
{
    const FString RootDir = ScratchRootContentDir();
    if (RootDir.IsEmpty() || !IFileManager::Get().DirectoryExists(*RootDir))
    {
        return 0;
    }

    FString SweepDir = RootDir;
    if (!SubDirectory.IsEmpty())
    {
        SweepDir = FPaths::ConvertRelativePathToFull(RootDir / SubDirectory);
        // A caller-supplied subtree must resolve back inside the root; a ".." that climbs out of
        // it sweeps nothing rather than deleting somewhere else in the host's Content tree.
        if (!FPaths::IsUnderDirectory(SweepDir, RootDir))
        {
            return 0;
        }
    }

    if (!IFileManager::Get().DirectoryExists(*SweepDir))
    {
        return 0;
    }

    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(
        Files, *SweepDir, TEXT("*"), /*Files=*/true, /*Directories=*/false);

    int32 RemovedCount = 0;
    for (const FString& File : Files)
    {
        const FString Extension = FPaths::GetExtension(File, /*bIncludeDot=*/true);
        const bool bIsMapFile =
            Extension.Equals(FPackageName::GetMapPackageExtension(), ESearchCase::IgnoreCase);
        const bool bIsPackageFile = bIsMapFile
            || Extension.Equals(FPackageName::GetAssetPackageExtension(), ESearchCase::IgnoreCase);

        // A fixture .uasset can still have a live object and an attached linker, so it goes
        // through the sanctioned teardown, which detaches, deletes and unregisters without the
        // forbidden ObjectTools force-delete. Anything else under the root is a plain file that
        // teardown would not recognise, and is removed below.
        if (bIsPackageFile)
        {
            FString PackageName;
            if (FPackageName::TryConvertFilenameToLongPackageName(File, PackageName))
            {
                // CleanupTestAsset resolves its filename with GetAssetPackageExtension() only, so
                // for a .umap it probes a <pkg>.uasset that does not exist, skips ResetLoaders and
                // returns before deleting anything. It still detaches the world object -- and that
                // rename into /Transient is why the linker has to be reset FIRST, while the
                // original package can still be found by name. Without this the fallback delete
                // below runs with the map linker attached and fails with ERROR_SHARING_VIOLATION.
                if (bIsMapFile)
                {
                    if (UPackage* MapPackage = FindPackage(nullptr, *PackageName))
                    {
                        ResetLoaders(MapPackage);
                    }
                }
                CleanupTestAsset(PackageName);
            }
        }

        if (IFileManager::Get().FileSize(*File) < 0
            || IFileManager::Get().Delete(*File, /*RequireExists=*/false, /*EvenReadOnly=*/true,
                /*Quiet=*/true))
        {
            ++RemovedCount;
            continue;
        }

        if (OutRemainingFiles != nullptr)
        {
            OutRemainingFiles->Add(File);
        }
    }

    // Per-asset teardown removes files and never the directories they sat in, which is why a
    // scratch root with nothing left in it still survives a run as a tree of empty folders.
    TArray<FString> Directories;
    IFileManager::Get().FindFilesRecursive(
        Directories, *SweepDir, TEXT("*"), /*Files=*/false, /*Directories=*/true);
    Directories.Add(SweepDir);

    // Deepest first: a child path is always longer than its parent, so length-descending order
    // is enough to guarantee a directory is only tried once its own children are gone.
    Directories.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });
    for (const FString& Directory : Directories)
    {
        // Tree=false removes a directory only when it is already empty, so one still holding a
        // fixture file the sweep could not delete survives instead of taking that file with it.
        IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists=*/false, /*Tree=*/false);
    }

    return RemovedCount;
}

FScopedForeignDirtyPackageSuspension::FScopedForeignDirtyPackageSuspension()
{
    // The same two collectors editor.save_all walks, so the guard cannot miss a package the
    // handler would have written.
    TArray<UPackage*> DirtyPackages;
    FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
    FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);

    for (UPackage* Package : DirtyPackages)
    {
        if (Package == nullptr)
        {
            continue;
        }

        const FString PackageName = Package->GetName();
        if (IsScratchPackageName(PackageName))
        {
            continue;
        }

        Package->SetDirtyFlag(false);
        SuspendedNames.Add(PackageName);
    }
}

FScopedForeignDirtyPackageSuspension::~FScopedForeignDirtyPackageSuspension()
{
    for (const FString& PackageName : SuspendedNames)
    {
        if (UPackage* Package = FindPackage(nullptr, *PackageName))
        {
            Package->SetDirtyFlag(true);
        }
    }
}
}

#endif
