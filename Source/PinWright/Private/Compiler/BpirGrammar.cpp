// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Compiler/BpirGrammar.h"
#include "Compiler/BpirSharedConstants.h"
#include "Compiler/BpirTypes.h"
#include "IrCore/IIrGrammar.h"

namespace
{
class FBpirGrammar final : public IIrGrammar
{
public:
    virtual bool IsKeyword(const FString& Word) const override
    {
        int32 IgnoredOpcode = 0;
        return TryGetOpcode(Word, IgnoredOpcode) || GetSyntaxKeywords().Contains(Word.ToLower());
    }

    virtual bool TryGetOpcode(const FString& Keyword, int32& OutOpcode) const override
    {
        if (const EBpirOpcode* Opcode = GetOpcodeKeywords().Find(Keyword.ToLower()))
        {
            OutOpcode = static_cast<int32>(*Opcode);
            return true;
        }

        return false;
    }

private:
    static const TMap<FString, EBpirOpcode>& GetOpcodeKeywords()
    {
        static const TMap<FString, EBpirOpcode> Keywords = {
            { TEXT("call"), EBpirOpcode::Call },
            // `message` shares the Call opcode; the parser sets FBpirInstruction::bInterfaceMessage
            // so the emitter builds UK2Node_Message instead of UK2Node_CallFunction.
            { BpirSharedConstants::Keywords::Message, EBpirOpcode::Call },
            // `parent_call` shares the Call opcode; the parser sets
            // FBpirInstruction::bParentCall so the emitter builds
            // UK2Node_CallParentFunction instead of UK2Node_CallFunction.
            { BpirSharedConstants::Keywords::ParentCall, EBpirOpcode::Call },
            { TEXT("pure"), EBpirOpcode::Pure },
            { TEXT("latent"), EBpirOpcode::Latent },
            { TEXT("set"), EBpirOpcode::Set },
            { TEXT("get"), EBpirOpcode::Get },
            { TEXT("return"), EBpirOpcode::Return },
            { BpirSharedConstants::Keywords::End, EBpirOpcode::End },
            { TEXT("branch"), EBpirOpcode::Branch },
            { TEXT("foreach"), EBpirOpcode::Foreach },
            { TEXT("foreach_break"), EBpirOpcode::ForeachBreak },
            { TEXT("while"), EBpirOpcode::While },
            { TEXT("switch"), EBpirOpcode::Switch },
            { TEXT("sequence"), EBpirOpcode::Sequence },
            { TEXT("cast"), EBpirOpcode::Cast },
            { TEXT("select"), EBpirOpcode::Select },
            { TEXT("macro"), EBpirOpcode::Macro },
            { TEXT("timeline"), EBpirOpcode::Timeline },
            { TEXT("break"), EBpirOpcode::BreakStruct },
            { TEXT("make"), EBpirOpcode::MakeStruct },
            { TEXT("make_array"), EBpirOpcode::MakeArray },
            { TEXT("self"), EBpirOpcode::Self },
            { TEXT("enum"), EBpirOpcode::Enum },
            { TEXT("exec"), EBpirOpcode::ExecGoto },
            { TEXT("call_dispatcher"), EBpirOpcode::CallDispatcher },
            { TEXT("bind_dispatcher"), EBpirOpcode::BindDispatcher },
            { TEXT("unbind_dispatcher"), EBpirOpcode::UnbindDispatcher },
            { TEXT("clear_dispatcher"), EBpirOpcode::ClearDispatcher },
            { TEXT("switch_int"), EBpirOpcode::SwitchInt },
            { TEXT("switch_string"), EBpirOpcode::SwitchString },
            { TEXT("switch_enum"), EBpirOpcode::SwitchEnum },
            { TEXT("field_notify_subscribe"), EBpirOpcode::FieldNotifySubscribe },
            { TEXT("field_notify_unsubscribe"), EBpirOpcode::FieldNotifyUnsubscribe },
            { TEXT("subsystem"), EBpirOpcode::Subsystem },
            { TEXT("alias"), EBpirOpcode::Alias },
        };
        return Keywords;
    }

    static const TSet<FString>& GetSyntaxKeywords()
    {
        static const TSet<FString> Keywords = {
            TEXT("entry"),
        };
        return Keywords;
    }
};
}

const IIrGrammar& GetBpirGrammar()
{
    static const FBpirGrammar Grammar;
    return Grammar;
}
