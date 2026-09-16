// Copyright (c) 2026 Alexander Penkin. MIT License.

// Cross-verb parameter parity: verbs that share a helper must expose the same knobs.
//
// WHY THIS FILE EXISTS. The capture-convergence wave built a verb x DOMAIN matrix and closed every
// cell, and a parameter-parity defect was found immediately afterwards
// (docs/preview-scene-rig.md 5.1): `camera.frame_actor` and `camera.orbit_shots` both call
// `ComputeFitDistance(Radius, Fov, Padding)` and each exposed the OPPOSITE half of it - one the
// margin with the distance welded, the other the distance with the margin welded. A fully green
// row of that matrix was compatible with a welded constant, because both of its matrices used the
// same column axis (the six subject domains) and neither axis was "the inputs of the helper these
// verbs share". Nothing checked that two verbs on one code path offer the same surface. This file
// is that check.
//
// WHAT IS DERIVED AND WHAT IS WRITTEN DOWN. A hand-maintained inventory of verbs drifts the moment
// someone adds a verb, which is the exact failure mode being guarded against, so:
//   * the VERBS come from the real registry - `FAutoRegisterHandler::GetPendingRegistrations()`,
//     the array `FRpcDispatcher::DrainAutoRegistrations` copies into the dispatch map. Nothing
//     here greps a source file for a verb name.
//   * the GROUP MEMBERSHIP comes from the source: a verb is a member of a helper's group when the
//     helper is called inside that verb's `REGISTER_RPC_HANDLER` block. Add a verb that calls
//     `ComputeFitDistance` and it joins the group at the next run with no edit here.
//   * only the SLOT VOCABULARY is written down - which wire spellings correspond to a helper's
//     inputs. That is a handful of short tables, each guarded for non-vacuity (a spelling no
//     registered verb declares anywhere is treated as a typo in the table, not as a gap in every
//     verb).
//
// HOW THE DELIBERATELY-OPEN GAPS ARE HANDLED. 5.3 of the plan fixes P1 only; P2-P9 stay open by
// design, because each of them moves a default that has already shipped. They are therefore
// EXCEPTION ROWS in `GSlotExceptions` / `GPoseListExceptions` below, not skips:
//   * every row names the verb, the slot and the KIND of gap, so the table reads as an inventory
//     of what is wrong rather than as a list of things not to look at;
//   * every row carries a `file:line` citation plus a distinctive anchor phrase, and
//     `EveryExceptionCitesARationale` opens the file and matches the anchor. A citation that is
//     merely non-empty cannot pass. This is the rule `docs/tools/check_hazards.py` applies to the
//     hazard corpus, with the same tolerance idea (it uses NEAR_LINES = 3; this file uses 12
//     because it cites files that other chunks of this wave are editing while it runs);
//   * an anchor that does not itself contain the parameter name or the verb name is REJECTED, so
//     a row cannot be made to pass by choosing an anchor that matches everywhere;
//   * an exception row whose gap no longer exists FAILS. The table cannot rot into a permanent
//     licence: fixing P2 turns the row that documented it into a test failure that says "delete
//     me". That is what makes this a to-do list with teeth rather than a suppression file.
//
// COUNTERFACTUAL. Delete `GSlotExceptions` and `SharedHelperInputsAreExposedUniformly` reports
// fifteen gaps: `render.capture_asset_preview` with neither `padding` nor a distance override
// (P2), the padding description and published default two verbs do not share (P3, plus the fork
// R5's new PINWRIGHT_FIT_PADDING_PARAM_DESC introduced), four verbs with no `orthoWidth` (P4),
// `render.capture_ortho_tiles` forking the exposure vocabulary in two directions (P9),
// `distribution` with no shared macro at all (P8), and the one row that is NOT a defect -
// `render.capture_open_level` refusing the preview-scene rig on purpose (decision 9). Delete
// `GPoseListExceptions` and `PoseListSetFieldsAreReachableFromTheWire` reports twenty-one, five of
// which are P5. Both lists ARE the defect backlog; they are not decoration.
//
// THE WIDEST GAP, P5, IS ALSO ASSERTED DIRECTLY. `FPoseListCaptureRequest` carries
// `ViewDistanceScale` / `bViewDistanceScaleProvided` / `bAutoViewDistanceScale`, `MakeFrameRequest`
// forwards all three into every frame of every set, no handler assigns any of them, and the wire
// spelling `viewDistanceScale` is declared only by two verbs that never build an
// `FPoseListCaptureRequest` at all. Dead on the path that carries it, unreachable from the verbs
// that carry it. `PoseListSetFieldsAreReachableFromTheWire` states all three halves separately, so
// the day any one of them changes the test says which.
//
// WHAT THIS FILE DELIBERATELY DOES NOT DO.
//   * It checks description parity only for slots that HAVE a shared macro to check against
//     (`padding`, `exposure`, `viewMode`, `previewScene`). A slot with no macro is judged only when
//     the model says one is wanted - which today is `distribution`, and that expectation is itself
//     the P8 row. Every other slot's prose is unjudged, deliberately: this test is about reachable
//     inputs, not about writing style.
//   * It scans only `.cpp` files under `Handlers/Render` that register at least one verb. A helper
//     call inside such a file that falls outside every registration block is a HARD FAILURE (the
//     attribution model cannot place it); translation units with no registrations are out of
//     scope, and each tracked helper's DEFINITION is separately asserted to still exist so that a
//     rename cannot silently empty a group.
//   * It does not judge whether a default is the right number. It judges whether every verb on one
//     helper can reach the same inputs, with the same vocabulary, and publish the same default.

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Render/CameraShotPlanUtils.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"

// Named namespace, not anonymous: the plugin's tests share one module and Unity merges translation
// units, so identically-named anonymous-namespace helpers collide at ODR. Same convention as
// Tests/Infra/ParamSpecTestHelpers.h and Tests/Infra/DispatcherTestHelpers.h.
namespace CaptureVerbParameterParityTest
{
    // Tolerance, in lines, when resolving an exception row's `file:line` citation. See the header
    // comment: check_hazards.py uses 3 for files it also owns; this file cites sources that other
    // chunks of the same wave are editing while it runs.
    constexpr int32 GCitationSlackLines = 12;

    // ============================================================================
    // The registry side
    // ============================================================================

