// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "CRIR/CRIRDecompiler.h"

#include "CRIR/CRIROpcodes.h"
#include "CRIR/CRIRParser.h"
#include "CRIR/CRIRTextEmitter.h"

#include "Utils/ControlRigBlueprintCompat.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyDefines.h"
#include "Rigs/RigHierarchyElements.h"

#include "IrCore/IrTextUtils.h"

#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMFunction.h"
#include "RigVMCore/RigVMGraphFunctionDefinition.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMCommentNode.h"
#include "RigVMModel/Nodes/RigVMDispatchNode.h"
#include "RigVMModel/Nodes/RigVMEnumNode.h"
#include "RigVMModel/Nodes/RigVMFunctionEntryNode.h"
#if __has_include("RigVMModel/Nodes/RigVMFunctionInterfaceNode.h")
#include "RigVMModel/Nodes/RigVMFunctionInterfaceNode.h"
#endif
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReturnNode.h"
#include "RigVMModel/Nodes/RigVMInvokeEntryNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/Nodes/RigVMRerouteNode.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"

#include "Math/Transform.h"
#include "Math/UnrealMathUtility.h"
#include "Compat/EngineVersionCompat.h"
#include "Misc/PackageName.h"

namespace
{
constexpr float TransformEpsilon = 1.e-4f;

constexpr const TCHAR* kRigVMDispatchIfName = TEXT("RigVMDispatch_If");
constexpr const TCHAR* kRigVMDispatchSelectPrefix = TEXT("RigVMDispatch_Select");

bool IsApproxEqual(double A, double B)
{
    return FMath::IsNearlyEqual(A, B, static_cast<double>(TransformEpsilon));
}

bool IsLocationDefault(const FVector& V)
{
    return IsApproxEqual(V.X, 0.0) && IsApproxEqual(V.Y, 0.0) && IsApproxEqual(V.Z, 0.0);
}

bool IsRotationDefault(const FRotator& R)
{
    return IsApproxEqual(R.Pitch, 0.0) && IsApproxEqual(R.Yaw, 0.0) && IsApproxEqual(R.Roll, 0.0);
}

bool IsScaleDefault(const FVector& V)
{
    return IsApproxEqual(V.X, 1.0) && IsApproxEqual(V.Y, 1.0) && IsApproxEqual(V.Z, 1.0);
}

bool TryClassifyElementKind(ERigElementType Type, ECRIRElementKind& OutKind)
{
    switch (Type)
    {
    case ERigElementType::Bone:    OutKind = ECRIRElementKind::Bone;    return true;
    case ERigElementType::Null:    OutKind = ECRIRElementKind::Null;    return true;
    case ERigElementType::Control: OutKind = ECRIRElementKind::Control; return true;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    // ERigElementType::Socket was added in UE 5.4; 5.3 hierarchies never contain sockets.
    case ERigElementType::Socket:  OutKind = ECRIRElementKind::Socket;  return true;
#endif
    case ERigElementType::Curve:   OutKind = ECRIRElementKind::Curve;   return true;
    default: return false;
    }
}

FString ElementTypeToDebugString(ERigElementType Type)
{
    const UEnum* TypeEnum = StaticEnum<ERigElementType>();
    if (TypeEnum)
    {
        return TypeEnum->GetNameStringByValue(static_cast<int64>(Type));
    }
    return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(Type));
}

// Pre-order DFS over the rig hierarchy. The compiler reconstructs parents from
// the `parent=` attribute, so children must come after their parents in the
// emitted text. Children at each level are sorted by name for byte-stable
// output across consecutive decompiles.
void CollectElementsPreOrder(
    URigHierarchy* Hierarchy,
    const FRigElementKey& Key,
    TArray<FRigElementKey>& OutOrdered)
{
    OutOrdered.Add(Key);
    TArray<FRigElementKey> Children = Hierarchy->GetChildren(Key, /*bRecursive*/false);
    Children.Sort([](const FRigElementKey& A, const FRigElementKey& B)
    {
        return A.Name.Compare(B.Name) < 0;
    });
    for (const FRigElementKey& Child : Children)
    {
        CollectElementsPreOrder(Hierarchy, Child, OutOrdered);
    }
}

