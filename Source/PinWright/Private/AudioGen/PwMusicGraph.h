// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwMusicGraph.h - assembles an interactive-music MetaSound Source over already-rendered
// stems. AudioGen/PwMusicRender.h owns turning a score into seamless PCM stems; this header
// owns turning those stems into the one delivery format the engine can drive at runtime.
//
// ---------------------------------------------------------------------------------------
// WHY A METASOUND AND NOT A SOUNDCUE / BARE WAVE
// ---------------------------------------------------------------------------------------
// Two reasons, both structural rather than stylistic.
//
// 1. DYNAMIC MEANS PARAMETERS. A rendered stem is static audio. What makes music dynamic is
//    a graph with named parameter inputs the game writes while it plays, and MetaSound is the
//    only thing in the engine that exposes them (UAudioComponent::SetFloatParameter /
//    SetIntParameter / SetTriggerParameter land on the graph's inputs by name). A SoundCue's
//    graph has no equivalent.
//
// 2. SAMPLE-ACCURATE LOOP POINTS EXIST ONLY ON THE WAVE PLAYER. Bare USoundWave loop
//    metadata is inert: FSoundWaveCuePoint / GetLoopRegions() serialize into the asset and no
//    decoder or mixer source reads them (the same fact PwMusicRender.h states as the reason
//    its stems have to BE seamless by construction). The only consumer of loop points in the
//    engine is the Wave Player node's Loop / Loop Start / Loop Duration pins - which is why
//    this file binds them there and nowhere else.
//
// MetaSound ships enabled by default, so this is a primary path, not a gated extra.
//
// ---------------------------------------------------------------------------------------
// THE GRAPH
// ---------------------------------------------------------------------------------------
//   Stem0 (WaveAsset input) ---> Wave Player ---> Multiply (Audio by Float) ---\
//   Stem1 (WaveAsset input) ---> Wave Player ---> Multiply (Audio by Float) ----> Mixer -> Out
//   ...                                              ^
//   Intensity (Float) -> Map Range (Float) ----------/        one Map Range per stem
//   Play  (Trigger) -> every Wave Player's "Play"
//   Stop  (Trigger) -> every Wave Player's "Stop"
//   Section (Int32) -> exposed, NOT consumed by this graph (see SECTION below)
//
// Node classes used, by their exact registry keys (resolved through
// Metasound::Frontend::ISearchEngine, never assumed):
//   UE.Wave Player.Mono | UE.Wave Player.Stereo      (per stem, by the stem's channel count)
//   MapRange.MapRange.Float                          (per stem, intensity -> gain)
//   UE.Multiply.Audio by Float                       (per stem, per graph channel)
//   AudioMixer.Audio Mixer (Mono, N)                 (N stems, mono graph)
//   AudioMixer.Audio Mixer (Stereo, N)               (N stems, stereo graph)
//
// ---------------------------------------------------------------------------------------
// THE MONO/STEREO TRAP THIS FILE EXISTS TO NOT FALL INTO
// ---------------------------------------------------------------------------------------
// `audio.authoring.add_metasound_node`'s `waveplayer` shorthand resolves to the single
// hardcoded key "UE.Wave Player.Mono" (AudioAuthoringHandler.cpp), and there is NO verb that
// changes a node's class after creation - a wrong player is unfixable except by rebuilding
// the asset. A stereo stem through a Mono Wave Player is not an error at any layer: the node
// downmixes ("The wave's channel configurations will be up or down mixed to match the wave
// players audio channel format", MetasoundWavePlayerNode.cpp:151) and the stereo image is
// gone with every stage reporting success. That is the §1 defect class exactly.
//
// So the variant is DERIVED, per stem, from USoundWave::NumChannels, and the derived key is
// then CONFIRMED against the live class registry before any node is added. A stem whose asset
// does not resolve, is not a USoundWave, or reports NumChannels outside 1..2 is an ERROR
// naming the count - never a guess and never a silent downmix (§3).
//
// GRAPH CHANNEL COUNT is the max over the stems, so no stem can lose a channel to the graph
// either: one stereo stem makes the whole graph stereo (output format interface swapped to
// UE.OutputFormat.Stereo via FMetaSoundFrontendDocumentBuilder::ModifyInterfaces, the same
// call UMetaSoundSourceBuilder::SetFormat makes). A mono stem inside a stereo graph feeds its
// single Multiply output to BOTH of its mixer input channels, i.e. centered - which is a
// stated placement, not a dropped channel.
//
// ---------------------------------------------------------------------------------------
// INTENSITY -> PER-STEM GAIN
// ---------------------------------------------------------------------------------------
// One Map Range (Float) node per stem, wired Intensity -> "In", with:
//     In Range A  = FPwMusicGraphStem::IntensityThreshold
//     In Range B  = IntensityThreshold + PwMusicGraphIntensityFadeWidth
//     Out Range A = 0.0,  Out Range B = 1.0,  Clamped = true
// so a stem is silent below its threshold, fades in across the fade width, and sits at unity
// above it. Rising Intensity therefore brings layers in in threshold order, which is the whole
// point of the parameter. Thresholds are the caller's and are NOT sorted, deduplicated or
// clamped here: a threshold outside [0, 1] is an error naming the range, because silently
// clamping it would produce a layer that can never come in while reporting success.
//
// The mapping is intentionally the simplest honest one. It is a straight-line crossfade, not
// an equal-power one, so two stems fading against each other dip slightly at the crossover;
// a caller that cares stages the thresholds so the fades do not overlap.
//
// ---------------------------------------------------------------------------------------
// SECTION IS EXPOSED, NOT CONSUMED - AND SAYS SO
// ---------------------------------------------------------------------------------------
// `Section` is added as an Int32 graph input and nothing in this graph reads it. Branching on
// it means swapping which wave each player holds per section, which needs a stem set per
// section rather than one stem list - out of scope for this assembler. Because a caller
// reading GraphInputs would otherwise reasonably assume the input does something, the build
// emits a Warnings entry saying it does not. The input is still worth having: the game sets
// it exactly like Intensity, and a graph edited by hand afterwards has the parameter already
// on the interface with its edges intact.
//
// ---------------------------------------------------------------------------------------
// QUARTZ - DOCUMENTED HERE, DEPLOYED BY THE GAME, NOT BY THIS FILE
// ---------------------------------------------------------------------------------------
// Bar-aligned starts/switches are RUNTIME SCHEDULING, not asset authoring, so nothing here
// depends on Quartz and this file adds no Quartz module dependency. Quartz is core engine
// (Runtime/AudioMixer + Runtime/Engine, no plugin), so it is always available to the game.
// The intended runtime pattern, in full:
//
//   UQuartzSubsystem* Quartz = UQuartzSubsystem::Get(World);
//   FQuartzClockSettings Settings;                       // TimeSignature, bIgnoreLevelChange
//   UQuartzClockHandle* Clock = Quartz->CreateNewClock(World, "MusicClock", Settings);
//   Clock->SetBeatsPerMinute(World, {}, {}, Clock, Bpm); // the score's tempo
//   Clock->StartClock(World, Clock);
//   Clock->SubscribeToQuantizationEvent(World, EQuartzCommandQuantization::Bar, OnBarDelegate, Clock);
//
//   FQuartzQuantizationBoundary Boundary;
//   Boundary.Quantization = EQuartzCommandQuantization::Bar;
//   Boundary.CountingReferencePoint = EQuarztQuantizationReference::BarRelative;
//   AudioComponent->PlayQuantized(World, Clock, Boundary, OnStartedDelegate);
//
// and Intensity / Section / Play / Stop are then ordinary MetaSound parameters on that same
// component (SetFloatParameter("Intensity", x), SetIntParameter("Section", n),
// SetTriggerParameter("Play")). Note the division of labour: Quartz decides WHEN a change
// takes effect; this graph decides WHAT the change does. Neither substitutes for the other -
// setting Intensity without Quartz still works, it just lands mid-bar.
//
// ---------------------------------------------------------------------------------------
// FAILURE DIRECTION AND VERIFICATION
// ---------------------------------------------------------------------------------------
// Every stem is resolved and validated BEFORE any package or asset is touched, so an
// unresolvable stem, an empty stem list or a bad package path leaves nothing behind to clean
// up (§12). `bMeasured` is false on a default-constructed result and is set only on the single
// full-success path, after the read-back below passed.
//
// The read-back (§4) does not ask the builder anything. After FinishBuilding() the document is
// re-read through IMetaSoundDocumentInterface::GetConstDocument() - the same const document
// describe_metasound and the MSIR decompiler read - and the build FAILS unless that document
// independently shows: every expected graph input present with the expected TypeName; every
// Wave Player node carrying the expected registry class name for its stem's channel count;
// every Wave Player's "Wave Asset" pin fed by an edge from its stem's graph input, whose stored
// default literal resolves to the USoundWave that was passed in; and the Loop / Loop Start /
// Loop Duration literals holding the supplied values. NodesAdded / ConnectionsMade are the
// difference between document counts measured before and after the edits (§1) - never a tally
// of how many builder calls were issued, because a builder call that silently no-ops would
// otherwise be counted as work.
//
// TICK SAFETY (§10). This function creates an asset and, when asked, writes the .uasset on the
// calling stack - the same three hazards Dispatch/SafePoint.cpp section E lists for
// audio.authoring.create_sound_wave_from_pcm and audio.synth.export (AssetCreatePolicy's
// overwrite branch can force a GC, the factory's NewObject reconstructs an existing asset in
// place, and the save writes synchronously). It is therefore NOT safe to call from inside
// UWorld::Tick. The gate is a method-name table, so it applies to the REGISTERED verb rather
// than to this function: `audio.music.build_interactive` (Handlers/Audio/AudioMusicHandler.cpp)
// is the caller and is listed in SafePoint.cpp's asset-creation section. Any further verb that
// reaches this function owes an entry there in the same change - and only there, because every
// entry must name a registered handler or PinWright.core.safe_point.TickUnsafeMethodsAreRegistered
// fails, and one operation must never carry both a table entry and a hand-written gate.

