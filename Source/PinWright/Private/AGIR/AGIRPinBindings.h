// Copyright (c) 2026 Alexander Penkin. MIT License.

// AGIRPinBindings.h
//
// The AGIR vocabulary for anim-node *data*-pin bindings, shared by the text
// emitter (read) and every compile-side argument loop (write). Pose links are
// a separate mechanism and stay in AGIRTextEmitter / AGIRPinResolver.
//
// An anim node's non-pose input pin can be driven two ways. They are different
// mechanisms — one is a real node in the EdGraph and compiles to Blueprint VM
// code, the other is a fast-path property copy recorded on the node — so AGIR
// spells them differently and round-trips each back to its own form:
//
//   X: $Direction         graph-wired: a `UK2Node_VariableGet` for the AnimBP
//                         member `Direction`, linked into the `X` pin. Same
//                         `$Name` spelling BPIR uses for a member read, so the
//                         two IRs share one sigil for one concept.
//   Alpha: bind Speed     property-access binding: an entry in the node's
//                         `PropertyBindings` map (`FAnimGraphNodePropertyBinding`).
//                         Dotted paths (`bind Struct.Member`) are supported.
//   Alpha: bind fn GetX   the same, bound to a function rather than a property.
//
// A bound pin's literal value in the runtime `FAnimNode_*` struct is dead — the
// binding overwrites it every frame — so the read side reports the bound pin
// names back to its caller and the reflected-field pass suppresses the literal
// for them. That keeps the argument list a map (no duplicate keys) and makes
// "driven" textually distinct from "left at its default", which is exactly the
// distinction the decompiler used to erase
// (B-decompile-agir-omits-wired-data-pin-bindings).
//
// Links AGIR cannot spell (a pin fed by a function call, a math node, a getter
// on another object) are reported as warnings rather than dropped in silence.

#pragma once

#include "CoreMinimal.h"

class UAnimGraphNode_Base;

namespace AGIRPinBindings
{
// True for an argument value in one of the binding forms above (`$Name` or
// `bind ...`). Reflected literals can never take these shapes: the emitter
// quotes every string/name/struct export, and numeric, bool and enum exports
// start with a digit, sign or letter.
bool IsBindingValue(const FString& Value);

// Read side. Appends one `PinName: <binding>` entry per bound data pin of Node
// to OutFields, records the emitted pin names in OutBoundPins so the caller can
// suppress their now-meaningless reflected literal, and appends one warning per
// wired pin whose driver AGIR has no spelling for. Output is ordered by pin
// declaration order (graph-wired) then by name (property bindings) so the text
// is diff-stable.
void AppendBindingFields(
    UAnimGraphNode_Base* Node,
    TArray<FString>& OutFields,
    TSet<FName>& OutBoundPins,
    TArray<FString>& OutWarnings);

// Write side. Applies one binding-form argument to PinName on Node, exposing
// the pin first when it is currently hidden. Returns an empty string on
// success, or a diagnostic the caller surfaces as an AGIR_FIELD_WRITE warning.
FString ApplyBindingArg(UAnimGraphNode_Base* Node, FName PinName, const FString& Value);
} // namespace AGIRPinBindings