FString EmitHierarchyBlock(URigHierarchy* Hierarchy, TArray<FString>& OutWarnings)
{
    TArray<FString> Lines;
    Lines.Add(FCRIRTextEmitter::EmitRigHierarchyHeader());

    if (Hierarchy && Hierarchy->Num() > 0)
    {
        TArray<FRigElementKey> Roots = Hierarchy->GetRootElementKeys();
        Roots.Sort([](const FRigElementKey& A, const FRigElementKey& B)
        {
            return A.Name.Compare(B.Name) < 0;
        });

        TArray<FRigElementKey> Ordered;
        for (const FRigElementKey& Root : Roots)
        {
            CollectElementsPreOrder(Hierarchy, Root, Ordered);
        }

        TArray<FString> BodyLines;
        for (const FRigElementKey& Key : Ordered)
        {
            ECRIRElementKind Kind;
            if (!TryClassifyElementKind(Key.Type, Kind))
            {
                const FString Note = FString::Printf(
                    TEXT("# TODO unsupported element kind: %s (name=%s)"),
                    *ElementTypeToDebugString(Key.Type),
                    *Key.Name.ToString());
                BodyLines.Add(Note);
                OutWarnings.Add(FString::Printf(
                    TEXT("CRIR_UNSUPPORTED_ELEMENT: %s '%s' decompiled as TODO comment"),
                    *ElementTypeToDebugString(Key.Type),
                    *Key.Name.ToString()));
                continue;
            }

            const FRigElementKey ParentKey = Hierarchy->GetFirstParent(Key);
            const FString ParentName = (ParentKey.Type != ERigElementType::None) ? ParentKey.Name.ToString() : FString();

            TArray<TPair<FString, FString>> Attributes;

            if (Kind == ECRIRElementKind::Curve)
            {
                Attributes.Add({TEXT("value"), FString::Printf(TEXT("%g"), Hierarchy->GetCurveValue(Key))});
                BodyLines.Add(FCRIRTextEmitter::EmitElement(Kind, Key.Name.ToString(), FString(), Attributes));
                continue;
            }

            const FTransform LocalXf = Hierarchy->GetLocalTransform(Key);
            const FVector Loc = LocalXf.GetLocation();
            if (!IsLocationDefault(Loc))
            {
                Attributes.Add({TEXT("location"), FCRIRTextEmitter::FormatVector(Loc)});
            }
            const FRotator Rot = LocalXf.GetRotation().Rotator();
            if (!IsRotationDefault(Rot))
            {
                Attributes.Add({TEXT("rotation"), FCRIRTextEmitter::FormatRotator(Rot)});
            }
            const FVector Scale = LocalXf.GetScale3D();
            if (!IsScaleDefault(Scale))
            {
                Attributes.Add({TEXT("scale"), FCRIRTextEmitter::FormatVector(Scale)});
            }

            // Phase B control mutation: emit full FRigControlSettings via the
            // multi-line EmitControlElement helper. Non-control elements still
            // route through the flat EmitElement path.
            if (Kind == ECRIRElementKind::Control)
            {
                const FRigControlElement* ControlElement = Hierarchy->Find<FRigControlElement>(Key);
                if (ControlElement)
                {
                    const FRigControlSettings& Settings = ControlElement->Settings;
                    const ERigControlType Type = Settings.ControlType;
                    const FRigControlValue Value = Hierarchy->GetControlValue(
                        const_cast<FRigControlElement*>(ControlElement),
                        ERigControlValueType::Current);
                    const FString ValueLit = FCRIRTextEmitter::FormatControlValue(Type, Value);
                    FString ShapeLit;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
                    // FRigControlSettings::ShapeTransform was added in UE 5.6.
                    const FTransform& ShapeXf = Settings.ShapeTransform;
#else
                    // In UE 5.4/5.5 the shape transform lives on FRigControlElement::Shape
                    // (a FRigCurrentAndInitialTransform), not on FRigControlSettings.
                    const FTransform ShapeXf = Hierarchy->GetControlShapeTransform(
                        const_cast<FRigControlElement*>(ControlElement),
                        ERigTransformType::CurrentLocal);
#endif
                    if (!ShapeXf.Equals(FTransform::Identity))
                    {
                        // Top-level shape= is a transform literal regardless of ControlType.
                        FRigControlValue ShapeRcv = FRigControlValue::Make<FTransform>(ShapeXf);
                        ShapeLit = FCRIRTextEmitter::FormatControlValue(ERigControlType::Transform, ShapeRcv);
                    }
                    const FString SubBlock = FCRIRTextEmitter::FormatControlSettingsSubBlock(Settings, Type);
                    BodyLines.Add(FCRIRTextEmitter::EmitControlElement(
                        Key.Name.ToString(),
                        ParentName,
                        Type,
                        ValueLit,
                        ShapeLit,
                        Attributes,
                        SubBlock));
                    continue;
                }
            }

            BodyLines.Add(FCRIRTextEmitter::EmitElement(Kind, Key.Name.ToString(), ParentName, Attributes));
        }

        if (BodyLines.Num() > 0)
        {
            const FString Body = FString::Join(BodyLines, TEXT("\n"));
            Lines.Add(FCRIRTextEmitter::IndentBlockBody(Body));
        }
    }

    Lines.Add(FCRIRTextEmitter::EmitBlockFooter());
    return FString::Join(Lines, TEXT("\n"));
}

// Pin path *relative to the owning node* — what the compiler needs to
// reconstruct the link via URigVMController::AddLink. URigVMPin::GetSegmentPath
// with bIncludeRootPin=true gives "RootPin.SubA.SubB"; the leading node name
// is stripped because the local-id reference (`%nN`) already carries it.
FString GetPinPathRelativeToNode(const URigVMPin* Pin)
{
    if (!Pin)
    {
        return FString();
    }
    return Pin->GetSegmentPath(/*bIncludeRootPin*/true);
}

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
// CVar-independent stand-in for URigVMPin::HasDefaultValueOverride().
//
// Do NOT call the engine predicate to decide whether to emit a pin literal. Its
// first statement is an unconditional bail on a console variable that ships off:
//
//     RigVMPin.cpp:34   TAutoConsoleVariable<bool> CVarRigVMEnablePinOverrides(
//                           TEXT("RigVM.EnablePinOverrides"), false, ...)
//     RigVMPin.cpp:1363 if(!CVarRigVMEnablePinOverrides.GetValueOnAnyThread()) { return false; }
//
// Nothing in this project or the engine's shipped ini files turns it on, so the
// predicate answered "no override" for every pin on every node, and CRIR silently
// dropped EVERY authored pin literal — reroutes, unit-node inputs, interface pins
// alike. Round-trip tests could not see it: both decompiles run this same emitter,
// so a value the emitter stops reading is missing from both texts and they stay
// byte-equal over a graph that lost its values.
//
// The body below mirrors the rest of URigVMPin::HasDefaultValueOverride()
// statement for statement — the CanProvideDefaultValue() eligibility gate, the
// explicit Override type check, the recursion into sub-pins, and the
// "differs from the struct's original default" fallback. None of those three
// engine calls is CVar-gated. Deliberately uncached: the engine memoizes on the
// pin through a private mutable field we cannot reach, and pin counts here are
// small enough that recomputation is not worth a parallel cache that could drift.
bool PinHasAuthoredDefaultOverride(const URigVMPin* Pin)
{
    if (!Pin || !Pin->CanProvideDefaultValue())
    {
        return false;
    }
    if (Pin->GetDefaultValueType() == ERigVMPinDefaultValueType::Override)
    {
        return true;
    }
    for (const URigVMPin* SubPin : Pin->GetSubPins())
    {
        if (PinHasAuthoredDefaultOverride(SubPin))
        {
            return true;
        }
    }
    return !Pin->HasOriginalDefaultValue();
}
#endif

