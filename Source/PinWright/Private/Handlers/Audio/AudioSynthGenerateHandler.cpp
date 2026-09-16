// Copyright (c) 2026 Alexander Penkin. MIT License.

// AudioSynthGenerateHandler.cpp - audio.synth.generate / patch / variations / export
//
// The four verbs an agent actually calls to make a sound. Everything else in the namespace
// describes (describe_schema), inspects (list_candidates), plays (audition) or frees
// (discard) what these four produce.
//
// -------------------------------------------------------------------------------------------
// WHY generate AND patch RUN INLINE AND variations RUNS AS A JOB (rpc-design.md §9)
// -------------------------------------------------------------------------------------------
// generate and patch each perform exactly ONE render, bounded by the recipe schema's own caps
// (PwSynthLimits::MaxDurationMs, MaxSampleRate, MaxLayers). variations performs `count` of
// them, and `count` is the caller's number - which is what makes it the one verb here that can
// genuinely run past the transport's response timeout. So variations goes through Ctx.StartJob;
// the other two answer directly, which also keeps their failures as real error CODES rather
// than as a job's bare error string (§7).
//
// StartJob is NOT a deferral primitive - FHandlerContext::StartJob invokes the bind delegate
// synchronously - so variations still renders on the caller's stack. What the ticket buys is
// exactly one thing, and it is the thing §9 is about: the RESPONSE is sent before the work
// starts, so a non-streaming client gets its ticket in milliseconds instead of holding a
// connection open for N renders, and the terminal result lands in the ticket either way.
//
// What it deliberately does NOT buy, stated so nobody adds a claim later: no per-variation
// progress events (StartJob returns the ticket id only AFTER the bind delegate has already run
// to completion, so there is no id to record progress against from inside it), and no
// cancellation. Nothing here registers a cancel callback because nothing can stop a render
// mid-flight, so FJobRegistry::Cancel answers Unsupported (JOB_CANCEL_UNSUPPORTED) rather than
// claiming a stop that did not happen.
//
// -------------------------------------------------------------------------------------------
// IDEMPOTENCE (rpc-design.md §8)
// -------------------------------------------------------------------------------------------
// The transport's timeout is response-only: when it fires the handler keeps running and its
// work commits, so a client retry must CONVERGE rather than accumulate. Every verb here is
// therefore keyed on content, not on call count:
//   - generate / patch / variations render, then look for a resident candidate whose canonical
//     recipe is byte-identical and reuse its id instead of adding a second copy. A render is
//     deterministic in the recipe and the seed, so the reused candidate holds exactly the
//     samples this call produced - `reused` is measured from that lookup, never assumed.
//   - plot files are named <candidateId>_<view>.png, so a retry rewrites the same file, and
//     FPwCandidateRegistry::AddImagePath is AddUnique.
//   - export goes through AssetCreatePolicy::Resolve + the engine writer's in-place NewObject,
//     so a retry rewrites the same USoundWave rather than making a second one.
//
// -------------------------------------------------------------------------------------------
// RESPONSE SIZE
// -------------------------------------------------------------------------------------------
// This handler assembles the complete functional result. The shared HttpResponseSpill transport
// measures one condensed reader-facing copy and spills an oversized tool result to its response
// file/reference shape; no handler-local wrapper estimate or response budget belongs here. The
// summary analysis, compact per-layer report and image paths are response-shape choices, while
// variations returns a metric table rather than N full render reports. Audio never travels
// inline in either direction: it leaves as a USoundWave asset path plus duration / rate / channels.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamAliasUtils.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "PinWrightHelpers.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/PathUtils.h"

#include "AudioGen/PwAudioAnalysis.h"
#include "AudioGen/PwAudioBuffer.h"
#include "AudioGen/PwAudioDecode.h"
#include "AudioGen/PwAudioExport.h"
#include "AudioGen/PwAudioPlot.h"
#include "AudioGen/PwCandidateRegistry.h"
#include "AudioGen/PwStft.h"
#include "AudioGen/PwSynthDsp.h"
#include "AudioGen/PwSynthRecipe.h"
#include "State/PluginState.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Math/RandomStream.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundWave.h"

// Named (not anonymous) namespace: the main module builds with Unity on, and two anonymous
// namespaces merged into one TU collide by name. See CLAUDE.md > Building.
namespace PwSynthGenerateInternal
{
    // ---------------------------------------------------------------------------------------
    // Caps
    // ---------------------------------------------------------------------------------------

    // Ceiling on audio.synth.variations' `count`. Each variation is a full render plus a full
    // analysis; eight bounds one request's work and the number of rows in its result table.
    constexpr int32 MaxVariations = 8;

    // Ceiling on audio.synth.variations' `mutations`. Each target adds a `values` column to
    // every row, so this bounds the table's width the way MaxVariations bounds its height.
    constexpr int32 MaxMutations = 8;

    // Ceiling on an RFC-6902 patch document. A recipe is a bounded structure (8 layers, 4
    // effects each, 6 master effects), so a patch longer than this is a caller error rather
    // than a large edit.
    constexpr int32 MaxPatchOps = 64;

    // Longest per-row error text carried in the variations table. A render failure's message
    // names a field path and a measured quantity; 160 characters keeps that intact while
    // stopping eight failures from spilling the table.
    constexpr int32 MaxRowErrorChars = 160;

    // ---------------------------------------------------------------------------------------
    // Param specs
    // ---------------------------------------------------------------------------------------

    // Accepted wire spellings of the candidate-identity slot, canonical first. The bodies of
    // audio.synth.patch / variations / export already read the value with this list through
    // GetStringFirstOf, so the spec must declare all three or the dispatcher's unknown-param
    // gate refuses the alternates before the body runs. `id` is not decorative: the
    // variations table hands each row back under `id`, so a caller feeding a row straight
    // into export arrives with that spelling.
    const TArray<FString>& CandidateIdKeys()
    {
        static const TArray<FString> Keys = {
            TEXT("candidateId"),
            TEXT("candidate_id"),
            TEXT("id")
        };
        return Keys;
    }

    FParamSpec CandidateIdParamReq(const TCHAR* Desc)
    {
        return ParamAliasUtils::MakeAliasParamSpec(*CandidateIdKeys()[0], TEXT("string"), Desc,
            /*bRequired=*/true, CandidateIdKeys());
    }

    // ---------------------------------------------------------------------------------------
    // Small JSON helpers
    // ---------------------------------------------------------------------------------------

    /**
     * Emits a rounded number, or NOTHING when the value is not finite.
     *
     * Rounding is not cosmetic: UE's JSON writer prints doubles with "%.17g", so an unrounded
     * peak costs twenty characters of a response budget measured in thousands, and publishing
     * sixteen digits of a two-digit measurement is its own small lie. Omission on a non-finite
     * value follows the analysis serializer's rule - an absent field is explicit, a fabricated
     * 0.0 reads as a measurement.
     */
    void SetRounded(const TSharedPtr<FJsonObject>& Out, const TCHAR* Key, double Value, int32 Decimals)
    {
        if (!FMath::IsFinite(Value))
        {
            return;
        }
        const double Scale = FMath::Pow(10.0, static_cast<double>(Decimals));
        Out->SetNumberField(Key, FMath::RoundToDouble(Value * Scale) / Scale);
    }

    /** Condensed encoding, used only for exact document comparison - never for a response. */
    FString ToCondensedJson(const TSharedPtr<FJsonObject>& In)
    {
        FString Text;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
        if (In.IsValid())
        {
            FJsonSerializer::Serialize(In.ToSharedRef(), Writer);
        }
        return Text;
    }

    /** The canonical form of a recipe: every default made explicit, every key in spec order. */
    FString CanonicalRecipeJson(const FPwSynthRecipe& Recipe)
    {
        return ToCondensedJson(SerializeSynthRecipe(Recipe));
    }

    /**
     * The three registry numbers a render verb owes its caller, under FPwCandidateRegistry's own
     * field spellings.
     *
     * Deliberately a PROJECTION of UsageToJson rather than the whole thing: generate is what
     * causes eviction, so the caller needs to see pressure coming, but the full nine-field block
     * costs ~300 characters of a response already carrying an analysis and three image paths.
     * The session totals and the caps are one audio.synth.list_candidates away and do not change
     * between calls; these three do.
     */
    void AddRegistryPressure(const TSharedPtr<FJsonObject>& Result, const FPwCandidateUsage& Usage)
    {
        TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        // Key spellings are FPwCandidateRegistry::UsageToJson's, character for character, so a
        // caller reading `registry.count` here and from list_candidates reads the same field.
        Block->SetNumberField(TEXT("count"), Usage.NumCandidates);
        Block->SetNumberField(TEXT("approxBytes"), static_cast<double>(Usage.ApproxBytes));
        Block->SetNumberField(TEXT("bytesRemaining"), static_cast<double>(Usage.BytesRemaining));
        // Only when true: an over-budget registry is a fact the caller must act on, and false is
        // the state every other response already implies.
        if (Usage.bOverBudget)
        {
            Block->SetBoolField(TEXT("overBudget"), true);
        }
        Result->SetObjectField(TEXT("registry"), Block);
    }

    FString Truncate(const FString& In, int32 MaxChars)
    {
        return In.Len() <= MaxChars ? In : (In.Left(MaxChars - 3) + TEXT("..."));
    }

    // ---------------------------------------------------------------------------------------
    // RFC-6901 JSON Pointer + RFC-6902 JSON Patch
    // ---------------------------------------------------------------------------------------
    //
    // Written here because the plugin has no JSON Patch anywhere else. All six operations are
    // implemented: a subset that silently accepted `move` as a no-op, or rejected it as
    // "unknown", would be answering a narrower question than the caller asked (§1).
    //
    // The application is FUNCTIONAL - every step rebuilds the containers along the pointer path
    // and shares every untouched subtree - so the source document is never mutated. That is
    // what makes `patch` structurally incapable of touching the original candidate's recipe
    // (§2): there is no code path that writes through the input document.

    enum class EPatchOp : uint8
    {
        Add,
        Remove,
        Replace,
        Move,
        Copy,
        Test
    };

    bool PatchOpFromString(const FString& In, EPatchOp& Out)
    {
        if (In.Equals(TEXT("add"), ESearchCase::CaseSensitive))     { Out = EPatchOp::Add;     return true; }
        if (In.Equals(TEXT("remove"), ESearchCase::CaseSensitive))  { Out = EPatchOp::Remove;  return true; }
        if (In.Equals(TEXT("replace"), ESearchCase::CaseSensitive)) { Out = EPatchOp::Replace; return true; }
        if (In.Equals(TEXT("move"), ESearchCase::CaseSensitive))    { Out = EPatchOp::Move;    return true; }
        if (In.Equals(TEXT("copy"), ESearchCase::CaseSensitive))    { Out = EPatchOp::Copy;    return true; }
        if (In.Equals(TEXT("test"), ESearchCase::CaseSensitive))    { Out = EPatchOp::Test;    return true; }
        return false;
    }

