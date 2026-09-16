// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "JournalRecorder.h"
#include "PinWrightSettings.h"

#include "Compat/EngineVersionCompat.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/CoreDelegates.h"

DEFINE_LOG_CATEGORY_STATIC(LogRecorderLifecycle, Log, All);

namespace
{
    /**
     * Owns the PIE bindings and the drain ticker for the journal recorder. A single static
     * instance registers itself once the editor is up, so the chunk needs no edit to the module
     * startup file. The recorder facade lives in the lightweight PinWrightRecorder module;
     * this handler (in the UnrealEd-linked gateway module) supplies the editor-only PIE wiring.
     */
    class FRecorderLifecycle
    {
    public:
        FRecorderLifecycle()
        {
            // FEditorDelegates aren't available until the editor is constructed; defer registration.
            PostEngineInitHandle = MCP_ON_POST_ENGINE_INIT.AddRaw(this, &FRecorderLifecycle::OnPostEngineInit);
        }

    private:
        void OnPostEngineInit()
        {
            MCP_ON_POST_ENGINE_INIT.Remove(PostEngineInitHandle);
            PostEngineInitHandle.Reset();

            if (!GIsEditor)
            {
                return;
            }

            FEditorDelegates::BeginPIE.AddRaw(this, &FRecorderLifecycle::OnBeginPIE);
            FEditorDelegates::EndPIE.AddRaw(this, &FRecorderLifecycle::OnEndPIE);
        }

        void OnBeginPIE(const bool /*bIsSimulating*/)
        {
            const UPinWrightSettings* Settings = GetDefault<UPinWrightSettings>();
            if (!Settings->bJournalEnabled)
            {
                return;
            }

            FJournalRecorder::BeginSession(FString(), Settings->JournalRetentionCap, Settings->JournalRecordingsSubdir);
            if (!FJournalRecorder::IsRecording())
            {
                return;
            }

            // Drain producer messages once per game-thread tick while the session is open.
            DrainTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
                FTickerDelegate::CreateRaw(this, &FRecorderLifecycle::OnDrainTick));

            // Stamp the render time domain once per game-thread world tick, so every game-thread
            // event/value (UI, game-mode delegates, etc.) inherits the current world time + render
            // frame without per-site stamping.
            WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddRaw(this, &FRecorderLifecycle::OnWorldTickStart);
        }

        void OnEndPIE(const bool /*bIsSimulating*/)
        {
            if (WorldTickStartHandle.IsValid())
            {
                FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
                WorldTickStartHandle.Reset();
            }

            if (DrainTickerHandle.IsValid())
            {
                FTSTicker::GetCoreTicker().RemoveTicker(DrainTickerHandle);
                DrainTickerHandle.Reset();
            }

            // EndSession drains remaining messages to empty, flushes, and closes the file.
            FJournalRecorder::EndSession();
        }

        bool OnDrainTick(float /*DeltaTime*/)
        {
            FJournalRecorder::DrainAndFlush();
            return true;
        }

        // Per-world-tick render-domain stamp (game thread). Game-thread logs emitted during this
        // world's tick inherit the render time domain: world time as dt, GFrameCounter as df.
        void OnWorldTickStart(UWorld* World, ELevelTick /*TickType*/, float /*DeltaSeconds*/)
        {
            if (!World || !FJournalRecorder::IsRecording())
            {
                return;
            }
            if (World->WorldType != EWorldType::PIE && World->WorldType != EWorldType::Game)
            {
                return;
            }
            FJournalRecorder::StampDomain(EJournalDomain::Render, World->GetTimeSeconds(), static_cast<int64>(GFrameCounter));
        }

        FDelegateHandle PostEngineInitHandle;
        FDelegateHandle WorldTickStartHandle;
        FTSTicker::FDelegateHandle DrainTickerHandle;
    };

    FRecorderLifecycle GRecorderLifecycle;
}
