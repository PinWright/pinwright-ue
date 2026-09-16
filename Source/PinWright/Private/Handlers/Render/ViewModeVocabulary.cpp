// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/ViewModeVocabulary.h"

#include "Handlers/ErrorCodes.h"

#include "Compat/EngineVersionCompat.h"
#include "EditorViewportClient.h"
#include "Engine/EngineBaseTypes.h"
#include "ShowFlags.h"
#include "UObject/Class.h"
#include "UObject/ReflectedTypeAccessors.h"

namespace PinWrightViewModes
{
namespace
{
    // The published wire key differs from the enumerator name for exactly five modes. These are
    // the PRE-EXISTING contract - editor.set_view_mode has accepted them since it shipped and
    // every capture response has reported them - so they are overrides on top of the reflected
    // name rather than a reason to abandon reflection.
    //
    // Note the deliberate collision: "Wireframe" means VMI_BrushWireframe here, which is what the
    // editor UI calls "Wireframe only" and what set_view_mode has always set, while the
    // enumerator literally named VMI_Wireframe (BSP/CSG wireframe) is keyed "CSGWireframe". The
    // alias table below resolves the collision in favour of the published key, so the engine's
    // own spelling "Wireframe" resolves to VMI_BrushWireframe and VMI_Wireframe is reachable only
    // as "CSGWireframe" - which is exactly the key a capture reports for it, so the round trip
    // still closes.
    struct FKeyOverride
    {
        EViewModeIndex ViewMode;
        const TCHAR* Key;
    };

    const FKeyOverride GKeyOverrides[] = {
        { VMI_BrushWireframe,     TEXT("Wireframe") },
        { VMI_Wireframe,          TEXT("CSGWireframe") },
        { VMI_Lit_DetailLighting, TEXT("DetailLighting") },
        { VMI_CollisionPawn,      TEXT("CollisionSimple") },
        { VMI_CollisionVisibility,TEXT("CollisionComplex") },
    };

    // Spellings editor.set_view_mode has accepted since it shipped, kept working so no stored
    // request breaks. Every one of them is a SYNONYM for a key above; none of them introduces a
    // mode the reflected table does not already carry.
    struct FAlias
    {
        const TCHAR* Spelling;
        EViewModeIndex ViewMode;
    };

    const FAlias GAliases[] = {
        { TEXT("worldcollision"),      VMI_CollisionPawn },
        { TEXT("playercollision"),     VMI_CollisionPawn },
        { TEXT("precisecollision"),    VMI_CollisionVisibility },
        { TEXT("visibilitycollision"), VMI_CollisionVisibility },
        { TEXT("collisionvis"),        VMI_CollisionVisibility },
    };

    // The engine's `viewmode` console command string-matches against GetViewModeName
    // (ShowFlags.cpp:1017-1018), which spells VMI_CollisionVisibility "CollisionVis" rather than
    // the enumerator name. GetViewModeName carries no ENGINE_API, so the two exceptions are
    // spelled here rather than linked. Every other mode's exec token equals its key.
    struct FExecName
    {
        EViewModeIndex ViewMode;
        const TCHAR* ExecName;
    };

    const FExecName GExecNames[] = {
        { VMI_CollisionPawn,       TEXT("CollisionPawn") },
        { VMI_CollisionVisibility, TEXT("CollisionVis") },
    };

    // The ten modes whose picture is chosen by a sub-visualisation the mode name does not carry.
    //
    // This one IS a hand-written table, and it has to be: the companion is a plain FName member on
    // FEditorViewportClient (EditorViewportClient.h:1991-2001) with no registry, no reflection and
    // no generic accessor. It is a property of the viewport client, not of EViewModeIndex, so
    // there is nothing to derive it from. The list is transcribed from that member block, and
    // FViewModeCompanionTableIsCompleteTest pins the count so a new engine member cannot be added
    // without this table noticing.
    //
    // WHY NOT SET IT. PinWright refuses to write the companion rather than accepting a
    // `viewModeCompanion` string, because validating that string needs a different engine registry
    // per mode (GetBufferVisualizationData, GetNaniteVisualizationData, GetLumenVisualizationData,
    // ...) and an unvalidated name renders the mode's overview default instead - which is the
    // silent-wrong-picture failure this whole parameter exists to prevent. A mode whose companion
    // is ALREADY selected in the editor is accepted, because that needs no write and therefore no
    // restore.
    struct FCompanionMode
    {
        EViewModeIndex ViewMode;
        // The member that carries the selection, read directly (it is public).
        FName FEditorViewportClient::* Member;
        // What the caller has to pick, named the way the editor's viewport menu names it.
        const TCHAR* CompanionLabel;
    };

