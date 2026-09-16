// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwModelParser.h - Document parser, op table and semantic validation for .pwmodel.
//
// Produces an FPwModelDocument from source text and reports every problem it can see
// without a mesh. Everything that needs geometry - a hull body that turns out empty, an
// op the engine rejects, a boolean that changed nothing - belongs to the compiler; the
// parser's contract is that a document it accepts is structurally and semantically
// well-formed, so a later diagnostic is about geometry rather than about spelling.
//
// The op table below is the format's single vocabulary. `model.describe_ops` emits it
// verbatim and the parser validates against it, so the published vocabulary cannot drift
// from the one that actually compiles - which is the reason docs/pwmodel-format.md
// deliberately carries no per-op parameter list.
#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Containers/StringView.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelDiagnostic.h"
#include "PwSource/PwParamSpec.h"

// Where an op is legal. `box`, `sphere` and `capsule` exist in both contexts with
// different parameters, so the context is part of an op's identity rather than a
// post-hoc check.
enum class EPwModelOpContext : uint8
{
    Part,
    Collision,
    Skin
};

// What `from=` / `to=` supplies to a generator BESIDES placement and direction: which of the op's
// own parameters carries the primitive's extent along its own local +Z, and how the endpoint
// distance maps onto it.
//
// It is per-op rather than a rule, because the parameter genuinely differs: a cylinder spells the
// extent `height`, a capsule spells the CYLINDRICAL PART of it `length` and adds a hemisphere at
// each end, and a box buries it in the third component of `size`. An op with no local-Z extent at
// all - a sphere, a torus, a plane - is `None`, and aiming one is still meaningful (it turns the
// primitive) while the distance is not.
enum class EPwModelAimExtent : uint8
{
    // No parameter of this op is an extent along local Z. `from`/`to` place and aim it; the
    // distance between the endpoints is unused, and the parser says so rather than letting an
    // author believe a `plane` was stretched to reach `to`.
    None,

    // The named parameter IS the extent, in full. cylinder / cone / pipe `height`.
    Scalar,

    // The extent is the Z component of a Vector3 `size`; X and Y stay the author's.
    SizeZ,

    // The named parameter is the extent MINUS the two hemispherical caps, which is what the
    // engine's capsule takes (GeometryOps_Primitives.h FCapsuleParams). Aiming one therefore
    // needs `length = distance - 2 * radius`, and a distance no greater than `2 * radius` names
    // no capsule at all. Spelled out as its own kind because deriving it as though it were
    // `Scalar` overshoots `to` by exactly one radius at each end - silently, on the one primitive
    // an author reaches for most when aiming at bones.
    CapsuleLength,
};

enum class EPwModelParamType : uint8
{
    Number,       // 2.5
    Integer,      // 3 - a Number token that must have no fractional part
    Bool,         // the bare identifiers true / false
    String,       // "…"
    Enum,         // a bare identifier drawn from FPwModelParamSpec::AllowedValues
    Vector2,      // (u, v)
    Vector3,      // (x, y, z)
    Vector4,      // (r, g, b, a)
    NumberList,   // (a, b, c, …) of any arity - an id list, not a fixed-arity vector
    PointList2,   // [(u, v), …]
    PointList3,   // [(x, y, z), …]
    PointList4,   // [(r, g, b, a), …]

    // [(x, y, z, roll, pitch, yaw), …] - a list of position+rotation FRAMES, degrees on the last
    // three, read through PwValueRead::MakeRotator so it cannot disagree with `rotate=`.
    //
    // Named for what an entry MEANS rather than for its arity. `PointList6` would have fitted the
    // PointListN idiom and told an author nothing: an entry is not a point in six dimensions, its
    // halves carry different units, and "expects 6 components" leaves the ordering to be guessed.
    // A general list-of-N-tuples was the other candidate and is worse here for a concrete reason:
    // PwModelParamTypeToString is the ONE spelling an author sees in both the diagnostic and
    // model.describe_ops, and a parameterized type has no name to print - the arity would have to
    // travel as a separate published field that every consumer, including the describe_ops
    // round-trip test, would need taught. One more named type costs one enum entry; the general
    // form costs a field on every spec and a second thing that can drift.
    FrameList,