    // Every registered verb, by method name. Read from the auto-registration array rather than
    // from the dispatcher's map so the walk works with or without a live subsystem; the dispatcher
    // is populated FROM this array (RpcDispatcher.cpp, DrainAutoRegistrations), so the two cannot
    // disagree about a verb's declared parameters.
    inline TMap<FString, const FHandlerRegistration*> LoadRegistry()
    {
        TMap<FString, const FHandlerRegistration*> ByMethod;
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            ByMethod.Add(Reg.MethodName, &Reg);
        }
        return ByMethod;
    }

    // A declared parameter, matched by its own name OR by any registered alias - an alias is a wire
    // spelling the dispatcher accepts, so a verb reachable only through its alias is not a gap.
    inline const FParamSpec* FindParam(const FHandlerRegistration& Reg, const FString& Name)
    {
        for (const FParamSpec& Spec : Reg.Params)
        {
            if (Spec.Name.Equals(Name, ESearchCase::CaseSensitive))
            {
                return &Spec;
            }
            for (const FString& Alias : Spec.Aliases)
            {
                if (Alias.Equals(Name, ESearchCase::CaseSensitive))
                {
                    return &Spec;
                }
            }
            for (const FParamAliasSpec& Alias : Spec.TypedAliases)
            {
                if (Alias.Name.Equals(Name, ESearchCase::CaseSensitive))
                {
                    return &Spec;
                }
            }
        }
        return nullptr;
    }

    // The first of `Spellings` this verb declares, or nullptr when it declares none of them. A slot
    // can carry several spellings on purpose: `camera.orbit_shots` names its distance override
    // `radius` and `camera.frame_actor` names it `distance`, and both satisfy the slot.
    inline const FParamSpec* FindSlotParam(const FHandlerRegistration& Reg, const TArray<FString>& Spellings)
    {
        for (const FString& Spelling : Spellings)
        {
            if (const FParamSpec* Spec = FindParam(Reg, Spelling))
            {
                return Spec;
            }
        }
        return nullptr;
    }

    // PURE, and shared by the live walk and by TheKnownDefectIsCaught's synthetic pair: given the
    // members of a helper group and a slot's accepted spellings, which members reach none of them.
    // A free function precisely so the synthetic pre-fix model can be fed to the SAME code the
    // registry walk uses - a second, hand-written evaluator would prove nothing about the first.
    inline TArray<FString> FindDeclarationGaps(
        const TArray<const FHandlerRegistration*>& Members, const TArray<FString>& Spellings)
    {
        TArray<FString> Gaps;
        for (const FHandlerRegistration* Member : Members)
        {
            if (Member != nullptr && FindSlotParam(*Member, Spellings) == nullptr)
            {
                Gaps.Add(Member->MethodName);
            }
        }
        Gaps.Sort();
        return Gaps;
    }

    inline TArray<FString> SplitSpellings(const TCHAR* CommaSeparated)
    {
        TArray<FString> Out;
        FString(CommaSeparated).ParseIntoArray(Out, TEXT(","), /*InCullEmpty=*/true);
        for (FString& Entry : Out)
        {
            Entry.TrimStartAndEndInline();
        }
        return Out;
    }

    // ============================================================================
    // The source side: registration blocks and helper call sites
    // ============================================================================

    inline FString ResolvePluginDir()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
    }

    inline FString ResolveRenderHandlerDir()
    {
        const FString PluginDir = ResolvePluginDir();
        return PluginDir.IsEmpty()
            ? FString()
            : (PluginDir / TEXT("Source") / TEXT("PinWright") / TEXT("Private")
                / TEXT("Handlers") / TEXT("Render"));
    }

    // Remove the `//` tail and whole comment lines. A helper NAMED in a comment is not a call site,
    // and attributing one would put a verb into a group it does not belong to - several of these
    // files discuss their shared helpers at length directly above the code that calls them.
    inline FString StripComment(const FString& Line)
    {
        const FString Trimmed = Line.TrimStart();
        if (Trimmed.StartsWith(TEXT("//")) || Trimmed.StartsWith(TEXT("*")) || Trimmed.StartsWith(TEXT("/*")))
        {
            return FString();
        }
        const int32 Comment = Line.Find(TEXT("//"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
        return (Comment == INDEX_NONE) ? Line : Line.Left(Comment);
    }

    struct FVerbBlock
    {
        FString Method;
        FString File;             // absolute
        FString FileLeaf;
        int32   FirstLine = 0;    // 1-based; the REGISTER_RPC_HANDLER line
        int32   LastLine = 0;     // 1-based, inclusive
    };

    struct FScannedSource
    {
        TArray<FString>                RegisteringFiles;   // absolute, sorted
        TMap<FString, TArray<FString>> StrippedLines;      // absolute path -> comment-stripped lines
        TArray<FVerbBlock>             Blocks;
        bool                           bUsable = false;
    };

    // Walk every `.cpp` under Handlers/Render, keep the ones that register at least one verb, and
    // cut each into [REGISTER line, next REGISTER line) blocks. Deliberately NOT a guess about
    // where a handler body ends: a file's free functions live above its first registration or
    // between two of them, and a call site landing outside every block is REPORTED, never assumed.
    inline FScannedSource ScanRenderSources()
    {
        FScannedSource Out;
        const FString Dir = ResolveRenderHandlerDir();
        if (Dir.IsEmpty() || !IFileManager::Get().DirectoryExists(*Dir))
        {
            return Out;
        }

        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.cpp"), true, false, false);
        Files.Sort();

        const FRegexPattern RegisterPattern(TEXT("REGISTER_RPC_HANDLER\\s*\\(\\s*\"([^\"]+)\""));

        for (const FString& File : Files)
        {
            TArray<FString> Lines;
            if (!FFileHelper::LoadFileToStringArray(Lines, *File))
            {
                continue;
            }

            TArray<int32>   RegisterLines;
            TArray<FString> RegisterMethods;
            TArray<FString> Stripped;
            Stripped.Reserve(Lines.Num());
            for (int32 Index = 0; Index < Lines.Num(); ++Index)
            {
                Stripped.Add(StripComment(Lines[Index]));
                FRegexMatcher Matcher(RegisterPattern, Stripped[Index]);
                if (Matcher.FindNext())
                {
                    RegisterLines.Add(Index + 1);
                    RegisterMethods.Add(Matcher.GetCaptureGroup(1));
                }
            }

            if (RegisterLines.Num() == 0)
            {
                continue;   // utility translation unit; out of scope by construction
            }

            Out.RegisteringFiles.Add(File);
            Out.StrippedLines.Add(File, MoveTemp(Stripped));

            for (int32 Index = 0; Index < RegisterLines.Num(); ++Index)
            {
                FVerbBlock Block;
                Block.Method    = RegisterMethods[Index];
                Block.File      = File;
                Block.FileLeaf  = FPaths::GetCleanFilename(File);
                Block.FirstLine = RegisterLines[Index];
                Block.LastLine  = (Index + 1 < RegisterLines.Num())
                    ? RegisterLines[Index + 1] - 1
                    : Lines.Num();
                Out.Blocks.Add(MoveTemp(Block));
            }
        }

        Out.bUsable = Out.Blocks.Num() > 0;
        return Out;
    }

    struct FTrackedHelper
    {
        const TCHAR* Symbol;
        const TCHAR* CallPattern;        // matched against one comment-stripped source line
        const TCHAR* DefinitionPattern;  // must match somewhere under Handlers/Render
    };

    struct FHelperCallSurvey
    {
        TSet<FString>   Members;          // verbs whose registration block calls the helper
        int32           SiteCount = 0;    // call sites found in registering files
        TArray<FString> Unattributed;     // "file:line" of sites outside every block
    };

    // Which verbs call this helper. `Unattributed` is the precondition that keeps the block model
    // honest: if a call site moves into a file-scope helper shared by two verbs, this walk cannot
    // say which verb owns it and must say so, rather than quietly dropping the site and shrinking
    // the group it was meant to police.
    inline FHelperCallSurvey SurveyHelper(const FScannedSource& Source, const FTrackedHelper& Helper)
    {
        FHelperCallSurvey Survey;
        const FRegexPattern Pattern(FString(Helper.CallPattern));

        for (const FString& File : Source.RegisteringFiles)
        {
            const TArray<FString>* Lines = Source.StrippedLines.Find(File);
            if (Lines == nullptr)
            {
                continue;
            }
            for (int32 Index = 0; Index < Lines->Num(); ++Index)
            {
                const FString& Line = (*Lines)[Index];
                if (Line.IsEmpty())
                {
                    continue;
                }
                FRegexMatcher Matcher(Pattern, Line);
                if (!Matcher.FindNext())
                {
                    continue;
                }
                ++Survey.SiteCount;

                const int32 LineNumber = Index + 1;
                const FVerbBlock* Owner = nullptr;
                for (const FVerbBlock& Block : Source.Blocks)
                {
                    if (Block.File == File && LineNumber >= Block.FirstLine && LineNumber <= Block.LastLine)
                    {
                        Owner = &Block;
                        break;
                    }
                }
                if (Owner != nullptr)
                {
                    Survey.Members.Add(Owner->Method);
                }
                else
                {
                    Survey.Unattributed.Add(FString::Printf(TEXT("%s:%d"),
                        *FPaths::GetCleanFilename(File), LineNumber));
                }
            }
        }
        return Survey;
    }

    inline bool HelperDefinitionExists(const FTrackedHelper& Helper)
    {
        const FString Dir = ResolveRenderHandlerDir();
        if (Dir.IsEmpty())
        {
            return false;
        }
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.h"), true, false, false);
        IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.cpp"), true, false, false);

        const FRegexPattern Pattern(FString(Helper.DefinitionPattern));
        for (const FString& File : Files)
        {
            FString Contents;
            if (!FFileHelper::LoadFileToString(Contents, *File))
            {
                continue;
            }
            FRegexMatcher Matcher(Pattern, Contents);
            if (Matcher.FindNext())
            {
                return true;
            }
        }
        return false;
    }

    // ============================================================================
    // The model: helper groups, slots and the exception table
    // ============================================================================

    struct FHelperGroup
    {
        const TCHAR*           Id;
        TArray<FTrackedHelper> Helpers;
    };

    // A group's membership is the UNION over its helpers, because one wire field can be read by
    // more than one shared entry point: `exposure` is parsed by ParseExposurePin directly on the
    // camera and animation verbs, and by ParseViewportCaptureRequest on the three still verbs that
    // go through the shared capture parser. Same vocabulary, same struct field, one group.
    inline TArray<FHelperGroup> MakeHelperGroups()
    {
        TArray<FHelperGroup> Groups;

        {
            FHelperGroup& Group = Groups.AddDefaulted_GetRef();
            Group.Id = TEXT("fit");
            Group.Helpers.Add({ TEXT("ComputeFitDistance"),
                TEXT("ComputeFitDistance\\s*\\("),
                TEXT("inline\\s+float\\s+ComputeFitDistance\\s*\\(") });
            // ResolveCameraDistance is the precedence wrapper R5 extracted (override, then the
            // subject's own radius, then the bounds fit). The two camera verbs reach
            // ComputeFitDistance only THROUGH it, so a group tracking the leaf alone would have
            // silently dropped both of them - and they are the pair P1 was about.
            Group.Helpers.Add({ TEXT("ResolveCameraDistance"),
                TEXT("ResolveCameraDistance\\s*\\("),
                TEXT("inline\\s+float\\s+ResolveCameraDistance\\s*\\(") });
        }
        {
            FHelperGroup& Group = Groups.AddDefaulted_GetRef();
            Group.Id = TEXT("ortho");
            Group.Helpers.Add({ TEXT("ComputeOrthoWorldWidth"),
                TEXT("ComputeOrthoWorldWidth\\s*\\("),
                TEXT("inline\\s+float\\s+ComputeOrthoWorldWidth\\s*\\(") });
        }
        {
            FHelperGroup& Group = Groups.AddDefaulted_GetRef();
            Group.Id = TEXT("exposurePin");
            Group.Helpers.Add({ TEXT("ParseExposurePin"),
                TEXT("ParseExposurePin\\s*\\("),
                TEXT("bool\\s+ParseExposurePin\\s*\\(") });
            Group.Helpers.Add({ TEXT("ParseViewportCaptureRequest"),
                TEXT("ParseViewportCaptureRequest\\s*\\("),
                TEXT("bool\\s+ParseViewportCaptureRequest\\s*\\(") });
            Group.Helpers.Add({ TEXT("ParseOffscreenCaptureRequest"),
                TEXT("ParseOffscreenCaptureRequest\\s*\\("),
                TEXT("bool\\s+ParseOffscreenCaptureRequest\\s*\\(") });
            Group.Helpers.Add({ TEXT("PinWrightOpenLevelCapture::Handle entry"),
                TEXT("return\\s+PinWrightOpenLevelCapture::Handle\\s*\\("),
                TEXT("bool\\s+PinWrightOpenLevelCapture::Handle\\s*\\(") });
        }
        {
            FHelperGroup& Group = Groups.AddDefaulted_GetRef();
            Group.Id = TEXT("viewModePin");
            Group.Helpers.Add({ TEXT("ParseViewModePin"),
                TEXT("ParseViewModePin\\s*\\("),
                TEXT("bool\\s+ParseViewModePin\\s*\\(") });
            Group.Helpers.Add({ TEXT("ParseViewportCaptureRequest"),
                TEXT("ParseViewportCaptureRequest\\s*\\("),
                TEXT("bool\\s+ParseViewportCaptureRequest\\s*\\(") });
            Group.Helpers.Add({ TEXT("ParseOffscreenCaptureRequest"),
                TEXT("ParseOffscreenCaptureRequest\\s*\\("),
                TEXT("bool\\s+ParseOffscreenCaptureRequest\\s*\\(") });
            Group.Helpers.Add({ TEXT("PinWrightOpenLevelCapture::Handle entry"),
                TEXT("return\\s+PinWrightOpenLevelCapture::Handle\\s*\\("),
                TEXT("bool\\s+PinWrightOpenLevelCapture::Handle\\s*\\(") });
        }
        {
            FHelperGroup& Group = Groups.AddDefaulted_GetRef();
            Group.Id = TEXT("previewSceneRig");
            Group.Helpers.Add({ TEXT("ParsePreviewSceneRigPin"),
                TEXT("ParsePreviewSceneRigPin\\s*\\("),
                TEXT("bool\\s+ParsePreviewSceneRigPin\\s*\\(") });
            Group.Helpers.Add({ TEXT("ParseViewportCaptureRequest"),
                TEXT("ParseViewportCaptureRequest\\s*\\("),
                TEXT("bool\\s+ParseViewportCaptureRequest\\s*\\(") });
            Group.Helpers.Add({ TEXT("ParseOffscreenCaptureRequest"),
                TEXT("ParseOffscreenCaptureRequest\\s*\\("),
                TEXT("bool\\s+ParseOffscreenCaptureRequest\\s*\\(") });
            // The registered verb delegates through a file-scope wrapper, so track its entry call
            // explicitly instead of silently losing render.capture_open_level from this group.
            Group.Helpers.Add({ TEXT("PinWrightOpenLevelCapture::Handle entry"),
                TEXT("return\\s+PinWrightOpenLevelCapture::Handle\\s*\\("),
                TEXT("bool\\s+PinWrightOpenLevelCapture::Handle\\s*\\(") });
        }
        {
            FHelperGroup& Group = Groups.AddDefaulted_GetRef();
            Group.Id = TEXT("shotDistribution");
            Group.Helpers.Add({ TEXT("MakeRingDistribution"),
                TEXT("MakeRingDistribution\\s*\\("),
                TEXT("inline\\s+TArray<FPlannedShot>\\s+MakeRingDistribution\\s*\\(") });
            Group.Helpers.Add({ TEXT("MakeSphereDistribution"),
                TEXT("MakeSphereDistribution\\s*\\("),
                TEXT("inline\\s+TArray<FPlannedShot>\\s+MakeSphereDistribution\\s*\\(") });
        }
        return Groups;
    }

    struct FParitySlot
    {
        const TCHAR* Id;
        const TCHAR* GroupId;
        const TCHAR* Spellings;                 // comma-separated; any one satisfies the slot
        const TCHAR* SharedDescriptionMacro;    // nullptr: this slot has no shared description
        bool         bWantsSharedMacro;         // true + nullptr macro == a NoSharedMacro gap
        bool         bCheckRequiredness;
        const float* ExpectedNumericDefault;    // nullptr: do not check the published default
    };

    // The published default for the bounds-fit margin, taken from the code's own constant rather
    // than typed in here. A literal 1.15 in this file would pass even if the verbs AND the constant
    // moved to 1.3 together, which is exactly the state this check exists to notice.
    static constexpr float GExpectedFitPaddingDefault = PinWrightCameraFrame::GDefaultFitPadding;

    static const FParitySlot GParitySlots[] =
    {
        // ---- ComputeFitDistance(Radius, Fov, Padding): its three inputs, and who can reach them
        { TEXT("fit.padding"),  TEXT("fit"),   TEXT("padding"),         TEXT(PINWRIGHT_FIT_PADDING_PARAM_DESC), true,  true, &GExpectedFitPaddingDefault },
        { TEXT("fit.distance"), TEXT("fit"),   TEXT("distance,radius"), nullptr,                                false, true, nullptr },
        { TEXT("fit.fov"),      TEXT("fit"),   TEXT("fov"),             nullptr,                                false, true, nullptr },

        // ---- ComputeOrthoWorldWidth(Radius, Padding, Width, Height): the caller-facing override
        { TEXT("ortho.width"),  TEXT("ortho"), TEXT("orthoWidth"),      nullptr,                                false, true, nullptr },

        // ---- the two shared pins
        { TEXT("exposure"),     TEXT("exposurePin"), TEXT("exposure"),  TEXT(PINWRIGHT_EXPOSURE_PARAM_DESC),    true,  true, nullptr },
        { TEXT("viewMode"),     TEXT("viewModePin"), TEXT("viewMode"),  TEXT(PINWRIGHT_VIEW_MODE_PARAM_DESC),   true,  true, nullptr },

        // ---- the preview-scene rig. `render.capture_open_level` is a member because it runs the
        //      shared capture parser, which fills the pin whether or not the verb declares it;
        //      decision 9 of the plan is that the pin must be actively CLEARED there rather than
        //      silently accepted, and the exception row below is where that refusal is recorded.
        { TEXT("previewScene"), TEXT("previewSceneRig"), TEXT("previewScene"), TEXT(PINWRIGHT_PREVIEW_SCENE_PARAM_DESC), true, true, nullptr },

        // ---- the shot planner. `distribution` is declared four times with divergent prose and no
        //      shared macro at all, which is why bWantsSharedMacro is true against a null macro.
        { TEXT("distribution"), TEXT("shotDistribution"), TEXT("distribution"), nullptr,                        true,  true, nullptr },
        { TEXT("seed"),         TEXT("shotDistribution"), TEXT("seed"),         nullptr,                        false, true, nullptr },
        { TEXT("count"),        TEXT("shotDistribution"), TEXT("count"),        nullptr,                        false, true, nullptr },
        { TEXT("elevation"),    TEXT("shotDistribution"), TEXT("elevation"),    nullptr,                        false, true, nullptr },
    };

    // Gap kinds as strings rather than an enum, so the key joining a gap to its exception row is
    // readable in a failure message without a decoder.
    namespace GapKind
    {
        static constexpr const TCHAR* Declaration  = TEXT("declaration");
        static constexpr const TCHAR* Description  = TEXT("description");
        static constexpr const TCHAR* Requiredness = TEXT("requiredness");
        static constexpr const TCHAR* Default      = TEXT("default");
        static constexpr const TCHAR* SharedMacro  = TEXT("sharedMacro");
    }

    struct FParityException
    {
        const TCHAR* Verb;       // method name, or "*" for a gap belonging to the slot itself
        const TCHAR* SlotId;
        const TCHAR* Kind;
        const TCHAR* File;       // plugin-relative
        int32        Line;
        const TCHAR* Anchor;     // distinctive text expected within GCitationSlackLines of Line
        const TCHAR* Note;
    };

    inline FString GapKey(const FString& Verb, const FString& SlotId, const FString& Kind)
    {
        return FString::Printf(TEXT("%s|%s|%s"), *Verb, *SlotId, *Kind);
    }

    static constexpr const TCHAR* GPlan = TEXT("docs/preview-scene-rig.md");
    static constexpr const TCHAR* GShotPlanUtils =
        TEXT("Source/PinWright/Private/Handlers/Render/CameraShotPlanUtils.h");
    static constexpr const TCHAR* GCaptureUtils =
        TEXT("Source/PinWright/Private/Handlers/Render/PreviewViewportCaptureUtils.h");
    static constexpr const TCHAR* GRenderHandler =
        TEXT("Source/PinWright/Private/Handlers/Render/RenderHandler.cpp");
    static constexpr const TCHAR* GCaptureSubjectsDoc =
        TEXT("docs/wiki-src/render.capture-subjects.md");

    // ---- the exception table for the helper-input slots --------------------------------------
    //
    // Fifteen rows, every one an open defect the plan deliberately did not fix (5.3). P1 is NOT
    // here: R5 fixed it. If R5's fix is reverted the two rows it would need are absent and this
    // test goes red naming them, which is the point.
    static const FParityException GSlotExceptions[] =
    {
        // The level viewport has no preview scene, so the shared parser's pin is deliberately
        // cleared and remains absent from this verb's public declaration.
        { TEXT("render.capture_open_level"), TEXT("previewScene"), GapKind::Declaration,
          GRenderHandler, 1939,
          TEXT("`render.capture_open_level` parses then clears `previewScene`"),
          TEXT("The registration delegates through PinWrightOpenLevelCapture::Handle, which parses the shared pin but clears it because a live level viewport has no FPreviewScene rig to apply.") },

        // ---- P2: render.capture_asset_preview welds BOTH inputs of ComputeFitDistance
        { TEXT("render.capture_asset_preview"), TEXT("fit.padding"), GapKind::Declaration, GPlan, 659,
          TEXT("no `radius`, padding hardcoded 1.25"),
          TEXT("P2. The 1.25 is welded in RenderHandler.cpp with a comment recording that the divergence was noticed and accepted per-verb.") },
        { TEXT("render.capture_asset_preview"), TEXT("fit.distance"), GapKind::Declaration, GPlan, 659,
          TEXT("no `radius`, padding hardcoded 1.25"),
          TEXT("P2, the other half: no distance override either, so neither input of the helper is reachable from the wire.") },

        // ---- P3, and the fork R5 created: two verbs neither share the padding vocabulary nor
        //      publish the padding default the other two publish.
        { TEXT("camera.animation_shots"), TEXT("fit.padding"), GapKind::Description, GPlan, 660,
          TEXT("Three padding defaults (1.15 / 1.25 / 1.4), two unreachable"),
          TEXT("BEYOND THE PLAN'S NINE: R5 introduced PINWRIGHT_FIT_PADDING_PARAM_DESC and applied it to the two camera verbs only, so this verb's padding prose is now a fork rather than a copy.") },
        { TEXT("render.capture_animation_preview"), TEXT("fit.padding"), GapKind::Description, GPlan, 660,
          TEXT("Three padding defaults (1.15 / 1.25 / 1.4), two unreachable"),
          TEXT("BEYOND THE PLAN'S NINE: same fork; this verb's 1.4 default is stated in its own prose instead of appended to the shared sentence.") },
        { TEXT("camera.animation_shots"), TEXT("fit.padding"), GapKind::Default, GPlan, 660,
          TEXT("Three padding defaults (1.15 / 1.25 / 1.4), two unreachable"),
          TEXT("P3. Defaults to GDefaultFitPadding in code and publishes no default in its spec, so the number is not discoverable from the registry.") },
        { TEXT("render.capture_animation_preview"), TEXT("fit.padding"), GapKind::Default, GPlan, 660,
          TEXT("Three padding defaults (1.15 / 1.25 / 1.4), two unreachable"),
          TEXT("P3. Defaults to 1.4 in code and publishes no default in its spec.") },

        // ---- the missing distance override on the animation-preview verb
        { TEXT("render.capture_animation_preview"), TEXT("fit.distance"), GapKind::Declaration, GPlan, 633,
          TEXT("`render.capture_animation_preview` | **none**"),
          TEXT("5.1's table row: distance override 'none'. camera.animation_shots exposes both inputs of the same helper, which is the proof this is oversight rather than design.") },

        // ---- P4: orthoWidth splits along the namespace line
        { TEXT("camera.frame_actor"), TEXT("ortho.width"), GapKind::Declaration, GPlan, 661,
          TEXT("`orthoWidth` and free-camera `location`/`rotation` split along the namespace line"),
          TEXT("P4. The three render.* still verbs expose orthoWidth; all four bounds-fitting verbs expose none.") },
        { TEXT("camera.orbit_shots"), TEXT("ortho.width"), GapKind::Declaration, GPlan, 661,
          TEXT("`orthoWidth` and free-camera `location`/`rotation` split along the namespace line"),
          TEXT("P4.") },
        { TEXT("camera.animation_shots"), TEXT("ortho.width"), GapKind::Declaration, GPlan, 661,
          TEXT("`orthoWidth` and free-camera `location`/`rotation` split along the namespace line"),
          TEXT("P4.") },
        { TEXT("render.capture_animation_preview"), TEXT("ortho.width"), GapKind::Declaration, GPlan, 661,
          TEXT("`orthoWidth` and free-camera `location`/`rotation` split along the namespace line"),
          TEXT("P4.") },

        // ---- P9: capture_ortho_tiles forks the exposure vocabulary in two directions
        { TEXT("render.capture_ortho_tiles"), TEXT("exposure"), GapKind::Description, GPlan, 666,
          TEXT("`capture_ortho_tiles`'s `exposure` forks the shared vocabulary"),
          TEXT("P9. Its own description text instead of PINWRIGHT_EXPOSURE_PARAM_DESC.") },
        { TEXT("render.capture_ortho_tiles"), TEXT("exposure"), GapKind::Requiredness, GPlan, 666,
          TEXT("`capture_ortho_tiles`'s `exposure` forks the shared vocabulary"),
          TEXT("P9. Required rather than optional. The requirement itself is correct and documented; the fork is that nothing else on this helper is required.") },

        // ---- P8: distribution has no shared macro at all
        { TEXT("*"), TEXT("distribution"), GapKind::SharedMacro, GPlan, 665,
          TEXT("`distribution`'s description is hand-copied four times"),
          TEXT("P8. Four verbs, four hand-written descriptions, divergent prose ('shots' vs 'poses'), no macro to share.") },
    };

    // ============================================================================
    // The set-level primitive: FPoseListCaptureRequest
    // ============================================================================

    struct FPoseListSetField
    {
        const TCHAR* Field;
        const TCHAR* WireSpelling;              // empty: the verb computes this, the caller never names it
        bool         bNoVerbDeclaresItAnywhere; // the P5/P6 shape: carried, forwarded, unreachable
        const TCHAR* Note;
    };

    // Every field of FPoseListCaptureRequest, classified. The struct is PARSED from its header and
    // the two sets are compared in both directions, so a field added without a row here fails and a
    // row here for a removed field fails. That symmetry is what makes this catch the NEXT
    // viewDistanceScale rather than only recording this one.
    static const FPoseListSetField GPoseListFields[] =
    {
        { TEXT("Poses"),                      TEXT(""),                  false, TEXT("The set itself; each verb generates it from its own shot plan.") },
        { TEXT("Width"),                      TEXT("width"),             false, TEXT("") },
        { TEXT("Height"),                     TEXT("height"),            false, TEXT("") },
        { TEXT("FilenamePrefix"),             TEXT(""),                  false, TEXT("Auto-name stem chosen per verb; the caller-facing spelling is `filename` on the still verbs, which do not use this primitive.") },
        { TEXT("Subdirectory"),               TEXT(""),                  false, TEXT("Output subdirectory chosen per verb.") },
        { TEXT("Exposure"),                   TEXT("exposure"),          false, TEXT("") },
        { TEXT("bHideEditorSprites"),         TEXT("hideEditorSprites"), false, TEXT("") },
        { TEXT("ViewMode"),                   TEXT("viewMode"),          false, TEXT("") },
        { TEXT("ViewDistanceScale"),          TEXT("viewDistanceScale"), false, TEXT("P5. Declared on two verbs that never build this struct, and carried plus forwarded by the struct no verb declaring it uses.") },
        { TEXT("bViewDistanceScaleProvided"), TEXT(""),                  false, TEXT("Presence companion of ViewDistanceScale - set from HasField, never from a value.") },
        { TEXT("bAutoViewDistanceScale"),     TEXT(""),                  false, TEXT("Handler policy, not caller input: the verb decides whether an omitted scale is derived from the scene.") },
        { TEXT("bRejectBlankCapture"),        TEXT("rejectBlank"),       false, TEXT("") },
        { TEXT("bAllowBlank"),                TEXT("allowBlank"),        false, TEXT("") },
        { TEXT("bRetainPixels"),              TEXT(""),                  false, TEXT("Handler policy, not caller input, and the same shape as bAutoViewDistanceScale: the primitive retains the decoded pixels of the previous frame so that a pose the evaluator says CHANGED, whose same-camera readback did not, can be failed closed instead of written out as a stale image. The caller asks for frames, not for the staleness check that makes those frames trustworthy.") },
        { TEXT("MaxPoses"),                   TEXT(""),                  false, TEXT("Per-call ceiling; published in the response as maxPosesPerCall rather than accepted from the wire.") },
        { TEXT("bWarmupShot"),                TEXT("warmupShot"),        true,  TEXT("P6. The header documents a caller affordance ('a caller that knows the viewport is warm can turn it off') that has no wire spelling on any verb.") },
        { TEXT("SubjectTimeSetter"),          TEXT(""),                  false, TEXT("A delegate; the caller names a time axis through frames/time, never through the setter.") },
        // ---- the subject-coverage seam, classified field by field ----
        { TEXT("SubjectVisibilitySetter"),    TEXT(""),                  false, TEXT("A delegate, and the same shape as SubjectTimeSetter: a TFunction has no wire spelling to give. The caller asks for the measurement through `measureCoverage`; which mechanism can hide THIS subject is the resolved subject's answer, not the caller's.") },
        { TEXT("bMeasureSubjectCoverage"),    TEXT("measureCoverage"),   false, TEXT("Caller input, not handler policy: the differential costs an extra draw + readback per shot and two full-size buffers, which is a cost the WIRE caller pays. Defaults true on the one verb that publishes a coverage number, and honours an explicit false.") },
        { TEXT("CoverageWarnFraction"),       TEXT(""),                  false, TEXT("Deliberately fixed, the same shape as MaxPoses: the datum is published, the constant is not accepted. `subjectCoverage` is emitted on every measured shot and the warning text states the floor it fell under, so a caller with a different floor applies it to a number they already hold - and a settable floor's only use would be to silence the warning that says the frame is empty.") },
        { TEXT("CoverageChannelThreshold"),   TEXT(""),                  false, TEXT("Deliberately fixed: it defines what a CHANGED pixel is, so it defines what the published coverage number means. Two calls made under different thresholds report figures that cannot be compared, and comparison is the number's only use. 8/255 is calibrated against measured inter-draw noise, well under the 100-plus a faint additive particle moves a pixel by.") },
        { TEXT("BoundsOrigin"),               TEXT(""),                  false, TEXT("Measured from the subject, never accepted from the caller - see the header's FROM A STATIC SOURCE note.") },
        { TEXT("BoundsRadius"),               TEXT(""),                  false, TEXT("Measured from the subject.") },
        // R1's addition, per plan 2.2's frozen contract. Pre-modelled deliberately: without a row
        // here the field lands during this same wave and fails as 'unmodelled', and with the row a
        // different spelling fails while naming the field that was expected.
        { TEXT("PreviewSceneRig"),            TEXT("previewScene"),      false, TEXT("Declaration parity here; its shared-macro parity is covered by the previewSceneRig slot, whose group also reaches the three single-shot verbs that never build this struct.") },
        { TEXT("bPreviewSceneRigAlreadyScoped"), TEXT(""),               false, TEXT("Internal set-wrapper handoff: CaptureCameraPoses sets it only after constructing the one rig guard for the set; callers select previewScene, not guard ownership.") },
        { TEXT("PreviewSceneRigAtSetEntry"),  TEXT(""),                  false, TEXT("Internal snapshot captured by the set wrapper and used to verify restoration after the guard exits; it is measured state, not caller input.") },
        { TEXT("bPreviewSceneCaptureUpdatedAtSetEntry"), TEXT(""),       false, TEXT("Internal measurement forwarded from the same set-level guard: whether the sky/reflection capture drain ran for the set. Published per shot as previewScene.captureUpdated, never accepted from the wire - a caller cannot ask for a stale sky capture, and the drain is unconditional.") },
        { TEXT("bPreviewSceneCaptureIncompleteAtSetEntry"), TEXT(""),    false, TEXT("The second half of the same measurement: a sky capture still queued when that drain returned, which the engine does while assets compile and will not retry for 5 s. Published per shot as previewScene.captureIncomplete. Not caller input for the same reason - nothing a caller can pass shortens that wait.") },
        { TEXT("ViewportCaptureSetContext"),  TEXT(""),                  false, TEXT("Internal synchronous-lifetime handoff: CaptureCameraPoses owns the render pins and viewport sizing for the set; callers select the resolution and exposure, not guard ownership.") },
        { TEXT("bMeasurePoseRepeatability"),  TEXT(""),                  false, TEXT("Internal integrity policy: CaptureCameraPoses enables the control for every real viewport set; only the injected primitive leaves it off to keep the unit-test seam minimal.") },
    };

    // Twenty-five rows. Every one is a set-level field the shared primitive applies to every frame
    // of a set, that some verb built on that primitive cannot reach from the wire.
    static const FParityException GPoseListExceptions[] =
    {
        // ---- hideEditorSprites is refused on the two preview verbs, and the refusal is reasoned
        { TEXT("render.capture_animation_preview"), TEXT("bHideEditorSprites"), GapKind::Declaration, GCaptureUtils, 44,
          TEXT("The `hideEditorSprites` parameter's wire description"),
          TEXT("Reasoned refusal: an FAdvancedPreviewScene holds nothing the BillboardSprites flag can act on, so declaring it would publish a knob whose stated purpose is unreachable.") },
        { TEXT("render.capture_asset_preview"), TEXT("bHideEditorSprites"), GapKind::Declaration, GCaptureUtils, 44,
          TEXT("The `hideEditorSprites` parameter's wire description"),
          TEXT("Same reasoned refusal.") },

        // ---- P5: viewDistanceScale, on all five verbs that carry the struct
        { TEXT("camera.frame_actor"), TEXT("ViewDistanceScale"), GapKind::Declaration, GPlan, 662,
          TEXT("`viewDistanceScale` is plumbed through `FPoseListCaptureRequest`"),
          TEXT("P5, the widest gap in the sweep.") },
        { TEXT("camera.orbit_shots"), TEXT("ViewDistanceScale"), GapKind::Declaration, GPlan, 662,
          TEXT("`viewDistanceScale` is plumbed through `FPoseListCaptureRequest`"), TEXT("P5.") },
        { TEXT("camera.animation_shots"), TEXT("ViewDistanceScale"), GapKind::Declaration, GPlan, 662,
          TEXT("`viewDistanceScale` is plumbed through `FPoseListCaptureRequest`"), TEXT("P5.") },
        { TEXT("render.capture_animation_preview"), TEXT("ViewDistanceScale"), GapKind::Declaration, GPlan, 662,
          TEXT("`viewDistanceScale` is plumbed through `FPoseListCaptureRequest`"), TEXT("P5.") },
        { TEXT("render.capture_asset_preview"), TEXT("ViewDistanceScale"), GapKind::Declaration, GPlan, 662,
          TEXT("`viewDistanceScale` is plumbed through `FPoseListCaptureRequest`"), TEXT("P5.") },

        // ---- P6: warmupShot has no wire spelling on any verb in the tree
        { TEXT("camera.frame_actor"), TEXT("bWarmupShot"), GapKind::Declaration, GPlan, 663,
          TEXT("`bWarmupShot` documents a caller affordance"), TEXT("P6.") },
        { TEXT("camera.orbit_shots"), TEXT("bWarmupShot"), GapKind::Declaration, GPlan, 663,
          TEXT("`bWarmupShot` documents a caller affordance"), TEXT("P6.") },
        { TEXT("camera.animation_shots"), TEXT("bWarmupShot"), GapKind::Declaration, GPlan, 663,
          TEXT("`bWarmupShot` documents a caller affordance"), TEXT("P6.") },
        { TEXT("render.capture_animation_preview"), TEXT("bWarmupShot"), GapKind::Declaration, GPlan, 663,
          TEXT("`bWarmupShot` documents a caller affordance"), TEXT("P6.") },
        { TEXT("render.capture_asset_preview"), TEXT("bWarmupShot"), GapKind::Declaration, GPlan, 663,
          TEXT("`bWarmupShot` documents a caller affordance"), TEXT("P6.") },

        // ---- rejectBlank: reasoned on the three camera verbs, unreasoned on the fourth
        { TEXT("camera.frame_actor"), TEXT("bRejectBlankCapture"), GapKind::Declaration, GShotPlanUtils, 366,
          TEXT("These verbs deliberately do NOT set bRejectBlankCapture"),
          TEXT("Reasoned: an actor legitimately reviewed against a dark backdrop must not hard-fail, so the caller gets the numbers and decides.") },
        { TEXT("camera.orbit_shots"), TEXT("bRejectBlankCapture"), GapKind::Declaration, GShotPlanUtils, 366,
          TEXT("These verbs deliberately do NOT set bRejectBlankCapture"), TEXT("Same reasoned refusal.") },
        { TEXT("camera.animation_shots"), TEXT("bRejectBlankCapture"), GapKind::Declaration, GShotPlanUtils, 366,
          TEXT("These verbs deliberately do NOT set bRejectBlankCapture"), TEXT("Same reasoned refusal.") },
        { TEXT("render.capture_animation_preview"), TEXT("bRejectBlankCapture"), GapKind::Declaration, GPlan, 664,
          TEXT("`rejectBlank`'s absence is reasoned and documented"),
          TEXT("BEYOND THE PLAN'S NINE: P7 records that the CAMERA verbs' refusal is reasoned. This verb is not a camera verb and is not covered by that rationale.") },

        // ---- P7: allowBlank reaches one of five
        { TEXT("camera.frame_actor"), TEXT("bAllowBlank"), GapKind::Declaration, GPlan, 664,
          TEXT("`allowBlank` reaches one of five pose-capture verbs"), TEXT("P7.") },
        { TEXT("camera.orbit_shots"), TEXT("bAllowBlank"), GapKind::Declaration, GPlan, 664,
          TEXT("`allowBlank` reaches one of five pose-capture verbs"), TEXT("P7.") },
        { TEXT("camera.animation_shots"), TEXT("bAllowBlank"), GapKind::Declaration, GPlan, 664,
          TEXT("`allowBlank` reaches one of five pose-capture verbs"), TEXT("P7.") },
        { TEXT("render.capture_animation_preview"), TEXT("bAllowBlank"), GapKind::Declaration, GPlan, 664,
          TEXT("`allowBlank` reaches one of five pose-capture verbs"), TEXT("P7.") },

        // ---- previewScene is level/actor-only on camera.animation_shots today
        { TEXT("camera.animation_shots"), TEXT("PreviewSceneRig"), GapKind::Declaration, GPlan, 1100,
          TEXT("Should `previewScene` also be accepted on `camera.animation_shots`?"),
          TEXT("Open question 2 of the plan: the verb is level/actor-only, so 1a marks the whole rig row unreachable there.") },

        // ---- measureCoverage: the coverage seam reaches one verb of five, and says so in writing
        //
        // NOT a reasoned refusal like hideEditorSprites above - these four CAN be measured, the
        // seam just does not reach them. Each row is written to die: the day a verb binds a
        // visibility setter and publishes a coverage number, its row stops describing a gap and
        // the obsolete-row check names it.
        { TEXT("camera.frame_actor"), TEXT("bMeasureSubjectCoverage"), GapKind::Declaration,
          GCaptureSubjectsDoc, 81,
          TEXT("No other pose-set verb declares `measureCoverage`"),
          TEXT("The verb binds no subject-visibility setter and emits no coverage field at all, so the flag could not change one byte of its response - declaring it would publish a knob that does nothing. A placed actor CAN be hidden and redrawn, so this is an open gap, not a decision.") },
        { TEXT("camera.orbit_shots"), TEXT("bMeasureSubjectCoverage"), GapKind::Declaration,
          GCaptureSubjectsDoc, 81,
          TEXT("No other pose-set verb declares `measureCoverage`"),
          TEXT("Same open gap: no visibility setter is bound and no coverage number is published on this verb.") },
        { TEXT("camera.animation_shots"), TEXT("bMeasureSubjectCoverage"), GapKind::Declaration,
          GCaptureSubjectsDoc, 81,
          TEXT("No other pose-set verb declares `measureCoverage`"),
          TEXT("Same open gap.") },
        { TEXT("render.capture_animation_preview"), TEXT("bMeasureSubjectCoverage"), GapKind::Declaration,
          GCaptureSubjectsDoc, 81,
          TEXT("No other pose-set verb declares `measureCoverage`"),
          TEXT("Same open gap. A skinned subject is the preview scene's only content, so measuring it needs a different reference than hiding it - which is a design question this row keeps open rather than answering by omission.") },
    };

    // Parse a struct's field names out of a header. Line-based rather than one whole-file regex so
    // a field's own line is available for a failure message.
    inline bool ParseStructFields(const FString& HeaderPath, const FString& StructName,
        TArray<FString>& OutFields)
    {
        TArray<FString> Lines;
        if (!FFileHelper::LoadFileToStringArray(Lines, *HeaderPath))
        {
            return false;
        }
        const FString Opener = FString::Printf(TEXT("struct %s"), *StructName);
        int32 Start = INDEX_NONE;
        for (int32 Index = 0; Index < Lines.Num(); ++Index)
        {
            if (Lines[Index].TrimStartAndEnd().StartsWith(Opener))
            {
                Start = Index;
                break;
            }
        }
        if (Start == INDEX_NONE)
        {
            return false;
        }

        // `Type Name;` or `Type Name = init;`, where Type may be qualified and templated. A member
        // function does not match: after the name comes `(`, not `;` or `=`.
        const FRegexPattern FieldPattern(
            TEXT("^\\s+[A-Za-z_][A-Za-z0-9_:]*(?:<[^;]*>)?\\s*[\\*&]?\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*(?:=[^;]*)?;\\s*$"));

        for (int32 Index = Start + 1; Index < Lines.Num(); ++Index)
        {
            const FString Raw = Lines[Index];
            const FString Trimmed = Raw.TrimStartAndEnd();
            if (Trimmed.Equals(TEXT("};")))
            {
                return true;    // end of the struct
            }
            if (Trimmed.IsEmpty() || Trimmed.StartsWith(TEXT("//")) || Trimmed.StartsWith(TEXT("*")))
            {
                continue;
            }
            FRegexMatcher Matcher(FieldPattern, Raw);
            if (Matcher.FindNext())
            {
                OutFields.Add(Matcher.GetCaptureGroup(1));
            }
        }
        return false;   // never found the closing brace: treat as a parse failure, not as an empty struct
    }

    // Which fields of the pose-list request each verb assigns, keyed by method name. Derived from
    // the local variable the verb declares, so a verb renaming its local still reports correctly.
    inline TMap<FString, TSet<FString>> SurveyPoseRequestAssignments(
        const FScannedSource& Source, TSet<FString>& OutMemberVerbs)
    {
        TMap<FString, TSet<FString>> ByMethod;
        const FRegexPattern DeclPattern(TEXT("FPoseListCaptureRequest\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*;"));

        for (const FVerbBlock& Block : Source.Blocks)
        {
            const TArray<FString>* Lines = Source.StrippedLines.Find(Block.File);
            if (Lines == nullptr)
            {
                continue;
            }
            FString VarName;
            for (int32 Index = Block.FirstLine - 1; Index < Block.LastLine && Index < Lines->Num(); ++Index)
            {
                FRegexMatcher Matcher(DeclPattern, (*Lines)[Index]);
                if (Matcher.FindNext())
                {
                    VarName = Matcher.GetCaptureGroup(1);
                    break;
                }
            }
            if (VarName.IsEmpty())
            {
                continue;
            }

            OutMemberVerbs.Add(Block.Method);
            TSet<FString>& Assigned = ByMethod.FindOrAdd(Block.Method);
            const FRegexPattern AssignPattern(
                FString::Printf(TEXT("\\b%s\\.([A-Za-z_][A-Za-z0-9_]*)\\s*="), *VarName));
            for (int32 Index = Block.FirstLine - 1; Index < Block.LastLine && Index < Lines->Num(); ++Index)
            {
                FRegexMatcher Matcher(AssignPattern, (*Lines)[Index]);
                while (Matcher.FindNext())
                {
                    Assigned.Add(Matcher.GetCaptureGroup(1));
                }
            }
        }
        return ByMethod;
    }

    // ============================================================================
    // Citation resolution
    // ============================================================================

    struct FCitationVerdict
    {
        bool  bFileFound = false;
        bool  bAnchorInFile = false;
        bool  bResolved = false;
        int32 NearestLine = INDEX_NONE;
    };

    inline FCitationVerdict ResolveCitation(const FString& AbsolutePath, int32 Line, const FString& Anchor)
    {
        FCitationVerdict Verdict;
        TArray<FString> Lines;
        if (!FFileHelper::LoadFileToStringArray(Lines, *AbsolutePath))
        {
            return Verdict;
        }
        Verdict.bFileFound = true;

        int32 BestDistance = MAX_int32;
        for (int32 Index = 0; Index < Lines.Num(); ++Index)
        {
            if (!Lines[Index].Contains(Anchor, ESearchCase::CaseSensitive))
            {
                continue;
            }
            Verdict.bAnchorInFile = true;
            const int32 Distance = FMath::Abs((Index + 1) - Line);
            if (Distance < BestDistance)
            {
                BestDistance = Distance;
                Verdict.NearestLine = Index + 1;
            }
        }
        Verdict.bResolved = Verdict.bAnchorInFile && BestDistance <= GCitationSlackLines;
        return Verdict;
    }
}

// ================================================================================================
// Test 1 -- the preconditions.
//
// Everything below asserts properties OF a derived set, so an empty derived set would make all of
// them pass over nothing. This test is what makes that impossible.
//
// Unable to fail if: it merely checked that the scan returned without error. It asserts a floor on
// the registry, that each tracked helper's DEFINITION still exists (a rename would otherwise empty
// its group in silence), that every group resolved at least TWO members (one member is not a parity
// question), that every scanned block names a registered method, that every slot spelling is a live
// wire spelling somewhere in the registry, and that ZERO helper call sites fell outside a
// registration block.
// ================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureParityGroupsDiscoveredTest,
    "PinWright.render.parameter_parity.HelperGroupsAreDiscoveredNotListed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureParityGroupsDiscoveredTest::RunTest(const FString& Parameters)
{
    using namespace CaptureVerbParameterParityTest;

    const TMap<FString, const FHandlerRegistration*> Registry = LoadRegistry();
    // A floor, not an exact count, for the same reason the sibling contract test uses one: anything
    // that guts auto-registration must fail HERE rather than making every walk below vacuous.
    if (!TestTrue(FString::Printf(TEXT("registry carries a plausible number of verbs (saw %d)"), Registry.Num()),
            Registry.Num() > 200))
    {
        return false;
    }

    const FScannedSource Source = ScanRenderSources();
    if (!TestTrue(TEXT("Handlers/Render resolved and yielded registration blocks"), Source.bUsable))
    {
        AddError(TEXT("Could not scan Source/PinWright/Private/Handlers/Render through IPluginManager. "
                      "Every parity assertion in this file is derived from that scan, so this is a hard "
                      "stop rather than a pass over an empty set."));
        return false;
    }
    TestTrue(FString::Printf(TEXT("scanned several registering sources (saw %d)"), Source.RegisteringFiles.Num()),
        Source.RegisteringFiles.Num() >= 4);
    TestTrue(FString::Printf(TEXT("found several registration blocks (saw %d)"), Source.Blocks.Num()),
        Source.Blocks.Num() >= 8);

    // The scan and the registry must agree about what a verb is called. A block naming a method the
    // registry does not know means the two views have drifted and no group below is trustworthy.
    for (const FVerbBlock& Block : Source.Blocks)
    {
        TestTrue(FString::Printf(TEXT("%s (%s:%d) is a registered method"),
                *Block.Method, *Block.FileLeaf, Block.FirstLine),
            Registry.Contains(Block.Method));
    }

    const TArray<FHelperGroup> Groups = MakeHelperGroups();
    TestTrue(TEXT("helper groups are modelled"), Groups.Num() >= 5);

    for (const FHelperGroup& Group : Groups)
    {
        TSet<FString> Members;
        int32 TotalSites = 0;
        for (const FTrackedHelper& Helper : Group.Helpers)
        {
            TestTrue(FString::Printf(TEXT("helper '%s' still has a definition under Handlers/Render"), Helper.Symbol),
                HelperDefinitionExists(Helper));

            const FHelperCallSurvey Survey = SurveyHelper(Source, Helper);
            TotalSites += Survey.SiteCount;
            Members.Append(Survey.Members);

            for (const FString& Site : Survey.Unattributed)
            {
                AddError(FString::Printf(
                    TEXT("Call site of '%s' at %s falls outside every REGISTER_RPC_HANDLER block. This "
                         "walk attributes a helper call to the verb whose block encloses it; a call in a "
                         "file-scope helper cannot be attributed and would silently shrink the '%s' group. "
                         "Move the call inside a verb block, or extend the attribution model."),
                    Helper.Symbol, *Site, Group.Id));
            }
            TestEqual(FString::Printf(TEXT("every '%s' call site is attributable to a verb"), Helper.Symbol),
                Survey.Unattributed.Num(), 0);
        }

        TestTrue(FString::Printf(TEXT("group '%s' found call sites (saw %d)"), Group.Id, TotalSites),
            TotalSites > 0);
        // Two is the floor for a PARITY question: a helper with one caller has nothing to be uniform
        // with, and a group collapsing to one member is how this check would quietly stop covering
        // the defect it was written for.
        TestTrue(FString::Printf(TEXT("group '%s' resolved at least two member verbs (saw %d)"),
                Group.Id, Members.Num()),
            Members.Num() >= 2);
    }

    // Every slot must belong to a modelled group, and every slot spelling must be a live wire
    // spelling somewhere in the registry - otherwise a typo in the table reads as a universal gap.
    for (const FParitySlot& Slot : GParitySlots)
    {
        bool bGroupExists = false;
        for (const FHelperGroup& Group : Groups)
        {
            bGroupExists = bGroupExists || FString(Group.Id).Equals(Slot.GroupId);
        }
        TestTrue(FString::Printf(TEXT("slot '%s' names a modelled group ('%s')"), Slot.Id, Slot.GroupId),
            bGroupExists);

        for (const FString& Spelling : SplitSpellings(Slot.Spellings))
        {
            bool bDeclaredSomewhere = false;
            for (const TPair<FString, const FHandlerRegistration*>& Entry : Registry)
            {
                if (Entry.Value != nullptr && FindParam(*Entry.Value, Spelling) != nullptr)
                {
                    bDeclaredSomewhere = true;
                    break;
                }
            }
            TestTrue(FString::Printf(
                    TEXT("slot '%s' spelling '%s' is declared by at least one registered verb "
                         "(otherwise it is a typo in the slot table, not a gap in every verb)"),
                    Slot.Id, *Spelling),
                bDeclaredSomewhere);
        }
    }

    return true;
}

// ================================================================================================
// Test 2 -- the parity walk itself.
//
// Unable to fail if: the group were a hand-written verb list (it is derived from the source, and
// Test 1 fails when that derivation returns nothing); if a gap with no exception row were merely
// warned about (it is an AddError plus a TestEqual on the count); or if the exception table were
// allowed to outlive its gap (an unused row is a failure, so fixing P2 turns the row that
// documented it into a red test naming itself). It would also be unable to fail if it asserted
// only that each verb declares SOMETHING: it asserts a named slot's spellings, then the
// requiredness, the shared description and the published default of that same slot.
// ================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureParitySharedHelperInputsTest,
    "PinWright.render.parameter_parity.SharedHelperInputsAreExposedUniformly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureParitySharedHelperInputsTest::RunTest(const FString& Parameters)
{
    using namespace CaptureVerbParameterParityTest;

    const TMap<FString, const FHandlerRegistration*> Registry = LoadRegistry();
    const FScannedSource Source = ScanRenderSources();
    if (!TestTrue(TEXT("Handlers/Render scan produced registration blocks"), Source.bUsable)
        || !TestTrue(TEXT("registry is populated"), Registry.Num() > 200))
    {
        return false;
    }

    const TArray<FHelperGroup> Groups = MakeHelperGroups();

    // group id -> member verbs, derived from the source
    TMap<FString, TArray<const FHandlerRegistration*>> MembersByGroup;
    for (const FHelperGroup& Group : Groups)
    {
        TSet<FString> Methods;
        for (const FTrackedHelper& Helper : Group.Helpers)
        {
            Methods.Append(SurveyHelper(Source, Helper).Members);
        }
        TArray<FString> Sorted = Methods.Array();
        Sorted.Sort();

        TArray<const FHandlerRegistration*>& Members = MembersByGroup.Add(Group.Id);
        for (const FString& Method : Sorted)
        {
            if (const FHandlerRegistration* const* Found = Registry.Find(Method))
            {
                Members.Add(*Found);
            }
        }
        if (!TestTrue(FString::Printf(TEXT("group '%s' has at least two registered members (saw %d)"),
                Group.Id, Members.Num()), Members.Num() >= 2))
        {
            return false;
        }
    }

    TSet<FString> ObservedGaps;
    int32 GapsWithoutRationale = 0;

    for (const FParitySlot& Slot : GParitySlots)
    {
        const TArray<const FHandlerRegistration*>* Members = MembersByGroup.Find(Slot.GroupId);
        if (Members == nullptr)
        {
            AddError(FString::Printf(TEXT("slot '%s' names group '%s', which was not resolved"),
                Slot.Id, Slot.GroupId));
            ++GapsWithoutRationale;
            continue;
        }
        const TArray<FString> Spellings = SplitSpellings(Slot.Spellings);

        // ---- declaration parity, through the shared pure evaluator
        for (const FString& Gap : FindDeclarationGaps(*Members, Spellings))
        {
            ObservedGaps.Add(GapKey(Gap, Slot.Id, GapKind::Declaration));
        }

        // ---- the checks below judge HOW a slot is declared, so they apply only to declarers
        TArray<const FHandlerRegistration*> Declarers;
        for (const FHandlerRegistration* Member : *Members)
        {
            if (Member != nullptr && FindSlotParam(*Member, Spellings) != nullptr)
            {
                Declarers.Add(Member);
            }
        }

        // ---- shared description vocabulary
        if (Slot.bWantsSharedMacro && Slot.SharedDescriptionMacro == nullptr)
        {
            // Several verbs declare the slot and there is no macro to share, which is a vocabulary
            // gap belonging to the slot rather than to any one verb.
            if (Declarers.Num() >= 2)
            {
                ObservedGaps.Add(GapKey(TEXT("*"), Slot.Id, GapKind::SharedMacro));
            }
        }
        else if (Slot.SharedDescriptionMacro != nullptr)
        {
            const FString Macro(Slot.SharedDescriptionMacro);
            TestTrue(FString::Printf(TEXT("slot '%s' shared description macro is substantial"), Slot.Id),
                Macro.Len() > 80);
            for (const FHandlerRegistration* Declarer : Declarers)
            {
                const FParamSpec* Spec = FindSlotParam(*Declarer, Spellings);
                // Prefix, not equality: a verb may append its own sentence by adjacent
                // string-literal concatenation, which is the shipped idiom. What is refused is a
                // description that does not START from the shared one.
                if (Spec != nullptr && !Spec->Description.StartsWith(Macro, ESearchCase::CaseSensitive))
                {
                    ObservedGaps.Add(GapKey(Declarer->MethodName, Slot.Id, GapKind::Description));
                }
            }
        }

        // ---- requiredness parity, against the majority of declarers
        if (Slot.bCheckRequiredness && Declarers.Num() >= 2)
        {
            int32 RequiredCount = 0;
            for (const FHandlerRegistration* Declarer : Declarers)
            {
                const FParamSpec* Spec = FindSlotParam(*Declarer, Spellings);
                RequiredCount += (Spec != nullptr && Spec->bRequired) ? 1 : 0;
            }
            const int32 OptionalCount = Declarers.Num() - RequiredCount;
            if (RequiredCount > 0 && OptionalCount > 0)
            {
                if (RequiredCount == OptionalCount)
                {
                    AddError(FString::Printf(
                        TEXT("slot '%s' splits %d required / %d optional with no majority: this check "
                             "cannot name an offender, and the even split is itself the defect."),
                        Slot.Id, RequiredCount, OptionalCount));
                    ++GapsWithoutRationale;
                }
                const bool bMajorityRequired = RequiredCount > OptionalCount;
                for (const FHandlerRegistration* Declarer : Declarers)
                {
                    const FParamSpec* Spec = FindSlotParam(*Declarer, Spellings);
                    if (Spec != nullptr && Spec->bRequired != bMajorityRequired)
                    {
                        ObservedGaps.Add(GapKey(Declarer->MethodName, Slot.Id, GapKind::Requiredness));
                    }
                }
            }
        }

        // ---- the published default, compared against the code's own constant
        if (Slot.ExpectedNumericDefault != nullptr)
        {
            for (const FHandlerRegistration* Declarer : Declarers)
            {
                const FParamSpec* Spec = FindSlotParam(*Declarer, Spellings);
                if (Spec == nullptr)
                {
                    continue;
                }
                const bool bPublished = !Spec->Default.IsEmpty()
                    && FMath::IsNearlyEqual(FCString::Atof(*Spec->Default), *Slot.ExpectedNumericDefault, 1.e-6f);
                if (!bPublished)
                {
                    ObservedGaps.Add(GapKey(Declarer->MethodName, Slot.Id, GapKind::Default));
                }
            }
        }
    }

    // The walk has to have looked at something. Zero observed gaps AND zero exception rows would
    // read exactly like a walk that evaluated nothing.
    TestEqual(TEXT("the parity walk resolved every modelled helper group"),
        MembersByGroup.Num(), Groups.Num());

    // ---- a gap must be covered by an exception row -------------------------------------------
    TSet<FString> ExcusedKeys;
    for (const FParityException& Exception : GSlotExceptions)
    {
        ExcusedKeys.Add(GapKey(Exception.Verb, Exception.SlotId, Exception.Kind));
    }

    TArray<FString> Uncovered = ObservedGaps.Difference(ExcusedKeys).Array();
    Uncovered.Sort();
    for (const FString& Key : Uncovered)
    {
        ++GapsWithoutRationale;
        AddError(FString::Printf(
            TEXT("Parameter-parity gap with no rationale: %s. Verbs that share a helper must expose "
                 "the same inputs. Either close the gap, or add a row to GSlotExceptions in this file "
                 "citing the file:line where the divergence is reasoned in writing."), *Key));
    }

    // ---- and an exception row must still describe a real gap ---------------------------------
    TArray<FString> Obsolete = ExcusedKeys.Difference(ObservedGaps).Array();
    Obsolete.Sort();
    for (const FString& Key : Obsolete)
    {
        AddError(FString::Printf(
            TEXT("Obsolete exception row: %s no longer describes a gap. This table is an inventory of "
                 "open defects, not a suppression file - delete the row now that the gap is closed, so "
                 "the next reader is not told a fixed thing is still broken."), *Key));
    }

    TestEqual(TEXT("every parity gap on a shared helper is either closed or has a written rationale"),
        GapsWithoutRationale, 0);
    TestEqual(TEXT("no exception row outlives the gap it documents"), Obsolete.Num(), 0);
    return true;
}

// ================================================================================================
// Test 3 -- the exception table's citations resolve.
//
// Unable to fail if: the citation were only checked for non-emptiness. Every row is opened on disk,
// its anchor phrase must be found within GCitationSlackLines of the cited line, and the anchor
// itself must contain the slot spelling or the verb's method name - so a row cannot be made to pass
// by citing a blank line or by anchoring on something that matches everywhere. It would also be
// unable to fail if a missing FILE were tolerated, or if an empty table passed the loop trivially;
// both are errors here, not skips.
// ================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureParityExceptionsCiteRationaleTest,
    "PinWright.render.parameter_parity.EveryExceptionCitesARationale",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureParityExceptionsCiteRationaleTest::RunTest(const FString& Parameters)
{
    using namespace CaptureVerbParameterParityTest;

    const FString PluginDir = ResolvePluginDir();
    if (!TestFalse(TEXT("resolved the plugin directory through IPluginManager"), PluginDir.IsEmpty()))
    {
        return false;
    }

    TArray<const FParityException*> AllRows;
    for (const FParityException& Row : GSlotExceptions)
    {
        AllRows.Add(&Row);
    }
    for (const FParityException& Row : GPoseListExceptions)
    {
        AllRows.Add(&Row);
    }
    // Non-vacuity: an empty table would make this loop pass while asserting nothing, and an empty
    // table is exactly what a "just delete the awkward rows" edit produces.
    if (!TestTrue(FString::Printf(TEXT("the exception tables are populated (saw %d rows)"), AllRows.Num()),
            AllRows.Num() >= 30))
    {
        return false;
    }

    // Slot spellings, so an anchor can be validated against the thing its row is about.
    TMap<FString, TArray<FString>> SpellingsBySlot;
    for (const FParitySlot& Slot : GParitySlots)
    {
        SpellingsBySlot.Add(Slot.Id, SplitSpellings(Slot.Spellings));
    }
    for (const FPoseListSetField& Field : GPoseListFields)
    {
        TArray<FString>& Spellings = SpellingsBySlot.FindOrAdd(Field.Field);
        Spellings.Add(Field.Field);
        const FString Wire(Field.WireSpelling);
        if (!Wire.IsEmpty())
        {
            Spellings.Add(Wire);
        }
    }

    int32 BadRows = 0;
    for (const FParityException* Row : AllRows)
    {
        const FString Key = GapKey(Row->Verb, Row->SlotId, Row->Kind);
        const FString Anchor(Row->Anchor);

        if (Anchor.Len() < 12)
        {
            AddError(FString::Printf(TEXT("%s: anchor '%s' is too short to identify a line."), *Key, *Anchor));
            ++BadRows;
            continue;
        }

        // The anchor must name what the row is about. Without this a row could cite any line at all
        // by anchoring on something generic, which is the failure this whole test exists to rule
        // out. A method name counts as well as a parameter name, because several of the rationale
        // lines in the corpus identify the verb rather than the knob.
        bool bAnchorNamesTheSubject = Anchor.Contains(Row->Verb, ESearchCase::IgnoreCase);
        if (const TArray<FString>* Spellings = SpellingsBySlot.Find(Row->SlotId))
        {
            for (const FString& Spelling : *Spellings)
            {
                bAnchorNamesTheSubject = bAnchorNamesTheSubject
                    || Anchor.Contains(Spelling, ESearchCase::IgnoreCase);
            }
        }
        if (!bAnchorNamesTheSubject)
        {
            AddError(FString::Printf(
                TEXT("%s: anchor '%s' names neither the parameter nor the verb, so it does not "
                     "establish that the cited line is about this gap."), *Key, *Anchor));
            ++BadRows;
            continue;
        }

        const FString Absolute = PluginDir / FString(Row->File);
        const FCitationVerdict Verdict = ResolveCitation(Absolute, Row->Line, Anchor);

        if (!Verdict.bFileFound)
        {
            AddError(FString::Printf(TEXT("%s: cited file '%s' could not be read."), *Key, Row->File));
            ++BadRows;
            continue;
        }
        if (!Verdict.bAnchorInFile)
        {
            AddError(FString::Printf(
                TEXT("%s: '%s:%d' - the anchor text is nowhere in that file. The rationale this row "
                     "claims to rest on has been rewritten or removed; re-cite it, or close the gap."),
                *Key, Row->File, Row->Line));
            ++BadRows;
            continue;
        }
        if (!Verdict.bResolved)
        {
            AddError(FString::Printf(
                TEXT("%s: '%s:%d' is stale - the anchor now sits at line %d (tolerance %d lines). "
                     "Update the row's line number."),
                *Key, Row->File, Row->Line, Verdict.NearestLine, GCitationSlackLines));
            ++BadRows;
        }
    }

    return TestEqual(TEXT("every parity exception cites a rationale that still resolves on disk"), BadRows, 0);
}

// ================================================================================================
// Test 4 -- the known defect is caught.
//
// Unable to fail if: it were written against the post-fix spec lists only, in which case it would
// prove the check accepts a good state and nothing at all about whether it rejects a bad one. It
// feeds a SYNTHETIC pre-fix pair - camera.orbit_shots with the margin welded, camera.frame_actor
// with no distance override, which is exactly P1 - to the same FindDeclarationGaps the live walk
// uses, and asserts the gaps come back on the right verbs. Then it feeds the post-fix pair and
// asserts they do not, so a checker that simply reported everything would fail here too.
// ================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureParityKnownDefectCaughtTest,
    "PinWright.render.parameter_parity.TheKnownDefectIsCaught",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureParityKnownDefectCaughtTest::RunTest(const FString& Parameters)
{
    using namespace CaptureVerbParameterParityTest;

    const TArray<FString> PaddingSlot  = SplitSpellings(TEXT("padding"));
    const TArray<FString> DistanceSlot = SplitSpellings(TEXT("distance,radius"));

    // ---- the tree as it stood before R5: opposite halves of one helper ----
    FHandlerRegistration PreFixOrbit;
    PreFixOrbit.MethodName = TEXT("camera.orbit_shots");
    PreFixOrbit.Params = RPC_PARAMS(
        RPC_PARAM_OPT("radius", "number", "Orbit radius / camera distance."),
        RPC_PARAM_OPT("fov", "number", "Field of view."));

    FHandlerRegistration PreFixFrame;
    PreFixFrame.MethodName = TEXT("camera.frame_actor");
    PreFixFrame.Params = RPC_PARAMS(
        RPC_PARAM_OPT("padding", "number", "Bounds-fit margin multiplier."),
        RPC_PARAM_OPT("fov", "number", "Field of view."));

    const TArray<const FHandlerRegistration*> PreFix = { &PreFixOrbit, &PreFixFrame };

    const TArray<FString> PaddingGapsBefore = FindDeclarationGaps(PreFix, PaddingSlot);
    TestEqual(TEXT("pre-fix: exactly one verb cannot reach the fit margin"), PaddingGapsBefore.Num(), 1);
    if (PaddingGapsBefore.Num() == 1)
    {
        TestEqual(TEXT("pre-fix: the margin gap is camera.orbit_shots"),
            PaddingGapsBefore[0], FString(TEXT("camera.orbit_shots")));
    }

    const TArray<FString> DistanceGapsBefore = FindDeclarationGaps(PreFix, DistanceSlot);
    TestEqual(TEXT("pre-fix: exactly one verb cannot reach the distance override"), DistanceGapsBefore.Num(), 1);
    if (DistanceGapsBefore.Num() == 1)
    {
        TestEqual(TEXT("pre-fix: the distance gap is camera.frame_actor"),
            DistanceGapsBefore[0], FString(TEXT("camera.frame_actor")));
    }

    // ---- the same pair after R5 ----
    FHandlerRegistration PostFixOrbit;
    PostFixOrbit.MethodName = TEXT("camera.orbit_shots");
    PostFixOrbit.Params = RPC_PARAMS(
        RPC_PARAM_OPT("radius", "number", "Orbit radius / camera distance."),
        RPC_PARAM_DEF("padding", "number", "Bounds-fit margin multiplier.", "1.15"),
        RPC_PARAM_OPT("fov", "number", "Field of view."));

    FHandlerRegistration PostFixFrame;
    PostFixFrame.MethodName = TEXT("camera.frame_actor");
    PostFixFrame.Params = RPC_PARAMS(
        RPC_PARAM_DEF("padding", "number", "Bounds-fit margin multiplier.", "1.15"),
        RPC_PARAM_OPT("distance", "number", "Explicit camera distance."),
        RPC_PARAM_OPT("fov", "number", "Field of view."));

    const TArray<const FHandlerRegistration*> PostFix = { &PostFixOrbit, &PostFixFrame };

    TestEqual(TEXT("post-fix: no verb is missing the fit margin"),
        FindDeclarationGaps(PostFix, PaddingSlot).Num(), 0);
    TestEqual(TEXT("post-fix: no verb is missing the distance override"),
        FindDeclarationGaps(PostFix, DistanceSlot).Num(), 0);

    // An alias is a real wire spelling, so a verb reachable only through one is not a gap. Without
    // this the checker would invent a false gap the first time a slot gains a compat alias.
    FHandlerRegistration AliasOnly;
    AliasOnly.MethodName = TEXT("_test.alias_only");
    AliasOnly.Params.Add(FParamSpec{ TEXT("orthoWidth"), TEXT("number"), TEXT("desc"), false,
        TEXT("2000"), TArray<FString>({ TEXT("orthoWorldWidth") }) });
    const TArray<const FHandlerRegistration*> AliasMembers = { &AliasOnly };
    TestEqual(TEXT("a slot reached through a registered alias is not a gap"),
        FindDeclarationGaps(AliasMembers, SplitSpellings(TEXT("orthoWorldWidth"))).Num(), 0);

    return true;
}

// ================================================================================================
// Test 5 -- the set-level primitive's fields, and P5.
//
// Unable to fail if: it asserted only that FPoseListCaptureRequest has the fields the model lists.
// The model is compared to the PARSED struct in both directions, so a field added without a row
// fails and a row for a removed field fails; every wire-reachable field is then checked against
// every verb that builds the struct; and a verb that DECLARES a parameter without assigning the
// field it feeds is reported, which is the failure mode that shipped camera.orbit_shots' inert
// viewMode. It would also be unable to fail if P5 were phrased as one claim - "viewDistanceScale is
// unused" - so all three halves are asserted separately: the struct carries it, the primitive
// forwards it into every frame, and no verb that builds the struct either declares or assigns it
// while two verbs that never build it do declare it.
//
// COUNTERFACTUAL: delete the P5 assertions and the plumbing can be quietly removed, or quietly
// wired to one verb only, with nothing anywhere to notice either.
// ================================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureParityPoseListFieldsTest,
    "PinWright.render.parameter_parity.PoseListSetFieldsAreReachableFromTheWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureParityPoseListFieldsTest::RunTest(const FString& Parameters)
{
    using namespace CaptureVerbParameterParityTest;

    const TMap<FString, const FHandlerRegistration*> Registry = LoadRegistry();
    const FScannedSource Source = ScanRenderSources();
    if (!TestTrue(TEXT("Handlers/Render scan produced registration blocks"), Source.bUsable)
        || !TestTrue(TEXT("registry is populated"), Registry.Num() > 200))
    {
        return false;
    }

    const FString HeaderPath = ResolveRenderHandlerDir() / TEXT("PoseListCapture.h");
    TArray<FString> ParsedFields;
    if (!TestTrue(TEXT("parsed FPoseListCaptureRequest out of PoseListCapture.h"),
            ParseStructFields(HeaderPath, TEXT("FPoseListCaptureRequest"), ParsedFields)))
    {
        AddError(TEXT("Could not parse the struct. Every assertion below is about its fields, so this "
                      "is a hard stop rather than a pass over an empty set."));
        return false;
    }
    if (!TestTrue(FString::Printf(TEXT("the struct parse found a plausible field count (saw %d)"),
            ParsedFields.Num()), ParsedFields.Num() >= 15))
    {
        return false;
    }

    // ---- the model and the struct must agree, in both directions ----
    TSet<FString> ModelledFields;
    for (const FPoseListSetField& Field : GPoseListFields)
    {
        ModelledFields.Add(Field.Field);
    }
    const TSet<FString> ParsedSet(ParsedFields);

    for (const FString& Field : ParsedSet.Difference(ModelledFields))
    {
        AddError(FString::Printf(
            TEXT("FPoseListCaptureRequest::%s is a set-level field with no row in GPoseListFields. "
                 "Classify it: give it the wire spelling every verb on this primitive must declare, or "
                 "record why the caller never names it. An unclassified field is how viewDistanceScale "
                 "came to be carried, forwarded and unreachable."), *Field));
    }
    for (const FString& Field : ModelledFields.Difference(ParsedSet))
    {
        AddError(FString::Printf(
            TEXT("GPoseListFields models FPoseListCaptureRequest::%s, which the header no longer "
                 "declares. If it was renamed, rename the row; if it was removed, delete the row."), *Field));
    }
    TestEqual(TEXT("every set-level field is classified and every classification is live"),
        ParsedSet.Num(), ModelledFields.Num());

    // ---- which verbs build the struct, and what they assign on it ----
    TSet<FString> MemberMethods;
    const TMap<FString, TSet<FString>> AssignmentsByMethod =
        SurveyPoseRequestAssignments(Source, MemberMethods);

    TArray<FString> Members = MemberMethods.Array();
    Members.Sort();
    if (!TestTrue(FString::Printf(TEXT("several verbs build an FPoseListCaptureRequest (saw %d)"),
            Members.Num()), Members.Num() >= 4))
    {
        return false;
    }

    TSet<FString> ObservedGaps;
    int32 InertParameters = 0;

    for (const FPoseListSetField& Field : GPoseListFields)
    {
        const FString Wire(Field.WireSpelling);
        if (Wire.IsEmpty())
        {
            continue;   // computed by the verb; the classification row is the record of that
        }

        // A spelling no verb declares ANYWHERE is either a typo in the model or the P5/P6 shape, and
        // the model has to say which. Both directions are checked, so a row claiming undeclarable
        // while the parameter has since shipped fails as well.
        bool bDeclaredSomewhere = false;
        for (const TPair<FString, const FHandlerRegistration*>& Entry : Registry)
        {
            if (Entry.Value != nullptr && FindParam(*Entry.Value, Wire) != nullptr)
            {
                bDeclaredSomewhere = true;
                break;
            }
        }
        if (Field.bNoVerbDeclaresItAnywhere)
        {
            TestFalse(FString::Printf(
                    TEXT("FPoseListCaptureRequest::%s is still declared by no verb at all (if '%s' has "
                         "since shipped, clear bNoVerbDeclaresItAnywhere and drop its exception rows)"),
                    Field.Field, *Wire),
                bDeclaredSomewhere);
        }
        else
        {
            TestTrue(FString::Printf(
                    TEXT("wire spelling '%s' for FPoseListCaptureRequest::%s is declared by at least one "
                         "verb (otherwise the model has a typo and every verb reads as a gap)"),
                    *Wire, Field.Field),
                bDeclaredSomewhere);
        }

        for (const FString& Method : Members)
        {
            const FHandlerRegistration* const* Reg = Registry.Find(Method);
            if (Reg == nullptr || *Reg == nullptr)
            {
                continue;
            }
            const bool bDeclares = FindParam(**Reg, Wire) != nullptr;
            if (!bDeclares)
            {
                ObservedGaps.Add(GapKey(Method, Field.Field, GapKind::Declaration));
                continue;
            }
            // Declared AND reachable: the verb must actually put the parsed value on the request, or
            // the parameter is inert on the wire while reading as supported.
            const TSet<FString>* Assigned = AssignmentsByMethod.Find(Method);
            const bool bAssigns = Assigned != nullptr && Assigned->Contains(Field.Field);
            if (!bAssigns)
            {
                ++InertParameters;
                AddError(FString::Printf(
                    TEXT("%s declares '%s' but never assigns FPoseListCaptureRequest::%s, so the "
                         "parameter is inert: the call succeeds and the value reaches no frame. This is "
                         "how camera.orbit_shots shipped a viewMode that did nothing."),
                    *Method, *Wire, Field.Field));
            }
        }
    }

    // ---- P5, stated as its three separate halves ---------------------------------------------
    // (a) the struct still carries the plumbing.
    TestTrue(TEXT("P5(a): FPoseListCaptureRequest still carries ViewDistanceScale"),
        ParsedSet.Contains(TEXT("ViewDistanceScale")));

    // (b) the shared primitive forwards it into every frame of every set.
    {
        FString PrimitiveSource;
        const FString PrimitivePath = ResolveRenderHandlerDir() / TEXT("PoseListCapture.cpp");
        if (TestTrue(TEXT("read PoseListCapture.cpp"),
                FFileHelper::LoadFileToString(PrimitiveSource, *PrimitivePath)))
        {
            TestTrue(TEXT("P5(b): the per-frame request is still filled from Request.ViewDistanceScale"),
                PrimitiveSource.Contains(TEXT("Frame.ViewDistanceScale = Request.ViewDistanceScale"),
                    ESearchCase::CaseSensitive));
        }
    }

    // (c) no verb that builds the struct assigns any of the three fields, while the wire spelling IS
    //     declared - by verbs that never build the struct at all. Dead on the path that carries it;
    //     unreachable from the verbs that carry it.
    const TCHAR* ViewDistanceFields[] =
        { TEXT("ViewDistanceScale"), TEXT("bViewDistanceScaleProvided"), TEXT("bAutoViewDistanceScale") };
    for (const TCHAR* FieldName : ViewDistanceFields)
    {
        for (const FString& Method : Members)
        {
            const TSet<FString>* Assigned = AssignmentsByMethod.Find(Method);
            const bool bAssigns = Assigned != nullptr && Assigned->Contains(FieldName);
            TestFalse(FString::Printf(
                    TEXT("P5(c): %s does not assign FPoseListCaptureRequest::%s (if it now does, P5 is "
                         "being fixed - delete the five ViewDistanceScale exception rows)"),
                    *Method, FieldName),
                bAssigns);
        }
    }

    TArray<FString> DeclaringVerbs;
    for (const TPair<FString, const FHandlerRegistration*>& Entry : Registry)
    {
        if (Entry.Value != nullptr && FindParam(*Entry.Value, TEXT("viewDistanceScale")) != nullptr)
        {
            DeclaringVerbs.Add(Entry.Key);
        }
    }
    DeclaringVerbs.Sort();
    TestTrue(FString::Printf(
            TEXT("P5: 'viewDistanceScale' is a live wire spelling (declared by %d verb(s): %s)"),
            DeclaringVerbs.Num(), *FString::Join(DeclaringVerbs, TEXT(", "))),
        DeclaringVerbs.Num() > 0);
    for (const FString& Method : DeclaringVerbs)
    {
        TestFalse(FString::Printf(
                TEXT("P5: %s declares 'viewDistanceScale' and does NOT build an FPoseListCaptureRequest "
                     "- the parameter and the plumbing are on disjoint paths"), *Method),
            MemberMethods.Contains(Method));
    }

    // ---- gaps must be covered, and rows must still describe gaps ------------------------------
    TSet<FString> ExcusedKeys;
    for (const FParityException& Exception : GPoseListExceptions)
    {
        ExcusedKeys.Add(GapKey(Exception.Verb, Exception.SlotId, Exception.Kind));
    }

    TArray<FString> Uncovered = ObservedGaps.Difference(ExcusedKeys).Array();
    Uncovered.Sort();
    for (const FString& Key : Uncovered)
    {
        AddError(FString::Printf(
            TEXT("Set-level field unreachable from the wire with no rationale: %s. Every verb that "
                 "builds an FPoseListCaptureRequest gets this field applied to every frame of its set, "
                 "so a verb that cannot name it is a parity gap. Close it, or add a row to "
                 "GPoseListExceptions citing where the refusal is reasoned in writing."), *Key));
    }

    TArray<FString> Obsolete = ExcusedKeys.Difference(ObservedGaps).Array();
    Obsolete.Sort();
    for (const FString& Key : Obsolete)
    {
        AddError(FString::Printf(
            TEXT("Obsolete exception row: %s no longer describes a gap - delete it."), *Key));
    }

    TestEqual(TEXT("every set-level field is reachable from every verb on the primitive, or excepted"),
        Uncovered.Num(), 0);
    TestEqual(TEXT("no set-level exception row outlives its gap"), Obsolete.Num(), 0);
    TestEqual(TEXT("no verb declares a set-level parameter it never assigns"), InertParameters, 0);
    return true;
}