    /**
     * Splits an RFC-6901 pointer into its unescaped reference tokens.
     *
     * "" is the whole document (zero tokens); "/" is one empty-string token, which is a legal
     * object key. Escapes are undone in RFC order - ~1 then ~0 - so a literal "~1" in a key
     * survives the round trip.
     */
    bool SplitJsonPointer(const FString& Pointer, TArray<FString>& OutTokens, FString& OutReason)
    {
        OutTokens.Reset();
        if (Pointer.IsEmpty())
        {
            return true;
        }
        if (!Pointer.StartsWith(TEXT("/")))
        {
            OutReason = FString::Printf(
                TEXT("'%s' is not a JSON Pointer: a non-empty pointer must start with '/' "
                     "(e.g. /layers/0/gainDb)."), *Pointer);
            return false;
        }

        int32 TokenStart = 1;
        for (int32 Index = 1; Index <= Pointer.Len(); ++Index)
        {
            if (Index == Pointer.Len() || Pointer[Index] == TEXT('/'))
            {
                FString Token = Pointer.Mid(TokenStart, Index - TokenStart);
                Token.ReplaceInline(TEXT("~1"), TEXT("/"));
                Token.ReplaceInline(TEXT("~0"), TEXT("~"));
                OutTokens.Add(MoveTemp(Token));
                TokenStart = Index + 1;
            }
        }
        return true;
    }

    /** RFC-6901 array index: digits only, no leading zero except the single digit "0". */
    bool ParseArrayIndex(const FString& Token, int32& OutIndex)
    {
        if (Token.IsEmpty() || (Token.Len() > 1 && Token[0] == TEXT('0')))
        {
            return false;
        }
        for (const TCHAR Ch : Token)
        {
            if (Ch < TEXT('0') || Ch > TEXT('9'))
            {
                return false;
            }
        }
        OutIndex = FCString::Atoi(*Token);
        return true;
    }

