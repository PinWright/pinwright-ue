// Copyright (c) 2026 Alexander Penkin. MIT License.

// BlueprintNodeDiscoveryHandler.cpp
// blueprint.graph.find_node_types    — search the Blueprint action database for spawnable
//                                       node types, with optional pin-type-compatibility
//                                       (contextPins) filtering.
// blueprint.graph.get_node_type_pins — the expected pin list for a discovered type id
//                                       (spawn-and-scan a real node, as Epic does).
//
// These go BEYOND the two existing PARTIAL-discovery surfaces (board F-blueprint-node-discovery):
//   - blueprint.graph.list_node_types : a flat TObjectIterator<UClass> over UK2Node
//                                        container classes (no filter, no pins).
//   - blueprint.search_api            : keyword search over reflected FUNC_BlueprintCallable
//                                        UFunctions ONLY (BlueprintApiIndexHandler.cpp:86).
// The novel capability is FBlueprintActionDatabase breadth (function AND non-function
// spawnable actions: flow control, casts, variable get/set, events, macros) plus pin-context
// compatibility filtering — the query the editor's drag-off-a-pin menu answers — which
// neither existing surface provides. find_node_types COMPLEMENTS search_api; it does not
// replace it.
//
// Pin reads use UBlueprintNodeSpawner::Invoke into a transient scratch graph, NOT the template
// cache (FBlueprintNodeTemplateCache): the cache asserts on a transient user graph and returns
// null on a cold cache headlessly, whereas Invoke spawns a fully-pinned node with no such
// restriction.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"
#include "Handlers/Blueprint/BlueprintGraphHelpers.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using BlueprintGraphHelpers::ResolveBlueprintAndGraph;

namespace
{
    // Bound the worst case for an unfiltered contextPins query (each candidate is spawned).
    constexpr int32 GMaxContextProbes = 4000;

