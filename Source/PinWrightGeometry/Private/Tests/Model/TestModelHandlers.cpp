// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests for the `model` RPC namespace, driven through the real dispatcher.
// Reverting any of them regresses something specific:
//
//  - model.compile refusing inline `text` is the mechanism that keeps every generated asset
//    traceable to a file on disk. Accept text once and the asset has no recoverable source
//    and an empty provenance stamp that matches every other empty one - the exact failure
//    the format exists to prevent, reproduced inside the tool meant to prevent it. The test
//    asserts the refusal AND that the message names model.validate: a refusal with nowhere
//    to go just trains callers to work around it.
//  - The dispatcher rejects any undeclared parameter before a handler runs, so `text` is
//    declared on model.compile purely to be refused with that explanation. Drop the
//    declaration and the steering message becomes unreachable - silently, because the
//    request still fails, just uselessly.
//  - model.validate creating nothing is what makes it safe to iterate on a document that
//    does not exist as a file yet.
//  - model.describe_ops is the vocabulary an authoring agent reads before writing its first
//    document. Two tests build a document for EVERY entry the verb reports - one for each op,
//    one for each of the two parameter sets that are not ops - using only the reported name,
//    context, block flag, parameter types and numeric ranges, and require the parser to accept
//    it. That is the anti-drift property in one assertion: describe_ops cannot advertise an op,
//    a parameter, a type, a requiredness or a range that would not compile. Both also compare
//    the published parameter list against the parser list it came from, because a round-trip
//    only sees what was published: a field the spec carries and the wire drops - the state the
//    0-7 channel range shipped in - parses perfectly and is a hole in the surface the docs
//    delegate to. An entry in a context neither test can place in a document is an ERROR in
//    both, never a skip; teach the test the new context rather than letting the entry ship
//    unchecked.
//  - The provenance stamp is written project-relative when the source lies under the project
//    directory and absolute otherwise (the contract UPwModelAssetUserData documents in
//    GeometryAssetCreate.h). Stamps are compared by plain string equality, so an
//    absolute-always stamp refuses a colleague's recompile of their own source from a
//    checkout at a different root, leaving overwrite=true as the only way through - which
//    retires the one signal that separates iteration from a destructive overwrite. The two
//    stamp tests drive the real handler and read the stamp off the created asset, because
//    every test that seeded FPwModelCompileOptions::SourcePath by hand passed while the only
//    production caller violated the contract.
//  - A failed compile creates no asset, so its response must not carry pendingFlush: that
//    field tells the caller an editor.save_all is needed to land a .uasset that, here, was
//    never created.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/Geometry/GeometryAssetCreate.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Utils/AssetSaveState.h"
#include "Utils/HttpResponseSpill.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include "Model/PwModelAst.h"
#include "Model/PwModelDiagnostic.h"
#include "Model/PwModelParser.h"

#include "Engine/StaticMesh.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

using DispatcherTestHelpers::MakeDispatcher;
using DispatcherTestHelpers::Dispatch;
using DispatcherTestHelpers::FSinkPtr;

namespace
{
// Uniquely prefixed: this module builds with bUseUnity = true, so an anonymous-namespace
// helper with a common name would collide with a sibling test TU once Unity merges them.

const TCHAR* ModelHandlersTest_MinimalDocument =
    TEXT("pwmodel 0\n")
    TEXT("part body {\n")
    TEXT("    box size=(10, 10, 10)\n")
    TEXT("}\n");

// Fails in the parser, so the compile never reaches asset creation - which is exactly the
// state in which the response must not claim a save is pending.
const TCHAR* ModelHandlersTest_UnparsableDocument =
    TEXT("pwmodel 0\n")
    TEXT("part body {\n")
    TEXT("    nosuchop size=(10, 10, 10)\n")
    TEXT("}\n");

TSharedPtr<FJsonObject> ModelHandlersTest_Payload()
{
    return MakeShared<FJsonObject>();
}

// The no-argument response is an index. Tests that need the complete parser metadata follow the
// same contract as an author: discover a name first, then query that name explicitly.
bool ModelHandlersTest_QueryDescribeOps(FRpcDispatcher& Dispatcher, FSinkPtr& Sink,
                                        const FString& RequestId, const FString& Op,
                                        TSharedPtr<FJsonObject>& OutResult, FString& OutErrorCode)
{
    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    if (!Op.IsEmpty())
    {
        Payload->SetStringField(TEXT("op"), Op);
    }

    bool bSuccess = false;
    Dispatch(Dispatcher, Sink, TEXT("model.describe_ops"), RequestId, Payload,
        bSuccess, OutResult, OutErrorCode);
    return bSuccess && OutResult.IsValid();
}

// Failure messages carry the response's diagnostics; "expected true, got false" on a
// compiler test costs a rerun under a debugger to learn anything.
FString ModelHandlersTest_DescribeResult(const TSharedPtr<FJsonObject>& Result)
{
    if (!Result.IsValid())
    {
        return TEXT("<no result>");
    }

    const TArray<TSharedPtr<FJsonValue>>* Diagnostics = nullptr;
    if (!Result->TryGetArrayField(TEXT("diagnostics"), Diagnostics) || !Diagnostics)
    {
        return TEXT("<no diagnostics field>");
    }

    TArray<FString> Lines;
    for (const TSharedPtr<FJsonValue>& Value : *Diagnostics)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Entry) || !(*Entry).IsValid())
        {
            continue;
        }
        Lines.Add(FString::Printf(TEXT("%s [%s] line %d: %s"),
            *(*Entry)->GetStringField(TEXT("severity")),
            *(*Entry)->GetStringField(TEXT("code")),
            static_cast<int32>((*Entry)->GetNumberField(TEXT("line"))),
            *(*Entry)->GetStringField(TEXT("message"))));
    }
    return Lines.Num() > 0 ? FString::Join(Lines, TEXT("; ")) : TEXT("<no diagnostics>");
}

// A parse-legal literal for each type model.describe_ops can report. Deliberately driven off
// the reported type STRING rather than EPwModelParamType, so a type renamed on the wire
// without updating this switch fails loudly instead of silently skipping the op.
//
// A reported numeric range picks the literal for the bounded types: the sample is the upper
// bound, so a published domain wider than the parser's own is rejected here rather than
// steering an author into `channel=8`. Min/Max are ignored when bHasRange is false.
FString ModelHandlersTest_SampleValue(const FString& Type, const TArray<FString>& AllowedValues,
                                      bool bHasRange, double Min, double Max)
{
    if (bHasRange && (Type == TEXT("number") || Type == TEXT("integer")))
    {
        // int64 and SanitizeFloat rather than FromInt and %g: a bound wider than int32 must
        // not wrap into a literal inside the domain, and %g would spell a large one as
        // `1e+06`, which the tokenizer does not read as a number.
        return Type == TEXT("integer")
            ? FString::Printf(TEXT("%lld"), static_cast<int64>(FMath::RoundToDouble(Max)))
            : FString::SanitizeFloat(Max);
    }

    if (Type == TEXT("number"))      { return TEXT("1"); }
    if (Type == TEXT("integer"))     { return TEXT("1"); }
    if (Type == TEXT("boolean"))     { return TEXT("true"); }
    if (Type == TEXT("string"))      { return TEXT("\"Shell\""); }
    if (Type == TEXT("enum"))        { return AllowedValues.Num() > 0 ? AllowedValues[0] : FString(); }
    if (Type == TEXT("vector2"))     { return TEXT("(1, 1)"); }
    if (Type == TEXT("vector3"))     { return TEXT("(1, 1, 1)"); }
    if (Type == TEXT("vector4"))     { return TEXT("(1, 1, 1, 1)"); }
    if (Type == TEXT("number_list")) { return TEXT("(0, 1)"); }
    if (Type == TEXT("point_list2")) { return TEXT("[(0, 0), (10, 0), (10, 10), (0, 10)]"); }
    if (Type == TEXT("point_list3")) { return TEXT("[(0, 0, 0), (10, 0, 0), (0, 10, 0), (0, 0, 10)]"); }
    if (Type == TEXT("point_list4")) { return TEXT("[(1, 1, 1, 1), (1, 1, 1, 1), (1, 1, 1, 1), (1, 1, 1, 1)]"); }
    // Three frames, not one: every op that takes a frame_list needs at least two to describe a
    // path, and the synthesized document is parsed as an author would have written it.
    if (Type == TEXT("frame_list"))  { return TEXT("[(0, 0, 0, 0, 0, 0), (0, 0, 50, 0, 0, 0), (0, 0, 100, 0, 0, 0)]"); }
    // Two terms, not one, so the sample exercises the SUM rather than a single wave - which is
    // the whole shape of the feature. Both components inside their domains: the order is whole
    // and at least 1, and the amplitudes total 0.31, under the radial ceiling of 1.
    if (Type == TEXT("harmonic_list")) { return TEXT("[(3, 0.2, 0), (5, 0.11, 90)]"); }
    return FString();
}

TArray<FString> ModelHandlersTest_StringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
    TArray<FString> Out;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    if (Object.IsValid() && Object->TryGetArrayField(Field, Values) && Values)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            FString Text;
            if (Value.IsValid() && Value->TryGetString(Text))
            {
                Out.Add(Text);
            }
        }
    }
    return Out;
}

// The literal for one reported parameter, read entirely off that parameter's own JSON - type,
// allowed values and the optional min / max pair - so nothing the verb publishes is consumed
// from a second, hand-kept copy.
FString ModelHandlersTest_SampleForParam(const TSharedPtr<FJsonObject>& Param)
{
    double Min = 0.0;
    double Max = 0.0;
    const bool bHasMin = Param->TryGetNumberField(TEXT("min"), Min);
    const bool bHasMax = Param->TryGetNumberField(TEXT("max"), Max);

    return ModelHandlersTest_SampleValue(
        Param->GetStringField(TEXT("type")),
        ModelHandlersTest_StringArray(Param, TEXT("allowedValues")),
        bHasMin && bHasMax, Min, Max);
}

// One published parameter against the parser spec it was generated from. This is the
// serialization half of the anti-drift property - it catches a field the spec carries and the
// wire drops, which is how the parser enforced `channel` 0-7 while the vocabulary surface
// published no bound at all. The round-trip parse below is the other half; neither substitutes
// for the other.
void ModelHandlersTest_CheckParamMatchesSpec(FAutomationTestBase& Test, const FString& Owner,
                                             const TSharedPtr<FJsonObject>& Published,
                                             const FPwModelParamSpec& Spec)
{
    const FString Where = FString::Printf(TEXT("%s.%s"), *Owner, *Spec.Name);

    Test.TestEqual(*FString::Printf(TEXT("%s publishes the spec's type"), *Where),
        Published->GetStringField(TEXT("type")), FString(PwModelParamTypeToString(Spec.Type)));
    // Presence separately from value: GetBoolField answers false for a field that is not there,
    // so `required` dropped from the wire reads as "optional" and matches every optional
    // parameter silently - and an author told a required parameter is optional writes a document
    // the parser refuses with PWSRC_MISSING_PARAM.
    Test.TestTrue(*FString::Printf(TEXT("%s publishes 'required' at all"), *Where),
        Published->HasField(TEXT("required")));
    Test.TestTrue(*FString::Printf(TEXT("%s publishes the spec's requiredness"), *Where),
        Published->GetBoolField(TEXT("required")) == Spec.bRequired);

    double Min = 0.0;
    double Max = 0.0;
    const bool bHasMin = Published->TryGetNumberField(TEXT("min"), Min);
    const bool bHasMax = Published->TryGetNumberField(TEXT("max"), Max);

    Test.TestTrue(*FString::Printf(
        TEXT("%s publishes min/max exactly when the spec is ranged (spec ranged=%d, min=%d, max=%d)"),
        *Where, Spec.bHasRange ? 1 : 0, bHasMin ? 1 : 0, bHasMax ? 1 : 0),
        bHasMin == Spec.bHasRange && bHasMax == Spec.bHasRange);

    if (Spec.bHasRange && bHasMin && bHasMax)
    {
        Test.TestEqual(*FString::Printf(TEXT("%s publishes the spec's min"), *Where), Min, Spec.MinValue);
        Test.TestEqual(*FString::Printf(TEXT("%s publishes the spec's max"), *Where), Max, Spec.MaxValue);
    }

    // The WHOLE allowed-values list. The round-trip writes AllowedValues[0] and nothing else, so
    // a list published truncated - or emptied for a parameter the parser still restricts - steers
    // an author away from spellings that compile while every other check here passes. `auto
    // method=` publishes PwModelCollisionNames::AutoMethodNames(), which exists precisely so the
    // accepted set is not retyped anywhere; dropping it on the wire re-creates the retyped copy
    // as an absence.
    const TArray<FString> PublishedAllowed = ModelHandlersTest_StringArray(Published, TEXT("allowedValues"));
    Test.TestEqual(*FString::Printf(TEXT("%s publishes every value the parser accepts"), *Where),
        PublishedAllowed.Num(), Spec.AllowedValues.Num());
    for (const FString& Allowed : Spec.AllowedValues)
    {
        Test.TestTrue(*FString::Printf(TEXT("%s publishes the allowed value '%s'"), *Where, *Allowed),
            PublishedAllowed.Contains(Allowed));
    }

    // The two display-only fields, for the same reason the range is checked: they are what the
    // docs delegate to, they are copied verbatim from the spec, and nothing else on the wire
    // would notice them going missing. A parameter whose published default is blank reads as
    // "no default" to the author it was written for.
    Test.TestEqual(*FString::Printf(TEXT("%s publishes the spec's default"), *Where),
        Published->GetStringField(TEXT("default")), Spec.Default);
    Test.TestEqual(*FString::Printf(TEXT("%s publishes the spec's description"), *Where),
        Published->GetStringField(TEXT("description")), Spec.Description);
}

// Every parameter of one published entry against the parser list behind it, matched by name so
// a reordering is not a failure but a dropped or invented parameter is. Guards the `paramSets`
// block the way the op table's own round-trip guards `ops`: a parameter the parser gains and
// this verb does not publish is a hole nothing else fills, because the docs point here.
void ModelHandlersTest_CheckParamsMatchSpecs(FAutomationTestBase& Test, const FString& Owner,
                                             const TSharedPtr<FJsonObject>& Entry,
                                             TArrayView<const FPwModelParamSpec> Specs)
{
    const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
    if (!Entry->TryGetArrayField(TEXT("params"), Params) || !Params)
    {
        Test.AddError(FString::Printf(TEXT("'%s' carries no params array"), *Owner));
        return;
    }

    Test.TestEqual(*FString::Printf(TEXT("'%s' publishes every parameter the parser validates"), *Owner),
        Params->Num(), Specs.Num());

    for (const FPwModelParamSpec& Spec : Specs)
    {
        const TSharedPtr<FJsonObject>* Match = nullptr;
        for (const TSharedPtr<FJsonValue>& Value : *Params)
        {
            const TSharedPtr<FJsonObject>* Candidate = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Candidate) && (*Candidate).IsValid()
                && (*Candidate)->GetStringField(TEXT("name")) == Spec.Name)
            {
                Match = Candidate;
                break;
            }
        }

        if (!Match)
        {
            Test.AddError(FString::Printf(
                TEXT("the parser validates '%s' against parameter '%s', which model.describe_ops does not publish"),
                *Owner, *Spec.Name));
            continue;
        }

        ModelHandlersTest_CheckParamMatchesSpec(Test, Owner, *Match, Spec);
    }
}

