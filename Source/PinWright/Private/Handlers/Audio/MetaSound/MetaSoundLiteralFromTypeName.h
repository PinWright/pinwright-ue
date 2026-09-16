// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared helpers for turning an MCP-facing MetaSound type name / value parameter into
// an FMetasoundFrontendLiteral that FMetasoundFrontendClassInput::InitDefault,
// FMetaSoundFrontendDocumentBuilder::SetGraphInputDefault,
// FMetaSoundFrontendDocumentBuilder::SetGraphVariableDefault and
// FMetaSoundFrontendDocumentBuilder::SetNodeInputDefault can accept.
//
// Lives in MetaSound/ to keep all MetaSound-specific glue colocated. Used by:
//   - audio.authoring.add_metasound_input            (AudioAuthoringHandler.cpp)
//   - audio.authoring.set_metasound_default          (AudioAuthoringHandler.cpp)
//   - audio.authoring.add_metasound_variable         (MetaSoundVariableHandler.cpp)
//   - audio.authoring.set_metasound_variable_default (MetaSoundVariableHandler.cpp)
//   - audio.authoring.set_metasound_node_input_default (MetaSoundNodeInputDefaultHandler.cpp)
//
// Functions are namespace-scope (not static-file-local) so tests can link
// against them — see TestAddMetaSoundInputLiteral.cpp / TestMetaSoundLiteralGaps.cpp.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

#if __has_include("MetasoundFrontendDocument.h")
#include "MetasoundFrontendDocument.h"
#define MCP_HAS_METASOUND_LITERAL_HELPER 1
#else
#define MCP_HAS_METASOUND_LITERAL_HELPER 0
#endif

// The MetaSound data-type registry is what actually decides which type names exist and
// which UObject classes a proxy-parsable type accepts. It ships in the same module as
// MetasoundFrontendDocument.h, but is probed separately so a host missing the header
// degrades to the hardcoded primitive table below instead of failing to compile.
#if MCP_HAS_METASOUND_LITERAL_HELPER && __has_include("MetasoundFrontendDataTypeRegistry.h")
#define PW_METASOUND_HAS_DATATYPE_REGISTRY 1
#else
#define PW_METASOUND_HAS_DATATYPE_REGISTRY 0
#endif

#if MCP_HAS_METASOUND_LITERAL_HELPER

namespace PinWright::MetaSound
{
    /**
     * Populate OutLiteral with a default value matching the MCP-facing TypeName.
     * Returns true on success, false if TypeName names no MetaSound data type.
     * Comparison is case-insensitive.
     *
     * Explicitly tabled names: "Float", "Int" / "Int32", "Bool" / "Boolean", "String",
     * "Audio", "Trigger", "WaveAsset", "Time". Every other name is resolved against the
     * live data-type registry (see PW_METASOUND_HAS_DATATYPE_REGISTRY), so registered
     * types the table does not name — "AudioBusAsset", "WaveTable", "Enum:*", the array
     * types — work without another edit here.
     */
    bool MakeDefaultLiteralForMetaSoundType(const FString& TypeName, FMetasoundFrontendLiteral& OutLiteral);

    /**
     * Map an MCP-facing convenience type name to the MetaSound data-type registry's
     * canonical key (what FMetaSoundFrontendDocumentBuilder resolves against), so the
     * registry FName fed to AddGraphInput/Output/Variable is one the registry knows.
     * Comparison is case-insensitive; unknown names pass through unchanged. An array
     * name canonicalizes through its element ("Int:Array" -> "Int32:Array"). See the
     * .cpp for which aliases are rewritten and why.
     */
    FString CanonicalizeMetaSoundTypeName(const FString& TypeName);

    /**
     * True when TypeName names an ARRAY data type ("Float:Array", "WaveAsset:Array").
     * Measured from the live registry's FDataTypeRegistryInfo::bIsArrayType; for a name the
     * registry does not know (or a host without the registry header) it falls back to the
     * ":Array" name suffix, which is how the engine itself derives an array type's name
     * (METASOUND_DATA_TYPE_NAME_ARRAY_TYPE_SPECIFIER). Canonicalizes first; case-insensitive.
     */
    bool IsMetaSoundArrayDataType(const FString& TypeName);

    /**
     * The canonical ELEMENT data-type name of an array type: "WaveAsset:Array" -> "WaveAsset".
     * Empty for a name that is not an array type.
     *
     * This exists because the registry does NOT relate the two, and that is what made array
     * literals unreachable. An array type is registered by RegisterDataTypeArrayWithFrontend,
     * which drops the UClassToUse template argument, so GetUClassForDataType("WaveAsset:Array")
     * is null and IsValidUObjectForDataType rejects EVERY object for it (the array entry sets
     * only bIsProxyArrayParsable, while that predicate tests bIsProxyParsable). Object entries
     * therefore have to be validated against the ELEMENT type, and only this name yields it.
     */
    FString GetMetaSoundArrayElementTypeName(const FString& ArrayTypeName);

    /**
     * Whether the live registry knows this type name (canonicalized internally).
     * Diagnostics only — callers must not gate on it, because it answers false on a
     * host without the registry header. Returns false when the registry is unavailable.
     */
    bool IsMetaSoundDataTypeRegistered(const FString& TypeName);