    /** Structural equality, as RFC-6902's `test` defines it. */
    bool JsonValuesEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
    {
        if (!A.IsValid() || !B.IsValid())
        {
            return A.IsValid() == B.IsValid();
        }
        if (A->Type != B->Type)
        {
            return false;
        }
        switch (A->Type)
        {
        case EJson::Null:
            return true;
        case EJson::Boolean:
            return A->AsBool() == B->AsBool();
        case EJson::Number:
            return A->AsNumber() == B->AsNumber();
        case EJson::String:
            return A->AsString().Equals(B->AsString(), ESearchCase::CaseSensitive);
        case EJson::Array:
        {
            const TArray<TSharedPtr<FJsonValue>>& LeftArray = A->AsArray();
            const TArray<TSharedPtr<FJsonValue>>& RightArray = B->AsArray();
            if (LeftArray.Num() != RightArray.Num())
            {
                return false;
            }
            for (int32 Index = 0; Index < LeftArray.Num(); ++Index)
            {
                if (!JsonValuesEqual(LeftArray[Index], RightArray[Index]))
                {
                    return false;
                }
            }
            return true;
        }
        case EJson::Object:
        {
            const TSharedPtr<FJsonObject> LeftObject = A->AsObject();
            const TSharedPtr<FJsonObject> RightObject = B->AsObject();
            if (!LeftObject.IsValid() || !RightObject.IsValid())
            {
                return LeftObject.IsValid() == RightObject.IsValid();
            }
            if (LeftObject->Values.Num() != RightObject->Values.Num())
            {
                return false;
            }
            for (const auto& Pair : LeftObject->Values)
            {
                const TSharedPtr<FJsonValue> Other = RightObject->TryGetField(Pair.Key);
                if (!Other.IsValid() || !JsonValuesEqual(Pair.Value, Other))
                {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
        }
    }

    /** Read-only pointer resolution. Returns an invalid pointer when the path does not exist. */
    TSharedPtr<FJsonValue> GetAtPointer(const TSharedPtr<FJsonValue>& Root,
                                        const TArray<FString>& Tokens)
    {
        TSharedPtr<FJsonValue> Node = Root;
        for (const FString& Token : Tokens)
        {
            if (!Node.IsValid())
            {
                return nullptr;
            }
            if (Node->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> Object = Node->AsObject();
                const TSharedPtr<FJsonValue> Found = Object.IsValid()
                    ? Object->TryGetField(Token) : TSharedPtr<FJsonValue>();
                if (!Found.IsValid())
                {
                    return nullptr;
                }
                Node = Found;
            }
            else if (Node->Type == EJson::Array)
            {
                const TArray<TSharedPtr<FJsonValue>>& Array = Node->AsArray();
                int32 ElementIndex = 0;
                if (!ParseArrayIndex(Token, ElementIndex) || !Array.IsValidIndex(ElementIndex))
                {
                    return nullptr;
                }
                Node = Array[ElementIndex];
            }
            else
            {
                return nullptr;
            }
        }
        return Node;
    }

    /**
     * Functional pointer write. Returns a replacement for `Node` with the operation applied at
     * Tokens[Depth..]; `Node` and every subtree not on the path are shared, never modified.
     *
     * Op is Add, Remove or Replace only - move / copy / test are composed from these plus
     * GetAtPointer by ApplyJsonPatch.
     */
    bool ApplyAtPointer(const TSharedPtr<FJsonValue>& Node, const TArray<FString>& Tokens,
                        int32 Depth, EPatchOp Op, const TSharedPtr<FJsonValue>& Operand,
                        TSharedPtr<FJsonValue>& OutNode, FString& OutReason)
    {
        if (Depth >= Tokens.Num())
        {
            // The whole document is the target. Replacing it is meaningful; removing it is not.
            if (Op == EPatchOp::Remove)
            {
                OutReason = TEXT("'remove' with an empty path would delete the whole recipe, "
                                 "which is not a patch this verb can render.");
                return false;
            }
            OutNode = Operand;
            return true;
        }

        if (!Node.IsValid())
        {
            OutReason = FString::Printf(
                TEXT("path segment '%s' has no parent container; the pointer walks through a "
                     "value that does not exist."), *Tokens[Depth]);
            return false;
        }

        const FString& Token = Tokens[Depth];
        const bool bLast = (Depth == Tokens.Num() - 1);

        if (Node->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Source = Node->AsObject();
            if (!Source.IsValid())
            {
                OutReason = TEXT("an object on the pointer path is null.");
                return false;
            }
            const TSharedPtr<FJsonValue> Existing = Source->TryGetField(Token);

            TSharedPtr<FJsonValue> Replacement;
            if (bLast)
            {
                if (Op != EPatchOp::Add && !Existing.IsValid())
                {
                    OutReason = FString::Printf(
                        TEXT("key '%s' does not exist, so it cannot be %s. Use 'add' to create "
                             "it, or check the key against the canonical recipe."),
                        *Token, Op == EPatchOp::Remove ? TEXT("removed") : TEXT("replaced"));
                    return false;
                }
                Replacement = Operand;
            }
            else
            {
                if (!ApplyAtPointer(Existing, Tokens, Depth + 1, Op, Operand, Replacement, OutReason))
                {
                    return false;
                }
            }

            const TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>(*Source);
            if (bLast && Op == EPatchOp::Remove)
            {
                Copy->RemoveField(Token);
            }
            else
            {
                Copy->SetField(Token, Replacement);
            }
            OutNode = MakeShared<FJsonValueObject>(Copy);
            return true;
        }

        if (Node->Type == EJson::Array)
        {
            TArray<TSharedPtr<FJsonValue>> Copy = Node->AsArray();

            // "-" is the append position, and RFC-6902 allows it only for `add`.
            const bool bAppend = Token.Equals(TEXT("-"), ESearchCase::CaseSensitive);
            int32 ElementIndex = Copy.Num();
            if (!bAppend && !ParseArrayIndex(Token, ElementIndex))
            {
                OutReason = FString::Printf(
                    TEXT("'%s' is not an array index; array positions are decimal digits with no "
                         "leading zero, or '-' to append."), *Token);
                return false;
            }

            if (bLast)
            {
                if (bAppend && Op != EPatchOp::Add)
                {
                    OutReason = TEXT("'-' names the position after the last element, so it is "
                                     "only valid for 'add'.");
                    return false;
                }
                // add may target Num() (append); remove / replace need an existing element.
                const int32 Limit = (Op == EPatchOp::Add) ? Copy.Num() : Copy.Num() - 1;
                if (ElementIndex < 0 || ElementIndex > Limit)
                {
                    OutReason = FString::Printf(
                        TEXT("index %d is outside the array's %d element(s)."),
                        ElementIndex, Copy.Num());
                    return false;
                }
                if (Op == EPatchOp::Remove)
                {
                    Copy.RemoveAt(ElementIndex);
                }
                else if (Op == EPatchOp::Add)
                {
                    Copy.Insert(Operand, ElementIndex);
                }
                else
                {
                    Copy[ElementIndex] = Operand;
                }
            }
            else
            {
                if (!Copy.IsValidIndex(ElementIndex))
                {
                    OutReason = FString::Printf(
                        TEXT("index %d is outside the array's %d element(s)."),
                        ElementIndex, Copy.Num());
                    return false;
                }
                TSharedPtr<FJsonValue> Replacement;
                if (!ApplyAtPointer(Copy[ElementIndex], Tokens, Depth + 1, Op, Operand,
                                    Replacement, OutReason))
                {
                    return false;
                }
                Copy[ElementIndex] = Replacement;
            }

            OutNode = MakeShared<FJsonValueArray>(Copy);
            return true;
        }

        OutReason = FString::Printf(
            TEXT("path segment '%s' indexes into a %s, which has no members."),
            *Token, Node->Type == EJson::Null ? TEXT("null") : TEXT("scalar"));
        return false;
    }

    /** Which half of a patch failure this is - the two have different remedies (§7). */
    struct FPatchFault
    {
        FString Code;       // ERR_INVALID_PARAMS (the document is wrong) or
                            // ERR_VERIFICATION_FAILED (a 'test' op did not hold)
        int32   OpIndex = INDEX_NONE;
        FString Op;
        FString Path;
        FString Reason;

        TSharedPtr<FJsonObject> ToJson() const
        {
            TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
            Out->SetNumberField(TEXT("opIndex"), OpIndex);
            if (!Op.IsEmpty())   { Out->SetStringField(TEXT("op"), Op); }
            if (!Path.IsEmpty()) { Out->SetStringField(TEXT("path"), Path); }
            Out->SetStringField(TEXT("reason"), Reason);
            return Out;
        }
    };

    /**
     * Applies an RFC-6902 patch array to a document, returning a new document.
     *
     * `In` is never mutated - not on success, not on a fault partway through the array - so a
     * rejected patch leaves the caller's recipe exactly as it was.
     */
    bool ApplyJsonPatch(const TSharedPtr<FJsonObject>& In,
                        const TArray<TSharedPtr<FJsonValue>>& Ops,
                        TSharedPtr<FJsonObject>& Out, FPatchFault& OutFault)
    {
        TSharedPtr<FJsonValue> Document = MakeShared<FJsonValueObject>(In);

        for (int32 OpIndex = 0; OpIndex < Ops.Num(); ++OpIndex)
        {
            OutFault = FPatchFault();
            OutFault.Code = ErrorCodes::ERR_INVALID_PARAMS;
            OutFault.OpIndex = OpIndex;

            const TSharedPtr<FJsonValue>& Entry = Ops[OpIndex];
            const TSharedPtr<FJsonObject> OpObject =
                (Entry.IsValid() && Entry->Type == EJson::Object)
                    ? Entry->AsObject() : TSharedPtr<FJsonObject>();
            if (!OpObject.IsValid())
            {
                OutFault.Reason = TEXT("every element of a JSON Patch array must be an object "
                                       "with at least 'op' and 'path'.");
                return false;
            }

            FString OpName;
            if (!OpObject->TryGetStringField(TEXT("op"), OpName))
            {
                OutFault.Reason = TEXT("missing 'op'.");
                return false;
            }
            OutFault.Op = OpName;

            EPatchOp Op = EPatchOp::Add;
            if (!PatchOpFromString(OpName, Op))
            {
                OutFault.Reason = FString::Printf(
                    TEXT("'%s' is not an RFC-6902 operation; valid values are add, remove, "
                         "replace, move, copy, test."), *OpName);
                return false;
            }

            FString Path;
            if (!OpObject->TryGetStringField(TEXT("path"), Path))
            {
                OutFault.Reason = TEXT("missing 'path' (an RFC-6901 JSON Pointer).");
                return false;
            }
            OutFault.Path = Path;

            TArray<FString> Tokens;
            if (!SplitJsonPointer(Path, Tokens, OutFault.Reason))
            {
                return false;
            }

            // The operand: `value` for add/replace/test, the value read from `from` for
            // move/copy, and nothing for remove.
            TSharedPtr<FJsonValue> Operand;
            if (Op == EPatchOp::Add || Op == EPatchOp::Replace || Op == EPatchOp::Test)
            {
                const TSharedPtr<FJsonValue>* Value = OpObject->Values.Find(TEXT("value"));
                if (Value == nullptr)
                {
                    OutFault.Reason = FString::Printf(
                        TEXT("'%s' requires a 'value' field."), *OpName);
                    return false;
                }
                Operand = *Value;
            }

            TArray<FString> FromTokens;
            if (Op == EPatchOp::Move || Op == EPatchOp::Copy)
            {
                FString From;
                if (!OpObject->TryGetStringField(TEXT("from"), From))
                {
                    OutFault.Reason = FString::Printf(
                        TEXT("'%s' requires a 'from' pointer."), *OpName);
                    return false;
                }
                if (!SplitJsonPointer(From, FromTokens, OutFault.Reason))
                {
                    return false;
                }
                Operand = GetAtPointer(Document, FromTokens);
                if (!Operand.IsValid())
                {
                    OutFault.Reason = FString::Printf(
                        TEXT("'from' pointer '%s' resolves to nothing."), *From);
                    return false;
                }
            }

            if (Op == EPatchOp::Test)
            {
                const TSharedPtr<FJsonValue> Actual = GetAtPointer(Document, Tokens);
                if (!JsonValuesEqual(Actual, Operand))
                {
                    // A failed `test` is not a malformed patch: the document is not in the
                    // state the caller asserted, and the remedy is to re-read the recipe
                    // rather than to fix the patch. Distinct code so nobody parses a message.
                    OutFault.Code = ErrorCodes::ERR_VERIFICATION_FAILED;
                    OutFault.Reason = Actual.IsValid()
                        ? FString::Printf(
                            TEXT("the recipe's value at '%s' is not the one this op asserted, so "
                                 "the document is not in the state the patch was written against. "
                                 "Re-read the recipe rather than editing the patch."), *Path)
                        : FString::Printf(
                            TEXT("'%s' resolves to nothing, so the asserted value cannot hold. "
                                 "Re-read the recipe for the pointer paths it actually has."),
                            *Path);
                    return false;
                }
                continue;
            }

            // move is remove-then-add, and the removal must happen first: moving an array
            // element forward within the same array shifts the destination index otherwise.
            if (Op == EPatchOp::Move)
            {
                TSharedPtr<FJsonValue> AfterRemove;
                if (!ApplyAtPointer(Document, FromTokens, 0, EPatchOp::Remove,
                                    TSharedPtr<FJsonValue>(), AfterRemove, OutFault.Reason))
                {
                    return false;
                }
                Document = AfterRemove;
            }

            const EPatchOp Effective = (Op == EPatchOp::Move || Op == EPatchOp::Copy)
                ? EPatchOp::Add : Op;

            TSharedPtr<FJsonValue> Next;
            if (!ApplyAtPointer(Document, Tokens, 0, Effective, Operand, Next, OutFault.Reason))
            {
                return false;
            }
            Document = Next;
        }

        OutFault = FPatchFault();

        if (!Document.IsValid() || Document->Type != EJson::Object)
        {
            OutFault.Code = ErrorCodes::ERR_INVALID_PARAMS;
            OutFault.Reason = TEXT("the patched document is not a JSON object, so it cannot be "
                                   "a recipe.");
            return false;
        }
        Out = Document->AsObject();
        return true;
    }

    // ---------------------------------------------------------------------------------------
    // Candidate resolution
    // ---------------------------------------------------------------------------------------

    /**
     * Resolves a candidate id, sending the registry's own miss code and structured recovery
     * payload on a miss. CANDIDATE_EVICTED / NO_CANDIDATES / CANDIDATE_NOT_FOUND mean three
     * different next moves, and MissErrorCode is the single writer of that mapping.
     */
    bool ResolveCandidate(FHandlerContext& Ctx, FPwCandidateRegistry& Registry,
                          const FString& CandidateId, FPwCandidateLookupResult& Out)
    {
        Out = Registry.Get(CandidateId);
        if (Out.IsHit())
        {
            return true;
        }
        Ctx.SendError(FPwCandidateRegistry::MissErrorCode(Out.Status),
                      FPwCandidateRegistry::MakeMissMessage(CandidateId, Out),
                      Registry.BuildMissPayload(CandidateId, Out));
        return false;
    }

    /**
     * The id of a resident candidate whose canonical recipe is byte-identical to `Recipe`, or
     * an empty string.
     *
     * The digest is only the cheap filter; the verdict is an exact comparison of the canonical
     * serialized documents, because a CRC32 digest collision would otherwise hand the caller
     * somebody else's audio under their own recipe's name.
     */
    FString FindEquivalentCandidate(FPwCandidateRegistry& Registry, const FPwSynthRecipe& Recipe)
    {
        const FString Digest = FPwCandidateRegistry::RecipeDigest(Recipe);
        const FString Canonical = CanonicalRecipeJson(Recipe);

        int32 Total = 0;
        const TArray<FPwCandidateSummary> Rows = Registry.List(0, /*Limit=*/0, Total);
        for (const FPwCandidateSummary& Row : Rows)
        {
            if (Row.RecipeDigest != Digest)
            {
                continue;
            }
            const FPwCandidateLookupResult Hit = Registry.Get(Row.CandidateId);
            if (Hit.IsHit() && CanonicalRecipeJson(Hit.Candidate->Recipe) == Canonical)
            {
                return Row.CandidateId;
            }
        }
        return FString();
    }

    // ---------------------------------------------------------------------------------------
    // Render report
    // ---------------------------------------------------------------------------------------

    /**
     * The measured half of a render, compactly.
     *
     * Every number here is read off FPwRenderReport, which the renderer fills from observation:
     * FramesMixed comes from FPwAudioBuffer::MixInto (0 means the layer contributed nothing),
     * ClampedSamples counts the samples the final clamp actually moved, and the normalize block
     * carries the analyzer's own input level. Nothing is re-derived from the recipe.
     */
    TSharedPtr<FJsonObject> BuildRenderReport(const FPwRenderReport& Report)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("measured"), Report.bMeasured);
        Out->SetNumberField(TEXT("clampedSamples"), Report.ClampedSamples);
        SetRounded(Out, TEXT("peakDbBeforeNormalize"), Report.PeakDbBeforeNormalize, 2);

        TSharedPtr<FJsonObject> Normalize = MakeShared<FJsonObject>();
        Normalize->SetBoolField(TEXT("measured"), Report.bNormalizeMeasured);
        SetRounded(Normalize, TEXT("gainDb"), Report.NormalizeGainDb, 2);
        // Read bNormalizeMeasured before inputDb: the default 0.0 is not a level, so an
        // unmeasured normalization omits the number rather than publishing a plausible zero.
        if (Report.bNormalizeMeasured)
        {
            SetRounded(Normalize, TEXT("inputDb"), Report.NormalizeInputDb, 2);
        }
        Out->SetObjectField(TEXT("normalize"), Normalize);

        TArray<TSharedPtr<FJsonValue>> Rows;
        TArray<TSharedPtr<FJsonValue>> Unmeasured;
        for (const FPwLayerReport& Layer : Report.Layers)
        {
            if (!Layer.bMeasured)
            {
                // Absence is explicit: a row the renderer never reached is listed by index
                // rather than emitted with zeros that would read as measurements.
                Unmeasured.Add(MakeShared<FJsonValueNumber>(Layer.LayerIndex));
                continue;
            }
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetNumberField(TEXT("layer"), Layer.LayerIndex);
            Row->SetNumberField(TEXT("framesMixed"), Layer.FramesMixed);
            // `peakLinear` and `peakLinearPreGain` are the deliberate pre-gain mono-layer peak
            // after the generator, envelope and layer fx chain. `peakLinearPostGain` is the
            // max absolute sample across the stereo layer after gain and equal-power pan.
            SetRounded(Row, TEXT("peakLinear"), Layer.PeakLinear, 4);
            SetRounded(Row, TEXT("peakLinearPreGain"), Layer.PeakLinear, 4);
            SetRounded(Row, TEXT("peakLinearPostGain"), Layer.PeakLinearPostGain, 4);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        Out->SetArrayField(TEXT("layers"), Rows);
        if (Unmeasured.Num() > 0)
        {
            Out->SetArrayField(TEXT("unmeasuredLayers"), Unmeasured);
        }
        return Out;
    }

    // ---------------------------------------------------------------------------------------
    // Plots
    // ---------------------------------------------------------------------------------------

    enum class EPlotView : uint8
    {
        Waveform,
        Spectrogram,
        ConstantQ
    };

    const TCHAR* LexPlotView(EPlotView View)
    {
        switch (View)
        {
        case EPlotView::Waveform:    return TEXT("waveform");
        case EPlotView::Spectrogram: return TEXT("spectrogram");
        case EPlotView::ConstantQ:   return TEXT("constantq");
        default:                     return TEXT("unknown");
        }
    }

    bool PlotViewFromString(const FString& In, EPlotView& Out)
    {
        const FString Lower = In.ToLower();
        if (Lower == TEXT("waveform"))    { Out = EPlotView::Waveform;    return true; }
        if (Lower == TEXT("spectrogram")) { Out = EPlotView::Spectrogram; return true; }
        if (Lower == TEXT("constantq") || Lower == TEXT("constant_q"))
        {
            Out = EPlotView::ConstantQ;
            return true;
        }
        return false;
    }

    /**
     * Reads the `plots` argument, which accepts a single view name or an array of them.
     *
     * An unrecognised view is an ERROR naming the closed set (§3): silently dropping it would
     * leave the caller waiting for an image that was never going to arrive.
     */
    bool ReadPlotViews(FHandlerContext& Ctx, TArray<EPlotView>& Out)
    {
        Out.Reset();

        const auto AddOne = [&Out](const FString& Name, FString& OutBad)
        {
            EPlotView View = EPlotView::Waveform;
            if (!PlotViewFromString(Name, View))
            {
                OutBad = Name;
                return false;
            }
            Out.AddUnique(View);
            return true;
        };

        FString Bad;
        if (const TArray<TSharedPtr<FJsonValue>>* Array = Ctx.GetArray(TEXT("plots")))
        {
            for (const TSharedPtr<FJsonValue>& Value : *Array)
            {
                if (!Value.IsValid() || Value->Type != EJson::String)
                {
                    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                        TEXT("'plots' holds a non-string element. Every entry names a view: "
                             "waveform, spectrogram or constantq."));
                    return false;
                }
                if (!AddOne(Value->AsString(), Bad))
                {
                    Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                        FString::Printf(
                            TEXT("'plots' holds '%s', which is not a view. Valid views are "
                                 "waveform, spectrogram, constantq."), *Bad));
                    return false;
                }
            }
            return true;
        }

        const FString Single = Ctx.GetString(TEXT("plots"));
        if (Single.IsEmpty())
        {
            return true;
        }
        if (!AddOne(Single, Bad))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("'plots' is '%s', which is not a view. Valid views are "
                                     "waveform, spectrogram, constantq."), *Bad));
            return false;
        }
        return true;
    }

    /** Absolute directory the analyzer PNGs land in. The only thing this subsystem writes to disk. */
    FString PlotDirectory()
    {
        return FPaths::ConvertRelativePathToFull(
            FPaths::ProjectSavedDir() / TEXT("PinWright") / TEXT("audio-plots"));
    }

    /**
     * Renders the requested views for a candidate and reports every one of them - a produced
     * image under `images`, a failed one under `imageFailures`.
     *
     * A plot failure never fails the verb: the render happened and the candidate exists, so
     * reporting a missing image is the truth, while erroring would throw away work the caller
     * can still use.
     */
    void RenderPlots(const FPwAudioBuffer& Buffer, const FString& CandidateId,
                     const TArray<EPlotView>& Views, FPwCandidateRegistry& Registry,
                     const TSharedPtr<FJsonObject>& Result)
    {
        if (Views.Num() == 0)
        {
            return;
        }

        const FString Directory = PlotDirectory();
        IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);

        // The spectrogram view needs an STFT of the mono downmix; the other two read the
        // stereo buffer directly. Computed once and only when asked for.
        FPwStftResult Stft;
        bool bStftAttempted = false;
        FPwStftError StftError;

        TArray<TSharedPtr<FJsonValue>> Images;
        TArray<TSharedPtr<FJsonValue>> Failures;

        for (const EPlotView View : Views)
        {
            // Deterministic name: a retried call rewrites this file rather than adding a
            // second one, which is what makes the verb converge (§8).
            const FString Path = Directory / FString::Printf(
                TEXT("%s_%s.png"), *CandidateId, LexPlotView(View));

            FString PlotError;
            bool bWrote = false;
            switch (View)
            {
            case EPlotView::Waveform:
                bWrote = PwPlotWaveform(Buffer, Path, PlotError);
                break;
            case EPlotView::ConstantQ:
                bWrote = PwPlotConstantQ(Buffer, Path, PlotError);
                break;
            case EPlotView::Spectrogram:
            {
                if (!bStftAttempted)
                {
                    bStftAttempted = true;
                    TArray<float> Mono;
                    Mono.SetNumUninitialized(Buffer.NumFrames());
                    for (int32 Frame = 0; Frame < Buffer.NumFrames(); ++Frame)
                    {
                        Mono[Frame] = 0.5f * (Buffer.Left[Frame] + Buffer.Right[Frame]);
                    }
                    PwComputeStft(Mono, Buffer.SampleRate, FPwStftSettings(), Stft, &StftError);
                }
                if (!Stft.IsValid())
                {
                    PlotError = StftError.Message.IsEmpty()
                        ? TEXT("the STFT could not be computed for this buffer.")
                        : StftError.Message;
                    break;
                }
                bWrote = PwPlotSpectrogram(Stft, Buffer.SampleRate, /*bLogFrequency=*/true,
                                           Path, PlotError);
                break;
            }
            default:
                PlotError = TEXT("unhandled view.");
                break;
            }

            if (!bWrote)
            {
                TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
                Failure->SetStringField(TEXT("view"), LexPlotView(View));
                Failure->SetStringField(TEXT("error"), Truncate(PlotError, MaxRowErrorChars));
                Failures.Add(MakeShared<FJsonValueObject>(Failure));
                continue;
            }

            // sizeBytes is read off the file system, not off the encoder - the one number here
            // the writer cannot fake (§4). A zero-byte PNG is a failed write that returned true.
            const int64 SizeBytes = IFileManager::Get().FileSize(*Path);
            TSharedPtr<FJsonObject> Image = MakeShared<FJsonObject>();
            Image->SetStringField(TEXT("view"), LexPlotView(View));
            Image->SetStringField(TEXT("path"), Path);
            Image->SetNumberField(TEXT("sizeBytes"), static_cast<double>(SizeBytes));
            Image->SetStringField(TEXT("mimeType"), TEXT("image/png"));
            Images.Add(MakeShared<FJsonValueObject>(Image));

            Registry.AddImagePath(CandidateId, Path);
        }

        if (Images.Num() > 0)
        {
            Result->SetArrayField(TEXT("images"), Images);
        }
        if (Failures.Num() > 0)
        {
            Result->SetArrayField(TEXT("imageFailures"), Failures);
        }
    }

    // ---------------------------------------------------------------------------------------
    // The shared render -> register -> analyze -> plot pipeline
    // ---------------------------------------------------------------------------------------

    struct FRenderedCandidate
    {
        FString          CandidateId;
        bool             bReused = false;
        FPwAudioBuffer   Buffer;
        FPwRenderReport  Report;
        FPwAudioAnalysis Analysis;
        bool             bAnalyzed = false;
    };

    /**
     * Renders a recipe and lands it in the registry.
     *
     * Always renders, even when an equivalent candidate is already resident: the render is
     * deterministic in (recipe, seed), so the reused candidate holds exactly these samples, and
     * re-rendering is what lets the response carry a real FPwRenderReport instead of dropping
     * the measurements on the idempotent path.
     */
    bool RenderAndRegister(const FPwSynthRecipe& Recipe, FPwCandidateRegistry& Registry,
                           bool bAnalyze, FRenderedCandidate& Out,
                           FString& OutErrorCode, FString& OutError)
    {
        Out = FRenderedCandidate();

        if (!PwRenderRecipe(Recipe, Out.Buffer, Out.Report, OutErrorCode, OutError))
        {
            return false;
        }

        const FString Existing = FindEquivalentCandidate(Registry, Recipe);
        if (!Existing.IsEmpty())
        {
            Out.CandidateId = Existing;
            Out.bReused = true;
        }
        else
        {
            FPwCandidate Candidate;
            Candidate.Recipe = Recipe;
            Candidate.Buffer = Out.Buffer;
            Out.CandidateId = Registry.Add(MoveTemp(Candidate));
        }

        if (bAnalyze)
        {
            FString AnalysisCode;
            FString AnalysisError;
            Out.bAnalyzed = PwAnalyzeBuffer(Out.Buffer, Out.Analysis, AnalysisCode, AnalysisError);
            if (Out.bAnalyzed)
            {
                Registry.SetAnalysis(Out.CandidateId,
                                     SerializeAudioAnalysis(Out.Analysis, /*bFullDetail=*/false));
            }
            else
            {
                // Kept so the caller learns WHY there is no analysis rather than inferring it
                // from an absent field.
                OutErrorCode = AnalysisCode;
                OutError = AnalysisError;
            }
        }
        return true;
    }

    /** Adds flat target range checks without treating an unavailable metric as a miss. */
    void AddTargetReport(const TSharedPtr<FJsonObject>& Result, const FPwSynthRecipe& Recipe,
                         const FRenderedCandidate& In, bool bAnalyzeRequested)
    {
        if (Recipe.Targets.Num() == 0)
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> Rows;
        Rows.Reserve(Recipe.Targets.Num());
        int32 TargetsMet = 0;

        for (const FPwSynthTargetRange& Target : Recipe.Targets)
        {
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("metric"), PwSynthMetricToString(Target.Metric));

            // Preserve the parsed inclusive range exactly in flat fields. Recipe targets have
            // no separate epsilon, so zero explicitly means no additional tolerance is applied.
            if (Target.Min.IsSet())
            {
                Row->SetNumberField(TEXT("min"), Target.Min.GetValue());
            }
            if (Target.Max.IsSet())
            {
                Row->SetNumberField(TEXT("max"), Target.Max.GetValue());
            }
            Row->SetNumberField(TEXT("tolerance"), 0.0);

            double Measured = 0.0;
            if (bAnalyzeRequested && In.bAnalyzed &&
                PwGetAudioAnalysisMetric(In.Analysis, Target.Metric, Measured) &&
                FMath::IsFinite(Measured))
            {
                // Keep the exposed value identical to the value used for the comparison. A
                // rounded response can otherwise say 0 while the hidden raw value misses.
                Row->SetNumberField(TEXT("measured"), Measured);
                const bool bMinMet = !Target.Min.IsSet() || Measured >= Target.Min.GetValue();
                const bool bMaxMet = !Target.Max.IsSet() || Measured <= Target.Max.GetValue();
                const bool bMet = bMinMet && bMaxMet;
                Row->SetBoolField(TEXT("met"), bMet);
                Row->SetStringField(TEXT("status"), bMet ? TEXT("met") : TEXT("missed"));
                if (bMet)
                {
                    ++TargetsMet;
                }
            }
            else
            {
                // No analysis, failed analysis, or an undefined/non-finite metric is not a
                // measured miss. Omit `met` so absence cannot be mistaken for a false result.
                Row->SetStringField(TEXT("status"), TEXT("unscored"));
            }
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }

        Result->SetArrayField(TEXT("targets"), Rows);
        Result->SetNumberField(TEXT("targetsMet"), TargetsMet);
    }

    /** The candidate-shaped half of a generate / patch response. */
    void AddCandidateBlock(const TSharedPtr<FJsonObject>& Result, const FPwSynthRecipe& Recipe,
                           const FRenderedCandidate& In,
                           bool bAnalyzeRequested, const FString& AnalysisErrorCode,
                           const FString& AnalysisError)
    {
        Result->SetStringField(TEXT("candidateId"), In.CandidateId);
        // Measured from the registry lookup, not from a call counter: true means an identical
        // recipe was already resident and this call converged onto it (§8).
        Result->SetBoolField(TEXT("reused"), In.bReused);
        Result->SetNumberField(TEXT("frames"), In.Buffer.NumFrames());
        Result->SetNumberField(TEXT("sampleRate"), In.Buffer.SampleRate);
        Result->SetNumberField(TEXT("channels"), PwExportChannels);
        SetRounded(Result, TEXT("durationSeconds"), In.Buffer.DurationSeconds(), 4);
        Result->SetObjectField(TEXT("render"), BuildRenderReport(In.Report));

        if (bAnalyzeRequested && In.bAnalyzed)
        {
            // The SUMMARY form. bFullDetail=true adds the onset list, the pitch track and the
            // flux series, which on their own exceed the wrapped response ceiling.
            Result->SetObjectField(TEXT("analysis"),
                                   SerializeAudioAnalysis(In.Analysis, /*bFullDetail=*/false));
        }
        else if (bAnalyzeRequested)
        {
            TSharedPtr<FJsonObject> Unavailable = MakeShared<FJsonObject>();
            Unavailable->SetStringField(TEXT("errorCode"), AnalysisErrorCode);
            Unavailable->SetStringField(TEXT("error"), Truncate(AnalysisError, MaxRowErrorChars));
            Result->SetObjectField(TEXT("analysisUnavailable"), Unavailable);
        }
        AddTargetReport(Result, Recipe, In, bAnalyzeRequested);
    }

    /** Parse failure -> the wire error, carrying the parser's own code and field path. */
    void SendRecipeParseError(FHandlerContext& Ctx, const FPwSynthRecipeError& Error,
                              const TSharedPtr<FJsonObject>& Extra = nullptr)
    {
        TSharedPtr<FJsonObject> Data = Extra.IsValid() ? Extra : MakeShared<FJsonObject>();
        // The field path is what the caller patches, so it travels as structure rather than
        // only inside the message (§7).
        Data->SetStringField(TEXT("field"), Error.Field);
        Ctx.SendError(Error.Code.IsEmpty() ? FString(ErrorCodes::ERR_INVALID_RECIPE) : Error.Code,
                      Error.ToString(), Data);
    }
}