// The four op-level booleans against the spec they were generated from. `params` has its own
// comparison above; these had none, and three of the four are invisible to the round-trip
// below, so the round-trip is not the check for them:
//
//  - `generator` only decides whether the synthesized document gets a primitive in FRONT of the
//    op. Dropped from the wire it reads as false, every document gains a leading `box`, and
//    every one of them still parses. Flipped the other way it produces
//    PWMODEL_PART_NEEDS_PRIMITIVE, which ModelHandlersTest_IsVocabularyMismatch deliberately
//    does not count as drift. Either direction is silent - the same shape as the 0-7 channel
//    range the parser enforced and the wire published nowhere.
//  - `acceptsMaterial` and `boolean` are never read by either round-trip at all.
//  - `acceptsBlock` is the one the parse does pin, through PWSRC_BAD_BLOCK in both directions
//    (PwModelParser.cpp:1417 and :1422); it is checked here anyway so the four read as one rule.
void ModelHandlersTest_CheckOpFlagsMatchSpec(FAutomationTestBase& Test, const FString& Where,
                                             const TSharedPtr<FJsonObject>& Published,
                                             const FPwModelOpSpec& Spec)
{
    auto CheckFlag = [&Test, &Where, &Published](const TCHAR* Field, bool bExpected)
    {
        // Presence as well as value: GetBoolField answers false for a field that is not there,
        // so a dropped flag would silently match every spec whose flag is false.
        Test.TestTrue(*FString::Printf(TEXT("%s publishes '%s' at all"), *Where, Field),
            Published->HasField(Field));
        Test.TestTrue(*FString::Printf(TEXT("%s publishes the spec's '%s' (expected %d)"),
            *Where, Field, bExpected ? 1 : 0),
            Published->GetBoolField(Field) == bExpected);
    };

    CheckFlag(TEXT("generator"), Spec.bGenerator);
    CheckFlag(TEXT("acceptsBlock"), Spec.bAcceptsBlock);
    CheckFlag(TEXT("acceptsMaterial"), Spec.bAcceptsMaterial);
    CheckFlag(TEXT("boolean"), Spec.bBoolean);

    Test.TestEqual(*FString::Printf(TEXT("%s publishes the spec's description"), *Where),
        Published->GetStringField(TEXT("description")), Spec.Description);
}

// Builds the smallest document that exercises one reported op, from the reported metadata
// alone. Returns an empty string and fills OutError when the metadata cannot be honoured -
// which is itself a describe_ops defect, not a test limitation.
FString ModelHandlersTest_BuildDocumentForOp(const TSharedPtr<FJsonObject>& Op, FString& OutError)
{
    const FString Name = Op->GetStringField(TEXT("name"));
    const FString Context = Op->GetStringField(TEXT("context"));
    const bool bGenerator = Op->GetBoolField(TEXT("generator"));
    const bool bAcceptsBlock = Op->GetBoolField(TEXT("acceptsBlock"));

    FString Statement = Name;

    const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
    if (Op->TryGetArrayField(TEXT("params"), Params) && Params)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Params)
        {
            const TSharedPtr<FJsonObject>* Param = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(Param) || !(*Param).IsValid())
            {
                continue;
            }
            if (!(*Param)->GetBoolField(TEXT("required")))
            {
                continue;
            }

            const FString ParamName = (*Param)->GetStringField(TEXT("name"));
            const FString ParamType = (*Param)->GetStringField(TEXT("type"));
            const FString Sample = ModelHandlersTest_SampleForParam(*Param);

            if (Sample.IsEmpty())
            {
                OutError = FString::Printf(
                    TEXT("op '%s' reports required param '%s' with type '%s', for which no literal exists"),
                    *Name, *ParamName, *ParamType);
                return FString();
            }

            Statement += FString::Printf(TEXT(" %s=%s"), *ParamName, *Sample);
        }
    }

    if (bAcceptsBlock)
    {
        Statement += TEXT(" {\n        box size=(50, 50, 50)\n    }");
    }

    if (Context == TEXT("collision"))
    {
        return FString::Printf(
            TEXT("pwmodel 0\npart body {\n    box size=(100, 100, 100)\n}\ncollision {\n    %s\n}\n"),
            *Statement);
    }

    if (Context == TEXT("skin"))
    {
        return FString::Printf(
            TEXT("pwmodel 0\npart body {\n    box size=(100, 100, 100)\n}\nskin {\n    %s\n}\n"),
            *Statement);
    }

    // Stated rather than assumed. Every `ops` entry is synthesised as a statement inside a part
    // or a collision/skin block, so a context that is neither cannot be checked here at all - it must
    // fail loudly and be taught, never be swept into the part branch where it would be reported
    // as an unknown op and read as a parser defect. The two parameter sets that are not ops are
    // published in their own `paramSets` block for exactly this reason and have their own
    // round-trip; a THIRD kind of context arriving in `ops` lands here.
    if (Context != TEXT("part"))
    {
        OutError = FString::Printf(
            TEXT("model.describe_ops reports op '%s' in context '%s'. Only 'part', 'collision' and 'skin' entries can be ")
            TEXT("synthesised as a statement; teach this test the new context rather than skipping the entry."),
            *Name, *Context);
        return FString();
    }

    // A part's first op must create geometry, so a modifier gets a primitive in front of it.
    return bGenerator
        ? FString::Printf(TEXT("pwmodel 0\npart body {\n    %s\n}\n"), *Statement)
        : FString::Printf(TEXT("pwmodel 0\npart body {\n    box size=(100, 100, 100)\n    %s\n}\n"), *Statement);
}

// The same construction for a `paramSets` entry, which is not an op and therefore has nowhere
// to go as a statement inside a part. Every published parameter is written, not only the
// required ones: these lists are short, their parameters are independent, and an optional
// parameter the parser would reject is precisely the defect this block exists to rule out.
FString ModelHandlersTest_BuildDocumentForParamSet(const TSharedPtr<FJsonObject>& Set, FString& OutError)
{
    const FString Name = Set->GetStringField(TEXT("name"));
    const FString Context = Set->GetStringField(TEXT("context"));

    FString Assignments;

    const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
    if (Set->TryGetArrayField(TEXT("params"), Params) && Params)
    {
        for (const TSharedPtr<FJsonValue>& Value : *Params)
        {
            const TSharedPtr<FJsonObject>* Param = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(Param) || !(*Param).IsValid())
            {
                continue;
            }

            const FString ParamName = (*Param)->GetStringField(TEXT("name"));
            const FString Sample = ModelHandlersTest_SampleForParam(*Param);
            if (Sample.IsEmpty())
            {
                OutError = FString::Printf(
                    TEXT("parameter set '%s' reports param '%s' with type '%s', for which no literal exists"),
                    *Name, *ParamName, *(*Param)->GetStringField(TEXT("type")));
                return FString();
            }

            Assignments += FString::Printf(TEXT(" %s=%s"), *ParamName, *Sample);
        }
    }

    // A model-level statement: written outside every part, after one so the document has
    // geometry to be about.
    if (Context == TEXT("model"))
    {
        return FString::Printf(
            TEXT("pwmodel 0\npart body {\n    box size=(100, 100, 100)\n}\n%s%s\n"), *Name, *Assignments);
    }

    // The part header line, where the brace closes the header instead of opening a line.
    if (Context == TEXT("part_header"))
    {
        return FString::Printf(
            TEXT("pwmodel 0\n%s body%s {\n    box size=(100, 100, 100)\n}\n"), *Name, *Assignments);
    }

    OutError = FString::Printf(
        TEXT("model.describe_ops reports parameter set '%s' in context '%s', which this test cannot place in a ")
        TEXT("document. Teach it that context - dropping the entry would ship an unchecked parameter list, which ")
        TEXT("is the state this block was added to end."),
        *Name, *Context);
    return FString();
}

// Diagnostics that mean the vocabulary and the parser disagree, as opposed to a semantic
// rule the synthesized document happens to trip.
bool ModelHandlersTest_IsVocabularyMismatch(const FString& Code)
{
    return Code == PwSourceDiagnosticCodes::PWSRC_UNKNOWN_OP
        || Code == PwSourceDiagnosticCodes::PWSRC_UNKNOWN_PARAM
        || Code == PwSourceDiagnosticCodes::PWSRC_MISSING_PARAM
        || Code == PwSourceDiagnosticCodes::PWSRC_BAD_TUPLE_ARITY
        || Code == PwSourceDiagnosticCodes::PWSRC_BAD_VALUE
        || Code == PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK;
}

// Writes Source to a unique .pwmodel in Directory and returns the path, empty on failure.
// The caller deletes it. Directory is a parameter because the provenance tests need one
// fixture inside the project directory and one outside it.
FString ModelHandlersTest_WriteTempSourceIn(const FString& Directory, const FString& Source)
{
    IFileManager::Get().MakeDirectory(*Directory, /*Tree=*/true);

    const FString Path = FPaths::Combine(Directory,
        FString::Printf(TEXT("PwModelHandlerTest_%s.pwmodel"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

    return FFileHelper::SaveStringToFile(Source, *Path) ? Path : FString();
}

// The under-the-project fixture directory.
FString ModelHandlersTest_ProjectSourceDirectory()
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PinWright"), TEXT("ModelHandlerTests"));
}

FString ModelHandlersTest_WriteTempSource(const FString& Source)
{
    return ModelHandlersTest_WriteTempSourceIn(ModelHandlersTest_ProjectSourceDirectory(), Source);
}

// Writes one convention-following source under the project's Content tree. The asset path is
// computed independently from the production helper so a shared bad transformation cannot make
// the fixture and the handler agree on the same wrong answer.
FString ModelHandlersTest_WriteContentSource(FString& OutAssetPath, FString& OutDirectory)
{
    const FString UniqueSegment = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    OutDirectory = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("PinWrightTests"),
        TEXT("SourceBesideAsset"), UniqueSegment);
    OutAssetPath = FString::Printf(
        TEXT("/Game/PinWrightTests/SourceBesideAsset/%s/PW_DerivedModel"), *UniqueSegment);
    IFileManager::Get().MakeDirectory(*OutDirectory, /*Tree=*/true);
    const FString SourcePath = FPaths::Combine(OutDirectory, TEXT("PW_DerivedModel.pwmodel"));
    return FFileHelper::SaveStringToFile(ModelHandlersTest_MinimalDocument, *SourcePath)
        ? SourcePath
        : FString();
}

// The outside-the-project fixture directory. It exists so the absolute branch of the stamp
// is exercised by a real compile rather than by a hand-seeded SourcePath, which is how the
// absolute-always defect survived every earlier test.
FString ModelHandlersTest_OutsideProjectSourceDirectory()
{
    return FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("PinWrightModelHandlerTests"));
}

// The same normalization ModelHandler_ResolveSourcePath applies, so a test states the path
// the handler resolves to rather than the spelling it happened to write.
FString ModelHandlersTest_FullPath(const FString& Path)
{
    FString Full = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Full);
    return Full;
}

FString ModelHandlersTest_UniqueAssetPath()
{
    return FString::Printf(TEXT("/Game/PwModelHandlerTests/PW_ModelHandler_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

// What one compile through the real dispatcher produced, gathered before the fixtures are
// deleted so the assertions can run against a cleaned-up host.
struct FModelHandlersTest_CompileProbe
{
    bool bCompiled = false;
    bool bStampFound = false;
    // The absolute path of the written source: what the handler must resolve to.
    FString ResolvedPath;
    // The response's sourcePath field.
    FString ReportedSourcePath;
    // UPwModelAssetUserData::SourcePath on the created asset.
    FString Stamp;
    TSharedPtr<FJsonObject> Result;
};

// Compiles a minimal document written into Directory, reads the provenance stamp back off the
// created asset, then deletes both the source and the asset. save=false on purpose: nothing
// here needs a .uasset on disk, and the stamp is on the in-memory asset either way.
FModelHandlersTest_CompileProbe ModelHandlersTest_CompileFrom(
    FAutomationTestBase& Test, const FString& Directory)
{
    FModelHandlersTest_CompileProbe Probe;

    const FString SourcePath = ModelHandlersTest_WriteTempSourceIn(Directory, ModelHandlersTest_MinimalDocument);
    if (SourcePath.IsEmpty())
    {
        Test.AddError(FString::Printf(TEXT("Could not write a temporary .pwmodel source under '%s'"), *Directory));
        return Probe;
    }
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*SourcePath, false, true, true); };

    Probe.ResolvedPath = ModelHandlersTest_FullPath(SourcePath);

    const FString AssetPath = ModelHandlersTest_UniqueAssetPath();
    ON_SCOPE_EXIT { CleanupTestAsset(AssetPath); };

    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("filePath"), Probe.ResolvedPath);
    Payload->SetStringField(TEXT("outputPath"), AssetPath);
    Payload->SetBoolField(TEXT("save"), false);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("t-stamp"),
        Payload, bSuccess, Probe.Result, ErrorCode);

    if (!Test.TestTrue(*FString::Printf(TEXT("model.compile succeeds from '%s'. Diagnostics: %s"),
            *Directory, *ModelHandlersTest_DescribeResult(Probe.Result)), bSuccess))
    {
        return Probe;
    }

    Probe.bCompiled = true;
    if (Probe.Result.IsValid())
    {
        Probe.ReportedSourcePath = Probe.Result->GetStringField(TEXT("sourcePath"));
    }

    UStaticMesh* Mesh = FindObject<UStaticMesh>(nullptr, *ToObjectPath(AssetPath));
    const UPwModelAssetUserData* Stamp = Mesh
        ? Cast<UPwModelAssetUserData>(Mesh->GetAssetUserDataOfClass(UPwModelAssetUserData::StaticClass()))
        : nullptr;
    if (!Stamp)
    {
        Test.AddError(FString::Printf(
            TEXT("the asset compiled to '%s' carries no provenance stamp to check"), *AssetPath));
        return Probe;
    }

    Probe.bStampFound = true;
    Probe.Stamp = Stamp->SourcePath;
    return Probe;
}

// ============================================================================
// Diagnostic reporting shape
// ============================================================================

// A document whose only mistake is repeated: N part-level generators appended into one block,
// each overlapping the last, so the compiler raises exactly one PWMODEL_UNUNIONED_OVERLAP per op
// after the first. It is Examples/pwmodel/crystal_cluster.pwmodel reduced to its cause, kept
// in-test rather than pointed at that file so these assertions do not move when the example is
// edited - the real one compiled to 17,815 characters against a 10,000-character budget and
// spilled to disk on every iteration of the authoring loop.
//
// 16 ops is chosen to overflow the budget with margin when nothing is shaped; if the overlap
// message text ever shrinks, RepeatedOverlapsOverflowTheInlineBudget fails first and says so.
constexpr int32 ModelHandlersTest_OverlapOpCount = 16;

FString ModelHandlersTest_RepeatedOverlapDocument()
{
    FString Source = TEXT("pwmodel 0\npart stack {\n");
    for (int32 Index = 0; Index < ModelHandlersTest_OverlapOpCount; ++Index)
    {
        // Full extent 20, stepped 5 apart: every box interpenetrates its predecessor by 15 uu in
        // X and 20 in Y and Z, well past FCompiler::WarnOnUnunionedOverlap's 1e-3 epsilon.
        Source += FString::Printf(TEXT("    box size=(20, 20, 20) at=(%d, 0, 0)\n"), Index * 5);
    }
    Source += TEXT("}\n");
    return Source;
}

