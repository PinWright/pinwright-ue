// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "AGIR/AGIRGrammar.h"

namespace
{
FString NormalizeKeyword(const FString& Keyword)
{
    return Keyword.ToLower();
}
}

const FAGIRGrammar& FAGIRGrammar::Get()
{
    static const FAGIRGrammar Grammar;
    return Grammar;
}

bool FAGIRGrammar::IsKeyword(const FString& Word) const
{
    int32 IgnoredOpcode = 0;
    return TryGetOpcode(Word, IgnoredOpcode) || GetSyntaxKeywords().Contains(NormalizeKeyword(Word));
}

bool FAGIRGrammar::TryGetOpcode(const FString& Keyword, int32& OutOpcode) const
{
    if (const EAGIROpcode* Opcode = GetOpcodeKeywords().Find(NormalizeKeyword(Keyword)))
    {
        OutOpcode = static_cast<int32>(*Opcode);
        return true;
    }

    return false;
}

FString FAGIRGrammar::OpcodeToText(EAGIROpcode Opcode)
{
    switch (Opcode)
    {
    case EAGIROpcode::Call: return TEXT("call");
    case EAGIROpcode::StateMachine: return TEXT("state_machine");
    case EAGIROpcode::BlendSpace: return TEXT("blend_space");
    case EAGIROpcode::LayeredBlend: return TEXT("layered_blend");
    case EAGIROpcode::LinkedAnim: return TEXT("linked_anim");
    case EAGIROpcode::LinkedInputPose: return TEXT("linked_input_pose");
    case EAGIROpcode::SaveCachedPose: return TEXT("save_cached_pose");
    case EAGIROpcode::UseCachedPose: return TEXT("use_cached_pose");
    case EAGIROpcode::Output: return TEXT("output");
    case EAGIROpcode::State: return TEXT("state");
    case EAGIROpcode::Transition: return TEXT("transition");
    case EAGIROpcode::Conduit: return TEXT("conduit");
    case EAGIROpcode::StateAlias: return TEXT("state_alias");
    case EAGIROpcode::BlendSpaceSampleGraph: return TEXT("sample_graph");
    case EAGIROpcode::CustomTransitionBody: return TEXT("custom_transition_body");
    case EAGIROpcode::Implements: return TEXT("implements");
    default: return FString();
    }
}

const TMap<FString, EAGIROpcode>& FAGIRGrammar::GetOpcodeKeywords()
{
    // Surface keywords for the line-oriented parser. `state` / `transition` /
    // `conduit` / `output` are conceptually opcodes but appear inside parent
    // blocks; tokenization treats them as keywords either way.
    static const TMap<FString, EAGIROpcode> Keywords = {
        { TEXT("call"), EAGIROpcode::Call },
        { TEXT("state_machine"), EAGIROpcode::StateMachine },
        { TEXT("blend_space"), EAGIROpcode::BlendSpace },
        { TEXT("layered_blend"), EAGIROpcode::LayeredBlend },
        { TEXT("linked_anim"), EAGIROpcode::LinkedAnim },
        { TEXT("linked_input_pose"), EAGIROpcode::LinkedInputPose },
        { TEXT("save_cached_pose"), EAGIROpcode::SaveCachedPose },
        { TEXT("use_cached_pose"), EAGIROpcode::UseCachedPose },
        { TEXT("output"), EAGIROpcode::Output },
        { TEXT("state"), EAGIROpcode::State },
        { TEXT("transition"), EAGIROpcode::Transition },
        { TEXT("conduit"), EAGIROpcode::Conduit },
        { TEXT("state_alias"), EAGIROpcode::StateAlias },
        { TEXT("sample_graph"), EAGIROpcode::BlendSpaceSampleGraph },
        { TEXT("custom_transition_body"), EAGIROpcode::CustomTransitionBody },
        { TEXT("implements"), EAGIROpcode::Implements },
    };
    return Keywords;
}

const TSet<FString>& FAGIRGrammar::GetSyntaxKeywords()
{
    // Top-level entry block kinds (`anim_graph` / `anim_layer` / `anim_function`)
    // and the `rule` attribute keyword used inside transition lines. `entry` is
    // the block-introducer keyword shared with BPIR / MGIR.
    static const TSet<FString> Keywords = {
        TEXT("entry"),
        TEXT("anim_graph"),
        TEXT("anim_layer"),
        TEXT("anim_function"),
        TEXT("interfaces"),
        TEXT("rule"),
    };
    return Keywords;
}