// ===============================================================================================
// audio.synth.generate
// ===============================================================================================

REGISTER_RPC_HANDLER("audio.synth.generate", "audio.synth",
    "Render a synth recipe into a session candidate and report what the render measured: per-layer "
    "frames mixed and pre/post-gain peaks, clamped samples, the normalizer's input level and "
    "applied gain, target scoring, the summary analysis and any requested analyzer PNGs. Audio "
    "never travels inline - the "
    "candidate is in-memory until audio.synth.export writes a USoundWave. Idempotent: an identical "
    "recipe converges onto the candidate it already produced (reused:true) rather than adding a "
    "second copy. A malformed recipe errors with the parser's own field path (e.g. "
    "layers[2].fx[1].kind) and creates nothing.",
    RPC_PARAMS(
        RPC_PARAM_REQ("recipe", "object",
            "The recipe document. Call audio.synth.describe_schema for the grammar; unknown keys, "
            "unrecognised enum values and out-of-range numbers are errors naming the field path."),
        RPC_PARAM_DEF("analyze", "boolean",
            "Measure the rendered buffer and include the summary analysis (scalars only, no "
            "per-frame series).", "true"),
        RPC_PARAM_OPT("plots", "array|string",
            "Analyzer PNGs to render: any of waveform, spectrogram, constantq. Omit for none. "
            "Files land in Saved/PinWright/audio-plots and the response carries their paths, "
            "sizeBytes and mimeType - never image data.")
    ))
{
    using namespace PwSynthGenerateInternal;

    TSharedPtr<FJsonObject> RecipeJson;
    if (!Ctx.RequireObject(TEXT("recipe"), RecipeJson)) return true;

    TArray<EPlotView> Views;
    if (!ReadPlotViews(Ctx, Views)) return true;

    const bool bAnalyze = Ctx.GetBool(TEXT("analyze"), true);

    FPwSynthRecipe Recipe;
    FPwSynthRecipeError ParseError;
    if (!ParseSynthRecipe(RecipeJson, Recipe, ParseError))
    {
        SendRecipeParseError(Ctx, ParseError);
        return true;
    }

    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

    FRenderedCandidate Rendered;
    FString Code;
    FString Error;
    if (!RenderAndRegister(Recipe, Registry, bAnalyze, Rendered, Code, Error))
    {
        // The renderer's own registered code, forwarded unmodified.
        Ctx.SendError(Code.IsEmpty() ? FString(ErrorCodes::ERR_INVALID_RECIPE) : Code, Error);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    AddCandidateBlock(Result, Recipe, Rendered, bAnalyze, Code, Error);
    RenderPlots(Rendered.Buffer, Rendered.CandidateId, Views, Registry, Result);
    AddRegistryPressure(Result, Registry.Usage());

    Ctx.SendSuccess(FString::Printf(
        TEXT("%s candidate %s: %d frames @ %d Hz."),
        Rendered.bReused ? TEXT("Reused") : TEXT("Rendered"),
        *Rendered.CandidateId, Rendered.Buffer.NumFrames(), Rendered.Buffer.SampleRate),
        Result);
    return true;
}

// ===============================================================================================
// audio.synth.patch
// ===============================================================================================

REGISTER_RPC_HANDLER("audio.synth.patch", "audio.synth",
    "Apply an RFC-6902 JSON Patch to a candidate's canonical recipe, re-parse it and render the "
    "result as a NEW candidate. The source candidate is never modified - the patch is applied to a "
    "fresh serialization of its recipe - so both takes stay comparable, and the response reports "
    "both ids plus whether the source is still resident. All six operations are supported "
    "(add, remove, replace, move, copy, test); a failed 'test' reports VERIFICATION_FAILED because "
    "its remedy is to re-read the recipe, while a malformed patch reports INVALID_PARAMS. A patch "
    "whose result does not parse errors with the parser's field path and creates no candidate.",
    RPC_PARAMS(
        PwSynthGenerateInternal::CandidateIdParamReq(
            TEXT("Candidate whose recipe is patched. Its own audio and recipe are left untouched.")),
        RPC_PARAM_REQ("patch", "array",
            "RFC-6902 operations, e.g. [{\"op\":\"replace\",\"path\":\"/layers/0/gainDb\","
            "\"value\":-6}]. Paths are RFC-6901 pointers into the CANONICAL recipe, in which "
            "every optional value has been made explicit."),
        RPC_PARAM_DEF("analyze", "boolean",
            "Measure the rendered buffer and include the summary analysis.", "true"),
        RPC_PARAM_OPT("plots", "array|string",
            "Analyzer PNGs to render for the new candidate: waveform, spectrogram, constantq.")
    ))
{
    using namespace PwSynthGenerateInternal;

    const FString CandidateId = Ctx.GetStringFirstOf(
        {TEXT("candidateId"), TEXT("candidate_id"), TEXT("id")});
    if (CandidateId.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("'candidateId' is required. audio.synth.list_candidates enumerates the ids this "
                 "session holds."));
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
    if (!Ctx.RequireArray(TEXT("patch"), Ops)) return true;

    // An empty patch is a selector that changed nothing, which is an error rather than a
    // re-render dressed up as an edit (§3).
    if (Ops->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("'patch' is an empty array, so the call would change nothing. Pass at least one "
                 "RFC-6902 operation, or call audio.synth.generate to re-render a recipe."));
        return true;
    }
    if (Ops->Num() > MaxPatchOps)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            FString::Printf(TEXT("'patch' holds %d operations; the cap is %d. A recipe is a "
                                 "bounded structure, so a longer patch is a caller error."),
                Ops->Num(), MaxPatchOps));
        return true;
    }

    TArray<EPlotView> Views;
    if (!ReadPlotViews(Ctx, Views)) return true;

    const bool bAnalyze = Ctx.GetBool(TEXT("analyze"), true);

    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

    FPwCandidateLookupResult Source;
    if (!ResolveCandidate(Ctx, Registry, CandidateId, Source)) return true;

    // A fresh serialization, not the stored struct: the patch operates on a document nothing
    // else holds a reference to, and ApplyJsonPatch is functional on top of that.
    const FPwSynthRecipe SourceRecipe = Source.Candidate->Recipe;
    const TSharedPtr<FJsonObject> SourceJson = SerializeSynthRecipe(SourceRecipe);
    if (!SourceJson.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_RECIPE,
            FString::Printf(TEXT("Candidate %s holds a recipe that does not serialize, so there is "
                                 "nothing to patch."), *CandidateId));
        return true;
    }
    const FString SourceCanonical = ToCondensedJson(SourceJson);

    TSharedPtr<FJsonObject> PatchedJson;
    FPatchFault Fault;
    if (!ApplyJsonPatch(SourceJson, *Ops, PatchedJson, Fault))
    {
        TSharedPtr<FJsonObject> Data = Fault.ToJson();
        Data->SetStringField(TEXT("sourceCandidateId"), CandidateId);
        Ctx.SendError(Fault.Code,
            FString::Printf(TEXT("JSON Patch operation %d (%s %s) could not be applied: %s"),
                Fault.OpIndex, *Fault.Op, *Fault.Path, *Fault.Reason),
            Data);
        return true;
    }

    FPwSynthRecipe Patched;
    FPwSynthRecipeError ParseError;
    if (!ParseSynthRecipe(PatchedJson, Patched, ParseError))
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("sourceCandidateId"), CandidateId);
        // Stated because it is the caller's next question: the patch produced an illegal recipe,
        // and nothing was rendered or registered for it.
        Data->SetBoolField(TEXT("candidateCreated"), false);
        SendRecipeParseError(Ctx, ParseError, Data);
        return true;
    }

    FRenderedCandidate Rendered;
    FString Code;
    FString Error;
    if (!RenderAndRegister(Patched, Registry, bAnalyze, Rendered, Code, Error))
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("sourceCandidateId"), CandidateId);
        Data->SetBoolField(TEXT("candidateCreated"), false);
        Ctx.SendError(Code.IsEmpty() ? FString(ErrorCodes::ERR_INVALID_RECIPE) : Code, Error, Data);
        return true;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("sourceCandidateId"), CandidateId);
    AddCandidateBlock(Result, Patched, Rendered, bAnalyze, Code, Error);

    // Measured, not asserted. `changed` compares the two recipes in their CANONICAL form - the
    // parsed-and-reserialized shape - so a patch that only restated a default reports false
    // instead of pretending to be an edit; and sourceStillResident is a fresh registry lookup,
    // because adding this candidate can push the source out under the byte budget.
    Result->SetBoolField(TEXT("changed"), CanonicalRecipeJson(Patched) != SourceCanonical);
    Result->SetBoolField(TEXT("sourceStillResident"), Registry.Get(CandidateId).IsHit());

    RenderPlots(Rendered.Buffer, Rendered.CandidateId, Views, Registry, Result);
    AddRegistryPressure(Result, Registry.Usage());

    Ctx.SendSuccess(FString::Printf(
        TEXT("Patched %s into %s (%d operation(s))."),
        *CandidateId, *Rendered.CandidateId, Ops->Num()), Result);
    return true;
}

