// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamAliasUtils.h"
#include "PinWrightSubsystem.h"
#include "PinWrightHelpers.h"

#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/Decorators/BTDecorator_BlueprintBase.h"
#include "BehaviorTree/Decorators/BTDecorator_Blackboard.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Services/BTService_BlueprintBase.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeTypes.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/Tasks/BTTask_BlueprintBase.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/Tasks/BTTask_FinishWithResult.h"
#include "BehaviorTree/Tasks/BTTask_MoveTo.h"
#include "BehaviorTree/Tasks/BTTask_RotateToFaceBBEntry.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Utils/AssetCreatePolicy.h"
#include "Utils/AssetUtils.h"
#include "Utils/PropertyChangeNotify.h"
#include "Utils/PropertyUtils.h"
#include "Compat/JsonKeyCompat.h"

#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "Handlers/AI/BehaviorTreeGraphNodeCompat.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "AssetToolsModule.h"
#include "Factories/BlueprintFactory.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectHash.h"
#include "Utils/JsonUtils.h"

// Accepted wire spellings of the behavior-tree asset slot, canonical first. Every graph verb in
// this file already reads the value with this list through GetStringFirstOf, so the spec has to
// declare all three or the dispatcher's unknown-param gate refuses the alternates with
// UNKNOWN_PARAMS before the body runs.
static const TArray<FString>& BTAssetPathKeys()
{
    static const TArray<FString> Keys = {
        TEXT("assetPath"),
        TEXT("behaviorTreePath"),
        TEXT("path")
    };
    return Keys;
}

static FParamSpec BTAssetPathParamReq()
{
    return ParamAliasUtils::MakeAliasParamSpec(TEXT("assetPath"), TEXT("path"),
        TEXT("Path to the Behavior Tree asset. The behaviorTreePath and path spellings are also accepted."),
        /*bRequired=*/true, BTAssetPathKeys());
}

// Shared helper: Find a graph node by GUID, graph-node name/path, or node-instance name
static UEdGraphNode* FindBTGraphNode(UEdGraph* BTGraph, const FString& IdOrName)
{
    if (IdOrName.IsEmpty() || !BTGraph) return nullptr;
    const FString Needle = IdOrName.TrimStartAndEnd();

    for (UEdGraphNode* Node : BTGraph->Nodes)
    {
        if (!Node) continue;
        if (Node->NodeGuid.ToString() == Needle) return Node;
        FGuid SearchGuid;
        if (FGuid::Parse(Needle, SearchGuid) && Node->NodeGuid == SearchGuid) return Node;
        if (Node->GetName().Equals(Needle, ESearchCase::IgnoreCase)) return Node;
        if (Node->GetPathName().Equals(Needle, ESearchCase::IgnoreCase)) return Node;
    }

    // Second pass so a graph-node identity always wins: the NodeInstance object name
    // (`BTT_Reload_C_0`) is what asset dumps and BT runtime logs print, and callers reach for
    // it as an id. It is unique within the tree because every instance is outered to the same
    // UBehaviorTree, so accepting it here is unambiguous once the pass above has found nothing.
    for (UEdGraphNode* Node : BTGraph->Nodes)
    {
        UBehaviorTreeGraphNode* BTNode = PinWright::BehaviorTree::CastGraphNode(Node);
        const UObject* Instance = BTNode ? BTNode->NodeInstance.Get() : nullptr;
        if (Instance && Instance->GetName().Equals(Needle, ESearchCase::IgnoreCase)) return Node;
    }
    return nullptr;
}

// Locate the hidden Root entry node that CreateDefaultNodesForGraph seeds.
static UBehaviorTreeGraphNode_Root* FindBTRootNode(UEdGraph* BTGraph)
{
    if (!BTGraph) return nullptr;
    for (UEdGraphNode* Node : BTGraph->Nodes)
    {
        if (UBehaviorTreeGraphNode_Root* Root = Cast<UBehaviorTreeGraphNode_Root>(Node))
        {
            return Root;
        }
    }
    return nullptr;
}

static UClass* ResolveBTNativeSubNodeClass(const FString& Trimmed, UClass* RequiredBaseClass, const TCHAR* NativePrefix)
{
    if (Trimmed.IsEmpty() || Trimmed.Contains(TEXT("/")) || Trimmed.Contains(TEXT(".")))
    {
        return nullptr;
    }

    FString NativeName = Trimmed;
    NativeName.RemoveFromStart(TEXT("U"));
    if (!NativeName.StartsWith(NativePrefix))
    {
        NativeName = FString(NativePrefix) + NativeName;
    }

    const FString NativePath = FString::Printf(TEXT("/Script/AIModule.%s"), *NativeName);
    if (UClass* NativeClass = FindObject<UClass>(nullptr, *NativePath))
    {
        return NativeClass->IsChildOf(RequiredBaseClass) ? NativeClass : nullptr;
    }
    if (UClass* NativeClass = LoadObject<UClass>(nullptr, *NativePath))
    {
        return NativeClass->IsChildOf(RequiredBaseClass) ? NativeClass : nullptr;
    }

    return nullptr;
}

static UClass* ResolveBTSubNodeClass(const FString& ClassSpec, UClass* RequiredBaseClass, const TCHAR* NativePrefix, UClass* DefaultClass)
{
    const FString Trimmed = ClassSpec.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        return DefaultClass;
    }

    if (UClass* NativeClass = ResolveBTNativeSubNodeClass(Trimmed, RequiredBaseClass, NativePrefix))
    {
        return NativeClass;
    }

    if (UClass* Resolved = ResolveClassByName(Trimmed))
    {
        return Resolved->IsChildOf(RequiredBaseClass) ? Resolved : nullptr;
    }

    return nullptr;
}

// Enumerate the short native names of every concrete (instantiable) subnode class
// under RequiredBaseClass, with NativePrefix stripped so the names round-trip back
// into ResolveBTSubNodeClass. Used to populate the INVALID_CLASS recovery hint when
// a requested decorator/service name does not resolve — the same enumerate-concrete-
// candidates convention AIHandler uses for SmartObject behaviorType resolution.
// Abstract bases (e.g. UBTService_BlackboardBase) are filtered out: they are exactly
// the names that look plausible but cannot be attached, so listing them would re-send
// the caller down the same dead end.
static TArray<FString> EnumerateConcreteBTSubNodeNames(UClass* RequiredBaseClass, const TCHAR* NativePrefix)
{
    TArray<FString> Names;
    if (!RequiredBaseClass)
    {
        return Names;
    }

    TArray<UClass*> Derived;
    GetDerivedClasses(RequiredBaseClass, Derived, /*bRecursive*/ true);
    for (UClass* Candidate : Derived)
    {
        if (!Candidate || Candidate == RequiredBaseClass)
        {
            continue;
        }
        if (Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            continue;
        }
        // Only surface native AIModule classes by short name — Blueprint-generated
        // subnodes resolve by asset path, not by the short-name convention the hint
        // teaches, so listing their generated class names would mislead.
        if (!Candidate->IsNative())
        {
            continue;
        }
        FString ShortName = Candidate->GetName();
        ShortName.RemoveFromStart(NativePrefix);
        // Distinct native classes have distinct names, and stripping a fixed prefix
        // keeps them distinct, so no two iterations collide; Sort() below gives order.
        Names.Add(ShortName);
    }
    Names.Sort();
    return Names;
}

static UClass* ResolveBTBlueprintParentClass(const FString& ClassSpec, UClass* DefaultClass, UClass* RequiredBaseClass)
{
    const FString Trimmed = ClassSpec.TrimStartAndEnd();
    if (Trimmed.IsEmpty())
    {
        return DefaultClass;
    }

    UClass* Resolved = ResolveClassByName(Trimmed);
    if (!Resolved)
    {
        Resolved = LoadClass<UObject>(nullptr, *Trimmed);
    }
    if (!Resolved && !Trimmed.Contains(TEXT("/")) && !Trimmed.Contains(TEXT(".")))
    {
        FString NativeName = Trimmed;
        NativeName.RemoveFromStart(TEXT("U"));
        const FString NativePath = FString::Printf(TEXT("/Script/AIModule.%s"), *NativeName);
        Resolved = LoadClass<UObject>(nullptr, *NativePath);
    }

    return Resolved && Resolved->IsChildOf(RequiredBaseClass) ? Resolved : nullptr;
}

