// Copyright (c) 2026 Alexander Penkin. MIT License.

// ModelCompileHandler.cpp - the `model` RPC namespace: model.compile, model.validate,
// model.describe_ops.
//
// Marshalling only. FPwModelCompiler owns the pipeline and is deliberately free of JSON
// and FHandlerContext (docs/pwmodel-design.md), so nothing decided here may be something a
// non-RPC caller would also have to decide - otherwise a later UPwModelLibrary Python
// wrapper stops being a shim and becomes a second implementation of the same policy.
//
// model.describe_ops emits the parser's own tables rather than a hand-written list. It is the
// vocabulary an authoring agent reads before writing its first document, and it reads the
// same static tables the parser validates against, so the published vocabulary cannot drift
// from the one that actually compiles. This is why docs/pwmodel-format.md carries no per-op
// parameter reference.
//
// "The whole vocabulary" is the standard it is held to, not "the op table": a parameter the
// parser enforces and this verb omits is a hole nothing else fills, because the docs point
// here instead of listing anything. Hence `ops` (PwModelOpTable::Get) AND `paramSets` (the
// model-level statements and the `part` header), and hence the per-parameter min/max - the
// channel range was checked by the parser and published by nobody.
#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Utils/HttpResponseSpill.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/AssetUtils.h"
#include "Utils/JsonUtils.h"

#include "Utils/MeshRebuildRenderGuard.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"
#include "PwSource/PwSourcePathUtils.h"
#include "PwSource/PwSuggest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Optional.h"
#include "Misc/Paths.h"

namespace
{

// A box as min / max / size / center rather than the origin + extent static_mesh.describe
// publishes for a built asset. Both describe the same box; this is the form an acceptance test
// is written in - `bounds.min.z == -12`, `bounds.size.x <= 910` - and halving an extent by hand
// to get there is where the arithmetic mistake goes. Components are {x, y, z} objects, matching
// every other vector this surface emits.
TSharedPtr<FJsonObject> ModelHandler_EmitVector(const FVector& V)
{
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetNumberField(TEXT("x"), V.X);
    Out->SetNumberField(TEXT("y"), V.Y);
    Out->SetNumberField(TEXT("z"), V.Z);
    return Out;
}

TSharedPtr<FJsonObject> ModelHandler_EmitBounds(const FBox& Box)
{
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetObjectField(TEXT("min"), ModelHandler_EmitVector(Box.Min));
    Out->SetObjectField(TEXT("max"), ModelHandler_EmitVector(Box.Max));
    Out->SetObjectField(TEXT("size"), ModelHandler_EmitVector(Box.GetSize()));
    Out->SetObjectField(TEXT("center"), ModelHandler_EmitVector(Box.GetCenter()));
    return Out;
}

// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace
// helper with a common name would collide with a sibling handler TU once Unity merges them.

bool ModelHandler_HasField(const FHandlerContext& Ctx, const TCHAR* Key)
{
    const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
    return Payload.IsValid() && Payload->HasField(Key);
}

// Resolves a caller-supplied filesystem path. A .pwmodel may live anywhere, so this is
// deliberately NOT the /Game/ asset-path sanitizer. A relative path resolves against the
// project directory rather than the process working directory, which for the editor is the
// engine's Binaries dir - matching the plugin's WikiOutputDirectory / AssetDumpRootDirectory
// idiom, and the only reading of a relative path an author would predict.
FString ModelHandler_ResolveSourcePath(const FString& RawPath)
{
    FString Resolved = RawPath;
    if (FPaths::IsRelative(Resolved))
    {
        Resolved = FPaths::Combine(FPaths::ProjectDir(), Resolved);
    }
    Resolved = FPaths::ConvertRelativePathToFull(Resolved);
    FPaths::NormalizeFilename(Resolved);
    return Resolved;
}

// The value written into the asset's provenance stamp: project-relative when the source lies
// under the project directory, absolute otherwise - the contract UPwModelAssetUserData
// documents (GeometryAssetCreate.h) and the same idiom as WikiOutputDirectory /
// AssetDumpRootDirectory.
//
// It matters because the stamp is compared by plain string equality on the next compile
// (GeometryAssetCreate.cpp:169). An absolute stamp therefore mismatches on every checkout at
// a different root: the second machine's recompile of its OWN source is refused as "different
// source", and the only way past is overwrite=true - the reflex the stamp exists to keep
// meaningful.
//
// Relativized AFTER ModelHandler_ResolveSourcePath's normalization, so two spellings of one
// file still collapse to one stamp. An empty path stays empty: an empty stamp is never
// written and never matches, which is what makes inline text unusable for compile.
FString ModelHandler_ProvenancePath(const FString& ResolvedPath)
{
    if (ResolvedPath.IsEmpty())
    {
        return ResolvedPath;
    }

    const FString ProjectDir = FPaths::ProjectDir();
    if (!FPaths::IsUnderDirectory(ResolvedPath, ProjectDir))
    {
        return ResolvedPath;
    }

    // MakePathRelativeTo fails only for paths with no common root at all (a different drive),
    // which IsUnderDirectory has already excluded. The absolute fallback is kept anyway: it
    // must never return an emptied path, which would match nothing and stamp nothing.
    FString Relative = ResolvedPath;
    return FPaths::MakePathRelativeTo(Relative, *ProjectDir) ? Relative : ResolvedPath;
}

// Reads the source file, sending the error itself on failure. OutResolvedPath is the path
// that goes into the provenance stamp, so it is the resolved one rather than what the
// caller typed - two spellings of one file must not produce two different stamps.
bool ModelHandler_LoadSourceFile(FHandlerContext& Ctx, const FString& RawPath,
                                 FString& OutSource, FString& OutResolvedPath)
{
    OutResolvedPath = ModelHandler_ResolveSourcePath(RawPath);

    if (!FPaths::FileExists(OutResolvedPath))
    {
        Ctx.SendError(ErrorCodes::ERR_MODEL_FILE_NOT_FOUND,
            FString::Printf(TEXT("No .pwmodel source at '%s' (resolved to '%s')."),
                *RawPath, *OutResolvedPath));
        return false;
    }

    if (!FFileHelper::LoadFileToString(OutSource, *OutResolvedPath))
    {
        Ctx.SendError(ErrorCodes::ERR_MODEL_INVALID_SOURCE,
            FString::Printf(TEXT("Could not read '%s' as text. .pwmodel sources are UTF-8 text files."),
                *OutResolvedPath));
        return false;
    }

    return true;
}

const TCHAR* ModelHandler_SeverityToString(EPwSeverity Severity)
{
    return Severity == EPwSeverity::Warning ? TEXT("warning") : TEXT("error");
}

// ---------------------------------------------------------------------------
// Diagnostic reporting shape
// ---------------------------------------------------------------------------
//
// Compiling Examples/pwmodel/crystal_cluster.pwmodel answered 17,815 characters against a
// 10,000-character budget, spilled the response to Saved/PinWright/HttpResponses/ and cost the
// author a file read on EVERY iteration of the authoring loop. The payload was almost entirely
// one warning raised over and over.
//
// The budget is measured against ONE CONDENSED COPY of this object, and that is a recent
// change worth knowing about before touching the numbers below.
// HttpResponseSpill::MeasureReaderFacingCharacters serializes `structuredContent` condensed and
// counts that - the payload a reader actually consumes, once. It used to pretty-print the whole
// MCP tool result, which carries this object TWICE (once as `structuredContent`, once
// JSON-escaped inside `content[0].text`) and counted tab indentation and CRLF line ends at two
// characters each after escaping, for about 3.1x the reader-facing size.
//
// Nothing below changes which diagnostics the compiler PRODUCES - only how many of them this
// verb prints. Every count in `diagnosticSummary` is taken over the FULL array before any
// shaping, so a shaped response can never be mistaken for a clean compile (docs/rpc-design.md
// section 1).

// Worst-case condensed cost of ONE printed diagnostic entry, and of the response body around the
// diagnostic array. Both re-measured after the gate changed, by serializing the exact objects
// this file builds through a condensed writer - the same string the gate counts, so these are
// lengths rather than a factor applied to some other length.
//
//   entry  992 chars: the largest message that REPEATS (PWMODEL_UNUNIONED_OVERLAP, now 487
//          format characters - it was 439 when this was last derived) plus severity, line,
//          column, code, part, occurrences, twelve occurrenceSites, occurrenceSitesOmitted and
//          distinctMessages. Two messages in the vocabulary are longer (~651) and neither
//          repeats: PWMODEL_EXTRUDE_FACING_OPPOSED is per op and PWMODEL_UNUNIONED_OVERLAP_PARTS
//          is once per model.
//   body   ~1,520 chars at one part, growing by ~330 per part - the per-part array carries two
//          counts, three orientation fields and a four-corner bounds box each. Six slots, health,
//          model bounds, the counts, provenance and diagnosticSummary make up the rest.
constexpr int32 ModelHandler_WorstCaseEntryCharacters = 992;
constexpr int32 ModelHandler_ResponseBodyCharacters = 1520;
constexpr int32 ModelHandler_ResponseBodyCharactersPerPart = 330;

// The part count the PUBLISHED default is sized to hold - the figure model.describe_ops and the
// parameter documentation quote. The value actually used is fitted to the real body instead; see
// ModelHandler_FitDiagnosticLimit.
constexpr int32 ModelHandler_BudgetPartCount = 8;

// What ModelHandler_SendOutcome appends after ModelHandler_ResultToJson has measured the body -
// `sourcePath`, the optional save report - plus `diagnosticSummary`, which AddDiagnostics writes
// after the fit has been computed. Reserved rather than measured because none of it exists yet
// at the moment the fit is taken.
constexpr int32 ModelHandler_OutcomeTailCharacters = 600;

// The PUBLISHED default: what the parameter documentation and model.describe_ops quote, derived
// from the gate's own threshold rather than from a second copy of it, so a changed threshold
// moves the published figure instead of silently invalidating it.
//
// DO NOT SCALE THIS BY THE GATE'S IMPROVEMENT. The gate got ~3x cheaper when it stopped counting
// the payload twice pretty-printed, and the payload grew by about as much over the same period -
// the repeating message went 439 -> 487 characters, and the response gained materialSlotList,
// unboundSlots, bounds, health and per-part orientation - so the two nearly cancel and the
// answer is still five. Re-measured condensed: (10,000 - 3,830) / 992 = 6.2 worst-case entries at
// eight parts, 8.5 at one part, 3.6 at sixteen.
int32 ModelHandler_DefaultDiagnosticLimit()
{
    const int32 Budget = HttpResponseSpill::GetDefaultThresholdCharacters();
    const int32 Body = ModelHandler_ResponseBodyCharacters
        + ModelHandler_ResponseBodyCharactersPerPart * ModelHandler_BudgetPartCount;

    // Never zero: zero is the documented sentinel for "print everything", so an arithmetic result
    // of zero would turn the tightest budget into the loudest response. One entry is the floor.
    return FMath::Clamp((Budget - Body) / ModelHandler_WorstCaseEntryCharacters, 1, 64);
}

// THE LIMIT ACTUALLY USED, fitted to the body this response really built.
//
// A fixed default cannot be right, and the reason is measurable: the body is dominated by the
// per-part array, which costs ~330 characters a part, so the room left for diagnostics runs from
// ~8,500 characters on a one-part model to ~3,500 on a sixteen-part one. A single number sized
// for the middle of that range overflows the top of it - and the top of the range is exactly
// where the diagnostics are, because a model with enough parts to raise dozens of overlap
// warnings is a model with dozens of parts. Four separate callers reported the same symptom:
// a document with ~45 legitimate overlap warnings spilled its whole response at the default,
// which read as "the default is not applied" and was in fact the default being too generous for
// that body.
//
// So the body is measured after it is built and the array is sized against what is left. Called
// from the end of ModelHandler_ResultToJson, which is the one point where the body is complete
// and the diagnostics are not yet in it.
//
// CONDENSED, because that is what the gate counts: HttpResponseSpill measures one condensed copy
// of `structuredContent`. This is the one place that rule is restated rather than called, since
// the gate exports only its threshold and its decision, not its measurement - so if the two ever
// diverge, they diverge in the safe direction only while the gate counts no MORE than this does.
// Exporting the measurement would remove the restatement; that is the follow-up.
//
// An explicit `diagnosticLimit` is never fitted. A caller who names a number gets that number,
// including 0 for "everything", and the spill path is what handles the consequences - the whole
// point of the sentinel is that the full list stays reachable.
int32 ModelHandler_FitDiagnosticLimit(const TSharedPtr<FJsonObject>& Body)
{
    if (!Body.IsValid())
    {
        return ModelHandler_DefaultDiagnosticLimit();
    }

    FString Condensed;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Condensed);
    FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
    Writer->Close();

