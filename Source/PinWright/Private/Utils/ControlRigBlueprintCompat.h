// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "Compat/EngineVersionCompat.h"

#if __has_include("ControlRigBlueprint.h")
#include "ControlRigBlueprint.h"
#elif __has_include("ControlRigBlueprintLegacy.h")
#include "ControlRigBlueprintLegacy.h"
#else
#error "Missing ControlRigBlueprint header."
#endif

#include "EdGraph/RigVMEdGraph.h"

// UE 5.7 introduced the RigVM editor asset interface as FRigVMAssetInterfacePtr; UE 5.8 renamed it
// to FRigVMEditorAssetInterfacePtr. Before 5.7 there is no such interface at all: the blueprint
// itself carries the refresh event, the ed-graph list and the ed-graph binding, so the alias names
// the blueprint pointer there. CRIR names the pointer through this alias so the call sites do not
// have to repeat the version test.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
using FPwRigVMEditorAssetInterfacePtr = FRigVMEditorAssetInterfacePtr;
#elif UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
using FPwRigVMEditorAssetInterfacePtr = FRigVMAssetInterfacePtr;
#else
using FPwRigVMEditorAssetInterfacePtr = URigVMBlueprint*;
#endif

// The object CRIR drives its editor-notification and graph-rebinding work through. 5.7+ routes
// both through the RigVM asset interface; earlier engines expose them on the blueprint directly.
inline FPwRigVMEditorAssetInterfacePtr GetControlRigEditorAsset(UControlRigBlueprint* Blueprint)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    return Blueprint ? Blueprint->GetRigVMAssetInterface() : FPwRigVMEditorAssetInterfacePtr();
#else
    return Blueprint;
#endif
}

// IRigVMAssetInterface::GetAllEdGraphs arrived with the interface in 5.7. The pre-5.7 blueprint
// answers the same question through UBlueprint::GetAllGraphs, which is what the 5.7 legacy
// blueprint's own override forwards to.
inline void GetControlRigEdGraphs(
    FPwRigVMEditorAssetInterfacePtr Asset, TArray<UEdGraph*>& OutEdGraphs)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    Asset->GetAllEdGraphs(OutEdGraphs);
#else
    Asset->GetAllGraphs(OutEdGraphs);
#endif
}

// URigVMEdGraph::InitializeFromAsset replaced InitializeFromBlueprint in 5.7, which deprecated the
// older spelling. Both rebind the ed-graph to its owning rig.
inline void InitializeRigVMEdGraphFromAsset(
    URigVMEdGraph* EdGraph, FPwRigVMEditorAssetInterfacePtr Asset)
{
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
    EdGraph->InitializeFromAsset(Asset);
#else
    EdGraph->InitializeFromBlueprint(Asset);
#endif
}

// UControlRigBlueprint::GetHierarchy() was added in UE 5.4. On 5.3 the URigHierarchy is reached
// through the public Hierarchy member. Use this accessor so CRIR call sites stay version-agnostic.
inline URigHierarchy* GetControlRigHierarchy(UControlRigBlueprint* Blueprint)
{
    if (!Blueprint)
    {
        return nullptr;
    }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    return Blueprint->GetHierarchy();
#else
    return Blueprint->Hierarchy;
#endif
}

inline FRigVMClient* GetControlRigRigVMClient(URigVMBlueprint* Blueprint)
{
    return Blueprint ? Blueprint->GetRigVMClient() : nullptr;
}

inline FRigVMClient* GetControlRigRigVMClient(UControlRigBlueprint* Blueprint)
{
    return Blueprint ? static_cast<URigVMBlueprint*>(Blueprint)->GetRigVMClient() : nullptr;
}

inline bool ModifyControlRigBlueprint(UControlRigBlueprint* Blueprint)
{
    return Blueprint ? static_cast<UBlueprint*>(Blueprint)->Modify() : false;
}