static bool HandleCreateBTNodeBlueprint(FHandlerContext& Ctx, UClass* DefaultParentClass, const TCHAR* Kind)
{
    FString Name = Ctx.GetString(TEXT("name"));
    if (Name.TrimStartAndEnd().IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("name required for Behavior Tree Blueprint creation"));
        return true;
    }

    FString SavePath = Ctx.GetString(TEXT("savePath"), TEXT("/Game"));
    if (!SavePath.StartsWith(TEXT("/")))
    {
        SavePath = TEXT("/Game/") + SavePath;
    }

    const FString PackagePath = SavePath / Name;
    if (!IsValidAssetPath(PackagePath))
    {
        Ctx.SendError(TEXT("INVALID_PATH"),
            FString::Printf(TEXT("Invalid asset path: '%s'. Path must start with '/', cannot contain '..' or '//'."), *PackagePath));
        return true;
    }

    const FString ParentClassSpec = Ctx.GetString(TEXT("parentClass"));
    UClass* ParentClass = ResolveBTBlueprintParentClass(ParentClassSpec, DefaultParentClass, DefaultParentClass);
    if (!ParentClass)
    {
        Ctx.SendError(TEXT("INVALID_PARENT_CLASS"),
            FString::Printf(TEXT("parentClass '%s' must resolve to %s or a subclass."), *ParentClassSpec, *DefaultParentClass->GetName()));
        return true;
    }

    // Replaces the registry-only DoesAssetExist / ASSET_EXISTS pre-check, which missed
    // a Blueprint created earlier this session and let it reach CanCreateAsset's modal
    // chain. Runs after parent-class resolution so a bad parentClass is still reported
    // as INVALID_PARENT_CLASS rather than masked by an idempotent early return.
    const AssetCreatePolicy::FResolution Resolution = AssetCreatePolicy::Resolve(
        PackagePath, Name, UBlueprint::StaticClass(), Ctx.GetBool(TEXT("overwrite"), false));
    if (Resolution.IsRejected())
    {
        return AssetCreatePolicy::SendRejection(Ctx, Resolution);
    }
    if (Resolution.Action == AssetCreatePolicy::EAction::UpdateInPlace)
    {
        UBlueprint* ExistingBlueprint = CastChecked<UBlueprint>(Resolution.Existing);

        FString ExistingNormalizedPath = ExistingBlueprint->GetPathName();
        if (ExistingNormalizedPath.Contains(TEXT(".")))
        {
            ExistingNormalizedPath = ExistingNormalizedPath.Left(ExistingNormalizedPath.Find(TEXT(".")));
        }

        TSharedPtr<FJsonObject> ExistingResult = MakeShared<FJsonObject>();
        ExistingResult->SetStringField(TEXT("path"), ExistingNormalizedPath);
        ExistingResult->SetStringField(TEXT("assetPath"), ExistingBlueprint->GetPathName());
        ExistingResult->SetStringField(TEXT("name"), Name);
        // The EXISTING parent, which may differ from the requested one: reparenting a
        // BT node Blueprint invalidates its authored graph, so it is reported, not done.
        const UClass* ExistingParent = ExistingBlueprint->ParentClass;
        ExistingResult->SetStringField(TEXT("parentClass"),
            ExistingParent ? ExistingParent->GetPathName() : FString());
        ExistingResult->SetBoolField(TEXT("saved"), false);
        if (ExistingParent != ParentClass)
        {
            ExistingResult->SetStringField(TEXT("warning"),
                FString::Printf(TEXT("Existing Blueprint derives from '%s', not the requested '%s'. ")
                                TEXT("Pass overwrite:true to recreate it with the requested parent."),
                    ExistingParent ? *ExistingParent->GetPathName() : TEXT("<none>"), *ParentClass->GetPathName()));
        }
        AssetCreatePolicy::AddCreateReport(ExistingResult, Resolution);
        AddAssetVerification(ExistingResult, ExistingBlueprint);
        Ctx.SendSuccess(ExistingResult);
        return true;
    }

    UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
    Factory->ParentClass = ParentClass;
    Factory->BlueprintType = BPTYPE_Normal;

    FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("MCP: behavior_tree.create_%s_blueprint"), Kind)));
    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    UBlueprint* CreatedBlueprint = Cast<UBlueprint>(
        AssetToolsModule.Get().CreateAsset(Name, SavePath, UBlueprint::StaticClass(), Factory));
    if (!CreatedBlueprint)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), FString::Printf(TEXT("Failed to create Behavior Tree %s Blueprint."), Kind));
        return true;
    }

    FAssetRegistryModule::AssetCreated(CreatedBlueprint);
    CreatedBlueprint->MarkPackageDirty();
    // Mark-dirty only; see McpSafeAssetSave. `saved` is reported below from a
    // measurement rather than from this call's former constant-true return.
    McpSafeAssetSave(CreatedBlueprint);

    FString NormalizedPath = CreatedBlueprint->GetPathName();
    if (NormalizedPath.Contains(TEXT(".")))
    {
        NormalizedPath = NormalizedPath.Left(NormalizedPath.Find(TEXT(".")));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), NormalizedPath);
    Result->SetStringField(TEXT("assetPath"), CreatedBlueprint->GetPathName());
    Result->SetStringField(TEXT("name"), Name);
    Result->SetStringField(TEXT("parentClass"), ParentClass->GetPathName());
    AddMarkDirtySaveReport(Result, CreatedBlueprint, /*bSaveRequested=*/true);
    AssetCreatePolicy::AddCreateReport(Result, Resolution);
    AddAssetVerification(Result, CreatedBlueprint);
    Ctx.SendSuccess(Result);
    return true;
}

// A single property key that did not land on the node instance: either the key
// matched no property, or its value could not be converted. Carries the key and
// a human-readable reason so set_node_properties can surface the drop instead of
// silently swallowing it (board B-bt-set-node-properties-silent-noop).
struct FBTNodePropertyFailure
{
    FString Key;
    FString Reason;
};

// Per-key outcome of one `properties` payload. Applied names what landed; Failures
// names what did not and why. Every BT verb that takes `properties` reports BOTH,
// so a caller never has to decompile the tree to find out which half of its payload
// took effect (board B-attach-decorator-properties-silently-ignored).
struct FBTNodePropertyReport
{
    TArray<FString> Applied;
    TArray<FBTNodePropertyFailure> Failures;

    bool AnyApplied() const { return Applied.Num() > 0; }
};

// Returns Property as a blackboard key-selector struct property, or null when it is not one.
static FStructProperty* AsBlackboardKeySelectorProperty(FProperty* Property)
{
    FStructProperty* StructProperty = CastField<FStructProperty>(Property);
    return (StructProperty && StructProperty->Struct == FBlackboardKeySelector::StaticStruct())
        ? StructProperty
        : nullptr;
}

// Every key name reachable from Blackboard, parent chain included — the "what would
// have worked" half of a rejected key-selector write.
static TArray<FString> CollectBlackboardKeyNames(const UBlackboardData* Blackboard)
{
    TArray<FString> Names;
    for (const UBlackboardData* It = Blackboard; It; It = It->Parent)
    {
        for (const FBlackboardEntry& Entry : It->Keys)
        {
            Names.AddUnique(Entry.EntryName.ToString());
        }
    }
    return Names;
}

// Second half of a key-selector write. The reflection store lands SelectedKeyName and
// nothing else: SelectedKeyID / SelectedKeyType — the fields the runtime actually reads
// — keep whatever the node was constructed with, which for a fresh decorator is the
// blackboard's FIRST key (FBlackboardKeySelector::InitSelection, BehaviorTreeTypes.cpp:565).
// That is the silent-wrong-key shape: the branch is gated on SelfActor, always true, and
// the tree still compiles. The BT editor closes it through ResolveSelectedKey against the
// tree's blackboard (UBehaviorTreeGraph::UpdateBlackboardChange -> InitializeFromAsset ->
// UBTDecorator_BlackboardBase::InitializeFromAsset, BTDecorator_BlackboardBase.cpp:20), so
// this does the same. A name the blackboard does not carry leaves the selector unresolved
// and is reported as a failed key rather than accepted.
static bool ResolveBlackboardKeySelector(UObject* Target, FStructProperty* SelectorProperty,
    UBehaviorTree* OwnerTree, FString& OutError)
{
    FBlackboardKeySelector* Selector =
        SelectorProperty->ContainerPtrToValuePtr<FBlackboardKeySelector>(Target);
    if (!Selector)
    {
        OutError = TEXT("blackboard key selector could not be read back after the write");
        return false;
    }

    // TreeAsset is what GetBlackboardAsset() and the engine's own PostEditChangeProperty
    // branches read; a node reached through set_node_properties may never have been
    // initialized from the asset in this session.
    UBTNode* NodeInstance = Cast<UBTNode>(Target);
    if (NodeInstance && OwnerTree && NodeInstance->GetBlackboardAsset() == nullptr)
    {
        NodeInstance->InitializeFromAsset(*OwnerTree);
    }

    UBlackboardData* Blackboard = nullptr;
    if (OwnerTree)
    {
        Blackboard = OwnerTree->BlackboardAsset;
    }
    if (!Blackboard && NodeInstance)
    {
        Blackboard = NodeInstance->GetBlackboardAsset();
    }
    if (!Blackboard)
    {
        OutError = TEXT("the Behavior Tree has no BlackboardAsset, so a blackboard key cannot be resolved; assign one first");
        return false;
    }

    Selector->ResolveSelectedKey(*Blackboard);
    if (!Selector->SelectedKeyName.IsNone() && !Selector->IsSet())
    {
        const TArray<FString> AvailableKeys = CollectBlackboardKeyNames(Blackboard);
        OutError = FString::Printf(
            TEXT("blackboard key '%s' does not exist in %s (available: %s)"),
            *Selector->SelectedKeyName.ToString(), *Blackboard->GetName(),
            AvailableKeys.Num() > 0 ? *FString::Join(AvailableKeys, TEXT(", ")) : TEXT("none"));
        return false;
    }

    return true;
}