    // [(order, amplitude, phase), …] - a list of AZIMUTHAL HARMONIC TERMS. Named for what an
    // entry means, on the FrameList reasoning above: the three components carry different units
    // (cycles per revolution, a fraction or a distance, degrees) and "expects 3 components"
    // would leave the ordering to be guessed. It is also the only list type whose FIRST
    // component has a domain of its own - an order must be integral and 1 or more - and that
    // check is keyed on the TYPE here rather than on the op's name, so a second op taking
    // harmonics gets it by declaring the parameter.
    HarmonicList
};

// Wire name for the type, shared by diagnostics and model.describe_ops so an author sees
// one spelling in the error and in the vocabulary dump.
const TCHAR* PwModelParamTypeToString(EPwModelParamType Type);

const TCHAR* PwModelOpContextToString(EPwModelOpContext Context);

// Wire name for the aim kind, published by model.describe_ops beside `aimExtentParam` so an
// agent can derive what `from`/`to` will do to an op without parsing its prose.
const TCHAR* PwModelAimExtentToString(EPwModelAimExtent Extent);

struct FPwModelParamSpec
{
    FString Name;
    EPwModelParamType Type = EPwModelParamType::Number;
    bool bRequired = false;

    // Display text for the default, empty when the parameter is required or when the
    // value is whatever the engine call already defaults to. Display only - the compiler
    // reads defaults from the F<Verb>Params structs, which are the real source.
    FString Default;

    FString Description;

    // Enum only.
    TArray<FString> AllowedValues;

    // Inclusive numeric domain for Number / Integer, checked by the parser and publishable by
    // model.describe_ops. Table-driven rather than a name check: the UV channel range used to
    // be gated on a hardcoded uv / set_uvs / transform_uvs triple, so a later op carrying a
    // channel would have silently lost the check.
    bool bHasRange = false;
    double MinValue = 0.0;
    double MaxValue = 0.0;

    // ---- The CLAMPED domain, which is not the range above and must never become it ----------
    //
    // Two different things read alike here, and conflating them breaks one of them. The pair
    // above is a REFUSED domain: outside it the value is rejected before the op runs. The pair
    // below is a CLAMPED domain: outside it the value is ACCEPTED, moved to the nearest legal
    // value, and a warning says so.
    //
    // Declaring a clamped count's domain as an enforced range would turn every clamp into a
    // refusal AND destroy the second tier these ops document, where a value of 0 or less means
    // "unset" and takes the verb's own default - a legal, useful thing to write that is outside
    // the clamped domain by construction. So this is ADVISORY METADATA, published and never
    // enforced. Nothing in the parser reads it to decide anything.
    //
    // Values come from GeometryOps::ClampDomains, whose own test RUNS each op at its floor, one
    // below it, at its ceiling, one above it and at zero, and fails when the declared row stops
    // matching. Stamped onto this table by PwModelOpTable rather than re-typed here.
    bool bHasClampDomain = false;

    // Inclusive floor an author meets at the op's OWN default sweep, and the floor when the
    // sweep closes. Equal on every parameter whose op has no angle at all; they differ only
    // where the floor genuinely depends on the sweep, because a revolve that closes wraps its
    // last section onto its first and needs one more than a partial one does.
    int32 ClampMin = 0;
    int32 ClampMinClosedSweep = 0;

    // Inclusive ceiling.
    int32 ClampMax = 0;

    // The second tier: a value of 0 or less is not raised to the floor, it reads as "unset" and
    // takes ClampUnsetDefault. False where 0 is an ordinary legal value.
    bool bClampZeroMeansUnset = false;
    int32 ClampUnsetDefault = 0;
};

struct FPwModelOpSpec
{
    FString Name;
    EPwModelOpContext Context = EPwModelOpContext::Part;
    FString Description;

