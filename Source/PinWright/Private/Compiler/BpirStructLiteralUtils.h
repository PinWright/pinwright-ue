// Copyright (c) 2026 Alexander Penkin. MIT License.

// BpirStructLiteralUtils.h - Reflective formatter for positional struct literal tokens

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class FProperty;


namespace BpirStructLiteralUtils
{
    // K2-validator-grammar registry. Detects `F<Vector|Rotator|LinearColor>(c0,...)` tokens
    // and emits the exact pin-default string K2's per-struct validator accepts
    // (`IsStringValid{Vector,Rotator,LinearColor}`). Each engine validator hardcodes a
    // different grammar, so the helper hardcodes a matching set; unknown structs return
    // false and the caller falls back to its default formatter.
    PINWRIGHT_API bool TryFormatPositionalStructLiteralAsPinText(const FString& Token, FString& OutPinText);

    // Render a JSON value (as produced by Utils/PropertyUtils.cpp::ExportPropertyToJsonValue)
    // into a BPIR-literal string suitable for the right-hand side of a `node_props { Key: Value }`
    // entry. Quoting/formatting matches the conventions used elsewhere in BPIR emit (string
    // values are double-quoted with backslash escapes; FVector/FRotator JSON arrays are emitted
    // as the positional `FVector(x,y,z)` / `FRotator(p,y,r)` sugar form; other struct values
    // arrive as already-exported `(X=...,Y=...)` strings and are passed through verbatim).
    // Prop drives the struct-shape detection; nullptr is allowed (falls back to a JSON-shape-only
    // formatting path).
    FString FormatJsonValueAsBpir(const TSharedPtr<FJsonValue>& JsonValue, FProperty* Prop);

    // Inner half of the BPIR string-literal escape: backslash then double-quote,
    // no outer quotes. Shared between EscapeBpirString and any caller that needs
    // to escape a payload before embedding it in a larger literal (e.g. NSLOCTEXT).
    FString EscapeBpirStringInner(const FString& In);

    // Quote/escape a string value for the BPIR literal grammar — backslash and
    // double-quote escaped, then wrapped in double quotes. Single source of truth
    // for the escape pattern; both decompiler and node_props emit route through here
    // so the parser sees the same shape on the round trip.
    PINWRIGHT_API FString EscapeBpirString(const FString& In);

    // Inverse of EscapeBpirString's inner transform. Reverses the backslash escapes
    // applied to a BPIR string-literal payload: \\\" → \" first, then \\\\ → \\ —
    // unescape order matters to avoid double-collapsing legitimate \\\" sequences.
    // Pure: does NOT strip outer quotes; the caller is expected to have already
    // taken the inner content (e.g. via Mid(1, Len-2)).
    FString UnescapeBpirString(const FString& In);

    // Emit the positional sugar form `F<StructName>(c0,c1,...)` for known
    // pin-default struct types (FVector, FRotator, FLinearColor). Components are
    // expected pre-stringified by the caller — the helper adds no formatting of its
    // own — which lets both the JSON-array path (FormatJsonValueAsBpir) and the
    // decompiler's `(X=...,Y=...)` extraction path share one implementation.
    // Returns an empty string when StructName is not a sugared type or when the
    // component count does not match; caller falls through to its default formatter.
    FString FormatStructComponentsAsBpir(const FString& StructName, TArrayView<const FString> Components);
}