// Applies each key in Props to Target through the plugin's one shared property writer
// (ApplyJsonValueToProperty — the same converter property.set uses, so struct / enum /
// object-path coercion cannot drift between the two verbs) and reports every key's
// outcome. OwnerTree is the tree the node belongs to; it supplies the blackboard a key
// selector is resolved against and may be null only for a target that carries none.
static FBTNodePropertyReport ApplyBTNodeProperties(UObject* Target, const TSharedPtr<FJsonObject>& Props,
    UBehaviorTree* OwnerTree)
{
    FBTNodePropertyReport Report;
    if (!Target || !Props.IsValid())
    {
        return Report;
    }

    struct FPendingBTPropertyWrite
    {
        FString Key;
        FProperty* Property = nullptr;
        FStructProperty* KeySelector = nullptr;
        TSharedPtr<FJsonValue> Value;
    };

    // Name-resolution pass. Key selectors are written FIRST because
    // UBTDecorator_Blackboard::PostEditChangeProperty resets IntValue/StringValue when the
    // selected key is an enum key (BTDecorator_Blackboard.cpp:186-196); with the payload's
    // unordered map that would clobber a value the same call had already set.
    TArray<FPendingBTPropertyWrite> Pending;
    Pending.Reserve(Props->Values.Num());
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        for (const auto& Pair : Props->Values)
        {
            const FString Key = EARGCompat::JsonKeyToString(Pair.Key);
            FProperty* Prop = FindPropertyCI(Target->GetClass(), Key);
            FStructProperty* KeySelector = AsBlackboardKeySelectorProperty(Prop);
            if ((KeySelector != nullptr) != (Pass == 0))
            {
                continue;
            }

            if (!Prop)
            {
                Report.Failures.Add({Key, FString::Printf(
                    TEXT("no property named '%s' on %s (dotted sub-field paths are not resolved here)"),
                    *Key, *Target->GetClass()->GetName())});
                continue;
            }

            FPendingBTPropertyWrite Write;
            Write.Key = Key;
            Write.Property = Prop;
            Write.KeySelector = KeySelector;
            Write.Value = Pair.Value;

            // A bare string is the natural spelling of a key selector
            // ("BlackboardKey": "bDwellStalled") and the one shape the shared converter
            // cannot take: it reaches the struct branch, fails both the JSON and the
            // ImportText parse and drops. Normalize it into the sub-field shape the
            // converter already handles rather than growing a second coercion ladder here.
            if (Write.KeySelector && Write.Value.IsValid() && Write.Value->Type == EJson::String)
            {
                TSharedPtr<FJsonObject> SelectorObject = MakeShared<FJsonObject>();
                SelectorObject->SetStringField(TEXT("SelectedKeyName"), Write.Value->AsString());
                Write.Value = MakeShared<FJsonValueObject>(SelectorObject);
            }

            Pending.Add(MoveTemp(Write));
        }
    }

    bool bTargetModifiedForApply = false;
    for (const FPendingBTPropertyWrite& Write : Pending)
    {
        if (!bTargetModifiedForApply)
        {
            Target->Modify();
            bTargetModifiedForApply = true;
        }

        // Restored when the resolve below rejects the name, so a failed key write leaves
        // the selector as it was instead of half-written to an unresolvable name.
        const FBlackboardKeySelector PreviousSelector = Write.KeySelector
            ? *Write.KeySelector->ContainerPtrToValuePtr<FBlackboardKeySelector>(Target)
            : FBlackboardKeySelector();

        FString ApplyError;
        if (!ApplyJsonValueToProperty(Target, Write.Property, Write.Value, ApplyError))
        {
            Report.Failures.Add({Write.Key, ApplyError.IsEmpty()
                ? FString(TEXT("value could not be applied"))
                : ApplyError});
            continue;
        }

        if (Write.KeySelector)
        {
            FString ResolveError;
            if (!ResolveBlackboardKeySelector(Target, Write.KeySelector, OwnerTree, ResolveError))
            {
                *Write.KeySelector->ContainerPtrToValuePtr<FBlackboardKeySelector>(Target) = PreviousSelector;
                Report.Failures.Add({Write.Key, ResolveError});
                continue;
            }
        }

        // The store is not the whole write for every BT property either:
        // UBTDecorator_Blackboard maps BasicOperation / ArithmeticOperation / TextOperation
        // onto the OperationType byte its evaluation actually reads, and only in
        // PostEditChangeProperty (BTDecorator_Blackboard.cpp:224-247). Fire the engine's own
        // notification in the leaf/member shape those branches match on — for a key selector
        // the leaf is SelectedKeyName, the name that class's enum-key branch tests.
        // Rationale for the non-chain form: Utils/PropertyChangeNotify.h.
        FProperty* LeafProperty = Write.Property;
        FProperty* MemberProperty = nullptr;
        if (Write.KeySelector)
        {
            if (FProperty* SelectedKeyNameProperty =
                    FindPropertyCI(Write.KeySelector->Struct, TEXT("SelectedKeyName")))
            {
                LeafProperty = SelectedKeyNameProperty;
                MemberProperty = Write.KeySelector;
            }
        }
        PinWright::NotifyPropertyChanged(Target, LeafProperty, MemberProperty);

        Report.Applied.Add(Write.Key);
    }

    return Report;
}

// Reads the optional `properties` payload field. Returns false only when the field is
// present but not an object — a shape that used to be dropped as silently as an
// unapplied key. OutProps stays null when the field is absent.
static bool ReadBTPropertiesField(const TSharedPtr<FJsonObject>& Payload,
    const TSharedPtr<FJsonObject>*& OutProps)
{
    OutProps = nullptr;
    if (!Payload.IsValid() || !Payload->HasField(TEXT("properties")))
    {
        return true;
    }
    return Payload->TryGetObjectField(TEXT("properties"), OutProps);
}

// The dropped-key rejection shared by every BT verb that takes `properties`: the failed
// keys with their reasons, plus what did land, so the caller can repair the payload from
// the response alone. bRolledBack says the verb undid its own write (attach_decorator /
// attach_service remove the subnode again), in which case nothing was left behind.
static void SendBTPropertyFailureError(FHandlerContext& Ctx, const FBTNodePropertyReport& Report,
    bool bRolledBack)
{
    TArray<TSharedPtr<FJsonValue>> DroppedArr;
    for (const FBTNodePropertyFailure& Failure : Report.Failures)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Failure.Key);
        Entry->SetStringField(TEXT("reason"), Failure.Reason);
        DroppedArr.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ErrResult = MakeShared<FJsonObject>();
    ErrResult->SetArrayField(TEXT("droppedFields"), DroppedArr);
    ErrResult->SetArrayField(TEXT("appliedProperties"), EmitStringArray(Report.Applied));
    // Reflects what SURVIVED the rejection, so it is false whenever the caller rolled back.
    ErrResult->SetBoolField(TEXT("partiallyApplied"), !bRolledBack && Report.AnyApplied());
    if (bRolledBack)
    {
        ErrResult->SetBoolField(TEXT("rolledBack"), true);
    }

    Ctx.SendError(TEXT("INVALID_PROPERTY"), FString::Printf(
        TEXT("%d propert%s could not be set on the node and %s dropped: %s: %s"),
        Report.Failures.Num(), Report.Failures.Num() == 1 ? TEXT("y") : TEXT("ies"),
        Report.Failures.Num() == 1 ? TEXT("was") : TEXT("were"),
        *Report.Failures[0].Key, *Report.Failures[0].Reason), ErrResult);
}

