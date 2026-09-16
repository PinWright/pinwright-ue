// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirTextEmitter.cpp - Converts individual Blueprint nodes into BPIR text lines

#include "Decompiler/BpirTextEmitter.h"
#include "Decompiler/BpirInputKeyHelpers.h"
#include "Decompiler/DecompilerTypes.h"
#include "Decompiler/GraphWalker.h"
#include "Compiler/BpirSharedConstants.h"
#include "Compiler/BpirStructLiteralUtils.h"
#include "Compiler/CodeNodeEmitter.h"
#include "IrCore/IrTextUtils.h"
#include "Utils/PropertyUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "K2Node.h"
#include "InputAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Message.h"
#include "K2Node_VariableSet.h"
#include "K2Node_VariableGet.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Timeline.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_InputKey.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputActionEvent.h"
#include "K2Node_InputAxisEvent.h"
#include "K2Node_InputAxisKeyEvent.h"
#include "K2Node_InputKeyEvent.h"
#include "K2Node_InputTouchEvent.h"
#include "K2Node_InputVectorAxisEvent.h"
#include "K2Node_ActorBoundEvent.h"
#if __has_include("K2Node_GeneratedBoundEvent.h")
#include "K2Node_GeneratedBoundEvent.h"
#define MCP_HAS_K2NODE_GENERATEDBOUNDEVENT 1
#else
#define MCP_HAS_K2NODE_GENERATEDBOUNDEVENT 0
#endif
#include "GameFramework/Actor.h"
#if __has_include("K2Node_WidgetAnimationEvent.h")
#include "K2Node_WidgetAnimationEvent.h"
#define MCP_HAS_WIDGET_ANIMATION_EVENT 1
#else
#define MCP_HAS_WIDGET_ANIMATION_EVENT 0
#endif
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_GetSubsystem.h"
#include "K2Node_Switch.h"
#if __has_include("WidgetBlueprint.h")
#include "WidgetBlueprint.h"
#define MCP_HAS_WIDGET_BLUEPRINT_CONTEXT 1
#else
#define MCP_HAS_WIDGET_BLUEPRINT_CONTEXT 0
#endif
#if __has_include("K2Node_EnumLiteral.h")
#include "K2Node_EnumLiteral.h"
#define MCP_HAS_ENUM_LITERAL 1
#else
#define MCP_HAS_ENUM_LITERAL 0
#endif
#if __has_include("K2Node_SwitchInteger.h")
#include "K2Node_SwitchInteger.h"
#define MCP_HAS_SWITCH_INTEGER 1
#else
#define MCP_HAS_SWITCH_INTEGER 0
#endif
#if __has_include("K2Node_SwitchString.h")
#include "K2Node_SwitchString.h"
#define MCP_HAS_SWITCH_STRING 1
#else
#define MCP_HAS_SWITCH_STRING 0
#endif
#if __has_include("K2Node_SwitchEnum.h")
#include "K2Node_SwitchEnum.h"
#define MCP_HAS_SWITCH_ENUM 1
#else
#define MCP_HAS_SWITCH_ENUM 0
#endif
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#if __has_include("K2Node_MakeArray.h")
#include "K2Node_MakeArray.h"
#define MCP_HAS_MAKE_ARRAY 1
#else
#define MCP_HAS_MAKE_ARRAY 0
#endif
#if __has_include("K2Node_Select.h")
#include "K2Node_Select.h"
#define MCP_HAS_SELECT 1
#else
#define MCP_HAS_SELECT 0
#endif
#if __has_include("K2Node_MakeMap.h")
#include "K2Node_MakeMap.h"
#define MCP_HAS_MAKE_MAP 1
#else
#define MCP_HAS_MAKE_MAP 0
#endif
#if __has_include("K2Node_MakeSet.h")
#include "K2Node_MakeSet.h"
#define MCP_HAS_MAKE_SET 1
#else
#define MCP_HAS_MAKE_SET 0
#endif
#include "EdGraphSchema_K2.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Compiler/BpirTypeGrammar.h"
#include "Engine/MemberReference.h"
#include "Internationalization/Text.h"
#include "Compat/EngineVersionCompat.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

#if __has_include("K2Node_BaseMCDelegate.h")
#include "K2Node_BaseMCDelegate.h"
#endif
#if __has_include("K2Node_CallDelegate.h")
#include "K2Node_CallDelegate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#define MCP_HAS_DELEGATE_NODES 1
#else
#define MCP_HAS_DELEGATE_NODES 0
#endif
#if __has_include("K2Node_CreateDelegate.h")
#include "K2Node_CreateDelegate.h"
#define MCP_HAS_CREATE_DELEGATE 1
#else
#define MCP_HAS_CREATE_DELEGATE 0
#endif

DEFINE_LOG_CATEGORY_STATIC(LogBpirTextEmitter, Log, All);

static FString FormatEnumLiteralForBpir(FString Literal, const FString& EnumName)
{
    Literal.TrimStartAndEndInline();
    if (Literal.IsEmpty()
        || EnumName.IsEmpty()
        || Literal.StartsWith(TEXT("%"))
        || Literal.StartsWith(TEXT("$"))
        || Literal == TEXT("?"))
    {
        return Literal;
    }

    FString Prefix;
    FString ValueName;
    if (Literal.Split(TEXT("::"), &Prefix, &ValueName))
    {
        ValueName.TrimStartAndEndInline();
        while (ValueName.StartsWith(TEXT(":")))
        {
            ValueName.RemoveFromStart(TEXT(":"));
        }
        return ValueName.IsEmpty()
            ? Literal
            : FString::Printf(TEXT("%s::%s"), *EnumName, *ValueName);
    }

    if (Literal.StartsWith(TEXT(":")))
    {
        ValueName = Literal;
        while (ValueName.StartsWith(TEXT(":")))
        {
            ValueName.RemoveFromStart(TEXT(":"));
        }
        return ValueName.IsEmpty()
            ? Literal
            : FString::Printf(TEXT("%s::%s"), *EnumName, *ValueName);
    }

    if (Literal.Split(TEXT(":"), &Prefix, &ValueName))
    {
        Prefix.TrimStartAndEndInline();
        ValueName.TrimStartAndEndInline();
        while (ValueName.StartsWith(TEXT(":")))
        {
            ValueName.RemoveFromStart(TEXT(":"));
        }
        if (!ValueName.IsEmpty() && Prefix.Equals(EnumName, ESearchCase::IgnoreCase))
        {
            return FString::Printf(TEXT("%s::%s"), *EnumName, *ValueName);
        }
    }

    return Literal;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

FBpirTextEmitter::FBpirTextEmitter()
{
}

int32 FBpirTextEmitter::ConsumeGeneratedPrefixLineCount()
{
    const int32 Result = LastGeneratedPrefixLineCount;
    LastGeneratedPrefixLineCount = 0;
    return Result;
}

// ---------------------------------------------------------------------------
// Pin type conversion
// ---------------------------------------------------------------------------

static FString GetParserSafeDelegateSignatureOwnerHint(const UFunction* SignatureFunction)
{
    if (!SignatureFunction)
    {
        return FString();
    }

    if (const UClass* OwnerClass = SignatureFunction->GetOuterUClass())
    {
        return OwnerClass->GetName();
    }

    if (const UPackage* Package = SignatureFunction->GetOutermost())
    {
        FString PackageName = Package->GetName();
        if (PackageName.StartsWith(TEXT("/Script/")))
        {
            return PackageName.Mid(8);
        }

        int32 LastSlash = INDEX_NONE;
        int32 LastDot = INDEX_NONE;
        PackageName.FindLastChar(TEXT('/'), LastSlash);
        PackageName.FindLastChar(TEXT('.'), LastDot);
        const int32 LastSeparator = FMath::Max(LastSlash, LastDot);
        if (LastSeparator != INDEX_NONE && LastSeparator + 1 < PackageName.Len())
        {
            return PackageName.Mid(LastSeparator + 1);
        }
        return PackageName;
    }

    return FString();
}

FString FBpirTextEmitter::PinTypeToBpirType(const FEdGraphPinType& PinType)
{
    // Container wrappers recurse on the element (and key for Map) with ContainerType
    // cleared; the tail is always a scalar/tagged form resolved by the grammar table.
    if (PinType.IsArray())
    {
        FEdGraphPinType Inner = PinType;
        Inner.ContainerType = EPinContainerType::None;
        return FString::Printf(TEXT("array<%s>"), *PinTypeToBpirType(Inner));
    }
    if (PinType.IsSet())
    {
        FEdGraphPinType Inner = PinType;
        Inner.ContainerType = EPinContainerType::None;
        return FString::Printf(TEXT("set<%s>"), *PinTypeToBpirType(Inner));
    }
    if (PinType.IsMap())
    {
        // Key uses the PinType's own category/sub; value is rebuilt from PinValueType.
        FEdGraphPinType Key = PinType;
        Key.ContainerType = EPinContainerType::None;

        FEdGraphPinType Value;
        Value.PinCategory = PinType.PinValueType.TerminalCategory;
        Value.PinSubCategory = PinType.PinValueType.TerminalSubCategory;
        Value.PinSubCategoryObject = PinType.PinValueType.TerminalSubCategoryObject;
        return FString::Printf(TEXT("map<%s, %s>"),
            *PinTypeToBpirType(Key), *PinTypeToBpirType(Value));
    }

    const FName& Category = PinType.PinCategory;

    // Narrow safety branches for pin categories the grammar table's FindByPinCategory
    // intentionally doesn't cover:
    //  - PC_Float / PC_Double: BP pins normally use PC_Real + subcategory, but some
    //    paths feed bare PC_Float/PC_Double directly (e.g. schema normalization).
    //  - PC_Enum: distinct from PC_Byte+UEnum routing; a dedicated "enum" pin category
    //    exists and carries the UEnum as its SubCategoryObject.
    if (Category == UEdGraphSchema_K2::PC_Float)
    {
        return TEXT("float");
    }
    if (Category == UEdGraphSchema_K2::PC_Double)
    {
        return TEXT("double");
    }
    if (Category == UEdGraphSchema_K2::PC_Enum)
    {
        if (UObject* SubObj = PinType.PinSubCategoryObject.Get())
        {
            return FString::Printf(TEXT("enum<%s>"), *SubObj->GetName());
        }
        return TEXT("enum");
    }

    if (const BpirTypeGrammar::FBpirTypeGrammarEntry* E = BpirTypeGrammar::FindByPinCategory(
            PinType.PinCategory,
            PinType.PinSubCategory,
            PinType.PinSubCategoryObject.Get()))
    {
        // Well-known structs (Vector/Rotator/Transform/...): the old cascade wrapped
        // these as "struct<CanonicalText>" via the generic PC_Struct path. Preserve
        // that tagged emission so existing tests and round-trips stay byte-identical.
        if (!E->CanonicalInnerName.IsNone())
        {
            return FString::Printf(TEXT("struct<%s>"), E->CanonicalText);
        }

        // Delegate / McDelegate: signature lives on PinSubCategoryMemberReference,
        // not on PinSubCategoryObject. Resolve the actual UFunction because native
        // delegate signatures may be package-owned rather than class-owned.
        if (E->Kind == EBpirTypeKind::Delegate || E->Kind == EBpirTypeKind::McDelegate)
        {
            const FSimpleMemberReference& MemberRef = PinType.PinSubCategoryMemberReference;
            UFunction* SignatureFunction = FMemberReference::ResolveSimpleMemberReference<UFunction>(MemberRef);
            if (SignatureFunction)
            {
                const FString OwnerHint = GetParserSafeDelegateSignatureOwnerHint(SignatureFunction);
                if (!OwnerHint.IsEmpty())
                {
                    return FString::Printf(TEXT("%s<%s, %s>"),
                        E->CanonicalText, *OwnerHint, *SignatureFunction->GetName());
                }
            }

            if (UClass* OwnerClass = MemberRef.GetMemberParentClass(); OwnerClass && !MemberRef.MemberName.IsNone())
            {
                return FString::Printf(TEXT("%s<%s, %s>"),
                    E->CanonicalText, *OwnerClass->GetName(), *MemberRef.MemberName.ToString());
            }
            return FString(E->CanonicalText);
        }

        // Tagged form with a real inner object -> "<Keyword><InnerName>".
        if (UObject* SubObj = PinType.PinSubCategoryObject.Get())
        {
            if (EnumHasAnyFlags(E->AcceptForms, BpirTypeGrammar::EAcceptForm::Tagged))
            {
                return FString::Printf(TEXT("%s<%s>"), E->CanonicalText, *SubObj->GetName());
            }
        }

        // Everything else emits the canonical keyword bare: primitives ("bool", "int",
        // "delegate", "byte", ...) and bare object/class/struct/interface/enum
        // (the SubCategoryObject was missing for tagged kinds).
        return FString(E->CanonicalText);
    }

    // Category didn't match any entry (e.g. PC_Wildcard) — fall back to the raw
    // category string, matching the old cascade's final branch.
    return Category.ToString();
}

// PN_Then is exec on K2 nodes (PC_Exec already filters it) but we guard it
// defensively in case a node mis-tags its "then" pin category.
static bool IsPrimaryOutputDataPin(const UEdGraphPin* Pin)
{
    // A split struct pin (UEdGraphSchema_K2::SplitPin) is hidden and its members
    // become sub-pins, but the struct is still the value the node produces: the
    // parent is the primary output and the sub-pins are members of it, not values
    // in their own right. Admitting the parent and rejecting the sub-pins keeps a
    // split FHitResult out of an entry signature as `struct<HitResult> Hit` rather
    // than a flattened member list, and keeps `%name: <type>` naming the struct
    // instead of whichever member happens to sort first.
    return Pin && Pin->Direction == EGPD_Output
        && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
        && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Delegate
        && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_MCDelegate
        && (!Pin->bHidden || Pin->SubPins.Num() > 0)
        && Pin->ParentPin == nullptr
        && Pin->PinName != UEdGraphSchema_K2::PN_Then;
}

FString FBpirTextEmitter::ResolvePrimaryOutputTypeAnnotation(UEdGraphNode* Node)
{
    if (!Node)
    {
        return FString();
    }

    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (IsPrimaryOutputDataPin(Pin))
        {
            return FString::Printf(TEXT(": %s"), *PinTypeToBpirType(Pin->PinType));
        }
    }
    return FString();
}

// ---------------------------------------------------------------------------
// Helper: variable name extraction
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::GetVariableName(UEdGraphNode* Node)
{
    if (UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node))
    {
        return FIrTextUtils::FormatNameToken(SetNode->GetVarName().ToString());
    }
    if (UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(Node))
    {
        return FIrTextUtils::FormatNameToken(GetNode->GetVarName().ToString());
    }
    return TEXT("UnknownVar");
}

// ---------------------------------------------------------------------------
// Helper: function display name
// ---------------------------------------------------------------------------