// ===============================================================================================
// audio.synth.variations
// ===============================================================================================

namespace PwSynthGenerateInternal
{
    /** One caller-specified perturbation target. Exactly one of the two jitters is set. */
    struct FMutationSpec
    {
        FString          Path;
        TArray<FString>  Tokens;
        bool             bRelative = false;
        double           Amount = 0.0;
        double           BaseValue = 0.0;
    };

    /**
     * Validates the `mutations` argument against the base recipe document.
     *
     * Every check runs BEFORE any render, so a typo'd pointer costs nothing and reports the
     * offending entry by index. A path that resolves to a non-number is an error rather than a
     * skipped target: silently ignoring it would produce `count` identical variations that
     * looked like a working sweep.
     */
    bool ReadMutations(FHandlerContext& Ctx, const TSharedPtr<FJsonObject>& BaseJson,
                       TArray<FMutationSpec>& Out)
    {
        Out.Reset();

        const TArray<TSharedPtr<FJsonValue>>* Entries = Ctx.GetArray(TEXT("mutations"));
        if (Entries == nullptr)
        {
            // A `mutations` that is present but not an array would otherwise read as "no
            // mutations given" and produce a sweep of identical renders (§3).
            const TSharedPtr<FJsonObject>& Payload = Ctx.GetRawPayload();
            if (Payload.IsValid() && Payload->HasField(TEXT("mutations")))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    TEXT("'mutations' is present but is not an array; it must be a list of "
                         "{path, relative|absolute} objects."));
                return false;
            }
            return true;
        }
        if (Entries->Num() > MaxMutations)
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
                FString::Printf(TEXT("'mutations' holds %d targets; the cap is %d. Each target "
                                     "adds a column to every row of the variations table, which "
                                     "is what the cap protects."),
                    Entries->Num(), MaxMutations));
            return false;
        }

        const TSharedPtr<FJsonValue> BaseValue = MakeShared<FJsonValueObject>(BaseJson);

        for (int32 Index = 0; Index < Entries->Num(); ++Index)
        {
            const TSharedPtr<FJsonValue>& Entry = (*Entries)[Index];
            const TSharedPtr<FJsonObject> Object =
                (Entry.IsValid() && Entry->Type == EJson::Object)
                    ? Entry->AsObject() : TSharedPtr<FJsonObject>();
            if (!Object.IsValid())
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("mutations[%d] is not an object; each entry is "
                                         "{path, relative|absolute}."), Index));
                return false;
            }

            FMutationSpec Spec;
            if (!Object->TryGetStringField(TEXT("path"), Spec.Path) || Spec.Path.IsEmpty())
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("mutations[%d] has no 'path'; it must be an RFC-6901 "
                                         "pointer into the canonical recipe, e.g. "
                                         "/layers/0/generator/params/frequencyHz."), Index));
                return false;
            }

            const bool bHasRelative = Object->HasTypedField<EJson::Number>(TEXT("relative"));
            const bool bHasAbsolute = Object->HasTypedField<EJson::Number>(TEXT("absolute"));
            if (bHasRelative == bHasAbsolute)
            {
                // Neither is not a default (nothing would move); both is a contradiction the
                // verb refuses to resolve for the caller (§3).
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(
                        TEXT("mutations[%d] must carry exactly one of 'relative' (a fraction of "
                             "the current value) or 'absolute' (a delta in the value's own "
                             "unit); it carries %s."),
                        Index, bHasRelative ? TEXT("both") : TEXT("neither")));
                return false;
            }
            Spec.bRelative = bHasRelative;
            Spec.Amount = Object->GetNumberField(bHasRelative ? TEXT("relative") : TEXT("absolute"));
            if (Spec.Amount == 0.0)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("mutations[%d] has a jitter of 0, so every variation "
                                         "would be identical at that path."), Index));
                return false;
            }

            FString Reason;
            if (!SplitJsonPointer(Spec.Path, Spec.Tokens, Reason))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(TEXT("mutations[%d]: %s"), Index, *Reason));
                return false;
            }

            const TSharedPtr<FJsonValue> Target = GetAtPointer(BaseValue, Spec.Tokens);
            if (!Target.IsValid() || Target->Type != EJson::Number)
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
                    FString::Printf(
                        TEXT("mutations[%d]: '%s' %s in the canonical recipe. Only numeric "
                             "parameters can be perturbed; fetch the candidate's recipe to see "
                             "the exact pointer paths."),
                        Index, *Spec.Path,
                        Target.IsValid() ? TEXT("is not a number") : TEXT("resolves to nothing")));
                return false;
            }
            Spec.BaseValue = Target->AsNumber();
            Out.Add(MoveTemp(Spec));
        }
        return true;
    }

    /** The metrics the variations table publishes, in emission order. */
    struct FTableMetric
    {
        EPwSynthMetric  Metric;
        const TCHAR*    Key;
        int32           Decimals;
    };

    const TArray<FTableMetric>& VariationTableMetrics()
    {
        static const TArray<FTableMetric> Metrics = {
            {EPwSynthMetric::PeakDb,     TEXT("peakDb"),     2},
            {EPwSynthMetric::RmsDb,      TEXT("rmsDb"),      2},
            {EPwSynthMetric::CrestDb,    TEXT("crestDb"),    2},
            {EPwSynthMetric::CentroidHz, TEXT("centroidHz"), 0},
        };
        return Metrics;
    }
}