    const FCompanionMode GCompanionModes[] = {
        { VMI_VisualizeBuffer,            &FEditorViewportClient::CurrentBufferVisualizationMode,           TEXT("a buffer visualization target (Buffer Visualization menu)") },
        { VMI_VisualizeNanite,            &FEditorViewportClient::CurrentNaniteVisualizationMode,           TEXT("a Nanite visualization target (Nanite Visualization menu)") },
        { VMI_VisualizeLumen,             &FEditorViewportClient::CurrentLumenVisualizationMode,            TEXT("a Lumen visualization target (Lumen Visualization menu)") },
        // MegaLights got its own view mode and its own viewport-client member in 5.8; before that
        // there is neither an enumerator nor a companion to point at.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        { VMI_VisualizeMegaLights,        &FEditorViewportClient::CurrentMegaLightsVisualizationMode,       TEXT("a MegaLights visualization target (MegaLights Visualization menu)") },
#endif
        // UE 5.3 already spells the view mode VMI_VisualizeSubstrate but still carries the
        // pre-rename viewport-client member CurrentStrataVisualizationMode; 5.4 renamed the member
        // to match. Same companion, two spellings.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        { VMI_VisualizeSubstrate,         &FEditorViewportClient::CurrentSubstrateVisualizationMode,        TEXT("a Substrate visualization target (Substrate Visualization menu)") },
#else
        { VMI_VisualizeSubstrate,         &FEditorViewportClient::CurrentStrataVisualizationMode,           TEXT("a Substrate visualization target (Substrate Visualization menu)") },
#endif
        { VMI_VisualizeGroom,             &FEditorViewportClient::CurrentGroomVisualizationMode,            TEXT("a Groom visualization target (Groom Visualization menu)") },
        { VMI_VisualizeVirtualShadowMap,  &FEditorViewportClient::CurrentVirtualShadowMapVisualizationMode, TEXT("a Virtual Shadow Map visualization target") },
        // The Virtual Texture view mode and its viewport-client member both arrived in UE 5.6;
        // before that there is neither an enumerator nor a companion to point at.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        { VMI_VisualizeVirtualTexture,    &FEditorViewportClient::CurrentVirtualTextureVisualizationMode,   TEXT("a Virtual Texture visualization target") },
#endif
        { VMI_RayTracingDebug,            &FEditorViewportClient::CurrentRayTracingDebugVisualizationMode,  TEXT("a ray tracing debug target (Ray Tracing Debug menu)") },
        { VMI_VisualizeGPUSkinCache,      &FEditorViewportClient::CurrentGPUSkinCacheVisualizationMode,     TEXT("a GPU Skin Cache visualization target") },
    };

    const FCompanionMode* FindCompanion(EViewModeIndex ViewMode)
    {
        for (const FCompanionMode& Entry : GCompanionModes)
        {
            if (Entry.ViewMode == ViewMode)
            {
                return &Entry;
            }
        }
        return nullptr;
    }

    // Lowercase, drop every separator, drop a leading "vmi". Makes "front_back_face",
    // "FrontBackFace", "front-back-face" and "VMI_FrontBackFace" one spelling, so snake_case works
    // for every mode without a per-mode entry.
    FString Normalize(const FString& In)
    {
        FString Out;
        Out.Reserve(In.Len());
        for (const TCHAR Ch : In)
        {
            if (FChar::IsAlnum(Ch))
            {
                Out.AppendChar(FChar::ToLower(Ch));
            }
        }
        if (Out.StartsWith(TEXT("vmi"), ESearchCase::CaseSensitive) && Out.Len() > 3)
        {
            Out.RightChopInline(3);
        }
        return Out;
    }