namespace
{
    // Mirrors the cascade order used by the compiler (Step 4-first when Target
    // is present, then self-class). If the unqualified lookup lands on the same
    // UFunction the node was bound to, the name is unambiguous and stays bare.
    // Otherwise the emitter prefixes the owning class so the round-trip pins
    // the exact UFunction explicitly. Single-class scope round-trip stays
    // byte-identical because the lookup picks the same overload either way.
    //
    // CASCADE-COUPLING NOTE: this probe re-implements the compiler's resolution
    // order from FBpirCompiler::EmitInstruction in Compiler/BpirCompiler.cpp
    // (the call-cascade: qualified short-circuit -> ResolveViaTargetArg ->
    // self-class -> SkeletonGeneratedClass -> libraries -> broad search). If
    // that cascade's ordering changes, this predicate silently desyncs and the
    // decompiler will under- or over-qualify. Any change to the cascade must
    // also update this probe.
    bool ShouldQualifyFunctionName(UK2Node_CallFunction* Node, const UFunction* BoundFunc)
    {
        if (!Node || !BoundFunc)
        {
            return false;
        }

        // An interface message node (`message Iface::Func(...)`) is ALWAYS qualified.
        // UK2Node_Message's self pin is a plain object pin (K2Node_Message.cpp:88-93), so
        // neither probe below can see the interface: the target-class probe reads UObject
        // and the self-class probe reads the owning Blueprint, and both miss the interface
        // function. Emitting the bare name would leave the recompile to the resolver's broad
        // search, which can land on a same-named function of a different class or on nothing
        // at all. The interface is the one piece of information a message call cannot infer.
        if (Node->IsA<UK2Node_Message>())
        {
            return true;
        }

        // Self-context calls never need class qualification in BPIR. IsSelfContext()
        // is the canonical engine-side answer: when true, FunctionReference.MemberParent
        // is zero (or implicitly the owning BP class), so prefixing the emitted call
        // with the owning-class name only adds noise and — when the resolver landed on
        // a SkeletonGeneratedClass UFunction (B-bpir-decompile-emits-skel-class-prefix-on-self-event-calls)
        // — leaks a misleading `SKEL_<Class>::` prefix. The comparison-against-GeneratedClass
        // logic below still handles explicit `Target: %obj` non-self calls (bSelfContext=false).
        if (Node->FunctionReference.IsSelfContext())
        {
            return false;
        }

        UBlueprint* OwningBP = nullptr;
        if (const UEdGraph* Graph = Node->GetGraph())
        {
            OwningBP = Graph->GetTypedOuter<UBlueprint>();
        }

        UClass* BPClass = nullptr;
        if (OwningBP)
        {
            BPClass = OwningBP->GeneratedClass ? OwningBP->GeneratedClass : OwningBP->ParentClass;
        }

        const FName FuncFName = BoundFunc->GetFName();

        // The BPIR resolver often hands back the SkeletonGeneratedClass copy of a
        // Blueprint function (post Phase-1.5 RegenerateSkeletonOnly), while a fresh
        // FindFunctionByName on the target/self class below returns the
        // GeneratedClass copy. Those are two DISTINCT UFunction objects for the
        // SAME authored function, so a raw `Found != BoundFunc` pointer compare
        // spuriously reports a name collision and over-qualifies. On a cross-class
        // (explicit Target:) call that surfaces as an unresolvable
        // `SKEL_<Class>::Func` token (B-bpir-decompile-skel-qualified-cross-class-calls)
        // — the non-self twin of the IsSelfContext() short-circuit above (from the
        // DONE self-event sibling). Collapse both sides to their GeneratedClass
        // instance before the identity compare so only a genuine same-name/
        // different-function collision qualifies.
        auto NormalizeToGeneratedFunction = [](const UFunction* Func) -> const UFunction*
        {
            if (!Func)
            {
                return nullptr;
            }
            UClass* OwnerClass = Func->GetOuterUClass();
            if (!OwnerClass)
            {
                return Func;
            }
            const UBlueprint* OwnerBP = Cast<UBlueprint>(OwnerClass->ClassGeneratedBy);
            if (!OwnerBP || !OwnerBP->GeneratedClass || OwnerBP->GeneratedClass == OwnerClass)
            {
                return Func;
            }
            const UFunction* GenFunc = OwnerBP->GeneratedClass->FindFunctionByName(Func->GetFName());
            return GenFunc ? GenFunc : Func;
        };
        // BoundFunc is invariant across the probes below, so normalize it once and
        // compare each probe's GeneratedClass twin against it.
        const UFunction* NormBound = NormalizeToGeneratedFunction(BoundFunc);

        // Probe target-class first to match the compiler cascade's Step 4-first reorder.
        UClass* TargetClass = nullptr;
        if (UEdGraphPin* SelfPin = Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input))
        {
            if (SelfPin->LinkedTo.Num() > 0 && SelfPin->LinkedTo[0])
            {
                TargetClass = Cast<UClass>(SelfPin->LinkedTo[0]->PinType.PinSubCategoryObject.Get());
            }
        }

        if (TargetClass)
        {
            if (UFunction* Found = TargetClass->FindFunctionByName(FuncFName))
            {
                return NormalizeToGeneratedFunction(Found) != NormBound;
            }
        }

        if (BPClass)
        {
            if (UFunction* Found = BPClass->FindFunctionByName(FuncFName))
            {
                return NormalizeToGeneratedFunction(Found) != NormBound;
            }
        }

        // Neither the target nor self-class scope holds a function by this name.
        // The cascade would fall through to library / broad-search lookups, which
        // (by definition) found the bound function in the first place. Emit
        // unqualified so static-library calls (PrintString, MakeVector, ...) stay
        // byte-identical to prior decompiler output.
        return false;
    }

    FString StripBPGeneratedClassSuffix(const FString& Name)
    {
        // BP-generated classes are named `<AssetName>_C`. Strip the suffix so the
        // emitted token reads as the BP asset name, not the runtime class identifier.
        if (Name.EndsWith(TEXT("_C")))
        {
            return Name.LeftChop(2);
        }
        return Name;
    }
}

FString FBpirTextEmitter::GetFunctionDisplayName(UK2Node_CallFunction* Node)
{
    if (!Node)
    {
        return TEXT("UnknownFunction");
    }

    const UFunction* Func = Node->GetTargetFunction();
    if (Func)
    {
        const FString FuncName = Func->GetName();
        if (Node->IsA<UK2Node_CallParentFunction>())
        {
            UClass* BPClass = nullptr;
            if (const UEdGraph* Graph = Node->GetGraph())
            {
                if (const UBlueprint* BP = Graph->GetTypedOuter<UBlueprint>())
                {
                    BPClass = BP->GeneratedClass ? BP->GeneratedClass : BP->ParentClass;
                }
            }
            const UClass* ParentClass = Node->FunctionReference.GetMemberParentClass(BPClass);
            if (!ParentClass)
            {
                ParentClass = Func->GetOwnerClass();
            }
            if (ParentClass)
            {
                ParentClass = ParentClass->GetAuthoritativeClass();
                return FString::Printf(TEXT("%s::%s"),
                    *FIrTextUtils::FormatNameToken(ParentClass->GetName()),
                    *FIrTextUtils::FormatNameToken(FuncName));
            }
        }
        if (ShouldQualifyFunctionName(Node, Func))
        {
            if (const UClass* OwnerClass = Func->GetOuterUClass())
            {
                // A message call keeps the generated-class name verbatim, `_C` and all.
                // ResolveUClass resolves a bare Blueprint name ONLY through its `_C` form
                // (ClassUtils.cpp step 7's asset-registry lookup); the stripped asset name
                // matches nothing there, because a loaded Blueprint class object is always
                // named `<Asset>_C` (step 5's exact-name iteration misses it too). Stripping
                // is safe for the ordinary qualified call because that path qualifies only
                // when the cascade would otherwise resolve to a DIFFERENT function, which
                // means the name is already reachable another way; a message call has no
                // such fallback, so an unresolvable token here is a failed recompile.
                const FString OwnerName = OwnerClass->GetName();
                const FString ClassName = Node->IsA<UK2Node_Message>()
                    ? OwnerName
                    : StripBPGeneratedClassSuffix(OwnerName);
                // Each half is formatted independently so backtick wrapping
                // applies per-token; the `::` separator is preserved literally
                // because the parser splits on the last `::` before unwrapping.
                return FString::Printf(TEXT("%s::%s"),
                    *FIrTextUtils::FormatNameToken(ClassName),
                    *FIrTextUtils::FormatNameToken(FuncName));
            }
        }
        return FuncName;
    }

    return Node->GetFunctionName().ToString();
}

// ---------------------------------------------------------------------------
// Helper: normalize event names
// ---------------------------------------------------------------------------

// Single source of truth for the ReceiveXxx -> clean-name mapping shared by
// NormalizeEventName (transform) and IsStandardOverrideEventName (predicate).
static const TMap<FString, FString>& GetStandardOverrideEventNameMap()
{
    static const TMap<FString, FString> Map = {
        { TEXT("ReceiveBeginPlay"),         TEXT("BeginPlay") },
        { TEXT("ReceiveTick"),              TEXT("Tick") },
        { TEXT("ReceiveEndPlay"),           TEXT("EndPlay") },
        { TEXT("ReceiveActorBeginOverlap"), TEXT("ActorBeginOverlap") },
        { TEXT("ReceiveActorEndOverlap"),   TEXT("ActorEndOverlap") },
        { TEXT("ReceiveHit"),               TEXT("Hit") },
        { TEXT("ReceiveDestroyed"),         TEXT("Destroyed") },
        { TEXT("ReceiveAnyDamage"),         TEXT("AnyDamage") },
        { TEXT("ReceivePointDamage"),       TEXT("PointDamage") },
        { TEXT("ReceiveRadialDamage"),      TEXT("RadialDamage") },
    };
    return Map;
}

FString FBpirTextEmitter::NormalizeEventName(const FString& RawName)
{
    // Default (no class context): preserve the pre-existing behavior — only the
    // ReceiveXxx-prefixed AActor names map back to clean shorthand. UUserWidget
    // members like literal "Tick" are never in the map, so they pass through.
    if (const FString* Mapped = GetStandardOverrideEventNameMap().Find(RawName))
    {
        return *Mapped;
    }
    return RawName;
}

// Class-gated normalizer mirroring the compile-side ResolveOverrideEventName.
// Only strips the Receive-prefix on AActor subclasses; on a non-AActor parent
// that happens to have a ReceiveXxx-named UFunction we keep the literal name so
// the round trip through the (now class-aware) compiler stays symmetric.
static FString NormalizeEventNameForParent(const FString& RawName, const UClass* ParentClass)
{
    const FString* Mapped = GetStandardOverrideEventNameMap().Find(RawName);
    if (!Mapped)
    {
        return RawName;
    }
    if (ParentClass && !ParentClass->IsChildOf(AActor::StaticClass()))
    {
        // Only strip the Receive-prefix when the literal short name also exists
        // on the parent (so the compiler's literal-first path will pick it up).
        if (ParentClass->FindFunctionByName(FName(**Mapped)) == nullptr)
        {
            return RawName;
        }
    }
    return *Mapped;
}

static bool IsStandardOverrideEventName(const FString& RawName)
{
    return GetStandardOverrideEventNameMap().Contains(RawName);
}

// ---------------------------------------------------------------------------
// Helper: detect "vacuous default" optional pins that should be omitted from
// the emitted arg list. A pin is omittable when it has no incoming connection,
// no default object, no default text, and either no default value, the literal
// "None", or matches the function's autogenerated default. The decompiler used
// to render these as `Name: ?` plus a warning, which the round-trip parser
// then has to strip anyway. Skipping them at the emitter is cleaner.
// File-local (only FormatArgs needs it; EmitVariableSet has its own single-pin
// path that legitimately wants to preserve `set Var = ?` for round-trip).
// ---------------------------------------------------------------------------

namespace BpirTextEmitterInternal
{
static UScriptStruct* ResolveStructTypeForPin(const UEdGraphPin* Pin)
{
    if (!Pin)
    {
        return nullptr;
    }

    if (UScriptStruct* Struct = Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()))
    {
        return Struct;
    }

    const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Pin->GetOwningNode());
    const UFunction* TargetFunction = CallNode ? CallNode->GetTargetFunction() : nullptr;
    const FProperty* ParameterProperty = TargetFunction ? TargetFunction->FindPropertyByName(Pin->PinName) : nullptr;
    const FStructProperty* StructProperty = CastField<FStructProperty>(ParameterProperty);
    return StructProperty ? StructProperty->Struct : nullptr;
}

bool IsPinOmittableAtCallSite(const UEdGraphPin* Pin)
{
    if (!Pin) return false;
    if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) return false;
    if (Pin->PinName == UEdGraphSchema_K2::PN_Self) return false;
    if (Pin->LinkedTo.Num() != 0) return false;
    if (Pin->DefaultObject != nullptr) return false;
    if (!Pin->DefaultTextValue.IsEmpty()) return false;

    // Object-ref pins serialize a null default as either "" or "None" depending
    // on the path; the engine's DoesDefaultValueMatchAutogenerated falls through
    // to IsDefaultAsStringEmpty which rejects "None", so we short-circuit here.
    {
        const FName& Cat = Pin->PinType.PinCategory;
        if (Cat == UEdGraphSchema_K2::PC_Object
            || Cat == UEdGraphSchema_K2::PC_Class
            || Cat == UEdGraphSchema_K2::PC_Interface
            || Cat == UEdGraphSchema_K2::PC_SoftObject
            || Cat == UEdGraphSchema_K2::PC_SoftClass)
        {
            if (Pin->DefaultValue.IsEmpty() || Pin->DefaultValue == TEXT("None"))
            {
                return true;
            }
        }

        // FName pins serialize an unset default as "" or the literal "None".
        // The strict DoesDefaultValueMatchAutogenerated path rejects "None"
        // (same root cause as object-ref pins), so short-circuit here.
        if (Cat == UEdGraphSchema_K2::PC_Name)
        {
            if (Pin->DefaultValue.IsEmpty() || Pin->DefaultValue == TEXT("None"))
            {
                return true;
            }
        }

        // Struct pins (FKey, FGameplayTag, FInstancedStruct, ...) serialize
        // their unset default as a non-empty ExportText form like
        // "(KeyName=\"None\")". Compare against a default-constructed instance
        // of the same struct via UScriptStruct::CompareScriptStruct so we omit
        // any struct pin whose value round-trips to the default.
        if (Cat == UEdGraphSchema_K2::PC_Struct)
        {
            if (Pin->DefaultValue.IsEmpty())
            {
                return true;
            }
            if (UScriptStruct* ST = ResolveStructTypeForPin(Pin))
            {
                FStructOnScope PinInstance(ST);
                FStructOnScope DefaultInstance(ST);
                // ImportText writes into PinInstance; on failure fall through
                // so the strict DoesDefaultValueMatchAutogenerated path runs.
                if (ST->ImportText(*Pin->DefaultValue, PinInstance.GetStructMemory(),
                        nullptr, PPF_None, GLog, ST->GetName()))
                {
                    if (ST->CompareScriptStruct(PinInstance.GetStructMemory(),
                            DefaultInstance.GetStructMemory(), PPF_None))
                    {
                        return true;
                    }
                }
            }
        }
    }

    // Only omit when the pin truly carries no value. A non-empty DefaultValue
    // is round-trip-significant even when it matches the function's
    // autogenerated default (e.g. PrintString's `InString="Hello"`): callers
    // depend on the literal surviving compile -> decompile -> recompile.
    if (!Pin->DefaultValue.IsEmpty() && Pin->DefaultValue != TEXT("None"))
    {
        return false;
    }
    return Pin->DoesDefaultValueMatchAutogenerated();
}

constexpr int32 MaxSplitPinDepth = 64;

const TCHAR* SplitInputUnresolvedValue = TEXT("<unresolved>");

UEdGraphPin* GetSplitPinRoot(UEdGraphPin* Pin)
{
    int32 Hops = 0;
    while (Pin && Pin->ParentPin && ++Hops <= MaxSplitPinDepth)
    {
        Pin = Pin->ParentPin;
    }
    return Pin;
}

bool HasSplitChildren(const UEdGraphNode* /*Node*/, const UEdGraphPin* ParentPin)
{
    // SplitPin always records its direct children on the parent. Keeping this
    // check local avoids rescanning Node->Pins for every ordinary argument.
    return ParentPin && ParentPin->SubPins.Num() > 0;
}

bool HasSplitInputChildren(const UEdGraphNode* /*Node*/, const UEdGraphPin* ParentPin)
{
    // Preserve the split path even when a child is malformed; the builder then
    // emits the unresolved sentinel instead of silently dropping the parent.
    return ParentPin && ParentPin->SubPins.Num() > 0;
}

bool IsLiveSplitPin(const UEdGraphNode* Node, const UEdGraphPin* Pin)
{
    return Node && Pin && !Pin->bOrphanedPin && !Pin->bWasTrashed
        && Node->Pins.Contains(const_cast<UEdGraphPin*>(Pin))
        && Pin->GetOwningNode() == Node;
}