// The OTHER shape of diagnostic-heavy document, and the one that actually spilled in the field:
// many PARTS rather than many ops in one part.
//
// It matters because the body and the diagnostics grow together. Each part adds ~330 characters
// of `parts[]` entry - two counts, three orientation fields, a four-corner bounds box - and also
// adds an overlap warning group of its own, since diagnostics collapse on (severity, code, part).
// So the response with the most diagnostics to print is also the response with the least room to
// print them in, which is exactly the case a single fixed limit gets wrong.
//
// Twenty-four parts, each two boxes overlapping by 10 uu on every axis. Every part raises its own
// op-level warning, and the parts overlap each other as well.
constexpr int32 ModelHandlersTest_OverlapPartCount = 24;

FString ModelHandlersTest_ManyPartOverlapDocument()
{
    FString Source = TEXT("pwmodel 0\n");
    for (int32 Index = 0; Index < ModelHandlersTest_OverlapPartCount; ++Index)
    {
        Source += FString::Printf(TEXT("part lobe_%02d {\n"), Index);
        Source += FString::Printf(TEXT("    box size=(20, 20, 20) at=(%d, 0, 0)\n"), Index * 10);
        Source += FString::Printf(TEXT("    box size=(20, 20, 20) at=(%d, 0, 0)\n"), Index * 10 + 10);
        Source += TEXT("}\n");
    }
    return Source;
}

// One model.validate call against that document with the given shaping payload. validate rather
// than compile: it runs the same compiler and the same response builder, and creates nothing to
// clean up.
bool ModelHandlersTest_ValidateOverlapDocument(FAutomationTestBase& Test, const FString& RequestId,
                                               const TSharedPtr<FJsonObject>& Payload,
                                               TSharedPtr<FJsonObject>& OutResult)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    Payload->SetStringField(TEXT("text"), ModelHandlersTest_RepeatedOverlapDocument());

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.validate"), RequestId, Payload, bSuccess, OutResult, ErrorCode);

    if (!bSuccess)
    {
        Test.AddError(FString::Printf(
            TEXT("model.validate rejected the repeated-overlap fixture (%s): %s"),
            *ErrorCode, *ModelHandlersTest_DescribeResult(OutResult)));
    }
    return bSuccess;
}

const TArray<TSharedPtr<FJsonValue>>* ModelHandlersTest_DiagnosticArray(const TSharedPtr<FJsonObject>& Result)
{
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    return (Result.IsValid() && Result->TryGetArrayField(TEXT("diagnostics"), Values)) ? Values : nullptr;
}

TSharedPtr<FJsonObject> ModelHandlersTest_AsObject(const TSharedPtr<FJsonValue>& Value)
{
    const TSharedPtr<FJsonObject>* Object = nullptr;
    return (Value.IsValid() && Value->TryGetObject(Object)) ? *Object : TSharedPtr<FJsonObject>();
}

TSharedPtr<FJsonObject> ModelHandlersTest_Summary(const TSharedPtr<FJsonObject>& Result)
{
    const TSharedPtr<FJsonObject>* Summary = nullptr;
    return (Result.IsValid() && Result->TryGetObjectField(TEXT("diagnosticSummary"), Summary))
        ? *Summary : TSharedPtr<FJsonObject>();
}

int32 ModelHandlersTest_SummaryValue(const TSharedPtr<FJsonObject>& Summary, const TCHAR* Field)
{
    int32 Value = 0;
    return (Summary.IsValid() && Summary->TryGetNumberField(Field, Value)) ? Value : -1;
}

// A missing flag reads as false rather than dereferencing nothing: a response that lost the
// summary altogether must fail an assertion, not take the suite down with it.
bool ModelHandlersTest_SummaryFlag(const TSharedPtr<FJsonObject>& Summary, const TCHAR* Field)
{
    bool Value = false;
    return Summary.IsValid() && Summary->TryGetBoolField(Field, Value) && Value;
}

// The first emitted entry carrying Code, or an invalid pointer.
TSharedPtr<FJsonObject> ModelHandlersTest_FindDiagnostic(const TSharedPtr<FJsonObject>& Result, const TCHAR* Code,
                                                         int32& OutMatchCount)
{
    OutMatchCount = 0;
    TSharedPtr<FJsonObject> First;

    const TArray<TSharedPtr<FJsonValue>>* Values = ModelHandlersTest_DiagnosticArray(Result);
    if (!Values)
    {
        return First;
    }

    for (const TSharedPtr<FJsonValue>& Value : *Values)
    {
        const TSharedPtr<FJsonObject> Entry = ModelHandlersTest_AsObject(Value);
        if (Entry.IsValid() && Entry->GetStringField(TEXT("code")) == Code)
        {
            ++OutMatchCount;
            if (!First.IsValid())
            {
                First = Entry;
            }
        }
    }
    return First;
}

// The arithmetic that makes a shaped response readable at all: every diagnostic the compiler
// produced is either printed, counted as an occurrence of a printed one, or counted as
// suppressed. Asserted on EVERY response these tests take, because the failure this shaping
// could introduce is a truncated list read as a clean compile (docs/rpc-design.md section 1).
void ModelHandlersTest_CheckSummaryInvariants(FAutomationTestBase& Test, const FString& Where,
                                              const TSharedPtr<FJsonObject>& Result)
{
    const TSharedPtr<FJsonObject> Summary = ModelHandlersTest_Summary(Result);
    if (!Summary.IsValid())
    {
        Test.AddError(Where + TEXT(": the response carries no diagnosticSummary"));
        return;
    }

    const int32 Total = ModelHandlersTest_SummaryValue(Summary, TEXT("total"));
    const int32 Errors = ModelHandlersTest_SummaryValue(Summary, TEXT("errors"));
    const int32 Warnings = ModelHandlersTest_SummaryValue(Summary, TEXT("warnings"));
    const int32 Emitted = ModelHandlersTest_SummaryValue(Summary, TEXT("emitted"));
    const int32 Represented = ModelHandlersTest_SummaryValue(Summary, TEXT("represented"));
    const int32 Collapsed = ModelHandlersTest_SummaryValue(Summary, TEXT("collapsedOccurrences"));
    const int32 BySeverity = ModelHandlersTest_SummaryValue(Summary, TEXT("suppressedBySeverity"));
    const int32 ByLimit = ModelHandlersTest_SummaryValue(Summary, TEXT("suppressedByLimit"));

    const TArray<TSharedPtr<FJsonValue>>* Values = ModelHandlersTest_DiagnosticArray(Result);
    Test.TestEqual(*(Where + TEXT(": emitted counts the entries actually in the array")),
        Emitted, Values ? Values->Num() : -1);

    Test.TestEqual(*(Where + FString::Printf(TEXT(": errors + warnings == total (%d + %d vs %d)"),
        Errors, Warnings, Total)), Errors + Warnings, Total);
    Test.TestEqual(*(Where + FString::Printf(
        TEXT(": total == represented + suppressedBySeverity + suppressedByLimit (%d vs %d + %d + %d)"),
        Total, Represented, BySeverity, ByLimit)), Total, Represented + BySeverity + ByLimit);
    Test.TestEqual(*(Where + FString::Printf(
        TEXT(": represented == emitted + collapsedOccurrences (%d vs %d + %d)"),
        Represented, Emitted, Collapsed)), Represented, Emitted + Collapsed);

    bool bComplete = false;
    Summary->TryGetBoolField(TEXT("complete"), bComplete);
    Test.TestEqual(*(Where + TEXT(": complete is true exactly when every diagnostic has its own entry")),
        bComplete, Emitted == Total);
}

// Whether the production gate would spill this result - measured BY the production gate, not by
// a local copy of its rule.
//
// This used to reimplement the measurement: build the MCP wrapper, pretty-print it, and count the
// payload twice. That was correct when it was written and is not any more - the gate now measures
// ONE CONDENSED copy of `structuredContent` (HttpResponseSpill::MeasureReaderFacingCharacters),
// which is roughly a third of what the old formula counted. A test that keeps its own copy of a
// production formula does not fail when production changes; it goes on passing while asserting
// against arithmetic nothing runs, which is worse than a red test because nothing reports it.
//
// So the oracle is HttpResponseSpill::MarkOversizedToolResult itself, which is exported and is
// the code path that decides. It rewrites the result in place when it spills, so "did it spill"
// is read off the result afterwards: a spilled one loses `structuredContent`'s payload and gains
// `outputTooLong`. The spill root is redirected to a scratch directory first, because the
// oversized case genuinely writes a file.
bool ModelHandlersTest_WouldSpill(const TSharedPtr<FJsonObject>& Result, int32 Threshold)
{
    const TSharedRef<FJsonObject> Payload =
        Result.IsValid() ? Result.ToSharedRef() : MakeShared<FJsonObject>();

    // The wrapper MakeToolCallSuccess builds (Transport/McpRequestCore.cpp): the payload as
    // `structuredContent`, and the same payload serialized into a text block for clients that do
    // not read structured output. Both copies are present exactly as they are on the wire - it is
    // the GATE that now counts only one of them, and that is the decision under test.
    FString StructuredText;
    {
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&StructuredText);
        FJsonSerializer::Serialize(Payload, Writer);
        Writer->Close();
    }

    TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
    TextBlock->SetStringField(TEXT("type"), TEXT("text"));
    TextBlock->SetStringField(TEXT("text"), StructuredText);

    TArray<TSharedPtr<FJsonValue>> Content;
    Content.Add(MakeShared<FJsonValueObject>(TextBlock));

    const TSharedRef<FJsonObject> ToolResult = MakeShared<FJsonObject>();
    ToolResult->SetArrayField(TEXT("content"), Content);
    ToolResult->SetObjectField(TEXT("structuredContent"), Payload);
    ToolResult->SetBoolField(TEXT("isError"), false);

    HttpResponseSpill::MarkOversizedToolResult(ToolResult, Threshold);

    const TSharedPtr<FJsonObject>* Structured = nullptr;
    if (!ToolResult->TryGetObjectField(TEXT("structuredContent"), Structured) ||
        Structured == nullptr || !Structured->IsValid())
    {
        return true;
    }
    return (*Structured)->HasField(TEXT("outputTooLong"));
}

// Reported alongside a verdict so a failure says how far over it was. Informational only - the
// assertions go through ModelHandlersTest_WouldSpill above, because this is the number the gate
// counts today and a second copy of it here is exactly what went stale last time.
int32 ModelHandlersTest_ReaderFacingChars(const TSharedPtr<FJsonObject>& Result)
{
    if (!Result.IsValid())
    {
        return 0;
    }
    FString Body;
    const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
    FJsonSerializer::Serialize(Result.ToSharedRef(), Writer);
    Writer->Close();
    return Body.Len();
}
}

// ============================================================================
// model.compile - parameter contract
// ============================================================================

// Inline text is refused, and the refusal says where to go instead. Both halves matter:
// the refusal protects provenance, the steering keeps it from being worked around.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileRefusesInlineTextTest,
    "PinWright.Model.Handlers.CompileRefusesInlineText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileRefusesInlineTextTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    // filePath and outputPath are both supplied so the request clears the dispatcher's
    // required-parameter gate and reaches the handler's own refusal.
    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("text"), ModelHandlersTest_MinimalDocument);
    Payload->SetStringField(TEXT("filePath"), TEXT("bracket.pwmodel"));
    Payload->SetStringField(TEXT("outputPath"), TEXT("/Game/PwModelHandlerTests/Bracket"));

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("t1"), Payload, bSuccess, Result, ErrorCode);

    TestFalse(TEXT("model.compile refuses inline text"), bSuccess);
    TestEqual(TEXT("refusal is a parameter error"), ErrorCode, FString(TEXT("INVALID_PARAMS")));
    TestTrue(*FString::Printf(TEXT("refusal names model.validate. Message: %s"), *Sink->Message),
        Sink->Message.Contains(TEXT("model.validate")));
    return true;
}

// filePath is required; the dispatcher's own gate is what enforces it, so this also pins
// that the parameter stayed declared as required.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileRequiresFilePathTest,
    "PinWright.Model.Handlers.CompileRequiresFilePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileRequiresFilePathTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("outputPath"), TEXT("/Game/PwModelHandlerTests/Bracket"));

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("t2"), Payload, bSuccess, ErrorCode);

    TestFalse(TEXT("model.compile without filePath fails"), bSuccess);
    // The code, because it is what distinguishes the two rejections that both name filePath.
    // Downgrading RPC_PARAM_REQ("filePath") to RPC_PARAM_OPT does not make the request succeed -
    // the handler's own Ctx.RequireString still refuses it, with "Missing required string field:
    // filePath" (HandlerContext.cpp:97) and ERR_INVALID_PARAMS. Only the dispatcher's gate
    // answers MISSING_REQUIRED_PARAM (RpcDispatcher.cpp:114), so this is the assertion that
    // holds the declaration in place rather than merely the refusal.
    TestEqual(TEXT("the dispatcher's required-parameter gate is what refused it"), ErrorCode,
        FString(TEXT("MISSING_REQUIRED_PARAM")));
    TestTrue(*FString::Printf(TEXT("the error names filePath. Message: %s"), *Sink->Message),
        Sink->Message.Contains(TEXT("filePath")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileOutputPathOptionalAcrossFormatsTest,
    "PinWright.Model.Handlers.CompileOutputPath.OptionalAcrossSourceFormats",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileOutputPathOptionalAcrossFormatsTest::RunTest(const FString& Parameters)
{
    const TCHAR* const Methods[] = {
        TEXT("model.compile"), TEXT("skeleton.compile"), TEXT("anim.compile")
    };
    for (const TCHAR* Method : Methods)
    {
        const FParamSpec* Spec = GetRegisteredParamSpec(Method, TEXT("outputPath"));
        if (TestNotNull(*FString::Printf(TEXT("%s publishes outputPath"), Method), Spec))
        {
            TestFalse(*FString::Printf(TEXT("%s outputPath is optional"), Method), Spec->bRequired);
            TestTrue(*FString::Printf(TEXT("%s explains Content derivation"), Method),
                Spec->Description.Contains(TEXT("Content")) &&
                Spec->Description.Contains(TEXT("/Game")));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileOmittedOutputOutsideContentTest,
    "PinWright.Model.Handlers.CompileOutputPath.OmittedOutsideContentRefuses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileOmittedOutputOutsideContentTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    const FString SourcePath = ModelHandlersTest_WriteTempSource(ModelHandlersTest_MinimalDocument);
    if (SourcePath.IsEmpty())
    {
        AddError(TEXT("Could not write the outside-Content .pwmodel fixture"));
        return true;
    }
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*SourcePath, false, true, true); };

    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("filePath"), SourcePath);

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("t3"), Payload, bSuccess, ErrorCode);

    TestFalse(TEXT("omitted outputPath outside Content fails"), bSuccess);
    TestEqual(TEXT("the refusal has the exact derivation diagnostic code"), ErrorCode,
        FString(TEXT("SOURCE_OUTPUT_PATH_NOT_DERIVABLE")));
    TestTrue(*FString::Printf(TEXT("the message names the outside-Content reason: %s"), *Sink->Message),
        Sink->Message.Contains(TEXT("outside the project Content")));
    TestTrue(*FString::Printf(TEXT("the message names the explicit recovery argument: %s"), *Sink->Message),
        Sink->Message.Contains(TEXT("outputPath='/Game/.../AssetName'")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileOmittedOutputUnderContentTest,
    "PinWright.Model.Handlers.CompileOutputPath.OmittedUnderContentDerives",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileOmittedOutputUnderContentTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    FString SourceDirectory;
    const FString SourcePath = ModelHandlersTest_WriteContentSource(AssetPath, SourceDirectory);
    if (SourcePath.IsEmpty())
    {
        AddError(TEXT("Could not write the under-Content .pwmodel fixture"));
        return true;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
        IFileManager::Get().Delete(*SourcePath, false, true, true);
        IFileManager::Get().DeleteDirectory(*SourceDirectory, false, false);
    };

    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);
    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("filePath"), SourcePath);
    Payload->SetBoolField(TEXT("save"), false);

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("t-derived"),
        Payload, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("compile with omitted outputPath succeeds: %s"), *Sink->Message),
        bSuccess);
    if (bSuccess && Result.IsValid())
    {
        TestEqual(TEXT("the response reports the independently expected derived path"),
            Result->GetStringField(TEXT("assetPath")), AssetPath);
        TestNotNull(TEXT("the asset exists at the derived path"),
            FindObject<UStaticMesh>(nullptr, *ToObjectPath(AssetPath)));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileDerivedForeignAssetRefusesTest,
    "PinWright.Model.Handlers.CompileOutputPath.DerivedForeignAssetStillRefuses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileDerivedForeignAssetRefusesTest::RunTest(const FString& Parameters)
{
    FString AssetPath;
    FString SourceDirectory;
    const FString ConventionSource = ModelHandlersTest_WriteContentSource(AssetPath, SourceDirectory);
    const FString ForeignSource = ModelHandlersTest_WriteTempSource(ModelHandlersTest_MinimalDocument);
    if (ConventionSource.IsEmpty() || ForeignSource.IsEmpty())
    {
        AddError(TEXT("Could not write both sources for the derived-path provenance fixture"));
        if (!ConventionSource.IsEmpty()) IFileManager::Get().Delete(*ConventionSource, false, true, true);
        if (!ForeignSource.IsEmpty()) IFileManager::Get().Delete(*ForeignSource, false, true, true);
        return true;
    }
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(AssetPath);
        IFileManager::Get().Delete(*ConventionSource, false, true, true);
        IFileManager::Get().Delete(*ForeignSource, false, true, true);
        IFileManager::Get().DeleteDirectory(*SourceDirectory, false, false);
    };

    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> ForeignPayload = ModelHandlersTest_Payload();
    ForeignPayload->SetStringField(TEXT("filePath"), ForeignSource);
    ForeignPayload->SetStringField(TEXT("outputPath"), AssetPath);
    ForeignPayload->SetBoolField(TEXT("save"), false);
    bool bForeignSuccess = false;
    FString ForeignError;
    Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("t-derived-foreign-seed"),
        ForeignPayload, bForeignSuccess, ForeignError);
    if (!TestTrue(*FString::Printf(TEXT("the foreign source seeds the target: %s"), *Sink->Message),
            bForeignSuccess))
    {
        return true;
    }

    TSharedPtr<FJsonObject> DerivedPayload = ModelHandlersTest_Payload();
    DerivedPayload->SetStringField(TEXT("filePath"), ConventionSource);
    DerivedPayload->SetBoolField(TEXT("save"), false);
    bool bDerivedSuccess = false;
    FString DerivedError;
    Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("t-derived-foreign-refusal"),
        DerivedPayload, bDerivedSuccess, DerivedError);

    TestFalse(TEXT("derivation does not bypass the foreign-source overwrite guard"), bDerivedSuccess);
    TestEqual(TEXT("the normal model-compile wrapper remains the wire result"), DerivedError,
        FString(TEXT("MODEL_COMPILE_FAILED")));
    TestTrue(*FString::Printf(TEXT("the refusal still names overwrite=true: %s"), *Sink->Message),
        Sink->Message.Contains(TEXT("overwrite=true")));
    return true;
}

