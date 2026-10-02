// Copyright (c) 2026 Alexander Penkin. MIT License.

// niagara.create_module_script — create a standalone Niagara module script whose behavior is one
// CustomHlsl node: Input -> MapGet(Module.<in>) -> CustomHlsl -> MapSet(<ns>.<out>) -> Output.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/PackagePathCompose.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraGraphCreateNodePayload.h"
#include "Handlers/Niagara/NiagaraGraphResetUtils.h"
#include "Handlers/Niagara/NiagaraJsonHelpers.h"
#include "Handlers/Niagara/NiagaraScriptCompileReport.h"
#include "Utils/AssetUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_Niagara.h"
#include "Factories/Factory.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "UObject/Package.h"

// The MapGet/MapSet pin helpers are the only exported way to add a typed parameter pin to those
// private, unexported node classes. Their public header exists from UE 5.5; 5.3/5.4 refuse.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0) && __has_include("Widgets/Wizard/SNiagaraModuleWizard.h")
class UNiagaraNodeParameterMapGet;
class UNiagaraNodeParameterMapSet;
#include "Widgets/Wizard/SNiagaraModuleWizard.h"
#define PINWRIGHT_HAS_NIAGARA_MAP_PIN_HELPERS 1
#else
#define PINWRIGHT_HAS_NIAGARA_MAP_PIN_HELPERS 0
#endif

namespace NiagaraCreateModuleScriptLocal
{
    // Namespaces a module may write. Module.* is the read side (inputs), and User.* / Engine.* are
    // read-only to a module.
    const TCHAR* const WritableNamespaces[] = {
        TEXT("Particles"), TEXT("Emitter"), TEXT("System"), TEXT("Output"),
        TEXT("Local"), TEXT("Transient"), TEXT("StackContext") };

    bool IsWritableNamespace(const FString& Namespace)
    {
        for (const TCHAR* Candidate : WritableNamespaces)
        {
            if (Namespace.Equals(Candidate, ESearchCase::CaseSensitive))
            {
                return true;
            }
        }
        return false;
    }

    UEdGraphPin* FindPin(UEdGraphNode* Node, FName Name, EEdGraphPinDirection Direction)
    {
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == Direction && Pin->PinName == Name)
            {
                return Pin;
            }
        }
        return nullptr;
    }

    UEdGraphPin* FindMapPin(UNiagaraNode* Node, EEdGraphPinDirection Direction)
    {
        TArray<UEdGraphPin*> Pins;
        if (Direction == EGPD_Input)
        {
            Node->GetInputPins(Pins);
        }
        else
        {
            Node->GetOutputPins(Pins);
        }
        return PinWrightNiagara::FindParameterMapPin(Pins);
    }

    // A script this call created but could not finish is moved out of the requested path, so the
    // caller can retry the same assetPath instead of being refused ASSET_ALREADY_EXISTS by a husk.
    void DiscardUnfinishedScript(UNiagaraScript* Script)
    {
        Script->ClearFlags(RF_Public | RF_Standalone);
        Script->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
    }

    TSharedPtr<FJsonObject> DescribeNode(const TCHAR* Role, const UEdGraphNode* Node)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("role"), Role);
        Obj->SetStringField(TEXT("nodeGuid"), Node->NodeGuid.ToString());
        Obj->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName());
        return Obj;
    }
}