bool HasExactSplitMemberPrefix(const UEdGraphPin* ParentPin, const UEdGraphPin* Pin)
{
    if (!ParentPin || !Pin)
    {
        return false;
    }

    const FString Prefix = ParentPin->PinName.ToString() + TEXT("_");
    const FString PinName = Pin->PinName.ToString();
    return PinName.Len() > Prefix.Len()
        && PinName.StartsWith(Prefix, ESearchCase::CaseSensitive);
}

FString FormatSplitMemberName(const UEdGraphPin* Pin)
{
    if (!Pin || !Pin->ParentPin)
    {
        return FString();
    }

    if (!HasExactSplitMemberPrefix(Pin->ParentPin, Pin))
    {
        return FString();
    }

    const FString PinName = Pin->PinName.ToString();
    const FString ParentPrefix = Pin->ParentPin->PinName.ToString() + TEXT("_");
    const FString MemberName = PinName.RightChop(ParentPrefix.Len());
    return FIrTextUtils::FormatNameToken(MemberName);
}

FString AllocateSplitValueName(const UEdGraphNode* Node, int32 Index)
{
    const FString NodeId = Node && Node->NodeGuid.IsValid()
        ? Node->NodeGuid.ToString(EGuidFormats::Digits)
        : TEXT("node");
    return FString::Printf(TEXT("split_%s_%d"), *NodeId, Index);
}

bool BuildSplitInputValue(
    UEdGraphNode* Node,
    UEdGraphPin* ParentPin,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin,
    int32& NextSplitValueIndex,
    TArray<FString>& GeneratedLines,
    FString& OutValueName,
    int32 Depth)
{
    if (!Node || !ParentPin || Depth >= MaxSplitPinDepth
        || !IsLiveSplitPin(Node, ParentPin) || ParentPin->SubPins.Num() == 0)
    {
        return false;
    }

    UScriptStruct* StructType = ResolveStructTypeForPin(ParentPin);
    if (!StructType)
    {
        return false;
    }

    TArray<FString> MemberParts;
    auto AppendMember = [&](UEdGraphPin* Pin) -> bool
    {
        if (!IsLiveSplitPin(Node, Pin) || Pin->ParentPin != ParentPin
            || Pin->Direction != EGPD_Input || !HasExactSplitMemberPrefix(ParentPin, Pin))
        {
            return false;
        }

        FString Value;
        if (HasSplitInputChildren(Node, Pin))
        {
            FString NestedValueName;
            if (!BuildSplitInputValue(
                    Node,
                    Pin,
                    ResolvePin,
                    NextSplitValueIndex,
                    GeneratedLines,
                    NestedValueName,
                    Depth + 1))
            {
                return false;
            }
            Value = FString::Printf(TEXT("%%%s"), *NestedValueName);
        }
        else
        {
            Value = ResolvePin(Pin);
        }

        MemberParts.Add(FString::Printf(TEXT("%s: %s"), *FormatSplitMemberName(Pin), *Value));
        return true;
    };

    for (UEdGraphPin* Pin : ParentPin->SubPins)
    {
        if (!AppendMember(Pin))
        {
            return false;
        }
    }

    if (MemberParts.Num() == 0)
    {
        return false;
    }

    OutValueName = AllocateSplitValueName(Node, NextSplitValueIndex++);
    GeneratedLines.Add(FString::Printf(
        TEXT("%%%s = make<%s>(%s)"),
        *OutValueName,
        *StructType->GetName(),
        *FString::Join(MemberParts, TEXT(", "))));
    return true;
}

bool TryBuildSplitInputValue(
    UEdGraphNode* Node,
    UEdGraphPin* Pin,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin,
    TSet<UEdGraphPin*>& ProcessedRoots,
    int32& NextSplitValueIndex,
    TArray<FString>& GeneratedLines,
    FString& OutValue)
{
    if (!Node || !Pin || !IsValid(Node) || Pin->ParentPin
        || !HasSplitInputChildren(Node, Pin) || !IsLiveSplitPin(Node, Pin))
    {
        UE_LOG(LogBpirTextEmitter, Warning,
            TEXT("Cannot reconstruct split input; invalid root or child topology; emitted %s"),
            SplitInputUnresolvedValue);
        return false;
    }
    if (!Node->NodeGuid.IsValid())
    {
        UE_LOG(LogBpirTextEmitter, Warning,
            TEXT("Cannot emit split input on node '%s' without a valid node GUID; emitted %s"),
            *Node->GetName(), SplitInputUnresolvedValue);
        return false;
    }
    if (ProcessedRoots.Contains(Pin))
    {
        UE_LOG(LogBpirTextEmitter, Warning,
            TEXT("Cannot reconstruct split input on node '%s' more than once; emitted %s"),
            *Node->GetName(), SplitInputUnresolvedValue);
        return false;
    }
    const int32 InitialGeneratedLineCount = GeneratedLines.Num();
    const int32 InitialSplitValueIndex = NextSplitValueIndex;
    ProcessedRoots.Add(Pin);

    FString ValueName;
    if (!BuildSplitInputValue(
            Node,
            Pin,
            ResolvePin,
            NextSplitValueIndex,
            GeneratedLines,
            ValueName,
            0))
    {
        GeneratedLines.SetNum(InitialGeneratedLineCount);
        NextSplitValueIndex = InitialSplitValueIndex;
        ProcessedRoots.Remove(Pin);
        OutValue.Reset();
        UE_LOG(LogBpirTextEmitter, Warning,
            TEXT("Cannot reconstruct split input '%s' on node '%s'; emitted %s"),
            *Pin->PinName.ToString(), *Node->GetName(), SplitInputUnresolvedValue);
        return false;
    }

    OutValue = FString::Printf(TEXT("%%%s"), *ValueName);
    return true;
}

FString AddGeneratedPrefix(const FString& GeneratedPrefix, const FString& Line)
{
    if (GeneratedPrefix.IsEmpty())
    {
        return Line;
    }
    if (Line.IsEmpty())
    {
        return GeneratedPrefix;
    }
    return GeneratedPrefix + TEXT("\n") + Line;
}
}

// ---------------------------------------------------------------------------
// Helper: format data input args
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::FormatArgs(
    UEdGraphNode* Node,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin,
    bool bSkipSelfPin,
    const TSet<FName>* PinNamesToSkip,
    FString* OutGeneratedPrefix)
{
    TArray<FString> ArgParts;
    TArray<FString> GeneratedLines;
    TSet<UEdGraphPin*> ProcessedSplitRoots;
    int32 NextSplitValueIndex = 0;

    if (OutGeneratedPrefix)
    {
        OutGeneratedPrefix->Reset();
    }

    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin) continue;
        if (Pin->Direction != EGPD_Input) continue;
        if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

        UEdGraphPin* SplitRoot = BpirTextEmitterInternal::GetSplitPinRoot(Pin);
        if (BpirTextEmitterInternal::HasSplitInputChildren(Node, SplitRoot))
        {
            if (Pin != SplitRoot) continue;
            if (bSkipSelfPin && SplitRoot->PinName == UEdGraphSchema_K2::PN_Self) continue;
            if (PinNamesToSkip && PinNamesToSkip->Contains(SplitRoot->PinName)) continue;

            FString Value;
            if (BpirTextEmitterInternal::TryBuildSplitInputValue(
                    Node,
                    SplitRoot,
                    ResolvePin,
                    ProcessedSplitRoots,
                    NextSplitValueIndex,
                    GeneratedLines,
                    Value))
            {
                const FString PinName = FIrTextUtils::FormatNameToken(SplitRoot->PinName.ToString());
                ArgParts.Add(FString::Printf(TEXT("%s: %s"), *PinName, *Value));
            }
            else
            {
                const FString PinName = FIrTextUtils::FormatNameToken(SplitRoot->PinName.ToString());
                ArgParts.Add(FString::Printf(TEXT("%s: %s"), *PinName,
                    BpirTextEmitterInternal::SplitInputUnresolvedValue));
            }
            continue;
        }

        if (bSkipSelfPin && Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;
        if (PinNamesToSkip && PinNamesToSkip->Contains(Pin->PinName)) continue;

        if (Pin->bOrphanedPin || Pin->bWasTrashed)
        {
            const FString PinName = FIrTextUtils::FormatNameToken(Pin->PinName.ToString());
            ArgParts.Add(FString::Printf(TEXT("%s: %s"), *PinName,
                BpirTextEmitterInternal::SplitInputUnresolvedValue));
            UE_LOG(LogBpirTextEmitter, Warning,
                TEXT("Cannot emit invalid input pin '%s' on node '%s'; emitted %s"),
                *Pin->PinName.ToString(), *Node->GetName(),
                BpirTextEmitterInternal::SplitInputUnresolvedValue);
            continue;
        }

        if (Pin->bHidden) continue;
        if (Pin->ParentPin) continue;

        if (BpirTextEmitterInternal::IsPinOmittableAtCallSite(Pin)) continue;

        FString Value = ResolvePin(Pin);
        FString PinName = FIrTextUtils::FormatNameToken(Pin->PinName.ToString());

        ArgParts.Add(FString::Printf(TEXT("%s: %s"), *PinName, *Value));
    }

    if (OutGeneratedPrefix)
    {
        *OutGeneratedPrefix = FString::Join(GeneratedLines, TEXT("\n"));
    }
    LastGeneratedPrefixLineCount = GeneratedLines.Num();
    return FString::Join(ArgParts, TEXT(", "));
}

// ---------------------------------------------------------------------------
// Helper: format exec target clauses
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::FormatExecTargets(const FBpirLabelMap& LabelMap)
{
    if (LabelMap.Num() == 0)
    {
        return FString();
    }

    TArray<FString> Parts;
    for (const auto& Pair : LabelMap)
    {
        const FString LabelTok = FIrTextUtils::FormatNameToken(Pair.Value.Label);
        if (Pair.Value.TargetInputPinName.IsEmpty())
        {
            Parts.Add(FString::Printf(TEXT("%s -> @%s"), *Pair.Key, *LabelTok));
        }
        else
        {
            const FString PinTok = FIrTextUtils::FormatNameToken(Pair.Value.TargetInputPinName);
            Parts.Add(FString::Printf(TEXT("%s -> @%s.%s"), *Pair.Key, *LabelTok, *PinTok));
        }
    }
    Parts.Sort();

    return FString::Printf(TEXT(" [%s]"), *FString::Join(Parts, TEXT(", ")));
}

FString FBpirTextEmitter::FormatEnumExecTargets(const FBpirLabelMap& LabelMap, UEnum* EnumType)
{
    if (LabelMap.Num() == 0)
    {
        return FString();
    }

    const FString EnumName = EnumType ? EnumType->GetName() : FString();

    TArray<FString> Parts;
    for (const auto& Pair : LabelMap)
    {
        FString CaseName = Pair.Key.TrimStartAndEnd();
        if (!CaseName.Equals(TEXT("default"), ESearchCase::IgnoreCase)
            && !EnumName.IsEmpty())
        {
            CaseName = FormatEnumLiteralForBpir(CaseName, EnumName);
            if (!CaseName.Contains(TEXT("::")))
            {
                CaseName = FString::Printf(TEXT("%s::%s"), *EnumName, *CaseName);
            }
        }
        const FString LabelTok = FIrTextUtils::FormatNameToken(Pair.Value.Label);
        if (Pair.Value.TargetInputPinName.IsEmpty())
        {
            Parts.Add(FString::Printf(TEXT("%s -> @%s"), *CaseName, *LabelTok));
        }
        else
        {
            const FString PinTok = FIrTextUtils::FormatNameToken(Pair.Value.TargetInputPinName);
            Parts.Add(FString::Printf(TEXT("%s -> @%s.%s"), *CaseName, *LabelTok, *PinTok));
        }
    }
    Parts.Sort();

    return FString::Printf(TEXT(" [%s]"), *FString::Join(Parts, TEXT(", ")));
}

// ---------------------------------------------------------------------------
// Helper: format target prefix for member calls
// ---------------------------------------------------------------------------

TArray<FString> FBpirTextEmitter::FormatTargetPrefix(
    UEdGraphNode* Node,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    TArray<FString> Out;
    if (!Node)
    {
        return Out;
    }

    UEdGraphPin* SelfPin = nullptr;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Input
            && Pin->PinName == UEdGraphSchema_K2::PN_Self
            && Pin->LinkedTo.Num() > 0)
        {
            SelfPin = Pin;
            break;
        }
    }
    if (!SelfPin)
    {
        return Out;
    }

    // A `Target: self` is normally redundant: an unconnected UK2Node_CallFunction self pin
    // already means self-context, so the recompile rebuilds the same graph without it.
    // UK2Node_Message has no such fallback — its self pin is a plain object pin
    // (K2Node_Message.cpp:88-93) and an unconnected one dispatches to nothing — so the
    // target must survive the round trip verbatim.
    const bool bSelfTargetIsRedundant = !Node->IsA<UK2Node_Message>();

    if (SelfPin->LinkedTo.Num() == 1 || !BpirDecompiler::Helpers::NodeSupportsMultiSelf(Node))
    {
        FString TargetRef = ResolvePin(SelfPin);
        if (!bSelfTargetIsRedundant || TargetRef != TEXT("self"))
        {
            Out.Add(FString::Printf(TEXT("Target: %s"), *TargetRef));
        }
        return Out;
    }

    // ResolvePin reads LinkedTo[0], so to fan out we promote each link to slot 0
    // and restore on the same iteration. The swap-and-restore pair is safe
    // because ResolveInputValue is fully synchronous, UE C++ has no exceptions,
    // and the graph treats LinkedTo as an unordered set (ResolveInputValue is
    // the only LinkedTo[0] consumer; nothing else observes the transient order).
    const int32 NumLinks = SelfPin->LinkedTo.Num();
    for (int32 i = 0; i < NumLinks; ++i)
    {
        if (i != 0)
        {
            SelfPin->LinkedTo.Swap(0, i);
        }
        FString TargetRef = ResolvePin(SelfPin);
        if (i != 0)
        {
            SelfPin->LinkedTo.Swap(0, i);
        }
        if (!bSelfTargetIsRedundant || TargetRef != TEXT("self"))
        {
            Out.Add(FString::Printf(TEXT("Target: %s"), *TargetRef));
        }
    }
    return Out;
}

// ---------------------------------------------------------------------------
// Entry signatures
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::GetNodeEnabledStateMarker(const UEdGraphNode* Node)
{
    if (!Node)
    {
        return FString();
    }

    switch (Node->GetDesiredEnabledState())
    {
    case ENodeEnabledState::Disabled:
        return BpirSharedConstants::Keywords::Disabled;
    case ENodeEnabledState::DevelopmentOnly:
        return BpirSharedConstants::Keywords::DevelopmentOnly;
    case ENodeEnabledState::Enabled:
    default:
        return FString();
    }
}

namespace
{
    // Source-of-truth view onto an entry node's user-editable metadata. Each
    // entry kind stores fields in a different shape (FunctionEntry uses
    // MetaData + ExtraFlags; CustomEvent uses GetUserDefinedMetaData() + the
    // inherited FunctionFlags; Tunnel uses MetaData only), so the emit branches
    // populate this struct and a single formatter consumes it.
    struct FEntryMetadataView
    {
        // Typed FKismetUserDeclaredFunctionMetadata fields
        FText Category;
        FText Tooltip;
        FText Keywords;
        FText CompactNodeTitle;
        FString DeprecationMessage;
        bool bCallInEditor = false;
        bool bThreadSafe = false;
        bool bUnsafeDuringActorConstruction = false;
        bool bDeprecated = false;

        // EFunctionFlags bag (ExtraFlags for FunctionEntry, FunctionFlags for CustomEvent).
        // Macros leave this at zero and the formatter skips @flags() for them.
        bool bHasFlags = false;        // true for function/override/custom_event; false for macro
        int32 FunctionFlagBits = 0;    // raw EFunctionFlags bag (already FUNC_Native-stripped)
    };

    bool IsTextNonEmpty(const FText& T)
    {
        return !T.IsEmpty();
    }