REGISTER_RPC_HANDLER("audio.synth.variations", "audio.synth",
    "Render N seeded perturbations of a candidate's recipe and return their ids beside a compact "
    "metric table, so the model can pick. Deliberately NOT an optimizer: nothing here scores, "
    "ranks or selects - the table is evidence and the choice is the caller's. Each variation is a "
    "candidate of its own, so audio.synth.audition and audio.synth.export take its id directly. "
    "Returns a job ticket immediately and renders afterwards, so poll system.job_status with the "
    "ticket_id for the table; the job registers no cancel callback, so system.job_cancel reports "
    "JOB_CANCEL_UNSUPPORTED rather than claiming a stop it cannot deliver. Deterministic: the same "
    "candidate, seed, count and mutations reproduce the same set. Every argument is validated "
    "before the ticket is issued, so a caller error is a real error code and not a failed job.",
    RPC_PARAMS(
        PwSynthGenerateInternal::CandidateIdParamReq(
            TEXT("Candidate whose recipe is the base of the sweep.")),
        RPC_PARAM_REQ("count", "integer",
            "How many variations to render, 1-8. Required rather than defaulted: how wide a sweep "
            "is worth the render time is the caller's decision, and 0 is rejected instead of "
            "returning an empty success."),
        RPC_PARAM_OPT("mutations", "array",
            "Perturbation targets: [{\"path\":\"/layers/0/generator/params/frequencyHz\","
            "\"relative\":0.15}]. 'relative' jitters by +/- that fraction of the current value, "
            "'absolute' by +/- that delta; exactly one per entry. A path that is not a number in "
            "the canonical recipe is an error."),
        FParamSpec{TEXT("varySeed"), TEXT("boolean"),
            TEXT("Also give each variation its own recipe seed, which re-rolls every noise, grain "
                 "and jitter decision. Usable on its own for takes of one unchanged design. "
                 "Snake_case vary_seed accepted."),
            /*bRequired=*/false, TEXT("false"), TArray<FString>({TEXT("vary_seed")})},
        ParamAliasUtils::MakeAliasParamSpec(TEXT("seed"), TEXT("integer"),
            TEXT("Root seed for the perturbations themselves. Defaults to the base recipe's own "
                 "seed, which makes the sweep reproducible without the caller tracking a second "
                 "number. Also accepted as rootSeed."),
            /*bRequired=*/false, TArray<FString>({TEXT("seed"), TEXT("rootSeed")}))
    ))
{
    using namespace PwSynthGenerateInternal;

    const FString CandidateId = Ctx.GetStringFirstOf(
        {TEXT("candidateId"), TEXT("candidate_id"), TEXT("id")});
    if (CandidateId.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("'candidateId' is required. audio.synth.list_candidates enumerates the ids this "
                 "session holds."));
        return true;
    }

    int32 Count = 0;
    if (!Ctx.RequireInt(TEXT("count"), Count)) return true;
    if (Count < 1 || Count > MaxVariations)
    {
        // Zero is not a small number: an empty sweep reported as a success would look identical
        // to a sweep whose every variation failed (§7).
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            FString::Printf(TEXT("count=%d is outside 1..%d. A count of 0 renders nothing, which "
                                 "is an error rather than an empty success."),
                Count, MaxVariations));
        return true;
    }

    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

    FPwCandidateLookupResult Source;
    if (!ResolveCandidate(Ctx, Registry, CandidateId, Source)) return true;

    const FPwSynthRecipe SourceRecipe = Source.Candidate->Recipe;
    const TSharedPtr<FJsonObject> BaseJson = SerializeSynthRecipe(SourceRecipe);
    if (!BaseJson.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_RECIPE,
            FString::Printf(TEXT("Candidate %s holds a recipe that does not serialize, so there is "
                                 "nothing to vary."), *CandidateId));
        return true;
    }

    TArray<FMutationSpec> Mutations;
    if (!ReadMutations(Ctx, BaseJson, Mutations)) return true;

    const bool bVarySeed = Ctx.GetBoolFirstOf({TEXT("varySeed"), TEXT("vary_seed")}, false);
    if (Mutations.Num() == 0 && !bVarySeed)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("Nothing would vary: pass 'mutations' with at least one target, or "
                 "'varySeed': true, or both. A sweep of identical renders is rejected rather "
                 "than converging onto one candidate and reporting N."));
        return true;
    }

    const TOptional<int32> SeedArg = Ctx.GetIntFirstOf({TEXT("seed"), TEXT("rootSeed")});
    const int32 RootSeed = SeedArg.IsSet() ? SeedArg.GetValue() : SourceRecipe.Seed;

    // Everything above is validated synchronously so a caller error is a real error code rather
    // than a job that starts and then fails with a bare string. From here the work is N renders,
    // which is the reason this verb has a ticket at all (§9).
    TSharedPtr<FJsonObject> Started = MakeShared<FJsonObject>();
    Started->SetStringField(TEXT("sourceCandidateId"), CandidateId);
    Started->SetNumberField(TEXT("count"), Count);
    // Structurally true, not an assertion: nothing below calls FJobRegistry::SetCancelCallback,
    // which is the only thing Cancel() reads to tell a cancellable verb from an uncancellable one.
    Started->SetBoolField(TEXT("cancellable"), false);
    Started->SetStringField(TEXT("message"),
        TEXT("Rendering variations. This job registers no cancel callback - a render cannot be "
             "stopped mid-flight - so system.job_cancel will report JOB_CANCEL_UNSUPPORTED "
             "rather than a cancellation that did not happen. Poll system.job_status with this "
             "ticket_id for the variations table."));

    FJobBindArgs Args;
    Args.Method = TEXT("audio.synth.variations");
    Args.StartedPayload = Started;
    Args.BindNativeDelegate =
        [BaseJson, SourceRecipe, CandidateId, Count, RootSeed, bVarySeed, Mutations]
        (FJobOnComplete OnComplete)
    {
        FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

        TArray<TSharedPtr<FJsonValue>> Rows;
        int32 NumRendered = 0;
        int32 NumFailed = 0;

        for (int32 Variation = 1; Variation <= Count; ++Variation)
        {
            // Per-variation stream hashed from (root seed, variation index) - so variation 3 is
            // the same variation whether or not 1 and 2 were rendered, and a retry of the whole
            // call reproduces the identical set.
            FRandomStream Stream(static_cast<int32>(HashCombine(
                GetTypeHash(RootSeed), GetTypeHash(Variation))));

            TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>(*BaseJson);
            TSharedPtr<FJsonValue> DocumentValue = MakeShared<FJsonValueObject>(Document);

            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetNumberField(TEXT("index"), Variation);

            TArray<TSharedPtr<FJsonValue>> AppliedValues;
            FString Reason;
            bool bBuilt = true;

            for (const FMutationSpec& Spec : Mutations)
            {
                const double Jitter = Stream.FRandRange(-1.0f, 1.0f);
                const double NewValue = Spec.bRelative
                    ? Spec.BaseValue * (1.0 + Spec.Amount * Jitter)
                    : Spec.BaseValue + Spec.Amount * Jitter;

                TSharedPtr<FJsonValue> Next;
                if (!ApplyAtPointer(DocumentValue, Spec.Tokens, 0, EPatchOp::Replace,
                                    MakeShared<FJsonValueNumber>(NewValue), Next, Reason))
                {
                    bBuilt = false;
                    break;
                }
                DocumentValue = Next;
                AppliedValues.Add(MakeShared<FJsonValueNumber>(
                    FMath::RoundToDouble(NewValue * 1000.0) / 1000.0));
            }

            int32 VariationSeed = SourceRecipe.Seed;
            if (bBuilt && bVarySeed)
            {
                VariationSeed = Stream.RandRange(0, MAX_int32 - 1);
                TSharedPtr<FJsonValue> Next;
                TArray<FString> SeedTokens = {TEXT("seed")};
                if (!ApplyAtPointer(DocumentValue, SeedTokens, 0, EPatchOp::Replace,
                                    MakeShared<FJsonValueNumber>(VariationSeed), Next, Reason))
                {
                    bBuilt = false;
                }
                else
                {
                    DocumentValue = Next;
                }
            }

            const auto FailRow = [&Row, &Rows, &NumFailed](const TCHAR* Code, const FString& Message)
            {
                Row->SetBoolField(TEXT("rendered"), false);
                Row->SetStringField(TEXT("errorCode"), Code);
                Row->SetStringField(TEXT("error"), Truncate(Message, MaxRowErrorChars));
                Rows.Add(MakeShared<FJsonValueObject>(Row));
                ++NumFailed;
            };

            if (!bBuilt)
            {
                FailRow(ErrorCodes::ERR_INVALID_PARAMS, Reason);
                continue;
            }

            FPwSynthRecipe Varied;
            FPwSynthRecipeError ParseError;
            if (!ParseSynthRecipe(DocumentValue->AsObject(), Varied, ParseError))
            {
                // The usual cause: a perturbation walked a parameter past its schema range.
                // Reported per row rather than aborting the sweep - the other variations are
                // still worth having.
                FailRow(ParseError.Code.IsEmpty() ? ErrorCodes::ERR_INVALID_RECIPE
                                                  : *ParseError.Code,
                        ParseError.ToString());
                continue;
            }

            FRenderedCandidate Rendered;
            FString Code;
            FString Error;
            if (!RenderAndRegister(Varied, Registry, /*bAnalyze=*/true, Rendered, Code, Error))
            {
                FailRow(Code.IsEmpty() ? ErrorCodes::ERR_INVALID_RECIPE : *Code, Error);
                continue;
            }

            Row->SetStringField(TEXT("id"), Rendered.CandidateId);
            Row->SetBoolField(TEXT("rendered"), true);
            if (Rendered.bReused)
            {
                Row->SetBoolField(TEXT("reused"), true);
            }
            if (bVarySeed)
            {
                Row->SetNumberField(TEXT("seed"), VariationSeed);
            }
            if (AppliedValues.Num() > 0)
            {
                // Aligned with the `mutations` argument by index, so the caller reads what each
                // target actually became without a second call.
                Row->SetArrayField(TEXT("values"), AppliedValues);
            }

            if (Rendered.bAnalyzed)
            {
                for (const FTableMetric& Metric : VariationTableMetrics())
                {
                    double Value = 0.0;
                    // Absent rather than zero when the family was not measured: the accessor is
                    // the same one the report serializer uses, so a metric this signal cannot
                    // express is omitted instead of scored against 0.
                    if (PwGetAudioAnalysisMetric(Rendered.Analysis, Metric.Metric, Value))
                    {
                        SetRounded(Row, Metric.Key, Value, Metric.Decimals);
                    }
                }
            }

            Rows.Add(MakeShared<FJsonValueObject>(Row));
            ++NumRendered;
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("sourceCandidateId"), CandidateId);
        Result->SetNumberField(TEXT("requested"), Count);
        // Three counters rather than one flag: "8 rendered" and "8 attempted, 8 rejected by the
        // schema" are different outcomes and only the counters can tell them apart.
        Result->SetNumberField(TEXT("rendered"), NumRendered);
        Result->SetNumberField(TEXT("failed"), NumFailed);
        Result->SetArrayField(TEXT("variations"), Rows);
        AddRegistryPressure(Result, Registry.Usage());

        if (NumRendered == 0)
        {
            // A failed job carries only an error STRING on the wire, so the code travels in the
            // result payload, which system.job_status publishes beside it.
            Result->SetStringField(TEXT("errorCode"), ErrorCodes::ERR_INVALID_RECIPE);
            OnComplete(false, Result, FString::Printf(
                TEXT("All %d variation(s) failed to render; see variations[].errorCode for the "
                     "per-variation reason."), Count));
            return;
        }
        OnComplete(true, Result, FString());
    };

    Ctx.StartJob(Args);
    return true;
}