void CollectArgsForNode(
    const URigVMNode* Node,
    const TMap<const URigVMNode*, FString>& NodeToLocalId,
    TArray<FCRIRArg>& OutArgs)
{
    for (URigVMPin* Pin : Node->GetPins())
    {
        if (!Pin)
        {
            continue;
        }

        const ERigVMPinDirection Direction = Pin->GetDirection();
        if (Direction == ERigVMPinDirection::Hidden || Direction == ERigVMPinDirection::Invalid)
        {
            continue;
        }

        const FString PinName = Pin->GetName();

        // Incoming wire: emitted on this side only. URigVMPin::GetSourceLinks
        // walks recursively through subpins so a link landing on RootPin.SubA
        // surfaces at the root pin level — encode the full sub-segment in the
        // arg name suffix so the compiler can resolve the exact target pin.
        const TArray<URigVMLink*> SourceLinks = Pin->GetSourceLinks(/*bRecursive*/true);
        const bool bHasIncomingLinks = SourceLinks.Num() > 0;
        for (URigVMLink* Link : SourceLinks)
        {
            if (!Link)
            {
                continue;
            }
            const URigVMPin* SourcePin = Link->GetSourcePin();
            const URigVMPin* TargetPin = Link->GetTargetPin();
            if (!SourcePin || !TargetPin)
            {
                continue;
            }
            const URigVMNode* SourceNode = Link->GetSourceNode();
            const FString* SourceLocalId = NodeToLocalId.Find(SourceNode);
            if (!SourceLocalId)
            {
                // Source node was an unsupported kind (skipped earlier). Drop
                // the link — the warning was already recorded for the source.
                continue;
            }
            const FString TargetSegment = GetPinPathRelativeToNode(TargetPin);
            const FString SourceSegment = GetPinPathRelativeToNode(SourcePin);

            FCRIRArg Arg;
            Arg.Name = FString::Printf(TEXT("wire_in_%s"), *TargetSegment);
            Arg.bIsLocalRef = true;
            Arg.LocalRefNode = *SourceLocalId;
            Arg.LocalRefPin = SourceSegment;
            OutArgs.Add(Arg);
        }

        // Literal default: only emit when the pin has no incoming wire AND a
        // non-empty default. Output-only pins never carry literals worth
        // round-tripping.
        if (Direction == ERigVMPinDirection::Output)
        {
            continue;
        }
        if (bHasIncomingLinks)
        {
            continue;
        }
        // Skip pins matching the engine default — emitting `(0, 0, 0)` /
        // `0.0` / `false` everywhere bloats the text and slows round-trips.
        // "User changed this from the struct default" is answered by
        // PinHasAuthoredDefaultOverride, not by URigVMPin::HasDefaultValueOverride():
        // the engine one is gated on a console variable that ships off, so it
        // answers "no" for every pin and drops every literal (see the helper).
        const FString DefaultValue = Pin->GetDefaultValue();
        if (DefaultValue.IsEmpty())
        {
            continue;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        if (!PinHasAuthoredDefaultOverride(Pin))
        {
            continue;
        }
#else
        // On UE 5.3-5.5 HasDefaultValueOverride() does not exist. For dispatch nodes, skip
        // pins whose default still matches the template's initial default — otherwise a
        // round-trip is asymmetric: a freshly resolved arg (e.g. ArrayAdd's float Element
        // reads "0.000000") vs. the same arg after compile, where the dispatch arm is
        // recreated as an unresolved wildcard reading "()". GetInitialDefaultValueForPin
        // yields the template's clean default for both states, so equal-to-initial pins
        // drop out on both sides. This mirrors the 5.6 HasDefaultValueOverride() skip.
        // Scoped to dispatch nodes (the only kind that re-emerges unresolved post-compile);
        // unit nodes resolve their argument types at creation and don't exhibit the flip.
        // A non-empty, non-template default string remains the proxy for a user override.
        if (const URigVMDispatchNode* DispatchTemplate = Cast<URigVMDispatchNode>(Node))
        {
            // A wildcard (unresolved) arg pin carries no meaningful authored default — the
            // compiler recreates dispatch arms unresolved, so the post-compile side reports
            // an unresolved "()" while the pre-compile side was resolved. Drop both.
            if (Pin->IsWildCard())
            {
                continue;
            }
            if (DefaultValue == DispatchTemplate->GetInitialDefaultValueForPin(Pin->GetFName()))
            {
                continue;
            }
        }
#endif

        FCRIRArg Arg;
        Arg.Name = PinName;
        Arg.RawText = DefaultValue;
        Arg.bIsLocalRef = false;
        OutArgs.Add(Arg);
    }
}

FCRIRPosition MakePosition(const FVector2D& InPos)
{
    // Every node read from URigVMNode::GetPosition has a real authored position;
    // the previous (0,0) special case silently dropped deliberate placements at
    // the origin, causing auto-layout to move them on round-trip.
    FCRIRPosition Out;
    Out.bSet = true;
    Out.X = static_cast<float>(InPos.X);
    Out.Y = static_cast<float>(InPos.Y);
    return Out;
}

// Walk an interface (entry/return) node's pins and emit args for incoming
// wires + any pin-default overrides — regardless of pin direction. The
// standard CollectArgsForNode skips Output pins entirely, which would lose
// function_entry pin defaults (entry pins are Outputs of the entry node).
// When bEmitAllNonEmptyDefaults is true, emit any non-empty default value
// regardless of `HasDefaultValueOverride()` (which short-circuits for Output
// direction pins via `CanProvideDefaultValue`). Set this for function-library
// entry/return nodes whose exposed-pin signature was emitted via `exposed_pin`
// lines; for collapse-subgraph interface nodes leave it false so we don't try
// to round-trip pin-defaults onto pins the compile path can't recreate.
void CollectInterfaceArgsForNode(
    const URigVMNode* Node,
    const TMap<const URigVMNode*, FString>& NodeToLocalId,
    TArray<FCRIRArg>& OutArgs,
    bool bEmitAllNonEmptyDefaults = false)
{
    for (URigVMPin* Pin : Node->GetPins())
    {
        if (!Pin)
        {
            continue;
        }
        const ERigVMPinDirection Direction = Pin->GetDirection();
        if (Direction == ERigVMPinDirection::Hidden || Direction == ERigVMPinDirection::Invalid)
        {
            continue;
        }

        const TArray<URigVMLink*> SourceLinks = Pin->GetSourceLinks(/*bRecursive*/true);
        const bool bHasIncomingLinks = SourceLinks.Num() > 0;
        for (URigVMLink* Link : SourceLinks)
        {
            if (!Link) { continue; }
            const URigVMPin* SourcePin = Link->GetSourcePin();
            const URigVMPin* TargetPin = Link->GetTargetPin();
            if (!SourcePin || !TargetPin) { continue; }
            const URigVMNode* SourceNode = Link->GetSourceNode();
            const FString* SourceLocalId = NodeToLocalId.Find(SourceNode);
            if (!SourceLocalId) { continue; }
            FCRIRArg Arg;
            Arg.Name = FString::Printf(TEXT("wire_in_%s"), *TargetPin->GetSegmentPath(true));
            Arg.bIsLocalRef = true;
            Arg.LocalRefNode = *SourceLocalId;
            Arg.LocalRefPin = SourcePin->GetSegmentPath(true);
            OutArgs.Add(Arg);
        }

        if (bHasIncomingLinks)
        {
            continue;
        }
        // Skip the execute-context pin — it never carries a data default.
        if (Pin->IsExecuteContext())
        {
            continue;
        }
        // Interface (entry/return) pins inherit their direction from the
        // *opposite* of the exposed-pin direction — an `Input` exposed pin
        // becomes an `Output` pin on the entry node so its value flows into
        // the function body. The override predicate short-circuits for
        // Output / IO_with_no_input directions via
        // CanProvideDefaultValue, which would silently drop authored entry/
        // return defaults. We bypass that gate ONLY when the caller has
        // committed to round-tripping the signature via `exposed_pin` lines
        // (function-library blocks). Collapse subgraphs don't emit a
        // signature, so we keep the original gate to avoid emitting type-zero
        // defaults that the compile path can't reapply.
        const FString DefaultValue = Pin->GetDefaultValue();
        if (DefaultValue.IsEmpty())
        {
            continue;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
        if (!bEmitAllNonEmptyDefaults && !PinHasAuthoredDefaultOverride(Pin))
        {
            continue;
        }
#else
        // On UE 5.4/5.5 HasDefaultValueOverride() does not exist; treat any
        // non-empty default value as an override (same proxy used elsewhere).
        if (!bEmitAllNonEmptyDefaults)
        {
            continue;
        }
#endif
        FCRIRArg Arg;
        Arg.Name = Pin->GetName();
        Arg.RawText = DefaultValue;
        Arg.bIsLocalRef = false;
        OutArgs.Add(Arg);
    }
}

// Emit the body of a graph (entry/return interface lines + sorted node
// instructions) WITHOUT the outer `rig_graph` / `rig_function` header/footer.
// The Body string returned has one instruction per line, NOT indented — the
// caller wraps with IndentBlockBody before sandwiching with header/footer.
FString EmitGraphBody(URigVMGraph* Graph, int32 Depth, TArray<FString>& OutWarnings, bool bIsFunctionBody = false)
{
    if (!Graph)
    {
        return FString();
    }

    // Sort nodes by UObject name for deterministic local-id assignment.
    TArray<URigVMNode*> Nodes = Graph->GetNodes();
    Nodes.Sort([](const URigVMNode& A, const URigVMNode& B)
    {
        return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
    });

    // Pre-pass: assign local ids to every node we *can* emit and that can
    // serve as a wire source. Comment nodes never source wires, so they're
    // intentionally excluded from the map. URigVMTemplateNode is the common
    // base for unit (URigVMUnitNode), dispatch (URigVMDispatchNode), and bare
    // template instances; collapse / function-ref / function interface nodes
    // also source wires and need ids.
    URigVMFunctionEntryNode* EntryNode = Graph->GetEntryNode();
    URigVMFunctionReturnNode* ReturnNode = Graph->GetReturnNode();

    TMap<const URigVMNode*, FString> NodeToLocalId;
    {
        int32 NextId = 0;
        for (URigVMNode* Node : Nodes)
        {
            // Interface nodes (entry/return) are emitted as fixed first/last
            // body lines without LocalIds.
            if (Node == EntryNode || Node == ReturnNode)
            {
                continue;
            }
            if (Cast<URigVMTemplateNode>(Node) != nullptr
                || Cast<URigVMVariableNode>(Node) != nullptr
                || Cast<URigVMRerouteNode>(Node) != nullptr
                || Cast<URigVMEnumNode>(Node) != nullptr
                || Cast<URigVMInvokeEntryNode>(Node) != nullptr
                || Cast<URigVMCollapseNode>(Node) != nullptr
                || Cast<URigVMFunctionReferenceNode>(Node) != nullptr)
            {
                NodeToLocalId.Add(Node, FCRIRTextEmitter::MakeLocalId(NextId++));
            }
        }
    }

    TArray<FString> BodyLines;

    // function_entry as the first body line (when present).
    if (EntryNode)
    {
        TArray<FCRIRArg> Args;
        CollectInterfaceArgsForNode(EntryNode, NodeToLocalId, Args, /*bEmitAllNonEmptyDefaults*/ bIsFunctionBody);
        BodyLines.Add(FCRIRTextEmitter::EmitFunctionEntry(
            Args,
            MakePosition(EntryNode->GetPosition())));
    }

    for (URigVMNode* Node : Nodes)
    {
        // Skip interface nodes — emitted as fixed first/last body lines.
        if (Node == EntryNode || Node == ReturnNode)
        {
            continue;
        }
        if (!Node)
        {
            continue;
        }

        // Comment first — no LocalId, no wires. URigVMCommentNode never appears
        // in NodeToLocalId so the wire-collection path can't dereference it.
        if (URigVMCommentNode* CommentNode = Cast<URigVMCommentNode>(Node))
        {
            const FString Text = CommentNode->GetCommentText();
            const FVector2D Size = CommentNode->GetSize();
            const FString SizeStr = FString::Printf(TEXT("(%g,%g)"), Size.X, Size.Y);
            const FLinearColor Color = CommentNode->GetNodeColor();
            const FString ColorStr = FString::Printf(TEXT("(%g,%g,%g,%g)"), Color.R, Color.G, Color.B, Color.A);
            BodyLines.Add(FCRIRTextEmitter::EmitComment(
                Text,
                SizeStr,
                ColorStr,
                MakePosition(Node->GetPosition())));
            continue;
        }

        if (URigVMRerouteNode* RerouteNode = Cast<URigVMRerouteNode>(Node))
        {
            const FString* LocalId = NodeToLocalId.Find(Node);
            check(LocalId);

            // Reroute carries its type on the Value pin (both input and
            // output pass-through). Default is the literal stored on that
            // pin when the reroute is IsLiteral() (no incoming source).
            FString CPPType;
            FString DefaultValue;
            for (URigVMPin* Pin : RerouteNode->GetPins())
            {
                if (Pin && Pin->GetName() == TEXT("Value"))
                {
                    CPPType = Pin->GetCPPType();
                    if (RerouteNode->IsLiteral())
                    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 6, 0)
                        if (PinHasAuthoredDefaultOverride(Pin))
#endif
                        {
                            DefaultValue = Pin->GetDefaultValue();
                        }
                    }
                    break;
                }
            }

            TArray<FCRIRArg> Args;
            CollectArgsForNode(Node, NodeToLocalId, Args);

            BodyLines.Add(FCRIRTextEmitter::EmitReroute(
                *LocalId,
                CPPType,
                DefaultValue,
                Args,
                MakePosition(Node->GetPosition())));
            continue;
        }

        // If/Select are URigVMDispatchNode instances today; discriminate by
        // the factory's script struct name. Other dispatch kinds (math /
        // array / etc.) fall through to the TODO comment.
        if (URigVMDispatchNode* DispatchNode = Cast<URigVMDispatchNode>(Node))
        {
            const FRigVMDispatchFactory* Factory = DispatchNode->GetFactory();
            const UScriptStruct* FactoryStruct = Factory ? Factory->GetScriptStruct() : nullptr;
            const FString FactoryName = FactoryStruct ? FactoryStruct->GetName() : FString();
            const bool bIsIf = FactoryName == kRigVMDispatchIfName;
            const bool bIsSelect = FactoryName.StartsWith(kRigVMDispatchSelectPrefix);

            if (bIsIf || bIsSelect)
            {
                const FString* LocalId = NodeToLocalId.Find(Node);
                check(LocalId);

                // CPP type is carried on the Result pin (both If and Select
                // share this convention from FRigVMDispatch_Core).
                FString CPPType;
                for (URigVMPin* Pin : DispatchNode->GetPins())
                {
                    if (Pin && Pin->GetName() == TEXT("Result"))
                    {
                        CPPType = Pin->GetCPPType();
                        break;
                    }
                }

                TArray<FCRIRArg> Args;
                CollectArgsForNode(Node, NodeToLocalId, Args);

                BodyLines.Add(bIsIf
                    ? FCRIRTextEmitter::EmitIf(*LocalId, CPPType, Args, MakePosition(Node->GetPosition()))
                    : FCRIRTextEmitter::EmitSelect(*LocalId, CPPType, Args, MakePosition(Node->GetPosition())));
                continue;
            }

            // General dispatch arm — every non-if/select dispatch factory routes
            // through the unified `dispatch <FactoryStructName>` opcode.
            if (!Factory || !FactoryStruct)
            {
                BodyLines.Add(FString::Printf(
                    TEXT("# TODO dispatch node '%s' has no factory script struct"),
                    *Node->GetName()));
                OutWarnings.Add(FString::Printf(
                    TEXT("CRIR_MISSING_DISPATCH_FACTORY: dispatch node '%s' GetFactory()/GetScriptStruct() returned nullptr"),
                    *Node->GetName()));
                continue;
            }

            const FString* LocalId = NodeToLocalId.Find(Node);
            check(LocalId);

            TArray<FCRIRArg> Args;
            CollectArgsForNode(Node, NodeToLocalId, Args);

            BodyLines.Add(FCRIRTextEmitter::EmitDispatch(
                *LocalId,
                FactoryName,
                Args,
                MakePosition(Node->GetPosition())));
            continue;
        }

        if (URigVMEnumNode* EnumNode = Cast<URigVMEnumNode>(Node))
        {
            const FString* LocalId = NodeToLocalId.Find(Node);
            check(LocalId);

            const UObject* CPPTypeObject = EnumNode->GetCPPTypeObject();
            const FString EnumPath = CPPTypeObject ? CPPTypeObject->GetPathName() : EnumNode->GetCPPType();
            const FString DefaultVal = EnumNode->GetDefaultValue();

            BodyLines.Add(FCRIRTextEmitter::EmitEnum(
                *LocalId,
                EnumPath,
                DefaultVal,
                MakePosition(Node->GetPosition())));
            continue;
        }

        if (URigVMInvokeEntryNode* InvokeNode = Cast<URigVMInvokeEntryNode>(Node))
        {
            const FString* LocalId = NodeToLocalId.Find(Node);
            check(LocalId);

            TArray<FCRIRArg> Args;
            CollectArgsForNode(Node, NodeToLocalId, Args);

            BodyLines.Add(FCRIRTextEmitter::EmitInvokeEntry(
                *LocalId,
                InvokeNode->GetEntryName().ToString(),
                Args,
                MakePosition(Node->GetPosition())));
            continue;
        }

        if (URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(Node))
        {
            UScriptStruct* StructType = UnitNode->GetScriptStruct();
            // Unresolved wildcard templates instantiate a URigVMUnitNode whose
            // script struct is null (the controller picks the unit class via
            // GetNodeClassForTemplate when the template has unit permutations,
            // but the permutation isn't resolved until a type is wired in).
            // Fall through to the bare-template arm below so we emit the
            // recoverable `template <Notation>` opcode instead of a lossy TODO.
            if (StructType)
            {
                const FString* LocalId = NodeToLocalId.Find(Node);
                check(LocalId);

                TArray<FCRIRArg> Args;
                CollectArgsForNode(Node, NodeToLocalId, Args);

                BodyLines.Add(FCRIRTextEmitter::EmitUnit(
                    *LocalId,
                    StructType->GetPathName(),
                    Args,
                    MakePosition(Node->GetPosition())));
                continue;
            }
        }

        // Bare template arm — runs after unit/dispatch so resolved-unit and
        // factory-dispatch nodes claim their dedicated opcodes first. Any
        // remaining URigVMTemplateNode is an unresolved (wildcard) template.
        // Exclude URigVMLibraryNode descendants (collapse / function-ref) —
        // they inherit from URigVMTemplateNode but have dedicated arms below
        // (`rig_subgraph` / `function_ref`). Their GetNotation() returns None,
        // which would otherwise produce an unparseable bare-template line.
        if (URigVMTemplateNode* TmplNode = Cast<URigVMTemplateNode>(Node);
            TmplNode != nullptr && Cast<URigVMLibraryNode>(Node) == nullptr)
        {
            const FString* LocalId = NodeToLocalId.Find(Node);
            check(LocalId);

            TArray<FCRIRArg> Args;
            CollectArgsForNode(Node, NodeToLocalId, Args);

            BodyLines.Add(FCRIRTextEmitter::EmitTemplate(
                *LocalId,
                TmplNode->GetNotation().ToString(),
                Args,
                MakePosition(Node->GetPosition())));
            continue;
        }

        if (URigVMVariableNode* VarNode = Cast<URigVMVariableNode>(Node))
        {
            const FString* LocalId = NodeToLocalId.Find(Node);
            check(LocalId);

            const FString VarName = VarNode->GetVariableName().ToString();
            // CPP type lives on the value pin (`URigVMVariableNode` has a
            // "Value" pin whose CPP type is the variable's type). Fall back to
            // the empty string if absent — the compiler errors loudly rather
            // than silently treating it as wildcard.
            FString VarType;
            for (URigVMPin* Pin : VarNode->GetPins())
            {
                if (Pin && Pin->GetName() == TEXT("Value"))
                {
                    VarType = Pin->GetCPPType();
                    break;
                }
            }

            // Phase A var grammar = name + type + optional default. No trailing
            // arg list (see EmitVar). Get-variable nodes have only an output
            // pin; if a var node ever carries pin overrides beyond the value,
            // they're out of Phase A scope and dropped intentionally.
            BodyLines.Add(FCRIRTextEmitter::EmitVar(
                *LocalId,
                VarName,
                VarType,
                /*VarDefault*/ FString(),
                MakePosition(Node->GetPosition())));
            continue;
        }

        // Collapse arm — also handles URigVMAggregateNode via
        // Cast<URigVMCollapseNode> inheritance (URigVMAggregateNode :
        // URigVMCollapseNode). Aggregate class identity is documented-loss
        // on round-trip; the inner unit nodes carry all runtime behavior.
        // See TestCRIRCollapseRoundTrip.cpp (AggregateAsCollapse) for the
        // round-trip regression. Recurses into the contained graph.
        if (URigVMCollapseNode* CollapseNode = Cast<URigVMCollapseNode>(Node))
        {
            const FString* LocalId = NodeToLocalId.Find(Node);
            check(LocalId);

            BodyLines.Add(FCRIRTextEmitter::EmitCollapseHeader(
                *LocalId,
                Node->GetName(),
                MakePosition(Node->GetPosition())));

            const FString InnerBody = EmitGraphBody(CollapseNode->GetContainedGraph(), Depth + 1, OutWarnings);
            if (!InnerBody.IsEmpty())
            {
                BodyLines.Add(FCRIRTextEmitter::IndentBlockBody(InnerBody));
            }
            BodyLines.Add(FCRIRTextEmitter::EmitBlockFooter());
            continue;
        }

        if (URigVMFunctionReferenceNode* FuncRefNode = Cast<URigVMFunctionReferenceNode>(Node))
        {
            const FString* LocalId = NodeToLocalId.Find(Node);
            check(LocalId);

            const FRigVMGraphFunctionHeader& Header = FuncRefNode->GetReferencedFunctionHeader();
            const FString FuncName = Header.Name.ToString();

            // Same-asset references leave HostPath empty so the emitter writes
            // `function_ref Name`. External references prefix with the host
            // asset path. Determine same-vs-external by comparing top-level
            // package names of the function ref's owning asset and the host.
            FString HostPath;
            const FString OwningAssetPath = Node->GetPackage() ? Node->GetPackage()->GetPathName() : FString();
            const FString HostAssetPath = Header.LibraryPointer.HostObject.ToString();
            const FString HostPkg = FPackageName::ObjectPathToPackageName(HostAssetPath);
            if (!HostPkg.IsEmpty() && HostPkg != OwningAssetPath)
            {
                HostPath = HostAssetPath;
            }

            TArray<FCRIRArg> Args;
            CollectArgsForNode(Node, NodeToLocalId, Args);

            BodyLines.Add(FCRIRTextEmitter::EmitFunctionRef(
                *LocalId,
                FuncName,
                HostPath,
                Args,
                MakePosition(Node->GetPosition())));
            continue;
        }

        // Any remaining node kind is out of scope for this pass. Emit a TODO so
        // the dump remains diff-friendly and surface a warning.
        const FString ClassName = Node->GetClass() ? Node->GetClass()->GetName() : TEXT("Unknown");
        BodyLines.Add(FString::Printf(
            TEXT("# TODO unsupported node kind: %s (name=%s)"),
            *ClassName,
            *Node->GetName()));
        OutWarnings.Add(FString::Printf(
            TEXT("CRIR_UNSUPPORTED_NODE: %s '%s' decompiled as TODO comment"),
            *ClassName,
            *Node->GetName()));
    }

    // function_return as the last body line (when present).
    if (ReturnNode)
    {
        TArray<FCRIRArg> Args;
        CollectInterfaceArgsForNode(ReturnNode, NodeToLocalId, Args, /*bEmitAllNonEmptyDefaults*/ bIsFunctionBody);
        BodyLines.Add(FCRIRTextEmitter::EmitFunctionReturn(
            Args,
            MakePosition(ReturnNode->GetPosition())));
    }

    return FString::Join(BodyLines, TEXT("\n"));
}