    // Format an FText value for embedding inside `@meta(Key=Value)`. NSLOCTEXT
    // identity is preserved; identity-less FText (display string only) round-trips
    // as a bare quoted string and the parser will lift it into FText::FromString
    // on compile.
    FString FormatFTextValueForMeta(const FText& Value)
    {
        const TOptional<FString> Namespace = FTextInspector::GetNamespace(Value);
        const TOptional<FString> Key = FTextInspector::GetKey(Value);
        const FString Display = Value.ToString();

        if (Namespace.IsSet() && !Namespace.GetValue().IsEmpty()
            && Key.IsSet() && !Key.GetValue().IsEmpty())
        {
            const FString NsLoc = FString::Printf(
                TEXT("NSLOCTEXT(\"%s\", \"%s\", \"%s\")"),
                *BpirStructLiteralUtils::EscapeBpirStringInner(Namespace.GetValue()),
                *BpirStructLiteralUtils::EscapeBpirStringInner(Key.GetValue()),
                *BpirStructLiteralUtils::EscapeBpirStringInner(Display));
            return BpirStructLiteralUtils::EscapeBpirString(NsLoc);
        }

        return BpirStructLiteralUtils::EscapeBpirString(Display);
    }

    FString FormatFStringValueForMeta(const FString& Value)
    {
        return BpirStructLiteralUtils::EscapeBpirString(Value);
    }

    // Build the `@meta(...)` line for a single entry view. Returns empty string
    // when no field is set (omission rule). Key order is fixed: Category, Tooltip,
    // Keywords, CompactNodeTitle, DeprecationMessage.
    FString BuildMetaDecoratorLine(const FEntryMetadataView& View)
    {
        TArray<FString> Entries;
        if (IsTextNonEmpty(View.Category))
        {
            Entries.Add(FString::Printf(TEXT("Category=%s"), *FormatFTextValueForMeta(View.Category)));
        }
        if (IsTextNonEmpty(View.Tooltip))
        {
            Entries.Add(FString::Printf(TEXT("Tooltip=%s"), *FormatFTextValueForMeta(View.Tooltip)));
        }
        if (IsTextNonEmpty(View.Keywords))
        {
            Entries.Add(FString::Printf(TEXT("Keywords=%s"), *FormatFTextValueForMeta(View.Keywords)));
        }
        if (IsTextNonEmpty(View.CompactNodeTitle))
        {
            Entries.Add(FString::Printf(TEXT("CompactNodeTitle=%s"), *FormatFTextValueForMeta(View.CompactNodeTitle)));
        }
        if (!View.DeprecationMessage.IsEmpty())
        {
            Entries.Add(FString::Printf(TEXT("DeprecationMessage=%s"), *FormatFStringValueForMeta(View.DeprecationMessage)));
        }

        if (Entries.Num() == 0)
        {
            return FString();
        }
        return FString::Printf(TEXT("@meta(%s)"), *FString::Join(Entries, TEXT(", ")));
    }

    // Build the `@flags(...)` line for a single entry view. Returns empty string
    // when no identifier would appear (omission rule). Access specifier `Public`
    // is the engine default — emit it only if any other flag would already force
    // the line to be present.
    FString BuildFlagsDecoratorLine(const FEntryMetadataView& View)
    {
        if (!View.bHasFlags)
        {
            return FString();
        }

        const bool bPure = (View.FunctionFlagBits & FUNC_BlueprintPure) != 0;
        const bool bConst = (View.FunctionFlagBits & FUNC_Const) != 0;
        const bool bExec = (View.FunctionFlagBits & FUNC_Exec) != 0;
        const bool bProtected = (View.FunctionFlagBits & FUNC_Protected) != 0;
        const bool bPrivate = (View.FunctionFlagBits & FUNC_Private) != 0;
        // Engine default is FUNC_Public; emit access only if forced by another flag below.
        const bool bExplicitPublic = (View.FunctionFlagBits & FUNC_Public) != 0;

        const bool bOtherFlagsPresent = bPure || bConst || bExec
            || View.bCallInEditor || View.bThreadSafe
            || View.bUnsafeDuringActorConstruction || View.bDeprecated;

        const bool bAnyNonPublicAccess = bProtected || bPrivate;
        const bool bIncludeAccess = bAnyNonPublicAccess
            || (bExplicitPublic && bOtherFlagsPresent);

        if (!bOtherFlagsPresent && !bAnyNonPublicAccess)
        {
            // Default state: Public + nothing else. Omit @flags entirely.
            return FString();
        }

        TArray<FString> Ids;
        if (bIncludeAccess)
        {
            if (bPrivate)        Ids.Add(TEXT("Private"));
            else if (bProtected) Ids.Add(TEXT("Protected"));
            else                 Ids.Add(TEXT("Public"));
        }
        if (bPure)                              Ids.Add(TEXT("Pure"));
        if (bConst)                             Ids.Add(TEXT("Const"));
        if (bExec)                              Ids.Add(TEXT("Exec"));
        if (View.bCallInEditor)                 Ids.Add(TEXT("CallInEditor"));
        if (View.bThreadSafe)                   Ids.Add(TEXT("ThreadSafe"));
        if (View.bUnsafeDuringActorConstruction)Ids.Add(TEXT("UnsafeDuringActorConstruction"));
        if (View.bDeprecated)                   Ids.Add(TEXT("Deprecated"));

        if (Ids.Num() == 0)
        {
            return FString();
        }
        return FString::Printf(TEXT("@flags(%s)"), *FString::Join(Ids, TEXT(", ")));
    }
}

TArray<FString> FBpirTextEmitter::EmitEntryDecoratorLines(UEdGraphNode* EntryNode)
{
    TArray<FString> Lines;
    if (!EntryNode)
    {
        return Lines;
    }

    FEntryMetadataView View;

    if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(EntryNode))
    {
        // Typed FKismetUserDeclaredFunctionMetadata fields live in the node's
        // private MetaData (reachable via GetUserDefinedMetaData()).
        const FKismetUserDeclaredFunctionMetadata& Meta = CustomEvent->GetUserDefinedMetaData();
        View.Category = Meta.Category;
        View.Tooltip = Meta.ToolTip;
        View.Keywords = Meta.Keywords;
        View.CompactNodeTitle = Meta.CompactNodeTitle;
        View.bThreadSafe = Meta.bThreadSafe;
        View.bUnsafeDuringActorConstruction = Meta.bIsUnsafeDuringActorConstruction;
        // CustomEvent carries DeprecationMessage / bIsDeprecated / bCallInEditor
        // as its own public UPROPERTY fields (separate from MetaData), so prefer
        // the node-level copies — they are the editor-set source of truth.
        View.DeprecationMessage = CustomEvent->DeprecationMessage;
        View.bDeprecated = CustomEvent->bIsDeprecated;
        View.bCallInEditor = CustomEvent->bCallInEditor;

        // FunctionFlags is the EFunctionFlags bag for custom events. Strip
        // FUNC_Native defensively to mirror UK2Node_FunctionEntry::SetExtraFlags.
        View.bHasFlags = true;
        View.FunctionFlagBits = static_cast<int32>(CustomEvent->FunctionFlags & ~FUNC_Native);
    }
    else if (UK2Node_FunctionEntry* FuncEntry = Cast<UK2Node_FunctionEntry>(EntryNode))
    {
        const FKismetUserDeclaredFunctionMetadata& Meta = FuncEntry->MetaData;
        View.Category = Meta.Category;
        View.Tooltip = Meta.ToolTip;
        View.Keywords = Meta.Keywords;
        View.CompactNodeTitle = Meta.CompactNodeTitle;
        View.DeprecationMessage = Meta.DeprecationMessage;
        View.bCallInEditor = Meta.bCallInEditor;
        View.bThreadSafe = Meta.bThreadSafe;
        View.bUnsafeDuringActorConstruction = Meta.bIsUnsafeDuringActorConstruction;
        View.bDeprecated = Meta.bIsDeprecated;

        View.bHasFlags = true;
        // SetExtraFlags strips FUNC_Native on the parse-back side; mirror here
        // so emit never produces an identifier the round-trip would drop.
        View.FunctionFlagBits = FuncEntry->GetExtraFlags() & ~FUNC_Native;
    }
    else if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(EntryNode))
    {
        // Only emit decorators for entry tunnels in macro graphs (matches the
        // EmitEntrySignature gating: bCanHaveOutputs entry tunnel).
        const FKismetUserDeclaredFunctionMetadata& Meta = Tunnel->MetaData;
        View.Category = Meta.Category;
        View.Tooltip = Meta.ToolTip;
        View.Keywords = Meta.Keywords;
        View.CompactNodeTitle = Meta.CompactNodeTitle;
        View.DeprecationMessage = Meta.DeprecationMessage;
        // Macros have no EFunctionFlags bag — @flags(...) is rejected by the parser
        // on `entry macro` lines, so emit must skip it too.
        View.bHasFlags = false;
    }
    else
    {
        // Engine events (UK2Node_Event, including ComponentBoundEvent / InputAction
        // / etc.) carry no user-editable BPIR-grammar metadata.
        return Lines;
    }

    const FString MetaLine = BuildMetaDecoratorLine(View);
    if (!MetaLine.IsEmpty())
    {
        Lines.Add(MetaLine);
    }
    const FString FlagsLine = BuildFlagsDecoratorLine(View);
    if (!FlagsLine.IsEmpty())
    {
        Lines.Add(FlagsLine);
    }
    return Lines;
}

const TCHAR* const FBpirTextEmitter::UnknownEntrySignature = TEXT("entry event UnknownEntry()");

FString FBpirTextEmitter::EmitEntrySignature(
    UEdGraphNode* EntryNode,
    const UBlueprint* BlueprintContext,
    const FString& SourceGraphName,
    bool bIsInterfaceGraph)
{
    if (!EntryNode)
    {
        return UnknownEntrySignature;
    }

    auto AppendEntryPosition = [EntryNode](const FString& Signature) -> FString
    {
        const FString EnabledState = FBpirTextEmitter::GetNodeEnabledStateMarker(EntryNode);
        const FString EnabledStateSuffix = EnabledState.IsEmpty()
            ? FString()
            : FString::Printf(TEXT(" %s"), *EnabledState);
        return FString::Printf(
            TEXT("%s%s @(%d, %d)"),
            *Signature,
            *EnabledStateSuffix,
            EntryNode->NodePosX,
            EntryNode->NodePosY);
    };

    auto CollectParams = [](UEdGraphNode* Node) -> FString
    {
        TArray<FString> ParamList;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (IsPrimaryOutputDataPin(Pin))
            {
                FString TypeStr = PinTypeToBpirType(Pin->PinType);
                FString ParamName = FIrTextUtils::FormatNameToken(Pin->PinName.ToString());
                ParamList.Add(FString::Printf(TEXT("%s %s"), *TypeStr, *ParamName));
            }
        }
        return FString::Join(ParamList, TEXT(", "));
    };

    // UK2Node_CustomEvent must be checked before UK2Node_Event (it derives from it)
    if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(EntryNode))
    {
        FString EventName = CustomEvent->CustomFunctionName.ToString();
        FString Params = CollectParams(EntryNode);
        return AppendEntryPosition(FString::Printf(TEXT("entry custom_event %s(%s)"), *EventName, *Params));
    }

    if (UK2Node_ComponentBoundEvent* CompEvent = Cast<UK2Node_ComponentBoundEvent>(EntryNode))
    {
        FString CompName = CompEvent->ComponentPropertyName.ToString();
        FString EventName = CompEvent->DelegatePropertyName.ToString();
        FString Params = CollectParams(EntryNode);

#if MCP_HAS_WIDGET_BLUEPRINT_CONTEXT
        const bool bIsWidgetBlueprint = Cast<UWidgetBlueprint>(BlueprintContext) != nullptr;
#else
        const bool bIsWidgetBlueprint = false;
#endif
        FString Kind = bIsWidgetBlueprint ? TEXT("widget_event") : TEXT("component_event");
        return AppendEntryPosition(FString::Printf(TEXT("entry %s %s.%s(%s)"), *Kind, *CompName, *EventName, *Params));
    }

    auto InputEventToken = [](EInputEvent Ev) -> FString
    {
        switch (Ev)
        {
            case IE_Pressed:     return TEXT("IE_Pressed");
            case IE_Released:    return TEXT("IE_Released");
            case IE_Repeat:      return TEXT("IE_Repeat");
            case IE_DoubleClick: return TEXT("IE_DoubleClick");
            case IE_Axis:        return TEXT("IE_Axis");
            default:             return FString::Printf(TEXT("%d"), static_cast<int32>(Ev));
        }
    };

    auto JoinSignature = [](const FString& EventTok, const FString& Params) -> FString
    {
        if (EventTok.IsEmpty()) return Params;
        if (Params.IsEmpty()) return EventTok;
        return EventTok + TEXT(", ") + Params;
    };

    if (UClass* EnhancedInputNodeClass = FCodeNodeEmitter::ResolveEnhancedInputActionNodeClass();
        EnhancedInputNodeClass && EntryNode->IsA(EnhancedInputNodeClass))
    {
        const UInputAction* InputAction = FCodeNodeEmitter::GetEnhancedInputAction(EntryNode);
        if (!InputAction)
        {
            return AppendEntryPosition(
                TEXT("# [WARNING] Unbound K2Node_EnhancedInputAction cannot be represented\nentry input_action <UNBOUND>()"));
        }

        TArray<FString> ConnectedEvents;
        for (const FName EventPinName : FCodeNodeEmitter::GetEnhancedInputActionEventPinNames())
        {
            UEdGraphPin* EventPin = EntryNode->FindPin(EventPinName, EGPD_Output);
            if (EventPin && EventPin->LinkedTo.Num() > 0)
            {
                ConnectedEvents.Add(EventPinName.ToString());
            }
        }

        FString Signature = FString::Printf(
            TEXT("entry input_action %s()"), *InputAction->GetPathName());
        if (ConnectedEvents.Num() > 1
            || (ConnectedEvents.Num() == 1 && ConnectedEvents[0] != TEXT("Triggered")))
        {
            TArray<FString> Targets;
            for (const FString& EventName : ConnectedEvents)
            {
                Targets.Add(FString::Printf(
                    TEXT("%s -> @%s"), *EventName, *EventName.ToLower()));
            }
            Signature += FString::Printf(TEXT(" [%s]"), *FString::Join(Targets, TEXT(", ")));
        }
        return AppendEntryPosition(Signature);
    }

    // UK2Node_InputAction is the legacy combined node — does NOT derive from UK2Node_Event.
    if (UK2Node_InputAction* InputActionNode = Cast<UK2Node_InputAction>(EntryNode))
    {
        FString ActionName = InputActionNode->InputActionName.ToString();
        FString Params = CollectParams(EntryNode);

        bool bIsReleased = false;
        for (UEdGraphPin* Pin : EntryNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                && Pin->PinName == TEXT("Released")
                && Pin->LinkedTo.Num() > 0)
            {
                bIsReleased = true;
                break;
            }
        }

        const TCHAR* Kind = bIsReleased ? TEXT("input_action_released") : TEXT("input_action_pressed");
        return AppendEntryPosition(FString::Printf(TEXT("entry %s %s(%s)"), Kind, *ActionName, *Params));
    }

    // Subclass-first ordering: UK2Node_InputVectorAxisEvent derives from UK2Node_InputAxisKeyEvent.
    if (UK2Node_InputVectorAxisEvent* VecAxisNode = Cast<UK2Node_InputVectorAxisEvent>(EntryNode))
    {
        FString KeyName = VecAxisNode->AxisKey.GetFName().ToString();
        FString Sig = JoinSignature(FString(), CollectParams(EntryNode));
        return AppendEntryPosition(FString::Printf(TEXT("entry input_vector_axis %s(%s)"), *KeyName, *Sig));
    }

    if (UK2Node_InputAxisKeyEvent* AxisKeyNode = Cast<UK2Node_InputAxisKeyEvent>(EntryNode))
    {
        FString KeyName = AxisKeyNode->AxisKey.GetFName().ToString();
        FString Sig = JoinSignature(FString(), CollectParams(EntryNode));
        return AppendEntryPosition(FString::Printf(TEXT("entry input_axis_key %s(%s)"), *KeyName, *Sig));
    }

    if (UK2Node_InputAxisEvent* AxisNode = Cast<UK2Node_InputAxisEvent>(EntryNode))
    {
        FString AxisName = AxisNode->InputAxisName.ToString();
        FString Sig = JoinSignature(FString(), CollectParams(EntryNode));
        return AppendEntryPosition(FString::Printf(TEXT("entry input_axis %s(%s)"), *AxisName, *Sig));
    }

    if (UK2Node_InputActionEvent* ActionEventNode = Cast<UK2Node_InputActionEvent>(EntryNode))
    {
        FString ActionName = ActionEventNode->InputActionName.ToString();
        FString Sig = JoinSignature(InputEventToken(ActionEventNode->InputKeyEvent.GetValue()), CollectParams(EntryNode));
        return AppendEntryPosition(FString::Printf(TEXT("entry input_action_event %s(%s)"), *ActionName, *Sig));
    }

    if (UK2Node_InputKeyEvent* KeyEventNode = Cast<UK2Node_InputKeyEvent>(EntryNode))
    {
        FString KeyName = KeyEventNode->InputChord.Key.GetFName().ToString();
        FString Sig = JoinSignature(InputEventToken(KeyEventNode->InputKeyEvent.GetValue()), CollectParams(EntryNode));
        return AppendEntryPosition(FString::Printf(TEXT("entry input_key_event %s(%s)"), *KeyName, *Sig));
    }

    if (UK2Node_InputTouchEvent* TouchNode = Cast<UK2Node_InputTouchEvent>(EntryNode))
    {
        FString Sig = JoinSignature(InputEventToken(TouchNode->InputKeyEvent.GetValue()), CollectParams(EntryNode));
        return AppendEntryPosition(FString::Printf(TEXT("entry input_touch_event(%s)"), *Sig));
    }

    if (UK2Node_ActorBoundEvent* ActorBoundNode = Cast<UK2Node_ActorBoundEvent>(EntryNode))
    {
        FString DelegateName = ActorBoundNode->DelegatePropertyName.ToString();
        FString OwnerName = ActorBoundNode->EventOwner ? ActorBoundNode->EventOwner->GetName() : TEXT("None");
        FString Params = CollectParams(EntryNode);
        return AppendEntryPosition(FString::Printf(TEXT("entry actor_bound_event %s.%s(%s)"), *OwnerName, *DelegateName, *Params));
    }