// ===============================================================================================
// audio.synth.export
// ===============================================================================================

namespace PwSynthGenerateInternal
{
    // Absolute tolerance for the decode-back RMS/peak comparison, in normalized float.
    //
    // Derived, not guessed, and the same figure audio.authoring.create_sound_wave_from_pcm uses
    // for the same round trip: the encode is Audio::ArrayFloatToPcm16 (multiply by 32767 and
    // truncate, FloatArrayMath.cpp:2490) and the decode divides by 32768, so a correct round
    // trip stays inside ~6.2e-5 per sample. Both RMS and peak are 1-Lipschitz in the per-sample
    // error, so the same bound covers the signatures. 1e-3 is ~16x that - loose enough to
    // survive a change of scale convention on either side, and still orders of magnitude away
    // from a silent asset, a half-length asset or a dropped channel.
    constexpr double ExportToleranceAbs = 1.0e-3;

    struct FChannelSignature
    {
        double Rms = 0.0;
        double Peak = 0.0;
    };

    FChannelSignature MeasureChannel(const TArray<float>& Samples)
    {
        FChannelSignature Out;
        if (Samples.Num() <= 0)
        {
            return Out;
        }
        double SumOfSquares = 0.0;
        double Peak = 0.0;
        for (const float Sample : Samples)
        {
            const double Value = static_cast<double>(Sample);
            SumOfSquares += Value * Value;
            Peak = FMath::Max(Peak, FMath::Abs(Value));
        }
        Out.Rms = FMath::Sqrt(SumOfSquares / static_cast<double>(Samples.Num()));
        Out.Peak = Peak;
        return Out;
    }

    /** Per-channel comparison; deltas are always emitted so a failure says by how much. */
    bool AddChannelComparison(const TSharedPtr<FJsonObject>& Parent, const TCHAR* FieldName,
                              const FChannelSignature& Source, const FChannelSignature& Decoded)
    {
        const double RmsDelta = FMath::Abs(Source.Rms - Decoded.Rms);
        const double PeakDelta = FMath::Abs(Source.Peak - Decoded.Peak);

        TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
        SetRounded(Block, TEXT("sourceRms"), Source.Rms, 6);
        SetRounded(Block, TEXT("decodedRms"), Decoded.Rms, 6);
        SetRounded(Block, TEXT("rmsDelta"), RmsDelta, 6);
        SetRounded(Block, TEXT("sourcePeak"), Source.Peak, 6);
        SetRounded(Block, TEXT("decodedPeak"), Decoded.Peak, 6);
        SetRounded(Block, TEXT("peakDelta"), PeakDelta, 6);
        Parent->SetObjectField(FieldName, Block);

        return RmsDelta <= ExportToleranceAbs && PeakDelta <= ExportToleranceAbs;
    }
}

