// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraParameterRenameUtils.h"

#include "Handlers/Niagara/NiagaraJsonHelpers.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "ScopedTransaction.h"

namespace PinWrightNiagara
{
    namespace
    {
        void SplitLeadingNamespace(const FString& FullName, FString& OutNamespace)
        {
            FString LocalName;
            if (!FullName.Split(TEXT("."), &OutNamespace, &LocalName))
            {
                OutNamespace = FullName;
            }
        }

        bool FindExposedParameterByName(UNiagaraSystem& System, const FString& Name, FNiagaraVariable& OutVariable)
        {
            const FNiagaraParameterStore& UserStore = System.GetExposedParameters();
            TArrayView<const FNiagaraVariableWithOffset> Params = UserStore.ReadParameterVariables();
            for (const FNiagaraVariableWithOffset& Entry : Params)
            {
                if (Entry.GetName().ToString().Equals(Name, ESearchCase::IgnoreCase))
                {
                    OutVariable = FNiagaraVariable(Entry.GetType(), Entry.GetName());
                    return true;
                }
            }
            return false;
        }

        bool ExposedParameterNameExists(UNiagaraSystem& System, const FString& Name)
        {
            FNiagaraVariable Ignored;
            return FindExposedParameterByName(System, Name, Ignored);
        }

        void CollectSystemGraphs(UNiagaraSystem& System, TArray<UNiagaraGraph*>& OutGraphs)
        {
            auto AddGraphFromScript = [&OutGraphs](UNiagaraScript* Script)
            {
                if (UNiagaraGraph* Graph = NiagaraJsonHelpers::GetGraphFromScript(Script))
                {
                    OutGraphs.AddUnique(Graph);
                }
            };

            AddGraphFromScript(System.GetSystemSpawnScript());
            AddGraphFromScript(System.GetSystemUpdateScript());

            for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
            {
                const FVersionedNiagaraEmitter Versioned = Handle.GetInstance();
                const FVersionedNiagaraEmitterData* EmitterData = Versioned.GetEmitterData();
                if (!EmitterData)
                {
                    continue;
                }

                TArray<UNiagaraScript*> Scripts;
                EmitterData->GetScripts(Scripts, false, false);
                for (UNiagaraScript* Script : Scripts)
                {
                    AddGraphFromScript(Script);
                }
            }
        }

        int32 RenameAssignmentTargets(const TArray<UNiagaraGraph*>& Graphs, FName OldName, FName NewName)
        {
            int32 Renamed = 0;
            for (UNiagaraGraph* Graph : Graphs)
            {
                if (!Graph)
                {
                    continue;
                }

                TArray<UNiagaraNodeAssignment*> AssignmentNodes;
                Graph->GetNodesOfClass<UNiagaraNodeAssignment>(AssignmentNodes);
                for (UNiagaraNodeAssignment* AssignmentNode : AssignmentNodes)
                {
                    if (AssignmentNode && AssignmentNode->RenameAssignmentTarget(OldName, NewName))
                    {
                        AssignmentNode->RefreshFromExternalChanges();
                        ++Renamed;
                    }
                }
            }
            return Renamed;
        }
    }

    bool RenameNiagaraParameterWithExportedApis(
        UNiagaraSystem& System,
        const FString& OldName,
        const FString& NewName,
        FNiagaraParameterRenameResult& OutResult,
        FString& OutErrorCode,
        FString& OutErrorMessage)
    {
        OutResult = FNiagaraParameterRenameResult();
        OutErrorCode.Empty();
        OutErrorMessage.Empty();

        FString OldNamespace;
        FString NewNamespace;
        SplitLeadingNamespace(OldName, OldNamespace);
        SplitLeadingNamespace(NewName, NewNamespace);
        if (!OldNamespace.Equals(NewNamespace, ESearchCase::IgnoreCase))
        {
            OutErrorCode = TEXT("NAMESPACE_CHANGE_UNSUPPORTED");
            OutErrorMessage = FString::Printf(
                TEXT("Cross-namespace renames are not supported in v1. oldName namespace '%s' differs from newName namespace '%s'."),
                *OldNamespace,
                *NewNamespace);
            return false;
        }

        FNiagaraVariable OldVariable;
        if (!FindExposedParameterByName(System, OldName, OldVariable))
        {
            OutErrorCode = TEXT("PARAMETER_NOT_FOUND");
            OutErrorMessage = FString::Printf(
                TEXT("Parameter '%s' was not found in the system exposed parameter store."),
                *OldName);
            return false;
        }

        if (ExposedParameterNameExists(System, NewName))
        {
            OutErrorCode = TEXT("PARAMETER_NAME_COLLISION");
            OutErrorMessage = FString::Printf(
                TEXT("A parameter named '%s' already exists in the user store."),
                *NewName);
            return false;
        }

        const FName NewFName(*NewName);
        const FNiagaraVariable NewVariable(OldVariable.GetType(), NewFName);

        TArray<UNiagaraGraph*> Graphs;
        CollectSystemGraphs(System, Graphs);

        OutResult.OldVariable = OldVariable;
        OutResult.NewVariable = NewVariable;
        OutResult.GraphsVisited = Graphs.Num();

        FScopedTransaction Transaction(FText::FromString(TEXT("MCP: niagara.rename_parameter")));
        System.Modify();

        // Match FNiagaraSystemViewModel::RenameParameter order: rename the user-script metadata
        // BEFORE the exposed-parameter store. The store's rename fires OnChanged →
        // UNiagaraSystemEditorData::SyncUserScriptVariables, which uses FindOrAddUserScriptVariable
        // keyed on the new name; if the metadata entry is still keyed on the old name at that point,
        // Sync creates a new entry for the new name and removes the old one — leaving our subsequent
        // RenameUserScriptVariable(OldVariable, ...) with nothing to match.
        if (UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System.GetEditorData()))
        {
            EditorData->Modify();
            OutResult.bUserScriptMetadataRenamed = EditorData->RenameUserScriptVariable(OldVariable, NewFName);
        }

        FNiagaraParameterStore& UserStore = System.GetExposedParameters();
        if (UserStore.IndexOf(OldVariable) != INDEX_NONE)
        {
            UserStore.RenameParameter(OldVariable, NewFName);
            OutResult.bUserStoreRenamed =
                UserStore.IndexOf(NewVariable) != INDEX_NONE &&
                UserStore.IndexOf(OldVariable) == INDEX_NONE;
        }

        OutResult.AssignmentTargetsRenamed = RenameAssignmentTargets(Graphs, OldVariable.GetName(), NewFName);

        for (UNiagaraGraph* Graph : Graphs)
        {
            if (Graph && Graph->RenameParameter(OldVariable, NewFName))
            {
                ++OutResult.GraphsRenamed;
            }
        }

        if (OutResult.bUserStoreRenamed ||
            OutResult.bUserScriptMetadataRenamed ||
            OutResult.GraphsRenamed > 0 ||
            OutResult.AssignmentTargetsRenamed > 0)
        {
            System.HandleVariableRenamed(OldVariable, NewVariable, true);
            OutResult.bSystemRenameHookInvoked = true;
            return true;
        }

        OutErrorCode = TEXT("PARAMETER_NOT_FOUND");
        OutErrorMessage = FString::Printf(
            TEXT("No exported Niagara rename path reported a change for '%s'. The parameter may have been removed concurrently."),
            *OldName);
        return false;
    }
}