    // Every enumerator of EViewModeIndex that names a real value, in declaration order.
    // UHT appends a synthetic <EnumName>_MAX; it and the two sentinels are carried here (so a
    // request for one gets a specific refusal rather than "unknown") and rejected by Classify.
    struct FEnumEntry
    {
        EViewModeIndex Value = VMI_Lit;
        FString EnumName;   // "VMI_FrontBackFace"
        FString BareName;   // "FrontBackFace"
        bool bHidden = false;
        bool bSentinel = false;
    };

    const TArray<FEnumEntry>& EnumEntries()
    {
        static const TArray<FEnumEntry> Entries = []()
        {
            TArray<FEnumEntry> Built;
            const UEnum* Enum = StaticEnum<EViewModeIndex>();
            if (!Enum)
            {
                return Built;
            }
            Built.Reserve(Enum->NumEnums());
            for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
            {
                const FString Name = Enum->GetNameStringByIndex(Index);
                if (Name.IsEmpty())
                {
                    continue;
                }
                FEnumEntry Entry;
                Entry.Value = static_cast<EViewModeIndex>(Enum->GetValueByIndex(Index));
                Entry.EnumName = Name;
                Entry.BareName = Name.StartsWith(TEXT("VMI_")) ? Name.RightChop(4) : Name;
                // UMETA(Hidden) marks VMI_Max and the deprecated VMI_Lit_Wireframe. Read from
                // metadata rather than listed, so a mode the engine hides later is hidden here
                // too.
                Entry.bHidden = Enum->HasMetaData(TEXT("Hidden"), Index);
                // The two names that are sentinels regardless of metadata: VMI_Unknown carries a
                // DisplayName and no Hidden flag, and UHT's synthetic _MAX has neither.
                Entry.bSentinel = Entry.bHidden
                    || Name.EndsWith(TEXT("_MAX"))
                    || Name == TEXT("VMI_Max")
                    || Name == TEXT("VMI_Unknown");
                Built.Add(MoveTemp(Entry));
            }
            return Built;
        }();
        return Entries;
    }

    const FEnumEntry* FindEntry(EViewModeIndex ViewMode)
    {
        for (const FEnumEntry& Entry : EnumEntries())
        {
            if (Entry.Value == ViewMode)
            {
                return &Entry;
            }
        }
        return nullptr;
    }

    // The enumerator's own UMETA(DisplayName), which every value of EViewModeIndex carries and
    // which no engine table gates. Used wherever the engine's display-name table cannot be asked.
    FString ReflectedDisplayName(EViewModeIndex ViewMode)
    {
        if (const UEnum* Enum = StaticEnum<EViewModeIndex>())
        {
            const int32 Index = Enum->GetIndexByValue(static_cast<int64>(ViewMode));
            if (Index != INDEX_NONE)
            {
                const FString Name = Enum->GetDisplayNameTextByIndex(Index).ToString();
                if (!Name.IsEmpty())
                {
                    return Name;
                }
            }
        }
        if (const FEnumEntry* Entry = FindEntry(ViewMode))
        {
            return Entry->BareName;
        }
        return FString();
    }

    // Spelling -> mode. Built once. Insert order matters and is the collision policy: reflected
    // enumerator names first, then the published key overrides, then the legacy aliases, so a
    // later entry deliberately wins ("wireframe" ends up on VMI_BrushWireframe).
    const TMap<FString, EViewModeIndex>& SpellingMap()
    {
        static const TMap<FString, EViewModeIndex> Map = []()
        {
            TMap<FString, EViewModeIndex> Built;
            for (const FEnumEntry& Entry : EnumEntries())
            {
                // FIRST reflected entry wins, unlike the two hand-written tables below, which
                // deliberately overwrite. Two reflected entries colliding is a UHT artifact, not a
                // policy, and one such collision exists: UHT names the synthetic terminator after
                // the enumerators' common prefix, so it is "VMI_MAX" and normalises to the same
                // "max" as the real VMI_Max. Last-wins put that spelling on VMI_Unknown + 1 (256),
                // a value outside every engine table keyed by EViewModeIndex - which is how
                // Resolve("VMI_Max") reached UViewModeUtils::GetViewModeDisplayName's raw indexed
                // read one past the end and took the process down. First-wins keeps the spelling on
                // the declared enumerator; both are sentinels, so the refusal is unchanged.
                Built.FindOrAdd(Normalize(Entry.BareName), Entry.Value);
            }
            for (const FKeyOverride& Override : GKeyOverrides)
            {
                Built.Add(Normalize(Override.Key), Override.ViewMode);
            }
            for (const FAlias& Alias : GAliases)
            {
                Built.Add(Normalize(Alias.Spelling), Alias.ViewMode);
            }
            return Built;
        }();
        return Map;
    }

