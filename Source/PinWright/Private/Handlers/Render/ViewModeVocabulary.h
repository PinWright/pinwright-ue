// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
// EViewModeIndex. Every function here is a pure function of it plus the engine's own
// ApplyViewMode / EngineShowFlagOverride, so nothing in this header needs a viewport.
#include "Engine/EngineBaseTypes.h"

class FEditorViewportClient;
struct FEngineShowFlags;

// ONE view-mode vocabulary, shared by every verb that names a view mode.
//
// WHY THIS FILE EXISTS. Two spellings of the same vocabulary is a drift source, and a
// hand-written enumerator chain is a rot source. The plugin already carries three hand-written
// enum chains (ApplyBlendMode / ApplyShadingModel / ApplyMaterialDomain in
// Handlers/Material/MaterialAuthoringHandler.cpp) and all three are ALREADY incomplete against
// UE 5.8 - they are missing BLEND_TranslucentColoredTransmittance, MSM_SingleLayerWater,
// MSM_FromMaterialExpression and MD_RuntimeVirtualTexture, and each returns a bool every caller
// discards, so an unrecognised name is a silent no-op. A fourth hand-maintained subset of
// EViewModeIndex would rot the same way, and the rot would be invisible: a mode the parser does
// not know simply cannot be asked for, and nothing reports the gap.
//
// So the vocabulary is DERIVED from StaticEnum<EViewModeIndex>() at first use. Adding an
// enumerator to the engine adds a key here with no edit to this file. The only hand-written
// tables are:
//   * five KEY OVERRIDES, where PinWright's published spelling deliberately differs from the
//     enumerator name (they are the pre-existing wire contract and changing them would break
//     every stored request and every reported viewModeKey);
//   * the legacy ALIASES editor.set_view_mode has always accepted;
//   * the ten modes whose picture is chosen by a companion sub-visualisation the mode name does
//     not carry, which is a property of FEditorViewportClient's members, not of the enum.
// Everything else - which modes are sentinels, which render identically to Lit, which the engine
// disables on this build - is COMPUTED from the engine's own functions, so it cannot drift.
namespace PinWrightViewModes
{
    // Why a wire spelling was refused, or Ok. Each non-Ok value is a DIFFERENT refusal, because
    // "unknown mode" and "this mode would silently render Lit" are different facts and a caller
    // acts differently on them.
    enum class EViewModeStatus : uint8
    {
        // Settable, distinguishable from Lit at the show-flag level, and available on this build.
        Ok,
        // No enumerator matches the spelling.
        Unknown,
        // A sentinel or a hidden/deprecated enumerator: VMI_Unknown (255), VMI_Max, the
        // deprecated VMI_Lit_Wireframe, and the UHT-generated _MAX. Not renderable at all.
        Sentinel,
        // ApplyViewMode leaves the show flags bit-identical to ApplyViewMode(VMI_Lit), so a
        // capture in this mode is a Lit capture with a different label. VMI_GroupLODColoration
        // (a menu grouping item, not a render mode) and the deprecated VMI_Lit_Wireframe land
        // here. This is COMPUTED, not listed - a future engine mode that stops distinguishing
        // itself is caught the same way.
        IndistinctFromLit,
        // The mode renders a sub-visualisation chosen separately from the mode name
        // (FEditorViewportClient::Current*VisualizationMode). Accepted only when the viewport
        // already has one selected; otherwise refused, because a bare mode name would render
        // whatever the viewport's overview default happens to be.
        NeedsCompanion,
        // The engine's own EngineShowFlagOverride clears the flag that distinguishes this mode
        // on this build - r.RayTracing off strips PathTracing and RayTracingDebug that way
        // (ShowFlags.cpp:513-517). Refused rather than applied, because applying it renders a
        // plausible near-Lit picture that is not the mode that was asked for.
        Unavailable,
    };

    // What a wire spelling resolved to, or why it did not.
    struct FViewModeResolution
    {
        EViewModeIndex ViewMode = VMI_Lit;
        // The canonical wire key: what this mode is reported as, and a spelling Resolve() accepts.
        // The round-trip property (report -> request) is what makes a capture response feed
        // straight back into a request.
        FString Key;
        // Localized, from GetDisplayName below. For reading, never comparing.
        FString DisplayName;
        // The token the engine's `viewmode` console command parses, filled ONLY where it differs
        // from Key (GetViewModeName spells VMI_CollisionVisibility "CollisionVis"). Empty means
        // Key is also the exec token. GetViewModeName carries no ENGINE_API and cannot be linked
        // from this module, which is why the exceptions are spelled out rather than queried.
        FString ExecName;
        // "collisionSimple" / "collisionComplex" for the two collision modes, empty otherwise.
        // Drives editor.set_view_mode's per-actor collision report.
        const TCHAR* CollisionChannelName = nullptr;