    const int32 Budget = HttpResponseSpill::GetDefaultThresholdCharacters();
    const int32 Remaining = Budget - Condensed.Len() - ModelHandler_OutcomeTailCharacters;

    // One entry minimum, for the same reason as above: 0 means "print everything". A body that
    // has already eaten the whole budget still gets its first diagnostic printed and then spills,
    // which is strictly better than spilling with no diagnostic visible at all.
    return FMath::Clamp(Remaining / ModelHandler_WorstCaseEntryCharacters, 1, 64);
}

// Bounded per-group site list. Twelve line/column pairs cost ~350 characters here, under a
// third of one full entry, and they are the part an author navigates by; the repeated message
// body is boilerplate.
constexpr int32 ModelHandler_MaxOccurrenceSites = 12;

// How much of the diagnostic array to print. Read identically by model.compile and
// model.validate: a caller that iterates with validate and then compiles must not have to learn
// a second spelling of the same three questions.
struct FModelHandler_DiagnosticView
{
    // 0 = every entry. The documented sentinel, and the reason the spill path stays reachable
    // for a caller that genuinely wants the whole list.
    int32 Limit = ModelHandler_DefaultDiagnosticLimit();

    // The caller named no number, so the limit is fitted to the response body rather than taken
    // from the published default. False the moment `diagnosticLimit` appears on the payload -
    // including when it appears as the published default value, because a caller who sends a
    // number is asking for that number.
    bool bFitLimitToBudget = true;
    bool bCollapse = true;
    // Unset = every severity.
    TOptional<EPwSeverity> Severity;
    // Echoed in the response so the shaping is self-describing.
    FString SeverityFilter = TEXT("all");
};

// Reads the three shaping parameters, sending the error itself on a bad value.
bool ModelHandler_ReadDiagnosticView(FHandlerContext& Ctx, FModelHandler_DiagnosticView& OutView)
{
    OutView.bFitLimitToBudget = !ModelHandler_HasField(Ctx, TEXT("diagnosticLimit"));
    OutView.Limit = Ctx.GetInt(TEXT("diagnosticLimit"), ModelHandler_DefaultDiagnosticLimit());
    if (OutView.Limit < 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("'diagnosticLimit' must be 0 or greater; got %d. 0 means every diagnostic, which is "
                 "what keeps the full list reachable when a response has to spill to a file."),
            OutView.Limit));
        return false;
    }

    OutView.bCollapse = Ctx.GetBool(TEXT("collapseDiagnostics"), true);

    const FString Requested = Ctx.GetString(TEXT("diagnosticSeverity"), TEXT("all"));
    if (Requested.Equals(TEXT("all"), ESearchCase::IgnoreCase))
    {
        OutView.Severity.Reset();
    }
    else if (Requested.Equals(TEXT("error"), ESearchCase::IgnoreCase))
    {
        OutView.Severity = EPwSeverity::Error;
    }
    else if (Requested.Equals(TEXT("warning"), ESearchCase::IgnoreCase))
    {
        OutView.Severity = EPwSeverity::Warning;
    }
    else
    {
        // Errors rather than degrading to "all": a filter that quietly widened on an unrecognised
        // spelling would answer a question the caller did not ask (docs/rpc-design.md section 3).
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS, FString::Printf(
            TEXT("'diagnosticSeverity' must be 'all', 'error' or 'warning'; got '%s'. Those are the two "
                 "severities the .pwmodel compiler produces, plus 'all' for both."),
            *Requested));
        return false;
    }

    // One spelling in the response whatever the caller typed.
    OutView.SeverityFilter = Requested.ToLower();
    return true;
}

// One emitted entry: the first diagnostic of its group plus whatever the rest of the group adds.
struct FModelHandler_DiagnosticGroup
{
    const FPwDiagnostic* First = nullptr;
    int32 Count = 0;
    TArray<const FPwDiagnostic*> Sites;
    int32 SitesOmitted = 0;
    // Distinct message texts in the group, so a collapsed entry can say that its single printed
    // message did not cover them all.
    TSet<FString> Messages;
};

// Grouping key: severity + code + part. NOT the message. The repeated PWMODEL_UNUNIONED_OVERLAP
// messages all DIFFER - each names the op it overlapped, that op's line, and the overlap extents
// - so a message-keyed group would collapse nothing at all, which is the entire defect. The
// fields that differ are kept structurally instead: every occurrence's line and column, bounded,
// plus `distinctMessages` when the texts were not identical.
FString ModelHandler_DiagnosticGroupKey(const FPwDiagnostic& Diagnostic)
{
    return FString::Printf(TEXT("%s|%s|%s"),
        ModelHandler_SeverityToString(Diagnostic.Severity), *Diagnostic.Code, *Diagnostic.ScopeName);
}

