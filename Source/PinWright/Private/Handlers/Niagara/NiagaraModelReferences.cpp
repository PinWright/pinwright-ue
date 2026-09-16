// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraModelBuilder.h"

#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Niagara/NiagaraJsonHelpers.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "Utils/PropertyUtils.h"
#include "Utils/PropertyInspection.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScriptSource.h"
#include "NiagaraTypes.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UnrealType.h"

namespace
{
    using namespace NiagaraJsonHelpers;

    TSharedPtr<FJsonObject> BuildDefaultReferenceProvenance()
    {
        // niagara_emitters.json was removed from asset.dump output
        // (see F-remove-niagara-json-sidecars). Provenance now lists only the sidecars
        // that are still emitted.
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("rawGraphFile"), DumpFileNames::NiagaraGraphs);
        TArray<TSharedPtr<FJsonValue>> RawDumpFiles;
        RawDumpFiles.Add(MakeStringValue(DumpFileNames::NiagaraParameters));
        RawDumpFiles.Add(MakeStringValue(DumpFileNames::NiagaraGraphs));
        Obj->SetArrayField(TEXT("rawDumpFiles"), RawDumpFiles);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildVariableModel(const FNiagaraVariableBase& Variable)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("name"), Variable.GetName().ToString());
        Obj->SetObjectField(TEXT("type"), BuildTypeModel(Variable.GetType()));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildGraphProvenance(const UNiagaraGraph* Graph, const UEdGraphNode* Node)
    {
        TSharedPtr<FJsonObject> Obj = BuildDefaultReferenceProvenance();
        TSharedPtr<FJsonObject> GraphObj = MakeObject();
        GraphObj->SetBoolField(TEXT("present"), Graph != nullptr);
        if (Graph)
        {
            GraphObj->SetStringField(TEXT("name"), Graph->GetName());
            GraphObj->SetStringField(TEXT("objectPath"), Graph->GetPathName());
        }
        if (Node)
        {
            GraphObj->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString());
            GraphObj->SetStringField(TEXT("nodeName"), Node->GetName());
        }
        Obj->SetObjectField(TEXT("graph"), GraphObj);
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildPinModel(const UEdGraphPin* Pin)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        if (!Pin)
        {
            Obj->SetBoolField(TEXT("present"), false);
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("pinId"), Pin->PinId.ToString());
        Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
        Obj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        Obj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
        Obj->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
        Obj->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
        Obj->SetStringField(TEXT("defaultObject"), GetObjectPathSafe(Pin->DefaultObject));
        Obj->SetBoolField(TEXT("orphaned"), Pin->bOrphanedPin);

        TArray<TSharedPtr<FJsonValue>> Linked;
        for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
        {
            TSharedPtr<FJsonObject> Link = MakeObject();
            Link->SetStringField(TEXT("pinId"), LinkedPin ? LinkedPin->PinId.ToString() : FString());
            Link->SetStringField(TEXT("pinName"), LinkedPin ? LinkedPin->PinName.ToString() : FString());
            const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
            Link->SetStringField(TEXT("nodeGuid"), LinkedNode ? LinkedNode->NodeGuid.ToString() : FString());
            Link->SetStringField(TEXT("nodeName"), LinkedNode ? LinkedNode->GetName() : FString());
            Link->SetStringField(TEXT("nodeClass"), GetClassPathSafe(LinkedNode));
            Linked.Add(MakeObjectValue(Link));
        }
        Obj->SetArrayField(TEXT("linkedTo"), Linked);
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildPinArray(const UEdGraphNode* Node)
    {
        TArray<TSharedPtr<FJsonValue>> Pins;
        if (!Node)
        {
            return Pins;
        }
        for (const UEdGraphPin* Pin : Node->GetAllPins())
        {
            Pins.Add(MakeObjectValue(BuildPinModel(Pin)));
        }
        return Pins;
    }

    TSharedPtr<FJsonObject> BuildBindingModel(const FNiagaraVariableAttributeBinding* Binding, const UObject* Owner)
    {
        if (!Binding)
        {
            return NiagaraModelBuilder::BuildStructuredReference(
                TEXT("renderer_binding_not_lowered"),
                TEXT("missing renderer binding"),
                nullptr,
                Owner,
                TEXT("renderer"));
        }

        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("kind"), TEXT("renderer_binding"));
        Obj->SetStringField(TEXT("displayName"), Binding->GetName().ToString());
        Obj->SetBoolField(TEXT("valid"), Binding->IsValid());
        Obj->SetBoolField(TEXT("existsOnSource"), Binding->DoesBindingExistOnSource());
        Obj->SetBoolField(TEXT("particleBinding"), Binding->IsParticleBinding());
        Obj->SetBoolField(TEXT("canBindToHostParameterMap"), Binding->CanBindToHostParameterMap());
        Obj->SetStringField(TEXT("bindingSourceMode"), EnumToString(Binding->GetBindingSourceMode()));
        Obj->SetObjectField(TEXT("paramMapVariable"), BuildVariableModel(Binding->GetParamMapBindableVariable()));
        Obj->SetObjectField(TEXT("dataSetVariable"), BuildVariableModel(Binding->GetDataSetBindableVariable()));
        Obj->SetObjectField(TEXT("provenance"), BuildDefaultReferenceProvenance());
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildRendererBindingArray(const UNiagaraRendererProperties* Renderer)
    {
        TArray<TSharedPtr<FJsonValue>> Bindings;
        if (!Renderer)
        {
            return Bindings;
        }
        for (const FNiagaraVariableAttributeBinding* Binding : Renderer->GetAttributeBindings())
        {
            Bindings.Add(MakeObjectValue(BuildBindingModel(Binding, Renderer)));
        }
        return Bindings;
    }

    TArray<TSharedPtr<FJsonValue>> BuildUsedMaterialRefs(const UNiagaraRendererProperties* Renderer)
    {
        TArray<TSharedPtr<FJsonValue>> Materials;
        if (!Renderer)
        {
            return Materials;
        }

        TArray<UMaterialInterface*> UsedMaterials;
        Renderer->GetUsedMaterials(nullptr, UsedMaterials);
        for (int32 Index = 0; Index < UsedMaterials.Num(); ++Index)
        {
            Materials.Add(MakeObjectValue(NiagaraModelBuilder::BuildStructuredReference(
                TEXT("renderer_material"),
                UsedMaterials[Index] ? UsedMaterials[Index]->GetName() : FString(),
                UsedMaterials[Index],
                Renderer,
                TEXT("renderer"))));
        }
        return Materials;
    }

    TSharedPtr<FJsonObject> BuildMaterialParameterPayload(const UNiagaraRendererProperties* Renderer)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetBoolField(TEXT("present"), false);
        if (!Renderer || !Renderer->GetClass())
        {
            return Obj;
        }

        if (FProperty* MaterialParametersProperty = Renderer->GetClass()->FindPropertyByName(FName(TEXT("MaterialParameters"))))
        {
            Obj->SetBoolField(TEXT("present"), true);
            Obj->SetStringField(TEXT("property"), MaterialParametersProperty->GetName());
            Obj->SetStringField(TEXT("type"), GetPropertyCppTypeWithParams(MaterialParametersProperty));
            if (TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(const_cast<UNiagaraRendererProperties*>(Renderer), MaterialParametersProperty))
            {
                Obj->SetField(TEXT("value"), Value);
            }
        }
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildRendererReferenceJson(const UNiagaraRendererProperties* Renderer, int32 Index)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetNumberField(TEXT("index"), Index);
        if (!Renderer)
        {
            Obj->SetBoolField(TEXT("present"), false);
            Obj->SetObjectField(TEXT("reference"), NiagaraModelBuilder::BuildStructuredReference(
                TEXT("renderer_binding_not_lowered"),
                TEXT("missing renderer"),
                nullptr,
                nullptr,
                TEXT("emitter")));
            return Obj;
        }

        Obj->SetBoolField(TEXT("present"), true);
        Obj->SetStringField(TEXT("name"), Renderer->GetName());
        Obj->SetStringField(TEXT("displayName"), Renderer->GetName());
        Obj->SetStringField(TEXT("class"), GetClassPathSafe(Renderer));
        Obj->SetStringField(TEXT("objectPath"), Renderer->GetPathName());
        Obj->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());
        Obj->SetStringField(TEXT("widgetDisplayName"), Renderer->GetWidgetDisplayName().ToString());
        Obj->SetObjectField(TEXT("properties"), BuildSparsePropertyDiffJson(
            const_cast<UNiagaraRendererProperties*>(Renderer),
            Renderer->GetClass() ? Renderer->GetClass()->GetDefaultObject() : nullptr));
        Obj->SetArrayField(TEXT("bindings"), BuildRendererBindingArray(Renderer));
        Obj->SetObjectField(TEXT("materialParameters"), BuildMaterialParameterPayload(Renderer));
        Obj->SetArrayField(TEXT("materialRefs"), BuildUsedMaterialRefs(Renderer));
        Obj->SetObjectField(TEXT("rendererBindings"), NiagaraModelBuilder::BuildStructuredReference(
            TEXT("renderer_binding_store"),
            TEXT("RendererBindings"),
            Renderer,
            Renderer,
            TEXT("renderer")));
        Obj->SetObjectField(TEXT("provenance"), BuildDefaultReferenceProvenance());
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildIncludePathPayload(const UNiagaraNodeCustomHlsl* Node)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetBoolField(TEXT("present"), Node != nullptr);
        if (!Node || !Node->GetClass())
        {
            return Obj;
        }

        if (FProperty* AbsoluteProperty = Node->GetClass()->FindPropertyByName(FName(TEXT("AbsoluteIncludeFilePaths"))))
        {
            if (TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(const_cast<UNiagaraNodeCustomHlsl*>(Node), AbsoluteProperty))
            {
                Obj->SetField(TEXT("absolute"), Value);
            }
        }
        if (FProperty* VirtualProperty = Node->GetClass()->FindPropertyByName(FName(TEXT("VirtualIncludeFilePaths"))))
        {
            if (TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(const_cast<UNiagaraNodeCustomHlsl*>(Node), VirtualProperty))
            {
                Obj->SetField(TEXT("virtual"), Value);
            }
        }
        return Obj;
    }

    FString GetCustomHlslBody(const UNiagaraNodeCustomHlsl* Node)
    {
        if (!Node || !Node->GetClass())
        {
            return FString();
        }
        if (FStrProperty* CustomHlslProperty = FindFProperty<FStrProperty>(Node->GetClass(), FName(TEXT("CustomHlsl"))))
        {
            return CustomHlslProperty->GetPropertyValue_InContainer(Node);
        }
        return FString();
    }

    TSharedPtr<FJsonObject> BuildCustomHlslNodeJson(const UNiagaraNodeCustomHlsl* Node, const UNiagaraGraph* Graph, const UObject* Owner, const FString& OwnerKind, const FString& OwnerName)
    {
        if (!Node)
        {
            return NiagaraModelBuilder::BuildStructuredReference(
                TEXT("custom_hlsl_not_lowered"),
                TEXT("missing custom hlsl node"),
                nullptr,
                Owner,
                OwnerKind);
        }

        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("kind"), TEXT("custom_hlsl"));
        Obj->SetStringField(TEXT("displayName"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        Obj->SetStringField(TEXT("nodeName"), Node->GetName());
        Obj->SetStringField(TEXT("class"), GetClassPathSafe(Node));
        Obj->SetStringField(TEXT("objectPath"), Node->GetPathName());
        Obj->SetStringField(TEXT("scriptUsage"), EnumToString(Node->ScriptUsage));
        Obj->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString());
        Obj->SetStringField(TEXT("hlsl"), GetCustomHlslBody(Node));
        Obj->SetObjectField(TEXT("includePaths"), BuildIncludePathPayload(Node));
        Obj->SetObjectField(TEXT("owner"), NiagaraJsonHelpers::BuildOwnerRecord(OwnerKind, OwnerName, Owner));
        Obj->SetArrayField(TEXT("pins"), BuildPinArray(Node));
        Obj->SetObjectField(TEXT("properties"), BuildSparsePropertyDiffJson(
            const_cast<UNiagaraNodeCustomHlsl*>(Node),
            Node->GetClass() ? Node->GetClass()->GetDefaultObject() : nullptr));
        Obj->SetObjectField(TEXT("provenance"), BuildGraphProvenance(Graph, Node));
        return Obj;
    }

    TSharedPtr<FJsonObject> BuildScriptRef(const UNiagaraScript* Script, const FGuid& SelectedVersion, const UObject* Owner, const FString& OwnerKind, const FString& OwnerName)
    {
        if (!Script)
        {
            return NiagaraModelBuilder::BuildStructuredReference(
                TEXT("dynamic_input_not_semantically_lowered"),
                TEXT("missing dynamic input script"),
                nullptr,
                Owner,
                OwnerKind);
        }

        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("displayName"), Script->GetName());
        Obj->SetStringField(TEXT("class"), GetClassPathSafe(Script));
        Obj->SetStringField(TEXT("objectPath"), Script->GetPathName());
        Obj->SetStringField(TEXT("sourceKind"), Script->IsAsset() ? TEXT("externalAsset") : TEXT("owned"));
        Obj->SetStringField(TEXT("usage"), EnumToString(Script->GetUsage()));
        Obj->SetStringField(TEXT("selectedVersion"), SelectedVersion.ToString());
        Obj->SetObjectField(TEXT("owner"), NiagaraJsonHelpers::BuildOwnerRecord(OwnerKind, OwnerName, Owner));
        if (const UNiagaraGraph* ScriptGraph = NiagaraJsonHelpers::GetGraphFromScript(Script))
        {
            Obj->SetObjectField(TEXT("graph"), BuildGraphProvenance(ScriptGraph, nullptr));
        }
        return Obj;
    }

    bool IsParameterMapPin(const UEdGraphPin* Pin)
    {
        return Pin && Pin->PinType.PinCategory == FName(TEXT("NiagaraParameterMap"));
    }

    bool IsDynamicInputNode(const UNiagaraNodeFunctionCall* Node)
    {
        return Node && Node->FunctionScript && Node->FunctionScript->IsDynamicInputScript();
    }

    TSharedPtr<FJsonObject> BuildDynamicInputNodeJson(
        const UNiagaraNodeFunctionCall* Node,
        const UNiagaraGraph* Graph,
        const UEdGraphPin* OwningInputPin,
        const UObject* Owner,
        const FString& OwnerKind,
        const FString& OwnerName,
        TSet<FGuid>& Visited,
        int32 Depth);

    TArray<TSharedPtr<FJsonValue>> BuildNestedDynamicInputs(
        const UNiagaraNodeFunctionCall* Node,
        const UNiagaraGraph* Graph,
        const UObject* Owner,
        const FString& OwnerKind,
        const FString& OwnerName,
        TSet<FGuid>& Visited,
        int32 Depth)
    {
        TArray<TSharedPtr<FJsonValue>> Inputs;
        if (!Node)
        {
            return Inputs;
        }

        for (const UEdGraphPin* Pin : Node->GetAllPins())
        {
            if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapPin(Pin) || Pin->LinkedTo.Num() == 0)
            {
                continue;
            }

            const UEdGraphPin* LinkedPin = Pin->LinkedTo[0];
            const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
            if (Depth >= 8)
            {
                Inputs.Add(MakeObjectValue(NiagaraModelBuilder::BuildStructuredReference(
                    TEXT("dynamic_input_depth_limit"),
                    LinkedNode ? LinkedNode->GetName() : Pin->PinName.ToString(),
                    LinkedNode,
                    Owner,
                    OwnerKind,
                    MakeObject(),
                    MakeObject(),
                    BuildGraphProvenance(Graph, LinkedNode))));
                continue;
            }
            if (const UNiagaraNodeCustomHlsl* CustomHlslNode = Cast<UNiagaraNodeCustomHlsl>(LinkedNode))
            {
                Inputs.Add(MakeObjectValue(BuildCustomHlslNodeJson(CustomHlslNode, Graph, Owner, OwnerKind, OwnerName)));
            }
            else if (const UNiagaraNodeFunctionCall* FunctionNode = Cast<UNiagaraNodeFunctionCall>(LinkedNode))
            {
                Inputs.Add(MakeObjectValue(BuildDynamicInputNodeJson(FunctionNode, Graph, Pin, Owner, OwnerKind, OwnerName, Visited, Depth + 1)));
            }
            else if (LinkedNode)
            {
                Inputs.Add(MakeObjectValue(NiagaraModelBuilder::BuildStructuredReference(
                    TEXT("dynamic_input_not_semantically_lowered"),
                    LinkedNode->GetName(),
                    LinkedNode,
                    Owner,
                    OwnerKind,
                    MakeObject(),
                    MakeObject(),
                    BuildGraphProvenance(Graph, LinkedNode))));
            }
        }
        return Inputs;
    }

    TSharedPtr<FJsonObject> BuildDynamicInputNodeJson(
        const UNiagaraNodeFunctionCall* Node,
        const UNiagaraGraph* Graph,
        const UEdGraphPin* OwningInputPin,
        const UObject* Owner,
        const FString& OwnerKind,
        const FString& OwnerName,
        TSet<FGuid>& Visited,
        int32 Depth)
    {
        if (!Node || !IsDynamicInputNode(Node))
        {
            return NiagaraModelBuilder::BuildStructuredReference(
                TEXT("dynamic_input_not_semantically_lowered"),
                Node ? Node->GetName() : FString(),
                Node,
                Owner,
                OwnerKind,
                MakeObject(),
                MakeObject(),
                BuildGraphProvenance(Graph, Node));
        }

        if (Visited.Contains(Node->NodeGuid))
        {
            return NiagaraModelBuilder::BuildStructuredReference(
                TEXT("dynamic_input_not_semantically_lowered"),
                Node->GetName(),
                Node,
                Owner,
                OwnerKind,
                MakeObject(),
                MakeObject(),
                BuildGraphProvenance(Graph, Node));
        }

        Visited.Add(Node->NodeGuid);

        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("kind"), TEXT("dynamic_input"));
        Obj->SetStringField(TEXT("valueMode"), TEXT("dynamic_input"));
        Obj->SetStringField(TEXT("displayName"), Node->GetFunctionName());
        Obj->SetStringField(TEXT("nodeName"), Node->GetName());
        Obj->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString());
        Obj->SetStringField(TEXT("class"), GetClassPathSafe(Node));
        Obj->SetStringField(TEXT("objectPath"), Node->GetPathName());
        Obj->SetStringField(TEXT("functionScript"), GetObjectPathSafe(Node->FunctionScript));
        Obj->SetStringField(TEXT("functionScriptAssetObjectPath"), Node->FunctionScriptAssetObjectPath.ToString());
        Obj->SetStringField(TEXT("selectedVersion"), Node->SelectedScriptVersion.ToString());
        Obj->SetStringField(TEXT("inputName"), OwningInputPin ? OwningInputPin->PinName.ToString() : FString());
        Obj->SetObjectField(TEXT("inputPin"), BuildPinModel(OwningInputPin));
        Obj->SetObjectField(TEXT("script"), BuildScriptRef(Node->FunctionScript, Node->SelectedScriptVersion, Owner, OwnerKind, OwnerName));
        Obj->SetObjectField(TEXT("owner"), NiagaraJsonHelpers::BuildOwnerRecord(OwnerKind, OwnerName, Owner));
        Obj->SetArrayField(TEXT("pins"), BuildPinArray(Node));
        Obj->SetArrayField(TEXT("nestedInputRefs"), BuildNestedDynamicInputs(Node, Graph, Owner, OwnerKind, OwnerName, Visited, Depth));
        Obj->SetObjectField(TEXT("provenance"), BuildGraphProvenance(Graph, Node));
        return Obj;
    }
}