    // Index + name for every built-in engine show flag. Custom (plugin-registered) flags are
    // skipped: ApplyViewMode never writes them, so they can only add noise to a diff.
    struct FShowFlagIndexSink
    {
        TArray<TPair<uint32, FString>>* Out = nullptr;
        bool OnEngineShowFlag(uint32 InIndex, const FString& InName)
        {
            Out->Emplace(InIndex, InName);
            return true;
        }
        bool OnCustomShowFlag(uint32 /*InIndex*/, const FString& /*InName*/) { return true; }
    };

    const TArray<TPair<uint32, FString>>& AllShowFlagIndices()
    {
        static const TArray<TPair<uint32, FString>> Indices = []()
        {
            TArray<TPair<uint32, FString>> Built;
            FShowFlagIndexSink Sink;
            Sink.Out = &Built;
            FEngineShowFlags::IterateAllFlags(Sink);
            return Built;
        }();
        return Indices;
    }

    // A fresh editor flag set with ApplyViewMode(ViewMode) run over it. The base is deliberately
    // a FRESH ESFIM_Editor set rather than the live viewport's flags: the classification has to
    // give the same answer for the same mode whatever the viewport currently renders, and a base
    // that varied would make "does this mode differ from Lit" depend on the mode the viewport
    // happened to be in.
    FEngineShowFlags ApplyToFreshEditorFlags(EViewModeIndex ViewMode, bool bPerspective)
    {
        FEngineShowFlags Flags(ESFIM_Editor);
        ApplyViewMode(ViewMode, bPerspective, Flags);
        return Flags;
    }
}

FString GetKey(EViewModeIndex ViewMode)
{
    for (const FKeyOverride& Override : GKeyOverrides)
    {
        if (Override.ViewMode == ViewMode)
        {
            return Override.Key;
        }
    }
    if (const FEnumEntry* Entry = FindEntry(ViewMode))
    {
        // VMI_Max and the synthetic _MAX have no useful key; report them the way the pre-existing
        // table did, as "Unknown", so nothing downstream has to special-case a new string.
        if (Entry->EnumName.EndsWith(TEXT("_MAX")) || Entry->EnumName == TEXT("VMI_Max")
            || Entry->EnumName == TEXT("VMI_Unknown"))
        {
            return TEXT("Unknown");
        }
        return Entry->BareName;
    }
    return TEXT("Unknown");
}

FString GetDisplayName(EViewModeIndex ViewMode)
{
    // WHY THIS WRAPS THE ENGINE CALL. UViewModeUtils::GetViewModeDisplayName
    // (UE 5.8 Runtime/Engine/Private/ViewModeNames.cpp:260-265) is a RAW indexed read of a
    // file-static TArray with no bound of its own, and this vocabulary reaches two classes of
    // value it cannot answer for.
    //
    // THE BOUND, measured from the engine rather than guessed. FillViewModeDisplayNames (:10)
    // Reserve()s and then Emplace()s exactly once per index over `Index < VMI_Unknown + 1`,
    // including an empty entry for every unhandled index (:250-253), so the table's Num() is
    // exactly VMI_Unknown + 1 == 256 and the only valid indices are [0, VMI_Unknown]. UHT appends
    // a synthetic terminator to the reflected enum at VMI_Unknown + 1 == 256; indexing with it is
    // a hard TArray assert ("Array index out of bounds: 256 into an array of size 256"), and the
    // engine's own ensureMsgf about "an unknown value of EViewModeIndex" on :263 is UNREACHABLE
    // for it, because the indexing fails one line earlier.
    //
    // THE GAPS. Inside the bound, every index FillViewModeDisplayNames has no branch for holds
    // FText::GetEmpty(), and :263 ensures on exactly that. Two of them are modes this file
    // reports as RENDERABLE: VMI_VisualizeSubstrate (34) and VMI_VisualizeGroom (35) have icons
    // (:284-285, :402-406) but no display-name branch, so resolving either fired an ensure with a
    // full callstack in the middle of an otherwise clean suite run. They are the only two - every
    // other assigned enumerator of EViewModeIndex has a branch (checked against the engine header,
    // UE 5.8 EngineBaseTypes.h:1003-1117).
    //
    // Both classes are answered from the enumerator's own UMETA(DisplayName), the same reflected
    // source the rest of this file derives from. The empty-result fallback at the end is not dead
    // code: it is where a FUTURE engine mode with no display-name branch lands, at the cost of one
    // ensure the first time that mode is asked for.
    const int32 Value = static_cast<int32>(ViewMode);
    if (Value < 0 || Value > static_cast<int32>(VMI_Unknown)
        || ViewMode == VMI_VisualizeSubstrate || ViewMode == VMI_VisualizeGroom)
    {
        return ReflectedDisplayName(ViewMode);
    }
    const FString EngineName = UViewModeUtils::GetViewModeDisplayName(ViewMode).ToString();
    return EngineName.IsEmpty() ? ReflectedDisplayName(ViewMode) : EngineName;
}

TArray<FString> DistinguishingShowFlags(EViewModeIndex ViewMode, bool bPerspective)
{
    TArray<FString> Names;
    if (ViewMode == VMI_Lit)
    {
        return Names;
    }
    const FEngineShowFlags Target = ApplyToFreshEditorFlags(ViewMode, bPerspective);
    const FEngineShowFlags LitReference = ApplyToFreshEditorFlags(VMI_Lit, bPerspective);
    for (const TPair<uint32, FString>& Flag : AllShowFlagIndices())
    {
        if (Target.GetSingleFlag(Flag.Key) != LitReference.GetSingleFlag(Flag.Key))
        {
            Names.Add(Flag.Value);
        }
    }
    return Names;
}

TArray<FString> MeasureShowFlagMismatches(const FEngineShowFlags& Live, EViewModeIndex ViewMode,
    bool bPerspective)
{
    TArray<FString> Mismatches;
    const FEngineShowFlags Expected = ApplyToFreshEditorFlags(ViewMode, bPerspective);
    // Only the flags that distinguish this mode from Lit are checked. The rest of the set carries
    // whatever the user's viewport carries, and asserting on those would fail on a viewport with
    // fog turned off by hand.
    const FEngineShowFlags LitReference = ApplyToFreshEditorFlags(VMI_Lit, bPerspective);
    for (const TPair<uint32, FString>& Flag : AllShowFlagIndices())
    {
        if (Expected.GetSingleFlag(Flag.Key) == LitReference.GetSingleFlag(Flag.Key))
        {
            continue;
        }
        if (Live.GetSingleFlag(Flag.Key) != Expected.GetSingleFlag(Flag.Key))
        {
            Mismatches.Add(Flag.Value);
        }
    }
    return Mismatches;
}

namespace
{
    // Does the engine itself strip this mode on this build?
    //
    // FEditorViewportClient::Draw runs EngineShowFlagOverride over the view family's flags every
    // frame (EditorViewportClient.cpp:4866), and that function clears PathTracing and
    // RayTracingDebug outright when !IsRayTracingEnabled() (ShowFlags.cpp:513-517). Running the
    // same function here answers "would this mode survive to the renderer" without duplicating a
    // single feature check, so a gate the engine adds later is picked up for free.
    //
    // bCanDisableTonemapper is false because Draw passes true only for a buffer / GPU-skin-cache /
    // ray-tracing-debug mode that already has a companion selected, and those modes take the
    // NeedsCompanion path before reaching here.
    bool SurvivesEngineOverride(EViewModeIndex ViewMode, bool bPerspective, FString& OutClearedFlag)
    {
        OutClearedFlag.Reset();
        const FEngineShowFlags Raw = ApplyToFreshEditorFlags(ViewMode, bPerspective);
        const FEngineShowFlags LitReference = ApplyToFreshEditorFlags(VMI_Lit, bPerspective);
        FEngineShowFlags Effective = Raw;
        EngineShowFlagOverride(ESFIM_Editor, ViewMode, Effective, /*bCanDisableTonemapper=*/false);
        for (const TPair<uint32, FString>& Flag : AllShowFlagIndices())
        {
            // Only a flag this mode turns ON, and only one that distinguishes it from Lit: a
            // mode is "unavailable" when the thing that makes it visible gets stripped, not when
            // the override happens to touch some unrelated bit.
            if (!Raw.GetSingleFlag(Flag.Key) || LitReference.GetSingleFlag(Flag.Key))
            {
                continue;
            }
            if (!Effective.GetSingleFlag(Flag.Key))
            {
                OutClearedFlag = Flag.Value;
                return false;
            }
        }
        return true;
    }