#pragma once

#include "CoreMinimal.h"

/**
 * One rendered stem and how it participates in the graph.
 *
 * AssetPath must resolve to a USoundWave with 1 or 2 channels. Loop points are in seconds and
 * are written onto that stem's Wave Player node, which is the only place in the engine that
 * honours them.
 */
struct FPwMusicGraphStem
{
    /** /Game path of the stem's USoundWave. Resolved and class-checked before anything is created. */
    FString AssetPath;

    /**
     * Name of the WaveAsset graph input created for this stem. Empty means "Stem<Index>".
     * Duplicates - and collisions with the four reserved control names - are an error, because
     * FMetaSoundFrontendDocumentBuilder would otherwise resolve the second one to the first.
     */
    FString InputName;

    /** Wave Player "Loop Start", in seconds. Negative is an error, not a clamp to zero. */
    double LoopStartSeconds = 0.0;

    /**
     * Wave Player "Loop Duration", in seconds. Zero or negative means "loop the whole asset",
     * which the node spells as a negative Loop Duration; the value written in that case is
     * PwMusicGraphLoopWholeAsset, not the caller's number.
     */
    double LoopDurationSeconds = 0.0;

    /**
     * Intensity at which this stem starts fading in. Must be within [0, 1]. See the
     * INTENSITY block above for the exact Map Range wiring this produces.
     */
    double IntensityThreshold = 0.0;
};

