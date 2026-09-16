// Copyright (c) 2026 Alexander Penkin. MIT License.

// GeometryScriptDebugSink.h - the UGeometryScriptDebug argument this module used to pass as
// nullptr at every call site, and the single reason it could not detect a failed boolean at all.
//
// The GeometryScript library does NOT report failure through its return value. Every
// UGeometryScriptLibrary_* entry point in UE 5.8 returns its TargetMesh on EVERY path it has,
// including the paths it considers errors:
//
//   ApplyMeshBoolean - returns TargetMesh from both null-input guards
//                      (MeshBooleanFunctions.cpp:38-47) and from the empty-result refusal
//                      (:96-99), which is the only reachable failure of a well-formed call.
//                      Its own `bSuccess` from FMeshBoolean::Compute() is deliberately dropped
//                      one line earlier (":93 // Note: ignore bSuccess, as it comes back false
//                      even if we only had small errors..."), so it is not a signal either.
//   AppendMesh       - returns TargetMesh from both null-input guards
//                      (MeshBasicEditFunctions.cpp:590-599).
//   WeldMeshEdges    - returns TargetMesh after appending "WeldMeshEdges: Weld Operation
//                      returned error flag" when FMergeCoincidentMeshEdges::Apply() fails
//                      (MeshRepairFunctions.cpp:147).
//   ScaleMesh        - returns TargetMesh from its null guard (MeshTransformFunctions.cpp).
//
// The Debug argument is the ONLY channel any of that travels through. A call site passing
// nullptr there discards the whole detection channel, which makes every `if (!ResultMesh)` guard
// written behind it dead code: the return can be null only when the CALLER passed a null mesh,
// and the ops in this module already reject that one line earlier. That is exactly how
// geometry.boolean_union / boolean_subtract / boolean_intersection / boolean_trim came to report
// success on a boolean the engine had refused, leaving the caller to build on the unmodified
// target - and how geometry.mirror's ERR_OPERATION_FAILED became unreachable.
//
// Ownership and lifetime: UGeometryScriptDebug is a plain UObject holding one TArray of
// FGeometryScriptDebugMessage (GeometryScriptTypes.h:833-846). The sink creates it in the
// transient package and roots it with TStrongObjectPtr for the sink's lifetime - the same
// pattern PwModelCollision/PwModelCompiler use for their scratch UDynamicMesh objects - because
// a GC between construction and the engine call would otherwise be free to collect it. The
// engine only ever appends to the object and never retains a pointer past the call, so the sink
// belongs on the stack of the op that uses it and must not outlive it.
//
// Cost at a PER-ELEMENT reporter: not one message, but one per rejected element. Passing a sink
// where the call site used to pass nullptr changes what the engine does with those messages -
// with nullptr each is built and dropped, with a sink each is RETAINED for the sink lifetime. A
// wholly-malformed 500k-triangle append therefore holds 500k FGeometryScriptDebugMessage entries
// at once. That is a constant factor on an input the caller already materialised (the buffers are
// larger than the messages), not an amplification, and the sink is stack-scoped so it is released
// the moment the op returns - but it is the reason ErrorSummary() exists and the reason a sink
// must not be hoisted to a longer-lived scope than the op it guards.
//
// Cost: one small UObject per guarded op. The .pwmodel compiler runs booleans in a loop, so this
// sits on an inner path - but the object is one empty array and the boolean it guards costs
// milliseconds, so it is not worth pooling. Do NOT turn it into a shared static: the message
// list is per-call state, and a shared one would attribute one op's failure to the next op.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "Handlers/Geometry/GeometryOps.h"

#if __has_include("GeometryScript/GeometryScriptTypes.h")
#include "GeometryScript/GeometryScriptTypes.h"
#else
#include "GeometryScriptTypes.h"
#endif

namespace GeometryOps
{
    // A scoped UGeometryScriptDebug. Pass Get() wherever a GeometryScript call takes a
    // UGeometryScriptDebug*, then ask the sink what the engine said.
    //
    // Non-copyable by construction (TStrongObjectPtr is move-only), which is what stops a sink
    // from being captured by value into a lambda and read after the op it belongs to returned.
    class FGeometryScriptDebugSink
    {
    public:
        FGeometryScriptDebugSink()
            : Debug(NewObject<UGeometryScriptDebug>(GetTransientPackage()))
        {
        }