// A missing source file gets its own code rather than a generic parse failure - the caller
// mistyped a path, and nothing about the document is wrong.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileMissingFileTest,
    "PinWright.Model.Handlers.CompileMissingFileReportsModelFileNotFound",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileMissingFileTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("filePath"),
        FString::Printf(TEXT("PwModelAbsent_%s.pwmodel"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    Payload->SetStringField(TEXT("outputPath"), TEXT("/Game/PwModelHandlerTests/Absent"));

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("t4"), Payload, bSuccess, ErrorCode);

    TestFalse(TEXT("model.compile on a missing file fails"), bSuccess);
    TestEqual(TEXT("missing source reports MODEL_FILE_NOT_FOUND"), ErrorCode,
        FString(TEXT("MODEL_FILE_NOT_FOUND")));
    return true;
}

// ============================================================================
// model.compile - the provenance stamp
// ============================================================================

// A source under the project directory is stamped project-relative. This is the assertion
// that catches the shipped defect: ModelHandler_ResolveSourcePath ends in
// ConvertRelativePathToFull, and handing that value straight to
// FPwModelCompileOptions::SourcePath stamps a path no second checkout can match. It drives
// the real handler and reads the stamp off the created asset, because a test that seeds
// SourcePath itself passes either way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileStampsProjectRelativeSourceTest,
    "PinWright.Model.Handlers.CompileStampsProjectRelativeSourcePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileStampsProjectRelativeSourceTest::RunTest(const FString& Parameters)
{
    const FModelHandlersTest_CompileProbe Probe =
        ModelHandlersTest_CompileFrom(*this, ModelHandlersTest_ProjectSourceDirectory());
    if (!Probe.bCompiled || !Probe.bStampFound)
    {
        return true;
    }

    TestTrue(*FString::Printf(
        TEXT("a source under the project directory is stamped relative, got '%s'"), *Probe.Stamp),
        FPaths::IsRelative(Probe.Stamp));

    // The stamp still names the same file: relativizing must drop the root and nothing else.
    // Stated as a round-trip rather than by recomputing MakePathRelativeTo, which would only
    // assert that the test and the handler call the same function.
    TestEqual(TEXT("the stamp resolves back to the file that was compiled"),
        ModelHandlersTest_FullPath(FPaths::Combine(FPaths::ProjectDir(), Probe.Stamp)),
        Probe.ResolvedPath);

    // The other half of the contract: sourcePath in the response stays absolute, so a caller
    // can see which file was read while the asset carries the portable form.
    TestEqual(TEXT("the response still reports the absolute resolved path"),
        Probe.ReportedSourcePath, Probe.ResolvedPath);
    return true;
}

// A source outside the project directory has no portable form, so it is stamped absolute.
// Relativizing it anyway would produce a '../..' walk out of the project, which is no more
// portable than the absolute path and harder to read.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileStampsAbsoluteSourceOutsideProjectTest,
    "PinWright.Model.Handlers.CompileStampsAbsoluteSourcePathOutsideProject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileStampsAbsoluteSourceOutsideProjectTest::RunTest(const FString& Parameters)
{
    const FString Directory = ModelHandlersTest_OutsideProjectSourceDirectory();

    // Asserted rather than assumed. Were the host's temp directory to sit inside the project,
    // this test would silently become a second copy of the relative one.
    TestFalse(*FString::Printf(TEXT("the fixture directory '%s' is outside the project"), *Directory),
        FPaths::IsUnderDirectory(Directory, FPaths::ProjectDir()));

    const FModelHandlersTest_CompileProbe Probe = ModelHandlersTest_CompileFrom(*this, Directory);
    IFileManager::Get().DeleteDirectory(*Directory, false, false);

    if (!Probe.bCompiled || !Probe.bStampFound)
    {
        return true;
    }

    TestEqual(TEXT("a source outside the project is stamped with its absolute path"),
        Probe.Stamp, Probe.ResolvedPath);
    return true;
}

// A failed compile creates no asset, so its response must carry no save report at all.
// pendingFlush means "a save was asked for and no .uasset reached disk, so an
// editor.save_all / asset.save is needed" (AssetUtils.cpp:624) - after a parse error that
// instruction points at nothing, and following it hides the real failure behind a no-op save.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileFailureHasNoSaveReportTest,
    "PinWright.Model.Handlers.CompileFailureCarriesNoPendingFlush",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileFailureHasNoSaveReportTest::RunTest(const FString& Parameters)
{
    const FString SourcePath = ModelHandlersTest_WriteTempSource(ModelHandlersTest_UnparsableDocument);
    if (SourcePath.IsEmpty())
    {
        AddError(TEXT("Could not write a temporary .pwmodel source"));
        return true;
    }
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*SourcePath, false, true, true); };

    const FString AssetPath = ModelHandlersTest_UniqueAssetPath();
    ON_SCOPE_EXIT { CleanupTestAsset(AssetPath); };

    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    // 'save' is left at its default of true on purpose: what suppresses the report has to be
    // the failure, not the absence of a request.
    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("filePath"), SourcePath);
    Payload->SetStringField(TEXT("outputPath"), AssetPath);

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("t11"), Payload, bSuccess, Result, ErrorCode);

    TestFalse(TEXT("an unparsable document fails to compile"), bSuccess);
    TestEqual(TEXT("a parse failure reports MODEL_PARSE_FAILED"), ErrorCode,
        FString(TEXT("MODEL_PARSE_FAILED")));

    if (!Result.IsValid())
    {
        AddError(TEXT("the failure response carried no result object to inspect"));
        return true;
    }

    TestFalse(TEXT("a failed compile does not ask the caller to flush an asset that was never created"),
        Result->HasField(TEXT("pendingFlush")));
    TestFalse(TEXT("a failed compile emits no save report at all"),
        Result->HasField(TEXT("saveRequested")));
    TestFalse(TEXT("a failed compile reports nothing saved"),
        Result->GetBoolField(TEXT("savedToDisk")));
    return true;
}

// ============================================================================
// model.validate
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelValidateRequiresExactlyOneSourceTest,
    "PinWright.Model.Handlers.ValidateRequiresExactlyOneSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelValidateRequiresExactlyOneSourceTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    {
        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("model.validate"), TEXT("t5a"),
            ModelHandlersTest_Payload(), bSuccess, ErrorCode);

        TestFalse(TEXT("model.validate with neither text nor filePath fails"), bSuccess);
        TestEqual(TEXT("neither-source rejection is a parameter error"), ErrorCode,
            FString(TEXT("INVALID_PARAMS")));
        // Which of the two mistakes it was. Both cases answer INVALID_PARAMS, so the code alone
        // leaves the messages free to be transposed: the handler picks between them on one
        // ternary (ModelCompileHandler.cpp, `bHasText ? "both were supplied" : "neither was
        // supplied"`), and inverting it tells every caller the exact opposite of what they did
        // while both halves of this test still pass.
        TestTrue(*FString::Printf(TEXT("the message says neither was supplied. Message: %s"), *Sink->Message),
            Sink->Message.Contains(TEXT("neither was supplied")));
    }

    {
        TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
        Payload->SetStringField(TEXT("text"), ModelHandlersTest_MinimalDocument);
        Payload->SetStringField(TEXT("filePath"), TEXT("bracket.pwmodel"));

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("model.validate"), TEXT("t5b"), Payload, bSuccess, ErrorCode);

        TestFalse(TEXT("model.validate with both text and filePath fails"), bSuccess);
        TestEqual(TEXT("both-sources rejection is a parameter error"), ErrorCode,
            FString(TEXT("INVALID_PARAMS")));
        TestTrue(*FString::Printf(TEXT("the message says both were supplied. Message: %s"), *Sink->Message),
            Sink->Message.Contains(TEXT("both were supplied")));
    }

    return true;
}