REGISTER_RPC_HANDLER("audio.synth.export", "audio.synth",
    "Write a candidate's audio into the project as a USoundWave asset, then verify it by decoding "
    "the created asset back through USoundWave::GetImportedSoundWaveData and comparing frames, "
    "sample rate, channel count and an RMS/peak signature against the candidate's buffer - a "
    "different subsystem from the one that wrote it, so nothing in the verification is a readback "
    "of a field the writer just set. There is no file export anywhere in this subsystem: audio "
    "leaves as an asset path plus duration, rate and channels. Idempotent - re-exporting the same "
    "candidate to the same path rewrites that wave's audio in place rather than creating a second "
    "asset, and the rewrite keeps every property that is not the payload: SoundClass, "
    "attenuation, concurrency, submix and bus sends, modulation, loading behaviour, compression "
    "type, looping, volume, sound group. verification.propertiesPreserved measures that and a "
    "failure fails the call. What the rewrite does refresh is the state parsed out of the old "
    "audio - duration, format, cue points, channel layout, timecode. The response also reports "
    "routing.soundClass / routing.attenuationSettings as they stand after the write; a newly "
    "created wave has neither, and an unrouted wave escapes every SoundMix and plays at full "
    "level at any distance, so set them with property.set before shipping it.",
    RPC_PARAMS(
        PwSynthGenerateInternal::CandidateIdParamReq(TEXT("Candidate to export.")),
        RPC_PARAM_REQ("name", "string", "Asset name without extension, e.g. 'SW_Impact'."),
        RPC_PARAM_REQ("path", "path",
            "Content-browser folder for the asset; must be under /Game, e.g. /Game/Audio."),
        RPC_PARAM_DEF("save", "boolean",
            "Write the .uasset to disk. false marks the package dirty only, and the response "
            "reports saved:false / pendingFlush:true.", "true"),
        RPC_PARAM_DEF("overwrite", "boolean",
            "Delete and recreate an existing SoundWave instead of rewriting it in place. Rejected "
            "with ASSET_IN_USE when other packages reference it.", "false")
    ))
{
    using namespace PwSynthGenerateInternal;

    const FString CandidateId = Ctx.GetStringFirstOf(
        {TEXT("candidateId"), TEXT("candidate_id"), TEXT("id")});
    if (CandidateId.IsEmpty())
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PARAMS,
            TEXT("'candidateId' is required. audio.synth.list_candidates enumerates the ids this "
                 "session holds."));
        return true;
    }

    FString Name;
    if (!Ctx.RequireString(TEXT("name"), Name)) return true;
    FString FolderPath;
    if (!Ctx.RequireString(TEXT("path"), FolderPath)) return true;

    // Reject a name the sanitizer would rewrite rather than quietly creating an asset under a
    // different name than the caller asked for (§1).
    const FString SanitizedName = SanitizeAssetName(Name);
    if (SanitizedName.IsEmpty() || SanitizedName != Name)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
            FString::Printf(TEXT("Invalid asset name '%s': contains characters that cannot be used "
                                 "in asset names. Valid name would be: '%s'"),
                *Name, *SanitizedName));
        return true;
    }

    FString ValidatedPath;
    FString PathError;
    if (!ValidateAssetCreationPath(FolderPath, Name, ValidatedPath, PathError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH, PathError);
        return true;
    }

    const FString ValidatedFolder = FPackageName::GetLongPackagePath(ValidatedPath);
    if (!ValidatedFolder.Equals(TEXT("/Game")) && !ValidatedFolder.StartsWith(TEXT("/Game/")))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_PATH,
            FString::Printf(TEXT("SoundWave assets can only be created under /Game; '%s' resolves "
                                 "to '%s'. The engine writer hardcodes a /Game/ prefix, so any "
                                 "other mount root would land the asset somewhere else."),
                *FolderPath, *ValidatedFolder));
        return true;
    }

    FPwCandidateRegistry& Registry = FPluginState::Get().GetCandidateRegistry();

    FPwCandidateLookupResult Lookup;
    if (!ResolveCandidate(Ctx, Registry, CandidateId, Lookup)) return true;

    // A reference, not a copy: the lookup's shared handle keeps the candidate alive for the
    // whole call even if another thread evicts it, and the buffer is megabytes.
    const FPwAudioBuffer& Source = Lookup.Candidate->Buffer;
    if (Source.NumFrames() <= 0 || !Source.IsValid())
    {
        Ctx.SendError(ErrorCodes::ERR_AUDIO_EMPTY_BUFFER,
            FString::Printf(TEXT("Candidate %s holds no audio (frames=%d, rate=%d Hz), so there is "
                                 "nothing to write."),
                *CandidateId, Source.NumFrames(), Source.SampleRate));
        return true;
    }

    const bool bSave = Ctx.GetBool(TEXT("save"), true);
    const bool bOverwrite = Ctx.GetBool(TEXT("overwrite"), false);

    // Mandatory before any create path: IAssetTools/CreatePackage on an occupied path can reach
    // a modal overwrite prompt that wedges the game thread for every client. bRequireExactClass
    // because a USoundWaveProcedural / USoundSourceBus at the path passes IsA(USoundWave) but is
    // a different object layout - reconstructing one as a plain USoundWave is corruption.
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        ValidatedPath, Name, USoundWave::StaticClass(), bOverwrite, /*bRequireExactClass=*/true);
    if (Resolution.IsRejected())
    {
        return AssetCreatePolicy::SendRejection(Ctx, Resolution);
    }

    FString ExportError;
    FPwSoundWaveWriteReport WriteReport;
    USoundWave* Wave = PwCreateSoundWaveAsset(
        Source, ValidatedFolder, Name, bSave, WriteReport, ExportError);
    if (!Wave)
    {
        Ctx.SendError(ErrorCodes::ERR_CREATION_FAILED, ExportError);
        return true;
    }

    const FString AssetPath = Wave->GetPathName();

    // -------------------------------------------------------------------------------------
    // Verification (§4). PwCreateSoundWaveAsset does NOT self-verify its contents - it checks
    // the writer's state machine and the package it landed in - so the round trip is done
    // here, through USoundWave::GetImportedSoundWaveData, which parses the RIFF payload
    // independently of the FSoundWavePCMWriter path that wrote it. Nothing below reads a field
    // the writer assigned.
    // -------------------------------------------------------------------------------------
    FPwAudioBuffer Decoded;
    FString DecodeCode;
    FString DecodeError;
    if (!PwDecodeSoundWaveWithCode(Wave, Decoded, DecodeCode, DecodeError))
    {
        TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
        Failure->SetStringField(TEXT("candidateId"), CandidateId);
        AssetCreatePolicy::AddCreateReport(Failure, Resolution);
        AddAssetVerification(Failure, Wave);
        // After AddAssetVerification, never before: it writes its own top-level "assetPath"
        // (ResolveVerificationAssetPath, AssetUtils.cpp:1346) which for a top-level asset is the
        // bare PACKAGE path, silently replacing the object path this verb publishes. Callers
        // chain audio.analysis.* on this handle, and those resolve an object path.
        Failure->SetStringField(TEXT("assetPath"), AssetPath);
        Ctx.SendError(DecodeCode,
            FString::Printf(TEXT("The asset was written but could not be decoded back, so nothing "
                                 "about its contents is verified: %s"), *DecodeError),
            Failure);
        return true;
    }

    // The channel count is the one thing a deinterleaved buffer cannot carry, so it is read
    // straight off the payload header - still the parse side, never the write side.
    TArray<uint8> PayloadPcm;
    uint32 PayloadSampleRate = 0;
    uint16 PayloadChannels = 0;
    const bool bReadPayloadHeader =
        Wave->GetImportedSoundWaveData(PayloadPcm, PayloadSampleRate, PayloadChannels);

    const bool bFramesMatch = Decoded.NumFrames() == Source.NumFrames();
    const bool bRateMatch = Decoded.SampleRate == Source.SampleRate;
    const bool bChannelsMatch = bReadPayloadHeader &&
        static_cast<int32>(PayloadChannels) == PwExportChannels;

    TSharedPtr<FJsonObject> Verification = MakeShared<FJsonObject>();
    Verification->SetBoolField(TEXT("measured"), true);
    Verification->SetStringField(TEXT("method"),
        TEXT("decoded back through USoundWave::GetImportedSoundWaveData (PwDecodeSoundWave), "
             "which parses the RIFF payload independently of the FSoundWavePCMWriter path that "
             "wrote it"));
    Verification->SetNumberField(TEXT("toleranceAbs"), ExportToleranceAbs);
    Verification->SetBoolField(TEXT("framesMatch"), bFramesMatch);
    Verification->SetNumberField(TEXT("sourceFrames"), Source.NumFrames());
    Verification->SetNumberField(TEXT("decodedFrames"), Decoded.NumFrames());
    Verification->SetBoolField(TEXT("sampleRateMatch"), bRateMatch);
    Verification->SetBoolField(TEXT("channelsMatch"), bChannelsMatch);
    Verification->SetNumberField(TEXT("payloadChannels"), bReadPayloadHeader ? PayloadChannels : 0);
    Verification->SetBoolField(TEXT("payloadHeaderRead"), bReadPayloadHeader);

    const bool bLeftMatch = AddChannelComparison(Verification, TEXT("left"),
        MeasureChannel(Source.Left), MeasureChannel(Decoded.Left));
    const bool bRightMatch = AddChannelComparison(Verification, TEXT("right"),
        MeasureChannel(Source.Right), MeasureChannel(Decoded.Right));

    // An in-place rewrite that moved a property the payload does not own fails the verb. The
    // asset's audio can be perfect and the asset still be broken: a wave that lost its
    // SoundClass escapes every SoundMix and one that lost its attenuation plays at full level
    // at any distance, and neither shows up in a frames/rate/RMS comparison. This is the half
    // of the verification that used to answer pass:true through exactly that loss
    // (board B-synth-export-wipes-soundclass-attenuation).
    const bool bPropertiesPreserved = WriteReport.ChangedProperties.Num() == 0;

    const bool bVerified = bFramesMatch && bRateMatch && bChannelsMatch && bLeftMatch &&
        bRightMatch && bPropertiesPreserved;
    Verification->SetBoolField(TEXT("pass"), bVerified);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("candidateId"), CandidateId);
    Result->SetStringField(TEXT("assetName"), Name);
    // Read off the decode, never off the wave's own fields: Duration, TotalSamples and
    // RawPCMDataSize are engine-derived and this verb neither writes nor reports them (§5a).
    Result->SetNumberField(TEXT("frames"), Decoded.NumFrames());
    Result->SetNumberField(TEXT("sampleRate"), Decoded.SampleRate);
    Result->SetNumberField(TEXT("channels"), bReadPayloadHeader ? PayloadChannels : 0);
    SetRounded(Result, TEXT("durationSeconds"), Decoded.DurationSeconds(), 4);
    Result->SetObjectField(TEXT("verification"), Verification);
    // "routing" on the result, "propertiesPreserved" on the verification. Emitted on both the
    // create and the rewrite: an empty soundClass on a create is not a loss, but it is the same
    // unrouted asset at the end of it, and the response is the only place the caller sees it.
    PwAddSoundWaveWriteReport(Result, Verification, WriteReport);

    // The same {saveRequested, saved, pendingFlush} triple from both save families, so a caller
    // never has to know which one ran (§5).
    if (bSave)
    {
        AddAssetSaveReport(Result, /*bSaveRequested=*/true, WriteReport.bSavedToDisk);
    }
    else
    {
        AddMarkDirtySaveReport(Result, Wave, /*bSaveRequested=*/false);
    }
    AssetCreatePolicy::AddCreateReport(Result, Resolution);
    AddAssetVerification(Result, Wave);
    // Same ordering rule as the decode-failure branch above: AddAssetVerification's own top-level
    // "assetPath" is the bare package path, so the object path this verb reports has to be written
    // after it, not before. Covers both sends below - the verification failure and the success.
    Result->SetStringField(TEXT("assetPath"), AssetPath);

    if (!bVerified)
    {
        // Two failure shapes share one code, so the message names which one happened: the audio
        // can be exact and the asset still be wrong.
        const FString PropertyClause = bPropertiesPreserved
            ? FString()
            : FString::Printf(
                TEXT(" The rewrite also changed %d propert%s the payload does not own (%s); see "
                     "verification.changedProperties."),
                WriteReport.ChangedProperties.Num(),
                WriteReport.ChangedProperties.Num() == 1 ? TEXT("y") : TEXT("ies"),
                *FString::JoinBy(WriteReport.ChangedProperties, TEXT(", "),
                    [](const FName& PropertyName) { return PropertyName.ToString(); }));

        Ctx.SendError(ErrorCodes::ERR_VERIFICATION_FAILED,
            FString::Printf(TEXT("'%s' was written but the decode-back disagrees with candidate %s "
                                 "(frames %d vs %d, %d Hz vs %d Hz, %d channels vs %d). The asset "
                                 "exists; its contents are not what was requested.%s"),
                *AssetPath, *CandidateId, Source.NumFrames(), Decoded.NumFrames(),
                Source.SampleRate, Decoded.SampleRate,
                PwExportChannels, bReadPayloadHeader ? static_cast<int32>(PayloadChannels) : 0,
                *PropertyClause),
            Result);
        return true;
    }

    // Recorded on the candidate only after the round trip passed, so list_candidates cannot
    // show an export that did not verify.
    Registry.SetExportedAssetPath(CandidateId, AssetPath);

    Ctx.SendSuccess(FString::Printf(
        TEXT("Exported candidate %s to %s (%d frames @ %d Hz)."),
        *CandidateId, *AssetPath, Decoded.NumFrames(), Decoded.SampleRate), Result);
    return true;
}