#if MCP_HAS_K2NODE_GENERATEDBOUNDEVENT
    if (UK2Node_GeneratedBoundEvent* GenBoundNode = Cast<UK2Node_GeneratedBoundEvent>(EntryNode))
    {
        FString DelegateName = GenBoundNode->DelegatePropertyName.ToString();
        FString Params = CollectParams(EntryNode);
        return AppendEntryPosition(FString::Printf(TEXT("entry generated_bound_event %s(%s)"), *DelegateName, *Params));
    }
#endif // MCP_HAS_K2NODE_GENERATEDBOUNDEVENT

#if MCP_HAS_WIDGET_ANIMATION_EVENT
    if (UK2Node_WidgetAnimationEvent* AnimEventNode = Cast<UK2Node_WidgetAnimationEvent>(EntryNode))
    {
        FString AnimName = AnimEventNode->AnimationPropertyName.ToString();
        const TCHAR* ActionTok =
            AnimEventNode->Action == EWidgetAnimationEvent::Started ? TEXT("Started") :
            AnimEventNode->Action == EWidgetAnimationEvent::Finished ? TEXT("Finished") :
            TEXT("Unknown");
        FString UserTag = AnimEventNode->UserTag.IsNone() ? FString() : AnimEventNode->UserTag.ToString();
        FString Params = CollectParams(EntryNode);

        TArray<FString> ArgList;
        if (!UserTag.IsEmpty())
        {
            ArgList.Add(UserTag);
        }
        if (!Params.IsEmpty())
        {
            ArgList.Add(Params);
        }
        FString ArgStr = FString::Join(ArgList, TEXT(", "));
        return AppendEntryPosition(FString::Printf(TEXT("entry widget_animation_event %s.%s(%s)"), *AnimName, ActionTok, *ArgStr));
    }
#endif

    if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(EntryNode))
    {
        FString RawName = EventNode->EventReference.GetMemberName().ToString();
        if (RawName.IsEmpty())
        {
            RawName = EntryNode->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
        }
        FString EventName = NormalizeEventNameForParent(RawName, BlueprintContext ? BlueprintContext->ParentClass : nullptr);
        FString Params = CollectParams(EntryNode);
        const bool bEmitAsOverride = EventNode->bOverrideFunction && !IsStandardOverrideEventName(RawName);
        if (bEmitAsOverride)
        {
            return AppendEntryPosition(FString::Printf(TEXT("entry override %s(%s)"), *EventName, *Params));
        }
        return AppendEntryPosition(FString::Printf(TEXT("entry event %s(%s)"), *EventName, *Params));
    }

    if (UK2Node_FunctionEntry* FuncEntry = Cast<UK2Node_FunctionEntry>(EntryNode))
    {
        // Caller-supplied source graph name: the entry node may live on a
        // composite-flattening clone whose name is auto-suffixed.
        FString GraphName = SourceGraphName;

        // Check for construction script
        if (GraphName == TEXT("UserConstructionScript"))
        {
            return AppendEntryPosition(TEXT("entry construction ConstructionScript()"));
        }

        FString Params = CollectParams(EntryNode);

        // Collect ALL return pins from FunctionResult node.
        // NOTE: Only the first FunctionResult node is inspected. Blueprints with
        // multiple return nodes (different return paths) will only reflect the
        // return type of whichever result node appears first in Graph->Nodes.
        TArray<TPair<FString, FName>> ReturnPins; // {Type, Name}
        if (UEdGraph* Graph = EntryNode->GetGraph())
        {
            for (UEdGraphNode* GraphNode : Graph->Nodes)
            {
                if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(GraphNode))
                {
                    for (UEdGraphPin* Pin : ResultNode->Pins)
                    {
                        if (!Pin || Pin->Direction != EGPD_Input
                            || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                        {
                            continue;
                        }

                        UEdGraphPin* RootPin = BpirTextEmitterInternal::GetSplitPinRoot(Pin);
                        if (RootPin != Pin
                            || (Pin->bHidden
                                && !BpirTextEmitterInternal::HasSplitInputChildren(ResultNode, Pin)))
                        {
                            continue;
                        }

                        FString TypeStr = PinTypeToBpirType(Pin->PinType);
                        ReturnPins.Add({TypeStr, Pin->PinName});
                    }
                    break;
                }
            }
        }

        // Implemented-interface functions are override entries too. The explicit
        // source-graph flag survives the composite-flattening clone path, where the
        // working graph pointer no longer appears in ImplementedInterfaces[].Graphs.
        bool bEmitAsOverride = bIsInterfaceGraph;
        if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(EntryNode->GetGraph()))
        {
            UFunction* OverrideFunction = nullptr;
            const UClass* OverrideClass = FBlueprintEditorUtils::GetOverrideFunctionClass(
                Blueprint,
                FName(*GraphName),
                &OverrideFunction);
            // GetOverrideFunctionClass also finds functions on the current BP's skeleton.
            // Only treat as override when the function was declared by a parent class.
            const UClass* ParentClass = Blueprint->ParentClass;
            bEmitAsOverride = bEmitAsOverride
                || (OverrideFunction != nullptr && OverrideClass != nullptr
                    && ParentClass != nullptr && ParentClass->IsChildOf(OverrideClass));

            // Fallback: if GetOverrideFunctionClass failed (e.g., skeleton not regenerated
            // after BPIR compile), check the parent class directly for the function.
            if (!bEmitAsOverride && ParentClass)
            {
                if (ParentClass->FindFunctionByName(FName(*GraphName)))
                {
                    bEmitAsOverride = true;
                }
            }
        }

        const TCHAR* EntryKeyword = bEmitAsOverride ? TEXT("entry override") : TEXT("entry function");

        FString EmittedName = FIrTextUtils::FormatNameToken(GraphName);

        if (ReturnPins.Num() == 0)
        {
            return AppendEntryPosition(FString::Printf(TEXT("%s %s(%s)"), EntryKeyword, *EmittedName, *Params));
        }
        else if (ReturnPins.Num() == 1 && ReturnPins[0].Value == UEdGraphSchema_K2::PN_ReturnValue)
        {
            return AppendEntryPosition(FString::Printf(TEXT("%s %s(%s) -> %s"), EntryKeyword, *EmittedName, *Params, *ReturnPins[0].Key));
        }
        else
        {
            TArray<FString> ReturnParts;
            for (const auto& RP : ReturnPins)
            {
                const FString PinName = FIrTextUtils::FormatNameToken(RP.Value.ToString());
                ReturnParts.Add(FString::Printf(TEXT("%s %s"), *RP.Key, *PinName));
            }
            return AppendEntryPosition(FString::Printf(TEXT("%s %s(%s) -> (%s)"), EntryKeyword, *EmittedName, *Params, *FString::Join(ReturnParts, TEXT(", "))));
        }
    }

    if (UK2Node_InputKey* InputKeyNode = Cast<UK2Node_InputKey>(EntryNode))
    {
        FString KeyName = FBpirInputKeyHelpers::FormatInputKeyAsBpirIdentifier(InputKeyNode->InputKey);

        // Determine pressed vs released by checking which exec pin is wired
        bool bIsReleased = false;
        for (UEdGraphPin* Pin : EntryNode->Pins)
        {
            if (Pin && Pin->Direction == EGPD_Output
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                && Pin->PinName == TEXT("Released")
                && Pin->LinkedTo.Num() > 0)
            {
                bIsReleased = true;
                break;
            }
        }

        FString Kind = bIsReleased ? TEXT("key_released") : TEXT("key_pressed");
        return AppendEntryPosition(FString::Printf(TEXT("entry %s %s()"), *Kind, *KeyName));
    }

    if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(EntryNode))
    {
        // Entry tunnel in macro graph — caller-supplied source graph name (the
        // tunnel may live on a composite-flattening clone with a suffixed name).
        FString GraphName = FIrTextUtils::FormatNameToken(SourceGraphName);

        // Inputs: OUTPUT pins on entry tunnel (excluding exec/hidden)
        TArray<FString> InputList;
        for (UEdGraphPin* Pin : EntryNode->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Output
                || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                continue;
            }

            UEdGraphPin* RootPin = BpirTextEmitterInternal::GetSplitPinRoot(Pin);
            if (RootPin != Pin
                || (Pin->bHidden
                    && !BpirTextEmitterInternal::HasSplitChildren(EntryNode, Pin)))
            {
                continue;
            }

            FString TypeStr = PinTypeToBpirType(Pin->PinType);
            FString ParamName = FIrTextUtils::FormatNameToken(Pin->PinName.ToString());
            InputList.Add(FString::Printf(TEXT("%s %s"), *TypeStr, *ParamName));
        }

        // Find exit tunnel and collect outputs + exec paths
        TArray<FString> OutputList;
        TArray<FString> ExecPathList;
        if (UEdGraph* Graph = EntryNode->GetGraph())
        {
            for (UEdGraphNode* N : Graph->Nodes)
            {
                UK2Node_Tunnel* ExitTunnel = Cast<UK2Node_Tunnel>(N);
                if (ExitTunnel && ExitTunnel != Tunnel
                    && ExitTunnel->bCanHaveInputs && !ExitTunnel->bCanHaveOutputs)
                {
                    // Collect data output pins
                    for (UEdGraphPin* Pin : ExitTunnel->Pins)
                    {
                        if (!Pin || Pin->Direction != EGPD_Input
                            || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
                        {
                            continue;
                        }

                        UEdGraphPin* RootPin = BpirTextEmitterInternal::GetSplitPinRoot(Pin);
                        if (RootPin != Pin
                            || (Pin->bHidden
                                && !BpirTextEmitterInternal::HasSplitChildren(ExitTunnel, Pin)))
                        {
                            continue;
                        }

                        FString TypeStr = PinTypeToBpirType(Pin->PinType);
                        FString ParamName = FIrTextUtils::FormatNameToken(Pin->PinName.ToString());
                        OutputList.Add(FString::Printf(TEXT("%s %s"), *TypeStr, *ParamName));
                    }
                    // Collect exec input pins (for multi-exit macros)
                    for (UEdGraphPin* Pin : ExitTunnel->Pins)
                    {
                        if (Pin && Pin->Direction == EGPD_Input
                            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                            && !Pin->bHidden)
                        {
                            ExecPathList.Add(FIrTextUtils::FormatNameToken(Pin->PinName.ToString()));
                        }
                    }
                    break;
                }
            }
        }

        FString InputStr = FString::Join(InputList, TEXT(", "));
        FString Result = FString::Printf(TEXT("entry macro %s(%s)"), *GraphName, *InputStr);

        if (OutputList.Num() > 0)
        {
            FString OutputStr = FString::Join(OutputList, TEXT(", "));
            Result += FString::Printf(TEXT(" -> (%s)"), *OutputStr);
        }

        // Multi-exit exec paths (more than 1 exec pin on exit tunnel)
        if (ExecPathList.Num() > 1)
        {
            TArray<FString> ExecTargets;
            for (const FString& Path : ExecPathList)
            {
                ExecTargets.Add(FString::Printf(TEXT("%s -> @%s"), *Path, *Path.ToLower()));
            }
            Result += FString::Printf(TEXT(" [%s]"), *FString::Join(ExecTargets, TEXT(", ")));
        }

        return AppendEntryPosition(Result);
    }

    return UnknownEntrySignature;
}

// ---------------------------------------------------------------------------
// Call node (impure function call)
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitCallNode(
    UEdGraphNode* Node,
    const FString& ResultName,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
    if (!CallNode)
    {
        return FString::Printf(TEXT("# [ERROR] Expected call node: %s"), *Node->GetClass()->GetName());
    }

    const FString RawFuncName = GetFunctionDisplayName(CallNode);
    // Qualified `Class::Method` output already formats each half — outer wrap
    // would backtick the whole token and confuse the parser's split.
    FString FuncName = RawFuncName.Contains(BpirSharedConstants::Syntax::QualifiedNameSeparator)
        ? RawFuncName
        : FIrTextUtils::FormatNameToken(RawFuncName);
    // UK2Node_Message derives from UK2Node_CallFunction, so without this the interface
    // message node emits as a plain `call` and recompiles into a plain UK2Node_CallFunction —
    // a silent node-class change whose only visible difference is at runtime (a message
    // no-ops on a non-implementing object; a plain call through a null interface does not).
    const TCHAR* CallKeyword = Node->IsA<UK2Node_CallParentFunction>()
        ? BpirSharedConstants::Keywords::ParentCall
        : (Node->IsA<UK2Node_Message>()
            ? BpirSharedConstants::Keywords::Message
            : BpirSharedConstants::Keywords::Call);
    TArray<FString> TargetArgs = FormatTargetPrefix(Node, ResolvePin);
    FString GeneratedPrefix;
    FString Args = FormatArgs(Node, ResolvePin, true, nullptr, &GeneratedPrefix);

    // Multi-self fan-out is only valid when the node has no consumed result
    // (CanFunctionSupportMultipleTargets requires no return param).
    if (TargetArgs.Num() > 1 && ResultName.IsEmpty())
    {
        TArray<FString> Lines;
        Lines.Reserve(TargetArgs.Num());
        for (const FString& TargetArg : TargetArgs)
        {
            FString AllArgs;
            if (!Args.IsEmpty())
            {
                AllArgs = TargetArg + TEXT(", ") + Args;
            }
            else
            {
                AllArgs = TargetArg;
            }
            Lines.Add(FString::Printf(TEXT("%s %s(%s)"), CallKeyword, *FuncName, *AllArgs));
        }
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            GeneratedPrefix,
            FString::Join(Lines, TEXT("\n")));
    }

    // Combine target arg with other args
    const FString TargetArg = TargetArgs.Num() > 0 ? TargetArgs[0] : FString();
    FString AllArgs;
    if (!TargetArg.IsEmpty() && !Args.IsEmpty())
    {
        AllArgs = TargetArg + TEXT(", ") + Args;
    }
    else if (!TargetArg.IsEmpty())
    {
        AllArgs = TargetArg;
    }
    else
    {
        AllArgs = Args;
    }

    if (ResultName.IsEmpty())
    {
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            GeneratedPrefix,
            FString::Printf(TEXT("%s %s(%s)"), CallKeyword, *FuncName, *AllArgs));
    }
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return BpirTextEmitterInternal::AddGeneratedPrefix(
        GeneratedPrefix,
        FString::Printf(TEXT("%%%s%s = %s %s(%s)"), *ResultName, *TypeAnno, CallKeyword, *FuncName, *AllArgs));
}

