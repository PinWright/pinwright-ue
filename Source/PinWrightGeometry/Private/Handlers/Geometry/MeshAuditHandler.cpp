// Copyright (c) 2026 Alexander Penkin. MIT License.

// MeshAuditHandler.cpp - geometry.audit_static_meshes, the RPC face of MeshAuditUtils.
//
// The verb answers ONE question that had no way to be asked before it: "are any of my shipped
// meshes inside out?" geometry.check_health could answer it for a live ADynamicMeshActor, and
// geometry.create_from_static_mesh could produce such an actor from a saved asset - but only by
// SPAWNING one, per asset, which is both mutating and unbatched. See MeshAuditUtils.h.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/Geometry/MeshAuditUtils.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Modules/ModuleManager.h"
#include "Utils/PathUtils.h"

// Every helper below is prefixed MeshAudit. This module builds with bUseUnity = true, so two
// anonymous-namespace statics sharing a name in different .cpp files become an ODR redefinition
// once the TUs are merged.
namespace
{
    constexpr int32 MeshAuditDefaultLimit = 50;
    constexpr int32 MeshAuditMaxLimit = 500;
    constexpr int32 MeshAuditDefaultMaxFindings = 200;
    constexpr int32 MeshAuditMaxFindings = 2000;
    constexpr int32 MeshAuditMaxCleanAssets = 500;
    constexpr int32 MeshAuditMaxErrorChars = 200;

    // Both arms of the excludeChecks ternary must be const lvalues, or the range-for would
    // iterate a temporary copy of the array on the present branch too.
    const TArray<TSharedPtr<FJsonValue>>& MeshAuditEmptyArray()
    {
        static const TArray<TSharedPtr<FJsonValue>> Empty;
        return Empty;
    }

    FString MeshAuditTruncate(const FString& In, int32 Max)
    {
        return In.Len() <= Max ? In : (In.Left(Max) + TEXT("..."));
    }

}

// ================= geometry.audit_static_meshes =================

