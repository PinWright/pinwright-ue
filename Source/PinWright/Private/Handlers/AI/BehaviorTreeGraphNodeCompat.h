// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// On UE 5.3 the BehaviorTreeEditor graph-node UCLASSes (UBehaviorTreeGraphNode and its
// _Decorator/_Service/_Task subclasses) are declared plain UCLASS() with no *_API export macro,
// so referencing their StaticClass()/NewObject<T>/Cast<T> across the module boundary fails to
// link (GetPrivateStaticClass is not exported). UE 5.4+ exports them, so direct use is fine there.
//
// These helpers resolve the classes by reflection on 5.3 (FindObject<UClass>) and construct via
// the exported UAIGraphNode base, then static_cast. On 5.4+ they forward to the native operators.

#include "Compat/EngineVersionCompat.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "AIGraphNode.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"

namespace PinWright::BehaviorTree
{
#if UE_VERSION_OLDER_THAN(5, 4, 0)
    inline UClass* GraphNodeClass()
    {
        static UClass* Cls = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode"));
        return Cls;
    }
    inline UClass* TaskClass()
    {
        static UClass* Cls = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Task"));
        return Cls;
    }
    inline UClass* DecoratorClass()
    {
        static UClass* Cls = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Decorator"));
        return Cls;
    }
    inline UClass* ServiceClass()
    {
        static UClass* Cls = FindObject<UClass>(nullptr, TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Service"));
        return Cls;
    }

    inline UBehaviorTreeGraphNode* CastGraphNode(UObject* Object)
    {
        UClass* Cls = GraphNodeClass();
        return (Object && Cls && Object->IsA(Cls)) ? static_cast<UBehaviorTreeGraphNode*>(Object) : nullptr;
    }

    inline bool IsTask(const UObject* Object)
    {
        UClass* Cls = TaskClass();
        return Object && Cls && Object->IsA(Cls);
    }

    inline UBehaviorTreeGraphNode* NewGraphNode(UObject* Outer, UClass* Cls)
    {
        if (!Outer || !Cls)
        {
            return nullptr;
        }
        // UAIGraphNode is exported (AIGRAPH_API); construct through it, then downcast.
        UAIGraphNode* Created = NewObject<UAIGraphNode>(Outer, Cls, NAME_None, RF_Transactional);
        return static_cast<UBehaviorTreeGraphNode*>(Created);
    }

    inline UBehaviorTreeGraphNode* NewDecorator(UObject* Outer) { return NewGraphNode(Outer, DecoratorClass()); }
    inline UBehaviorTreeGraphNode* NewService(UObject* Outer)   { return NewGraphNode(Outer, ServiceClass()); }
#else
    inline UClass* GraphNodeClass() { return UBehaviorTreeGraphNode::StaticClass(); }
    inline UClass* TaskClass()      { return UBehaviorTreeGraphNode_Task::StaticClass(); }
    inline UClass* DecoratorClass() { return UBehaviorTreeGraphNode_Decorator::StaticClass(); }
    inline UClass* ServiceClass()   { return UBehaviorTreeGraphNode_Service::StaticClass(); }

    inline UBehaviorTreeGraphNode* CastGraphNode(UObject* Object) { return Cast<UBehaviorTreeGraphNode>(Object); }
    inline bool IsTask(const UObject* Object) { return Object && Object->IsA(UBehaviorTreeGraphNode_Task::StaticClass()); }
    inline UBehaviorTreeGraphNode* NewDecorator(UObject* Outer) { return NewObject<UBehaviorTreeGraphNode_Decorator>(Outer, NAME_None, RF_Transactional); }
    inline UBehaviorTreeGraphNode* NewService(UObject* Outer)   { return NewObject<UBehaviorTreeGraphNode_Service>(Outer, NAME_None, RF_Transactional); }
#endif

    // Generic node construction for template helpers. Defaults to NewObject<T> (valid for the
    // exported graph-node types on every version). On 5.3 the unexported UBehaviorTreeGraphNode_Task
    // is specialized below to construct by reflection so its StaticClass symbol is never referenced.
    template <typename GraphNodeT>
    GraphNodeT* NewNode(UObject* Outer)
    {
        return NewObject<GraphNodeT>(Outer, NAME_None, RF_Transactional);
    }

#if UE_VERSION_OLDER_THAN(5, 4, 0)
    template <>
    inline UBehaviorTreeGraphNode_Task* NewNode<UBehaviorTreeGraphNode_Task>(UObject* Outer)
    {
        return static_cast<UBehaviorTreeGraphNode_Task*>(NewGraphNode(Outer, TaskClass()));
    }
#endif
}