/**
 * What the build measured. `bMeasured` is false on a default-constructed result and on every
 * failure exit, so a caller cannot read a clean zero the builder never looked at.
 */
struct FPwMusicGraphResult
{
    bool bMeasured = false;

    /** Object path of the created/updated UMetaSoundSource. */
    FString AssetPath;

    /**
     * Nodes the document gained: its node count after the edits minus its node count before
     * them, both read off the document. Not a count of AddNode calls (§1).
     *
     * The "before" snapshot is taken once the OUTPUT FORMAT is settled, so a stereo build does
     * not score the format interface's own output nodes as music-graph nodes. For N stems it
     * comes to 4 control inputs + N stem inputs + N Wave Players + N Map Ranges + one Multiply
     * per stem CHANNEL + (N > 1 ? one mixer : 0).
     */
    int32 NodesAdded = 0;

    /** Edges the document gained, measured the same way and over the same window. */
    int32 ConnectionsMade = 0;

    /** Graph input names present on the finished document, in creation order. */
    TArray<FString> GraphInputs;

    /**
     * Things that are true about the finished asset and that a caller reading the counts
     * would otherwise get wrong - notably that `Section` is exposed but unread. Not a failure
     * channel: a build that warns still returned true and still passed the read-back.
     */
    TArray<FString> Warnings;
};