    /**
     * Registered data-type names containing TypeName (case-insensitive), sorted, capped
     * at MaxSuggestions. Recovery payload for an unrecognized type name (rpc-design §7:
     * an error names the way out). Empty when the registry is unavailable or nothing matches.
     */
    TArray<FString> SuggestMetaSoundDataTypeNames(const FString& TypeName, int32 MaxSuggestions = 8);

    /** Outcome of resolving an object-path parameter into a UObject-typed literal. */
    enum class EMetaSoundObjectLiteralResult : uint8
    {
        Ok,
        PathRejected,    // empty path, or one the traversal sanitizer refused
        ObjectNotFound,  // nothing loadable at that path
        WrongClass       // loaded, but the data type does not accept an object of that class
    };

    /**
     * Resolve ObjectPath into a UObject and set OutLiteral to the matching UObject literal.
     *
     * DataTypeName is the vertex's declared MetaSound data type (e.g. "WaveAsset"); pass an
     * empty string to skip class validation. Validation goes through the registry's own
     * IsValidUObjectForDataType, so a Float-typed vertex rejects any object and a WaveAsset
     * vertex rejects anything that is not a USoundWave — the plugin never hardcodes the pairing.
     *
     * OutObject is set on Ok and on WrongClass (so the caller can name what it actually found);
     * OutExpectedClassPath carries the UClass the data type wants when the registry knows one.
     * OutLiteral is written only on Ok.
     */
    EMetaSoundObjectLiteralResult MakeObjectLiteralForMetaSoundType(
        const FString& ObjectPath,
        const FString& DataTypeName,
        FMetasoundFrontendLiteral& OutLiteral,
        UObject*& OutObject,
        FString& OutExpectedClassPath);

    /** Why a JSON array could not be turned into an array literal. */
    enum class EMetaSoundArrayLiteralResult : uint8
    {
        Ok,
        NotAnArrayType,        // the target vertex/variable is not array-typed
        UnsupportedArrayShape, // the array type's literal shape has no JSON form to build from
        ElementTypeMismatch,   // an entry was not the JSON kind this array's elements need
        ElementNotFound,       // an object entry resolved to no loadable object
        ElementWrongClass      // an object entry loaded, but the element data type rejects its class
    };

    /**
     * Everything needed to name an array-literal failure precisely: which entry, what the
     * elements had to be, and what the offending entry actually was. Populated on failure only;
     * ElementTypeName is filled whenever the target really is an array type.
     */
    struct FMetaSoundArrayLiteralOutcome
    {
        EMetaSoundArrayLiteralResult Result = EMetaSoundArrayLiteralResult::Ok;
        int32 FailedIndex = INDEX_NONE;
        FString ElementTypeName;
        FString ExpectedEntryKind;   // "number" / "integer" / "boolean" / "string" / "asset path"
        FString ExpectedClassPath;   // element type's UClass, when the registry knows one
        FString FailedEntryText;     // the offending entry, rendered for the error payload
        UObject* ResolvedObject = nullptr;  // what an ElementWrongClass entry actually loaded to
    };

    /**
     * Build an ARRAY FMetasoundFrontendLiteral out of a JSON array.
     *
     * The element form is decided by the ARRAY data type's own registry literal shape
     * (GetDesiredLiteralType -> FloatArray / IntegerArray / BooleanArray / StringArray /
     * UObjectArray / NoneArray), never guessed from what the JSON happens to hold — the same
     * direction the scalar path takes, so a `Float:Array` refuses a string exactly as a `Float`
     * refuses one. Object entries are asset paths resolved and class-checked through
     * MakeObjectLiteralForMetaSoundType against the ELEMENT type, so a `WaveAsset:Array` accepts
     * USoundWaves and nothing else without this file hardcoding that pairing.
     *
     * An empty Entries array is valid and produces an empty array literal of the right type —
     * that is how a caller clears an array default.
     *
     * Returns true with OutLiteral written; on false OutLiteral is untouched and OutOutcome says
     * why. Feeds FMetaSoundFrontendDocumentBuilder::SetGraphInputDefault /
     * SetGraphVariableDefault / SetNodeInputDefault exactly as the scalar literals do.
     */
    bool MakeArrayLiteralForMetaSoundType(
        const TArray<TSharedPtr<FJsonValue>>& Entries,
        const FString& DataTypeName,
        FMetasoundFrontendLiteral& OutLiteral,
        FMetaSoundArrayLiteralOutcome& OutOutcome);

    /**
     * Write a literal's measured shape into Out: `literalType`, `value`, plus `objectPath`
     * when it holds a UObject. Single formatter so every readback in the MetaSound surface
     * reports the same field names. Array literals additionally carry `arrayNum` and, for a
     * UObject array, `objectPaths` — the readback shape an arrayValue writer needs to verify
     * its write element by element rather than against one flattened string.
     */
    void DescribeMetaSoundLiteral(const FMetasoundFrontendLiteral& Literal, const TSharedPtr<FJsonObject>& Out);
}

#endif // MCP_HAS_METASOUND_LITERAL_HELPER
