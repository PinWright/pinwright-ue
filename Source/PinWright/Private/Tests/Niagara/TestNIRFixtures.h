// Copyright (c) 2026 Alexander Penkin. MIT License.

// Live-asset test fixtures for NIR v1b (override-chain) and v1c (script-graph) tests.
//
// SVM-backed fixture pattern (mirrors TestAGIRFixtures.h). v1a's TestNIRDecompiler.cpp
// head comment notes that bare NewObject<UNiagaraSystem>(GetTransientPackage()) does NOT
// run the editor pipeline that override-chain walking and script-graph emission depend
// on — the transient package construction skips PostLoad GraphSource wiring, leaves
// VersionData empty (FNiagaraSystemViewModel::Initialize then crashes inside
// UNiagaraScript::ComputeVMCompilationId on VersionData[0] OOB), and never sets a
// MessageAssetKey (FNiagaraMessageManager::SubscribeToAssetMessagesByObject's
// `MessageAssetKey != FGuid()` checkf fires).
//
// The honest workaround used by NiagaraEditTestUtils::NewTransientSystem is to
// duplicate a real saved system asset into a transient package — that gives us
// PostLoad-baked GraphSource, VersionData, UNiagaraScriptSource on every emitter
// script, and a real MessageAssetKey from the source asset save. We reuse the same
// approach here so the live editor pipeline (FNiagaraSystemViewModel, the stack-edit
// utilities, override-pin walks) all work end-to-end.
//
// Wave-2 (override-chain) and Wave-4 (script-graph) tests use these helpers to build
// real authored systems with literal / linked / dynamic-input / static-switch inputs
// and small dataflow / control / utility graphs, then assert that NIR emission
// produces the expected text.
#pragma once

#include "CoreMinimal.h"

#include "NiagaraTypes.h"

enum class ENiagaraScriptUsage : uint8;

class UNiagaraNodeFunctionCall;
class UNiagaraScript;
class UNiagaraSystem;

namespace NIRTestFixtures
{
    // Duplicate the small saved fixture system into a transient package and wipe its
    // pre-authored emitter handles + user parameters, then attach a single fresh
    // emitter handle so subsequent AddModuleToStack calls have a stack to land in.
    //
    // SystemName names the emitter handle within the constructed system. The system
    // asset name itself is GUID-suffixed by NewTransientSystem::MakeAssetName regardless
    // of this parameter, so multiple fixtures coexist safely even with identical
    // SystemName values.
    //
    // Returns nullptr if the source fixture asset cannot be loaded (e.g. test cooked
    // build missing the seed asset). DestroyFixture must run before the test exits,
    // or the system leaks until module shutdown.
    UNiagaraSystem* BuildEmptySystemWithEmitter(FName SystemName);

    // Add a module call to the named usage stack on the first emitter (or system-scope
    // when Usage is SystemSpawn/SystemUpdate). Routes through
    // FNiagaraStackGraphUtilities::AddScriptModuleToStack — the same path the
    // niagara.edit AddModule handler uses, which is what gives us the authored stack
    // graph that override-chain walks and graph-walk emission expect.
    //
    // Returns the created module node (a UNiagaraNodeFunctionCall positioned in the
    // stack); nullptr on failure (no matching output node, ModuleScript unloadable).
    UNiagaraNodeFunctionCall* AddModuleToStack(
        UNiagaraSystem* System,
        ENiagaraScriptUsage Usage,
        UNiagaraScript* ModuleScript);

    // Set the named module input to a literal FNiagaraVariable. Routes through
    // FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin so the
    // override-node and override-pin scaffolding the v1b walker reads is created
    // by the same editor pipeline that the niagara.edit SetModuleInput handler uses.
    void SetModuleInputLiteral(
        UNiagaraNodeFunctionCall* ModuleNode,
        FName InputName,
        FNiagaraVariable Value);

    // Bind the named module input to a linked parameter handle (e.g. "User.Speed",
    // "Engine.Time"). The override pin is created as in the literal path; the
    // linked-parameter wiring is established by inserting a UNiagaraNodeInput in
    // the override chain that reads ParameterHandle.
    //
    // The override-pin type defaults to FNiagaraTypeDefinition::GetFloatDef(). Callers
    // needing a different link type must rebuild the override pin via
    // FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin with the
    // correct type after this helper returns.
    void SetModuleInputLinkedParam(
        UNiagaraNodeFunctionCall* ModuleNode,
        FName InputName,
        FName ParameterHandle);

    // Bind the named module input to a dynamic-input module: another
    // UNiagaraNodeFunctionCall in the override chain whose FunctionScript is
    // DynamicInputScript (a Niagara DynamicInput-usage script). Returns the inner
    // function-call node so callers can recursively configure its own inputs to
    // build multi-level chains (used by recursion-depth tests).
    //
    // The override-pin type defaults to FNiagaraTypeDefinition::GetFloatDef(). Callers
    // needing a different override-pin type must rebuild the pin after this helper
    // returns.
    UNiagaraNodeFunctionCall* SetModuleInputDynamicInput(
        UNiagaraNodeFunctionCall* ModuleNode,
        FName InputName,
        UNiagaraScript* DynamicInputScript);

    // Drop any live UNiagaraSystemInstance previews wired to this system, then let
    // RemoveFromRoot expose the system to GC. KillSystemInstances must run before
    // GC reclaims the system to avoid leaked preview SimCache references in
    // subsequent tests' SVM construction (see PinWrightNiagara::KillSystemInstances).
    void DestroyFixture(UNiagaraSystem* System);
}