// Writes `diagnostics` and `diagnosticSummary` into Result.
void ModelHandler_AddDiagnostics(const TSharedPtr<FJsonObject>& Result,
                                 const TArray<FPwDiagnostic>& Diagnostics,
                                 const FModelHandler_DiagnosticView& View)
{
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (Diagnostic.Severity == EPwSeverity::Error)
        {
            ++ErrorCount;
        }
        else
        {
            ++WarningCount;
        }
    }

    // 1. Severity filter.
    TArray<const FPwDiagnostic*> Kept;
    Kept.Reserve(Diagnostics.Num());
    for (const FPwDiagnostic& Diagnostic : Diagnostics)
    {
        if (!View.Severity.IsSet() || Diagnostic.Severity == View.Severity.GetValue())
        {
            Kept.Add(&Diagnostic);
        }
    }
    const int32 SuppressedBySeverity = Diagnostics.Num() - Kept.Num();

    // 2. Collapse. With collapsing off every diagnostic becomes its own single-occurrence group,
    //    so everything downstream is one code path rather than two that can drift.
    TArray<FModelHandler_DiagnosticGroup> Groups;
    TMap<FString, int32> GroupIndexByKey;
    Groups.Reserve(Kept.Num());
    for (const FPwDiagnostic* Diagnostic : Kept)
    {
        int32 GroupIndex = INDEX_NONE;
        if (View.bCollapse)
        {
            const FString Key = ModelHandler_DiagnosticGroupKey(*Diagnostic);
            if (const int32* Existing = GroupIndexByKey.Find(Key))
            {
                GroupIndex = *Existing;
            }
            else
            {
                GroupIndex = Groups.AddDefaulted();
                GroupIndexByKey.Add(Key, GroupIndex);
            }
        }
        else
        {
            GroupIndex = Groups.AddDefaulted();
        }

        FModelHandler_DiagnosticGroup& Group = Groups[GroupIndex];
        if (Group.Count == 0)
        {
            Group.First = Diagnostic;
        }
        ++Group.Count;
        Group.Messages.Add(Diagnostic->Message);
        if (Group.Sites.Num() < ModelHandler_MaxOccurrenceSites)
        {
            Group.Sites.Add(Diagnostic);
        }
        else
        {
            ++Group.SitesOmitted;
        }
    }

    // 3. Limit. Errors are selected before warnings so a limit that bites can never drop an error
    //    in favour of a warning - the one truncation that would actively mislead. Within a
    //    severity the FIRST group of each distinct CODE is taken before any second group of a code
    //    already shown, so a limit of N reports N different problems rather than N copies of the
    //    loudest one. The selection is then re-sorted, so the ARRAY stays in source order and its
    //    ordering does not change with the limit.
    //
    //    The code pass is not a nicety. Diagnostics reach this function in emission order, and the
    //    material-binding warning - "slot 'X' is bound to '...', which could not be loaded; using
    //    the default material", the compile's ONLY report that a slot shipped unbound - is raised
    //    during asset creation, which is the last stage there is. It is therefore the LAST group
    //    in source order and can be the first thing a small limit drops. A model built from
    //    interpenetrating primitives generates many overlap groups by construction,
    //    so this was the normal case for the workload, not an edge of it.
    const int32 Limit = View.Limit <= 0 ? Groups.Num() : FMath::Min(View.Limit, Groups.Num());
    TArray<int32> Selected;
    Selected.Reserve(Limit);
    TArray<bool> Taken;
    Taken.Init(false, Groups.Num());
    TSet<FString> SeenCodes;
    for (int32 Pass = 0; Pass < 4 && Selected.Num() < Limit; ++Pass)
    {
        const EPwSeverity Wanted = (Pass < 2) ? EPwSeverity::Error : EPwSeverity::Warning;
        const bool bNewCodesOnly = (Pass % 2) == 0;
        for (int32 Index = 0; Index < Groups.Num() && Selected.Num() < Limit; ++Index)
        {
            if (Taken[Index] || Groups[Index].First->Severity != Wanted)
            {
                continue;
            }
            if (bNewCodesOnly && SeenCodes.Contains(Groups[Index].First->Code))
            {
                continue;
            }
            Selected.Add(Index);
            Taken[Index] = true;
            SeenCodes.Add(Groups[Index].First->Code);
        }
    }
    Selected.Sort();

    // 4. Emit.
    int32 Represented = 0;
    TArray<TSharedPtr<FJsonValue>> Values;
    Values.Reserve(Selected.Num());
    for (const int32 GroupIndex : Selected)
    {
        const FModelHandler_DiagnosticGroup& Group = Groups[GroupIndex];
        Represented += Group.Count;

        const FPwDiagnostic& Diagnostic = *Group.First;
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("severity"), ModelHandler_SeverityToString(Diagnostic.Severity));
        Entry->SetNumberField(TEXT("line"), Diagnostic.Line);
        Entry->SetNumberField(TEXT("column"), Diagnostic.Column);
        Entry->SetStringField(TEXT("code"), Diagnostic.Code);
        Entry->SetStringField(TEXT("message"), Diagnostic.Message);
        Entry->SetStringField(TEXT("part"), Diagnostic.ScopeName);
        Entry->SetArrayField(TEXT("suggestions"), EmitStringArray(Diagnostic.Suggestions));

        // Emitted only for a group that actually collapsed, so an entry that represents exactly
        // one diagnostic is byte-identical to what this verb answered before the shaping existed.
        if (Group.Count > 1)
        {
            Entry->SetNumberField(TEXT("occurrences"), Group.Count);

            TArray<TSharedPtr<FJsonValue>> Sites;
            Sites.Reserve(Group.Sites.Num());
            for (const FPwDiagnostic* Site : Group.Sites)
            {
                TSharedPtr<FJsonObject> SiteJson = MakeShared<FJsonObject>();
                SiteJson->SetNumberField(TEXT("line"), Site->Line);
                SiteJson->SetNumberField(TEXT("column"), Site->Column);
                Sites.Add(MakeShared<FJsonValueObject>(SiteJson));
            }
            Entry->SetArrayField(TEXT("occurrenceSites"), Sites);
            if (Group.SitesOmitted > 0)
            {
                Entry->SetNumberField(TEXT("occurrenceSitesOmitted"), Group.SitesOmitted);
            }

            // `message` is the FIRST occurrence's. When the group's texts were not identical this
            // says so, rather than leaving a caller to assume one message covered all of them -
            // each overlap warning names a different second op. collapseDiagnostics=false prints
            // every one.
            if (Group.Messages.Num() > 1)
            {
                Entry->SetNumberField(TEXT("distinctMessages"), Group.Messages.Num());
            }
        }

        Values.Add(MakeShared<FJsonValueObject>(Entry));
    }

    Result->SetArrayField(TEXT("diagnostics"), Values);

    // Emitted on EVERY response, shaped or not. A block that appeared only when something was
    // hidden would have to be known about to be missed, which is the same failure as hiding the
    // diagnostics: `total` is the compiler's own count and the two suppression fields plus
    // `collapsedOccurrences` account for every diagnostic the array does not show individually.
    //
    //   total == represented + suppressedBySeverity + suppressedByLimit
    //   represented == emitted + collapsedOccurrences
    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("total"), Diagnostics.Num());
    Summary->SetNumberField(TEXT("errors"), ErrorCount);
    Summary->SetNumberField(TEXT("warnings"), WarningCount);
    Summary->SetNumberField(TEXT("emitted"), Values.Num());
    Summary->SetNumberField(TEXT("represented"), Represented);
    Summary->SetNumberField(TEXT("collapsedOccurrences"), Represented - Values.Num());
    Summary->SetNumberField(TEXT("suppressedBySeverity"), SuppressedBySeverity);
    Summary->SetNumberField(TEXT("suppressedByLimit"), Kept.Num() - Represented);
    // One boolean a caller can gate on instead of re-deriving the arithmetic: true only when
    // every diagnostic is present as its own entry.
    Summary->SetBoolField(TEXT("complete"), Values.Num() == Diagnostics.Num());
    Summary->SetNumberField(TEXT("limit"), View.Limit);
    Summary->SetBoolField(TEXT("collapse"), View.bCollapse);
    Summary->SetStringField(TEXT("severityFilter"), View.SeverityFilter);
    Result->SetObjectField(TEXT("diagnosticSummary"), Summary);
}