REGISTER_RPC_HANDLER("niagara.create_module_script", "niagara",
    "Create a standalone Niagara module script whose body is one CustomHlsl node: typed inputs are read as Module.<name>, "
    "typed outputs are written to <namespace>.<name>, the usage bitmask is set from `usages`, and the script is compiled "
    "so the response carries the measured compile status and errors. Refuses an existing asset. UE 5.5+.",
    RPC_PARAMS(
        RPC_PARAM_REQ("assetPath", "path", "Package path of the new module, e.g. /Game/FX/Modules/M_Swirl. Must not exist."),
        RPC_PARAM_REQ("hlsl", "string", "CustomHlsl body. Inputs and outputs are referenced by their bare names; the parameter map is 'Map'."),
        RPC_PARAM_REQ("usages", "array",
            "Stacks the module may be added to: any of ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn, "
            "SystemUpdate. Required because no default is safe: niagara.add_module refuses a stack outside this set."),
        RPC_PARAM_OPT("inputs", "array", "Module inputs as [{name, type}]. Each is read from Module.<name> and wired to the HLSL input <name>."),
        RPC_PARAM_OPT("outputs", "array",
            "Module outputs as [{name, type, namespace}]. namespace is one of Particles, Emitter, System, Output, Local, "
            "Transient, StackContext; the HLSL output <name> is written to <namespace>.<name>."),
        RPC_PARAM_OPT("description", "string", "Script description shown in the module picker"),
        RPC_PARAM_DEF("compile", "boolean", "Compile the new script (synchronous) and report status and errors", "true"),
        RPC_PARAM_DEF("save", "boolean", "Save the new asset to disk", "false")
    ))
{
    using namespace NiagaraCreateModuleScriptLocal;

    FString AssetPath;
    if (!Ctx.RequireString(TEXT("assetPath"), AssetPath)) return true;
    FString Hlsl;
    if (!Ctx.RequireString(TEXT("hlsl"), Hlsl)) return true;

    const TSharedPtr<FJsonObject>& Raw = Ctx.GetRawPayload();

    // --- usages -> bitmask ---
    const TArray<TSharedPtr<FJsonValue>>* UsageValues = nullptr;
    if (!Raw->TryGetArrayField(TEXT("usages"), UsageValues) || !UsageValues || UsageValues->Num() == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT,
            TEXT("'usages' must be a non-empty array of stack names (ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn, SystemUpdate)."));
        return true;
    }
    int32 UsageBitmask = 0;
    for (const TSharedPtr<FJsonValue>& Value : *UsageValues)
    {
        FString UsageName;
        ENiagaraScriptUsage Usage;
        if (!Value.IsValid() || !Value->TryGetString(UsageName) || !NiagaraEdit::TryParseStackScriptUsageAlias(UsageName, Usage))
        {
            Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                TEXT("Unknown usage '%s'. Valid: ParticleSpawn, ParticleUpdate, EmitterSpawn, EmitterUpdate, SystemSpawn, SystemUpdate."),
                *UsageName));
            return true;
        }
        UsageBitmask |= 1 << static_cast<int32>(Usage);
    }

    // --- typed pins ---
    TArray<NiagaraGraphCreate::FCustomHlslPinSpec> Inputs;
    TArray<NiagaraGraphCreate::FCustomHlslPinSpec> Outputs;
    if (FNiagaraEditError Error = NiagaraGraphCreate::ParseCustomHlslPinSpecs(Raw, TEXT("inputs"), Inputs); Error.HasError())
    {
        Ctx.SendError(*Error.Code, Error.Message);
        return true;
    }
    if (FNiagaraEditError Error = NiagaraGraphCreate::ParseCustomHlslPinSpecs(Raw, TEXT("outputs"), Outputs); Error.HasError())
    {
        Ctx.SendError(*Error.Code, Error.Message);
        return true;
    }
    TArray<FString> OutputNamespaces;
    if (Outputs.Num() > 0)
    {
        const TArray<TSharedPtr<FJsonValue>>& OutputValues = Raw->GetArrayField(TEXT("outputs"));
        for (int32 Index = 0; Index < OutputValues.Num(); ++Index)
        {
            FString Namespace;
            OutputValues[Index]->AsObject()->TryGetStringField(TEXT("namespace"), Namespace);
            if (!IsWritableNamespace(Namespace))
            {
                Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, FString::Printf(
                    TEXT("outputs[%d].namespace '%s' is not a namespace a module can write. Required; one of Particles, Emitter, System, Output, Local, Transient, StackContext."),
                    Index, *Namespace));
                return true;
            }
            OutputNamespaces.Add(Namespace);
        }
    }

#if !PINWRIGHT_HAS_NIAGARA_MAP_PIN_HELPERS
    Ctx.SendUnsupportedEngineVersion(TEXT("5.5"), TEXT("niagara.create_module_script (Niagara map pin helpers)"));
    return true;