        EViewModeStatus Status = EViewModeStatus::Unknown;
        // Registered error code and a message that names what is missing. Both empty when Ok.
        FString ErrorCode;
        FString ErrorMessage;

        // EngineShowFlags names ApplyViewMode writes differently for this mode than it does for
        // VMI_Lit. This is the set a caller can MEASURE on a live viewport to prove the mode
        // reached the renderer's inputs rather than merely being assigned to a field. Empty
        // exactly when Status is IndistinctFromLit (or the mode is VMI_Lit itself).
        TArray<FString> DistinguishingShowFlags;

        // Set when Status is NeedsCompanion but the viewport already had a sub-visualisation
        // selected, in which case Status is Ok and this names it.
        FString CompanionMode;

        bool IsValid() const { return Status == EViewModeStatus::Ok; }
    };

    // The canonical wire key for a mode. Reflection-derived from the enumerator name minus the
    // VMI_ prefix, with the five published overrides applied. Returns "Unknown" for a value with
    // no enumerator. This is the single definition of the key spelling;
    // PinWrightRenderCapture::GetViewModeKey delegates to it so a capture response and a request
    // parser can never disagree.
    FString GetKey(EViewModeIndex ViewMode);

    // The human-readable name for a mode, read the only way that is safe. Call this rather than
    // UViewModeUtils::GetViewModeDisplayName, which is a RAW indexed read that HARD-ASSERTS on a
    // value the reflected enum produces and fires an ensure on two modes this vocabulary offers -
    // the implementation carries the engine line numbers and the measured bound.
    FString GetDisplayName(EViewModeIndex ViewMode);

    // Parse a wire spelling. Case-insensitive and separator-insensitive: "front_back_face",
    // "FrontBackFace", "frontbackface" and "VMI_FrontBackFace" are the same request. Does NOT
    // consult a viewport, so a NeedsCompanion mode is always refused here; pass a client to
    // ResolveForClient to let a pre-selected sub-visualisation through.
    FViewModeResolution Resolve(const FString& Wire);

    // Resolve against the viewport that will actually render. Adds exactly one thing over
    // Resolve(): a NeedsCompanion mode whose sub-visualisation is already selected on this client
    // is accepted, with CompanionMode naming it. Nothing is written to the client.
    FViewModeResolution ResolveForClient(const FString& Wire, const FEditorViewportClient& Client);

    // Every key Resolve() accepts as a canonical spelling (one per renderable mode), sorted.
    // Used by the error messages, the docs test and the wiki. Excludes sentinels and the modes
    // that render identically to Lit, because listing a key that is always refused is worse than
    // not listing it.
    TArray<FString> RenderableKeys();

    // Every key Resolve() maps to SOME enumerator, sorted - including sentinels and Lit-identical
    // modes. Only the round-trip test needs this; callers want RenderableKeys().
    TArray<FString> AllKeys();

    // EngineShowFlags names ApplyViewMode writes differently for ViewMode than for VMI_Lit,
    // computed by running the engine's own ApplyViewMode over two copies of a fresh editor flag
    // set. Deterministic and independent of whatever the live viewport currently carries, which
    // is the point: a classification that depended on the viewport's current mode would give two
    // different answers for the same request.
    //
    // bPerspective is passed through to ApplyViewMode. UE 5.8's ApplyViewMode ignores it
    // (ShowFlags.cpp:292-446 never reads the parameter), but it is threaded rather than hardcoded
    // so a future engine that starts using it is picked up.
    TArray<FString> DistinguishingShowFlags(EViewModeIndex ViewMode, bool bPerspective = true);

    // Which of DistinguishingShowFlags() do NOT read back off `Live` with the value
    // ApplyViewMode(ViewMode) gives them. Empty means the mode reached the show flags the
    // renderer reads - the measurement that separates "applied" from "echoed".
    //
    // Compares against the engine's computation rather than against a stored expectation, so it
    // stays correct when the engine changes which flags a mode writes.
    TArray<FString> MeasureShowFlagMismatches(const FEngineShowFlags& Live, EViewModeIndex ViewMode,
        bool bPerspective = true);
}