FString EmitGraphBlock(URigVMGraph* Graph, TArray<FString>& OutWarnings)
{
    TArray<FString> Lines;
    Lines.Add(FCRIRTextEmitter::EmitRigGraphHeader(Graph->GetGraphName()));
    const FString Body = EmitGraphBody(Graph, /*Depth*/ 1, OutWarnings);
    if (!Body.IsEmpty())
    {
        Lines.Add(FCRIRTextEmitter::IndentBlockBody(Body));
    }
    Lines.Add(FCRIRTextEmitter::EmitBlockFooter());
    return FString::Join(Lines, TEXT("\n"));
}

// Map ERigVMPinDirection to the narrow CRIR token. Visible / Hidden / Invalid
// are not exposable on a function signature; bucket them with Input so the
// decompile still emits a syntactically valid line that a human can fix up.
ECRIRExposedPinDirection MapExposedDirection(ERigVMPinDirection Direction)
{
    switch (Direction)
    {
    case ERigVMPinDirection::Output: return ECRIRExposedPinDirection::Output;
    case ERigVMPinDirection::IO:     return ECRIRExposedPinDirection::IO;
    default:                         return ECRIRExposedPinDirection::Input;
    }
}

FString EmitRigFunctionBlock(URigVMLibraryNode* FuncNode, TArray<FString>& OutWarnings)
{
    TArray<FString> Lines;
    Lines.Add(FCRIRTextEmitter::EmitRigFunctionHeader(FuncNode->GetName()));

    // Emit the exposed-pin signature so the compile path can recreate it via
    // URigVMController::AddExposedPin. Without this the target function has no
    // signature pins and any `function_entry` default would land on a missing
    // pin. Execute-context pins are auto-created by AddExposedPin (the first
    // exposed pin triggers entry/return scaffolding) so we skip them here.
    TArray<FString> SignatureLines;
    if (FuncNode)
    {
        for (URigVMPin* Pin : FuncNode->GetPins())
        {
            if (!Pin || Pin->IsExecuteContext())
            {
                continue;
            }
            const FString CPPType = Pin->GetCPPType();
            const FString TypeObject = Pin->GetCPPTypeObject() ? Pin->GetCPPTypeObject()->GetPathName() : FString();
            const FString DefaultValue = Pin->GetDefaultValue();
            SignatureLines.Add(FCRIRTextEmitter::EmitExposedPin(
                Pin->GetName(),
                MapExposedDirection(Pin->GetDirection()),
                CPPType,
                TypeObject,
                DefaultValue));
        }
    }

    const FString Body = EmitGraphBody(FuncNode ? FuncNode->GetContainedGraph() : nullptr, /*Depth*/ 1, OutWarnings, /*bIsFunctionBody*/ true);

    TArray<FString> InnerLines;
    if (SignatureLines.Num() > 0)
    {
        InnerLines.Add(FString::Join(SignatureLines, TEXT("\n")));
    }
    if (!Body.IsEmpty())
    {
        InnerLines.Add(Body);
    }
    const FString InnerJoined = FString::Join(InnerLines, TEXT("\n"));
    if (!InnerJoined.IsEmpty())
    {
        Lines.Add(FCRIRTextEmitter::IndentBlockBody(InnerJoined));
    }
    Lines.Add(FCRIRTextEmitter::EmitBlockFooter());
    return FString::Join(Lines, TEXT("\n"));
}
} // namespace