// One response shape for both compile and validate: a caller that iterates with
// model.validate and then compiles reads the same fields out of both.
TSharedPtr<FJsonObject> ModelHandler_ResultToJson(const FPwModelCompileResult& CompileResult,
                                                  const FModelHandler_DiagnosticView& View)
{
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("version"), CompileResult.Version);
    Result->SetBoolField(TEXT("success"), CompileResult.bSuccess);
    Result->SetStringField(TEXT("assetPath"), CompileResult.AssetPath);

    // OMITTED, not defaulted, when the run could not determine the class.
    //
    // One declaration decides it - `use skeleton from` - and a parse that failed before reaching
    // one has not seen it. These two fields used to be written unconditionally off a bool that
    // defaults to false, so every failed parse answered `assetClass: "UStaticMesh", skeletal:
    // false`, INCLUDING sources that plainly declare a skeleton: the parse recovers, so the
    // declaration is usually right there in the document that was just called static.
    //
    // A caller cannot distinguish a wrong answer from a right one, so the fields go away
    // instead. Same rule the asset counts already follow a few lines below: absent means not
    // measured, and publishing a default in its place is the failure, not the fix.
    if (CompileResult.bAssetClassKnown)
    {
        Result->SetStringField(TEXT("assetClass"),
            CompileResult.bSkeletal ? TEXT("USkeletalMesh") : TEXT("UStaticMesh"));
        Result->SetBoolField(TEXT("skeletal"), CompileResult.bSkeletal);
    }

    if (CompileResult.bSkeletal)
    {
        Result->SetStringField(TEXT("skeletonPath"), CompileResult.SkeletonPath);
        Result->SetStringField(TEXT("skeletonPackage"), CompileResult.SkeletonPackageName);
        Result->SetBoolField(TEXT("skeletonSavedToDisk"), CompileResult.bSkeletonSavedToDisk);
        Result->SetStringField(TEXT("skeletonSaveState"),
            AssetSaveStateToWire(CompileResult.SkeletonSaveState));
        Result->SetArrayField(TEXT("clearedFeatures"),
            EmitStringArray(CompileResult.ClearedFeatures));
    }

    // meshTriangleCount / meshVertexCount, not triangleCount / vertexCount: BOTH are the
    // UDynamicMesh's counts, taken before the UStaticMesh bake, and the bake moves them in
    // OPPOSITE directions. It drops every degenerate triangle (bRemoveDegenerates, default
    // true), so the asset has FEWER triangles. It splits vertices at every normal/tangent/UV/color
    // seam, so the asset has MORE vertices. Both directions, and the measured numbers, are in
    // docs/pwmodel-format.md, section "Four counts: two for the mesh, two for the asset" - cited
    // rather than restated, because the per-example pairs that used to sit here were one of five
    // hand-copies and every one of them went stale. Under the old names an author who added
    // split_normals saw a byte-identical response and read the op as a no-op, and an author
    // sizing a mesh against a budget was reading a number the asset does not have.
    // FPwModelCompileResult carries the measurements.
    Result->SetNumberField(TEXT("meshTriangleCount"), CompileResult.MeshTriangleCount);
    Result->SetNumberField(TEXT("meshVertexCount"), CompileResult.MeshVertexCount);

    // Boolean repair telemetry is present only when a boolean reached its post-operation cleanup
    // window. It is deliberately separate from meshTriangleCount: the latter is the final merged
    // mesh count, while these fields explain the most recent boolean's weld/delete pass.
    if (CompileResult.TrianglesBefore >= 0)
    {
        Result->SetNumberField(TEXT("trianglesBefore"), CompileResult.TrianglesBefore);
        Result->SetNumberField(TEXT("trianglesAfter"), CompileResult.TrianglesAfter);
        Result->SetNumberField(TEXT("sliversRemoved"), CompileResult.SliversRemoved);
    }

    // The asset's own LOD0 counts, read back off the built render data. Emitted only on a run
    // that wrote an asset: model.validate creates nothing to measure, and 0 would read as an
    // empty mesh rather than as "not measured". A caller gating on size reads these.
    if (CompileResult.AssetTriangleCount >= 0)
    {
        Result->SetNumberField(TEXT("assetTriangleCount"), CompileResult.AssetTriangleCount);
    }
    if (CompileResult.AssetVertexCount >= 0)
    {
        Result->SetNumberField(TEXT("assetVertexCount"), CompileResult.AssetVertexCount);
    }

    // The merged mesh's health, from the same walk geometry.check_health reports. Emitted as a
    // nested object rather than four loose fields because they are one measurement, and only on
    // a run that reached stage 4 - a document that failed to parse has no mesh to measure, and
    // 0 boundary edges would read as "closed" rather than as "not measured".
    //
    // The compiler also raises PWMODEL_MESH_NOT_CLOSED / PWMODEL_DEGENERATE_GEOMETRY as
    // warnings for these. Both channels exist on purpose: the diagnostic is what an author
    // reads, and this object is what a caller GATES on - refusing an open mesh means testing
    // health.boundaryEdges, not string-matching a message.
    if (CompileResult.MeshBoundaryEdges >= 0)
    {
        TSharedPtr<FJsonObject> Health = MakeShared<FJsonObject>();
        Health->SetBoolField(TEXT("isClosed"), CompileResult.MeshBoundaryEdges == 0);
        Health->SetNumberField(TEXT("boundaryEdges"), CompileResult.MeshBoundaryEdges);
        Health->SetNumberField(TEXT("degenerateTriangles"), CompileResult.MeshDegenerateTriangles);
        Health->SetNumberField(TEXT("nonManifoldVertices"), CompileResult.MeshNonManifoldVertices);
        Health->SetNumberField(TEXT("componentCount"), CompileResult.MeshComponentCount);

        // ORPHANS, which is the field that makes the response internally consistent rather than
        // one that adds a new defect class. `meshVertexCount` above is the vertex buffer, while
        // `bounds` is reduced over TRIANGLES so a vertex no triangle names cannot widen the box -
        // so the two describe the same mesh and disagree about what is in it, correctly, and with
        // nothing naming the difference until this number. Non-zero also means an op removed
        // geometry and left the leftovers behind, which is worth seeing on its own.
        //
        // Not part of any verdict here or in the compiler: an orphan is inert against the bake,
        // which is driven by triangles. It is reported, and the caller decides.
        Health->SetNumberField(TEXT("unreferencedVertices"),
            CompileResult.MeshUnreferencedVertices);

        // WINDING, which none of the five fields above can express. A closed mesh wound inside
        // out matches a correct one on every one of them and renders identically - backface
        // culling shows the camera whichever wall faces it - which is how an inside-out shell
        // shipped in an example and survived two investigations, one of which quoted an A/B
        // luminance difference of 0.000004 as proof it was fine.
        //
        // `signedVolume` is only meaningful when `isClosed`; on an open mesh it is the integral
        // of an unclosed surface. It is emitted anyway, next to the `isClosed` that qualifies
        // it, because a field that VANISHES on the open case makes `health.signedVolume < 0`
        // read as false for "no signal" as well as for "correct" - the same cannot-fail gate
        // this whole block exists to remove. The gate is `isClosed && signedVolume > 0`.
        //
        // `orientationConsistent` is independent, not a summary: a UNIFORM inversion leaves
        // every pair of neighbours agreeing, so it is true on exactly the shell signedVolume
        // catches, and false on a partly inverted surface signedVolume cannot see.
        Health->SetNumberField(TEXT("signedVolume"), CompileResult.MeshSignedVolume);
        Health->SetBoolField(TEXT("orientationConsistent"), CompileResult.MeshInconsistentEdges == 0);
        Health->SetNumberField(TEXT("inconsistentEdges"), CompileResult.MeshInconsistentEdges);

        // EMBEDDING, which none of the fields above can express and which defeats the gate they
        // document. A membrane spanning a bore is two oppositely wound fans: their contributions
        // to `signedVolume` cancel EXACTLY, so the number is the one the intended solid would
        // have had, while `isClosed` is true and every other field is byte-identical to a correct
        // mesh. A sweep whose walls were pushed through each other moves `signedVolume` smoothly
        // instead, through 62% and 25% of its analytic volume, and flips sign only long after it
        // stopped being a solid. The gate is `isClosed && signedVolume > 0 &&
        // selfIntersections === 0`.
        //
        // ABSENT, never zeroed, when the measurement was declined - it is skipped above a
        // triangle budget, and a `0` there would read as "no crossings" rather than as "not
        // measured", which is the same cannot-fail gate this block exists to remove. An absent
        // field fails `=== 0` in every caller, which is the correct way round.
        if (CompileResult.MeshSelfIntersections >= 0)
        {
            Health->SetNumberField(TEXT("selfIntersections"),
                CompileResult.MeshSelfIntersections);
            Health->SetNumberField(TEXT("selfIntersectingComponents"),
                CompileResult.MeshSelfIntersectingComponents);
            Health->SetBoolField(TEXT("selfIntersectionsTruncated"),
                CompileResult.bMeshSelfIntersectionTruncated);
        }
        Result->SetObjectField(TEXT("health"), Health);
    }

    // The merged mesh's extent, in mesh space. Emitted from model.validate as well as
    // model.compile, because it is measured on the mesh rather than on the asset and a
    // validate-only run has the same mesh. That is the point of the field: it is what lets an
    // author fit a model to a required bounding box on the surface that CREATES NOTHING,
    // instead of writing an asset per iteration just to call static_mesh.describe on it.
    //
    // Absent, never zeroed, on a run that produced no geometry - an all-zero box would read as a
    // model collapsed to a point rather than as "not measured", the same distinction the -1
    // sentinels above carry.
    if (CompileResult.MeshBounds.IsValid != 0)
    {
        Result->SetObjectField(TEXT("bounds"), ModelHandler_EmitBounds(CompileResult.MeshBounds));
    }

    if (CompileResult.FloatingGeometry.bMeasured)
    {
        const MeshAudit::FFloatingReport& Floating = CompileResult.FloatingGeometry;
        TSharedPtr<FJsonObject> FloatingObject = MakeShared<FJsonObject>();
        FloatingObject->SetNumberField(TEXT("componentCount"), Floating.ComponentCount);
        FloatingObject->SetNumberField(TEXT("islandCount"), Floating.IslandCount);
        FloatingObject->SetNumberField(TEXT("floatingCount"), Floating.FloatingCount);
        FloatingObject->SetNumberField(TEXT("suppressedCount"), Floating.SuppressedCount);
        FloatingObject->SetNumberField(TEXT("unsuppressedCount"), Floating.UnsuppressedCount);
        FloatingObject->SetNumberField(TEXT("largestComponentIndex"),
                                        Floating.LargestComponentIndex);
        FloatingObject->SetNumberField(TEXT("largestIslandTriangleCount"),
                                       Floating.LargestIslandTriangleCount);
        FloatingObject->SetNumberField(TEXT("toleranceFraction"), Floating.ToleranceFraction);
        FloatingObject->SetNumberField(TEXT("boundingSphereRadius"),
                                       Floating.BoundingSphereRadius);
        FloatingObject->SetNumberField(TEXT("tolerance"), Floating.Tolerance);

        TArray<TSharedPtr<FJsonValue>> FloatingRows;
        for (const MeshAudit::FFloatingComponent& Component : Floating.Components)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetNumberField(TEXT("componentIndex"), Component.ComponentIndex);
            Row->SetNumberField(TEXT("triangleCount"), Component.TriangleCount);
            Row->SetNumberField(TEXT("signedVolume"), Component.SignedVolume);
            Row->SetNumberField(TEXT("nearestComponentIndex"),
                                Component.NearestComponentIndex);
            Row->SetNumberField(TEXT("nearestDistance"), Component.NearestDistance);
            Row->SetBoolField(TEXT("suppressed"), Component.bSuppressed);
            TArray<TSharedPtr<FJsonValue>> PartIndices;
            TArray<TSharedPtr<FJsonValue>> PartNames;
            for (const int32 PartIndex : Component.OwningPartIndices)
            {
                PartIndices.Add(MakeShared<FJsonValueNumber>(PartIndex));
                if (CompileResult.Parts.IsValidIndex(PartIndex))
                {
                    PartNames.Add(MakeShared<FJsonValueString>(
                        CompileResult.Parts[PartIndex].PartName));
                }
            }
            Row->SetArrayField(TEXT("partIndices"), PartIndices);
            Row->SetArrayField(TEXT("partNames"), PartNames);
            TSharedPtr<FJsonObject> Center = MakeShared<FJsonObject>();
            Center->SetNumberField(TEXT("x"), Component.Center.X);
            Center->SetNumberField(TEXT("y"), Component.Center.Y);
            Center->SetNumberField(TEXT("z"), Component.Center.Z);
            Row->SetObjectField(TEXT("center"), Center);
            FloatingRows.Add(MakeShared<FJsonValueObject>(Row));
        }
        FloatingObject->SetArrayField(TEXT("components"), FloatingRows);
        Result->SetObjectField(TEXT("floatingGeometry"), FloatingObject);
    }

    Result->SetNumberField(TEXT("materialSlots"), CompileResult.MaterialSlots);

    // The slot table itself, in the order the created asset's sections take. `materialSlots` is
    // this array's length; it is kept because a count is what a budget check wants, and dropping
    // it would break every caller reading it.
    //
    // A COUNT CANNOT SEE THE FAILURE THIS EXISTS FOR. Slots are allocated in model-wide
    // first-use order (the implicit `Default` excepted - it is always last), an existing asset
    // is REBUILT IN PLACE so its referencers survive, and
    // every referencer addresses sections by index. Move one part above another and each index
    // now names a different section - on a skeletal rebuild the engine empties the slot list
    // first, so nothing even reports a change - while `materialSlots: 6` reads identically
    // before and after. Emitted from model.validate too, which is the only surface that can
    // answer the question before the overwrite rather than after it.
    TArray<TSharedPtr<FJsonValue>> SlotEntries;
    SlotEntries.Reserve(CompileResult.MaterialSlotList.Num());
    for (int32 SlotIndex = 0; SlotIndex < CompileResult.MaterialSlotList.Num(); ++SlotIndex)
    {
        const FPwModelSlotReport& Slot = CompileResult.MaterialSlotList[SlotIndex];
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("index"), SlotIndex);
        Entry->SetStringField(TEXT("name"), Slot.Name);
        Entry->SetStringField(TEXT("material"), Slot.BoundAssetPath);
        SlotEntries.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Result->SetArrayField(TEXT("materialSlotList"), SlotEntries);

    // Slots the creator left on the DEFAULT surface material - unbound, or bound to a path that
    // would not load. Both asset creators have always computed this and the model pipeline threw
    // it away, so the one report that a section shipped grey never reached the response: a
    // caller saw `success: true` and a slot count that looked exactly right.
    //
    // Emitted only on a run that created an asset. model.validate runs no creator, so an empty
    // array there would assert that every slot is bound - the same false confidence this field
    // exists to remove. Absent means not measured. Validate answers the same question with
    // diagnostics instead (PWMODEL_UNBOUND_MATERIAL, and the unloadable-binding warning).
    if (!CompileResult.AssetPath.IsEmpty())
    {
        Result->SetArrayField(TEXT("unboundSlots"), EmitStringArray(CompileResult.UnboundSlots));
    }

    Result->SetNumberField(TEXT("collisionElements"), CompileResult.CollisionElements);
    Result->SetBoolField(TEXT("savedToDisk"), CompileResult.bSavedToDisk);

    if (CompileResult.SkinVertexCount >= 0)
    {
        TSharedPtr<FJsonObject> Skin = MakeShared<FJsonObject>();
        Skin->SetBoolField(TEXT("hasBoneWeights"), CompileResult.bHasSkinWeights);
        Skin->SetNumberField(TEXT("vertices"), CompileResult.SkinVertexCount);
        Skin->SetNumberField(TEXT("verticesWeighted"), CompileResult.SkinWeightedVertexCount);
        Skin->SetNumberField(TEXT("verticesUnweighted"), CompileResult.SkinUnweightedVertexCount);
        Skin->SetBoolField(TEXT("fullyWeighted"), CompileResult.bFullyWeighted);
        Result->SetObjectField(TEXT("skin"), Skin);
    }

    // Informational per-part counts. Parts are sub-regions of the one output mesh, never
    // assets of their own, which is why there is exactly one assetPath above - and why there
    // is no per-part asset count: the parts are merged before the bake, so the asset cannot
    // say which triangle came from which part. Both fields here are mesh counts.
    TArray<TSharedPtr<FJsonValue>> PartValues;
    PartValues.Reserve(CompileResult.Parts.Num());
    for (const FPwModelPartInfo& Part : CompileResult.Parts)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("partName"), Part.PartName);
        Entry->SetBoolField(TEXT("allowFloating"), Part.bAllowFloating);
        Entry->SetNumberField(TEXT("meshTriangleCount"), Part.MeshTriangleCount);
        Entry->SetNumberField(TEXT("meshVertexCount"), Part.MeshVertexCount);

        // Winding, per part, because the model-wide `health.signedVolume` above AVERAGES an
        // inverted part away: a correct 20-cube (+8000) beside an inverted 10-cube (-1000)
        // sums to a healthy-looking +7000. One inverted part inside an otherwise correct model
        // is the realistic case, and this is the only field that names which part. Same
        // qualification as the model-wide pair: `signedVolume` means something only when this
        // part's `isClosed` is true.
        Entry->SetBoolField(TEXT("isClosed"), Part.bMeshIsClosed);
        Entry->SetBoolField(TEXT("orientationConsistent"), Part.bMeshOrientationConsistent);
        Entry->SetNumberField(TEXT("signedVolume"), Part.MeshSignedVolume);

        // Per-part box, in the same mesh space as the model-wide one. The model-wide box says
        // that SOMETHING reaches an extreme; this names which part does, which is the question
        // an author who has to shrink a model into a fixed box actually has, and the only
        // alternative is re-deriving it by hand from the source coordinates - which is exactly
        // what a noise_deform or harmonic_deform makes impossible.
        if (Part.MeshBounds.IsValid != 0)
        {
            Entry->SetObjectField(TEXT("bounds"), ModelHandler_EmitBounds(Part.MeshBounds));
        }

        PartValues.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Result->SetArrayField(TEXT("parts"), PartValues);

    // LAST, and the ordering is load-bearing: everything above is the body, so this is the only
    // point at which the body's real size is known and the diagnostic array is not yet in it.
    // The summary AddDiagnostics writes echoes the EFFECTIVE limit, so a fitted response says
    // what it did rather than reporting the published default it did not use.
    FModelHandler_DiagnosticView Fitted = View;
    if (Fitted.bFitLimitToBudget)
    {
        Fitted.Limit = ModelHandler_FitDiagnosticLimit(Result);
    }
    ModelHandler_AddDiagnostics(Result, CompileResult.Diagnostics, Fitted);
    return Result;
}