namespace NiagaraModelBuilder
{
    TSharedPtr<FJsonObject> BuildStructuredReference(
        const FString& Reason,
        const FString& DisplayName,
        const UObject* Object,
        const UObject* Owner,
        const FString& OwnerKind,
        TSharedPtr<FJsonObject> Refs,
        TSharedPtr<FJsonObject> Properties,
        TSharedPtr<FJsonObject> Provenance)
    {
        TSharedPtr<FJsonObject> Obj = MakeObject();
        Obj->SetStringField(TEXT("kind"), TEXT("reference"));
        Obj->SetStringField(TEXT("reason"), Reason);
        Obj->SetStringField(TEXT("displayName"), DisplayName);
        Obj->SetStringField(TEXT("class"), GetClassPathSafe(Object));
        Obj->SetStringField(TEXT("objectPath"), GetObjectPathSafe(Object));
        Obj->SetObjectField(TEXT("owner"), BuildOwnerRecord(OwnerKind, Owner ? Owner->GetName() : FString(), Owner));
        Obj->SetObjectField(TEXT("refs"), Refs.IsValid() ? Refs : MakeObject());
        Obj->SetObjectField(TEXT("properties"), Properties.IsValid() ? Properties : MakeObject());
        Obj->SetObjectField(TEXT("provenance"), Provenance.IsValid() ? Provenance : BuildDefaultReferenceProvenance());
        return Obj;
    }