    FViewModeResolution Classify(EViewModeIndex ViewMode, const FEditorViewportClient* Client)
    {
        FViewModeResolution Result;
        Result.ViewMode = ViewMode;
        Result.Key = GetKey(ViewMode);
        // Never the raw engine call: this walk reaches sentinels and the two modes the engine's
        // display-name table has no entry for. See GetDisplayName.
        Result.DisplayName = GetDisplayName(ViewMode);
        for (const FExecName& Exec : GExecNames)
        {
            if (Exec.ViewMode == ViewMode)
            {
                Result.ExecName = Exec.ExecName;
            }
        }
        if (ViewMode == VMI_CollisionPawn)
        {
            Result.CollisionChannelName = TEXT("collisionSimple");
        }
        else if (ViewMode == VMI_CollisionVisibility)
        {
            Result.CollisionChannelName = TEXT("collisionComplex");
        }

        const FEnumEntry* Entry = FindEntry(ViewMode);
        if (Entry && Entry->bSentinel)
        {
            Result.Status = EViewModeStatus::Sentinel;
            Result.ErrorCode = ErrorCodes::ERR_UNKNOWN_VIEW_MODE;
            Result.ErrorMessage = FString::Printf(
                TEXT("'%s' is a sentinel of EViewModeIndex, not a renderable view mode, and no ")
                TEXT("viewport can be put into it. Pick one of: %s."),
                *Entry->EnumName, *FString::Join(RenderableKeys(), TEXT(", ")));
            return Result;
        }

        const bool bPerspective = Client ? Client->IsPerspective() : true;
        Result.DistinguishingShowFlags = DistinguishingShowFlags(ViewMode, bPerspective);

        if (ViewMode != VMI_Lit && Result.DistinguishingShowFlags.Num() == 0)
        {
            Result.Status = EViewModeStatus::IndistinctFromLit;
            Result.ErrorCode = ErrorCodes::ERR_VIEW_MODE_NOT_RENDERABLE;
            Result.ErrorMessage = FString::Printf(
                TEXT("View mode '%s' sets no EngineShowFlags that Lit does not, so a capture in it ")
                TEXT("would be a Lit capture with a different label - the engine's ApplyViewMode ")
                TEXT("(UE 5.8 Runtime/Engine/Private/ShowFlags.cpp:292) has no case for it. ")
                TEXT("VMI_GroupLODColoration is a menu grouping item rather than a render mode, and ")
                TEXT("VMI_Lit_Wireframe was deprecated in 5.8 in favour of Lit plus the MeshEdges ")
                TEXT("show flag. Use 'Lit', or 'LODColoration' / 'HLODColoration' for the coloration ")
                TEXT("modes that do render."),
                *Result.Key);
            return Result;
        }

        if (const FCompanionMode* Companion = FindCompanion(ViewMode))
        {
            const FName Selected = Client ? (Client->*(Companion->Member)) : NAME_None;
            if (Selected.IsNone())
            {
                Result.Status = EViewModeStatus::NeedsCompanion;
                Result.ErrorCode = ErrorCodes::ERR_VIEW_MODE_NEEDS_COMPANION;
                Result.ErrorMessage = FString::Printf(
                    TEXT("View mode '%s' does not describe a picture on its own: it renders %s, ")
                    TEXT("which is a separate selection on the viewport client ")
                    TEXT("(FEditorViewportClient::Current*VisualizationMode) and is currently unset, ")
                    TEXT("so the frame would show the mode's overview default rather than anything ")
                    TEXT("this request named. PinWright will not set it, because validating a ")
                    TEXT("sub-visualisation name needs a different engine registry per mode and an ")
                    TEXT("unvalidated name renders the same wrong picture silently. Select the target ")
                    TEXT("in the editor's viewport menu once, then re-issue this call - a ")
                    TEXT("pre-selected target is accepted and reported back."),
                    *Result.Key, Companion->CompanionLabel);
                return Result;
            }
            Result.CompanionMode = Selected.ToString();
        }

        FString ClearedFlag;
        if (!SurvivesEngineOverride(ViewMode, bPerspective, ClearedFlag))
        {
            Result.Status = EViewModeStatus::Unavailable;
            Result.ErrorCode = ErrorCodes::ERR_VIEW_MODE_UNAVAILABLE;
            Result.ErrorMessage = FString::Printf(
                TEXT("View mode '%s' is not available on this build: the engine's own ")
                TEXT("EngineShowFlagOverride (UE 5.8 Runtime/Engine/Private/ShowFlags.cpp:448) ")
                TEXT("clears the '%s' show flag this mode depends on before the frame is drawn, ")
                TEXT("which is what happens to the ray-tracing modes when ray tracing is disabled. ")
                TEXT("Setting it anyway would render a plausible near-Lit picture that is not the ")
                TEXT("mode that was asked for, so the request is refused instead."),
                *Result.Key, *ClearedFlag);
            return Result;
        }

        Result.Status = EViewModeStatus::Ok;
        return Result;
    }