// Which half of the pipeline failed. Re-parsing costs one text pass and only runs on the
// failure path; matching diagnostic codes against a hand-kept parse-stage list would instead
// misattribute every code either stage gains later, silently.
const TCHAR* ModelHandler_FailureCode(FStringView Source)
{
    FPwModelDocument Document;
    TArray<FPwDiagnostic> ParseDiagnostics;
    return FPwModelParser::Parse(Source, Document, ParseDiagnostics)
        ? ErrorCodes::ERR_MODEL_COMPILE_FAILED
        : ErrorCodes::ERR_MODEL_PARSE_FAILED;
}

// The first error, plus a count of the rest. The full list rides along in the result object
// as structuredContent, so the message is a headline rather than a dump.
FString ModelHandler_FailureMessage(const FPwModelCompileResult& CompileResult)
{
    TArray<FString> Errors;
    for (const FPwDiagnostic& Diagnostic : CompileResult.Diagnostics)
    {
        if (Diagnostic.Severity == EPwSeverity::Error)
        {
            Errors.Add(Diagnostic.ToString());
        }
    }

    if (Errors.Num() == 0)
    {
        return TEXT("Compilation failed without reporting a diagnostic.");
    }
    if (Errors.Num() == 1)
    {
        return Errors[0];
    }
    return FString::Printf(TEXT("%s (and %d more)"), *Errors[0], Errors.Num() - 1);
}