static bool HandleAttachBTSubNode(FHandlerContext& Ctx, bool bDecorator)
{
    const FString AssetPath = Ctx.GetStringFirstOf({TEXT("assetPath"), TEXT("behaviorTreePath"), TEXT("path")});
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'."));
        return true;
    }

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    if (!BT)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("Could not load Behavior Tree at '%s'."), *AssetPath));
        return true;
    }

    UBehaviorTreeGraph* BTGraph = Cast<UBehaviorTreeGraph>(BT->BTGraph);
    if (!BTGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Behavior Tree has no graph."));
        return true;
    }

    const FString ParentNodeId = Ctx.GetString(TEXT("parentNodeId"));
    if (ParentNodeId.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'parentNodeId'. Decorators and services must be attached to an existing Behavior Tree graph node."));
        return true;
    }

    UBehaviorTreeGraphNode* ParentNode = PinWright::BehaviorTree::CastGraphNode(FindBTGraphNode(BTGraph, ParentNodeId));
    if (!ParentNode)
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"), FString::Printf(TEXT("Parent node '%s' was not found."), *ParentNodeId));
        return true;
    }

    const bool bParentCanOwnSubNode =
        ParentNode->IsA(UBehaviorTreeGraphNode_Composite::StaticClass()) ||
        PinWright::BehaviorTree::IsTask(ParentNode);
    if (!bParentCanOwnSubNode)
    {
        Ctx.SendError(TEXT("INVALID_PARENT"), TEXT("parentNodeId must identify a Behavior Tree composite or task graph node."));
        return true;
    }

    const FString ClassParam = bDecorator
        ? Ctx.GetStringFirstOf({TEXT("decoratorClass"), TEXT("decoratorType"), TEXT("class")})
        : Ctx.GetStringFirstOf({TEXT("serviceClass"), TEXT("serviceType"), TEXT("class")});
    if (ClassParam.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), bDecorator ? TEXT("Missing 'decoratorClass'.") : TEXT("Missing 'serviceClass'."));
        return true;
    }

    UClass* const RequiredBaseClass = bDecorator ? UBTDecorator::StaticClass() : UBTService::StaticClass();
    const TCHAR* const NativePrefix = bDecorator ? TEXT("BTDecorator_") : TEXT("BTService_");

    UClass* const DefaultClass = bDecorator
        ? UBTDecorator_Blackboard::StaticClass()
        : UBTService_DefaultFocus::StaticClass();
    UClass* NodeInstanceClass = ResolveBTSubNodeClass(ClassParam, RequiredBaseClass, NativePrefix, DefaultClass);

    if (!NodeInstanceClass)
    {
        // Surface the recovery path: the bare "Could not resolve" message is a dead
        // end (the caller cannot tell whether the name was misspelled, names an
        // abstract base, or belongs to the other verb). Enumerate the concrete
        // stock subnode names so the caller can pick a real one without reading
        // engine C++. This is why `Blackboard` resolves for attach_decorator but not
        // attach_service — the AIModule service base UBTService_BlackboardBase is
        // abstract, so no concrete BTService_Blackboard exists; the hint names
        // DefaultFocus and the other concrete services instead.
        const TArray<FString> Available = EnumerateConcreteBTSubNodeNames(RequiredBaseClass, NativePrefix);
        const TCHAR* const Kind = bDecorator ? TEXT("decorator") : TEXT("service");
        TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
        ErrData->SetStringField(bDecorator ? TEXT("decoratorClass") : TEXT("serviceClass"), ClassParam);
        ErrData->SetArrayField(TEXT("availableClasses"), EmitStringArray(Available));
        const FString Message = Available.Num() > 0
            ? FString::Printf(TEXT("Could not resolve %s class '%s'. No concrete native %s class matches; see availableClasses."), Kind, *ClassParam, Kind)
            : FString::Printf(TEXT("Could not resolve %s class '%s'."), Kind, *ClassParam);
        Ctx.SendError(TEXT("INVALID_CLASS"), Message, ErrData);
        return true;
    }

    BT->Modify();
    BTGraph->Modify();
    ParentNode->Modify();

    UBehaviorTreeGraphNode* SubNode = nullptr;
    if (bDecorator)
    {
        SubNode = PinWright::BehaviorTree::NewDecorator(BTGraph);
    }
    else
    {
        SubNode = PinWright::BehaviorTree::NewService(BTGraph);
    }
    if (!SubNode)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create Behavior Tree subnode."));
        return true;
    }

    SubNode->Modify();
    UAIGraphNode::UpdateNodeClassDataFrom(NodeInstanceClass, SubNode->ClassData);
    ParentNode->AddSubNode(SubNode, BTGraph);

    // Detaches the subnode again so a rejected attach leaves NOTHING behind. Shared by the
    // instance-class failure below and the properties rejection further down: a decorator
    // that exists but is configured differently from what the caller asked for is the exact
    // silent-wrong-branch shape this verb must never produce.
    auto RemoveAttachedSubNode = [&]()
    {
        ParentNode->Modify();
        ParentNode->RemoveSubNode(SubNode);
        SubNode->ParentNode = nullptr;
        BTGraph->NotifyGraphChanged();
        BTGraph->UpdateAsset();
    };

    if (!SubNode->NodeInstance || !SubNode->NodeInstance->IsA(NodeInstanceClass))
    {
        RemoveAttachedSubNode();
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create Behavior Tree subnode instance."));
        return true;
    }

    FScopedTransaction PostAddTransaction(FText::FromString(bDecorator
        ? TEXT("MCP: behavior_tree.attach_decorator")
        : TEXT("MCP: behavior_tree.attach_service")));
    BT->Modify();
    BTGraph->Modify();
    ParentNode->Modify();
    TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
    const TSharedPtr<FJsonObject>* Props = nullptr;
    if (!ReadBTPropertiesField(Payload, Props))
    {
        RemoveAttachedSubNode();
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            TEXT("'properties' must be an object of property name -> value."));
        return true;
    }

    FBTNodePropertyReport PropertyReport;
    if (Props)
    {
        SubNode->NodeInstance->SetFlags(RF_Transactional);
        SubNode->NodeInstance->Modify();
        // attach_decorator/attach_service's optional initial properties. These used to be
        // applied with the drop list discarded, so a payload naming a property the class
        // does not have — or a blackboard key the shared converter could not coerce —
        // returned a nodeId and no error while the decorator kept its construction
        // defaults (board B-attach-decorator-properties-silently-ignored). Any dropped key
        // now un-attaches the subnode and fails the call.
        PropertyReport = ApplyBTNodeProperties(SubNode->NodeInstance, *Props, BT);
        if (PropertyReport.Failures.Num() > 0)
        {
            RemoveAttachedSubNode();
            SendBTPropertyFailureError(Ctx, PropertyReport, /*bRolledBack=*/true);
            return true;
        }
    }

    SubNode->Modify();
    SubNode->NodePosX = static_cast<int32>(Ctx.GetNumber(TEXT("x"), 0.0));
    SubNode->NodePosY = static_cast<int32>(Ctx.GetNumber(TEXT("y"), 0.0));
    BTGraph->NotifyGraphChanged();
    BTGraph->UpdateAsset();
    BT->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("nodeId"), SubNode->NodeGuid.ToString());
    Result->SetStringField(TEXT("parentNodeId"), ParentNode->NodeGuid.ToString());
    // Per-key confirmation of what the payload actually did, so "it returned a nodeId"
    // is never again the only evidence the properties landed.
    Result->SetArrayField(TEXT("appliedProperties"), EmitStringArray(PropertyReport.Applied));
    AddAssetVerification(Result, BT);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- behavior_tree.create ----