#else
    // --- path ---
    const FString PackageNameIn = FPackageName::ObjectPathToPackageName(AssetPath);
    FString PackagePath;
    FString PathError;
    if (!PinWrightComposeAssetPackagePath(
            FPackageName::GetLongPackagePath(PackageNameIn), FPackageName::GetShortName(PackageNameIn), PackagePath, PathError))
    {
        Ctx.SendError(ErrorCodes::ERR_INVALID_ARGUMENT, PathError);
        return true;
    }
    const FString AssetName = FPackageName::GetShortName(PackagePath);
    const FString ObjectPath = PackagePath + TEXT(".") + AssetName;
    if (FindObject<UObject>(nullptr, *ObjectPath) || FPackageName::DoesPackageExist(PackagePath))
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_ALREADY_EXISTS,
            FString::Printf(TEXT("'%s' already exists. niagara.create_module_script never overwrites; pick a new assetPath."), *ObjectPath));
        return true;
    }

    // --- create from the module factory (no _API macro: resolved by reflection) ---
    UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraModuleScriptFactory"));
    UClass* MapGetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"));
    UClass* MapSetClass = FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
    if (!FactoryClass || !MapGetClass || !MapSetClass)
    {
        Ctx.SendError(ErrorCodes::ERR_DEPENDENCY_MISSING,
            TEXT("NiagaraEditor classes NiagaraModuleScriptFactory / NiagaraNodeParameterMapGet / NiagaraNodeParameterMapSet were not found."));
        return true;
    }
    UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
    UPackage* Package = CreatePackage(*PackagePath);
    UNiagaraScript* Script = Cast<UNiagaraScript>(Factory->FactoryCreateNew(
        UNiagaraScript::StaticClass(), Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
    UNiagaraScriptSource* Source = Script ? Cast<UNiagaraScriptSource>(Script->GetLatestSource()) : nullptr;
    UNiagaraGraph* Graph = Source ? Source->NodeGraph.Get() : nullptr;
    FVersionedNiagaraScriptData* ScriptData = Script ? Script->GetLatestScriptData() : nullptr;

    UNiagaraNodeInput* InputNode = nullptr;
    UNiagaraNodeOutput* OutputNode = nullptr;
    if (Graph)
    {
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UNiagaraNodeInput* AsInput = Cast<UNiagaraNodeInput>(Node);
            if (AsInput && AsInput->Input.GetType() == FNiagaraTypeDefinition::GetParameterMapDef())
            {
                InputNode = AsInput;
            }
            if (UNiagaraNodeOutput* AsOutput = Cast<UNiagaraNodeOutput>(Node))
            {
                OutputNode = AsOutput;
            }
        }
    }
    if (!Script || !Graph || !ScriptData || !InputNode || !OutputNode)
    {
        if (Script)
        {
            DiscardUnfinishedScript(Script);
        }
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED,
            TEXT("The module factory did not produce a script with a parameter-map Input node, an Output node and version data."));
        return true;
    }

    // --- strip the template body (its MapGet(Module.InputArg) -> MapSet(Particles.DummyFloat)) ---
    // RemoveNode breaks the links; the template's parameters are then dropped from the graph's
    // variable map so they do not linger as unused script variables. Parameter-map entries (the
    // Input node's own variable) stay.
    {
        TArray<UEdGraphNode*> TemplateBody;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node != InputNode && Node != OutputNode)
            {
                TemplateBody.Add(Node);
            }
        }
        for (UEdGraphNode* Node : TemplateBody)
        {
            Graph->RemoveNode(Node);
        }
        TArray<FNiagaraVariable> Stale;
        for (const auto& Entry : Graph->GetAllMetaData())
        {
            if (Entry.Key.GetType() != FNiagaraTypeDefinition::GetParameterMapDef())
            {
                Stale.Add(Entry.Key);
            }
        }
        for (const FNiagaraVariable& Variable : Stale)
        {
            Graph->GetAllMetaData().Remove(Variable);
        }
    }

    // --- build the body ---
    const int32 BaseX = InputNode->NodePosX;
    const int32 BaseY = InputNode->NodePosY;
    auto CreateBodyNode = [Graph](UClass* NodeClass, int32 X, int32 Y, TFunctionRef<void(UEdGraphNode*)> BeforeFinalize)
    {
        // No statement between CreateNode and Finalize may leave the block: ~FGraphNodeCreator is
        // an unconditional checkf(bPlaced).
        FGraphNodeCreator<UEdGraphNode> Creator(*Graph);
        UEdGraphNode* Node = Creator.CreateNode(/*bSelectNewNode=*/false, NodeClass);
        Node->NodePosX = X;
        Node->NodePosY = Y;
        BeforeFinalize(Node);
        Creator.Finalize();
        return Node;
    };

    UEdGraphNode* MapGetNode = CreateBodyNode(MapGetClass, BaseX + 250, BaseY - 150, [](UEdGraphNode*) {});
    UEdGraphNode* HlslEdNode = CreateBodyNode(UNiagaraNodeCustomHlsl::StaticClass(), BaseX + 550, BaseY,
        [&Inputs, &Outputs, &Hlsl](UEdGraphNode* Node)
        {
            UNiagaraNodeCustomHlsl* HlslNode = CastChecked<UNiagaraNodeCustomHlsl>(Node);
            HlslNode->ScriptUsage = ENiagaraScriptUsage::Module;
            NiagaraGraphCreate::SetCustomHlslSignature(*HlslNode, Inputs, Outputs);
            // SetCustomHlsl is not exported; the property write before Finalize is the same store,
            // and the pins come from Signature at Finalize.
            if (FStrProperty* Prop = FindFProperty<FStrProperty>(UNiagaraNodeCustomHlsl::StaticClass(), TEXT("CustomHlsl")))
            {
                Prop->SetPropertyValue_InContainer(HlslNode, Hlsl);
            }
        });
    UEdGraphNode* MapSetNode = CreateBodyNode(MapSetClass, BaseX + 900, BaseY, [](UEdGraphNode*) {});
    OutputNode->NodePosX = BaseX + 1200;
    OutputNode->NodePosY = BaseY;

    UNiagaraNodeCustomHlsl* HlslNode = CastChecked<UNiagaraNodeCustomHlsl>(HlslEdNode);
    UNiagaraNodeParameterMapGet* MapGet = reinterpret_cast<UNiagaraNodeParameterMapGet*>(MapGetNode);
    UNiagaraNodeParameterMapSet* MapSet = reinterpret_cast<UNiagaraNodeParameterMapSet*>(MapSetNode);

    // --- wire: every link is checked; a link the schema refused fails the whole call ---
    const UEdGraphSchema* Schema = Graph->GetSchema();
    TArray<FString> FailedLinks;
    auto Link = [Schema, &FailedLinks](UEdGraphPin* From, UEdGraphPin* To, const FString& Label)
    {
        if (!From || !To || !Schema->TryCreateConnection(From, To))
        {
            FailedLinks.Add(Label);
        }
    };

    UEdGraphPin* InputMapOut = FindMapPin(InputNode, EGPD_Output);
    Link(InputMapOut, FindMapPin(CastChecked<UNiagaraNode>(MapGetNode), EGPD_Input), TEXT("Input.Map -> MapGet.Source"));
    Link(InputMapOut, FindPin(HlslNode, NiagaraGraphCreate::CustomHlslParameterMapPinName(), EGPD_Input), TEXT("Input.Map -> CustomHlsl.Map"));
    Link(FindPin(HlslNode, NiagaraGraphCreate::CustomHlslParameterMapPinName(), EGPD_Output),
        FindMapPin(CastChecked<UNiagaraNode>(MapSetNode), EGPD_Input), TEXT("CustomHlsl.Map -> MapSet.Source"));
    Link(FindMapPin(CastChecked<UNiagaraNode>(MapSetNode), EGPD_Output), FindMapPin(OutputNode, EGPD_Input), TEXT("MapSet.Dest -> Output"));

    TArray<TSharedPtr<FJsonValue>> InputParams;
    for (const NiagaraGraphCreate::FCustomHlslPinSpec& Spec : Inputs)
    {
        const FName Parameter(*(TEXT("Module.") + Spec.Name.ToString()));
        UEdGraphPin* ReadPin = UE::Niagara::Wizard::Utilities::AddReadParameterPin(Spec.Type, Parameter, MapGet);
        Link(ReadPin, FindPin(HlslNode, Spec.Name, EGPD_Input), FString::Printf(TEXT("MapGet.%s -> CustomHlsl.%s"), *Parameter.ToString(), *Spec.Name.ToString()));
        InputParams.Add(MakeShared<FJsonValueString>(Parameter.ToString()));
    }
    TArray<TSharedPtr<FJsonValue>> OutputParams;
    for (int32 Index = 0; Index < Outputs.Num(); ++Index)
    {
        const NiagaraGraphCreate::FCustomHlslPinSpec& Spec = Outputs[Index];
        const FName Parameter(*FString::Printf(TEXT("%s.%s"), *OutputNamespaces[Index], *Spec.Name.ToString()));
        UEdGraphPin* WritePin = UE::Niagara::Wizard::Utilities::AddWriteParameterPin(Spec.Type, Parameter, MapSet);
        Link(FindPin(HlslNode, Spec.Name, EGPD_Output), WritePin, FString::Printf(TEXT("CustomHlsl.%s -> MapSet.%s"), *Spec.Name.ToString(), *Parameter.ToString()));
        OutputParams.Add(MakeShared<FJsonValueString>(Parameter.ToString()));
    }

    if (FailedLinks.Num() > 0)
    {
        DiscardUnfinishedScript(Script);
        Ctx.SendError(ErrorCodes::ERR_CREATE_FAILED, FString::Printf(
            TEXT("The Niagara schema refused %d link(s): %s. Nothing was created."),
            FailedLinks.Num(), *FString::Join(FailedLinks, TEXT("; "))));
        return true;
    }

    ScriptData->ModuleUsageBitmask = UsageBitmask;
    FString Description;
    if (Raw->TryGetStringField(TEXT("description"), Description))
    {
        ScriptData->Description = FText::FromString(Description);
    }
    Graph->NotifyGraphChanged();
    FAssetRegistryModule::AssetCreated(Script);
    Script->MarkPackageDirty();

    // --- compile (UNiagaraScript::RequestCompile is synchronous) ---
    const bool bCompile = Ctx.GetBool(TEXT("compile"), true);
    TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
    Compile->SetBoolField(TEXT("requested"), bCompile);
    if (bCompile)
    {
        Script->RequestCompile(Script->GetExposedVersion().VersionGuid, /*bForceCompile=*/true);
        Compile->SetStringField(TEXT("status"), PinWrightNiagara::DescribeScriptCompileStatus(*Script));
        Compile->SetArrayField(TEXT("errors"), PinWrightNiagara::BuildScriptCompileEventsJson(*Script, /*bErrorsOnly=*/true));
    }
    else
    {
        Compile->SetStringField(TEXT("status"), TEXT("notRequested"));
        Compile->SetArrayField(TEXT("errors"), TArray<TSharedPtr<FJsonValue>>());
    }

    // --- save ---
    const bool bSave = Ctx.GetBool(TEXT("save"), false);
    EAssetSaveState SaveState = EAssetSaveState::NotRequested;
    const bool bSaved = bSave && SaveAssetToDiskReportingPresence(Script, /*bForce=*/true, nullptr, nullptr, &SaveState);

    // --- response: everything read back off the created asset ---
    TArray<TSharedPtr<FJsonValue>> Nodes;
    Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(TEXT("input"), InputNode)));
    Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(TEXT("mapGet"), MapGetNode)));
    Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(TEXT("customHlsl"), HlslNode)));
    Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(TEXT("mapSet"), MapSetNode)));
    Nodes.Add(MakeShared<FJsonValueObject>(DescribeNode(TEXT("output"), OutputNode)));

    TArray<TSharedPtr<FJsonValue>> Pins;
    for (UEdGraphPin* Pin : HlslNode->Pins)
    {
        if (!Pin || NiagaraJsonHelpers::IsDynamicAddPin(Pin))
        {
            continue;
        }
        TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
        PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
        PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        PinObj->SetStringField(TEXT("niagaraType"), UEdGraphSchema_Niagara::PinToTypeDefinition(Pin).GetName());
        PinObj->SetNumberField(TEXT("links"), Pin->LinkedTo.Num());
        Pins.Add(MakeShared<FJsonValueObject>(PinObj));
    }

    TArray<TSharedPtr<FJsonValue>> UsageNames;
    for (const ENiagaraScriptUsage Usage : UNiagaraScript::GetSupportedUsageContextsForBitmask(Script->GetLatestScriptData()->ModuleUsageBitmask))
    {
        UsageNames.Add(MakeShared<FJsonValueString>(NiagaraEdit::StackScriptUsageToString(Usage)));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("assetPath"), Script->GetPathName());
    Result->SetStringField(TEXT("customHlslNodeGuid"), HlslNode->NodeGuid.ToString());
    Result->SetArrayField(TEXT("nodes"), Nodes);
    Result->SetArrayField(TEXT("pins"), Pins);
    Result->SetArrayField(TEXT("inputParameters"), InputParams);
    Result->SetArrayField(TEXT("outputParameters"), OutputParams);
    Result->SetArrayField(TEXT("usages"), UsageNames);
    Result->SetObjectField(TEXT("compile"), Compile);
    AddAssetSaveReport(Result, bSave, bSaved, TOptional<EAssetSaveState>(SaveState));
    Ctx.SendSuccess(Result);
    return true;
#endif
}