// ---------------------------------------------------------------------------
// Dispatcher (event dispatcher call/bind/unbind/clear)
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitDispatcherNode(
    UEdGraphNode* Node,
    const FString& ValueName,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
#if MCP_HAS_DELEGATE_NODES
    // Determine keyword and dispatcher/field name based on node type.
    // UK2Node_CallFunction targeting K2_Add/RemoveFieldValueChangedDelegate is a
    // field-notify bind; all other delegate node types map to dispatcher keywords.
    bool bIsFieldNotify = false;
    FString Keyword;
    FString DispatcherName;

    if (UK2Node_CallFunction* CallFn = Cast<UK2Node_CallFunction>(Node))
    {
        const UFunction* Func = CallFn->GetTargetFunction();
        const FName FuncName = Func ? Func->GetFName() : NAME_None;
        if (FuncName == FName(BpirSharedConstants::FieldNotify::SubscribeFnName))
        {
            bIsFieldNotify = true;
            Keyword = TEXT("field_notify_subscribe");
        }
        else if (FuncName == FName(BpirSharedConstants::FieldNotify::UnsubscribeFnName))
        {
            bIsFieldNotify = true;
            Keyword = TEXT("field_notify_unsubscribe");
        }
        else
        {
            UE_LOG(LogBpirTextEmitter, Warning,
                TEXT("EmitDispatcherNode: UK2Node_CallFunction with unexpected function '%s' routed to dispatcher emitter"),
                *FuncName.ToString());
        }

        // Extract FieldName from the FieldId pin's struct-literal default: (FieldName="X")
        UEdGraphPin* FieldIdPin = CallFn->FindPin(TEXT("FieldId"));
        if (FieldIdPin && !FieldIdPin->DefaultValue.IsEmpty())
        {
            const FString& Def = FieldIdPin->DefaultValue; // e.g. (FieldName="Replay")
            int32 QuoteOpen = INDEX_NONE;
            Def.FindChar(TEXT('"'), QuoteOpen);
            if (QuoteOpen != INDEX_NONE)
            {
                int32 QuoteClose = Def.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, QuoteOpen + 1);
                if (QuoteClose != INDEX_NONE)
                {
                    DispatcherName = Def.Mid(QuoteOpen + 1, QuoteClose - QuoteOpen - 1);
                }
            }
        }
        if (DispatcherName.IsEmpty())
        {
            DispatcherName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        }
    }
    else if (Node->IsA<UK2Node_CallDelegate>())
    {
        Keyword = TEXT("call_dispatcher");
    }
    else if (Node->IsA<UK2Node_AddDelegate>())
    {
        Keyword = TEXT("bind_dispatcher");
    }
    else if (Node->IsA<UK2Node_RemoveDelegate>())
    {
        Keyword = TEXT("unbind_dispatcher");
    }
    else if (Node->IsA<UK2Node_ClearDelegate>())
    {
        Keyword = TEXT("clear_dispatcher");
    }
    else
    {
        Keyword = TEXT("unknown_dispatcher");
        UE_LOG(LogBpirTextEmitter, Warning, TEXT("Unrecognized delegate node type: %s"), *Node->GetClass()->GetName());
    }

    // For non-field-notify nodes, get dispatcher name from the delegate property
    if (DispatcherName.IsEmpty())
    {
#if __has_include("K2Node_BaseMCDelegate.h")
        if (UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node))
        {
            DispatcherName = DelegateNode->GetPropertyName().ToString();
        }
#endif
        if (DispatcherName.IsEmpty())
        {
            DispatcherName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
        }
    }

    TArray<FString> TargetArgs = FormatTargetPrefix(Node, ResolvePin);

    // Recover the handler reference from the Delegate input pin before calling
    // FormatArgs so we can suppress the redundant "Delegate: $OutputDelegate" arg.
    // Supports both UK2Node_CreateDelegate and direct UK2Node_CustomEvent.OutputDelegate
    // links (including reroute hops).
    FString EventArg;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input)
        {
            continue;
        }
        const FName& Cat = Pin->PinType.PinCategory;
        if (Cat != UEdGraphSchema_K2::PC_Delegate && Cat != UEdGraphSchema_K2::PC_MCDelegate)
        {
            continue;
        }
        for (UEdGraphPin* Linked : Pin->LinkedTo)
        {
            if (!Linked || !Linked->GetOwningNode())
            {
                continue;
            }
            UEdGraphPin* DeadEnd = nullptr;
            UEdGraphPin* Source = BpirDecompiler::Helpers::FollowKnotsBackward(Linked, DeadEnd);
            UEdGraphNode* SourceNode = Source ? Source->GetOwningNode() : nullptr;
#if MCP_HAS_CREATE_DELEGATE
            if (UK2Node_CreateDelegate* CDNode = Cast<UK2Node_CreateDelegate>(SourceNode))
            {
                const FName FnName = CDNode->GetFunctionName();
                if (!FnName.IsNone())
                {
                    EventArg = FString::Printf(TEXT("event: @%s"), *FnName.ToString());
                    break;
                }
            }
            else
#endif
            if (UK2Node_CustomEvent* CENode = Cast<UK2Node_CustomEvent>(SourceNode))
            {
                const FName FnName = CENode->GetFunctionName();
                if (!FnName.IsNone())
                {
                    EventArg = FString::Printf(TEXT("event: @%s"), *FnName.ToString());
                    break;
                }
            }
        }
        if (!EventArg.IsEmpty())
        {
            break;
        }
    }

    // Field-notify nodes express all inputs via DispatcherName (FieldId) and EventArg (Delegate);
    // FormatArgs would produce spurious FieldId/Delegate entries, so skip it for those nodes.
    // When EventArg was recovered, skip the Delegate pin so FormatArgs doesn't also emit it.
    TSet<FName> DelegatePinSkip;
    if (!EventArg.IsEmpty())
    {
        DelegatePinSkip.Add(TEXT("Delegate"));
    }
    FString GeneratedPrefix;
    FString Args = bIsFieldNotify
        ? FString()
        : FormatArgs(Node, ResolvePin, /*bSkipSelfPin=*/true,
                     EventArg.IsEmpty() ? nullptr : &DelegatePinSkip,
                     &GeneratedPrefix);

    auto AssembleAllArgs = [&Args, &EventArg](const FString& TargetArg) -> FString
    {
        FString Result;
        if (!TargetArg.IsEmpty() && !Args.IsEmpty())
            Result = TargetArg + TEXT(", ") + Args;
        else if (!TargetArg.IsEmpty())
            Result = TargetArg;
        else
            Result = Args;

        if (!EventArg.IsEmpty())
        {
            if (!Result.IsEmpty())
            {
                Result += TEXT(", ");
            }
            Result += EventArg;
        }
        return Result;
    };

    // Multi-self fan-out for dispatcher nodes: emit one sibling statement per
    // linked source. UE's dispatcher expansion (UK2Node_BaseMCDelegate::ExpandNode)
    // produces N independent dispatcher invocations at compile time; preserve
    // that on decompile. Only applies when there is no value-bind site, which
    // is the only configuration multi-self can take.
    if (TargetArgs.Num() > 1 && ValueName.IsEmpty())
    {
        TArray<FString> Lines;
        Lines.Reserve(TargetArgs.Num());
        for (const FString& TargetArg : TargetArgs)
        {
            const FString AllArgs = AssembleAllArgs(TargetArg);
            Lines.Add(FString::Printf(TEXT("%s %s(%s)"), *Keyword, *DispatcherName, *AllArgs));
        }
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            GeneratedPrefix,
            FString::Join(Lines, TEXT("\n")));
    }

    const FString TargetArg = TargetArgs.Num() > 0 ? TargetArgs[0] : FString();
    const FString AllArgs = AssembleAllArgs(TargetArg);

    if (!ValueName.IsEmpty())
    {
        const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            GeneratedPrefix,
            FString::Printf(TEXT("%%%s%s = %s %s(%s)"), *ValueName, *TypeAnno, *Keyword, *DispatcherName, *AllArgs));
    }
    return BpirTextEmitterInternal::AddGeneratedPrefix(
        GeneratedPrefix,
        FString::Printf(TEXT("%s %s(%s)"), *Keyword, *DispatcherName, *AllArgs));
#else
    return FString::Printf(TEXT("# [UNSUPPORTED] Dispatcher node (delegate headers not available)"));
#endif
}

// ---------------------------------------------------------------------------
// Branch
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitBranch(
    UEdGraphNode* Node,
    const FString& ResultName,
    const FBpirLabelMap& LabelMap,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    UK2Node_IfThenElse* BranchNode = Cast<UK2Node_IfThenElse>(Node);
    FString CondRef;
    if (BranchNode)
    {
        UEdGraphPin* CondPin = BranchNode->GetConditionPin();
        CondRef = CondPin ? ResolvePin(CondPin) : TEXT("?");
    }
    else
    {
        CondRef = TEXT("?");
    }

    FString ExecTargets = FormatExecTargets(LabelMap);
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return FString::Printf(TEXT("%%%s%s = branch(%s)%s"), *ResultName, *TypeAnno, *CondRef, *ExecTargets);
}

// ---------------------------------------------------------------------------
// Switch
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitSwitch(
    UEdGraphNode* Node,
    const FString& ResultName,
    const FBpirLabelMap& LabelMap,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    // Find the selection input pin (first non-exec input)
    FString SelRef = TEXT("?");
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Input
            && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
            && !Pin->bHidden
            && Pin->ParentPin == nullptr)
        {
            SelRef = ResolvePin(Pin);
            break;
        }
    }

    // Determine switch keyword based on concrete node class
    FString Keyword = TEXT("switch");
#if MCP_HAS_SWITCH_INTEGER
    if (Node->IsA<UK2Node_SwitchInteger>())
    {
        Keyword = TEXT("switch_int");
    }
#endif
#if MCP_HAS_SWITCH_STRING
    if (Node->IsA<UK2Node_SwitchString>())
    {
        Keyword = TEXT("switch_string");
    }
#endif
#if MCP_HAS_SWITCH_ENUM
    if (Node->IsA<UK2Node_SwitchEnum>())
    {
        UK2Node_SwitchEnum* EnumSwitch = Cast<UK2Node_SwitchEnum>(Node);
        FString EnumName = EnumSwitch && EnumSwitch->Enum
            ? EnumSwitch->Enum->GetName()
            : TEXT("Unknown");
        if (!EnumName.IsEmpty())
        {
            SelRef = FormatEnumLiteralForBpir(SelRef, EnumName);
        }
        FString ExecTargets = FormatEnumExecTargets(LabelMap, EnumSwitch ? EnumSwitch->Enum : nullptr);
        const FString EnumTypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
        return FString::Printf(TEXT("%%%s%s = switch_enum<%s>(%s)%s"), *ResultName, *EnumTypeAnno, *EnumName, *SelRef, *ExecTargets);
    }
#endif

    FString ExecTargets = FormatExecTargets(LabelMap);
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return FString::Printf(TEXT("%%%s%s = %s(%s)%s"), *ResultName, *TypeAnno, *Keyword, *SelRef, *ExecTargets);
}

// ---------------------------------------------------------------------------
// Loop (ForEach, ForEachBreak, While)
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitLoop(
    UEdGraphNode* Node,
    const FString& ResultName,
    const FBpirLabelMap& LabelMap,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node);
    if (!MacroNode)
    {
        return FString::Printf(TEXT("# [ERROR] Expected macro node for loop"));
    }

    FString MacroName;
    if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
    {
        MacroName = MacroGraph->GetName();
    }

    FString ExecTargets = FormatExecTargets(LabelMap);

    if (MacroName == BpirSharedConstants::MacroNames::While)
    {
        UEdGraphPin* CondPin = MacroNode->FindPin(TEXT("Condition"));
        FString CondRef = CondPin ? ResolvePin(CondPin) : TEXT("?");
        const FString WhileTypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
        return FString::Printf(TEXT("%%%s%s = while(%s)%s"), *ResultName, *WhileTypeAnno, *CondRef, *ExecTargets);
    }

    // ForEach variants
    UEdGraphPin* ArrayPin = MacroNode->FindPin(TEXT("Array"));
    FString ArrayRef = ArrayPin ? ResolvePin(ArrayPin) : TEXT("?");

    FString Keyword = TEXT("foreach");
    if (MacroName == BpirSharedConstants::MacroNames::ForEachLoopWithBreak)
    {
        Keyword = TEXT("foreach_break");
    }

    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return FString::Printf(TEXT("%%%s%s = %s(%s)%s"), *ResultName, *TypeAnno, *Keyword, *ArrayRef, *ExecTargets);
}

// ---------------------------------------------------------------------------
// Sequence
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitSequence(
    UEdGraphNode* Node,
    const FString& ResultName,
    const FBpirLabelMap& LabelMap)
{
    int32 Count = 0;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->Direction == EGPD_Output
            && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            ++Count;
        }
    }

    FString ExecTargets = FormatExecTargets(LabelMap);
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return FString::Printf(TEXT("%%%s%s = sequence(%d)%s"), *ResultName, *TypeAnno, Count, *ExecTargets);
}

// ---------------------------------------------------------------------------
// Variable set
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitVariableSet(
    UEdGraphNode* Node,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    LastGeneratedPrefixLineCount = 0;
    UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node);
    FString VarName = GetVariableName(Node);

    // Check for external variable set (non-self target)
    FString TargetPrefix;
    if (SetNode && !SetNode->VariableReference.IsSelfContext())
    {
        UEdGraphPin* SelfPin = SetNode->FindPin(UEdGraphSchema_K2::PN_Self);
        if (SelfPin && SelfPin->LinkedTo.Num() > 0)
        {
            FString TargetRef = ResolvePin(SelfPin);
            TargetPrefix = TargetRef + TEXT(".");
        }
    }

    TArray<FString> GeneratedLines;
    TSet<UEdGraphPin*> ProcessedSplitRoots;
    int32 NextSplitValueIndex = 0;

    // Find the value input pin
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input
            || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
            || Pin->PinName == UEdGraphSchema_K2::PN_Self)
        {
            continue;
        }

        if (Pin->ParentPin)
        {
            continue;
        }

        if (BpirTextEmitterInternal::HasSplitInputChildren(Node, Pin))
        {
            FString ValueRef;
            if (BpirTextEmitterInternal::TryBuildSplitInputValue(
                    Node,
                    Pin,
                    ResolvePin,
                    ProcessedSplitRoots,
                    NextSplitValueIndex,
                    GeneratedLines,
                    ValueRef))
            {
                LastGeneratedPrefixLineCount = GeneratedLines.Num();
                return BpirTextEmitterInternal::AddGeneratedPrefix(
                    FString::Join(GeneratedLines, TEXT("\n")),
                    FString::Printf(TEXT("set %s%s = %s"), *TargetPrefix, *VarName, *ValueRef));
            }
            LastGeneratedPrefixLineCount = GeneratedLines.Num();
            return BpirTextEmitterInternal::AddGeneratedPrefix(
                FString::Join(GeneratedLines, TEXT("\n")),
                FString::Printf(TEXT("set %s%s = %s"), *TargetPrefix, *VarName,
                    BpirTextEmitterInternal::SplitInputUnresolvedValue));
        }

        if (Pin->bHidden)
        {
            continue;
        }

        FString ValueRef = ResolvePin(Pin);
        return FString::Printf(TEXT("set %s%s = %s"), *TargetPrefix, *VarName, *ValueRef);
    }

    // Defense-in-depth: a UK2Node_VariableSet should always expose its value
    // input pin, so the loop above returns. If we reach here, no value pin
    // exists at all — there's no Pin to dispatch FormatPinDefaultLiteral on,
    // so emit the same <unresolved> sentinel the helper uses for its hard-fail
    // path. Keeps the IR free of magic `?` tokens.
    return FString::Printf(TEXT("set %s%s = <unresolved>"), *TargetPrefix, *VarName);
}