REGISTER_RPC_HANDLER("behavior_tree.create", "behavior_tree", "Create a new Behavior Tree asset. Returns rootNodeId/rootNodeName for the hidden Root entry node; connect your top composite to it via behavior_tree.connect_nodes or the tree will not run.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the Behavior Tree asset"),
        RPC_PARAM_OPT("savePath", "path", "Folder path (default /Game)")
    ))
{
    FString Name = Ctx.GetString(TEXT("name"));
    if (Name.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("name required for create"));
        return true;
    }

    FString SavePath = Ctx.GetString(TEXT("savePath"), TEXT("/Game"));
    if (!SavePath.StartsWith(TEXT("/")))
    {
        SavePath = TEXT("/Game/") + SavePath;
    }

    FString PackagePath = SavePath / Name;

    if (!IsValidAssetPath(PackagePath))
    {
        Ctx.SendError(TEXT("INVALID_PATH"),
            FString::Printf(TEXT("Invalid asset path: '%s'. Path must start with '/', cannot contain '..' or '//'."), *PackagePath));
        return true;
    }

    if (ResolveAsset(PackagePath).bExists)
    {
        Ctx.SendError(TEXT("ASSET_EXISTS"),
            FString::Printf(TEXT("Behavior Tree already exists at %s"), *PackagePath));
        return true;
    }

    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        Ctx.SendError(TEXT("PACKAGE_FAILED"), TEXT("Failed to create package"));
        return true;
    }

    UBehaviorTree* NewBT = NewObject<UBehaviorTree>(Package, UBehaviorTree::StaticClass(), FName(*Name), RF_Public | RF_Standalone);
    if (!NewBT)
    {
        Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create Behavior Tree"));
        return true;
    }

    UEdGraph* NewGraph = NewObject<UBehaviorTreeGraph>(NewBT, TEXT("BehaviorTree"));
    NewGraph->Schema = UEdGraphSchema_BehaviorTree::StaticClass();
    NewBT->BTGraph = NewGraph;
    NewGraph->GetSchema()->CreateDefaultNodesForGraph(*NewGraph);

    FAssetRegistryModule::AssetCreated(NewBT);
    Package->MarkPackageDirty();
    // Mark-dirty only; the former bool return was a constant. Report from a measurement.
    McpSafeAssetSave(NewBT);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), NewBT->GetPathName());
    Result->SetStringField(TEXT("name"), Name);
    AddMarkDirtySaveReport(Result, NewBT, /*bSaveRequested=*/true);

    // Surface the hidden Root entry node so callers can immediately connect their
    // top composite to it. Without this connection every added node is orphaned and
    // the tree does not run; the Root id/name is otherwise undiscoverable through the
    // RPC surface (no list_nodes RPC; decompile does not emit it).
    if (UBehaviorTreeGraphNode_Root* RootNode = FindBTRootNode(NewGraph))
    {
        Result->SetStringField(TEXT("rootNodeId"), RootNode->NodeGuid.ToString());
        Result->SetStringField(TEXT("rootNodeName"), RootNode->GetName());
    }

    AddAssetVerification(Result, NewBT);
    Ctx.SendSuccess(Result);
    return true;
}

// ---- behavior_tree.create_task_blueprint ----
REGISTER_RPC_HANDLER("behavior_tree.create_task_blueprint", "behavior_tree", "Create a UBTTask_BlueprintBase Blueprint asset. Idempotent: an existing Blueprint at the path is returned with existing:true, mode:\"updated_in_place\" (plus a warning when its parent differs from the requested one). Errors ASSET_ALREADY_EXISTS when a non-Blueprint asset occupies the path.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the task Blueprint asset"),
        RPC_PARAM_OPT("savePath", "path", "Folder path (default /Game)"),
        RPC_PARAM_OPT("parentClass", "classref", "Optional UBTTask_BlueprintBase subclass parent"),
        RPC_PARAM_DEF("overwrite", "boolean", "Delete and recreate an existing Blueprint (discarding its graph) instead of returning it. Rejected with ASSET_IN_USE when any package still references it.", "false")
    ))
{
    return HandleCreateBTNodeBlueprint(Ctx, UBTTask_BlueprintBase::StaticClass(), TEXT("task"));
}

// ---- behavior_tree.create_service_blueprint ----
REGISTER_RPC_HANDLER("behavior_tree.create_service_blueprint", "behavior_tree", "Create a UBTService_BlueprintBase Blueprint asset. Idempotent: an existing Blueprint at the path is returned with existing:true, mode:\"updated_in_place\" (plus a warning when its parent differs from the requested one). Errors ASSET_ALREADY_EXISTS when a non-Blueprint asset occupies the path.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the service Blueprint asset"),
        RPC_PARAM_OPT("savePath", "path", "Folder path (default /Game)"),
        RPC_PARAM_OPT("parentClass", "classref", "Optional UBTService_BlueprintBase subclass parent"),
        RPC_PARAM_DEF("overwrite", "boolean", "Delete and recreate an existing Blueprint (discarding its graph) instead of returning it. Rejected with ASSET_IN_USE when any package still references it.", "false")
    ))
{
    return HandleCreateBTNodeBlueprint(Ctx, UBTService_BlueprintBase::StaticClass(), TEXT("service"));
}

// ---- behavior_tree.create_decorator_blueprint ----
REGISTER_RPC_HANDLER("behavior_tree.create_decorator_blueprint", "behavior_tree", "Create a UBTDecorator_BlueprintBase Blueprint asset. Idempotent: an existing Blueprint at the path is returned with existing:true, mode:\"updated_in_place\" (plus a warning when its parent differs from the requested one). Errors ASSET_ALREADY_EXISTS when a non-Blueprint asset occupies the path.",
    RPC_PARAMS(
        RPC_PARAM_REQ("name", "string", "Name for the decorator Blueprint asset"),
        RPC_PARAM_OPT("savePath", "path", "Folder path (default /Game)"),
        RPC_PARAM_OPT("parentClass", "classref", "Optional UBTDecorator_BlueprintBase subclass parent"),
        RPC_PARAM_DEF("overwrite", "boolean", "Delete and recreate an existing Blueprint (discarding its graph) instead of returning it. Rejected with ASSET_IN_USE when any package still references it.", "false")
    ))
{
    return HandleCreateBTNodeBlueprint(Ctx, UBTDecorator_BlueprintBase::StaticClass(), TEXT("decorator"));
}

