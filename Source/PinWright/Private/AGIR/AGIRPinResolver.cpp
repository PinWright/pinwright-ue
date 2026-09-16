// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AGIR/AGIRPinResolver.h"


#include "AnimGraphNode_Base.h"
#include "AnimationGraphSchema.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"

bool FAGIRPinResolver::ParseReference(FStringView Ref, FAGIRPinReference& Out)
{
    Out = FAGIRPinReference();

    Ref = Ref.TrimStartAndEnd();
    if (Ref.IsEmpty() || Ref[0] != TEXT('%'))
    {
        return false;
    }

    FStringView Body = Ref.RightChop(1);
    if (Body.IsEmpty())
    {
        return false;
    }

    Out.SymbolName = FString(Body);

    // Numeric form: `%nNN` (lowercase 'n' followed by digits). FCString::IsNumeric
    // accepts an optional sign and one decimal point — neither appears in
    // emitter output for AGIR-local ids, so a positive parse below covers the
    // strict-unsigned-integer contract.
    if (Body.Len() >= 2 && (Body[0] == TEXT('n') || Body[0] == TEXT('N')))
    {
        const FString DigitsStr(Body.RightChop(1));
        if (!DigitsStr.IsEmpty() && FCString::IsNumeric(*DigitsStr))
        {
            int32 Parsed = INDEX_NONE;
            if (LexTryParseString(Parsed, *DigitsStr) && Parsed >= 0)
            {
                Out.bIsNumeric = true;
                Out.NumericId = Parsed;
                return true;
            }
        }
    }

    // Symbolic form: any non-empty token after `%`.
    return true;
}

UEdGraphNode* FAGIRPinResolver::ResolveReference(
    const FAGIRPinReference& Ref,
    const TMap<FString, UEdGraphNode*>& Symbols)
{
    return LookupSymbol(Symbols, Ref.SymbolName);
}

FAGIRPinResolver::FWireResult FAGIRPinResolver::WirePoseInput(
    UAnimGraphNode_Base* DownstreamNode,
    FName InputPinName,
    UAnimGraphNode_Base* UpstreamNode,
    FName OutputPinName)
{
    if (!DownstreamNode || !UpstreamNode)
    {
        return MakeError(TEXT("AGIR_SYMBOL_NOT_FOUND"), TEXT("Upstream or downstream pose node is missing."));
    }

    UEdGraphPin* InPin = DownstreamNode->FindPin(InputPinName);
    UEdGraphPin* OutPin = UpstreamNode->FindPin(OutputPinName);
    if (!InPin || !OutPin)
    {
        return MakeError(TEXT("AGIR_INVALID_PIN_REFERENCE"), TEXT("Pose input or output pin was not found."));
    }

    // Pose-link compatibility check: both endpoints must use the anim-graph
    // pose pin category (UAnimationGraphSchema::PC_Struct with FPoseLink /
    // FComponentSpacePoseLink subcategory). The schema's CanCreateConnection
    // already enforces this; surface as a typed error code on failure.
    const UEdGraphSchema* Schema = DownstreamNode->GetSchema();
    if (!Schema)
    {
        return MakeError(TEXT("AGIR_INVALID_PIN_REFERENCE"), TEXT("Downstream node has no schema."));
    }

    const FPinConnectionResponse Response = Schema->CanCreateConnection(OutPin, InPin);
    if (Response.Response == CONNECT_RESPONSE_DISALLOW)
    {
        return MakeError(TEXT("AGIR_POSE_TYPE_MISMATCH"), TEXT("Pose pins are not connectable."));
    }

    // Schema accepted the pair (above), but the actual wire-up still failed.
    // This is a different signal from a type mismatch: it means the schema
    // would have allowed the connection but something inside `TryCreateConnection`
    // (e.g. break-existing-link logic, transactional rollback) refused.
    if (!Schema->TryCreateConnection(OutPin, InPin))
    {
        return MakeError(TEXT("AGIR_POSE_CONNECT_FAILED"), TEXT("Pose connection was rejected by the schema."));
    }

    return FWireResult();
}