// ---------------------------------------------------------------------------
// Variable get
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitVariableGet(
    UEdGraphNode* Node,
    const FString& ResultName,
    const FBpirLabelMap& LabelMap)
{
    FString VarName = GetVariableName(Node);
    FString ExecTargets = FormatExecTargets(LabelMap);
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return FString::Printf(TEXT("%%%s%s = get %s%s"), *ResultName, *TypeAnno, *VarName, *ExecTargets);
}

// ---------------------------------------------------------------------------
// Macro (DoOnce, FlipFlop, Gate, MultiGate)
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitMacro(
    UEdGraphNode* Node,
    const FString& ResultName,
    const FBpirLabelMap& LabelMap,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node);
    if (!MacroNode)
    {
        return FString::Printf(TEXT("# [ERROR] Expected macro node"));
    }

    FString MacroName;
    if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
    {
        MacroName = MacroGraph->GetName();
    }

    FString GeneratedPrefix;
    FString Args = FormatArgs(Node, ResolvePin, true, nullptr, &GeneratedPrefix);
    FString ExecTargets = FormatExecTargets(LabelMap);
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);

    return BpirTextEmitterInternal::AddGeneratedPrefix(
        GeneratedPrefix,
        FString::Printf(TEXT("%%%s%s = macro %s(%s)%s"), *ResultName, *TypeAnno, *MacroName, *Args, *ExecTargets));
}

// ---------------------------------------------------------------------------
// Latent
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitLatent(
    UEdGraphNode* Node,
    const FString& ResultName,
    const FBpirLabelMap& LabelMap,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
    if (!CallNode)
    {
        return FString::Printf(TEXT("# [ERROR] Expected call node for latent"));
    }

    const FString RawLatentFuncName = GetFunctionDisplayName(CallNode);
    FString FuncName = RawLatentFuncName.Contains(BpirSharedConstants::Syntax::QualifiedNameSeparator)
        ? RawLatentFuncName
        : FIrTextUtils::FormatNameToken(RawLatentFuncName);
    // Latent calls cannot fan out (UE's CanFunctionSupportMultipleTargets bars latents);
    // FormatTargetPrefix will return at most one entry here.
    TArray<FString> TargetArgs = FormatTargetPrefix(Node, ResolvePin);
    const FString TargetArg = TargetArgs.Num() > 0 ? TargetArgs[0] : FString();
    FString GeneratedPrefix;
    FString Args = FormatArgs(Node, ResolvePin, true, nullptr, &GeneratedPrefix);
    FString AllArgs;
    if (!TargetArg.IsEmpty() && !Args.IsEmpty())
        AllArgs = TargetArg + TEXT(", ") + Args;
    else if (!TargetArg.IsEmpty())
        AllArgs = TargetArg;
    else
        AllArgs = Args;
    FString ExecTargets = FormatExecTargets(LabelMap);

    if (ResultName.IsEmpty())
    {
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            GeneratedPrefix,
            FString::Printf(TEXT("latent %s(%s)%s"), *FuncName, *AllArgs, *ExecTargets));
    }
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return BpirTextEmitterInternal::AddGeneratedPrefix(
        GeneratedPrefix,
        FString::Printf(TEXT("%%%s%s = latent %s(%s)%s"), *ResultName, *TypeAnno, *FuncName, *AllArgs, *ExecTargets));
}

// ---------------------------------------------------------------------------
// Cast
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitCast(
    UEdGraphNode* Node,
    const FString& ResultName,
    const FBpirLabelMap& LabelMap,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node);
    if (!CastNode)
    {
        return FString::Printf(TEXT("# [ERROR] Expected cast node"));
    }

    FString TargetTypeName = CastNode->TargetType ? CastNode->TargetType->GetName() : TEXT("Unknown");

    UEdGraphPin* ObjectPin = CastNode->GetCastSourcePin();
    FString ObjRef = ObjectPin ? ResolvePin(ObjectPin) : TEXT("?");

    FString ExecTargets = FormatExecTargets(LabelMap);
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return FString::Printf(TEXT("%%%s%s = cast<%s>(%s)%s"), *ResultName, *TypeAnno, *TargetTypeName, *ObjRef, *ExecTargets);
}

// ---------------------------------------------------------------------------
// Return
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitReturn(
    UEdGraphNode* Node,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    LastGeneratedPrefixLineCount = 0;
    if (!Node)
    {
        return TEXT("return");
    }

    // Collect all non-exec input data pins as return values.
    // DataPinCount (not ReturnParts.Num()) picks the emitted form: a result node with two
    // outputs where only one is wired must still name the pin it wired, because the
    // positional form always lands on the node's FIRST data pin.
    int32 DataPinCount = 0;
    TArray<TPair<FString, FString>> ReturnParts; // {PinName, Value}
    TArray<FString> GeneratedLines;
    TSet<UEdGraphPin*> ProcessedSplitRoots;
    int32 NextSplitValueIndex = 0;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input
            || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
        {
            continue;
        }

        if (Pin->ParentPin)
        {
            continue;
        }

        FString Value;
        if (BpirTextEmitterInternal::HasSplitInputChildren(Node, Pin))
        {
            if (!BpirTextEmitterInternal::TryBuildSplitInputValue(
                    Node,
                    Pin,
                    ResolvePin,
                    ProcessedSplitRoots,
                    NextSplitValueIndex,
                    GeneratedLines,
                    Value))
            {
                Value = BpirTextEmitterInternal::SplitInputUnresolvedValue;
            }
        }
        else
        {
            if (Pin->bHidden)
            {
                continue;
            }
            Value = ResolvePin(Pin);
        }

        ++DataPinCount;
        if (Value != TEXT("?") && !Value.IsEmpty())
        {
            ReturnParts.Add({FIrTextUtils::FormatNameToken(Pin->PinName.ToString()), Value});
        }
    }

    if (ReturnParts.Num() == 0)
    {
        return TEXT("return");
    }
    if (DataPinCount == 1)
    {
        LastGeneratedPrefixLineCount = GeneratedLines.Num();
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            FString::Join(GeneratedLines, TEXT("\n")),
            FString::Printf(TEXT("return %s"), *ReturnParts[0].Value));
    }
    // Multi-output function result. `return %a, %b` does NOT parse — the parser reads the
    // whole comma list as one value reference — so emit the named form, the same grammar the
    // multi-output macro return uses and the one ResolveTargetPin_Return wires by pin name.
    TArray<FString> NamedParts;
    NamedParts.Reserve(ReturnParts.Num());
    for (const TPair<FString, FString>& Part : ReturnParts)
    {
        NamedParts.Add(FString::Printf(TEXT("%s: %s"), *Part.Key, *Part.Value));
    }
    LastGeneratedPrefixLineCount = GeneratedLines.Num();
    return BpirTextEmitterInternal::AddGeneratedPrefix(
        FString::Join(GeneratedLines, TEXT("\n")),
        FString::Printf(TEXT("return (%s)"), *FString::Join(NamedParts, TEXT(", "))));
}

// ---------------------------------------------------------------------------
// Macro return (exit tunnel)
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitMacroReturn(
    UEdGraphNode* Node,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin,
    const FString& ExitPinName)
{
    LastGeneratedPrefixLineCount = 0;
    if (!Node) return TEXT("return");

    TArray<FString> OutputParts;
    TArray<FString> GeneratedLines;
    TSet<UEdGraphPin*> ProcessedSplitRoots;
    int32 NextSplitValueIndex = 0;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input
            || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
            || Pin->ParentPin)
        {
            continue;
        }

        FString Value;
        if (BpirTextEmitterInternal::HasSplitInputChildren(Node, Pin))
        {
            if (!BpirTextEmitterInternal::TryBuildSplitInputValue(
                    Node,
                    Pin,
                    ResolvePin,
                    ProcessedSplitRoots,
                    NextSplitValueIndex,
                    GeneratedLines,
                    Value))
            {
                Value = BpirTextEmitterInternal::SplitInputUnresolvedValue;
            }
        }
        else
        {
            if (Pin->bHidden)
            {
                continue;
            }
            Value = ResolvePin(Pin);
        }

        const FString PinName = FIrTextUtils::FormatNameToken(Pin->PinName.ToString());
        if (!Value.IsEmpty() && Value != TEXT("?"))
        {
            OutputParts.Add(FString::Printf(TEXT("%s: %s"), *PinName, *Value));
        }
    }

    // Build optional exit pin bracket for multi-exit macros: [PinName]
    FString ExitPart;
    if (!ExitPinName.IsEmpty())
    {
        FString CleanName = FIrTextUtils::FormatNameToken(ExitPinName);
        ExitPart = FString::Printf(TEXT(" [%s]"), *CleanName);
    }

    if (OutputParts.Num() == 0)
    {
        LastGeneratedPrefixLineCount = GeneratedLines.Num();
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            FString::Join(GeneratedLines, TEXT("\n")),
            FString::Printf(TEXT("return%s"), *ExitPart));
    }
    LastGeneratedPrefixLineCount = GeneratedLines.Num();
    return BpirTextEmitterInternal::AddGeneratedPrefix(
        FString::Join(GeneratedLines, TEXT("\n")),
        FString::Printf(TEXT("return%s (%s)"), *ExitPart, *FString::Join(OutputParts, TEXT(", "))));
}

// ---------------------------------------------------------------------------
// Make struct
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitMakeStruct(
    UEdGraphNode* Node,
    const FString& ResultName,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    UK2Node_MakeStruct* MakeNode = Cast<UK2Node_MakeStruct>(Node);
    FString StructName = TEXT("Unknown");
    if (MakeNode && MakeNode->StructType)
    {
        StructName = MakeNode->StructType->GetName();
    }

    FString GeneratedPrefix;
    FString Args = FormatArgs(Node, ResolvePin, false, nullptr, &GeneratedPrefix);
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return BpirTextEmitterInternal::AddGeneratedPrefix(
        GeneratedPrefix,
        FString::Printf(TEXT("%%%s%s = make<%s>(%s)"), *ResultName, *TypeAnno, *StructName, *Args));
}

// ---------------------------------------------------------------------------
// Break struct
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitBreakStruct(
    UEdGraphNode* Node,
    const FString& ResultName,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    LastGeneratedPrefixLineCount = 0;
    UK2Node_BreakStruct* BreakNode = Cast<UK2Node_BreakStruct>(Node);
    FString StructName = TEXT("Unknown");
    if (BreakNode && BreakNode->StructType)
    {
        StructName = BreakNode->StructType->GetName();
    }

    // Find the struct input pin
    FString StructRef = TEXT("?");
    TArray<FString> GeneratedLines;
    TSet<UEdGraphPin*> ProcessedSplitRoots;
    int32 NextSplitValueIndex = 0;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input
            || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
            || Pin->ParentPin)
        {
            continue;
        }

        if (BpirTextEmitterInternal::HasSplitInputChildren(Node, Pin))
        {
            if (BpirTextEmitterInternal::TryBuildSplitInputValue(
                    Node,
                    Pin,
                    ResolvePin,
                    ProcessedSplitRoots,
                    NextSplitValueIndex,
                    GeneratedLines,
                    StructRef))
            {
                break;
            }
            StructRef = BpirTextEmitterInternal::SplitInputUnresolvedValue;
            break;
        }

        if (!Pin->bHidden)
        {
            StructRef = ResolvePin(Pin);
            break;
        }
    }

    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    LastGeneratedPrefixLineCount = GeneratedLines.Num();
    return BpirTextEmitterInternal::AddGeneratedPrefix(
        FString::Join(GeneratedLines, TEXT("\n")),
        FString::Printf(TEXT("%%%s%s = break<%s>(%s)"), *ResultName, *TypeAnno, *StructName, *StructRef));
}

// ---------------------------------------------------------------------------
// Timeline
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitTimeline(
    UEdGraphNode* Node,
    const FString& ResultName,
    const FBpirLabelMap& LabelMap)
{
    UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node);
    FString TimelineName = TEXT("Unknown");
    if (TimelineNode)
    {
        TimelineName = TimelineNode->TimelineName.ToString();
    }

    FString ExecTargets = FormatExecTargets(LabelMap);
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return FString::Printf(TEXT("%%%s%s = timeline %s()%s"), *ResultName, *TypeAnno, *TimelineName, *ExecTargets);
}

// ---------------------------------------------------------------------------
// Pure node
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitPureNode(
    UEdGraphNode* Node,
    const FString& ResultName,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    LastGeneratedPrefixLineCount = 0;
    // MakeArray is a pure node
#if MCP_HAS_MAKE_ARRAY
    if (Node->IsA<UK2Node_MakeArray>())
    {
        TArray<FString> Elements;
        TArray<FString> GeneratedLines;
        TSet<UEdGraphPin*> ProcessedSplitRoots;
        int32 NextSplitValueIndex = 0;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input
                || Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
                || Pin->ParentPin)
            {
                continue;
            }

            if (BpirTextEmitterInternal::HasSplitInputChildren(Node, Pin))
            {
                FString Value;
                if (BpirTextEmitterInternal::TryBuildSplitInputValue(
                        Node,
                        Pin,
                        ResolvePin,
                        ProcessedSplitRoots,
                        NextSplitValueIndex,
                        GeneratedLines,
                        Value))
                {
                    Elements.Add(Value);
                }
                else
                {
                    Elements.Add(BpirTextEmitterInternal::SplitInputUnresolvedValue);
                }
                continue;
            }

            if (!Pin->bHidden)
            {
                Elements.Add(ResolvePin(Pin));
            }
        }
        const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
        LastGeneratedPrefixLineCount = GeneratedLines.Num();
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            FString::Join(GeneratedLines, TEXT("\n")),
            FString::Printf(TEXT("%%%s%s = make_array(%s)"), *ResultName, *TypeAnno, *FString::Join(Elements, TEXT(", "))));
    }
#endif

    // Select is a pure node
#if MCP_HAS_SELECT
    if (UK2Node_Select* SelectNode = Cast<UK2Node_Select>(Node))
    {
        const bool bEnumBackedSelect = SelectNode->GetEnum() != nullptr;
        TArray<FString> ArgParts;
        TArray<FString> GeneratedLines;
        TSet<UEdGraphPin*> ProcessedSplitRoots;
        int32 NextSplitValueIndex = 0;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input || Pin->ParentPin) continue;
            if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
            if (Pin->PinName == UEdGraphSchema_K2::PN_Self) continue;

            FString Value;
            if (BpirTextEmitterInternal::HasSplitInputChildren(Node, Pin))
            {
                if (!BpirTextEmitterInternal::TryBuildSplitInputValue(
                        Node,
                        Pin,
                        ResolvePin,
                        ProcessedSplitRoots,
                        NextSplitValueIndex,
                        GeneratedLines,
                        Value))
                {
                    Value = BpirTextEmitterInternal::SplitInputUnresolvedValue;
                }
            }
            else
            {
                if (Pin->bHidden)
                {
                    continue;
                }
                Value = ResolvePin(Pin);
            }

            FString PinName = Pin->PinName.ToString();

            // Map Select pin names to BPIR keywords.
            // UE's UK2Node_Select::AllocateDefaultPins assigns Option 0 the
            // friendly name "False" (Idx == 0 ? CoreTexts.False : CoreTexts.True),
            // so Option 0 is the false branch and Option 1 is the true branch.
            if (PinName == TEXT("Index"))
            {
                ArgParts.Insert(FString::Printf(TEXT("Index: %s"), *Value), 0);
            }
            else if (!bEnumBackedSelect && PinName == TEXT("Option 0"))
            {
                ArgParts.Add(FString::Printf(TEXT("false: %s"), *Value));
            }
            else if (!bEnumBackedSelect && PinName == TEXT("Option 1"))
            {
                ArgParts.Add(FString::Printf(TEXT("true: %s"), *Value));
            }
            else
            {
                ArgParts.Add(FString::Printf(TEXT("%s: %s"), *FIrTextUtils::FormatNameToken(PinName), *Value));
            }
        }
        const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
        LastGeneratedPrefixLineCount = GeneratedLines.Num();
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            FString::Join(GeneratedLines, TEXT("\n")),
            FString::Printf(TEXT("%%%s%s = select(%s)"), *ResultName, *TypeAnno, *FString::Join(ArgParts, TEXT(", "))));
    }