// ---- behavior_tree.add_node ----
REGISTER_RPC_HANDLER("behavior_tree.add_node", "behavior_tree", "Add a node to a Behavior Tree graph",
    RPC_PARAMS(
        BTAssetPathParamReq(),
        RPC_PARAM_REQ("nodeType", "classref", "Type of node (Sequence, Selector, Wait, MoveTo, etc.)"),
        RPC_PARAM_REQ("x", "number", "X position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_REQ("y", "number", "Y position in graph (required — nodes stack at origin if all callers pass 0)"),
        RPC_PARAM_OPT("nodeId", "string", "Optional explicit node GUID")
    ))
{
    FString AssetPath = Ctx.GetStringFirstOf({TEXT("assetPath"), TEXT("behaviorTreePath"), TEXT("path")});
    if (AssetPath.IsEmpty())
    {
        Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'."));
        return true;
    }

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    if (!BT)
    {
        Ctx.SendError(TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("Could not load Behavior Tree at '%s'."), *AssetPath));
        return true;
    }

    UEdGraph* BTGraph = BT->BTGraph;
    if (!BTGraph)
    {
        Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Behavior Tree has no graph."));
        return true;
    }

    FString NodeType = Ctx.GetString(TEXT("nodeType"));
    double XD = 0.0, YD = 0.0;
    if (!Ctx.RequireNumber(TEXT("x"), XD)) return true;
    if (!Ctx.RequireNumber(TEXT("y"), YD)) return true;
    float X = static_cast<float>(XD), Y = static_cast<float>(YD);
    FString ProvidedNodeId = Ctx.GetString(TEXT("nodeId"));

    UBehaviorTreeGraphNode* NewNode = nullptr;
    UClass* NodeClass = nullptr;
    UClass* NodeInstanceClass = nullptr;

    if (NodeType == TEXT("Sequence"))
    {
        NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Composite"));
        NodeInstanceClass = UBTComposite_Sequence::StaticClass();
    }
    else if (NodeType == TEXT("Selector"))
    {
        NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Composite"));
        NodeInstanceClass = UBTComposite_Selector::StaticClass();
    }
    else if (NodeType == TEXT("SimpleParallel"))
    {
        NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Composite"));
        NodeInstanceClass = UBTComposite_SimpleParallel::StaticClass();
    }
    else if (NodeType == TEXT("Wait"))
    {
        NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Task"));
        NodeInstanceClass = UBTTask_Wait::StaticClass();
    }
    else if (NodeType == TEXT("MoveTo"))
    {
        NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Task"));
        NodeInstanceClass = UBTTask_MoveTo::StaticClass();
    }
    else if (NodeType == TEXT("RotateTo"))
    {
        NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Task"));
        NodeInstanceClass = UBTTask_RotateToFaceBBEntry::StaticClass();
    }
    else if (NodeType == TEXT("RunBehavior"))
    {
        NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Task"));
        NodeInstanceClass = UBTTask_RunBehavior::StaticClass();
    }
    else if (NodeType == TEXT("Fail") || NodeType == TEXT("Succeed"))
    {
        NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Task"));
        NodeInstanceClass = UBTTask_FinishWithResult::StaticClass();
    }
    else if (NodeType == TEXT("Root"))
    {
        NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Root"));
    }
    else if (NodeType == TEXT("Task"))
    {
        NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Task"));
        NodeInstanceClass = UBTTask_Wait::StaticClass();
    }
    else if (NodeType == TEXT("Decorator") || NodeType == TEXT("Blackboard"))
    {
        Ctx.SendError(TEXT("INVALID_NODE_TYPE"), TEXT("Decorators are Behavior Tree subnodes. Use behavior_tree.attach_decorator with parentNodeId instead of creating a floating decorator node."));
        return true;
    }
    else if (NodeType == TEXT("Service") || NodeType == TEXT("DefaultFocus"))
    {
        Ctx.SendError(TEXT("INVALID_NODE_TYPE"), TEXT("Services are Behavior Tree subnodes. Use behavior_tree.attach_service with parentNodeId instead of creating a floating service node."));
        return true;
    }
    else if (NodeType == TEXT("Composite"))
    {
        NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Composite"));
        NodeInstanceClass = UBTComposite_Sequence::StaticClass();
    }
    else
    {
        UClass* Resolved = ResolveClassByName(NodeType);
        if (Resolved)
        {
            if (Resolved->IsChildOf(UBTCompositeNode::StaticClass()))
            {
                NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Composite"));
                NodeInstanceClass = Resolved;
            }
            else if (Resolved->IsChildOf(UBTTaskNode::StaticClass()))
            {
                NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Task"));
                NodeInstanceClass = Resolved;
            }
            else if (Resolved->IsChildOf(UBTDecorator::StaticClass()))
            {
                Ctx.SendError(TEXT("INVALID_NODE_TYPE"), TEXT("Decorators are Behavior Tree subnodes. Use behavior_tree.attach_decorator with parentNodeId instead of creating a floating decorator node."));
                return true;
            }
            else if (Resolved->IsChildOf(UBTService::StaticClass()))
            {
                Ctx.SendError(TEXT("INVALID_NODE_TYPE"), TEXT("Services are Behavior Tree subnodes. Use behavior_tree.attach_service with parentNodeId instead of creating a floating service node."));
                return true;
            }
        }
    }

    if (NodeClass)
    {
        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: behavior_tree.add_node")));
        BT->Modify();
        BTGraph->Modify();

        UObject* NewNodeObj = NewObject<UObject>(BTGraph, NodeClass, NAME_None, RF_Transactional);
        UClass* BTNodeBaseClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode"));
        if (NewNodeObj && BTNodeBaseClass && NewNodeObj->GetClass()->IsChildOf(BTNodeBaseClass))
        {
            NewNode = static_cast<UBehaviorTreeGraphNode*>(NewNodeObj);
            NewNode->Modify();
            NewNode->CreateNewGuid();

            FGuid NewGuid;
            if (!ProvidedNodeId.IsEmpty() && FGuid::Parse(ProvidedNodeId, NewGuid))
            {
                NewNode->NodeGuid = NewGuid;
            }

            NewNode->NodePosX = X;
            NewNode->NodePosY = Y;

            if (NodeInstanceClass)
            {
                UAIGraphNode::UpdateNodeClassDataFrom(NodeInstanceClass, NewNode->ClassData);
            }

            BTGraph->AddNode(NewNode, true, false);
            NewNode->PostPlacedNewNode();
            if (NewNode->NodeInstance)
            {
                NewNode->NodeInstance->SetFlags(RF_Transactional);
                NewNode->NodeInstance->Modify();
            }
            NewNode->AllocateDefaultPins();
            BTGraph->NotifyGraphChanged();
            BT->MarkPackageDirty();

            TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetStringField(TEXT("nodeId"), NewNode->NodeGuid.ToString());
            AddAssetVerification(Result, BT);
            Ctx.SendSuccess(Result);
        }
        else
        {
            Ctx.SendError(TEXT("CREATE_FAILED"), TEXT("Failed to create node object."));
        }
    }
    else
    {
        Ctx.SendError(TEXT("UNKNOWN_TYPE"), FString::Printf(TEXT("Unknown node type '%s'"), *NodeType));
    }
    return true;
}

// ---- behavior_tree.attach_decorator ----
REGISTER_RPC_HANDLER("behavior_tree.attach_decorator", "behavior_tree", "Attach a decorator subnode to a Behavior Tree task or composite graph node",
    RPC_PARAMS(
        BTAssetPathParamReq(),
        RPC_PARAM_REQ("parentNodeId", "string", "Parent task or composite node GUID or name"),
        RPC_PARAM_REQ("decoratorClass", "classref", "Decorator class path or short name"),
        RPC_PARAM_OPT("x", "number", "X position for the subnode"),
        RPC_PARAM_OPT("y", "number", "Y position for the subnode"),
        RPC_PARAM_OPT("properties", "object", "Key-value properties to set on the decorator instance. Every key must land: an unknown name, an unconvertible value, or a blackboard key the tree's blackboard does not carry fails the call and the decorator is NOT attached. A FBlackboardKeySelector field (BlackboardKey) takes the key name as a plain string and is resolved against the tree's blackboard. Applied names come back in appliedProperties")
    ))
{
    return HandleAttachBTSubNode(Ctx, true);
}

// ---- behavior_tree.attach_service ----
REGISTER_RPC_HANDLER("behavior_tree.attach_service", "behavior_tree", "Attach a service subnode to a Behavior Tree task or composite graph node",
    RPC_PARAMS(
        BTAssetPathParamReq(),
        RPC_PARAM_REQ("parentNodeId", "string", "Parent task or composite node GUID or name"),
        RPC_PARAM_REQ("serviceClass", "classref", "Service class path or short name"),
        RPC_PARAM_OPT("x", "number", "X position for the subnode"),
        RPC_PARAM_OPT("y", "number", "Y position for the subnode"),
        RPC_PARAM_OPT("properties", "object", "Key-value properties to set on the service instance. Every key must land: an unknown name, an unconvertible value, or a blackboard key the tree's blackboard does not carry fails the call and the service is NOT attached. A FBlackboardKeySelector field (BlackboardKey) takes the key name as a plain string and is resolved against the tree's blackboard. Applied names come back in appliedProperties")
    ))
{
    return HandleAttachBTSubNode(Ctx, false);
}

