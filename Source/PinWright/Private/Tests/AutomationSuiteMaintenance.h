// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS
namespace PinWrightSuiteMaintenance
{
    // Suite statistics and scheduling count completed automation tests whose dotted ID starts with "PinWright.".
    struct PINWRIGHT_API FStats
    {
        int32 TestsEnded = 0;
        int32 Resets = 0;
        int32 LastResetAtTest = 0;
        // Tests completed since the last reset. This, not TestsEnded, is what the scheduling
        // predicate reads: a watermark-triggered reset must restart the count, which a modulo
        // over the running total cannot express.
        int32 TestsSinceReset = 0;
        // Subset of Resets that the memory watermark forced ahead of the count trigger.
        int32 WatermarkResets = 0;
        // Resets after which the working set was STILL at or above the hard fraction, i.e. the
        // reclaim did not work. Each one emits PINWRIGHT_MEMORY_WATERMARK_EXCEEDED.
        int32 HardFractionEscalations = 0;
    };

    // Why a reset ran, for the audit log line and for WatermarkResets.
    enum class EResetTrigger : uint8
    {
        Manual,
        Count,
        Watermark,
    };

    // One physical-memory reading. Separated from FPlatformMemory so the escalation path can be
    // driven from a test without manufacturing real memory pressure.
    struct PINWRIGHT_API FMemorySample
    {
        uint64 UsedPhysical = 0;
        uint64 TotalPhysical = 0;
    };

    PINWRIGHT_API FStats& GetStats();
    PINWRIGHT_API int32& IntervalOverrideForTests();
    PINWRIGHT_API float& WatermarkOverrideForTests();
    PINWRIGHT_API float& HardFractionOverrideForTests();

    struct PINWRIGHT_API FScopedInterval final
    {
        explicit FScopedInterval(int32 InInterval);
        ~FScopedInterval();

        FScopedInterval(const FScopedInterval&) = delete;
        FScopedInterval& operator=(const FScopedInterval&) = delete;

    private:
        int32 PreviousInterval;
    };

    struct PINWRIGHT_API FScopedWatermark final
    {
        FScopedWatermark(float InWatermark, float InHardFraction);
        ~FScopedWatermark();

        FScopedWatermark(const FScopedWatermark&) = delete;
        FScopedWatermark& operator=(const FScopedWatermark&) = delete;

    private:
        float PreviousWatermark;
        float PreviousHardFraction;
    };

    // Test seam for the memory reading. While one of these is alive, SampleMemory() returns the
    // injected bytes instead of the live FPlatformMemory::GetStats(), so both the watermark
    // trigger and the post-reset escalation are reachable deterministically.
    struct PINWRIGHT_API FScopedMemorySample final
    {
        FScopedMemorySample(uint64 InUsedPhysical, uint64 InTotalPhysical);
        ~FScopedMemorySample();

        FScopedMemorySample(const FScopedMemorySample&) = delete;
        FScopedMemorySample& operator=(const FScopedMemorySample&) = delete;

    private:
        bool bPreviousEnabled;
        FMemorySample PreviousSample;
    };

    PINWRIGHT_API FMemorySample SampleMemory();

    PINWRIGHT_API int32 ResolveInterval();
    PINWRIGHT_API float ResolveWatermark();
    PINWRIGHT_API float ResolveHardFraction();

    // True when the working set is still at or above HardFraction of physical RAM after a reset,
    // i.e. the collect did not give the memory back. Pure so the escalation rule is testable.
    PINWRIGHT_API bool ShouldEscalateAfterReset(
        uint64 UsedPhysicalBytes, uint64 TotalPhysicalBytes, float HardFraction);

    // Counts one completed test and reports whether a reset is due. OutTrigger, when supplied,
    // receives which of the two triggers fired.
    PINWRIGHT_API bool AdvanceAndShouldReset(EResetTrigger* OutTrigger = nullptr);
    PINWRIGHT_API void RunResetNow(EResetTrigger Trigger = EResetTrigger::Manual);
    PINWRIGHT_API bool IsRegistered();
    PINWRIGHT_API void Register();
    PINWRIGHT_API void Unregister();

    // The one spelling of the suite-wide fixture scratch root. Fixtures that need a persistable
    // /Game package put it here; the save-all dirty-set scoping below and the end-of-suite sweep
    // are the two readers that have to agree with those fixtures on where "here" is.
    PINWRIGHT_API const TCHAR* ScratchRootPackagePath();

    // <Project>/Content/PinWrightTests as an absolute path, or empty when the package path does
    // not resolve to a mounted content root.
    PINWRIGHT_API FString ScratchRootContentDir();

    PINWRIGHT_API bool IsScratchPackageName(const FString& PackageName);

    PINWRIGHT_API bool ScratchRootExistsOnDisk();

    // Removes every fixture file left under the scratch root, unregisters it, and then removes
    // the emptied directory tree that per-asset teardown never touches. Returns how many files
    // it removed; OutRemainingFiles, when supplied, receives the ones it could not.
    //
    // SubDirectory narrows the sweep to one subtree of the root (a path relative to it). Only the
    // end-of-suite gate may sweep the whole root: a global sweep run mid-suite detaches and
    // deletes fixtures belonging to tests that have not finished with them, so anything else that
    // needs to sweep passes its own directory. A SubDirectory that does not resolve inside the
    // root sweeps nothing.
    PINWRIGHT_API int32 SweepScratchRoot(
        TArray<FString>* OutRemainingFiles = nullptr, const FString& SubDirectory = FString());

    // Clears the dirty flag of every dirty package OUTSIDE the scratch root for the lifetime of
    // the guard and restores it on destruction. An editor-wide save-all invoked inside the scope
    // therefore sees a fixture-only dirty set and cannot persist a shipped host package; the
    // packages it stepped over are exactly as dirty afterwards as they were before, so a real
    // unsaved edit in a live editor survives the suite instead of being written or discarded.
    class PINWRIGHT_API FScopedForeignDirtyPackageSuspension final
    {
    public:
        FScopedForeignDirtyPackageSuspension();
        ~FScopedForeignDirtyPackageSuspension();

        FScopedForeignDirtyPackageSuspension(const FScopedForeignDirtyPackageSuspension&) = delete;
        FScopedForeignDirtyPackageSuspension& operator=(const FScopedForeignDirtyPackageSuspension&) = delete;

        const TArray<FString>& SuspendedPackageNames() const { return SuspendedNames; }

    private:
        // Names, not pointers: a package can be collected inside the scope, and re-resolving by
        // name lets the restore skip one that is gone instead of touching freed memory.
        TArray<FString> SuspendedNames;
    };
}
#endif