// Validating from a string is the iteration loop the format is designed around, and it must
// leave nothing behind: no asset path in the result, nothing saved.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelValidateAcceptsTextTest,
    "PinWright.Model.Handlers.ValidateAcceptsTextAndCreatesNoAsset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelValidateAcceptsTextTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("text"), ModelHandlersTest_MinimalDocument);

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.validate"), TEXT("t6"), Payload, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("model.validate accepts inline text. Diagnostics: %s"),
        *ModelHandlersTest_DescribeResult(Result)), bSuccess);

    if (bSuccess && Result.IsValid())
    {
        TestEqual(TEXT("validate reports no asset path"),
            Result->GetStringField(TEXT("assetPath")), FString());
        TestFalse(TEXT("validate saved nothing"), Result->GetBoolField(TEXT("savedToDisk")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelValidateCompoundSubtractComponentsTest,
    "PinWright.Model.Handlers.ValidateCompoundSubtractComponentsIndividually",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelValidateCompoundSubtractComponentsTest::RunTest(const FString& Parameters)
{
    const TCHAR* CompoundSource =
        TEXT("pwmodel 0\n")
        TEXT("part rail {\n")
        TEXT("    box size=(37.0, 4.2, 1.0) at=(13.0, 0, 8.5) material=\"Receiver\"\n")
        TEXT("    bevel distance=0.12 segments=0 infer_material_id=true\n")
        TEXT("    subtract material=\"Receiver\" {\n")
        TEXT("        box size=(0.58, 5.0, 0.55) at=(-4.8, 0, 8.85) material=\"Receiver\"\n")
        TEXT("        array_linear count=34 offset=(1.083, 0, 0)\n")
        TEXT("    }\n")
        TEXT("}\n");

    FString ExpandedSource =
        TEXT("pwmodel 0\n")
        TEXT("part rail {\n")
        TEXT("    box size=(37.0, 4.2, 1.0) at=(13.0, 0, 8.5) material=\"Receiver\"\n")
        TEXT("    bevel distance=0.12 segments=0 infer_material_id=true\n");
    for (int32 Index = 0; Index < 34; ++Index)
    {
        const double X = -4.8 + 1.083 * Index;
        ExpandedSource += FString::Printf(
            TEXT("    subtract material=\"Receiver\" { box size=(0.58, 5.0, 0.55) at=(%.6f, 0, 8.85) material=\"Receiver\" }\n"),
            X);
    }
    ExpandedSource += TEXT("}\n");

    FTestResponseCapture CompoundCapture;
    TSharedPtr<FJsonObject> CompoundPayload = ModelHandlersTest_Payload();
    CompoundPayload->SetStringField(TEXT("text"), CompoundSource);
    TestTrue(TEXT("model.validate handler is registered for the compound fixture"),
        InvokeHandlerWithCapture(TEXT("model.validate"), CompoundPayload, CompoundCapture));
    TestTrue(TEXT("the compound model.validate response was sent"), CompoundCapture.bWasCalled);
    TestTrue(TEXT("the compound model.validate succeeds"), CompoundCapture.bSuccess);

    FTestResponseCapture ExpandedCapture;
    TSharedPtr<FJsonObject> ExpandedPayload = ModelHandlersTest_Payload();
    ExpandedPayload->SetStringField(TEXT("text"), ExpandedSource);
    TestTrue(TEXT("model.validate handler is registered for the expanded fixture"),
        InvokeHandlerWithCapture(TEXT("model.validate"), ExpandedPayload, ExpandedCapture));
    TestTrue(TEXT("the expanded model.validate response was sent"), ExpandedCapture.bWasCalled);
    TestTrue(TEXT("the expanded model.validate succeeds"), ExpandedCapture.bSuccess);

    struct FValidateReport
    {
        int32 TriangleCount = -1;
        int32 TrianglesBefore = -1;
        int32 TrianglesAfter = -1;
        int32 SliversRemoved = -1;
        int32 DegenerateTriangles = -1;
        int32 BoundaryEdges = -1;
        double SignedVolume = 0.0;
        bool bClosed = false;
        bool bFieldsPresent = false;
    };

    auto ReadReport = [this](const TCHAR* Label, const FTestResponseCapture& Capture,
                             FValidateReport& OutReport)
    {
        if (!TestTrue(*FString::Printf(TEXT("%s response carries a result"), Label),
                Capture.Result.IsValid()))
        {
            return;
        }

        bool bReportedSuccess = false;
        TestTrue(*FString::Printf(TEXT("%s response publishes success"), Label),
            Capture.Result->TryGetBoolField(TEXT("success"), bReportedSuccess));
        TestTrue(*FString::Printf(TEXT("%s response success is true"), Label), bReportedSuccess);

        TestTrue(*FString::Printf(TEXT("%s response publishes meshTriangleCount"), Label),
            Capture.Result->TryGetNumberField(TEXT("meshTriangleCount"), OutReport.TriangleCount));

        const bool bTrianglesBeforePresent = Capture.Result->TryGetNumberField(
            TEXT("trianglesBefore"), OutReport.TrianglesBefore);
        const bool bTrianglesAfterPresent = Capture.Result->TryGetNumberField(
            TEXT("trianglesAfter"), OutReport.TrianglesAfter);
        const bool bSliversRemovedPresent = Capture.Result->TryGetNumberField(
            TEXT("sliversRemoved"), OutReport.SliversRemoved);
        TestTrue(*FString::Printf(TEXT("%s response publishes trianglesBefore"), Label),
            bTrianglesBeforePresent);
        TestTrue(*FString::Printf(TEXT("%s response publishes trianglesAfter"), Label),
            bTrianglesAfterPresent);
        TestTrue(*FString::Printf(TEXT("%s response publishes sliversRemoved"), Label),
            bSliversRemovedPresent);

        const TSharedPtr<FJsonObject>* Health = nullptr;
        if (!TestTrue(*FString::Printf(TEXT("%s response carries a health block"), Label),
                Capture.Result->TryGetObjectField(TEXT("health"), Health)
                && Health != nullptr && (*Health).IsValid()))
        {
            return;
        }

        const bool bDegeneratePresent = (*Health)->TryGetNumberField(
            TEXT("degenerateTriangles"), OutReport.DegenerateTriangles);
        const bool bBoundaryPresent = (*Health)->TryGetNumberField(
            TEXT("boundaryEdges"), OutReport.BoundaryEdges);
        const bool bClosedPresent = (*Health)->TryGetBoolField(TEXT("isClosed"), OutReport.bClosed);
        const bool bVolumePresent = (*Health)->TryGetNumberField(
            TEXT("signedVolume"), OutReport.SignedVolume);

        TestTrue(*FString::Printf(TEXT("%s health publishes degenerateTriangles"), Label),
            bDegeneratePresent);
        TestTrue(*FString::Printf(TEXT("%s health publishes boundaryEdges"), Label),
            bBoundaryPresent);
        TestTrue(*FString::Printf(TEXT("%s health publishes isClosed"), Label), bClosedPresent);
        TestTrue(*FString::Printf(TEXT("%s health publishes signedVolume"), Label), bVolumePresent);
        OutReport.bFieldsPresent = bTrianglesBeforePresent && bTrianglesAfterPresent
            && bSliversRemovedPresent && bDegeneratePresent && bBoundaryPresent
            && bClosedPresent && bVolumePresent;
    };

    FValidateReport Compound;
    ReadReport(TEXT("compound"), CompoundCapture, Compound);
    FValidateReport Expanded;
    ReadReport(TEXT("expanded"), ExpandedCapture, Expanded);

    if (Compound.bFieldsPresent && Expanded.bFieldsPresent)
    {
        TestTrue(TEXT("the expanded control has triangles"), Expanded.TriangleCount > 0);
        TestEqual(TEXT("the compound result has no degenerate triangles"),
            Compound.DegenerateTriangles, 0);
        TestEqual(TEXT("the expanded control has no degenerate triangles"),
            Expanded.DegenerateTriangles, 0);
        TestTrue(*FString::Printf(TEXT("the compound result stays within twice the expanded control: %d <= 2 * %d"),
            Compound.TriangleCount, Expanded.TriangleCount),
            Compound.TriangleCount <= Expanded.TriangleCount * 2);
        TestEqual(TEXT("compound trianglesAfter is the final mesh count"),
            Compound.TrianglesAfter, Compound.TriangleCount);
        TestEqual(TEXT("expanded trianglesAfter is the final mesh count"),
            Expanded.TrianglesAfter, Expanded.TriangleCount);
        TestTrue(TEXT("compound cleanup telemetry reports a non-negative sliver count"),
            Compound.SliversRemoved >= 0);
        TestTrue(TEXT("expanded cleanup telemetry reports a non-negative sliver count"),
            Expanded.SliversRemoved >= 0);
        TestTrue(TEXT("the two spellings preserve the same enclosed volume"),
            FMath::IsNearlyEqual(Compound.SignedVolume, Expanded.SignedVolume, 1e-5));
        TestTrue(TEXT("the compound result is closed"), Compound.bClosed);
        TestTrue(TEXT("the expanded control is closed"), Expanded.bClosed);
        TestEqual(TEXT("the two spellings preserve the same boundary-edge count"),
            Compound.BoundaryEdges, Expanded.BoundaryEdges);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelValidateAcceptsFilePathTest,
    "PinWright.Model.Handlers.ValidateAcceptsFilePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelValidateAcceptsFilePathTest::RunTest(const FString& Parameters)
{
    const FString SourcePath = ModelHandlersTest_WriteTempSource(ModelHandlersTest_MinimalDocument);
    if (SourcePath.IsEmpty())
    {
        AddError(TEXT("Could not write a temporary .pwmodel source"));
        return true;
    }
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*SourcePath, false, true, true); };

    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("filePath"), SourcePath);

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.validate"), TEXT("t7"), Payload, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("model.validate accepts a filePath. Diagnostics: %s"),
        *ModelHandlersTest_DescribeResult(Result)), bSuccess);
    return true;
}

// ============================================================================
// model.describe_ops
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDescribeOpsUnknownOpTest,
    "PinWright.Model.Handlers.DescribeOpsRejectsUnknownOp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDescribeOpsUnknownOpTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("op"), TEXT("boxx"));

    bool bSuccess = false;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.describe_ops"), TEXT("t8"), Payload, bSuccess, ErrorCode);

    TestFalse(TEXT("an unknown op name is rejected"), bSuccess);
    TestEqual(TEXT("unknown op reports UNKNOWN_OPERATION"), ErrorCode,
        FString(TEXT("UNKNOWN_OPERATION")));
    // The whole suggestion phrase, not the bare word. `boxx` CONTAINS `box`, and the message
    // echoes the requested name either way - so Contains(TEXT("box")) held with
    // PwModelOpTable::SuggestClosest returning nothing at all, which is the fallback branch
    // (ModelCompileHandler.cpp, "Call model.describe_ops with no 'op'") rather than a suggestion.
    TestTrue(*FString::Printf(TEXT("the rejection names box as the near miss. Message: %s"), *Sink->Message),
        Sink->Message.Contains(TEXT("Did you mean 'box'?")));
    return true;
}

// The documented first authoring call must remain readable in the normal MCP response instead of
// being replaced by an outputTooLong file reference. Measure through the production spill gate,
// not through a copy of its serialization arithmetic.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDescribeOpsIndexStaysInlineTest,
    "PinWright.Model.Handlers.DescribeOpsIndexStaysInline",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDescribeOpsIndexStaysInlineTest::RunTest(const FString& Parameters)
{
    const int32 Threshold = HttpResponseSpill::GetDefaultThresholdCharacters();
    const FString SpillRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("PinWrightTests"), TEXT("ModelHandlersDescribeOpsIndex"));
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(SpillRoot);
    ON_SCOPE_EXIT
    {
        HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
        IFileManager::Get().DeleteDirectory(*SpillRoot, /*RequireExists=*/false, /*Tree=*/true);
    };

    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    const bool bSuccess = ModelHandlersTest_QueryDescribeOps(
        Dispatcher, Sink, TEXT("t-describe-index"), TEXT(""), Result, ErrorCode);

    if (!TestTrue(*FString::Printf(TEXT("model.describe_ops index succeeds (%s: %s)"),
            *ErrorCode, *Sink->Message), bSuccess))
    {
        return true;
    }

    bool bIndex = false;
    TestTrue(TEXT("the no-argument response identifies itself as an index"),
        Result->TryGetBoolField(TEXT("index"), bIndex) && bIndex);
    TestFalse(*FString::Printf(
        TEXT("the no-argument index stays within the response budget (threshold %d)"), Threshold),
        ModelHandlersTest_WouldSpill(Result, Threshold));
    return true;
}

// An op legal in both contexts is two entries, because its parameters differ between them -
// collapsing them would publish one parameter list that is wrong in one of the two places.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDescribeOpsNarrowsToOneOpTest,
    "PinWright.Model.Handlers.DescribeOpsNarrowsToOneOp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDescribeOpsNarrowsToOneOpTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("op"), TEXT("box"));

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.describe_ops"), TEXT("t9"), Payload, bSuccess, Result, ErrorCode);

    TestTrue(TEXT("model.describe_ops op=box succeeds"), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
    TestTrue(TEXT("the response carries an ops array"), Result->TryGetArrayField(TEXT("ops"), Ops));
    if (!Ops)
    {
        return true;
    }

    // Two, because that is the claim: one entry per context, not one collapsed entry and not a
    // context emitted twice. The per-entry checks below are all satisfied by a duplicate.
    TestEqual(TEXT("box narrows to exactly one entry per context"), Ops->Num(), 2);

    bool bSawPart = false;
    bool bSawCollision = false;
    for (const TSharedPtr<FJsonValue>& Value : *Ops)
    {
        const TSharedPtr<FJsonObject>* Op = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Op) || !(*Op).IsValid())
        {
            continue;
        }
        TestEqual(TEXT("every entry is the requested op"), (*Op)->GetStringField(TEXT("name")),
            FString(TEXT("box")));
        bSawPart |= (*Op)->GetStringField(TEXT("context")) == TEXT("part");
        bSawCollision |= (*Op)->GetStringField(TEXT("context")) == TEXT("collision");
    }

    TestTrue(TEXT("box is reported in the part context"), bSawPart);
    TestTrue(TEXT("box is reported in the collision context"), bSawCollision);
    return true;
}

// The anti-drift assertion: every op the verb advertises is built into a document from the
// advertised metadata alone and handed to the parser. A name, parameter, type or
// requiredness that describe_ops reports but the parser does not accept fails here.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDescribeOpsVocabularyParsesTest,
    "PinWright.Model.Handlers.DescribeOpsVocabularyParses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDescribeOpsVocabularyParsesTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    const bool bSuccess = ModelHandlersTest_QueryDescribeOps(
        Dispatcher, Sink, TEXT("t10"), TEXT(""), Result, ErrorCode);

    TestTrue(TEXT("model.describe_ops succeeds with no arguments"), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    bool bIndex = false;
    TestTrue(TEXT("the no-argument response is the compact index"),
        Result->TryGetBoolField(TEXT("index"), bIndex) && bIndex);

    const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
    if (!Result->TryGetArrayField(TEXT("ops"), Ops) || !Ops)
    {
        AddError(TEXT("model.describe_ops returned no ops array"));
        return true;
    }

    // Asserted before the comparison below, not implied by it: an op table that derived empty
    // would satisfy an equality against a response that published nothing, and every per-entry
    // check in this test is a loop that a derived-empty list walks zero times.
    TestTrue(TEXT("the op table itself is not empty"), PwModelOpTable::Get().Num() > 0);

    // The count is knowable, so `> 0` was never the check. The index emits one compact entry per
    // op-table row; a filter that dropped a whole context would leave a shorter list that every
    // per-entry assertion below still accepts one by one.
    TestEqual(TEXT("the compact index publishes one entry per op the parser validates"),
        Ops->Num(), PwModelOpTable::Get().Num());
    TestEqual(TEXT("opCount matches the ops array"),
        static_cast<int32>(Result->GetNumberField(TEXT("opCount"))), Ops->Num());

    // The index is intentionally small, but it must still carry enough identity to drive the
    // detail queries. This proves every TABLE op is indexed, including an op with two contexts.
    for (const FPwModelOpSpec& Spec : PwModelOpTable::Get())
    {
        const FString SpecContext = PwModelOpContextToString(Spec.Context);

        const TSharedPtr<FJsonObject>* Published = nullptr;
        for (const TSharedPtr<FJsonValue>& Value : *Ops)
        {
            const TSharedPtr<FJsonObject>* Op = nullptr;
            if (Value.IsValid() && Value->TryGetObject(Op) && (*Op).IsValid()
                && (*Op)->GetStringField(TEXT("name")) == Spec.Name
                && (*Op)->GetStringField(TEXT("context")) == SpecContext)
            {
                Published = Op;
                break;
            }
        }

        if (!Published)
        {
            AddError(FString::Printf(
                TEXT("the parser validates '%s' in context '%s', which model.describe_ops does not publish"),
                *Spec.Name, *SpecContext));
            continue;
        }

        double ParameterCount = 0.0;
        TestTrue(*FString::Printf(TEXT("the compact index reports a parameter count for '%s' (%s)"),
            *Spec.Name, *SpecContext), (*Published)->TryGetNumberField(TEXT("parameterCount"), ParameterCount));
        TestEqual(*FString::Printf(TEXT("the compact index reports the parser's parameter count for '%s' (%s)"),
            *Spec.Name, *SpecContext), static_cast<int32>(ParameterCount), Spec.Params.Num());
    }

    // Now follow the public discovery contract: one explicit detail query per distinct name. The
    // detail response is where the anti-drift checks belong; the index must not grow back into a
    // second copy of it just to make these tests convenient.
    TSet<FString> QueriedNames;
    TArray<TSharedPtr<FJsonValue>> DetailedOps;
    for (const TSharedPtr<FJsonValue>& Value : *Ops)
    {
        const TSharedPtr<FJsonObject>* Op = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Op) || !(*Op).IsValid())
        {
            AddError(TEXT("an entry of the compact ops[] index is not an object"));
            continue;
        }

        const FString Name = (*Op)->GetStringField(TEXT("name"));
        if (QueriedNames.Contains(Name))
        {
            continue;
        }
        QueriedNames.Add(Name);

        TSharedPtr<FJsonObject> DetailResult;
        FString DetailErrorCode;
        if (!ModelHandlersTest_QueryDescribeOps(Dispatcher, Sink,
                FString::Printf(TEXT("t10-%s"), *Name), Name, DetailResult, DetailErrorCode))
        {
            AddError(FString::Printf(TEXT("detail query for model op '%s' failed (%s): %s"),
                *Name, *DetailErrorCode, *Sink->Message));
            continue;
        }

        bool bDetailIndex = true;
        TestTrue(*FString::Printf(TEXT("detail query for '%s' identifies a complete response"), *Name),
            DetailResult->TryGetBoolField(TEXT("index"), bDetailIndex) && !bDetailIndex);

        const TArray<TSharedPtr<FJsonValue>>* DetailValues = nullptr;
        if (!DetailResult->TryGetArrayField(TEXT("ops"), DetailValues) || !DetailValues)
        {
            AddError(FString::Printf(TEXT("detail query for '%s' returned no ops array"), *Name));
            continue;
        }
        for (const TSharedPtr<FJsonValue>& DetailValue : *DetailValues)
        {
            DetailedOps.Add(DetailValue);
        }
    }

    TestEqual(TEXT("detail queries cover every parser op row"), DetailedOps.Num(), PwModelOpTable::Get().Num());

    for (const TSharedPtr<FJsonValue>& Value : DetailedOps)
    {
        const TSharedPtr<FJsonObject>* Op = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Op) || !(*Op).IsValid())
        {
            AddError(TEXT("an entry of a detailed ops[] response is not an object"));
            continue;
        }

        const FString Name = (*Op)->GetStringField(TEXT("name"));
        const FString Context = (*Op)->GetStringField(TEXT("context"));

        EPwModelOpContext OpContext = EPwModelOpContext::Part;
        if (Context == TEXT("collision"))
        {
            OpContext = EPwModelOpContext::Collision;
        }
        else if (Context == TEXT("skin"))
        {
            OpContext = EPwModelOpContext::Skin;
        }
        else if (Context != TEXT("part"))
        {
            AddError(FString::Printf(
                TEXT("model.describe_ops reports op '%s' in unknown context '%s'"), *Name, *Context));
            continue;
        }

        if (const FPwModelOpSpec* Spec = PwModelOpTable::Find(Name, OpContext))
        {
            const FString Where = FString::Printf(TEXT("%s (%s)"), *Name, *Context);
            ModelHandlersTest_CheckParamsMatchSpecs(*this, Where, *Op, Spec->Params);
            ModelHandlersTest_CheckOpFlagsMatchSpec(*this, Where, *Op, *Spec);
        }
        else
        {
            AddError(FString::Printf(
                TEXT("model.describe_ops advertises '%s' in context '%s', which the op table does not hold"),
                *Name, *Context));
        }

        FString BuildError;
        const FString Document = ModelHandlersTest_BuildDocumentForOp(*Op, BuildError);
        if (Document.IsEmpty())
        {
            AddError(BuildError);
            continue;
        }

        FPwModelDocument Parsed;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(Document, Parsed, Diagnostics);

        for (const FPwDiagnostic& Diagnostic : Diagnostics)
        {
            if (Diagnostic.Severity == EPwSeverity::Error
                && ModelHandlersTest_IsVocabularyMismatch(Diagnostic.Code))
            {
                AddError(FString::Printf(
                    TEXT("model.describe_ops advertises '%s' but the parser rejects it: %s. Document:\n%s"),
                    *Name, *Diagnostic.ToString(), *Document));
            }
        }
    }

    return true;
}