// ---- behavior_tree.connect_nodes ----
REGISTER_RPC_HANDLER("behavior_tree.connect_nodes", "behavior_tree", "Connect two nodes in a Behavior Tree graph",
    RPC_PARAMS(
        BTAssetPathParamReq(),
        RPC_PARAM_REQ("parentNodeId", "string", "Parent node GUID or name"),
        RPC_PARAM_REQ("childNodeId", "string", "Child node GUID or name")
    ))
{
    FString AssetPath = Ctx.GetStringFirstOf({TEXT("assetPath"), TEXT("behaviorTreePath"), TEXT("path")});
    if (AssetPath.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'.")); return true; }

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    if (!BT) { Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load Behavior Tree.")); return true; }

    UEdGraph* BTGraph = BT->BTGraph;
    if (!BTGraph) { Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Behavior Tree has no graph.")); return true; }

    FString ParentNodeId = Ctx.GetString(TEXT("parentNodeId"));
    FString ChildNodeId = Ctx.GetString(TEXT("childNodeId"));

    UEdGraphNode* Parent = FindBTGraphNode(BTGraph, ParentNodeId);
    UEdGraphNode* Child = FindBTGraphNode(BTGraph, ChildNodeId);

    if (!Parent || !Child)
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Parent or child node not found."));
        return true;
    }

    UEdGraphPin* OutputPin = nullptr;
    for (UEdGraphPin* Pin : Parent->Pins)
    {
        if (Pin->Direction == EGPD_Output) { OutputPin = Pin; break; }
    }

    UEdGraphPin* InputPin = nullptr;
    for (UEdGraphPin* Pin : Child->Pins)
    {
        if (Pin->Direction == EGPD_Input) { InputPin = Pin; break; }
    }

    if (OutputPin && InputPin)
    {
        if (BTGraph->GetSchema()->TryCreateConnection(OutputPin, InputPin))
        {
            BTGraph->NotifyGraphChanged();
            // Recompile the EdGraph into the runtime tree so the new edge actually takes
            // effect. Without UpdateAsset() the connection exists only in the editor graph
            // and BT->RootNode stays null — the tree shows the link but never runs. This
            // mirrors the attach_subnodes path, which already rebuilds via UpdateAsset().
            if (UBehaviorTreeGraph* TypedGraph = Cast<UBehaviorTreeGraph>(BTGraph))
            {
                TypedGraph->UpdateAsset();
            }
            BT->MarkPackageDirty();
            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            AddAssetVerification(Resp, BT);
            Ctx.SendSuccess(Resp);
        }
        else
        {
            Ctx.SendError(TEXT("CONNECT_FAILED"), TEXT("Failed to connect nodes."));
        }
    }
    else
    {
        Ctx.SendError(TEXT("PIN_NOT_FOUND"), TEXT("Could not find valid pins for connection."));
    }
    return true;
}

// ---- behavior_tree.remove_node ----
REGISTER_RPC_HANDLER("behavior_tree.remove_node", "behavior_tree", "Remove a node from a Behavior Tree graph",
    RPC_PARAMS(
        BTAssetPathParamReq(),
        RPC_PARAM_REQ("nodeId", "string", "Node GUID to remove")
    ))
{
    FString AssetPath = Ctx.GetStringFirstOf({TEXT("assetPath"), TEXT("behaviorTreePath"), TEXT("path")});
    if (AssetPath.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'.")); return true; }

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    if (!BT) { Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load Behavior Tree.")); return true; }

    UEdGraph* BTGraph = BT->BTGraph;
    if (!BTGraph) { Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Behavior Tree has no graph.")); return true; }

    FString NodeId = Ctx.GetString(TEXT("nodeId"));
    UEdGraphNode* TargetNode = FindBTGraphNode(BTGraph, NodeId);

    if (TargetNode)
    {
        BTGraph->RemoveNode(TargetNode);
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        AddAssetVerification(Resp, BT);
        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Node not found."));
    }
    return true;
}

// ---- behavior_tree.break_connections ----
REGISTER_RPC_HANDLER("behavior_tree.break_connections", "behavior_tree", "Break all connections on a Behavior Tree node",
    RPC_PARAMS(
        BTAssetPathParamReq(),
        RPC_PARAM_REQ("nodeId", "string", "Node GUID to break connections on")
    ))
{
    FString AssetPath = Ctx.GetStringFirstOf({TEXT("assetPath"), TEXT("behaviorTreePath"), TEXT("path")});
    if (AssetPath.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'.")); return true; }

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    if (!BT) { Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load Behavior Tree.")); return true; }

    UEdGraph* BTGraph = BT->BTGraph;
    if (!BTGraph) { Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Behavior Tree has no graph.")); return true; }

    FString NodeId = Ctx.GetString(TEXT("nodeId"));
    UEdGraphNode* TargetNode = FindBTGraphNode(BTGraph, NodeId);

    if (TargetNode)
    {
        TargetNode->BreakAllNodeLinks();
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        AddAssetVerification(Resp, BT);
        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Node not found."));
    }
    return true;
}

// ---- behavior_tree.set_node_properties ----
REGISTER_RPC_HANDLER("behavior_tree.set_node_properties", "behavior_tree", "Set properties on a Behavior Tree node",
    RPC_PARAMS(
        BTAssetPathParamReq(),
        RPC_PARAM_REQ("nodeId", "string", "Node GUID"),
        RPC_PARAM_OPT("comment", "string", "Node comment"),
        RPC_PARAM_OPT("properties", "object", "Key-value properties to set on the node instance. Any key that does not land fails the call with droppedFields; applied names come back in appliedProperties. A FBlackboardKeySelector field takes the key name as a plain string and is resolved against the tree's blackboard")
    ))
{
    FString AssetPath = Ctx.GetStringFirstOf({TEXT("assetPath"), TEXT("behaviorTreePath"), TEXT("path")});
    if (AssetPath.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'.")); return true; }

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    if (!BT) { Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load Behavior Tree.")); return true; }

    UEdGraph* BTGraph = BT->BTGraph;
    if (!BTGraph) { Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Behavior Tree has no graph.")); return true; }

    FString NodeId = Ctx.GetString(TEXT("nodeId"));
    UEdGraphNode* TargetNode = FindBTGraphNode(BTGraph, NodeId);

    if (TargetNode)
    {
        bool bModified = false;
        FString Comment = Ctx.GetString(TEXT("comment"));
        if (!Comment.IsEmpty())
        {
            TargetNode->NodeComment = Comment;
            bModified = true;
        }

        // Try to set properties on the underlying NodeInstance
        UBehaviorTreeGraphNode* BTNode = nullptr;
        UClass* BTNodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode"));
        if (BTNodeClass && TargetNode->GetClass()->IsChildOf(BTNodeClass))
        {
            BTNode = static_cast<UBehaviorTreeGraphNode*>(TargetNode);
        }

        TSharedPtr<FJsonObject> Payload = Ctx.GetRawPayload();
        const TSharedPtr<FJsonObject>* Props = nullptr;
        if (!ReadBTPropertiesField(Payload, Props))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("'properties' must be an object of property name -> value."));
            return true;
        }

        FBTNodePropertyReport PropertyReport;
        if (BTNode && BTNode->NodeInstance && Props)
        {
            PropertyReport = ApplyBTNodeProperties(BTNode->NodeInstance, *Props, BT);
            bModified |= PropertyReport.AnyApplied();
        }

        // Any successful partial apply (or comment edit) was already written into
        // the (transacted) node/graph; reflect the change once so the caller's
        // readback is consistent with what landed, whether we then succeed or
        // reject for dropped keys.
        if (bModified)
        {
            BTGraph->NotifyGraphChanged();
            BT->MarkPackageDirty();
        }

        // Any property key that did not land — unknown name (incl. an unresolved
        // dotted sub-field key), an unconvertible value (e.g. a bare number into
        // UE 5.7's FValueOrBBKey_Float WaitTime) or a blackboard key the tree's
        // blackboard does not carry — must surface as an error. Previously these
        // were swallowed and the call still reported success, so an every-key-failed
        // write looked like it landed (B-bt-set-node-properties-silent-noop).
        // partiallyApplied reflects property writes only, never a comment-only edit,
        // so it tells the caller which property writes survived the rejection.
        if (PropertyReport.Failures.Num() > 0)
        {
            SendBTPropertyFailureError(Ctx, PropertyReport, /*bRolledBack=*/false);
            return true;
        }

        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetArrayField(TEXT("appliedProperties"), EmitStringArray(PropertyReport.Applied));
        AddAssetVerification(Resp, BT);
        Ctx.SendSuccess(Resp);
    }
    else
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"), TEXT("Node not found."));
    }
    return true;
}

// ---- behavior_tree.set_child_order ----

// A composite's children execute left to right by graph X position — UE sorts each output pin's
// LinkedTo with FCompareNodeXLocation before rebuilding the runtime Children array — so the
// non-subnode graph nodes wired to the parent's output pins ARE its ordered children.
static TArray<UBehaviorTreeGraphNode*> CollectBTLinkedChildren(UEdGraphNode* ParentNode)
{
    TArray<UBehaviorTreeGraphNode*> Children;
    if (!ParentNode) return Children;

    for (UEdGraphPin* Pin : ParentNode->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Output) continue;
        for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            UBehaviorTreeGraphNode* Child = LinkedPin
                ? PinWright::BehaviorTree::CastGraphNode(LinkedPin->GetOwningNode())
                : nullptr;
            if (Child && !Child->IsSubNode())
            {
                Children.AddUnique(Child);
            }
        }
    }
    return Children;
}

static TSharedPtr<FJsonObject> MakeBTChildOrderEntry(UBehaviorTreeGraphNode* Child)
{
    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("nodeId"), Child->NodeGuid.ToString());
    // GetNodeName() is the same token behavior_tree.decompile prints, so a caller can line the
    // echoed order up against the BTIR text it read the ids out of.
    const UBTNode* Instance = Cast<UBTNode>(Child->NodeInstance.Get());
    Entry->SetStringField(TEXT("name"), Instance ? Instance->GetNodeName() : Child->GetName());
    Entry->SetNumberField(TEXT("x"), Child->NodePosX);
    Entry->SetNumberField(TEXT("y"), Child->NodePosY);
    return Entry;
}

// Reports the order off the runtime tree rather than off the pins the caller just re-sorted: the
// composite's Children array is what actually executes, so an order read from it is evidence the
// rebuild landed instead of an echo of the request.
static TArray<TSharedPtr<FJsonValue>> EmitBTChildOrder(
    UEdGraphNode* ParentNode, const TArray<UBehaviorTreeGraphNode*>& PinOrderedChildren)
{
    TArray<UBehaviorTreeGraphNode*> Ordered;

    UBehaviorTreeGraphNode* ParentBTNode = PinWright::BehaviorTree::CastGraphNode(ParentNode);
    UBTCompositeNode* ParentComposite = ParentBTNode
        ? Cast<UBTCompositeNode>(ParentBTNode->NodeInstance.Get())
        : nullptr;
    if (ParentComposite)
    {
        for (const FBTCompositeChild& RuntimeChild : ParentComposite->Children)
        {
            const UObject* ChildInstance = RuntimeChild.ChildComposite.Get()
                ? static_cast<const UObject*>(RuntimeChild.ChildComposite.Get())
                : static_cast<const UObject*>(RuntimeChild.ChildTask.Get());
            UBehaviorTreeGraphNode* const* Match = PinOrderedChildren.FindByPredicate(
                [ChildInstance](UBehaviorTreeGraphNode* Candidate)
                {
                    return Candidate && Candidate->NodeInstance.Get() == ChildInstance;
                });
            if (Match && *Match)
            {
                Ordered.Add(*Match);
            }
        }
    }

    // The Root entry node carries no composite instance, and an orphaned parent's runtime array is
    // stale because UpdateAsset only rebuilds what is reachable from the root. Fall back to the pin
    // order UpdateAsset would consume rather than reporting a short list.
    if (Ordered.Num() != PinOrderedChildren.Num())
    {
        Ordered = PinOrderedChildren;
    }

    TArray<TSharedPtr<FJsonValue>> Out;
    Out.Reserve(Ordered.Num());
    for (UBehaviorTreeGraphNode* Child : Ordered)
    {
        Out.Add(MakeShared<FJsonValueObject>(MakeBTChildOrderEntry(Child)));
    }
    return Out;
}

REGISTER_RPC_HANDLER("behavior_tree.set_child_order", "behavior_tree",
    "Set the execution order of a composite's children. A composite runs its children left to right by graph X position, so this rewrites their X positions and rebuilds the runtime child array the way the Behavior Tree editor does when a node is dragged. Returns the resulting order as childOrder.",
    RPC_PARAMS(
        BTAssetPathParamReq(),
        RPC_PARAM_REQ("parentNodeId", "string", "Composite node GUID or name whose children are reordered. Ids come from behavior_tree.add_node or the nodeId field on every behavior_tree.decompile node line."),
        RPC_PARAM_REQ("childNodeIds", "array", "Child node GUIDs or names in the wanted execution order, highest priority first. Must list every current child of the parent exactly once; a short, long or duplicated list is rejected with the parent's currentChildOrder.")
    ))
{
    FString AssetPath = Ctx.GetStringFirstOf({TEXT("assetPath"), TEXT("behaviorTreePath"), TEXT("path")});
    if (AssetPath.IsEmpty()) { Ctx.SendError(TEXT("INVALID_ARGUMENT"), TEXT("Missing 'assetPath'.")); return true; }

    UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *AssetPath);
    if (!BT) { Ctx.SendError(TEXT("ASSET_NOT_FOUND"), TEXT("Could not load Behavior Tree.")); return true; }

    UBehaviorTreeGraph* BTGraph = Cast<UBehaviorTreeGraph>(BT->BTGraph);
    if (!BTGraph) { Ctx.SendError(TEXT("GRAPH_NOT_FOUND"), TEXT("Behavior Tree has no graph.")); return true; }

    const FString ParentNodeId = Ctx.GetString(TEXT("parentNodeId"));
    UEdGraphNode* ParentNode = FindBTGraphNode(BTGraph, ParentNodeId);
    if (!ParentNode)
    {
        Ctx.SendError(TEXT("NODE_NOT_FOUND"), FString::Printf(TEXT("Parent node '%s' was not found."), *ParentNodeId));
        return true;
    }

    // A Simple Parallel is the one BT node with two output pins ('Task' for the main task, 'Out'
    // for the background branch), and the engine sorts each pin's LinkedTo independently. One flat
    // childNodeIds list spanning both pins therefore cannot express an order: the X rewrite would
    // land, each pin would re-sort within itself, and the child arrays would come back unchanged
    // while the call reported success. Refuse instead of no-opping.
    int32 OutputPinCount = 0;
    for (const UEdGraphPin* Pin : ParentNode->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output)
        {
            ++OutputPinCount;
        }
    }
    if (OutputPinCount > 1)
    {
        TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
        ErrData->SetNumberField(TEXT("outputPinCount"), OutputPinCount);
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("Parent node '%s' has %d output pins (a Simple Parallel splits into 'Task' and 'Out'), and each pin's children are ordered independently, so one childNodeIds list cannot express their order. Put the branch you want ordered under a child composite and reorder that composite's children."),
                *ParentNodeId, OutputPinCount),
            ErrData);
        return true;
    }

    const TArray<TSharedPtr<FJsonValue>>* RequestedIds = nullptr;
    if (!Ctx.RequireArray(TEXT("childNodeIds"), RequestedIds)) return true;

    const TArray<UBehaviorTreeGraphNode*> CurrentChildren = CollectBTLinkedChildren(ParentNode);

    TArray<UBehaviorTreeGraphNode*> Reordered;
    Reordered.Reserve(RequestedIds->Num());
    for (const TSharedPtr<FJsonValue>& Value : *RequestedIds)
    {
        FString ChildId;
        if (!Value.IsValid() || !Value->TryGetString(ChildId) || ChildId.IsEmpty())
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                TEXT("'childNodeIds' must be an array of node GUID or name strings."));
            return true;
        }

        UBehaviorTreeGraphNode* Child = PinWright::BehaviorTree::CastGraphNode(FindBTGraphNode(BTGraph, ChildId));
        if (!Child || !CurrentChildren.Contains(Child))
        {
            TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
            ErrData->SetStringField(TEXT("childNodeId"), ChildId);
            ErrData->SetArrayField(TEXT("currentChildOrder"), EmitBTChildOrder(ParentNode, CurrentChildren));
            Ctx.SendError(TEXT("NODE_NOT_FOUND"),
                FString::Printf(TEXT("'%s' is not a child of parent node '%s'; see currentChildOrder."),
                    *ChildId, *ParentNodeId),
                ErrData);
            return true;
        }

        if (Reordered.Contains(Child))
        {
            Ctx.SendError(TEXT("INVALID_ARGUMENT"),
                FString::Printf(TEXT("'%s' appears more than once in 'childNodeIds'."), *ChildId));
            return true;
        }
        Reordered.Add(Child);
    }

    // A partial list cannot express an order: the children left out would keep X positions that
    // interleave with the ones being moved, so the result would not be the requested priority.
    if (Reordered.Num() != CurrentChildren.Num())
    {
        TSharedPtr<FJsonObject> ErrData = MakeShared<FJsonObject>();
        ErrData->SetArrayField(TEXT("currentChildOrder"), EmitBTChildOrder(ParentNode, CurrentChildren));
        Ctx.SendError(TEXT("INVALID_ARGUMENT"),
            FString::Printf(TEXT("'childNodeIds' must list all %d children of parent node '%s' exactly once; %d were given. See currentChildOrder."),
                CurrentChildren.Num(), *ParentNodeId, Reordered.Num()),
            ErrData);
        return true;
    }

    FScopedTransaction Transaction(FText::FromString(TEXT("MCP: behavior_tree.set_child_order")));
    BT->Modify();
    BTGraph->Modify();

    // Reuse the children's own current X values as the slots to fill, so the reorder keeps the
    // author's layout footprint instead of re-laying the row out. Slots that tie are pushed apart
    // because equal X leaves the engine's comparator deciding on Y, which is not the caller's ask.
    TArray<int32> Slots;
    Slots.Reserve(CurrentChildren.Num());
    for (const UBehaviorTreeGraphNode* Child : CurrentChildren)
    {
        Slots.Add(Child->NodePosX);
    }
    Slots.Sort();

    constexpr int32 MinimumChildSpacing = 200;
    int32 PreviousX = 0;
    for (int32 Index = 0; Index < Reordered.Num(); ++Index)
    {
        int32 NewX = Slots[Index];
        if (Index > 0 && NewX <= PreviousX)
        {
            NewX = PreviousX + MinimumChildSpacing;
        }
        Reordered[Index]->Modify();
        Reordered[Index]->NodePosX = NewX;
        PreviousX = NewX;
    }

    // The editor's own "a node moved, re-derive priority" routine: it sorts each output pin's
    // LinkedTo with FCompareNodeXLocation and, when that changed anything, calls
    // UpdateAsset(KeepRebuildCounter), which rebuilds the runtime Children array from the new pin
    // order. Reusing it keeps this verb on exactly the ordering rule the BT editor applies.
    BTGraph->RebuildChildOrder(ParentNode);
    BTGraph->NotifyGraphChanged();
    BT->MarkPackageDirty();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("parentNodeId"), ParentNode->NodeGuid.ToString());
    Resp->SetArrayField(TEXT("childOrder"), EmitBTChildOrder(ParentNode, CollectBTLinkedChildren(ParentNode)));
    AddAssetVerification(Resp, BT);
    Ctx.SendSuccess(Resp);
    return true;
}
