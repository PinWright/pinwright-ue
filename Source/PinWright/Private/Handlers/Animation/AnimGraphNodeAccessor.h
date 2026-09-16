// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// UAnimGraphNode_Base::GetFNodeProperty() / GetFNode() are protected on UE 5.3 (they were made
// public in 5.4). To keep the handler code version-agnostic, expose them through a non-UObject
// derived "accessor" that re-publishes the protected members via using-declarations. The
// static_cast is on the same object instance, so it is well-defined; the accessor type is never
// instantiated or registered with reflection.

#include "AnimGraphNode_Base.h"

namespace PinWright::Anim
{
    struct FAnimGraphNodeAccessor : public UAnimGraphNode_Base
    {
        using UAnimGraphNode_Base::GetFNodeProperty;
        using UAnimGraphNode_Base::GetFNode;
    };

    inline FStructProperty* GetFNodeProperty(const UAnimGraphNode_Base* Node)
    {
        if (!Node)
        {
            return nullptr;
        }
        return static_cast<const FAnimGraphNodeAccessor*>(Node)->GetFNodeProperty();
    }

    inline FAnimNode_Base* GetFNode(UAnimGraphNode_Base* Node)
    {
        if (!Node)
        {
            return nullptr;
        }
        return static_cast<FAnimGraphNodeAccessor*>(Node)->GetFNode();
    }
}