// The whole outcome tail, shared by model.compile and model.validate. ModelHandler_ResultToJson
// claims one response shape for both verbs; with the sequence written out twice that was a
// promise rather than a property, and the two copies had already drifted over the save report.
//
// SaveRequested is unset for model.validate, which never saves. On the failure path the save
// report is not emitted at all: a failed compile created no asset, and AddAssetSaveReport would
// answer pendingFlush:true - documented as "save was asked for but no .uasset reached disk, an
// editor.save_all / asset.save is needed" (AssetUtils.cpp:624) - telling the caller to flush
// something that never existed.
template <typename TResponder>
void ModelHandler_SendOutcome(TResponder& Ctx, FStringView Source,
                              const FPwModelCompileResult& CompileResult,
                              const FString& ResolvedPath,
                              const FModelHandler_DiagnosticView& View,
                              const TOptional<bool>& SaveRequested = TOptional<bool>())
{
    TSharedPtr<FJsonObject> Result = ModelHandler_ResultToJson(CompileResult, View);
    // The resolved source file, not the spelling the caller typed. The provenance stamp is
    // derived from it (ModelHandler_ProvenancePath), so a caller cannot reason about whether a
    // later recompile will need overwrite without knowing which file was read.
    Result->SetStringField(TEXT("sourcePath"), ResolvedPath);

    if (!CompileResult.bSuccess)
    {
        Ctx.SendError(ModelHandler_FailureCode(Source),
            ModelHandler_FailureMessage(CompileResult), Result);
        return;
    }

    if (SaveRequested.IsSet())
    {
        // The state rides along so a saved:false is actionable: `saveState` says whether a
        // flush fixes it and `saveDetail` names the verb. Only on this branch - a failed
        // compile still emits no save report at all (see the note above).
        AddAssetSaveReport(Result, SaveRequested.GetValue(), CompileResult.bSavedToDisk,
                           CompileResult.SaveState);
    }

    Ctx.SendSuccess(Result);
}

TSharedPtr<FJsonObject> ModelHandler_ParamSpecToJson(const FPwModelParamSpec& Spec)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("name"), Spec.Name);
    Json->SetStringField(TEXT("type"), PwModelParamTypeToString(Spec.Type));
    Json->SetBoolField(TEXT("required"), Spec.bRequired);
    Json->SetStringField(TEXT("default"), Spec.Default);
    Json->SetStringField(TEXT("description"), Spec.Description);

    Json->SetArrayField(TEXT("allowedValues"), EmitStringArray(Spec.AllowedValues));

    // The CLAMPED domain, which is NOT the enforced `min` / `max` pair below and is emitted under
    // its own names for exactly that reason. A value outside this one is accepted, moved to the
    // nearest legal value, and warned about; a value outside `min` / `max` is refused. A caller
    // that treats these as a gate will reject counts the op happily takes - including the
    // documented `<= 0 means unset` tier, which is outside the clamped domain by construction and
    // is what `clampUnsetDefault` describes.
    //
    // Absent, rather than 0/0, on a parameter that clamps nothing: a published 0-0 would read as
    // a domain of exactly zero. Same reason the enforced pair is conditional.
    if (Spec.bHasClampDomain)
    {
        Json->SetNumberField(TEXT("clampMin"), Spec.ClampMin);
        Json->SetNumberField(TEXT("clampMax"), Spec.ClampMax);

        // Only where the floor depends on the sweep, which is two parameters in the whole
        // vocabulary. `clampMin` is already the floor at the op's own default angle, so a caller
        // that ignores this field gets the right number for the line it is most likely to write;
        // one that closes the sweep needs the other.
        if (Spec.ClampMinClosedSweep != Spec.ClampMin)
        {
            Json->SetNumberField(TEXT("clampMinClosedSweep"), Spec.ClampMinClosedSweep);
        }

        if (Spec.bClampZeroMeansUnset)
        {
            Json->SetNumberField(TEXT("clampUnsetDefault"), Spec.ClampUnsetDefault);
        }
    }

    // The parser's inclusive numeric domain, emitted only when the spec carries one - the
    // pair is absent rather than 0/0 for an unbounded parameter, because a published 0..0
    // would read as a domain of exactly zero. Without this the parser rejected `channel=8`
    // against a bound the vocabulary surface never mentioned.
    if (Spec.bHasRange)
    {
        Json->SetNumberField(TEXT("min"), Spec.MinValue);
        Json->SetNumberField(TEXT("max"), Spec.MaxValue);
    }

    return Json;
}

TSharedPtr<FJsonObject> ModelHandler_OpSpecToJson(const FPwModelOpSpec& Spec)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("name"), Spec.Name);
    Json->SetStringField(TEXT("context"), PwModelOpContextToString(Spec.Context));
    Json->SetStringField(TEXT("description"), Spec.Description);
    Json->SetBoolField(TEXT("generator"), Spec.bGenerator);
    Json->SetBoolField(TEXT("acceptsBlock"), Spec.bAcceptsBlock);
    Json->SetBoolField(TEXT("acceptsMaterial"), Spec.bAcceptsMaterial);
    Json->SetBoolField(TEXT("boolean"), Spec.bBoolean);

    // What `from=`/`to=` do to this op besides placing and turning it. Published as a machine
    // -readable pair rather than left in the `from` parameter's prose, because the one thing a
    // caller has to decide before writing the line is whether it may also write the extent
    // parameter - and `aimExtentParam` names exactly which parameter that is. 'none' means the
    // op has no extent along its local Z and the endpoint distance goes unused.
    Json->SetStringField(TEXT("aimExtent"), PwModelAimExtentToString(Spec.AimExtent));
    Json->SetStringField(TEXT("aimExtentParam"), Spec.AimExtentParam);

    TArray<TSharedPtr<FJsonValue>> Params;
    Params.Reserve(Spec.Params.Num());
    for (const FPwModelParamSpec& Param : Spec.Params)
    {
        Params.Add(MakeShared<FJsonValueObject>(ModelHandler_ParamSpecToJson(Param)));
    }
    Json->SetArrayField(TEXT("params"), Params);

    return Json;
}

// The zero-argument discovery call is intentionally an index rather than a second copy of every
// description and parameter. The full table is useful only after an author has chosen a name, and
// returning it here made the documented first call spill to disk as soon as the vocabulary grew.
// Keep one row per parser spec so a name legal in two contexts still advertises both contexts.
TSharedPtr<FJsonObject> ModelHandler_OpSpecToIndexJson(const FPwModelOpSpec& Spec)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("name"), Spec.Name);
    Json->SetStringField(TEXT("context"), PwModelOpContextToString(Spec.Context));
    Json->SetNumberField(TEXT("parameterCount"), Spec.Params.Num());
    return Json;
}

// The model-level/header `key=value` lists that are not ops. They are published as their own
// top-level block rather than as `ops` entries because none is legal where an op goes:
// `uv_layout` and `lightmap` are model-level statements and
// bone= / at / rotate / scale live on the `part` header line, before its brace.
//
// That distinction is load-bearing rather than cosmetic. Every consumer of `ops` builds a
// document by dropping the entry into a part or a collision block - the vocabulary round-trip
// test in TestModelHandlers.cpp most of all, which is what proves the published surface is the
// one that compiles. Folding these into `ops` would make that assumption false for their entries
// and force the check to be relaxed for them; a separate block leaves it intact and
// gets an equivalent round-trip check of its own (DescribeOpsParamSetsParse).
//
// `context` therefore says where the parameters are written, in the same slot where an op says
// where the op is legal: `model` for a model-level statement, `part_header` for the header line.
struct FModelHandler_ParamSetSpec
{
    const TCHAR* Name;
    const TCHAR* Context;
    const TCHAR* Description;
    TArrayView<const FPwModelParamSpec> (*Params)();
};