// The same anti-drift assertion for the parameter sets that are not ops. They are published
// as their own block precisely because they cannot be synthesised as a statement inside a part
// or a collision block, which is the assumption every `ops` consumer makes; the answer to that
// is a second round-trip that knows where each one goes, not a relaxed check.
//
// It asserts three things a missing block would each hide on its own: that all sets are
// published at all, that each publishes exactly the parameter list the parser validates against
// (so a parameter the parser gains and the verb does not is a failure, not a silent hole), and
// that a document written from the published metadata alone parses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDescribeOpsParamSetsParseTest,
    "PinWright.Model.Handlers.DescribeOpsParamSetsParse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDescribeOpsParamSetsParseTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> IndexResult;
    FString ErrorCode;
    const bool bSuccess = ModelHandlersTest_QueryDescribeOps(
        Dispatcher, Sink, TEXT("t11-index"), TEXT(""), IndexResult, ErrorCode);

    TestTrue(TEXT("model.describe_ops succeeds with no arguments"), bSuccess);
    if (!bSuccess || !IndexResult.IsValid())
    {
        return true;
    }

    bool bIndex = false;
    TestTrue(TEXT("the no-argument response is the compact index"),
        IndexResult->TryGetBoolField(TEXT("index"), bIndex) && bIndex);

    const TArray<TSharedPtr<FJsonValue>>* IndexSets = nullptr;
    if (!IndexResult->TryGetArrayField(TEXT("paramSets"), IndexSets) || !IndexSets)
    {
        AddError(TEXT("model.describe_ops index returned no paramSets array. The model-level statements and the "
                      "part header are validated by the parser and published nowhere else."));
        return true;
    }

    TestEqual(TEXT("the index carries all non-op parameter sets"), IndexSets->Num(), 3);
    TestEqual(TEXT("paramSetCount matches the index paramSets array"),
        static_cast<int32>(IndexResult->GetNumberField(TEXT("paramSetCount"))), IndexSets->Num());

    for (const TSharedPtr<FJsonValue>& Value : *IndexSets)
    {
        const TSharedPtr<FJsonObject>* Set = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Set) || !(*Set).IsValid())
        {
            AddError(TEXT("an entry of the compact paramSets[] index is not an object"));
            continue;
        }

        const FString Name = (*Set)->GetStringField(TEXT("name"));
        const FString ExpectedContext = Name == TEXT("part") ? TEXT("part_header") : TEXT("model");
        const int32 ExpectedCount = Name == TEXT("uv_layout")
            ? PwModelOpTable::UVLayoutParams().Num()
            : Name == TEXT("lightmap")
                ? PwModelOpTable::LightmapParams().Num()
                : PwModelOpTable::PartHeaderParams().Num();
        TestTrue(*FString::Printf(TEXT("the index reports a known parameter set ('%s')"), *Name),
            Name == TEXT("uv_layout") || Name == TEXT("lightmap") || Name == TEXT("part"));
        TestEqual(*FString::Printf(TEXT("the index reports '%s' in its parser context"), *Name),
            (*Set)->GetStringField(TEXT("context")), ExpectedContext);
        TestEqual(*FString::Printf(TEXT("the index reports '%s' parameter count"), *Name),
            static_cast<int32>((*Set)->GetNumberField(TEXT("parameterCount"))), ExpectedCount);
    }

    // Detail is intentionally fetched by name. This is the same two-step contract an author uses
    // and keeps the full parameter lists out of the zero-argument response.
    TArray<TSharedPtr<FJsonValue>> DetailedSets;
    for (const TCHAR* Name : { TEXT("uv_layout"), TEXT("lightmap"), TEXT("part") })
    {
        TSharedPtr<FJsonObject> DetailResult;
        FString DetailErrorCode;
        if (!ModelHandlersTest_QueryDescribeOps(Dispatcher, Sink,
                FString::Printf(TEXT("t11-%s"), Name), Name, DetailResult, DetailErrorCode))
        {
            AddError(FString::Printf(TEXT("detail query for parameter set '%s' failed (%s): %s"),
                Name, *DetailErrorCode, *Sink->Message));
            continue;
        }

        bool bDetailIndex = true;
        TestFalse(*FString::Printf(TEXT("detail query for '%s' is not an index"), Name),
            !DetailResult->TryGetBoolField(TEXT("index"), bDetailIndex) || bDetailIndex);

        const TArray<TSharedPtr<FJsonValue>>* DetailValues = nullptr;
        if (!DetailResult->TryGetArrayField(TEXT("paramSets"), DetailValues) || !DetailValues)
        {
            AddError(FString::Printf(TEXT("detail query for parameter set '%s' returned no paramSets array"), Name));
            continue;
        }
        for (const TSharedPtr<FJsonValue>& DetailValue : *DetailValues)
        {
            DetailedSets.Add(DetailValue);
        }
    }

    bool bSawUVLayout = false;
    bool bSawLightmap = false;
    bool bSawPartHeader = false;

    for (const TSharedPtr<FJsonValue>& Value : DetailedSets)
    {
        const TSharedPtr<FJsonObject>* Set = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(Set) || !(*Set).IsValid())
        {
            AddError(TEXT("an entry of paramSets[] is not an object"));
            continue;
        }

        const FString Name = (*Set)->GetStringField(TEXT("name"));

        if (Name == TEXT("uv_layout"))
        {
            bSawUVLayout = true;
            ModelHandlersTest_CheckParamsMatchSpecs(*this, Name, *Set,
                PwModelOpTable::UVLayoutParams());
        }
        else if (Name == TEXT("lightmap"))
        {
            bSawLightmap = true;
            ModelHandlersTest_CheckParamsMatchSpecs(*this, Name, *Set, PwModelOpTable::LightmapParams());

            // Named explicitly on top of the generic spec comparison above. That comparison is
            // relative - it fails only when the wire and the parser disagree - so dropping
            // `resolution` from BOTH would slip through it, and `resolution` is the one
            // parameter whose absence is invisible in the compiled asset: the mesh simply keeps
            // the UStaticMesh default of 4 and every authored lightmap UV is baked at 4x4.
            bool bSawResolution = false;
            const TArray<TSharedPtr<FJsonValue>>* LightmapParams = nullptr;
            if ((*Set)->TryGetArrayField(TEXT("params"), LightmapParams) && LightmapParams)
            {
                for (const TSharedPtr<FJsonValue>& ParamValue : *LightmapParams)
                {
                    const TSharedPtr<FJsonObject>* Param = nullptr;
                    if (ParamValue.IsValid() && ParamValue->TryGetObject(Param) && (*Param).IsValid()
                        && (*Param)->GetStringField(TEXT("name")) == TEXT("resolution"))
                    {
                        bSawResolution = true;
                        break;
                    }
                }
            }
            TestTrue(TEXT("the lightmap statement publishes 'resolution'"), bSawResolution);
        }
        else if (Name == TEXT("part"))
        {
            bSawPartHeader = true;
            ModelHandlersTest_CheckParamsMatchSpecs(*this, Name, *Set, PwModelOpTable::PartHeaderParams());
        }
        else
        {
            // The same policy the contexts get: an entry this test cannot name is an ERROR, not
            // a skip. Only known names reach a spec comparison, so a new set would
            // otherwise ship with its parameter list checked by nothing but the parse below -
            // which sees only what was published and cannot notice a parameter the wire dropped.
            AddError(FString::Printf(
                TEXT("model.describe_ops publishes parameter set '%s', which this test has no parser spec to ")
                TEXT("compare against. Teach it the new set rather than letting its parameters ship unchecked."),
                *Name));
        }

        FString BuildError;
        const FString Document = ModelHandlersTest_BuildDocumentForParamSet(*Set, BuildError);
        if (Document.IsEmpty())
        {
            AddError(BuildError);
            continue;
        }

        FPwModelDocument Parsed;
        TArray<FPwDiagnostic> Diagnostics;
        FPwModelParser::Parse(Document, Parsed, Diagnostics);

        for (const FPwDiagnostic& Diagnostic : Diagnostics)
        {
            if (Diagnostic.Severity == EPwSeverity::Error
                && ModelHandlersTest_IsVocabularyMismatch(Diagnostic.Code))
            {
                AddError(FString::Printf(
                    TEXT("model.describe_ops advertises parameter set '%s' but the parser rejects it: %s. Document:\n%s"),
                    *Name, *Diagnostic.ToString(), *Document));
            }
        }
    }

    TestTrue(TEXT("the uv_layout statement's parameters are published"), bSawUVLayout);
    TestTrue(TEXT("the lightmap statement's parameters are published"), bSawLightmap);
    TestTrue(TEXT("the part header's parameters are published"), bSawPartHeader);
    return true;
}

// `op` narrows by name across the whole vocabulary, not only the op table. Answering
// UNKNOWN_OPERATION for `lightmap` while the unnarrowed response carries it would tell an
// author the word does not exist.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDescribeOpsNarrowsToParamSetTest,
    "PinWright.Model.Handlers.DescribeOpsNarrowsToParamSet",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDescribeOpsNarrowsToParamSetTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("op"), TEXT("lightmap"));

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.describe_ops"), TEXT("t12"), Payload, bSuccess, Result, ErrorCode);

    TestTrue(*FString::Printf(TEXT("model.describe_ops op=lightmap succeeds. Error: %s %s"),
        *ErrorCode, *Sink->Message), bSuccess);
    if (!bSuccess || !Result.IsValid())
    {
        return true;
    }

    // Both arrays are present whatever the narrowing selected, so a caller reads one shape.
    const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
    TestTrue(TEXT("the narrowed response still carries an ops array"), Result->TryGetArrayField(TEXT("ops"), Ops));
    if (Ops)
    {
        TestEqual(TEXT("lightmap is not an op"), Ops->Num(), 0);
    }

    const TArray<TSharedPtr<FJsonValue>>* Sets = nullptr;
    TestTrue(TEXT("the narrowed response carries a paramSets array"),
        Result->TryGetArrayField(TEXT("paramSets"), Sets));
    if (!Sets)
    {
        return true;
    }

    TestEqual(TEXT("op=lightmap narrows to exactly one parameter set"), Sets->Num(), 1);
    if (Sets->Num() == 1)
    {
        const TSharedPtr<FJsonObject>* Set = nullptr;
        if ((*Sets)[0].IsValid() && (*Sets)[0]->TryGetObject(Set) && (*Set).IsValid())
        {
            TestEqual(TEXT("the entry is the lightmap statement"), (*Set)->GetStringField(TEXT("name")),
                FString(TEXT("lightmap")));
        }
    }

    TSharedPtr<FJsonObject> UVLayoutPayload = ModelHandlersTest_Payload();
    UVLayoutPayload->SetStringField(TEXT("op"), TEXT("uv_layout"));
    TSharedPtr<FJsonObject> UVLayoutResult;
    bSuccess = false;
    ErrorCode.Reset();
    Dispatch(Dispatcher, Sink, TEXT("model.describe_ops"), TEXT("t12-uv-layout"),
        UVLayoutPayload, bSuccess, UVLayoutResult, ErrorCode);
    TestTrue(*FString::Printf(TEXT("model.describe_ops op=uv_layout succeeds. Error: %s %s"),
        *ErrorCode, *Sink->Message), bSuccess);
    if (bSuccess && UVLayoutResult.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* UVLayoutOps = nullptr;
        if (TestTrue(TEXT("uv_layout detail carries ops"),
                UVLayoutResult->TryGetArrayField(TEXT("ops"), UVLayoutOps))
            && UVLayoutOps)
        {
            TestEqual(TEXT("uv_layout is not an op"), UVLayoutOps->Num(), 0);
        }

        const TArray<TSharedPtr<FJsonValue>>* UVLayoutSets = nullptr;
        if (TestTrue(TEXT("uv_layout detail carries paramSets"),
                UVLayoutResult->TryGetArrayField(TEXT("paramSets"), UVLayoutSets))
            && UVLayoutSets)
        {
            TestEqual(TEXT("op=uv_layout narrows to exactly one parameter set"),
                UVLayoutSets->Num(), 1);
            if (UVLayoutSets->Num() == 1)
            {
                const TSharedPtr<FJsonObject>* Set = nullptr;
                if ((*UVLayoutSets)[0].IsValid()
                    && (*UVLayoutSets)[0]->TryGetObject(Set)
                    && (*Set).IsValid())
                {
                    TestEqual(TEXT("the entry is the uv_layout statement"),
                        (*Set)->GetStringField(TEXT("name")), FString(TEXT("uv_layout")));
                }
            }
        }
    }
    return true;
}


// ============================================================================
// model.compile save reporting
// ============================================================================

