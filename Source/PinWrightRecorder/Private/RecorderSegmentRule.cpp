// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "RecorderSegmentRule.h"

namespace RecorderSegmentRegistry
{
namespace
{
    // The one engine-generic rule the recorder ships with: editor open/exit,
    // correlated by editor_session_id. Hosts add their gameplay rules on top.
    FRecorderSegmentRule MakeEditorSessionRule()
    {
        FRecorderSegmentRule Rule;
        Rule.Type = TEXT("editor_session");
        Rule.Starts = { FRecorderEventMatcher{ TEXT("editor:session"), TEXT("action"), TEXT("open") } };
        Rule.Ends = { FRecorderEventMatcher{ TEXT("editor:session"), TEXT("action"), TEXT("exit") } };
        Rule.CorrelationProp = TEXT("editor_session_id");
        return Rule;
    }

    TArray<FRecorderSegmentRule>& MutableRules()
    {
        static TArray<FRecorderSegmentRule> Rules = []
        {
            TArray<FRecorderSegmentRule> R;
            R.Add(MakeEditorSessionRule());
            return R;
        }();
        return Rules;
    }
}

void Register(const FRecorderSegmentRule& Rule)
{
    MutableRules().Add(Rule);
}

const TArray<FRecorderSegmentRule>& Get()
{
    return MutableRules();
}

void ResetToDefault()
{
    TArray<FRecorderSegmentRule>& Rules = MutableRules();
    Rules.Empty();
    Rules.Add(MakeEditorSessionRule());
}
}
