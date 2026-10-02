// Copyright (c) 2026 Alexander Penkin. MIT License.

// F-niagara-create-module-script:
//   - niagara.create_module_script builds Input -> MapGet(Module.<in>) -> CustomHlsl -> MapSet -> Output,
//     sets the usage bitmask, compiles, and reports the measured status.
//   - niagara.graph.create_node / remove_node accept a standalone script asset (refused
//     UNSUPPORTED_ASSET / ASSET_NOT_FOUND before), and a CustomHlsl node created with
//     payload.inputs / payload.outputs carries those typed pins (no typed pins before).
// Counterfactuals: without the Signature fill the typed-pin assertions fail; without the
// standalone-script branches in ResolveTarget / ResolveNiagaraGraph the graph verbs on the module
// asset are refused; without the bitmask write niagara.add_module refuses ParticleUpdate.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeCustomHlsl.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS && UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)

namespace NiagaraCreateModuleScriptTest
{
    FString NewModulePackagePath()
    {
        return FString::Printf(TEXT("/Game/PinWrightTests/%s"), *NiagaraEditTestUtils::MakeAssetName(TEXT("M_PwHlsl")));
    }

    TSharedPtr<FJsonValue> PinSpec(const TCHAR* Name, const TCHAR* Type, const TCHAR* Namespace = nullptr)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Name);
        Obj->SetStringField(TEXT("type"), Type);
        if (Namespace)
        {
            Obj->SetStringField(TEXT("namespace"), Namespace);
        }
        return MakeShared<FJsonValueObject>(Obj);
    }

    TArray<TSharedPtr<FJsonValue>> Strings(std::initializer_list<const TCHAR*> Values)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const TCHAR* Value : Values)
        {
            Out.Add(MakeShared<FJsonValueString>(Value));
        }
        return Out;
    }

    // inputs [Scale: float], outputs [Velocity: vec3 -> Particles], usages [ParticleUpdate].
    TSharedPtr<FJsonObject> MakePayload(const FString& PackagePath, const TCHAR* Hlsl, bool bCompile)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), PackagePath);
        Payload->SetStringField(TEXT("hlsl"), Hlsl);
        Payload->SetArrayField(TEXT("inputs"), { PinSpec(TEXT("Scale"), TEXT("float")) });
        Payload->SetArrayField(TEXT("outputs"), { PinSpec(TEXT("Velocity"), TEXT("vec3"), TEXT("Particles")) });
        Payload->SetArrayField(TEXT("usages"), Strings({ TEXT("ParticleUpdate") }));
        Payload->SetBoolField(TEXT("compile"), bCompile);
        return Payload;
    }

    // Keeps the autosaver off the unsaved module package (same reason NewTransientSystem marks its
    // package transient) for the rest of the test.
    UNiagaraScript* FindCreatedScript(const FString& PackagePath)
    {
        const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath);
        UNiagaraScript* Script = FindObject<UNiagaraScript>(nullptr, *ObjectPath);
        if (Script)
        {
            Script->GetOutermost()->SetFlags(RF_Transient);
        }
        return Script;
    }

    const TSharedPtr<FJsonObject>* FindPinObject(const TArray<TSharedPtr<FJsonValue>>& Pins, const TCHAR* Name, const TCHAR* Direction)
    {
        for (const TSharedPtr<FJsonValue>& Value : Pins)
        {
            const TSharedPtr<FJsonObject>* Obj = nullptr;
            if (Value->TryGetObject(Obj)
                && (*Obj)->GetStringField(TEXT("name")) == Name
                && (*Obj)->GetStringField(TEXT("direction")) == Direction)
            {
                return Obj;
            }
        }
        return nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraCreateModuleScriptWiredTest,
    "PinWright.niagara.create_module_script.CreatesWiredCompiledModule",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCreateModuleScriptWiredTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraCreateModuleScriptTest;
    const FString PackagePath = NewModulePackagePath();
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.create_module_script"),
            MakePayload(PackagePath, TEXT("Velocity = float3(0.0f, 0.0f, Scale);"), /*bCompile=*/true), Capture))
    {
        return false;
    }
    UNiagaraScript* Script = FindCreatedScript(PackagePath);
    if (!TestNotNull(TEXT("the module asset exists at the requested path"), Script))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Compile = Capture.Result->GetObjectField(TEXT("compile"));
    TestEqual(TEXT("compile.status is a measured success"), Compile->GetStringField(TEXT("status")), FString(TEXT("succeeded")));
    TestEqual(TEXT("no compile errors"), Compile->GetArrayField(TEXT("errors")).Num(), 0);
    TestEqual(TEXT("the engine agrees the script compiled"),
        static_cast<int32>(Script->GetLastCompileStatus()), static_cast<int32>(ENiagaraScriptCompileStatus::NCS_UpToDate));

    // Typed pins read back off the node, every one wired.
    const TArray<TSharedPtr<FJsonValue>>& Pins = Capture.Result->GetArrayField(TEXT("pins"));
    const TSharedPtr<FJsonObject>* Scale = FindPinObject(Pins, TEXT("Scale"), TEXT("input"));
    const TSharedPtr<FJsonObject>* Velocity = FindPinObject(Pins, TEXT("Velocity"), TEXT("output"));
    if (TestNotNull(TEXT("CustomHlsl has a typed Scale input"), Scale))
    {
        TestEqual(TEXT("Scale is a float"), (*Scale)->GetStringField(TEXT("niagaraType")), FNiagaraTypeDefinition::GetFloatDef().GetName());
        TestEqual(TEXT("Scale is wired from MapGet"), (*Scale)->GetIntegerField(TEXT("links")), 1);
    }
    if (TestNotNull(TEXT("CustomHlsl has a typed Velocity output"), Velocity))
    {
        TestEqual(TEXT("Velocity is a vec3"), (*Velocity)->GetStringField(TEXT("niagaraType")), FNiagaraTypeDefinition::GetVec3Def().GetName());
        TestEqual(TEXT("Velocity is wired to MapSet"), (*Velocity)->GetIntegerField(TEXT("links")), 1);
    }
    TestNotNull(TEXT("parameter-map input pin"), FindPinObject(Pins, TEXT("Map"), TEXT("input")));
    TestNotNull(TEXT("parameter-map output pin"), FindPinObject(Pins, TEXT("Map"), TEXT("output")));

    const TArray<TSharedPtr<FJsonValue>>& Usages = Capture.Result->GetArrayField(TEXT("usages"));
    TestEqual(TEXT("exactly one usage read back"), Usages.Num(), 1);
    TestEqual(TEXT("the usage is ParticleUpdate"), Usages.Num() ? Usages[0]->AsString() : FString(), FString(TEXT("ParticleUpdate")));
    TestEqual(TEXT("the bitmask on the asset is ParticleUpdate only"),
        Script->GetLatestScriptData()->ModuleUsageBitmask, 1 << static_cast<int32>(ENiagaraScriptUsage::ParticleUpdateScript));

    // The reported CustomHlsl guid is the node in the asset's graph.
    const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
    int32 HlslNodes = 0;
    for (const UEdGraphNode* Node : Source->NodeGraph->Nodes)
    {
        if (Cast<UNiagaraNodeCustomHlsl>(Node))
        {
            ++HlslNodes;
            TestEqual(TEXT("customHlslNodeGuid names the graph's CustomHlsl node"),
                Node->NodeGuid.ToString(), Capture.Result->GetStringField(TEXT("customHlslNodeGuid")));
        }
    }
    TestEqual(TEXT("exactly one CustomHlsl node"), HlslNodes, 1);

    // Round trip: NIR decompile shows the wired customHlsl block and the module input it reads.
    TSharedPtr<FJsonObject> NirPayload = MakeShared<FJsonObject>();
    NirPayload->SetStringField(TEXT("assetPath"), Script->GetPathName());
    FTestResponseCapture Nir;
    if (NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.decompile_nir"), NirPayload, Nir))
    {
        const FString Ir = Nir.Result->GetStringField(TEXT("ir"));
        TestTrue(TEXT("NIR shows a customHlsl block"), Ir.Contains(TEXT("customHlsl")));
        TestTrue(TEXT("NIR shows the Module.Scale read"), Ir.Contains(TEXT("Module.Scale")));
        TestTrue(TEXT("NIR shows the Particles.Velocity write"), Ir.Contains(TEXT("Particles.Velocity")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraCreateModuleScriptBadHlslTest,
    "PinWright.niagara.create_module_script.BadHlslReportsFailedCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCreateModuleScriptBadHlslTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraCreateModuleScriptTest;
    const FString PackagePath = NewModulePackagePath();
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    // The VM backend compiler's own reaction to the broken body (NiagaraCompiler.cpp).
    AddExpectedErrorPlain(TEXT("errors encountered compiling Vector VM shaders"), EAutomationExpectedErrorFlags::Contains, 1);

    FTestResponseCapture Capture;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.create_module_script"),
            MakePayload(PackagePath, TEXT("Velocity = this_is_not_hlsl(;"), /*bCompile=*/true), Capture))
    {
        return false;
    }
    TestNotNull(TEXT("a failed compile keeps the asset"), FindCreatedScript(PackagePath));

    const TSharedPtr<FJsonObject> Compile = Capture.Result->GetObjectField(TEXT("compile"));
    TestEqual(TEXT("compile.status reports the failure"), Compile->GetStringField(TEXT("status")), FString(TEXT("failed")));
    const TArray<TSharedPtr<FJsonValue>>& Errors = Compile->GetArrayField(TEXT("errors"));
    TestTrue(TEXT("compile.errors is not empty"), Errors.Num() > 0);
    for (const TSharedPtr<FJsonValue>& Error : Errors)
    {
        TestFalse(TEXT("every error carries a message"), Error->AsObject()->GetStringField(TEXT("message")).IsEmpty());
    }
    TestFalse(TEXT("the CustomHlsl node guid is reported for correlation"),
        Capture.Result->GetStringField(TEXT("customHlslNodeGuid")).IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraCreateModuleScriptRefusalsTest,
    "PinWright.niagara.create_module_script.RefusesExistingAssetAndInvalidArguments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCreateModuleScriptRefusalsTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraCreateModuleScriptTest;
    const FString PackagePath = NewModulePackagePath();
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    FTestResponseCapture First;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.create_module_script"),
            MakePayload(PackagePath, TEXT("Velocity = float3(Scale, 0.0f, 0.0f);"), /*bCompile=*/false), First))
    {
        return false;
    }
    FindCreatedScript(PackagePath);
    TestEqual(TEXT("compile:false is reported as not requested"),
        First.Result->GetObjectField(TEXT("compile"))->GetStringField(TEXT("status")), FString(TEXT("notRequested")));

    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.create_module_script"),
        MakePayload(PackagePath, TEXT("Velocity = float3(0, 0, 0);"), false), TEXT("ASSET_ALREADY_EXISTS"));

    // Each refusal below must leave nothing at its path.
    const FString Fresh = NewModulePackagePath();
    auto ExpectRefused = [this, &Fresh](const TSharedPtr<FJsonObject>& Payload, const TCHAR* Code, const TCHAR* What)
    {
        NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.create_module_script"), Payload, Code);
        TestNull(FString::Printf(TEXT("%s created nothing"), What),
            FindObject<UNiagaraScript>(nullptr, *(Fresh + TEXT(".") + FPackageName::GetShortName(Fresh))));
    };

    TSharedPtr<FJsonObject> BadUsage = MakePayload(Fresh, TEXT("Velocity = 0;"), false);
    BadUsage->SetArrayField(TEXT("usages"), Strings({ TEXT("NotAStack") }));
    ExpectRefused(BadUsage, TEXT("INVALID_ARGUMENT"), TEXT("an unknown usage"));

    TSharedPtr<FJsonObject> NoNamespace = MakePayload(Fresh, TEXT("Velocity = 0;"), false);
    NoNamespace->SetArrayField(TEXT("outputs"), { PinSpec(TEXT("Velocity"), TEXT("vec3")) });
    ExpectRefused(NoNamespace, TEXT("INVALID_ARGUMENT"), TEXT("an output with no namespace"));

    TSharedPtr<FJsonObject> Dotted = MakePayload(Fresh, TEXT("Velocity = 0;"), false);
    Dotted->SetArrayField(TEXT("inputs"), { PinSpec(TEXT("Module.Scale"), TEXT("float")) });
    ExpectRefused(Dotted, TEXT("INVALID_ARGUMENT"), TEXT("a dotted input name"));

    TSharedPtr<FJsonObject> BadType = MakePayload(Fresh, TEXT("Velocity = 0;"), false);
    BadType->SetArrayField(TEXT("inputs"), { PinSpec(TEXT("Scale"), TEXT("NotARealNiagaraType")) });
    ExpectRefused(BadType, TEXT("INVALID_PARAMETER_TYPE"), TEXT("an unknown type"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraCreateModuleScriptAddModuleTest,
    "PinWright.niagara.create_module_script.AddModuleHonorsUsages",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraCreateModuleScriptAddModuleTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraCreateModuleScriptTest;
    const FString PackagePath = NewModulePackagePath();
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    FTestResponseCapture Created;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.create_module_script"),
            MakePayload(PackagePath, TEXT("Velocity = float3(0.0f, 0.0f, Scale);"), /*bCompile=*/true), Created))
    {
        return false;
    }
    UNiagaraScript* Module = FindCreatedScript(PackagePath);
    if (!TestNotNull(TEXT("module created"), Module))
    {
        return false;
    }

    // Scoped ownership: the duplicated fixture is held by TStrongObjectPtr and its package is
    // discarded on exit, so nothing here is rooted.
    FString SystemPath;
    ON_SCOPE_EXIT { CleanupTestAsset(SystemPath); };
    TStrongObjectPtr<UNiagaraSystem> SystemOwner(
        NiagaraEditTestUtils::DuplicateFixtureSystemWithEmitters(TEXT("NS_CreateModuleScript"), SystemPath));
    UNiagaraSystem* System = SystemOwner.Get();
    if (!TestNotNull(TEXT("fixture system created"), System)
        || !TestTrue(TEXT("fixture system has an emitter"), System->GetEmitterHandles().Num() > 0))
    {
        return false;
    }
    const FName EmitterName = System->GetEmitterHandles()[0].GetName();

    auto MakeAdd = [System, Module, &EmitterName](const TCHAR* ScriptUsage, bool bWithEmitter)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), System->GetPathName());
        if (bWithEmitter)
        {
            Payload->SetStringField(TEXT("emitter"), EmitterName.ToString());
        }
        Payload->SetStringField(TEXT("modulePath"), Module->GetPathName());
        Payload->SetStringField(TEXT("scriptUsage"), ScriptUsage);
        Payload->SetBoolField(TEXT("compile"), false);
        return Payload;
    };

    FTestResponseCapture Added;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.add_module"), MakeAdd(TEXT("ParticleUpdateScript"), true), Added);
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.add_module"), MakeAdd(TEXT("SystemSpawnScript"), false),
        TEXT("INCOMPATIBLE_STACK_GROUP"));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraGraphVerbsOnStandaloneScriptTest,
    "PinWright.niagara.graph.create_node.StandaloneScriptCustomHlslTypedPins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraGraphVerbsOnStandaloneScriptTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraCreateModuleScriptTest;
    const FString PackagePath = NewModulePackagePath();
    ON_SCOPE_EXIT { CleanupTestAsset(PackagePath); };

    FTestResponseCapture Created;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.create_module_script"),
            MakePayload(PackagePath, TEXT("Velocity = float3(0.0f, 0.0f, Scale);"), /*bCompile=*/false), Created))
    {
        return false;
    }
    UNiagaraScript* Module = FindCreatedScript(PackagePath);
    if (!TestNotNull(TEXT("module created"), Module))
    {
        return false;
    }
    const UNiagaraGraph* Graph = Cast<UNiagaraScriptSource>(Module->GetLatestSource())->NodeGraph;
    const int32 NodesBefore = Graph->Nodes.Num();

    TSharedPtr<FJsonObject> NodePayload = MakeShared<FJsonObject>();
    NodePayload->SetStringField(TEXT("customHlsl"), TEXT("Out = In * 2.0f;"));
    NodePayload->SetArrayField(TEXT("inputs"), { PinSpec(TEXT("In"), TEXT("float")) });
    NodePayload->SetArrayField(TEXT("outputs"), { PinSpec(TEXT("Out"), TEXT("vec2")) });

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("assetPath"), Module->GetPathName());
    Params->SetStringField(TEXT("nodeClass"), TEXT("NiagaraNodeCustomHlsl"));
    Params->SetNumberField(TEXT("x"), 0.0);
    Params->SetNumberField(TEXT("y"), 400.0);
    Params->SetObjectField(TEXT("payload"), NodePayload);

    FTestResponseCapture Node;
    if (!NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.graph.create_node"), Params, Node))
    {
        return false;
    }
    TestEqual(TEXT("the node landed in the module's own graph"), Graph->Nodes.Num(), NodesBefore + 1);

    const TArray<TSharedPtr<FJsonValue>>& Pins = Node.Result->GetArrayField(TEXT("pins"));
    const TSharedPtr<FJsonObject>* In = FindPinObject(Pins, TEXT("In"), TEXT("input"));
    const TSharedPtr<FJsonObject>* Out = FindPinObject(Pins, TEXT("Out"), TEXT("output"));
    if (TestNotNull(TEXT("typed input pin In"), In))
    {
        TestEqual(TEXT("In is a float"), (*In)->GetStringField(TEXT("niagaraType")), FNiagaraTypeDefinition::GetFloatDef().GetName());
    }
    if (TestNotNull(TEXT("typed output pin Out"), Out))
    {
        TestEqual(TEXT("Out is a vec2"), (*Out)->GetStringField(TEXT("niagaraType")), FNiagaraTypeDefinition::GetVec2Def().GetName());
    }
    TestNotNull(TEXT("parameter-map input pin first"), FindPinObject(Pins, TEXT("Map"), TEXT("input")));

    // remove_node resolves through the other graph resolver; it must accept the script too.
    TSharedPtr<FJsonObject> Remove = MakeShared<FJsonObject>();
    Remove->SetStringField(TEXT("assetPath"), Module->GetPathName());
    Remove->SetStringField(TEXT("nodeId"), Node.Result->GetStringField(TEXT("nodeId")));
    FTestResponseCapture Removed;
    NiagaraEditTestUtils::InvokeExpectSuccess(*this, TEXT("niagara.graph.remove_node"), Remove, Removed);
    TestEqual(TEXT("remove_node took the node back out of the module graph"), Graph->Nodes.Num(), NodesBefore);

    // A typed-pin payload that cannot be honoured is refused and leaves the graph as it was.
    NodePayload->SetArrayField(TEXT("inputs"), { PinSpec(TEXT("Bad.Name"), TEXT("float")) });
    NiagaraEditTestUtils::InvokeExpectError(*this, TEXT("niagara.graph.create_node"), Params, TEXT("INVALID_ARGUMENT"));
    TestEqual(TEXT("a refused typed-pin payload leaves no node behind"), Graph->Nodes.Num(), NodesBefore);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && UE 5.5+