// The response contract that closes B-model-compile-saved-false-opaque:
//
//   1. a save:true compile onto a fresh path reports a state that is UNAMBIGUOUSLY durable -
//      saveState:"written", saved:true, no pendingFlush - and the .uasset is on disk to prove
//      it;
//   2. no successful compile response may carry saved:false without a saveState AND a
//      saveDetail explaining it, in either direction of the save flag.
//
// (2) is the invariant, not (1): the original defect was an INTERMITTENT saved:false, so a test
// that only pinned the happy path would have stayed green through the whole incident. It is
// asserted structurally - if not saved, then explained - so it fires on whichever branch the
// next occurrence takes.
//
// Deliberately NOT added to the determinism test (TestPwModelCompiler.cpp), which documents that
// bSavedToDisk is a function of file mtime and the pre-save dirty flag rather than of the
// source, and therefore excludes it from its recompiles-reproduce comparison.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileSaveReportsDurableStateTest,
    "PinWright.Model.Handlers.CompileSaveReportsDurableState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileSaveReportsDurableStateTest::RunTest(const FString& Parameters)
{
    const FString SourcePath = ModelHandlersTest_WriteTempSource(ModelHandlersTest_MinimalDocument);
    if (SourcePath.IsEmpty())
    {
        AddError(TEXT("Could not write a temporary .pwmodel source"));
        return true;
    }
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*SourcePath, false, true, true); };

    const FString AssetPath = ModelHandlersTest_UniqueAssetPath();
    ON_SCOPE_EXIT { CleanupTestAsset(AssetPath); };

    FString Filename;
    const bool bResolved = FPackageName::TryConvertLongPackageNameToFilename(
        AssetPath, Filename, FPackageName::GetAssetPackageExtension());
    if (!TestTrue(TEXT("the output package path resolves to a filename"), bResolved))
    {
        return true;
    }
    TestTrue(TEXT("nothing is on disk at the output path before the compile"),
        IFileManager::Get().FileSize(*Filename) < 0);

    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
    Payload->SetStringField(TEXT("filePath"), SourcePath);
    Payload->SetStringField(TEXT("outputPath"), AssetPath);
    Payload->SetBoolField(TEXT("save"), true);

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("tsave1"), Payload, bSuccess, Result, ErrorCode);

    if (!TestTrue(*FString::Printf(TEXT("the compile succeeded. Diagnostics: %s"),
            *ModelHandlersTest_DescribeResult(Result)), bSuccess) || !Result.IsValid())
    {
        return true;
    }

    // The disk is the arbiter. Everything below is the response agreeing with it.
    TestTrue(TEXT("the .uasset is on disk after a save:true compile"),
        IFileManager::Get().FileSize(*Filename) > 0);

    TestTrue(TEXT("savedToDisk:true"), Result->GetBoolField(TEXT("savedToDisk")));
    TestTrue(TEXT("saveRequested:true"), Result->GetBoolField(TEXT("saveRequested")));
    TestTrue(TEXT("saved:true"), Result->GetBoolField(TEXT("saved")));
    TestFalse(TEXT("a durable compile owes no flush"), Result->HasField(TEXT("pendingFlush")));

    FString SaveState;
    Result->TryGetStringField(TEXT("saveState"), SaveState);
    TestEqual(TEXT("a fresh save:true compile reports written"), SaveState, FString(TEXT("written")));
    TestTrue(TEXT("the reported state is one of the durable states"),
        SaveState == AssetSaveStateToWire(EAssetSaveState::Written)
        || SaveState == AssetSaveStateToWire(EAssetSaveState::AlreadyCurrent));

    FString SaveDetail;
    Result->TryGetStringField(TEXT("saveDetail"), SaveDetail);
    TestFalse(TEXT("saveDetail is present and non-empty"), SaveDetail.IsEmpty());
    return true;
}

// The invariant, run over both save flags: a SUCCESS response that admits saved:false must say
// which of the not-durable states it is and what to do about it. Before the fix a saved:false
// carried only pendingFlush, which is the same field for "flush it" and "flushing will not
// help" - and that is why a busy period of compiles was unactionable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelCompileNotSavedIsAlwaysExplainedTest,
    "PinWright.Model.Handlers.CompileNotSavedIsAlwaysExplained",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelCompileNotSavedIsAlwaysExplainedTest::RunTest(const FString& Parameters)
{
    const FString SourcePath = ModelHandlersTest_WriteTempSource(ModelHandlersTest_MinimalDocument);
    if (SourcePath.IsEmpty())
    {
        AddError(TEXT("Could not write a temporary .pwmodel source"));
        return true;
    }
    ON_SCOPE_EXIT { IFileManager::Get().Delete(*SourcePath, false, true, true); };

    const bool bSaveFlags[] = { true, false };
    for (const bool bSave : bSaveFlags)
    {
        const FString AssetPath = ModelHandlersTest_UniqueAssetPath();
        ON_SCOPE_EXIT { CleanupTestAsset(AssetPath); };

        FRpcDispatcher Dispatcher;
        FSinkPtr Sink;
        MakeDispatcher(Sink, Dispatcher);

        TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
        Payload->SetStringField(TEXT("filePath"), SourcePath);
        Payload->SetStringField(TEXT("outputPath"), AssetPath);
        Payload->SetBoolField(TEXT("save"), bSave);

        bool bSuccess = false;
        TSharedPtr<FJsonObject> Result;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("model.compile"), TEXT("tsave2"), Payload, bSuccess, Result, ErrorCode);

        const FString Where = FString::Printf(TEXT("[save=%s] "), bSave ? TEXT("true") : TEXT("false"));
        if (!TestTrue(*(Where + FString::Printf(TEXT("the compile succeeded. Diagnostics: %s"),
                *ModelHandlersTest_DescribeResult(Result))), bSuccess) || !Result.IsValid())
        {
            continue;
        }

        // A successful compile always carries the save report, whichever way the flag went.
        if (!TestTrue(*(Where + TEXT("a successful compile carries a save report")),
                Result->HasField(TEXT("saveRequested"))))
        {
            continue;
        }

        FString SaveState;
        FString SaveDetail;
        Result->TryGetStringField(TEXT("saveState"), SaveState);
        Result->TryGetStringField(TEXT("saveDetail"), SaveDetail);

        TestFalse(*(Where + TEXT("a save report always names a state")), SaveState.IsEmpty());
        TestFalse(*(Where + TEXT("a save report always explains what to do")), SaveDetail.IsEmpty());

        if (Result->GetBoolField(TEXT("saved")))
        {
            TestTrue(*(Where + FString::Printf(TEXT("saved:true only under a durable state, got %s"), *SaveState)),
                SaveState == AssetSaveStateToWire(EAssetSaveState::Written)
                || SaveState == AssetSaveStateToWire(EAssetSaveState::AlreadyCurrent));
        }
        else
        {
            // The load-bearing half. Whatever branch produced the false, the caller can act.
            TestTrue(*(Where + FString::Printf(TEXT("saved:false is explained by a not-durable state, got %s"), *SaveState)),
                SaveState == AssetSaveStateToWire(EAssetSaveState::NotRequested)
                || SaveState == AssetSaveStateToWire(EAssetSaveState::Deferred)
                || SaveState == AssetSaveStateToWire(EAssetSaveState::Failed)
                || SaveState == AssetSaveStateToWire(EAssetSaveState::NotPersistable));
        }

        if (!bSave)
        {
            TestEqual(*(Where + TEXT("save:false reports notRequested, not a failure")),
                SaveState, FString(AssetSaveStateToWire(EAssetSaveState::NotRequested)));
        }
    }
    return true;
}

// ============================================================================
// model.compile / model.validate - diagnostic reporting shape
// ============================================================================
//
// Compiling Examples/pwmodel/crystal_cluster.pwmodel answered 17,815 characters against a
// 10,000-character inline budget and spilled to Saved/PinWright/HttpResponses/, so the author
// read the result off disk on every iteration. The payload was almost entirely one warning
// repeated. These four tests hold the three mechanisms that fixed it - collapse, limit, severity
// filter - and the one property they exist for: the response fits inline.
//
// The fifth guards the honesty half. Every one of them re-checks the summary arithmetic, because
// the defect this shaping could introduce is worse than the one it fixes: a truncated diagnostic
// list read as a clean compile.

// Repeated warnings become ONE entry carrying an occurrence count and the sites. Reverting this
// puts the response back over the budget and back onto the disk-spill path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiagnosticsCollapseRepeatsTest,
    "PinWright.Model.Handlers.RepeatedDiagnosticsCollapseToOneEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDiagnosticsCollapseRepeatsTest::RunTest(const FString& Parameters)
{
    // The unshaped run first: the expected occurrence count is read off it, so a change in how
    // many warnings the compiler raises retunes this test instead of breaking it.
    TSharedPtr<FJsonObject> RawPayload = ModelHandlersTest_Payload();
    RawPayload->SetBoolField(TEXT("collapseDiagnostics"), false);
    RawPayload->SetNumberField(TEXT("diagnosticLimit"), 0);

    TSharedPtr<FJsonObject> Raw;
    if (!ModelHandlersTest_ValidateOverlapDocument(*this, TEXT("t-collapse-raw"), RawPayload, Raw))
    {
        return true;
    }
    ModelHandlersTest_CheckSummaryInvariants(*this, TEXT("unshaped"), Raw);

    int32 RawOverlaps = 0;
    ModelHandlersTest_FindDiagnostic(Raw, PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP, RawOverlaps);
    if (!TestTrue(*FString::Printf(
            TEXT("the fixture raises repeated PWMODEL_UNUNIONED_OVERLAP warnings (got %d). Diagnostics: %s"),
            RawOverlaps, *ModelHandlersTest_DescribeResult(Raw)),
        RawOverlaps >= 2))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Shaped;
    if (!ModelHandlersTest_ValidateOverlapDocument(*this, TEXT("t-collapse"), ModelHandlersTest_Payload(), Shaped))
    {
        return true;
    }
    ModelHandlersTest_CheckSummaryInvariants(*this, TEXT("default"), Shaped);

    int32 ShapedOverlaps = 0;
    const TSharedPtr<FJsonObject> Entry =
        ModelHandlersTest_FindDiagnostic(Shaped, PwModelDiagnosticCodes::PWMODEL_UNUNIONED_OVERLAP, ShapedOverlaps);

    TestEqual(TEXT("collapsing is on by default, so the repeated warning is one entry"), ShapedOverlaps, 1);
    if (!Entry.IsValid())
    {
        AddError(FString::Printf(TEXT("the default response carries no PWMODEL_UNUNIONED_OVERLAP entry at all: %s"),
            *ModelHandlersTest_DescribeResult(Shaped)));
        return true;
    }

    int32 Occurrences = 0;
    Entry->TryGetNumberField(TEXT("occurrences"), Occurrences);
    TestEqual(TEXT("the collapsed entry counts every occurrence it stands for"), Occurrences, RawOverlaps);

    // ModelHandler_MaxOccurrenceSites. The sites are the distinguishing detail - the messages are
    // boilerplate - so the bound is on the list, and what it dropped is reported rather than lost.
    const int32 MaxSites = 12;
    const TArray<TSharedPtr<FJsonValue>>* Sites = nullptr;
    if (TestTrue(TEXT("a collapsed entry lists where its repeats are"),
            Entry->TryGetArrayField(TEXT("occurrenceSites"), Sites)) && Sites)
    {
        TestEqual(TEXT("the site list is bounded"), Sites->Num(), FMath::Min(RawOverlaps, MaxSites));

        const TSharedPtr<FJsonObject> FirstSite = ModelHandlersTest_AsObject((*Sites)[0]);
        if (TestTrue(TEXT("a site is an object"), FirstSite.IsValid()))
        {
            TestEqual(TEXT("the first site is the entry's own position"),
                FirstSite->GetIntegerField(TEXT("line")), Entry->GetIntegerField(TEXT("line")));
            TestTrue(TEXT("a site carries a column"), FirstSite->HasField(TEXT("column")));
        }
    }

    int32 SitesOmitted = 0;
    Entry->TryGetNumberField(TEXT("occurrenceSitesOmitted"), SitesOmitted);
    TestEqual(TEXT("the site list says how many positions it did not print"),
        SitesOmitted, FMath::Max(0, RawOverlaps - MaxSites));

    // Each occurrence names the op it overlapped and that op's line, so the texts genuinely
    // differ. The entry prints the first one and must say so rather than let a caller read it as
    // covering all of them.
    int32 DistinctMessages = 0;
    Entry->TryGetNumberField(TEXT("distinctMessages"), DistinctMessages);
    TestEqual(TEXT("a collapsed entry reports that its one message did not cover every occurrence"),
        DistinctMessages, RawOverlaps);

    const TSharedPtr<FJsonObject> Summary = ModelHandlersTest_Summary(Shaped);
    TestEqual(TEXT("collapsing does not change the reported total"),
        ModelHandlersTest_SummaryValue(Summary, TEXT("total")),
        ModelHandlersTest_SummaryValue(ModelHandlersTest_Summary(Raw), TEXT("total")));
    TestEqual(TEXT("the folded diagnostics are counted as collapsed, not as suppressed"),
        ModelHandlersTest_SummaryValue(Summary, TEXT("collapsedOccurrences")), RawOverlaps - 1);
    TestEqual(TEXT("collapsing suppresses nothing by the limit"),
        ModelHandlersTest_SummaryValue(Summary, TEXT("suppressedByLimit")), 0);
    return true;
}

// The limit caps the array and the response says how much it dropped; 0 restores everything,
// which is what keeps the disk-spill path reachable on purpose.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiagnosticLimitCapsArrayTest,
    "PinWright.Model.Handlers.DiagnosticLimitCapsTheArrayAndReportsTheRest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDiagnosticLimitCapsArrayTest::RunTest(const FString& Parameters)
{
    // Collapsing off throughout, so the limit is the only thing shaping the array.
    TSharedPtr<FJsonObject> RawPayload = ModelHandlersTest_Payload();
    RawPayload->SetBoolField(TEXT("collapseDiagnostics"), false);
    RawPayload->SetNumberField(TEXT("diagnosticLimit"), 0);

    TSharedPtr<FJsonObject> Raw;
    if (!ModelHandlersTest_ValidateOverlapDocument(*this, TEXT("t-limit-raw"), RawPayload, Raw))
    {
        return true;
    }
    ModelHandlersTest_CheckSummaryInvariants(*this, TEXT("limit 0"), Raw);

    const TSharedPtr<FJsonObject> RawSummary = ModelHandlersTest_Summary(Raw);
    const int32 RawTotal = ModelHandlersTest_SummaryValue(RawSummary, TEXT("total"));
    if (!TestTrue(*FString::Printf(TEXT("the fixture produces more diagnostics than the cap under test (got %d)"),
            RawTotal), RawTotal > 3))
    {
        return true;
    }

    // The documented sentinel: 0 is "every entry", not "no entries".
    TestEqual(TEXT("diagnosticLimit 0 emits every diagnostic"),
        ModelHandlersTest_SummaryValue(RawSummary, TEXT("emitted")), RawTotal);
    TestTrue(TEXT("an unshaped response reports itself complete"),
        ModelHandlersTest_SummaryFlag(RawSummary, TEXT("complete")));

    TSharedPtr<FJsonObject> CappedPayload = ModelHandlersTest_Payload();
    CappedPayload->SetBoolField(TEXT("collapseDiagnostics"), false);
    CappedPayload->SetNumberField(TEXT("diagnosticLimit"), 3);

    TSharedPtr<FJsonObject> Capped;
    if (!ModelHandlersTest_ValidateOverlapDocument(*this, TEXT("t-limit-3"), CappedPayload, Capped))
    {
        return true;
    }
    ModelHandlersTest_CheckSummaryInvariants(*this, TEXT("limit 3"), Capped);

    const TArray<TSharedPtr<FJsonValue>>* Values = ModelHandlersTest_DiagnosticArray(Capped);
    TestEqual(TEXT("the limit caps the array"), Values ? Values->Num() : -1, 3);

    const TSharedPtr<FJsonObject> Summary = ModelHandlersTest_Summary(Capped);
    TestEqual(TEXT("the limit does not change the reported total"),
        ModelHandlersTest_SummaryValue(Summary, TEXT("total")), RawTotal);
    TestEqual(TEXT("everything the limit dropped is counted"),
        ModelHandlersTest_SummaryValue(Summary, TEXT("suppressedByLimit")), RawTotal - 3);
    TestEqual(TEXT("the response echoes the limit it applied"),
        ModelHandlersTest_SummaryValue(Summary, TEXT("limit")), 3);

    TestFalse(TEXT("a capped response never claims to be complete"),
        ModelHandlersTest_SummaryFlag(Summary, TEXT("complete")));
    return true;
}