REGISTER_RPC_HANDLER("geometry.audit_static_meshes", "geometry",
    "Sweep a folder (or a named set) of SAVED StaticMesh assets in ONE call and report the ones "
    "that are demonstrably wrong: inside out per edge-connected component (with each component's "
    "signed volume reported), wound inconsistently, open, degenerate, non-manifold, empty, "
    "mirrored by a negative Build Scale, spatially isolated, or at geometric risk of z-fighting. "
    "A whole-mesh signed-volume sum is retained as context "
    "only because separate shells can cancel. An open, degenerate or near-zero-volume component "
    "is UNKNOWN, never clean. Nothing is spawned - unlike geometry.create_from_static_mesh, which reaches a saved "
    "mesh only by placing an actor for it, one per asset - and nothing is loaded for writing, "
    "modified or saved. "
    "This is the batched form on purpose: use it instead of looping geometry.check_health over a "
    "folder. "
    "The report says what it COULD NOT check: every check reports per asset as flagged, clean, "
    "unrunnable or not-applicable, so an asset that would not load never reads as one that "
    "passed. An empty match set is an ERROR, not a zero-item success, because a typo'd folder "
    "otherwise looks exactly like a folder of correct meshes. "
    "Returns a JOB TICKET: every asset costs a package load, since orientation is not in the "
    "asset registry's tags.",
    RPC_PARAMS(
        RPC_PARAM_OPT("folder", "path",
            "Content folder to sweep, e.g. /Game/Meshes. Provide exactly one of folder or assets; "
            "there is no default, because a folder guessed for the caller would audit assets they "
            "did not ask about."),
        RPC_PARAM_OPT("assets", "array",
            "Explicit StaticMesh asset paths to sweep instead of a folder. Both forms are "
            "accepted: the package path /Game/Folder/SM_Name and the object path "
            "/Game/Folder/SM_Name.SM_Name. A string that is not a content path at all is "
            "rejected with INVALID_ARGUMENT before the sweep runs - a bad argument is not a "
            "finding about a mesh. A well-formed path the registry has nothing at is reported "
            "UNRUNNABLE with ASSET_NOT_FOUND, not dropped and not confused with a mesh that "
            "would not load."),
        RPC_PARAM_DEF("recursive", "boolean", "Include sub-folders of `folder`.", "true"),
        FParamSpec{TEXT("namePattern"), TEXT("string"),
            TEXT("Wildcard filter on the asset name (`*`, `?`), case-insensitive, e.g. SM_Tree_*. "
                 "Applied after the folder match; if it matches nothing, that is the same error an "
                 "empty folder gives."),
            false, TEXT(""), TArray<FString>({TEXT("name_pattern")})},
        RPC_PARAM_OPT("checks", "array",
            "Check ids to run, e.g. [\"inverted\",\"inconsistent_winding\"]. Omit for the default "
            "set (everything except thin_shell). An unknown id is an ERROR, not a skip: a typo "
            "that silently ran nothing looks exactly like a folder of clean meshes. The response "
            "echoes every check with whether it ran."),
        RPC_PARAM_OPT("excludeChecks", "array",
            "Check ids to subtract from the resolved set. Applied after `checks`."),
        FParamSpec{TEXT("failOn"), TEXT("string"),
            TEXT("Severity that makes pass false: error | any | none. Independent of the two other "
                 "pass terms - an unrunnable check or a truncated sweep fails regardless."),
            false, TEXT("error"), TArray<FString>({TEXT("fail_on")})},
        FParamSpec{TEXT("lodType"), TEXT("string"),
            TEXT("Which mesh inside the asset is measured: MaxAvailable | HiResSourceModel | "
                 "SourceModel | RenderData. RenderData is the BUILT mesh, split at every UV seam and "
                 "hard-normal crease, so a perfectly closed authored mesh reads there as thousands of "
                 "boundary edges and every closed-only check except `inverted` goes not-applicable. "
                 "`inverted` reports those open components as UNKNOWN/unrunnable. Do not use it for an "
                 "orientation verdict."),
            false, TEXT("MaxAvailable"), TArray<FString>({TEXT("lod_type")})},
        RPC_PARAM_DEF("lodIndex", "integer",
            "LOD index for SourceModel / RenderData. The engine SILENTLY CLAMPS this to the "
            "available LOD count, so the echoed value is what you asked for, not necessarily what "
            "was read.", "0"),
        RPC_PARAM_DEF("applyBuildSettings", "boolean",
            "Apply the asset's Build Settings during the read.", "true"),
        RPC_PARAM_DEF("useBuildScale", "boolean",
            "Apply the asset's Build Scale during the read. Default true so the sweep measures the "
            "mesh AS IT SHIPS: a Build Scale with an odd number of negative axes mirrors the "
            "geometry, and reading without it would report that asset healthy.", "true"),
        RPC_PARAM_DEF("minVolumeRatio", "number",
            "Scale-free threshold on |signedVolume| / surfaceArea^1.5. `inverted` uses it to mark "
            "a closed component UNKNOWN when its volume is too small to answer winding; "
            "`thin_shell` uses the same threshold to flag a thin shell. A cube reads 0.068 and a "
            "sphere 0.094.", "0.001"),
        RPC_PARAM_DEF("floatingToleranceFraction", "number",
            "Dimensionless fraction of the model's bounding-sphere radius used to link "
            "components. The narrow phase measures triangle distance; use this only to tune "
            "the scale for a particular asset, not as a project-unit distance.", "0.005"),
        RPC_PARAM_DEF("limit", "integer",
            "Assets this call loads and measures (1-500). Bounds the work: every audited asset "
            "costs a package load.", "50"),
        RPC_PARAM_DEF("offset", "integer",
            "Assets to skip before the page. Ordering is by asset path, so paging is stable and a "
            "retry converges.", "0"),
        RPC_PARAM_DEF("maxFindings", "integer",
            "Cap on findings[] ROWS (1-2000). Per-check tallies stay exact when rows are clipped, "
            "and a clipped page reports findingsTruncated and cannot pass.", "200"),
        RPC_PARAM_DEF("includeClean", "boolean",
            "Also list the asset paths that produced no finding at all (capped at 500).", "false")
    ))
{
    using namespace MeshAudit;

    // ---- scope: exactly one of folder / assets ----
    // Rejecting both is not pedantry. Silently preferring one would make a typo'd folder look
    // like a successful sweep of the other, which is the same class of lie as reporting an
    // empty match set as clean.
    FString RawFolder = Ctx.GetString(TEXT("folder")).TrimStartAndEnd();
    TArray<FString> ExplicitAssets;
    if (const TArray<TSharedPtr<FJsonValue>>* AssetArray = Ctx.GetArray(TEXT("assets")))
    {
        for (const TSharedPtr<FJsonValue>& Value : *AssetArray)
        {
            FString Entry;
            if (Value.IsValid() && Value->TryGetString(Entry))
            {
                Entry.TrimStartAndEndInline();
                if (!Entry.IsEmpty()) { ExplicitAssets.AddUnique(Entry); }
            }
        }
    }

    if (RawFolder.IsEmpty() == (ExplicitAssets.Num() == 0))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("Provide exactly one of `folder` (a content folder to sweep) or `assets` (an "
                 "explicit list of StaticMesh paths). With neither, there is nothing to audit; "
                 "with both, the sweep would have to guess which one you meant."));
        return true;
    }

    // ---- checks ----
    uint32 Selected = DefaultCheckMask();
    const TArray<TSharedPtr<FJsonValue>>* RequestedChecks = Ctx.GetArray(TEXT("checks"));
    if (RequestedChecks && RequestedChecks->Num() > 0)
    {
        Selected = 0;
        for (const TSharedPtr<FJsonValue>& Value : *RequestedChecks)
        {
            FString Id;
            if (!Value.IsValid() || !Value->TryGetString(Id)) { continue; }
            ECheck Check;
            if (!ParseCheckId(Id, Check))
            {
                // An unknown id ERRORS rather than being skipped. A `checks` array that
                // silently ran nothing would be indistinguishable from a folder of correct
                // meshes - the exact failure this verb exists to make impossible.
                const FString Valid = PinWrightAudit::ValidCheckIdList(AllChecks());
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(TEXT("Unknown check id '%s'. Valid ids: %s."),
                        *MeshAuditTruncate(Id, MeshAuditMaxErrorChars), *Valid));
                return true;
            }
            Selected |= CheckBit(Check);
        }
    }
    const TArray<TSharedPtr<FJsonValue>>* ExcludedChecks = Ctx.GetArray(TEXT("excludeChecks"));
    for (const TSharedPtr<FJsonValue>& Value : ExcludedChecks ? *ExcludedChecks
                                                              : MeshAuditEmptyArray())
    {
        FString Id;
        if (!Value.IsValid() || !Value->TryGetString(Id)) { continue; }
        ECheck Check;
        if (!ParseCheckId(Id, Check))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("Unknown check id '%s' in excludeChecks."),
                    *MeshAuditTruncate(Id, MeshAuditMaxErrorChars)));
            return true;
        }
        Selected &= ~CheckBit(Check);
    }
    if (Selected == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("Every check was excluded, so the sweep would examine assets and answer nothing. "
                 "A run that measures nothing must not be able to report pass."));
        return true;
    }

    const FString FailOnToken = Ctx.GetStringFirstOf({TEXT("failOn"), TEXT("fail_on")},
                                                     TEXT("error")).ToLower();
    PinWrightAudit::EFailOn FailOnMode;
    if (!PinWrightAudit::ParseFailOn(FailOnToken, FailOnMode))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown failOn '%s'. Valid: error, any, none."), *FailOnToken));
        return true;
    }
    const FString FailOn = PinWrightAudit::FailOnToWire(FailOnMode);

    ELodSource LodSource = ELodSource::MaxAvailable;
    const FString LodToken = Ctx.GetStringFirstOf({TEXT("lodType"), TEXT("lod_type")}, FString());
    if (!ParseLodSource(LodToken, LodSource))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("Unknown lodType '%s'. Valid: MaxAvailable, HiResSourceModel, ")
                            TEXT("SourceModel, RenderData."),
                *MeshAuditTruncate(LodToken, MeshAuditMaxErrorChars)));
        return true;
    }

    FConfig Config;
    Config.SelectedChecks = Selected;
    Config.Read.LodSource = LodSource;
    Config.Read.LodIndex = FMath::Max(0, Ctx.GetInt(TEXT("lodIndex"), 0));
    Config.Read.bApplyBuildSettings = Ctx.GetBool(TEXT("applyBuildSettings"), true);
    Config.Read.bUseBuildScale = Ctx.GetBool(TEXT("useBuildScale"), true);
    Config.Thresholds.MinVolumeRatio =
        FMath::Max(0.0, Ctx.GetNumber(TEXT("minVolumeRatio"), 1.0e-3));
    const double FloatingToleranceFraction = Ctx.GetNumber(
        TEXT("floatingToleranceFraction"), MeshAudit::DefaultFloatingToleranceFraction);
    if (!FMath::IsFinite(FloatingToleranceFraction) || FloatingToleranceFraction < 0.0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("floatingToleranceFraction must be a finite, non-negative number."));
        return true;
    }
    Config.Thresholds.FloatingToleranceFraction = FloatingToleranceFraction;
    Config.MaxFindings = FMath::Clamp(
        Ctx.GetInt(TEXT("maxFindings"), MeshAuditDefaultMaxFindings), 1, MeshAuditMaxFindings);
    Config.MaxCleanAssets = Ctx.GetBool(TEXT("includeClean"), false) ? MeshAuditMaxCleanAssets : 0;

    const bool bRecursive = Ctx.GetBool(TEXT("recursive"), true);
    const int32 Limit = FMath::Clamp(Ctx.GetInt(TEXT("limit"), MeshAuditDefaultLimit),
                                     1, MeshAuditMaxLimit);
    const int32 Offset = FMath::Max(0, Ctx.GetInt(TEXT("offset"), 0));
    const FString NamePattern = Ctx.GetStringFirstOf({TEXT("namePattern"), TEXT("name_pattern")},
                                                     FString()).TrimStartAndEnd();

    FAssetRegistryModule& RegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = RegistryModule.Get();
    const bool bScanning = AssetRegistry.IsLoadingAssets();

    TArray<FAssetData> Matched;
    TArray<FString> Unresolved;
    FString ScopeDescription;

    if (!RawFolder.IsEmpty())
    {
        FString Folder = SanitizeProjectRelativePath(RawFolder);
        while (Folder.Len() > 1 && Folder.EndsWith(TEXT("/"))) { Folder.LeftChopInline(1); }
        if (Folder.IsEmpty() || !IsValidAssetPath(Folder) || Folder.Contains(TEXT(".")))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
                FString::Printf(
                    TEXT("'folder' is '%s', which is not a content folder path. Expected a mounted ")
                    TEXT("package path with no object suffix, e.g. /Game/Meshes."),
                    *MeshAuditTruncate(RawFolder, MeshAuditMaxErrorChars)));
            return true;
        }

        FARFilter Filter;
        Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
        // Recursive classes so a UStaticMesh SUBCLASS is enumerated rather than silently
        // skipped: a skipped asset is invisible, whereas one that fails to read is reported
        // with its own unrunnable code.
        Filter.bRecursiveClasses = true;
        Filter.bRecursivePaths = bRecursive;
        Filter.PackagePaths.Add(FName(*Folder));
        AssetRegistry.GetAssets(Filter, Matched);

        if (!NamePattern.IsEmpty())
        {
            Matched.RemoveAll([&NamePattern](const FAssetData& Data)
            {
                return !Data.AssetName.ToString().MatchesWildcard(NamePattern,
                                                                  ESearchCase::IgnoreCase);
            });
        }
        ScopeDescription = FString::Printf(TEXT("%s%s"), *Folder,
            bRecursive ? TEXT(" (recursive)") : TEXT(" (this folder only)"));
    }
    else
    {
        for (const FString& Entry : ExplicitAssets)
        {
            // `assets` entries are accepted in BOTH forms - the package path /Game/A/SM_X and
            // the object path /Game/A/SM_X.SM_X - because FSoftObjectPath is invalid for the
            // former, and the previous code read that invalidity as "the asset would not load".
            // A caller who typed the shorter form got seven Unrunnable findings and pass:false
            // against a perfectly healthy mesh: an argument mistake wearing a content defect's
            // costume (B-mesh-audit-package-path-reads-as-broken-asset).
            //
            // The three outcomes are now three different answers:
            //   * not a content path at all -> INVALID_ARGUMENT, HERE, before the sweep. It
            //     never enters the findings structure, because it is not a fact about an asset.
            //   * well formed, resolves    -> audited.
            //   * well formed, absent      -> ASSET_NOT_FOUND, folded in below as unmeasured.
            const FString Sanitized = SanitizeProjectRelativePath(Entry);
            FString ObjectPath;
            FString NormalizeError;
            if (Sanitized.IsEmpty()
                || !NormalizeToObjectPath(Sanitized, ObjectPath, NormalizeError))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                    FString::Printf(
                        TEXT("'%s' in `assets` is not a StaticMesh asset path. %s Pass either ")
                        TEXT("the package path /Game/Folder/SM_Name or the object path ")
                        TEXT("/Game/Folder/SM_Name.SM_Name. This is rejected rather than ")
                        TEXT("reported as an unreadable asset, because a bad argument is not a ")
                        TEXT("finding about a mesh."),
                        *MeshAuditTruncate(Entry, MeshAuditMaxErrorChars),
                        NormalizeError.IsEmpty() ? TEXT("") : *NormalizeError));
                return true;
            }
            FAssetData Data = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
            // A well-formed path that resolves to nothing is carried through as UNRESOLVED and
            // reported unrunnable below, never dropped: silently shrinking the requested set is
            // how a sweep ends up reporting a clean verdict over assets it never saw.
            if (Data.IsValid()) { Matched.Add(MoveTemp(Data)); }
            else { Unresolved.Add(ObjectPath); }
        }
        ScopeDescription = FString::Printf(TEXT("%d explicit asset path(s)"),
                                           ExplicitAssets.Num());
    }

    // Sorted by object path so `offset` addresses the same asset on every call - the property
    // that makes paging stable and a retried page converge.
    Matched.Sort([](const FAssetData& A, const FAssetData& B)
    {
        return A.GetObjectPathString().Compare(B.GetObjectPathString(),
                                               ESearchCase::CaseSensitive) < 0;
    });

    const int32 Total = Matched.Num() + Unresolved.Num();

    if (Total == 0)
    {
        // Zero is not a small number. A folder that matched nothing and a folder full of
        // correct meshes must not produce the same-shaped answer - this project has exited 0
        // having examined nothing before, and that is what this branch exists to prevent.
        Ctx.SendError(ErrorCodes::ERR_NO_ASSETS_MATCHED,
            FString::Printf(
                TEXT("No StaticMesh assets matched %s%s. Nothing was measured, so this is an ")
                TEXT("error rather than a clean sweep: check the path%s%s."),
                *ScopeDescription,
                NamePattern.IsEmpty() ? TEXT("")
                                      : *FString::Printf(TEXT(" with namePattern '%s'"), *NamePattern),
                NamePattern.IsEmpty() ? TEXT(" and 'recursive'") : TEXT(", 'recursive' and the pattern"),
                bScanning
                    ? TEXT("; the asset registry is still scanning, so the answer may be "
                           "incomplete until the scan finishes")
                    : TEXT("")));
        return true;
    }

    if (Offset >= Total)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(
                TEXT("'offset' is %d but the match set holds only %d asset(s), so the page would ")
                TEXT("audit nothing. An empty page reported as a clean sweep is the one answer ")
                TEXT("this verb must not give; pass an offset below %d."), Offset, Total, Total));
        return true;
    }

    const int32 PageSize = FMath::Min(Limit, Matched.Num() - FMath::Min(Offset, Matched.Num()));
    TArray<FAssetData> Page;
    Page.Reserve(FMath::Max(0, PageSize));
    for (int32 Index = 0; Index < PageSize; ++Index)
    {
        Page.Add(Matched[Offset + Index]);
    }

    TSharedPtr<FJsonObject> Started = MakeShared<FJsonObject>();
    Started->SetStringField(TEXT("scope"), ScopeDescription);
    Started->SetNumberField(TEXT("total"), Total);
    Started->SetNumberField(TEXT("examining"), PageSize + (Offset == 0 ? Unresolved.Num() : 0));

    FJobBindArgs Args;
    Args.Method = TEXT("geometry.audit_static_meshes");
    Args.StartedPayload = Started;
    Args.BindNativeDelegate =
        [Page = MoveTemp(Page), Unresolved, Config, Offset, Total, FailOn, FailOnMode,
         ScopeDescription, NamePattern, bScanning, Limit]
        (FJobOnComplete OnComplete)
    {
        FReport Report;
        Run(Page, Offset, Total, Config, Report);

        // Unresolved explicit paths are folded in AFTER the sweep, as unmeasured assets, so
        // they occupy exactly the same buckets a mesh that failed to load would. They are the
        // reason `assets` cannot quietly shrink.
        if (Offset == 0)
        {
            for (const FString& Entry : Unresolved)
            {
                FAssetMeasurement Missing;
                // ASSET_NOT_FOUND, not MESH_AUDIT_UNLOADABLE. The path is well formed and the
                // registry holds no such asset; that is a different fact from a mesh whose LOD
                // would not copy, and only one of the two is about geometry. Still Unrunnable,
                // and correctly so - the asset was not measured - but a caller can now tell
                // "there is nothing here" from "this mesh is broken".
                Missing.UnrunnableCode = ErrorCodes::ERR_ASSET_NOT_FOUND;
                Missing.UnrunnableReason = FString::Printf(
                    TEXT("'%s' is a well-formed asset path, but the asset registry holds no "
                         "StaticMesh there. Nothing about this path's geometry was measured."),
                    *Entry);
                EvaluateAsset(Entry, Entry, Missing, Config, Report);
            }
            Report.bPageTruncated = (Report.AssetsExamined < Total);
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

        // The verdict, and the rule it came from, side by side. A pass rule the caller has to
        // infer from the numbers is a pass rule two readers will infer differently.
        Result->SetBoolField(TEXT("pass"), Report.DerivePass(FailOnMode));
        Result->SetStringField(TEXT("failOn"), FailOn);
        Result->SetStringField(TEXT("passRule"),
            PinWrightAudit::PassRuleText(/*bIncludeTruncation=*/true,
                TEXT("An asset that could not be read is not an asset that passed, and a page "
                     "that stopped short of the match set has not audited the set.")));
        Result->SetStringField(TEXT("scope"), ScopeDescription);
        if (!NamePattern.IsEmpty())
        {
            Result->SetStringField(TEXT("namePattern"), NamePattern);
        }

        TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
        Summary->SetNumberField(TEXT("assetsMatched"), Report.AssetsMatched);
        Summary->SetNumberField(TEXT("assetsExamined"), Report.AssetsExamined);
        Summary->SetNumberField(TEXT("assetsFlagged"), Report.AssetsFlagged);
        Summary->SetNumberField(TEXT("assetsUnmeasured"), Report.AssetsUnmeasured);
        Summary->SetNumberField(TEXT("findings"), Report.Findings.Num());
        Summary->SetNumberField(TEXT("errors"), Report.ErrorCount);
        Summary->SetNumberField(TEXT("warnings"), Report.WarningCount);
        Summary->SetNumberField(TEXT("unrunnable"), Report.UnrunnableCount);
        Summary->SetNumberField(TEXT("totalTriangles"), static_cast<double>(Report.TotalTriangles));
        Summary->SetNumberField(TEXT("totalVertices"), static_cast<double>(Report.TotalVertices));
        Summary->SetBoolField(TEXT("truncated"), Report.IsTruncated());
        Summary->SetBoolField(TEXT("pageTruncated"), Report.bPageTruncated);
        Summary->SetBoolField(TEXT("findingsTruncated"), Report.bFindingsTruncated);
        Summary->SetNumberField(TEXT("findingsDropped"), Report.FindingsDropped);
        Result->SetObjectField(TEXT("summary"), Summary);

        TSharedPtr<FJsonObject> PageObj = MakeShared<FJsonObject>();
        PageObj->SetNumberField(TEXT("offset"), Offset);
        PageObj->SetNumberField(TEXT("limit"), Limit);
        PageObj->SetNumberField(TEXT("total"), Total);
        Result->SetObjectField(TEXT("page"), PageObj);

        TSharedPtr<FJsonObject> ReadObj = MakeShared<FJsonObject>();
        ReadObj->SetStringField(TEXT("lodType"), LodSourceToString(Config.Read.LodSource));
        ReadObj->SetNumberField(TEXT("lodIndex"), Config.Read.LodIndex);
        ReadObj->SetBoolField(TEXT("applyBuildSettings"), Config.Read.bApplyBuildSettings);
        ReadObj->SetBoolField(TEXT("useBuildScale"), Config.Read.bUseBuildScale);
        ReadObj->SetNumberField(TEXT("minVolumeRatio"), Config.Thresholds.MinVolumeRatio);
        ReadObj->SetNumberField(TEXT("floatingToleranceFraction"),
                                Config.Thresholds.FloatingToleranceFraction);
        ReadObj->SetNumberField(TEXT("zFightPlaneExtentFraction"),
                                Config.Thresholds.ZFightPlaneExtentFraction);
        ReadObj->SetNumberField(TEXT("zFightNormalDotThreshold"),
                                Config.Thresholds.ZFightNormalDotThreshold);
        Result->SetObjectField(TEXT("read"), ReadObj);

        // Every check, selected or not, with its four buckets. The two identities
        // (applicable + notApplicable == assetsExamined, and
        //  flagged + unrunnable + clean == applicable) are what make a silent stop
        // detectable: a check that quietly stopped running drops out of both sums.
        TArray<TSharedPtr<FJsonValue>> CheckRows;
        for (const FCheckInfo& Info : AllChecks())
        {
            const FCheckTally& Tally = Report.Tallies[static_cast<int32>(Info.Check)];
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("id"), Info.Id);
            Row->SetStringField(TEXT("code"), Info.Code);
            Row->SetStringField(TEXT("severity"), PinWrightAudit::SeverityToWire(Info.Severity));
            Row->SetBoolField(TEXT("selected"), HasCheck(Config.SelectedChecks, Info.Check));
            Row->SetNumberField(TEXT("applicable"), Tally.Applicable);
            Row->SetNumberField(TEXT("notApplicable"), Tally.NotApplicable);
            Row->SetNumberField(TEXT("flagged"), Tally.Flagged);
            Row->SetNumberField(TEXT("unrunnable"), Tally.Unrunnable);
            Row->SetNumberField(TEXT("clean"), Tally.Clean);
            Row->SetStringField(TEXT("summary"), Info.Summary);
            CheckRows.Add(MakeShared<FJsonValueObject>(Row));
        }
        Result->SetArrayField(TEXT("checks"), CheckRows);

        TArray<TSharedPtr<FJsonValue>> FindingRows;
        for (const FFinding& Finding : Report.Findings)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("assetPath"), Finding.AssetPath);
            Row->SetStringField(TEXT("assetName"), Finding.AssetName);
            Row->SetStringField(TEXT("check"), CheckInfo(Finding.Check).Id);
            Row->SetStringField(TEXT("code"), Finding.Code);
            // status is the load-bearing field: "unrunnable" means this check did NOT evaluate
            // this asset, and reading it as anything other than an unanswered question is the
            // mistake this whole verb is shaped to prevent.
            Row->SetStringField(TEXT("status"), PinWrightAudit::StatusToWire(Finding.Status));
            Row->SetStringField(TEXT("severity"), PinWrightAudit::SeverityToWire(Finding.Severity));
            Row->SetStringField(TEXT("message"), Finding.Message);
            if (Finding.Measurements.IsValid() && Finding.Measurements->Values.Num() > 0)
            {
                Row->SetObjectField(TEXT("measurements"), Finding.Measurements);
            }
            FindingRows.Add(MakeShared<FJsonValueObject>(Row));
        }
        Result->SetArrayField(TEXT("findings"), FindingRows);

        if (Report.CleanAssets.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> CleanRows;
            for (const FString& Path : Report.CleanAssets)
            {
                CleanRows.Add(MakeShared<FJsonValueString>(Path));
            }
            Result->SetArrayField(TEXT("cleanAssets"), CleanRows);
        }

        // What this sweep could NOT see, said out loud rather than left to be discovered.
        if (bScanning)
        {
            Report.Caveats.Add(TEXT(
                "The asset registry was still scanning when the match set was resolved, so assets "
                "it had not indexed yet are absent from assetsMatched entirely - they are not "
                "counted anywhere in this report."));
        }
        if (Report.bPageTruncated)
        {
            Report.Caveats.Add(TEXT(
                "This page stopped short of the match set, so the report describes part of the "
                "folder. pass is false for that reason alone; page through the rest before "
                "concluding anything about the whole set."));
        }
        if (Report.Caveats.Num() > 0)
        {
            TArray<TSharedPtr<FJsonValue>> CaveatRows;
            for (const FString& Caveat : Report.Caveats)
            {
                CaveatRows.Add(MakeShared<FJsonValueString>(Caveat));
            }
            Result->SetArrayField(TEXT("caveats"), CaveatRows);
        }

        // Always a SUCCESS, however bad the content is. No PinWright verb turns a content
        // defect into an RPC error: the defect travels as pass:false plus structured findings,
        // so a caller can tell "your meshes are inverted" from "the call did not work".
        OnComplete(true, Result, FString());
    };

    Ctx.StartJob(Args);
    return true;
}
