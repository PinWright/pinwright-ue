// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "K2Node.h"
#include "EdGraphSchema_K2.h"
#include "TestBpirRootSetEntryNoSignature.generated.h"

// Impure UK2Node with an exec output and no input pins at all — the shape clause of
// IsEngineCompileRootSetNode, which is how AnimGraph roots and other engine-root nodes
// reach the BPIR decompiler as "entry points" while the grammar has no signature for
// them. Synthetic on purpose: it reproduces the shape without depending on a plugin
// module (EnhancedInput, AnimGraph) being enabled on the host.
UCLASS()
class UTestBpirRootSetOnlyNode : public UK2Node
{
    GENERATED_BODY()

public:
    virtual void AllocateDefaultPins() override
    {
        CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
    }

    virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override
    {
        return FText::FromString(TEXT("TestBpirRootSetOnlyNode"));
    }
};