FCRIRDecompiler::FCRIRDecompiler(UControlRigBlueprint* InBlueprint)
    : Blueprint(InBlueprint)
{
}

FCRIRDecompileResult FCRIRDecompiler::Decompile()
{
    FCRIRDecompileResult Result;

    if (!Blueprint)
    {
        Result.bSuccess = false;
        Result.ErrorCode = TEXT("CRIR_INVALID_INPUT");
        Result.ErrorMessage = TEXT("blueprint is null");
        return Result;
    }

    Result.AssetPath = Blueprint->GetPathName();

    TArray<FString> Blocks;

    // Canonical order: hierarchy first, then graphs sorted by model name.
    Blocks.Add(EmitHierarchyBlock(GetControlRigHierarchy(Blueprint), Result.Warnings));

    // Top-level user-facing models. Function library children are emitted
    // separately via their own `rig_function` blocks after the rig_graphs.
    FRigVMClient* Client = GetControlRigRigVMClient(Blueprint);
    TArray<URigVMGraph*> Models = Client
        ? Client->GetAllModels(/*bIncludeFunctionLibrary*/ false, /*bRecursive*/ false)
        : TArray<URigVMGraph*>();
    Models.Sort([](const URigVMGraph& A, const URigVMGraph& B)
    {
        return A.GetGraphName().Compare(B.GetGraphName(), ESearchCase::CaseSensitive) < 0;
    });

    for (URigVMGraph* Model : Models)
    {
        if (!Model)
        {
            continue;
        }
        Blocks.Add(EmitGraphBlock(Model, Result.Warnings));
    }

    // Function library — one `rig_function "<Name>" { ... }` block per child
    // library node, sorted by name for stable output. Emitted AFTER the regular
    // rig_graph blocks (deterministic order).
    if (Client)
    {
        if (URigVMFunctionLibrary* Library = Client->GetFunctionLibrary())
        {
            TArray<URigVMNode*> LibNodes = Library->GetNodes();
            LibNodes.Sort([](const URigVMNode& A, const URigVMNode& B)
            {
                return A.GetName().Compare(B.GetName(), ESearchCase::CaseSensitive) < 0;
            });
            for (URigVMNode* LibNode : LibNodes)
            {
                URigVMLibraryNode* FuncNode = Cast<URigVMLibraryNode>(LibNode);
                if (!FuncNode)
                {
                    continue;
                }
                Blocks.Add(EmitRigFunctionBlock(FuncNode, Result.Warnings));
            }
        }
    }

    Result.CRIRText = FString::Join(Blocks, TEXT("\n\n")) + TEXT("\n");

#if WITH_DEV_AUTOMATION_TESTS
    // Self-check: re-parse the emitted text with full reference validation. The
    // decompiler emits nodes in UObject-name order (EmitGraphBody), NOT in
    // exec/declaration order, so a wire source can be emitted *after* its sink —
    // a forward reference. The parser's ValidateInstructionScope is two-pass and
    // resolves forward refs within a scope, so re-parsing the decompiler's own
    // output must succeed; running validation here (bSkipReferenceValidation=false)
    // pins that the emitted text is genuinely re-parseable, surfacing any real
    // dangling ref as a warning. Compiled out of production builds because
    // asset.dump_folder invokes the decompiler for every Control Rig in the
    // project; the round-trip test exercises the equivalent invariant end-to-end.
    {
        TArray<FCRIREntryBlock> ReparsedBlocks;
        TArray<FCRIRParseError> ReparseErrors;
        const bool bReparseOk = FCRIRParser::Parse(
            Result.CRIRText,
            ReparsedBlocks,
            ReparseErrors,
            /*bSkipReferenceValidation*/false);
        if (!bReparseOk)
        {
            Result.Warnings.Add(FString::Printf(
                TEXT("CRIR_SELF_CHECK_FAILED: emitted text does not re-parse: %s"),
                *JoinCRIRParseErrors(ReparseErrors)));
        }
    }
#endif

    Result.bSuccess = true;
    return Result;
}