        UGeometryScriptDebug* Get() const
        {
            return Debug.Get();
        }

        // True when the engine appended at least one ErrorMessage. Warnings deliberately do not
        // count: EGeometryScriptDebugMessageType separates the two, and only an error means the
        // operation did not happen.
        bool HasError() const
        {
            const UGeometryScriptDebug* Ptr = Debug.Get();
            if (!Ptr)
            {
                return false;
            }
            for (const FGeometryScriptDebugMessage& Message : Ptr->Messages)
            {
                if (Message.MessageType == EGeometryScriptDebugMessageType::ErrorMessage)
                {
                    return true;
                }
            }
            return false;
        }

        // Every error the engine appended, verbatim and in order, joined with "; ".
        //
        // Verbatim on purpose. The empty-result refusal names its own remedy - "enable Allow
        // Empty Result if empty results should be accepted" - and that sentence is the whole
        // value of forwarding the text at all. It is also the only front-end-neutral wording
        // available here: the ops layer has no idea whether its caller spells that option
        // `allowEmptyResult` (RPC) or `allow_empty_result` (.pwmodel), so it names neither.
        //
        // Empty when the engine reported no error, so a caller must gate on HasError() rather
        // than on this being non-empty.
        FString ErrorText() const
        {
            const UGeometryScriptDebug* Ptr = Debug.Get();
            if (!Ptr)
            {
                return FString();
            }

            TArray<FString> Lines;
            for (const FGeometryScriptDebugMessage& Message : Ptr->Messages)
            {
                if (Message.MessageType == EGeometryScriptDebugMessageType::ErrorMessage)
                {
                    Lines.Add(Message.Message.ToString());
                }
            }
            return FString::Join(Lines, TEXT("; "));
        }

        // How many ErrorMessages the engine appended. Not a boolean restatement: several engine
        // functions report ONE ERROR PER REJECTED ELEMENT rather than one per call, so this is
        // the count of things the engine refused, and it is the number the caller needs.
        //
        // AppendBuffersToMesh is the reason this exists. It loops the incoming triangle array
        // and calls AppendError once per triangle it declines - invalid indices, non-manifold
        // topology, duplicate triangle - then CONTINUES with the rest
        // (MeshBasicEditFunctions.cpp, the AppendBuffersToMesh EditMesh lambda). A 40k-triangle
        // OBJ whose every face is non-manifold produces 40k messages.
        int32 ErrorCount() const
        {
            const UGeometryScriptDebug* Ptr = Debug.Get();
            if (!Ptr)
            {
                return 0;
            }
            int32 Count = 0;
            for (const FGeometryScriptDebugMessage& Message : Ptr->Messages)
            {
                if (Message.MessageType == EGeometryScriptDebugMessageType::ErrorMessage)
                {
                    ++Count;
                }
            }
            return Count;
        }

        // ErrorText() for the per-element reporters: the same text, deduplicated, each distinct
        // message carrying how many times the engine said it.
        //
        // ErrorText() cannot be used at those sites. It joins every message verbatim, so the
        // 40k-triangle case above produces a multi-megabyte string that goes into an FOpResult,
        // through the dispatcher, and out over the wire as one JSON field. Bounding it is not
        // cosmetic; it is what stops one malformed input file from turning an error response
        // into a denial of service against whatever is reading it.
        //
        // Deduplication is lossless HERE and only here: the engine builds these messages from
        // LOCTEXT constants with no per-element data interpolated into them, so every triangle
        // rejected for the same reason produces a byte-identical string. Collapsing them
        // discards no information the caller did not already get from the count. A future engine
        // that interpolates a triangle index into the text would defeat that, and the symptom
        // would be a summary as long as ErrorText() - which MaxDistinct then bounds anyway.
        //
        // Verbatim inside each entry, for the same reason ErrorText() is verbatim: the engine's
        // own wording is the part a caller can act on.
        FString ErrorSummary(int32 MaxDistinct = 4) const
        {
            const UGeometryScriptDebug* Ptr = Debug.Get();
            if (!Ptr)
            {
                return FString();
            }

            // Insertion-ordered, so the summary reads in the order the engine complained rather
            // than in TMap hash order - two runs over the same input must produce the same text.
            TArray<FString> Distinct;
            TArray<int32> Counts;
            for (const FGeometryScriptDebugMessage& Message : Ptr->Messages)
            {
                if (Message.MessageType != EGeometryScriptDebugMessageType::ErrorMessage)
                {
                    continue;
                }
                const FString Text = Message.Message.ToString();
                const int32 Existing = Distinct.Find(Text);
                if (Existing != INDEX_NONE)
                {
                    ++Counts[Existing];
                }
                else
                {
                    Distinct.Add(Text);
                    Counts.Add(1);
                }
            }

            TArray<FString> Lines;
            const int32 Shown = FMath::Min(Distinct.Num(), FMath::Max(MaxDistinct, 1));
            for (int32 Index = 0; Index < Shown; ++Index)
            {
                Lines.Add(Counts[Index] > 1
                    ? FString::Printf(TEXT("%s (x%d)"), *Distinct[Index], Counts[Index])
                    : Distinct[Index]);
            }
            if (Distinct.Num() > Shown)
            {
                Lines.Add(FString::Printf(TEXT("(and %d more distinct error(s))"),
                    Distinct.Num() - Shown));
            }
            return FString::Join(Lines, TEXT("; "));
        }