    FViewModeResolution ResolveInternal(const FString& Wire, const FEditorViewportClient* Client)
    {
        const FString Normalized = Normalize(Wire);
        if (Normalized.IsEmpty())
        {
            FViewModeResolution Result;
            Result.Status = EViewModeStatus::Unknown;
            Result.ErrorCode = ErrorCodes::ERR_UNKNOWN_VIEW_MODE;
            Result.ErrorMessage = FString::Printf(
                TEXT("viewMode was empty. Recognised: %s. Spelling is case- and ")
                TEXT("separator-insensitive, so front_back_face, FrontBackFace and ")
                TEXT("VMI_FrontBackFace are the same request."),
                *FString::Join(RenderableKeys(), TEXT(", ")));
            return Result;
        }
        const EViewModeIndex* Found = SpellingMap().Find(Normalized);
        if (!Found)
        {
            FViewModeResolution Result;
            Result.Status = EViewModeStatus::Unknown;
            Result.ErrorCode = ErrorCodes::ERR_UNKNOWN_VIEW_MODE;
            Result.ErrorMessage = FString::Printf(
                TEXT("Unrecognised view mode '%s'. Recognised: %s. Spelling is case- and ")
                TEXT("separator-insensitive, so front_back_face, FrontBackFace and ")
                TEXT("VMI_FrontBackFace are the same request."),
                *Wire, *FString::Join(RenderableKeys(), TEXT(", ")));
            return Result;
        }
        return Classify(*Found, Client);
    }
}

FViewModeResolution Resolve(const FString& Wire)
{
    return ResolveInternal(Wire, nullptr);
}

FViewModeResolution ResolveForClient(const FString& Wire, const FEditorViewportClient& Client)
{
    return ResolveInternal(Wire, &Client);
}

TArray<FString> AllKeys()
{
    TArray<FString> Keys;
    for (const FEnumEntry& Entry : EnumEntries())
    {
        const FString Key = GetKey(Entry.Value);
        if (Key != TEXT("Unknown"))
        {
            Keys.AddUnique(Key);
        }
    }
    Keys.Sort();
    return Keys;
}

TArray<FString> RenderableKeys()
{
    // Built without a client, so the ten companion modes are listed (they ARE reachable once a
    // target is selected) but the sentinels and the Lit-identical modes are not: offering a key
    // that is refused on every call is worse than omitting it.
    static const TArray<FString> Keys = []()
    {
        TArray<FString> Built;
        for (const FEnumEntry& Entry : EnumEntries())
        {
            if (Entry.bSentinel)
            {
                continue;
            }
            if (Entry.Value != VMI_Lit && DistinguishingShowFlags(Entry.Value, true).Num() == 0)
            {
                continue;
            }
            const FString Key = GetKey(Entry.Value);
            if (Key != TEXT("Unknown"))
            {
                Built.AddUnique(Key);
            }
        }
        Built.Sort();
        return Built;
    }();
    return Keys;
}
}