TArray<FModelHandler_ParamSetSpec> ModelHandler_ParamSets()
{
    return {
        { TEXT("uv_layout"), TEXT("model"),
          TEXT("Pack one UV channel after all parts have merged. Uses world-space texel density "
               "and may be written once per channel."),
          &PwModelOpTable::UVLayoutParams },
        { TEXT("lightmap"), TEXT("model"),
          TEXT("The model-level 'lightmap' statement, written outside every part. At most one per "
               "document: a UStaticMesh has a single LightMapCoordinateIndex."),
          &PwModelOpTable::LightmapParams },
        { TEXT("part"), TEXT("part_header"),
          TEXT("The 'part <name>' header line, before its '{'. bone= names the reference-skeleton "
               "binding; at / rotate / scale map part-local space into mesh space and are applied "
               "once after the part's ops have run - unlike the same-named parameters on a generator, "
               "which are part-local and compose into that one op."),
          &PwModelOpTable::PartHeaderParams },
    };
}

TSharedPtr<FJsonObject> ModelHandler_ParamSetToJson(const FModelHandler_ParamSetSpec& Set)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("name"), Set.Name);
    Json->SetStringField(TEXT("context"), Set.Context);
    Json->SetStringField(TEXT("description"), Set.Description);

    TArray<TSharedPtr<FJsonValue>> Params;
    for (const FPwModelParamSpec& Param : Set.Params())
    {
        Params.Add(MakeShared<FJsonValueObject>(ModelHandler_ParamSpecToJson(Param)));
    }
    Json->SetArrayField(TEXT("params"), Params);

    return Json;
}

TSharedPtr<FJsonObject> ModelHandler_ParamSetToIndexJson(const FModelHandler_ParamSetSpec& Set)
{
    TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
    Json->SetStringField(TEXT("name"), Set.Name);
    Json->SetStringField(TEXT("context"), Set.Context);
    Json->SetNumberField(TEXT("parameterCount"), Set.Params().Num());
    return Json;
}
}

// ============================================================================
// model.compile
// ============================================================================
REGISTER_RPC_HANDLER("model.compile", "model",
    "Compile a .pwmodel source file into exactly one UStaticMesh or USkeletalMesh asset.",
    RPC_PARAMS(
        RPC_PARAM_REQ("filePath", "filepath",
            "Filesystem path to the .pwmodel source; a relative path resolves against the project directory. "
            "Inline text is deliberately refused here - an asset compiled from an RPC payload has no recoverable "
            "source, which is the exact failure this format exists to prevent. Iterate with model.validate, then "
            "write the file."),
        RPC_PARAM_OPT("outputPath", "path",
            "Destination /Game/... asset path. Omit it only when filePath is a .pwmodel directly below the "
            "project Content directory; the target then uses the same relative path and basename under /Game. "
            "Sources elsewhere require this explicit argument."),
        RPC_PARAM_DEF("overwrite", "boolean",
            "PERMISSION to take over an asset this source did not generate: one carrying no provenance stamp, "
            "one stamped with a different source, or live state named by PWSRC_RECOMPILE_UNMANAGED_STATE. "
            "It does not change HOW the write happens - "
             "an existing asset of the same class is always REBUILT IN PLACE, so everything referencing it (placed actors, other assets) "
            "keeps resolving to it, with or without the flag. Being referenced is therefore never a reason a "
            "compile is refused - the one exception is a LIVE component whose scene proxy caches the asset's "
            "render data and cannot be quiesced across the rebuild, which is refused by name rather than "
            "rebuilt underneath. Clean same-source iteration does not need the flag; a path holding a different asset CLASS is refused whatever the flag says, because "
             "a StaticMesh cannot be rebuilt as a SkeletalMesh or vice versa.", "false"),
        RPC_PARAM_DEF("save", "boolean",
            "Save the created asset to disk. With save:true the compile is refused UP FRONT while a Play-In-Editor "
            "session is running - PIE_ACTIVE with saveState:blockedByPie, nothing built and nothing written - because "
            "the editor refuses every single-asset write while one is up, and rebuilding the mesh anyway would leave "
            "the loaded asset ahead of its .uasset with no verb that reconciles them. Retry when the session ends, or "
            "pass save:false to build in memory only.", "true"),
        // Declared only so it can be refused with an explanation. The dispatcher rejects any
        // undeclared field before a handler runs (RpcDispatcher.cpp ValidateHandlerParams),
        // and a bare "unknown parameter" leaves a caller with nowhere to go - whereas the
        // reason inline text is refused here is the whole point of the format.
        RPC_PARAM_OPT("text", "string",
            "NOT ACCEPTED on model.compile - supplying it is an error. An asset compiled from an RPC payload has "
            "no recoverable source. Use model.validate to iterate on a string, then write the file."),
        RPC_PARAM_DEF("diagnosticLimit", "integer",
            "Maximum number of diagnostic ENTRIES the response prints; 0 means every entry. The default is sized "
            "to keep a diagnostic-heavy compile inside the inline response budget rather than spilling it to a "
            "file the caller then has to open. Omit it and the limit is FITTED to the response actually built - the "
            "body grows with the part count, so a many-part model prints fewer entries and a one-part model more; "
            "'diagnosticSummary.limit' echoes the number used. Naming a value disables the fit and that value is "
            "honoured exactly. Errors are kept before warnings when the limit bites, and "
            "'diagnosticSummary' still reports every diagnostic the compile produced.", "5"),
        RPC_PARAM_DEF("diagnosticSeverity", "string",
            "Print only diagnostics of one severity: 'error', 'warning', or 'all'. Those are the two severities "
            "the compiler produces. Filtered-out diagnostics are still counted in 'diagnosticSummary', so this "
            "cannot turn a failing compile into a clean-looking one.", "all"),
        RPC_PARAM_DEF("collapseDiagnostics", "boolean",
            "Fold repeated diagnostics sharing a severity, code and part into one entry carrying 'occurrences' "
            "and a bounded 'occurrenceSites' list of their line/column pairs. On by default: one authoring "
            "mistake routinely raises the same warning at thirty sites, and thirty near-identical 450-character "
            "messages is what used to push this response out of the inline budget. Set false to read every "
            "message in full.", "true")
    ))
{
    if (ModelHandler_HasField(Ctx, TEXT("text")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("model.compile does not accept inline 'text'. An asset compiled from an RPC payload has no "
                 "recoverable source, and every inline compile would stamp an empty SourcePath that matches every "
                 "other one, defeating the provenance stamp. Use model.validate to iterate on a string, then write "
                 "the document to a .pwmodel file and pass filePath."));
        return true;
    }

    FString FilePath;
    if (!Ctx.RequireString(TEXT("filePath"), FilePath))
    {
        return true;
    }

    const FString ResolvedForOutput = ModelHandler_ResolveSourcePath(FilePath);
    FString OutputPath;
    if (ModelHandler_HasField(Ctx, TEXT("outputPath")))
    {
        if (!Ctx.RequireAssetPath(TEXT("outputPath"), OutputPath))
        {
            return true;
        }
    }
    else
    {
        FString Reason;
        if (!PinWrightPwSourcePaths::TryDeriveOutputAssetPath(
                ResolvedForOutput, TEXT(".pwmodel"), OutputPath, Reason))
        {
            Ctx.SendError(ErrorCodes::ERR_SOURCE_OUTPUT_PATH_NOT_DERIVABLE,
                FString::Printf(
                    TEXT("Cannot derive 'outputPath' from source '%s': %s. Pass explicit "
                         "outputPath='/Game/.../AssetName'. No default path was chosen."),
                    *ResolvedForOutput, *Reason));
            return true;
        }
    }

    // Read before the compile runs: a bad shaping parameter is the caller's mistake, and
    // discovering it after building a mesh and writing an asset would report it over work that
    // should not have started.
    FModelHandler_DiagnosticView View;
    if (!ModelHandler_ReadDiagnosticView(Ctx, View))
    {
        return true;
    }

    FString Source;
    FString ResolvedPath;
    if (!ModelHandler_LoadSourceFile(Ctx, FilePath, Source, ResolvedPath))
    {
        return true;
    }

    FPwModelCompileOptions Options;
    Options.OutputAssetPath = OutputPath;
    Options.SourcePath = ModelHandler_ProvenancePath(ResolvedPath);
    Options.bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);
    Options.bSave = Ctx.GetBool(TEXT("save"), true);
    Options.bValidateOnly = false;

    // Refused HERE, ahead of the build, rather than at the save that follows it. A compile
    // rebuilds the target UStaticMesh in place and leaves its package dirty, so a write refused
    // afterwards leaves the loaded asset ahead of its .uasset in an editor other agents share,
    // with no verb that reconciles the two - and the next save-all from any stream then persists
    // a revision nobody reviewed. Same all-or-nothing shape as the render-consumer guard below.
    // A session that starts after this point still meets the refusal at the save chokepoint.
    if (Options.bSave)
    {
        TSharedPtr<FJsonObject> Refusal = MakeShared<FJsonObject>();
        FString PieDescription;
        if (AddPieSaveRefusalReport(Refusal, PieDescription))
        {
            Refusal->SetStringField(TEXT("assetPath"), OutputPath);
            Refusal->SetStringField(TEXT("sourcePath"), ResolvedPath);
            Ctx.SendError(ErrorCodes::ERR_PIE_ACTIVE, FString::Printf(
                TEXT("Refusing to compile '%s' onto '%s': %s. Nothing was built and nothing was "
                     "written, so memory and disk still agree. Retry once the play session ends "
                     "(editor.pie_status polls it), or pass save:false to build in memory only."),
                *ResolvedPath, *OutputPath, *PieDescription), Refusal);
            return true;
        }
    }

    TArray<FString> RebuildPaths;
    RebuildPaths.Add(OutputPath);
    return PinWrightMeshRebuild::RunGuardedStaticMeshRebuild(Ctx, TEXT("model.compile"),
        RebuildPaths,
        [Source = MoveTemp(Source), Options, ResolvedPath, View](
            const PinWrightSafePoint::FSafePointResponder& Responder,
            const TArray<UStaticMesh*>& Meshes)
        {
            FPwModelCompileResult CompileResult =
                FPwModelCompiler::Compile(FStringView(Source), Options);
            ModelHandler_SendOutcome(Responder, FStringView(Source), CompileResult,
                ResolvedPath, View, Options.bSave);
        });
}