    // May legally be a part's - or a nested block's - first op, because it creates
    // geometry rather than modifying it. PWMODEL_PART_NEEDS_PRIMITIVE derives from this.
    bool bGenerator = false;

    // Takes a `{ … }` block. Every op that accepts one also requires one: a boolean with
    // no tool mesh and a `hull` with no body are both meaningless, so both spellings of
    // the mistake report PWSRC_BAD_BLOCK.
    bool bAcceptsBlock = false;

    // Accepts material=, the model-wide slot tag. True on the geometry-producing ops only, and
    // set with the parameter itself by AcceptMaterialSlot so the two cannot drift: the parser's
    // unknown-param gate reads the param list while model.describe_ops publishes this flag.
    //
    // NOT a claim about color=. The shape primitives take both, `append_buffers` takes only the
    // slot tag - a second scalar color would overwrite its per-vertex `colors=` buffer - and
    // `append_triangle` takes neither.
    bool bAcceptsMaterial = false;

    // This op writes the material IDs of its own output and must NOT be retagged afterwards by
    // the compiler's generic append-and-retag path (PwModelCompiler.cpp RunOp). `bevel` is the
    // one: the engine takes each new face's material from the two faces either side of the
    // bevelled edge, which is per-edge correct on a multi-slot part and strictly better than the
    // one-slot-for-everything answer the generic path can give. A flag rather than a name test,
    // for the same reason bGenerator and bBoolean are flags - an op given `material=` and not the
    // matching dispatch, or the reverse, is exactly the silent half-wiring that costs a render to
    // find. Meaningless without bAcceptsMaterial.
    bool bSelfTagsMaterial = false;

    // A boolean op. Kept separate from bAcceptsMaterial because the two answer different
    // questions: bAcceptsMaterial says the op takes the slot tag (booleans now do - it names the
    // faces the operation CREATES), while this says the op consumes a block and rewrites the mesh,
    // which is what selects RunBoolean and what makes `color=` PWMODEL_MATERIAL_ON_BOOLEAN. The
    // rejection stays table-driven rather than a name check.
    bool bBoolean = false;

    // How this op answers `from=` / `to=`. See EPwModelAimExtent. AimExtentParam names the
    // parameter the distance is written into and is empty when AimExtent is None; the two are set
    // together at registration so the published `from` description, the parser's conflict check
    // and the compiler's derivation all read one fact.
    EPwModelAimExtent AimExtent = EPwModelAimExtent::None;
    FString AimExtentParam;

    TArray<FPwModelParamSpec> Params;

    const FPwModelParamSpec* FindParam(const FString& ParamName) const;
};

namespace PwModelOpTable
{
    // Built once on first call. The whole vocabulary, both contexts.
    const TArray<FPwModelOpSpec>& Get();

    const FPwModelOpSpec* Find(const FString& OpName, EPwModelOpContext Context);

    // Any-context lookup, used only to tell an author that `hull` exists but not here.
    const FPwModelOpSpec* FindInAnyContext(const FString& OpName);

    TArray<FString> NamesInContext(EPwModelOpContext Context);

    // The model-level `key=value` lists that are not part or collision ops. They live in the
    // op table for the
    // same reason the ops do - one vocabulary, validated and published from one place - and are
    // reached by name rather than through Get() because Get() is what model.describe_ops emits,
    // and every consumer of that dump (including the round-trip test in TestModelHandlers.cpp)
    // builds its document assuming an entry is legal either inside a part or inside collision.
    // Publishing these needs that assumption widened first.
    TArrayView<const FPwModelParamSpec> UVLayoutParams();
    TArrayView<const FPwModelParamSpec> LightmapParams();
    TArrayView<const FPwModelParamSpec> PartHeaderParams();

}