    TArray<TSharedPtr<FJsonValue>> BuildRendererModel(const UNiagaraEmitter* Emitter)
    {
        TArray<TSharedPtr<FJsonValue>> Renderers;
        const FVersionedNiagaraEmitterData* EmitterData = Emitter ? Emitter->GetLatestEmitterData() : nullptr;
        if (!EmitterData)
        {
            return Renderers;
        }

        const TArray<UNiagaraRendererProperties*>& RendererProperties = EmitterData->GetRenderers();
        for (int32 Index = 0; Index < RendererProperties.Num(); ++Index)
        {
            Renderers.Add(MakeObjectValue(BuildRendererReferenceJson(RendererProperties[Index], Index)));
        }
        return Renderers;
    }

    TArray<TSharedPtr<FJsonValue>> BuildCustomHlslModel(const UNiagaraNodeFunctionCall* ModuleNode, const UNiagaraGraph* Graph, const UObject* Owner, const FString& OwnerKind, const FString& OwnerName)
    {
        TArray<TSharedPtr<FJsonValue>> Nodes;
        if (!ModuleNode)
        {
            return Nodes;
        }

        TSet<FGuid> AddedNodes;
        for (const UEdGraphPin* Pin : ModuleNode->GetAllPins())
        {
            if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapPin(Pin) || Pin->LinkedTo.Num() == 0)
            {
                continue;
            }

            const UEdGraphPin* LinkedPin = Pin->LinkedTo[0];
            const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
            if (const UNiagaraNodeCustomHlsl* CustomHlslNode = Cast<UNiagaraNodeCustomHlsl>(LinkedNode))
            {
                if (AddedNodes.Contains(CustomHlslNode->NodeGuid))
                {
                    continue;
                }
                AddedNodes.Add(CustomHlslNode->NodeGuid);
                Nodes.Add(MakeObjectValue(BuildCustomHlslNodeJson(CustomHlslNode, Graph, Owner, OwnerKind, OwnerName)));
            }
        }
        return Nodes;
    }

    TArray<TSharedPtr<FJsonValue>> BuildDynamicInputModel(const UNiagaraNodeFunctionCall* ModuleNode, const UNiagaraGraph* Graph, const UObject* Owner, const FString& OwnerKind, const FString& OwnerName)
    {
        TArray<TSharedPtr<FJsonValue>> Inputs;
        if (!ModuleNode)
        {
            return Inputs;
        }

        TSet<FGuid> Visited;
        for (const UEdGraphPin* Pin : ModuleNode->GetAllPins())
        {
            if (!Pin || Pin->Direction != EGPD_Input || IsParameterMapPin(Pin) || Pin->LinkedTo.Num() == 0)
            {
                continue;
            }

            const UEdGraphPin* LinkedPin = Pin->LinkedTo[0];
            const UEdGraphNode* LinkedNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
            if (const UNiagaraNodeCustomHlsl* CustomHlslNode = Cast<UNiagaraNodeCustomHlsl>(LinkedNode))
            {
                Inputs.Add(MakeObjectValue(BuildCustomHlslNodeJson(CustomHlslNode, Graph, Owner, OwnerKind, OwnerName)));
            }
            else if (const UNiagaraNodeFunctionCall* FunctionNode = Cast<UNiagaraNodeFunctionCall>(LinkedNode))
            {
                Inputs.Add(MakeObjectValue(BuildDynamicInputNodeJson(FunctionNode, Graph, Pin, Owner, OwnerKind, OwnerName, Visited, 0)));
            }
            else if (LinkedNode)
            {
                Inputs.Add(MakeObjectValue(BuildStructuredReference(
                    TEXT("dynamic_input_not_semantically_lowered"),
                    LinkedNode->GetName(),
                    LinkedNode,
                    Owner,
                    OwnerKind,
                    MakeObject(),
                    MakeObject(),
                    BuildGraphProvenance(Graph, LinkedNode))));
            }
        }
        return Inputs;
    }
}