// ============================================================================
// model.validate
// ============================================================================
REGISTER_RPC_HANDLER("model.validate", "model",
    "Parse and compile a .pwmodel document without writing an asset. The surface for iterating on a document "
    "before it exists as a file.",
    RPC_PARAMS(
        RPC_PARAM_OPT("text", "string", "Inline .pwmodel source. Supply exactly one of text or filePath."),
        RPC_PARAM_OPT("filePath", "filepath",
            "Filesystem path to a .pwmodel source; a relative path resolves against the project directory. "
            "Supply exactly one of text or filePath."),
        // The same three, spelled the same way, for the same reason ModelHandler_ResultToJson is
        // shared: the iterate-then-compile loop must not change shape halfway through.
        RPC_PARAM_DEF("diagnosticLimit", "integer",
            "Maximum number of diagnostic ENTRIES the response prints; 0 means every entry. The default is sized "
            "to keep a diagnostic-heavy document inside the inline response budget rather than spilling it to a "
            "file the caller then has to open. Omit it and the limit is FITTED to the response actually built - the "
            "body grows with the part count, so a many-part model prints fewer entries and a one-part model more; "
            "'diagnosticSummary.limit' echoes the number used. Naming a value disables the fit and that value is "
            "honoured exactly. Errors are kept before warnings when the limit bites, and "
            "'diagnosticSummary' still reports every diagnostic the run produced.", "5"),
        RPC_PARAM_DEF("diagnosticSeverity", "string",
            "Print only diagnostics of one severity: 'error', 'warning', or 'all'. Those are the two severities "
            "the compiler produces. Filtered-out diagnostics are still counted in 'diagnosticSummary', so this "
            "cannot turn a failing document into a clean-looking one.", "all"),
        RPC_PARAM_DEF("collapseDiagnostics", "boolean",
            "Fold repeated diagnostics sharing a severity, code and part into one entry carrying 'occurrences' "
            "and a bounded 'occurrenceSites' list of their line/column pairs. On by default: one authoring "
            "mistake routinely raises the same warning at thirty sites. Set false to read every message in full.",
            "true")
    ))
{
    const bool bHasText = ModelHandler_HasField(Ctx, TEXT("text"));
    const bool bHasFilePath = ModelHandler_HasField(Ctx, TEXT("filePath"));

    if (bHasText == bHasFilePath)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            bHasText
                ? TEXT("model.validate takes exactly one of 'text' or 'filePath'; both were supplied.")
                : TEXT("model.validate takes exactly one of 'text' or 'filePath'; neither was supplied."));
        return true;
    }

    FModelHandler_DiagnosticView View;
    if (!ModelHandler_ReadDiagnosticView(Ctx, View))
    {
        return true;
    }

    FString Source;
    FString ResolvedPath;
    if (bHasFilePath)
    {
        FString FilePath;
        if (!Ctx.RequireString(TEXT("filePath"), FilePath))
        {
            return true;
        }
        if (!ModelHandler_LoadSourceFile(Ctx, FilePath, Source, ResolvedPath))
        {
            return true;
        }
    }
    else
    {
        Source = Ctx.GetString(TEXT("text"));
        if (Source.TrimStartAndEnd().IsEmpty())
        {
            Ctx.SendError(ErrorCodes::ERR_MODEL_INVALID_SOURCE,
                TEXT("'text' is empty. A .pwmodel document starts with the version header 'pwmodel 0'."));
            return true;
        }
    }

    FPwModelCompileOptions Options;
    // Never stamped (bValidateOnly creates no asset), but SourcePath means one thing across
    // both verbs: the value a stamp would carry.
    Options.SourcePath = ModelHandler_ProvenancePath(ResolvedPath);
    Options.bOverwrite = false;
    Options.bSave = false;
    Options.bValidateOnly = true;

    const FPwModelCompileResult CompileResult = FPwModelCompiler::Compile(FStringView(Source), Options);

    // No save report: validate saves nothing, so there is no save to report on.
    ModelHandler_SendOutcome(Ctx, FStringView(Source), CompileResult, ResolvedPath, View);
    return true;
}

// ============================================================================
// model.describe_ops
// ============================================================================
REGISTER_RPC_HANDLER("model.describe_ops", "model",
    "Index the .pwmodel vocabulary, or return complete metadata for one op or parameter set. The zero-argument "
    "response is deliberately compact so it stays inline; query its names with 'op' for parameters with types, "
    "defaults, allowed values and numeric ranges. The parameter sets that are not ops are model-level "
    "'uv_layout' and 'lightmap' statements plus the 'part' header's bone= / at / rotate / scale. All data comes from the parser's own tables, "
    "so it cannot drift from what actually compiles.",
    RPC_PARAMS(
        RPC_PARAM_OPT("op", "string",
            "Narrow to one name and return its complete metadata. Omitted, a compact index is returned. An op legal "
            "in both contexts (box, sphere, capsule) returns one 'ops' entry per context, because its parameters differ "
            "between them; 'uv_layout', 'lightmap' and 'part' narrow to the matching 'paramSets' entry instead.")
    ))
{
    const FString Requested = Ctx.GetString(TEXT("op"));
    const bool bIndex = Requested.IsEmpty();

    TArray<TSharedPtr<FJsonValue>> OpValues;
    for (const FPwModelOpSpec& Spec : PwModelOpTable::Get())
    {
        if (!Requested.IsEmpty() && Spec.Name != Requested)
        {
            continue;
        }
        OpValues.Add(MakeShared<FJsonValueObject>(bIndex
            ? ModelHandler_OpSpecToIndexJson(Spec)
            : ModelHandler_OpSpecToJson(Spec)));
    }

    // Narrowed by the same name, so `op=lightmap` answers rather than reporting an unknown op
    // while the very same response body would have carried the entry.
    TArray<TSharedPtr<FJsonValue>> ParamSetValues;
    for (const FModelHandler_ParamSetSpec& Set : ModelHandler_ParamSets())
    {
        if (!Requested.IsEmpty() && Requested != Set.Name)
        {
            continue;
        }
        ParamSetValues.Add(MakeShared<FJsonValueObject>(bIndex
            ? ModelHandler_ParamSetToIndexJson(Set)
            : ModelHandler_ParamSetToJson(Set)));
    }

    if (OpValues.Num() == 0 && ParamSetValues.Num() == 0)
    {
        TArray<FString> AllNames = PwModelOpTable::NamesInContext(EPwModelOpContext::Part);
        for (const FString& Name : PwModelOpTable::NamesInContext(EPwModelOpContext::Collision))
        {
            AllNames.AddUnique(Name);
        }
        for (const FString& Name : PwModelOpTable::NamesInContext(EPwModelOpContext::Skin))
        {
            AllNames.AddUnique(Name);
        }
        // The non-op names are candidates too: `op=lightmpa` must reach `lightmap` rather
        // than the nearest op, which is the whole point of narrowing by one name.
        for (const FModelHandler_ParamSetSpec& Set : ModelHandler_ParamSets())
        {
            AllNames.AddUnique(Set.Name);
        }

        const FString Guess = PwSuggest::Closest(Requested, AllNames);
        Ctx.SendError(ErrorCodes::ERR_UNKNOWN_OPERATION,
            Guess.IsEmpty()
                ? FString::Printf(TEXT("No .pwmodel op named '%s'. Call model.describe_ops with no 'op' for the compact vocabulary index."), *Requested)
                : FString::Printf(TEXT("No .pwmodel op named '%s'. Did you mean '%s'?"), *Requested, *Guess));
        return true;
    }

    // Both arrays are always present, empty included: a caller iterating the response reads one
    // shape whether or not it narrowed, and `opCount` keeps counting ops only. `index` tells the
    // caller whether entries are compact names/counts or complete parser metadata.
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("index"), bIndex);
    Result->SetNumberField(TEXT("opCount"), OpValues.Num());
    Result->SetArrayField(TEXT("ops"), OpValues);
    Result->SetNumberField(TEXT("paramSetCount"), ParamSetValues.Num());
    Result->SetArrayField(TEXT("paramSets"), ParamSetValues);
    Ctx.SendSuccess(Result);
    return true;
}