#endif

    // MakeStruct is a pure node
    if (Node->IsA<UK2Node_MakeStruct>())
    {
        return EmitMakeStruct(Node, ResultName, ResolvePin);
    }

    // MakeMap is a pure node
#if MCP_HAS_MAKE_MAP
    if (Node->IsA<UK2Node_MakeMap>())
    {
        FString GeneratedPrefix;
        FString Args = FormatArgs(Node, ResolvePin, false, nullptr, &GeneratedPrefix);
        const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            GeneratedPrefix,
            FString::Printf(TEXT("%%%s%s = make_map(%s)"), *ResultName, *TypeAnno, *Args));
    }
#endif

    // MakeSet is a pure node
#if MCP_HAS_MAKE_SET
    if (Node->IsA<UK2Node_MakeSet>())
    {
        FString GeneratedPrefix;
        FString Args = FormatArgs(Node, ResolvePin, false, nullptr, &GeneratedPrefix);
        const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            GeneratedPrefix,
            FString::Printf(TEXT("%%%s%s = make_set(%s)"), *ResultName, *TypeAnno, *Args));
    }
#endif

    // BreakStruct is a pure node
    if (Node->IsA<UK2Node_BreakStruct>())
    {
        return EmitBreakStruct(Node, ResultName, ResolvePin);
    }

#if MCP_HAS_ENUM_LITERAL
    if (UK2Node_EnumLiteral* EnumLiteral = Cast<UK2Node_EnumLiteral>(Node))
    {
        UEnum* EnumType = EnumLiteral->Enum;
        UEdGraphPin* ValuePin = EnumLiteral->FindPin(UK2Node_EnumLiteral::GetEnumInputPinName());
        FString ValueName = ValuePin ? ValuePin->DefaultValue : FString();

        if (EnumType && ValueName.IsNumeric())
        {
            const FString FullName = EnumType->GetNameStringByValue(FCString::Atoi64(*ValueName));
            if (!FullName.IsEmpty())
            {
                ValueName = FullName;
            }
        }

        int32 ColonIdx = INDEX_NONE;
        if (ValueName.FindLastChar(TEXT(':'), ColonIdx) && ColonIdx + 1 < ValueName.Len())
        {
            ValueName = ValueName.Mid(ColonIdx + 1);
        }
        while (ValueName.StartsWith(TEXT(":")))
        {
            ValueName.RemoveFromStart(TEXT(":"));
        }

        const FString EnumName = EnumType ? EnumType->GetName() : TEXT("Unknown");
        const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
        return FString::Printf(TEXT("%%%s%s = enum %s::%s"), *ResultName, *TypeAnno, *EnumName, *ValueName);
    }
#endif

    // VariableGet — usually emitted inline as $VarName, but can be explicit.
    // Always pure here (EmitPureNode path), so pass an empty LabelMap.
    if (Node->IsA<UK2Node_VariableGet>())
    {
        return EmitVariableGet(Node, ResultName, FBpirLabelMap{});
    }

#if MCP_HAS_CREATE_DELEGATE
    // Unwired Self pin: engine implicitly binds to the BP being compiled, so emit `self`, not `?`.
    if (UK2Node_CreateDelegate* CDNode = Cast<UK2Node_CreateDelegate>(Node))
    {
        UEdGraphPin* SelfPin = Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
        FString SelfToken;
        if (SelfPin && SelfPin->LinkedTo.Num() > 0)
        {
            SelfToken = ResolvePin(SelfPin);
        }
        else
        {
            SelfToken = TEXT("self");
        }

        const FName FnName = CDNode->GetFunctionName();
        const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
        if (FnName.IsNone())
        {
            return FString::Printf(TEXT("%%%s%s = call Create_Event(self: %s)"), *ResultName, *TypeAnno, *SelfToken);
        }
        return FString::Printf(TEXT("%%%s%s = call Create_Event(self: %s, event: @%s)"),
            *ResultName, *TypeAnno, *SelfToken, *FnName.ToString());
    }
#endif

    // Subsystem getter — all four UK2Node_GetSubsystem subclasses
    // (GetSubsystem, GetSubsystemFromPC, GetEngineSubsystem, GetEditorSubsystem)
    // share the base, so a single base cast catches every variant. The configured
    // subsystem class is exposed through the result pin's PinSubCategoryObject
    // (the protected `CustomClass` UPROPERTY is not directly accessible).
    if (UK2Node_GetSubsystem* SubsystemNode = Cast<UK2Node_GetSubsystem>(Node))
    {
        UEdGraphPin* ResultPin = SubsystemNode->GetResultPin();
        UClass* SubsystemClass = ResultPin
            ? Cast<UClass>(ResultPin->PinType.PinSubCategoryObject.Get())
            : nullptr;
        if (SubsystemClass)
        {
            const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
            return FString::Printf(TEXT("%%%s%s = subsystem<%s>()"),
                *ResultName, *TypeAnno, *SubsystemClass->GetName());
        }
    }

    // General pure function call
    UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node);
    if (CallNode)
    {
        const FString RawPureFuncName = GetFunctionDisplayName(CallNode);
        FString FuncName = RawPureFuncName.Contains(BpirSharedConstants::Syntax::QualifiedNameSeparator)
            ? RawPureFuncName
            : FIrTextUtils::FormatNameToken(RawPureFuncName);
        // Pure call sites always have a consumed result, so multi-self fan-out is
        // disallowed (UE's CanFunctionSupportMultipleTargets requires no return param);
        // FormatTargetPrefix returns at most one entry here.
        TArray<FString> TargetArgs = FormatTargetPrefix(Node, ResolvePin);
        const FString TargetArg = TargetArgs.Num() > 0 ? TargetArgs[0] : FString();
        FString GeneratedPrefix;
        FString Args = FormatArgs(Node, ResolvePin, true, nullptr, &GeneratedPrefix);
        FString AllArgs;
        if (!TargetArg.IsEmpty() && !Args.IsEmpty())
            AllArgs = TargetArg + TEXT(", ") + Args;
        else if (!TargetArg.IsEmpty())
            AllArgs = TargetArg;
        else
            AllArgs = Args;
        const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
        const TCHAR* PureKeyword = Node->IsA<UK2Node_CallParentFunction>()
            ? BpirSharedConstants::Keywords::ParentCall
            : BpirSharedConstants::Keywords::Call;
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            GeneratedPrefix,
            FString::Printf(TEXT("%%%s%s = %s %s(%s)"), *ResultName, *TypeAnno, PureKeyword, *FuncName, *AllArgs));
    }

    // Any remaining pure UK2Node subclass with no specific handler above (e.g.
    // UK2Node_GetEnumeratorName) must round-trip through the generic-K2Node form
    // `call K2Node_<ClassName>(...)`, which is the only pure-node syntax the BPIR
    // compiler can re-resolve (EmitGenericK2NodeInstruction routes on the
    // K2Node_ prefix). The title-derived fallback below produces a display-name
    // token (e.g. "Enum_to_Name") that no library resolver can find, so it breaks
    // decompile→recompile. EmitGenericNode emits the class-named call plus the
    // CDO-diff node_props block; an empty LabelMap is passed because pure nodes
    // have no exec targets, so no exec suffix is appended.
    if (Node->IsA<UK2Node>())
    {
        return EmitGenericNode(Node, ResultName, FBpirLabelMap{}, ResolvePin);
    }

    // Fallback for other pure nodes
    FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::MenuTitle).ToString();
    NodeTitle.ReplaceInline(TEXT(" "), TEXT("_"));
    // Strip parentheses and their content — display names like "Equal_(Enum)"
    // contain characters that conflict with BPIR argument-list syntax
    {
        int32 ParenIdx = INDEX_NONE;
        if (NodeTitle.FindChar(TEXT('('), ParenIdx))
        {
            int32 CloseIdx = NodeTitle.Find(TEXT(")"), ESearchCase::IgnoreCase, ESearchDir::FromStart, ParenIdx);
            if (CloseIdx != INDEX_NONE)
            {
                NodeTitle = NodeTitle.Left(ParenIdx) + NodeTitle.Mid(CloseIdx + 1);
            }
            else
            {
                NodeTitle = NodeTitle.Left(ParenIdx);
            }
            while (NodeTitle.Len() > 0 && NodeTitle[NodeTitle.Len() - 1] == TEXT('_'))
            {
                NodeTitle.LeftChopInline(1);
            }
        }
    }
    FString GeneratedPrefix;
    FString Args = FormatArgs(Node, ResolvePin, false, nullptr, &GeneratedPrefix);
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return BpirTextEmitterInternal::AddGeneratedPrefix(
        GeneratedPrefix,
        FString::Printf(TEXT("%%%s%s = call %s(%s)"), *ResultName, *TypeAnno, *NodeTitle, *Args));
}

// ---------------------------------------------------------------------------
// Generic node (fallback for any UEdGraphNode)
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitGenericNode(
    UEdGraphNode* Node,
    const FString& ResultName,
    const FBpirLabelMap& LabelMap,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    if (!Node)
    {
        return TEXT("# [ERROR] nullptr in EmitGenericNode");
    }

    FString ClassName = Node->GetClass()->GetName();
    FString GeneratedPrefix;
    FString Args = FormatArgs(Node, ResolvePin, true, nullptr, &GeneratedPrefix);
    FString ExecTargets = FormatExecTargets(LabelMap);

    // Emit non-default UPROPERTY values via the same CDO-diff helper used by asset
    // dumps. The diff returns a sorted FJsonObject of `{ PropName: { type, value, ... } }`
    // entries; we extract each `.value` and route it through BpirStructLiteralUtils so
    // struct literals share the existing ExportText path. Block is omitted entirely
    // when the diff is empty (default-state nodes).
    FString NodePropsBlock;
    {
        UClass* NodeClass = Node->GetClass();
        UObject* CDO = NodeClass ? NodeClass->GetDefaultObject() : nullptr;
        if (CDO)
        {
            TSharedPtr<FJsonObject> Diff = BuildSparsePropertyDiffJson(Node, CDO, TSet<FName>());
            if (Diff.IsValid() && Diff->Values.Num() > 0)
            {
                TArray<FString> PropParts;
                PropParts.Reserve(Diff->Values.Num());
                for (const auto& KV : Diff->Values)
                {
                    FProperty* Prop = NodeClass->FindPropertyByName(*KV.Key);
                    TSharedPtr<FJsonValue> Wrapped = KV.Value;
                    TSharedPtr<FJsonValue> ValueField;
                    if (Wrapped.IsValid() && Wrapped->Type == EJson::Object)
                    {
                        const TSharedPtr<FJsonObject>& WrapperObj = Wrapped->AsObject();
                        if (WrapperObj.IsValid())
                        {
                            ValueField = WrapperObj->TryGetField(TEXT("value"));
                        }
                    }
                    if (!ValueField.IsValid())
                    {
                        ValueField = Wrapped;
                    }
                    const FString Formatted = BpirStructLiteralUtils::FormatJsonValueAsBpir(ValueField, Prop);
                    PropParts.Add(FString::Printf(TEXT("%s: %s"), *KV.Key, *Formatted));
                }
                NodePropsBlock = FString::Printf(TEXT(" %s { %s }"),
                    BpirSharedConstants::Keywords::NodeProps,
                    *FString::Join(PropParts, TEXT(", ")));
            }
        }
    }

    if (ResultName.IsEmpty())
    {
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            GeneratedPrefix,
            FString::Printf(TEXT("call %s(%s)%s%s"), *ClassName, *Args, *NodePropsBlock, *ExecTargets));
    }
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return BpirTextEmitterInternal::AddGeneratedPrefix(
        GeneratedPrefix,
        FString::Printf(TEXT("%%%s%s = call %s(%s)%s%s"), *ResultName, *TypeAnno, *ClassName, *Args, *NodePropsBlock, *ExecTargets));
}

FString FBpirTextEmitter::EmitAsyncActionNode(
    UEdGraphNode* Node,
    const FString& ResultName,
    const FBpirLabelMap& LabelMap,
    const TFunction<FString(UEdGraphPin*)>& ResolvePin)
{
    UK2Node_BaseAsyncTask* AsyncNode = Cast<UK2Node_BaseAsyncTask>(Node);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
    // GetFactoryFunction() was made public in UE 5.6.
    UFunction* FactoryFunction = AsyncNode ? AsyncNode->GetFactoryFunction() : nullptr;
#else
    // On UE 5.4/5.5 GetFactoryFunction() is protected; use UObject reflection to
    // resolve the function via the ProxyFactoryFunctionName UPROPERTY and the
    // ProxyFactoryClass, both of which are accessible at runtime.
    UFunction* FactoryFunction = nullptr;
    if (AsyncNode)
    {
        if (const FNameProperty* NameProp = FindFProperty<FNameProperty>(
                UK2Node_BaseAsyncTask::StaticClass(), TEXT("ProxyFactoryFunctionName")))
        {
            const FName FuncName = NameProp->GetPropertyValue_InContainer(AsyncNode);
            if (!FuncName.IsNone())
            {
                if (const FObjectProperty* ClassProp = FindFProperty<FObjectProperty>(
                        UK2Node_BaseAsyncTask::StaticClass(), TEXT("ProxyFactoryClass")))
                {
                    UClass* FactoryClass = Cast<UClass>(
                        ClassProp->GetObjectPropertyValue_InContainer(AsyncNode));
                    if (FactoryClass)
                    {
                        FactoryFunction = FactoryClass->FindFunctionByName(FuncName);
                    }
                }
            }
        }
    }
#endif
    if (!FactoryFunction)
    {
        return EmitGenericNode(Node, ResultName, LabelMap, ResolvePin);
    }

    const FString ClassName = FString::Printf(TEXT("%s%s"), BpirSharedConstants::AsyncAction::ClassPrefix, *FactoryFunction->GetName());
    FString GeneratedPrefix;
    FString Args = FormatArgs(Node, ResolvePin, true, nullptr, &GeneratedPrefix);
    FString ExecTargets = FormatExecTargets(LabelMap);

    if (ResultName.IsEmpty())
    {
        return BpirTextEmitterInternal::AddGeneratedPrefix(
            GeneratedPrefix,
            FString::Printf(TEXT("call %s(%s)%s"), *ClassName, *Args, *ExecTargets));
    }
    const FString TypeAnno = ResolvePrimaryOutputTypeAnnotation(Node);
    return BpirTextEmitterInternal::AddGeneratedPrefix(
        GeneratedPrefix,
        FString::Printf(TEXT("%%%s%s = call %s(%s)%s"), *ResultName, *TypeAnno, *ClassName, *Args, *ExecTargets));
}

// ---------------------------------------------------------------------------
// Unknown node
// ---------------------------------------------------------------------------

FString FBpirTextEmitter::EmitUnknownNode(UEdGraphNode* Node)
{
    if (!Node)
    {
        return TEXT("# [UNKNOWN] nullptr");
    }

    FString ClassName = Node->GetClass() ? Node->GetClass()->GetName() : TEXT("Unknown");
    FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
    return FString::Printf(TEXT("# [UNKNOWN] %s: %s"), *ClassName, *NodeTitle);
}
