// Copyright (c) 2026 Alexander Penkin. MIT License.

// Red/acceptance test for F-blueprint-node-discovery.
//
// The ticket requests action-database node discovery with pin-context filtering:
//   blueprint.graph.find_node_types(assetPath, graph, filter, contextPins?) — substring/category
//     search over the action database scoped to a graph, returning stable type ids usable by
//     node-creation RPCs, with optional pin-type compatibility filtering.
//   blueprint.graph.get_node_type_pins(typeId) — the expected pin list for a candidate type.
//
// Neither verb exists today (the only sibling, blueprint.graph.list_node_types, does a plain
// TObjectIterator<UClass> over UK2Node subclasses returning {className, displayName} with no
// filter/contextPins param and no action-database/pin-compatibility filtering). This test asserts
// the ticket's acceptance surface directly, so it is RED pre-fix precisely because the capability
// is absent, and becomes the acceptance test the implementer adopts on GO.
//
// Counterfactual (what fails without the fix):
//  - IsHandlerRegistered("blueprint.graph.find_node_types") / get_node_type_pins -> false, so the
//    presence TestTrue checks fail.
//  - InvokeHandlerWithCapture returns false for an unregistered method (bFound=false) and the
//    capture stays un-succeeded, so the "handler found" + "call succeeds" + non-empty result
//    assertions fail.
//  - the type-id round-trip (a find_node_types type id fed into get_node_type_pins) cannot run.
//
// Note on differential depth: the primary differential here is presence + the read-only invoke
// contract (call succeeds, returns a non-empty type list, a returned type id round-trips into
// get_node_type_pins). The deeper pin-context semantics from the acceptance criterion — "a float
// output-pin context returns math nodes accepting float and EXCLUDES exec-only nodes" — are
// exercised (the contextPins path is invoked) but not hard-asserted against speculative type-id
// spellings; the implementer should strengthen the exec-only-exclusion assertion once the concrete
// response shape / type-id vocabulary is fixed.

#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"

namespace
{
    // Returns the first array field on Result found among the candidate key names (the concrete
    // response key is the implementer's call; the sibling list_node_types uses "nodeTypes"), or
    // nullptr if none of the candidates is present. Keeps the acceptance assertions from being
    // brittle to the exact result-key spelling while still proving a non-empty type list came back.
    const TArray<TSharedPtr<FJsonValue>>* FindTypesArray(const TSharedPtr<FJsonObject>& Result)
    {
        if (!Result.IsValid())
        {
            return nullptr;
        }
        static const TCHAR* Candidates[] = { TEXT("nodeTypes"), TEXT("types"), TEXT("results"), TEXT("nodes") };
        for (const TCHAR* Key : Candidates)
        {
            const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
            if (Result->TryGetArrayField(Key, Arr) && Arr)
            {
                return Arr;
            }
        }
        return nullptr;
    }