        // Engine warnings become FOpResult warnings, so they reach an RPC caller through the
        // `warnings` array (GeometryOpWarnings.h) and a .pwmodel author through
        // PWMODEL_STAGE_WARNING - the same route this module's own clamp warnings take.
        //
        // Forwarding is the default and every message is drained verbatim, with the one
        // exception below: a site that wants to swallow a warning has to say so.
        //
        // This comment used to read "No GeometryScript function this module calls emits a
        // WarningMessage on UE 5.8, so this changes no response shape today." That was measured
        // false. RecomputeNormals emits one whenever the target's normal overlay is empty
        // (MeshNormalsFunctions.cpp:214, `PrimaryNormals()->ElementCount() == 0`): it falls back
        // to per-vertex normals and says so. Every part whose geometry came from buffers
        // carrying no normals reaches it, which is the ordinary case for generated geometry, so
        // it is a warning .pwmodel authors see rather than a hypothetical one.
        //
        // THE ONE REWRITE. That warning's remedy clause names two GeometryScript BLUEPRINT NODE
        // titles - "Set Mesh To Per Vertex Normals" and "Compute Split Normals" - and neither is
        // a verb of this plugin on either front-end: `split_normals` wraps the second, and the
        // first has no spelling at all. An author who followed the sentence spent a compile
        // reaching PWSRC_UNKNOWN_OP. Only the remedy is replaced; the diagnosis in front of it
        // is the engine's and stays verbatim, because that half is what says what happened.
        //
        // Naming `split_normals` from a layer that knows nothing about its callers is safe only
        // because both front-ends spell it the same way - `geometry.split_normals` on the wire,
        // `split_normals` in a document. Anything that diverges per surface must NOT be named
        // here; it belongs in the compiler's translation layer (PwModelWarningNames), which
        // rewrites leading parameter tokens and by construction cannot reach prose like this.
        //
        // Matched on the engine's whole sentence, so a reworded engine message falls through
        // unrewritten. Forwarding text we no longer recognise is the safe direction.
        void DrainWarningsInto(FOpResult& Result) const
        {
            const UGeometryScriptDebug* Ptr = Debug.Get();
            if (!Ptr)
            {
                return;
            }
            for (const FGeometryScriptDebugMessage& Message : Ptr->Messages)
            {
                if (Message.MessageType == EGeometryScriptDebugMessageType::WarningMessage)
                {
                    FString Text = Message.Message.ToString();
                    Text.ReplaceInline(
                        TEXT("Consider using 'Set Mesh To Per Vertex Normals' or "
                             "'Compute Split Normals' instead."),
                        TEXT("This is the expected result for a mesh built without normals and "
                             "needs no fix; run split_normals if you want hard edges."),
                        ESearchCase::CaseSensitive);
                    Result.Warnings.Add(MoveTemp(Text));
                }
            }
        }

    private:
        TStrongObjectPtr<UGeometryScriptDebug> Debug;
    };
}