// The filter empties the array of a severity while the totals still carry it. This is the
// dangerous one: a caller must not be able to read a filtered response as a clean compile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiagnosticSeverityFilterTest,
    "PinWright.Model.Handlers.DiagnosticSeverityFilterKeepsTheTotals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDiagnosticSeverityFilterTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> RawPayload = ModelHandlersTest_Payload();
    RawPayload->SetBoolField(TEXT("collapseDiagnostics"), false);
    RawPayload->SetNumberField(TEXT("diagnosticLimit"), 0);

    TSharedPtr<FJsonObject> Raw;
    if (!ModelHandlersTest_ValidateOverlapDocument(*this, TEXT("t-severity-raw"), RawPayload, Raw))
    {
        return true;
    }

    const TSharedPtr<FJsonObject> RawSummary = ModelHandlersTest_Summary(Raw);
    const int32 RawTotal = ModelHandlersTest_SummaryValue(RawSummary, TEXT("total"));
    const int32 RawWarnings = ModelHandlersTest_SummaryValue(RawSummary, TEXT("warnings"));
    if (!TestTrue(*FString::Printf(TEXT("the fixture produces warnings to filter (got %d of %d)"),
            RawWarnings, RawTotal), RawWarnings > 0))
    {
        return true;
    }

    {
        TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
        Payload->SetBoolField(TEXT("collapseDiagnostics"), false);
        Payload->SetNumberField(TEXT("diagnosticLimit"), 0);
        Payload->SetStringField(TEXT("diagnosticSeverity"), TEXT("error"));

        TSharedPtr<FJsonObject> ErrorsOnly;
        if (!ModelHandlersTest_ValidateOverlapDocument(*this, TEXT("t-severity-error"), Payload, ErrorsOnly))
        {
            return true;
        }
        ModelHandlersTest_CheckSummaryInvariants(*this, TEXT("errors only"), ErrorsOnly);

        const TArray<TSharedPtr<FJsonValue>>* Values = ModelHandlersTest_DiagnosticArray(ErrorsOnly);
        if (Values)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Values)
            {
                const TSharedPtr<FJsonObject> Entry = ModelHandlersTest_AsObject(Value);
                TestEqual(TEXT("severityFilter=error prints only errors"),
                    Entry.IsValid() ? Entry->GetStringField(TEXT("severity")) : FString(), FString(TEXT("error")));
            }
        }

        const TSharedPtr<FJsonObject> Summary = ModelHandlersTest_Summary(ErrorsOnly);
        TestEqual(TEXT("the filtered-out warnings are still counted"),
            ModelHandlersTest_SummaryValue(Summary, TEXT("warnings")), RawWarnings);
        TestEqual(TEXT("the filtered-out warnings are still totalled"),
            ModelHandlersTest_SummaryValue(Summary, TEXT("total")), RawTotal);
        TestEqual(TEXT("what the filter removed is reported as removed"),
            ModelHandlersTest_SummaryValue(Summary, TEXT("suppressedBySeverity")), RawWarnings);
        TestEqual(TEXT("the response echoes the filter it applied"),
            Summary.IsValid() ? Summary->GetStringField(TEXT("severityFilter")) : FString(),
            FString(TEXT("error")));

        TestFalse(TEXT("a filtered response never claims to be complete"),
            ModelHandlersTest_SummaryFlag(Summary, TEXT("complete")));
    }

    {
        TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
        Payload->SetBoolField(TEXT("collapseDiagnostics"), false);
        Payload->SetNumberField(TEXT("diagnosticLimit"), 0);
        Payload->SetStringField(TEXT("diagnosticSeverity"), TEXT("warning"));

        TSharedPtr<FJsonObject> WarningsOnly;
        if (!ModelHandlersTest_ValidateOverlapDocument(*this, TEXT("t-severity-warning"), Payload, WarningsOnly))
        {
            return true;
        }
        ModelHandlersTest_CheckSummaryInvariants(*this, TEXT("warnings only"), WarningsOnly);

        const TArray<TSharedPtr<FJsonValue>>* Values = ModelHandlersTest_DiagnosticArray(WarningsOnly);
        TestEqual(TEXT("severityFilter=warning keeps every warning"), Values ? Values->Num() : -1, RawWarnings);
    }
    return true;
}

// The property the whole feature exists for, measured the way the gate measures it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiagnosticHeavyResponseStaysInlineTest,
    "PinWright.Model.Handlers.DefaultShapingKeepsADiagnosticHeavyRunInline",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDiagnosticHeavyResponseStaysInlineTest::RunTest(const FString& Parameters)
{
    const int32 Threshold = HttpResponseSpill::GetDefaultThresholdCharacters();

    // The oversized half of this test really does spill, so the spill root is redirected before
    // anything measures. Restored on every exit path, including the early returns below.
    const FString SpillRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("PinWrightTests"), TEXT("ModelHandlersInlineBudget"));
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(SpillRoot);
    ON_SCOPE_EXIT
    {
        HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
        IFileManager::Get().DeleteDirectory(*SpillRoot, /*RequireExists=*/false, /*Tree=*/true);
    };

    TSharedPtr<FJsonObject> RawPayload = ModelHandlersTest_Payload();
    RawPayload->SetBoolField(TEXT("collapseDiagnostics"), false);
    RawPayload->SetNumberField(TEXT("diagnosticLimit"), 0);

    TSharedPtr<FJsonObject> Raw;
    if (!ModelHandlersTest_ValidateOverlapDocument(*this, TEXT("t-inline-raw"), RawPayload, Raw))
    {
        return true;
    }

    const int32 RawChars = ModelHandlersTest_ReaderFacingChars(Raw);
    // Fails first if the overlap message ever shrinks: without this the assertion below could
    // pass on a fixture that was never over budget, which would prove nothing at all. Raise
    // ModelHandlersTest_OverlapOpCount if it fires.
    TestTrue(*FString::Printf(
        TEXT("the unshaped fixture overflows the inline budget (%d reader-facing chars, threshold %d) - if it no ")
        TEXT("longer does, raise ModelHandlersTest_OverlapOpCount"), RawChars, Threshold),
        ModelHandlersTest_WouldSpill(Raw, Threshold));

    TSharedPtr<FJsonObject> Shaped;
    if (!ModelHandlersTest_ValidateOverlapDocument(*this, TEXT("t-inline"), ModelHandlersTest_Payload(), Shaped))
    {
        return true;
    }

    // THE PROPERTY THE DEFAULT LIMIT IS DERIVED FOR, checked by the code that decides it rather
    // than by this file's own arithmetic. ModelHandler_DefaultDiagnosticLimit sizes itself from
    // this same threshold against a measured per-entry and per-part cost; if either measurement
    // drifts - a longer diagnostic message, another field on the response body - this is what
    // reports it, instead of the default quietly starting to spill on real documents.
    const int32 ShapedChars = ModelHandlersTest_ReaderFacingChars(Shaped);
    TestFalse(*FString::Printf(
        TEXT("the default response stays inline (%d reader-facing chars against a %d threshold; unshaped was %d)"),
        ShapedChars, Threshold, RawChars),
        ModelHandlersTest_WouldSpill(Shaped, Threshold));
    return true;
}

// The regression four separate callers reported independently: a model with dozens of legitimate
// overlap warnings spilled its WHOLE response at the default shaping. It read as "the default
// limit is not applied" and was the opposite - the default was applied and was too generous for
// that body, because a fixed number cannot know that a 24-part response has already spent most
// of the budget on `parts[]` before a single diagnostic is printed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiagnosticLimitFitsTheBodyTest,
    "PinWright.Model.Handlers.DefaultLimitShrinksToFitAManyPartResponse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDiagnosticLimitFitsTheBodyTest::RunTest(const FString& Parameters)
{
    const int32 Threshold = HttpResponseSpill::GetDefaultThresholdCharacters();

    const FString SpillRoot = FPaths::Combine(
        FPaths::ProjectSavedDir(), TEXT("PinWrightTests"), TEXT("ModelHandlersFittedBudget"));
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(SpillRoot);
    ON_SCOPE_EXIT
    {
        HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
        IFileManager::Get().DeleteDirectory(*SpillRoot, /*RequireExists=*/false, /*Tree=*/true);
    };

    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    // No `diagnosticLimit` on the payload: fitting happens only when the caller names no number,
    // and sending the published default would be a caller naming one.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), ModelHandlersTest_ManyPartOverlapDocument());

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.validate"), TEXT("t-fitted"), Payload,
             bSuccess, Result, ErrorCode);
    if (!TestTrue(*FString::Printf(TEXT("model.validate accepted the many-part fixture (%s)"), *ErrorCode),
            bSuccess))
    {
        return true;
    }

    const TSharedPtr<FJsonObject> Summary = ModelHandlersTest_Summary(Result);
    const int32 Total = ModelHandlersTest_SummaryValue(Summary, TEXT("total"));
    const int32 Emitted = ModelHandlersTest_SummaryValue(Summary, TEXT("emitted"));
    const int32 EchoedLimit = ModelHandlersTest_SummaryValue(Summary, TEXT("limit"));

    // The premise, asserted rather than assumed. If this fixture stopped raising a pile of
    // diagnostics, everything below would pass for the wrong reason.
    if (!TestTrue(*FString::Printf(
            TEXT("Precondition: the fixture raises many diagnostics (total %d) - raise ")
            TEXT("ModelHandlersTest_OverlapPartCount if it no longer does"), Total),
            Total >= ModelHandlersTest_OverlapPartCount))
    {
        return true;
    }

    // THE FIX, measured as a DIFFERENCE between two responses rather than against a literal.
    // The same shaping, unshaped by any caller parameter, run against a one-part document: its
    // body is far smaller, so the fit must leave it room for more entries. A fixed limit gives
    // the two the same number, which is the behaviour this replaces.
    TSharedPtr<FJsonObject> SmallPayload = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> SmallResult;
    if (!ModelHandlersTest_ValidateOverlapDocument(*this, TEXT("t-fitted-small"), SmallPayload, SmallResult))
    {
        return true;
    }
    const int32 SmallLimit =
        ModelHandlersTest_SummaryValue(ModelHandlersTest_Summary(SmallResult), TEXT("limit"));

    TestTrue(*FString::Printf(
        TEXT("the limit shrank to fit the bigger body (%d parts echoed %d, one part echoed %d, ")
        TEXT("%d diagnostics total)"),
        ModelHandlersTest_OverlapPartCount, EchoedLimit, SmallLimit, Total),
        EchoedLimit < SmallLimit);

    // A fitted response must SAY what it did. The echoed limit is the one actually applied, not
    // the published default - otherwise a caller reading `limit` is told a number that did not
    // shape anything.
    TestEqual(*FString::Printf(
        TEXT("the echoed limit is the one that shaped the array (echoed %d, emitted %d)"),
        EchoedLimit, Emitted), Emitted, FMath::Min(EchoedLimit, Total));

    // And the whole point: it stays inline. Measured by the production gate, not by arithmetic
    // repeated here.
    TestFalse(*FString::Printf(
        TEXT("a %d-part response with %d diagnostics stays inline (%d reader-facing chars, threshold %d)"),
        ModelHandlersTest_OverlapPartCount, Total,
        ModelHandlersTest_ReaderFacingChars(Result), Threshold),
        ModelHandlersTest_WouldSpill(Result, Threshold));

    // Shaping still never hides the count. The summary is taken over the full array before any
    // limit applies, so a fitted response cannot be mistaken for a quiet one.
    TestTrue(TEXT("every diagnostic is still counted"), Total > Emitted);
    TestFalse(TEXT("a shaped response never claims to be complete"),
        ModelHandlersTest_SummaryFlag(Summary, TEXT("complete")));

    return true;
}

// An explicit limit is honoured exactly, fitting or not: a caller who names a number is not
// asking for advice. Without this the fit could quietly override the one parameter that exists
// to let a caller override the fit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiagnosticExplicitLimitIsNotFittedTest,
    "PinWright.Model.Handlers.AnExplicitDiagnosticLimitIsNeverFitted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDiagnosticExplicitLimitIsNotFittedTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("text"), ModelHandlersTest_ManyPartOverlapDocument());
    // Above what the fit would choose for this body, so "honoured" and "fitted" give different
    // answers and the test can tell them apart.
    Payload->SetNumberField(TEXT("diagnosticLimit"), 9);

    bool bSuccess = false;
    TSharedPtr<FJsonObject> Result;
    FString ErrorCode;
    Dispatch(Dispatcher, Sink, TEXT("model.validate"), TEXT("t-explicit-limit"), Payload,
             bSuccess, Result, ErrorCode);
    if (!TestTrue(*FString::Printf(TEXT("model.validate accepted the fixture (%s)"), *ErrorCode), bSuccess))
    {
        return true;
    }

    const TSharedPtr<FJsonObject> Summary = ModelHandlersTest_Summary(Result);
    TestEqual(TEXT("the caller's number is echoed, not a fitted one"),
        ModelHandlersTest_SummaryValue(Summary, TEXT("limit")), 9);
    TestEqual(TEXT("and the array is shaped to it"),
        ModelHandlersTest_SummaryValue(Summary, TEXT("emitted")),
        FMath::Min(9, ModelHandlersTest_SummaryValue(Summary, TEXT("total"))));

    return true;
}

// A shaping parameter with no meaning is refused rather than degraded to "everything": a filter
// that quietly widened would answer a question the caller did not ask.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FModelDiagnosticShapingParamsRejectedTest,
    "PinWright.Model.Handlers.DiagnosticShapingParamsRejectBadValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FModelDiagnosticShapingParamsRejectedTest::RunTest(const FString& Parameters)
{
    FRpcDispatcher Dispatcher;
    FSinkPtr Sink;
    MakeDispatcher(Sink, Dispatcher);

    {
        TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
        Payload->SetStringField(TEXT("text"), ModelHandlersTest_MinimalDocument);
        Payload->SetNumberField(TEXT("diagnosticLimit"), -1);

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("model.validate"), TEXT("t-bad-limit"), Payload, bSuccess, ErrorCode);

        TestFalse(TEXT("a negative diagnosticLimit is refused"), bSuccess);
        TestEqual(TEXT("a negative diagnosticLimit is a parameter error"), ErrorCode,
            FString(TEXT("INVALID_PARAMS")));
        // The refusal has to name the sentinel, or a caller who wanted everything reaches for -1
        // again rather than 0.
        TestTrue(*FString::Printf(TEXT("the refusal explains that 0 means every diagnostic. Message: %s"),
            *Sink->Message), Sink->Message.Contains(TEXT("0 means every diagnostic")));
    }

    {
        TSharedPtr<FJsonObject> Payload = ModelHandlersTest_Payload();
        Payload->SetStringField(TEXT("text"), ModelHandlersTest_MinimalDocument);
        Payload->SetStringField(TEXT("diagnosticSeverity"), TEXT("fatal"));

        bool bSuccess = false;
        FString ErrorCode;
        Dispatch(Dispatcher, Sink, TEXT("model.validate"), TEXT("t-bad-severity"), Payload, bSuccess, ErrorCode);

        TestFalse(TEXT("an unknown diagnosticSeverity is refused, not widened to 'all'"), bSuccess);
        TestEqual(TEXT("an unknown diagnosticSeverity is a parameter error"), ErrorCode,
            FString(TEXT("INVALID_PARAMS")));
        TestTrue(*FString::Printf(TEXT("the refusal lists the accepted spellings. Message: %s"), *Sink->Message),
            Sink->Message.Contains(TEXT("'all', 'error' or 'warning'")));
    }
    return true;
}