    // ---- Stable, round-trippable identity for a spawnable node type -----------------
    // Function spawners -> "function:<OwnerClassPath>:<FunctionName>" (meaningful and stable
    // across sessions). Every OTHER spawner -> "spawner:<SpawnerPath>". Keying non-function
    // actions on their node class (the old "node:<NodeClassPath>") would collapse every distinct
    // action that shares a UK2Node subclass — each cast target (UK2Node_DynamicCast), every
    // variable get/set (UK2Node_VariableGet/Set), every macro instance (UK2Node_MacroInstance) —
    // into ONE id, dropping all but the first at the dedup and making get_node_type_pins resolve
    // to an arbitrary sibling. Each action is instead a distinct UObject held by
    // FBlueprintActionDatabase for the session, so its path is a unique id that recomputes
    // identically on the get_node_type_pins rescan. Derived purely from the spawner, so
    // find_node_types and get_node_type_pins agree on the vocabulary with no shared mutable state.
    FString MakeSpawnerTypeId(const UBlueprintNodeSpawner* Spawner)
    {
        if (!Spawner)
        {
            return FString();
        }
        if (const UBlueprintFunctionNodeSpawner* FnSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
        {
            if (const UFunction* Fn = FnSpawner->GetFunction())
            {
                if (const UClass* Owner = Fn->GetOwnerClass())
                {
                    return FString::Printf(TEXT("function:%s:%s"), *Owner->GetPathName(), *Fn->GetName());
                }
            }
        }
        return FString::Printf(TEXT("spawner:%s"), *Spawner->GetPathName());
    }

    // Cheap searchable text for a spawner WITHOUT spawning a node: node class name +
    // (function name for function spawners) + menu name. Keeps the full-database scan fast —
    // a candidate is only spawned (Invoke) for the substring-matched subset.
    FString MakeCheapSearchText(const UBlueprintNodeSpawner* Spawner)
    {
        FString Text;
        if (const UClass* NodeClass = Spawner->NodeClass.Get())
        {
            Text += NodeClass->GetName();
            Text += TEXT(" ");
        }
        if (const UBlueprintFunctionNodeSpawner* FnSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
        {
            if (const UFunction* Fn = FnSpawner->GetFunction())
            {
                Text += Fn->GetName();
                Text += TEXT(" ");
            }
        }
        if (!Spawner->DefaultMenuSignature.MenuName.IsEmpty())
        {
            Text += Spawner->DefaultMenuSignature.MenuName.ToString();
        }
        return Text;
    }

    // Human-readable display name without a template spawn: menu name (populated at
    // registration for function spawners) -> function name -> node class display -> class name.
    FString MakeDisplayName(const UBlueprintNodeSpawner* Spawner)
    {
        if (!Spawner->DefaultMenuSignature.MenuName.IsEmpty())
        {
            return Spawner->DefaultMenuSignature.MenuName.ToString();
        }
        if (const UBlueprintFunctionNodeSpawner* FnSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
        {
            if (const UFunction* Fn = FnSpawner->GetFunction())
            {
                return Fn->GetName();
            }
        }
        if (const UClass* NodeClass = Spawner->NodeClass.Get())
        {
            const FString Display = NodeClass->GetDisplayNameText().ToString();
            return Display.IsEmpty() ? NodeClass->GetName() : Display;
        }
        return FString();
    }

    FString PinDirectionString(EEdGraphPinDirection Dir)
    {
        return Dir == EGPD_Output ? TEXT("output") : TEXT("input");
    }

    TSharedPtr<FJsonObject> BuildPinInfoJson(const UEdGraphPin* Pin)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
        Obj->SetStringField(TEXT("direction"), PinDirectionString(Pin->Direction));
        Obj->SetStringField(TEXT("pinCategory"), Pin->PinType.PinCategory.ToString());
        if (!Pin->PinType.PinSubCategory.IsNone())
        {
            Obj->SetStringField(TEXT("pinSubCategory"), Pin->PinType.PinSubCategory.ToString());
        }
        if (Pin->PinType.PinSubCategoryObject.IsValid())
        {
            Obj->SetStringField(TEXT("pinSubCategoryObject"), Pin->PinType.PinSubCategoryObject->GetName());
        }
        switch (Pin->PinType.ContainerType)
        {
            case EPinContainerType::Array: Obj->SetStringField(TEXT("container"), TEXT("array")); break;
            case EPinContainerType::Set:   Obj->SetStringField(TEXT("container"), TEXT("set"));   break;
            case EPinContainerType::Map:   Obj->SetStringField(TEXT("container"), TEXT("map"));   break;
            default: break;
        }
        return Obj;
    }

    // A parsed context pin: its type plus the direction of the pin the CALLER is holding.
    struct FContextPin
    {
        FEdGraphPinType Type;
        EEdGraphPinDirection Direction = EGPD_Output;
    };

    // Build an FEdGraphPinType from a contextPins JSON entry. UE5 unified float/double to
    // PC_Real, so a caller-supplied "float"/"double"/"real" category maps to PC_Real (with a
    // float/double subcategory) — what BP "float" variable pins and KismetMathLibrary math-node
    // pins actually are, so compatibility resolves as the editor's drag menu does.
    bool ParseContextPin(const TSharedPtr<FJsonObject>& PinObj, FContextPin& Out)
    {
        if (!PinObj.IsValid())
        {
            return false;
        }
        FString Category, SubCategory, Direction;
        PinObj->TryGetStringField(TEXT("pinCategory"), Category);
        PinObj->TryGetStringField(TEXT("pinSubCategory"), SubCategory);
        PinObj->TryGetStringField(TEXT("direction"), Direction);
        if (Category.IsEmpty())
        {
            return false;
        }

        Out.Direction = Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase) ? EGPD_Input : EGPD_Output;

        if (Category.Equals(TEXT("float"), ESearchCase::IgnoreCase)
            || Category.Equals(TEXT("double"), ESearchCase::IgnoreCase)
            || Category.Equals(TEXT("real"), ESearchCase::IgnoreCase))
        {
            Out.Type.PinCategory = UEdGraphSchema_K2::PC_Real;
            if (SubCategory.IsEmpty())
            {
                SubCategory = Category.Equals(TEXT("double"), ESearchCase::IgnoreCase)
                    ? UEdGraphSchema_K2::PC_Double.ToString()
                    : UEdGraphSchema_K2::PC_Float.ToString();
            }
        }
        else
        {
            Out.Type.PinCategory = FName(*Category);
        }
        if (!SubCategory.IsEmpty())
        {
            Out.Type.PinSubCategory = FName(*SubCategory);
        }
        return true;
    }

    // True iff Node has a pin on the opposite side of the context pin whose type is
    // compatible with the context pin. (context OUTPUT -> Node must accept it on an INPUT;
    // context INPUT -> Node must produce it on an OUTPUT.)
    bool NodeAcceptsContextPin(const UEdGraphNode* Node, const FContextPin& Ctx)
    {
        if (!Node)
        {
            return false;
        }
        const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
        const EEdGraphPinDirection Want = (Ctx.Direction == EGPD_Output) ? EGPD_Input : EGPD_Output;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != Want)
            {
                continue;
            }
            const FEdGraphPinType& OutT = (Ctx.Direction == EGPD_Output) ? Ctx.Type : Pin->PinType;
            const FEdGraphPinType& InT  = (Ctx.Direction == EGPD_Output) ? Pin->PinType : Ctx.Type;
            if (Schema->ArePinTypesCompatible(OutT, InT))
            {
                return true;
            }
            // UE5 real pins (float/double subcategories) interconvert — treat any scalar
            // real<->real as compatible so a "float" context matches "double"-typed math pins.
            if (OutT.PinCategory == UEdGraphSchema_K2::PC_Real
                && InT.PinCategory == UEdGraphSchema_K2::PC_Real
                && OutT.ContainerType == EPinContainerType::None
                && InT.ContainerType == EPinContainerType::None)
            {
                return true;
            }
        }
        return false;
    }

    // Node passes the filter iff it is compatible with EVERY context pin (mirrors
    // FBlueprintActionContext: an action must be viable for every pin in the context).
    bool NodeMatchesAllContextPins(const UEdGraphNode* Node, const TArray<FContextPin>& ContextPins)
    {
        for (const FContextPin& Ctx : ContextPins)
        {
            if (!NodeAcceptsContextPin(Node, Ctx))
            {
                return false;
            }
        }
        return true;
    }

    // Create a transient scratch blueprint whose event graph hosts spawn-and-scan probe nodes.
    // Transient is fine for Invoke (unlike the template cache). Returns the event graph, or null
    // on failure; OutScratchBP receives the owning blueprint so the caller's local keeps it (and
    // thus the returned graph) referenced for the synchronous call — GC does not run mid-dispatch,
    // and both are collected as transient garbage after the handler returns.
    UEdGraph* MakeScratchProbeGraph(UBlueprint*& OutScratchBP)
    {
        OutScratchBP = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(),
            GetTransientPackage(),
            MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("PinWrightNodeProbe")),
            BPTYPE_Normal,
            UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
        if (!OutScratchBP || OutScratchBP->UbergraphPages.Num() == 0)
        {
            return nullptr;
        }
        return OutScratchBP->UbergraphPages[0];
    }

    // Spawn a REAL node for the spawner into ScratchGraph, with pins allocated. Uses Invoke
    // (not the template cache) so it works headlessly on a transient graph.
    UEdGraphNode* SpawnProbeNode(UBlueprintNodeSpawner* Spawner, UEdGraph* ScratchGraph)
    {
        if (!Spawner || !ScratchGraph)
        {
            return nullptr;
        }
        return Spawner->Invoke(ScratchGraph, IBlueprintNodeBinder::FBindingSet(), FVector2D::ZeroVector);
    }
} // namespace