/** Reserved control-input names. A stem input may not reuse any of them. */
inline constexpr TCHAR PwMusicGraphIntensityInput[] = TEXT("Intensity");
inline constexpr TCHAR PwMusicGraphSectionInput[]   = TEXT("Section");
inline constexpr TCHAR PwMusicGraphPlayInput[]      = TEXT("Play");
inline constexpr TCHAR PwMusicGraphStopInput[]      = TEXT("Stop");

/**
 * Width of the intensity window a stem fades in across, above its threshold. A stem with
 * threshold T is silent at Intensity <= T and at unity from T + this value upward.
 */
inline constexpr double PwMusicGraphIntensityFadeWidth = 0.25;

/** Wave Player "Loop Duration" value meaning "the whole asset" (the node's own convention). */
inline constexpr double PwMusicGraphLoopWholeAsset = -1.0;

/**
 * Largest stem count the engine's Audio Mixer node family covers - MetasoundMixerNode.cpp
 * registers 2..8 inputs per channel layout and nothing wider. Note this is BELOW
 * PwMusicLimits::MaxTracks (16), which caps a score's tracks: a 9..16-track score therefore
 * renders fine and is rejected here, loudly, rather than being silently pre-mixed. Widening it
 * means chaining mixers, which this assembler deliberately does not do.
 */
inline constexpr int32 PwMusicGraphMaxStems = 8;

/**
 * Build (or rebuild in place) an interactive-music MetaSound Source at PackagePath/AssetName
 * over Stems.
 *
 * PackagePath is a /Game folder ("/Game/Audio/Music"), AssetName the leaf. An existing
 * same-class asset at that path is REUSED rather than duplicated - AssetCreatePolicy::Resolve
 * decides that, dialog-free, and its UpdateInPlace branch is what makes a retried build
 * converge on one asset instead of accumulating "_1" siblings (§8). An occupant of a different
 * class is an error, never an overwrite. There is no overwrite parameter: a caller that wants
 * delete-and-recreate runs its own Resolve with overwrite first (audio.music.build_interactive
 * does), which clears the path before this call - resolving twice is safe, because the second
 * Resolve either finds nothing or finds the same-class object it is about to rebuild.
 *
 * bSaveToDisk writes the .uasset through SaveAssetToDiskReportingPresence and a save that does
 * not land is a FAILURE, not a warning - the result carries no persistence field, so returning
 * true while nothing reached disk would be a persistence claim nobody measured (§5).
 *
 * On ANY failure returns false, leaves Out unmeasured, and fills OutErrorCode with an
 * ErrorCodes::ERR_* literal plus a sentence naming the stem or pin that failed. Game thread
 * only, editor only, and never from inside UWorld::Tick (see TICK SAFETY above).
 */
bool PwBuildInteractiveMusicGraph(const FString& PackagePath, const FString& AssetName,
                                  const TArray<FPwMusicGraphStem>& Stems, bool bSaveToDisk,
                                  FPwMusicGraphResult& Out, FString& OutErrorCode, FString& OutError);