// The `.pwmodel` spelling of an ops-layer warning label.
//
// GeometryOps labels its clamp-and-warn calls with the RPC's PUBLISHED parameter name -
// `heightSteps`, `numSteps`, `widthSegments` - because that is the name a `geometry.*` caller
// passed and greps the response for. `.pwmodel` publishes the same values under other names
// (`height_steps`, `num_steps`, and a single Vector3 `segments` where the RPC has three scalars),
// so the compiler re-emitting that text verbatim named parameters the document surface does not
// have: an author who wrote `segments=(6, 5, 4)` was told about `widthSegments`.
//
// ONE LABEL CANNOT SERVE BOTH SURFACES, so neither surface owns it. The ops layer keeps the RPC
// spelling and stays ignorant of who called it - no mode flag on the params, no thread-local -
// and the `.pwmodel` front-end translates on re-emission (`FCompiler::ReportOpResult`). The table
// lives here because the op table is what makes it CHECKABLE: every `ModelLabel` below resolves
// to a declared parameter of that op, which TestPwModelWarningNames asserts against
// `PwModelOpTable` rather than against a second hand-written list.
//
// Adding a clamp label to GeometryOps that `.pwmodel` spells differently means adding a row here.
// The test sweeps every generator's own warnings and fails on any leading token that is not a
// declared `.pwmodel` parameter, so a missing row is caught rather than shipped.
namespace PwModelWarningNames
{
    struct FEntry
    {
        // `.pwmodel` op name, Part context. Collision elements never reach this table: their
        // warnings are built in PwModelCollision.cpp and already carry document spellings.
        const TCHAR* OpName;

        // The label GeometryOps writes, i.e. the RPC parameter name.
        const TCHAR* RpcLabel;

        // What the document calls the same value. `size.x` / `segments.y` where the RPC splits
        // one `.pwmodel` vector into per-axis scalars - the base name is what an author greps
        // for and the component says which of the three moved, which a bare `size` would lose.
        const TCHAR* ModelLabel;
    };

    TArrayView<const FEntry> Entries();

    // Rewrites Warning's leading parameter name when OpName spells it differently. Warnings that
    // do not open with a mapped label come back byte-identical - every prose warning, and every
    // op whose two surfaces already agree (`sphere subdivisions`, `cylinder segments`,
    // `revolve steps`, `spherify factor`, …), which is most of them.
    FString Translate(const FString& OpName, const FString& Warning);

    // Translate, plus a WHOLE-WORD rewrite of every mapped label left anywhere else in the text.
    // For FOpResult::ErrorMessage, where the parameter name is embedded in a sentence rather
    // than leading it: GeneratePipe refuses with "pipe requires 0 < innerRadius < outerRadius",
    // and PWMODEL_OP_FAILED forwarded that verbatim while the warning beside it was translated -
    // so a failure named parameters `.pwmodel` does not have and a warning did not.
    //
    // THE TWO RULES ARE DELIBERATELY DIFFERENT, and the difference is the prose risk. A looser
    // rule can rewrite a mapped word where it is ENGLISH rather than a parameter, and three rows
    // (`box` width / height / depth) are plain English words. That is why warnings keep the
    // strict rule - a prose warning like `mesh width clamped from 1 to 2` must survive intact,
    // which TestPwModelWarningNames pins - and why the loose one is confined to failures, where
    // the exposure is bounded: GenerateBox has no failure path of its own at all (it clamps
    // everything and cannot refuse), so no `box` message can reach this with `width` as prose.
    // A new GeometryOps failure whose text uses a mapped row's word as English would need that
    // row's op checked here before the message ships.
    FString TranslateMessage(const FString& OpName, const FString& Message);
}

class FPwModelParser
{
public:
    // Tokenizes and parses Source into OutDocument. OutDocument and OutDiagnostics are
    // reset first. Lexical diagnostics from FPwTokenizer are forwarded unchanged.
    //
    // Returns false when any Error-severity diagnostic was produced; warnings do not fail
    // the parse. A false return still leaves OutDocument populated as far as the parser
    // got, because reporting several errors per run is worth more to an author than a
    // clean abort - but a caller must not compile a document that parsed false.
    static bool Parse(FStringView Source, FPwModelDocument& OutDocument,
                      TArray<FPwDiagnostic>& OutDiagnostics);
};