// ---- blueprint.graph.find_node_types --------------------------------------------------
REGISTER_RPC_HANDLER("blueprint.graph.find_node_types", "blueprint.graph",
    "Search the Blueprint action database for spawnable node types, optionally filtered by pin-type compatibility with context pins",
    RPC_PARAMS(
        BlueprintHandlerUtils::BlueprintPathParamReq(TEXT("assetPath"), TEXT("path"), TEXT("Blueprint asset path")),
        RPC_PARAM_OPT("graphName", "string", "Graph name (accepted; discovery is not graph-scoped in v1)"),
        RPC_PARAM_OPT("filter", "string", "Case-insensitive substring matched against node/function/menu names"),
        RPC_PARAM_OPT("contextPins", "array", "Pins to filter candidates by type compatibility; each entry {direction,pinCategory,pinSubCategory}"),
        RPC_PARAM_DEF("limit", "number", "Maximum number of node types to return", "200")
    ))
{
    UBlueprint* Blueprint = nullptr;
    UEdGraph* Unused = nullptr;
    if (!ResolveBlueprintAndGraph(Ctx, Blueprint, Unused, /*bGraphRequired=*/false))
    {
        return true;
    }

    const FString Filter = Ctx.GetString(TEXT("filter"));
    int32 Limit = Ctx.GetInt(TEXT("limit"), 200);
    if (Limit <= 0)
    {
        Limit = 200;
    }

    // Parse optional pin-type-compatibility context.
    TArray<FContextPin> ContextPins;
    if (const TArray<TSharedPtr<FJsonValue>>* ContextArray = Ctx.GetArray(TEXT("contextPins")))
    {
        for (const TSharedPtr<FJsonValue>& Val : *ContextArray)
        {
            const TSharedPtr<FJsonObject>* PinObj = nullptr;
            if (Val.IsValid() && Val->TryGetObject(PinObj) && PinObj)
            {
                FContextPin Parsed;
                if (ParseContextPin(*PinObj, Parsed))
                {
                    ContextPins.Add(Parsed);
                }
                else
                {
                    Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                        TEXT("Each contextPins entry needs a non-empty 'pinCategory' (e.g. {\"direction\":\"output\",\"pinCategory\":\"real\",\"pinSubCategory\":\"float\"})."));
                    return true;
                }
            }
        }
    }
    const bool bContextFiltered = ContextPins.Num() > 0;

    // A transient K2 scratch graph both (a) gates candidates to node types placeable in a
    // Blueprint event graph — excluding anim/widget/control-rig/etc. nodes that require a
    // different graph type and would fatally assert on spawn — and (b) hosts pin-context probes.
    UBlueprint* ScratchBP = nullptr;
    UEdGraph* ScratchGraph = MakeScratchProbeGraph(ScratchBP);
    if (!ScratchGraph)
    {
        Ctx.SendError(TEXT("GRAPH_UNAVAILABLE"), TEXT("Could not create a scratch graph for node discovery."));
        return true;
    }

    const FBlueprintActionDatabase::FActionRegistry& Registry = FBlueprintActionDatabase::Get().GetAllActions();

    TArray<TSharedPtr<FJsonValue>> NodeTypes;
    TSet<FString> SeenTypeIds;
    int32 ContextProbes = 0;
    bool bTruncated = false;

    for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Entry : Registry)
    {
        if (bTruncated)
        {
            break;
        }
        for (const TObjectPtr<UBlueprintNodeSpawner>& SpawnerPtr : Entry.Value)
        {
            UBlueprintNodeSpawner* Spawner = SpawnerPtr.Get();
            if (!Spawner || !Spawner->NodeClass.Get())
            {
                continue;
            }

            // Cheap substring pre-filter (no node spawn).
            if (!Filter.IsEmpty() && !MakeCheapSearchText(Spawner).Contains(Filter, ESearchCase::IgnoreCase))
            {
                continue;
            }

            const FString TypeId = MakeSpawnerTypeId(Spawner);
            if (TypeId.IsEmpty() || SeenTypeIds.Contains(TypeId))
            {
                continue;
            }

            // Gate to node types placeable in a Blueprint event graph (schema compatibility).
            // Excludes anim/widget/control-rig/etc. nodes bound to other graph types — both so
            // results are relevant and so the Invoke below never spawns a node whose post-spawn
            // setup fatally asserts on the wrong blueprint type.
            UEdGraphNode* NodeCDO = Spawner->NodeClass.GetDefaultObject();
            if (!NodeCDO || !NodeCDO->IsCompatibleWithGraph(ScratchGraph))
            {
                continue;
            }

            // Pin-context compatibility: spawn a real probe node (only for the pre-filtered
            // subset) and keep the candidate only if a pin accepts every context pin.
            if (bContextFiltered)
            {
                if (ContextProbes >= GMaxContextProbes)
                {
                    bTruncated = true;
                    break;
                }
                ++ContextProbes;
                UEdGraphNode* Probe = SpawnProbeNode(Spawner, ScratchGraph);
                const bool bMatches = Probe && NodeMatchesAllContextPins(Probe, ContextPins);
                if (Probe)
                {
                    ScratchGraph->RemoveNode(Probe);
                }
                if (!bMatches)
                {
                    continue;
                }
            }

            TSharedPtr<FJsonObject> TypeObj = MakeShared<FJsonObject>();
            TypeObj->SetStringField(TEXT("typeId"), TypeId);
            TypeObj->SetStringField(TEXT("displayName"), MakeDisplayName(Spawner));
            TypeObj->SetStringField(TEXT("category"), Spawner->DefaultMenuSignature.Category.ToString());
            TypeObj->SetStringField(TEXT("nodeClass"), Spawner->NodeClass->GetName());
            NodeTypes.Add(MakeShared<FJsonValueObject>(TypeObj));
            SeenTypeIds.Add(TypeId);

            if (NodeTypes.Num() >= Limit)
            {
                bTruncated = true;
                break;
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("nodeTypes"), NodeTypes);
    Result->SetNumberField(TEXT("count"), NodeTypes.Num());
    Result->SetStringField(TEXT("filter"), Filter);
    Result->SetBoolField(TEXT("contextFiltered"), bContextFiltered);
    if (bTruncated)
    {
        Result->SetBoolField(TEXT("truncated"), true);
    }
    AddAssetVerification(Result, Blueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- blueprint.graph.get_node_type_pins -----------------------------------------------
REGISTER_RPC_HANDLER("blueprint.graph.get_node_type_pins", "blueprint.graph",
    "Return the expected pin list for a node type id from blueprint.graph.find_node_types (spawn-and-scan)",
    RPC_PARAMS(
        RPC_PARAM_REQ("typeId", "string", "Stable node type id returned by blueprint.graph.find_node_types")
    ))
{
    FString TypeId;
    if (!Ctx.RequireString(TEXT("typeId"), TypeId))
    {
        return true;
    }

    // Locate the action-database spawner whose identity matches typeId. The spawner knows how
    // to build a correct node for its type (function call, cast, variable get/set, flow control,
    // ...), so we reuse it rather than re-deriving pins by hand.
    const FBlueprintActionDatabase::FActionRegistry& Registry = FBlueprintActionDatabase::Get().GetAllActions();
    UBlueprintNodeSpawner* MatchedSpawner = nullptr;
    for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Entry : Registry)
    {
        for (const TObjectPtr<UBlueprintNodeSpawner>& SpawnerPtr : Entry.Value)
        {
            UBlueprintNodeSpawner* Spawner = SpawnerPtr.Get();
            if (Spawner && MakeSpawnerTypeId(Spawner) == TypeId)
            {
                MatchedSpawner = Spawner;
                break;
            }
        }
        if (MatchedSpawner)
        {
            break;
        }
    }

    if (!MatchedSpawner)
    {
        Ctx.SendError(TEXT("NODE_TYPE_NOT_FOUND"),
            FString::Printf(TEXT("No spawnable node type matches id '%s'. Pass a typeId returned by blueprint.graph.find_node_types."), *TypeId));
        return true;
    }

    // Spawn-and-scan: build a real node for the spawner in a transient scratch graph and read
    // its pins. Invoke (not the template cache) so this works headlessly on a transient graph.
    UBlueprint* ScratchBP = nullptr;
    UEdGraph* ScratchGraph = MakeScratchProbeGraph(ScratchBP);
    if (!ScratchGraph)
    {
        Ctx.SendError(TEXT("GRAPH_UNAVAILABLE"), TEXT("Could not create a scratch graph to inspect node pins."));
        return true;
    }

    // Gate to a node type placeable in a Blueprint event graph before spawning — inspecting a
    // node bound to another graph type (anim/widget/...) would fatally assert on spawn here.
    UEdGraphNode* NodeCDO = MatchedSpawner->NodeClass.GetDefaultObject();
    if (!NodeCDO || !NodeCDO->IsCompatibleWithGraph(ScratchGraph))
    {
        Ctx.SendError(TEXT("NODE_TYPE_NOT_FOUND"),
            FString::Printf(TEXT("Node type '%s' is not placeable in a Blueprint event graph, so its pins cannot be inspected here."), *TypeId));
        return true;
    }

    const UEdGraphNode* Probe = SpawnProbeNode(MatchedSpawner, ScratchGraph);
    if (!Probe)
    {
        Ctx.SendError(TEXT("NODE_TYPE_NOT_FOUND"),
            FString::Printf(TEXT("Node type '%s' could not be instantiated for pin inspection."), *TypeId));
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Pins;
    for (const UEdGraphPin* Pin : Probe->Pins)
    {
        if (Pin)
        {
            Pins.Add(MakeShared<FJsonValueObject>(BuildPinInfoJson(Pin)));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("typeId"), TypeId);
    Result->SetStringField(TEXT("nodeClass"), Probe->GetClass()->GetName());
    Result->SetArrayField(TEXT("pins"), Pins);
    Result->SetNumberField(TEXT("pinCount"), Pins.Num());
    Ctx.SendSuccess(Result);
    return true;
}