    // Extracts a stable type id from a find_node_types entry, trying the ticket-named "typeId" first
    // then the sibling "className"/"id" spellings. Empty if the entry carries none.
    FString ExtractTypeId(const TSharedPtr<FJsonValue>& Entry)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Entry.IsValid() || !Entry->TryGetObject(Obj) || !Obj || !(*Obj).IsValid())
        {
            return FString();
        }
        static const TCHAR* IdKeys[] = { TEXT("typeId"), TEXT("className"), TEXT("id") };
        for (const TCHAR* Key : IdKeys)
        {
            FString Value;
            if ((*Obj)->TryGetStringField(Key, Value) && !Value.IsEmpty())
            {
                return Value;
            }
        }
        return FString();
    }
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintFindNodeTypesTest,
    "PinWright.blueprint.graph.find_node_types.PinContextDiscovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintFindNodeTypesTest::RunTest(const FString& Parameters)
{
    using namespace CompilerTestUtils;

    // 1) Discoverability: both proposed verbs must be registered so callers can find them and the
    //    dispatcher will route to them. Absent today -> these fail.
    TestTrue(TEXT("blueprint.graph.find_node_types is registered"),
        IsHandlerRegistered(TEXT("blueprint.graph.find_node_types")));
    TestTrue(TEXT("blueprint.graph.get_node_type_pins is registered"),
        IsHandlerRegistered(TEXT("blueprint.graph.get_node_type_pins")));

    // Fixture: a transient Actor Blueprint with an EventGraph — the graph the discovery is scoped
    // to. Built in-code (no on-disk content dependency). A missing fixture is a hard FAILURE.
    UBlueprint* BP = CreateTransientTestBP(TEXT("FindNodeTypes"));
    if (!BP || BP->UbergraphPages.Num() == 0)
    {
        AddError(TEXT("Failed to create transient test blueprint with an EventGraph"));
        return true; // hard failure, not a skip
    }
    UEdGraph* EventGraph = BP->UbergraphPages[0];
    if (!EventGraph)
    {
        AddError(TEXT("Transient blueprint has a null EventGraph"));
        return true; // hard failure, not a skip
    }

    // 2) find_node_types with a substring filter: the action-database search must succeed and
    //    return a non-empty type list. "Add" is a stable math/float family that the action
    //    database always populates for an Actor graph.
    TSharedPtr<FJsonObject> FindPayload = MakeShared<FJsonObject>();
    FindPayload->SetStringField(TEXT("assetPath"), BP->GetPathName());
    FindPayload->SetStringField(TEXT("graphName"), EventGraph->GetName());
    FindPayload->SetStringField(TEXT("filter"), TEXT("Add"));

    FTestResponseCapture FindCapture;
    const bool bFindFound = InvokeHandlerWithCapture(
        TEXT("blueprint.graph.find_node_types"), FindPayload, FindCapture);
    TestTrue(TEXT("find_node_types handler found"), bFindFound);
    TestTrue(TEXT("find_node_types call succeeds"), FindCapture.bSuccess);

    FString RoundTripTypeId;
    if (FindCapture.bSuccess && FindCapture.Result.IsValid())
    {
        // Concrete response shape (pinned now that the vocabulary is fixed): the array is
        // under "nodeTypes" and every entry carries a "typeId" and a "nodeClass".
        TestTrue(TEXT("find_node_types response uses the 'nodeTypes' array key"),
            FindCapture.Result->HasTypedField<EJson::Array>(TEXT("nodeTypes")));
        const TArray<TSharedPtr<FJsonValue>>* Types = FindTypesArray(FindCapture.Result);
        TestNotNull(TEXT("find_node_types returns a type array"), Types);
        if (Types)
        {
            TestTrue(TEXT("filtered search returns at least one candidate node type"), Types->Num() > 0);
            if (Types->Num() > 0)
            {
                const TSharedPtr<FJsonObject>* FirstObj = nullptr;
                if ((*Types)[0].IsValid() && (*Types)[0]->TryGetObject(FirstObj) && FirstObj && (*FirstObj).IsValid())
                {
                    FString NodeClass;
                    TestTrue(TEXT("a returned node type carries a nodeClass"),
                        (*FirstObj)->TryGetStringField(TEXT("nodeClass"), NodeClass) && !NodeClass.IsEmpty());
                }
                RoundTripTypeId = ExtractTypeId((*Types)[0]);
                TestTrue(TEXT("a returned node type carries a stable type id"), !RoundTripTypeId.IsEmpty());
            }
        }
    }

    // 3) Pin-context path: invoking find_node_types with a float output pin as context must still
    //    succeed (the contextPins compatibility-filter path is wired). Acceptance target: this
    //    returns float-accepting math nodes and excludes exec-only nodes.
    {
        TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
        Pin->SetStringField(TEXT("direction"), TEXT("output"));
        Pin->SetStringField(TEXT("pinCategory"), TEXT("real"));
        Pin->SetStringField(TEXT("pinSubCategory"), TEXT("float"));

        TArray<TSharedPtr<FJsonValue>> ContextPins;
        ContextPins.Add(MakeShared<FJsonValueObject>(Pin));

        TSharedPtr<FJsonObject> CtxPayload = MakeShared<FJsonObject>();
        CtxPayload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        CtxPayload->SetStringField(TEXT("graphName"), EventGraph->GetName());
        CtxPayload->SetStringField(TEXT("filter"), TEXT("Add"));
        CtxPayload->SetArrayField(TEXT("contextPins"), ContextPins);

        FTestResponseCapture CtxCapture;
        const bool bCtxFound = InvokeHandlerWithCapture(
            TEXT("blueprint.graph.find_node_types"), CtxPayload, CtxCapture);
        TestTrue(TEXT("find_node_types (pin-context) handler found"), bCtxFound);
        TestTrue(TEXT("find_node_types (pin-context) call succeeds"), CtxCapture.bSuccess);
        if (CtxCapture.bSuccess && CtxCapture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* CtxTypes = FindTypesArray(CtxCapture.Result);
            TestNotNull(TEXT("pin-context find_node_types returns a type array"), CtxTypes);
            if (CtxTypes)
            {
                TestTrue(TEXT("float-output context returns at least one compatible node type"),
                    CtxTypes->Num() > 0);
            }
        }
    }

    // 3b) Pin-context filtering must genuinely EXCLUDE an exec-only node. UK2Node_ExecutionSequence
    //     ("Sequence") carries only exec pins, so a float OUTPUT pin cannot legally connect to it:
    //     a real contextPins filter drops it, while an implementation that ignored contextPins would
    //     keep it. This is the differential guard against a stubbed / ignored pin-context path.
    auto FindNodeTypesHasExecutionSequence = [&](bool bWithFloatContext) -> bool
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("assetPath"), BP->GetPathName());
        P->SetStringField(TEXT("graphName"), EventGraph->GetName());
        P->SetStringField(TEXT("filter"), TEXT("Sequence"));
        if (bWithFloatContext)
        {
            TSharedPtr<FJsonObject> Pin = MakeShared<FJsonObject>();
            Pin->SetStringField(TEXT("direction"), TEXT("output"));
            Pin->SetStringField(TEXT("pinCategory"), TEXT("real"));
            Pin->SetStringField(TEXT("pinSubCategory"), TEXT("float"));
            TArray<TSharedPtr<FJsonValue>> CP;
            CP.Add(MakeShared<FJsonValueObject>(Pin));
            P->SetArrayField(TEXT("contextPins"), CP);
        }
        FTestResponseCapture Cap;
        if (!InvokeHandlerWithCapture(TEXT("blueprint.graph.find_node_types"), P, Cap)
            || !Cap.bSuccess || !Cap.Result.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Types = FindTypesArray(Cap.Result);
        if (!Types)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Entry : *Types)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            if (!Entry.IsValid() || !Entry->TryGetObject(Obj) || !Obj || !(*Obj).IsValid())
            {
                continue;
            }
            FString NodeClass, TypeId;
            (*Obj)->TryGetStringField(TEXT("nodeClass"), NodeClass);
            (*Obj)->TryGetStringField(TEXT("typeId"), TypeId);
            if (NodeClass.Contains(TEXT("ExecutionSequence")) || TypeId.Contains(TEXT("ExecutionSequence")))
            {
                return true;
            }
        }
        return false;
    };
    TestTrue(TEXT("exec-only Sequence node is discoverable without a pin context"),
        FindNodeTypesHasExecutionSequence(false));
    TestFalse(TEXT("a float-output pin context excludes the exec-only Sequence node"),
        FindNodeTypesHasExecutionSequence(true));

    // 3c) NON-FUNCTION type-id round-trip: a type id for a non-function (flow-control) spawner must
    //     resolve through get_node_type_pins back to the SAME node class — not collapse onto an
    //     arbitrary sibling spawner sharing that UK2Node subclass. UK2Node_ExecutionSequence is a
    //     non-function spawner, so this guards the non-function half of the "stable type ids"
    //     contract that the function-only test above cannot cover.
    {
        TSharedPtr<FJsonObject> SeqPayload = MakeShared<FJsonObject>();
        SeqPayload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        SeqPayload->SetStringField(TEXT("graphName"), EventGraph->GetName());
        SeqPayload->SetStringField(TEXT("filter"), TEXT("Sequence"));

        FString SeqTypeId;
        FTestResponseCapture SeqCap;
        if (InvokeHandlerWithCapture(TEXT("blueprint.graph.find_node_types"), SeqPayload, SeqCap)
            && SeqCap.bSuccess && SeqCap.Result.IsValid())
        {
            if (const TArray<TSharedPtr<FJsonValue>>* SeqTypes = FindTypesArray(SeqCap.Result))
            {
                for (const TSharedPtr<FJsonValue>& Entry : *SeqTypes)
                {
                    const TSharedPtr<FJsonObject>* Obj = nullptr;
                    if (!Entry.IsValid() || !Entry->TryGetObject(Obj) || !Obj || !(*Obj).IsValid())
                    {
                        continue;
                    }
                    FString NodeClass;
                    (*Obj)->TryGetStringField(TEXT("nodeClass"), NodeClass);
                    if (NodeClass.Contains(TEXT("ExecutionSequence")))
                    {
                        SeqTypeId = ExtractTypeId(Entry);
                        break;
                    }
                }
            }
        }
        TestTrue(TEXT("a non-function (ExecutionSequence) type id is discoverable"), !SeqTypeId.IsEmpty());
        if (!SeqTypeId.IsEmpty())
        {
            TSharedPtr<FJsonObject> SeqPinsPayload = MakeShared<FJsonObject>();
            SeqPinsPayload->SetStringField(TEXT("typeId"), SeqTypeId);

            FTestResponseCapture SeqPinsCap;
            const bool bSeqPinsFound = InvokeHandlerWithCapture(
                TEXT("blueprint.graph.get_node_type_pins"), SeqPinsPayload, SeqPinsCap);
            TestTrue(TEXT("get_node_type_pins handler found (non-function id)"), bSeqPinsFound);
            TestTrue(TEXT("get_node_type_pins succeeds for a non-function type id"), SeqPinsCap.bSuccess);
            if (SeqPinsCap.bSuccess && SeqPinsCap.Result.IsValid())
            {
                FString ResolvedNodeClass;
                SeqPinsCap.Result->TryGetStringField(TEXT("nodeClass"), ResolvedNodeClass);
                TestTrue(TEXT("non-function type id round-trips back to an ExecutionSequence node"),
                    ResolvedNodeClass.Contains(TEXT("ExecutionSequence")));
            }
        }
    }

    // 3d) MULTI-SPAWNER ANTI-COLLAPSE: distinct non-function spawners that share ONE UK2Node
    //     subclass must surface as SEPARATE entries with DISTINCT type ids. The action database
    //     registers a distinct UK2Node_DynamicCast spawner per castable object class ("Cast To
    //     Actor", "Cast To Pawn", ...), all reporting the SAME nodeClass "K2Node_DynamicCast".
    //     The precise anti-collapse invariant is therefore: at least TWO returned entries share
    //     one EXACT nodeClass string yet carry DISTINCT type ids. Under the pre-fix scheme (id
    //     keyed on the node class path) any two same-class spawners produced the SAME id and the
    //     dedup dropped all but one — so NO nodeClass could ever appear on two returned entries;
    //     grouping returned ids by nodeClass and finding a group of >=2 is the exact property the
    //     collapse made impossible. Each such id must also round-trip back to that same node class
    //     (an id must not resolve to an arbitrary sibling). This is the differential guard.
    {
        TSharedPtr<FJsonObject> CastPayload = MakeShared<FJsonObject>();
        CastPayload->SetStringField(TEXT("assetPath"), BP->GetPathName());
        CastPayload->SetStringField(TEXT("graphName"), EventGraph->GetName());
        CastPayload->SetStringField(TEXT("filter"), TEXT("Cast"));
        CastPayload->SetNumberField(TEXT("limit"), 2000);

        // nodeClass -> distinct type ids seen for it.
        TMap<FString, TSet<FString>> IdsByNodeClass;
        FTestResponseCapture CastCap;
        if (InvokeHandlerWithCapture(TEXT("blueprint.graph.find_node_types"), CastPayload, CastCap)
            && CastCap.bSuccess && CastCap.Result.IsValid())
        {
            if (const TArray<TSharedPtr<FJsonValue>>* CastTypes = FindTypesArray(CastCap.Result))
            {
                for (const TSharedPtr<FJsonValue>& Entry : *CastTypes)
                {
                    const TSharedPtr<FJsonObject>* Obj = nullptr;
                    if (!Entry.IsValid() || !Entry->TryGetObject(Obj) || !Obj || !(*Obj).IsValid())
                    {
                        continue;
                    }
                    FString NodeClass;
                    (*Obj)->TryGetStringField(TEXT("nodeClass"), NodeClass);
                    const FString Id = ExtractTypeId(Entry);
                    if (!NodeClass.IsEmpty() && !Id.IsEmpty())
                    {
                        IdsByNodeClass.FindOrAdd(NodeClass).Add(Id);
                    }
                }
            }
        }

        // Restrict to the DynamicCast family — a NON-function node class. (Function spawners share
        // nodeClass "K2Node_CallFunction" but were never collapsed: they keep a meaningful
        // "function:<Owner>:<Fn>" id, so a CallFunction group of >=2 proves nothing about the fix.
        // Object casts to distinct classes all report nodeClass "K2Node_DynamicCast" and are exactly
        // the non-function family the pre-fix scheme collapsed.) Pick the DynamicCast node class
        // carried by the most distinct returned entries.
        FString SharedNodeClass;
        TArray<FString> SharedIds;
        for (const TPair<FString, TSet<FString>>& Pair : IdsByNodeClass)
        {
            if (Pair.Key.Contains(TEXT("DynamicCast")) && Pair.Value.Num() > SharedIds.Num())
            {
                SharedNodeClass = Pair.Key;
                SharedIds = Pair.Value.Array();
            }
        }

        // Under the fix, many casts share nodeClass "K2Node_DynamicCast" as distinct entries;
        // under the pre-fix collapse this group can never exceed one, so this assertion is RED
        // pre-fix and GREEN post-fix.
        TestTrue(TEXT("two+ spawners sharing one node class surface as distinct entries (anti-collapse)"),
            SharedIds.Num() >= 2);

        // Each such distinct id must round-trip via get_node_type_pins back to that SAME node class
        // (not an arbitrary sibling), proving the id maps 1:1 to its own spawner.
        int32 CastRoundTrips = 0;
        for (const FString& CastId : SharedIds)
        {
            if (CastRoundTrips >= 2)
            {
                break;
            }
            TSharedPtr<FJsonObject> CastPinsPayload = MakeShared<FJsonObject>();
            CastPinsPayload->SetStringField(TEXT("typeId"), CastId);
            FTestResponseCapture CastPinsCap;
            if (InvokeHandlerWithCapture(TEXT("blueprint.graph.get_node_type_pins"), CastPinsPayload, CastPinsCap)
                && CastPinsCap.bSuccess && CastPinsCap.Result.IsValid())
            {
                FString ResolvedClass;
                CastPinsCap.Result->TryGetStringField(TEXT("nodeClass"), ResolvedClass);
                TestEqual(TEXT("a same-class type id round-trips to its own node class"),
                    ResolvedClass, SharedNodeClass);
                ++CastRoundTrips;
            }
        }
        TestTrue(TEXT("at least two distinct same-class type ids round-trip through get_node_type_pins"),
            CastRoundTrips >= 2);
    }

    // 4) Type-id round-trip: a type id returned by find_node_types must resolve through
    //    get_node_type_pins to an expected pin list (the ticket's "stable type ids" property).
    if (!RoundTripTypeId.IsEmpty())
    {
        TSharedPtr<FJsonObject> PinsPayload = MakeShared<FJsonObject>();
        PinsPayload->SetStringField(TEXT("typeId"), RoundTripTypeId);

        FTestResponseCapture PinsCapture;
        const bool bPinsFound = InvokeHandlerWithCapture(
            TEXT("blueprint.graph.get_node_type_pins"), PinsPayload, PinsCapture);
        TestTrue(TEXT("get_node_type_pins handler found"), bPinsFound);
        TestTrue(TEXT("get_node_type_pins call succeeds for a find_node_types type id"),
            PinsCapture.bSuccess);
        if (PinsCapture.bSuccess && PinsCapture.Result.IsValid())
        {
            const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
            TestTrue(TEXT("get_node_type_pins returns a pins array"),
                PinsCapture.Result->TryGetArrayField(TEXT("pins"), Pins) && Pins != nullptr);
        }
    }
    else
    {
        // The round-trip cannot even be attempted without the discovery verb — record the miss so
        // this half of the acceptance surface is visibly unproven pre-fix.
        AddError(TEXT("No type id available from find_node_types to round-trip through get_node_type_pins"));
    }

    return true;
}
