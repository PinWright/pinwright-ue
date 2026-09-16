// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-niagara-input-schema-readback: typed per-input schema readback
// for a placed module's stack inputs.
//
// The ticket asks for a readback that lists a stack module's inputs with name + type +
// current value mode, so agents stop guessing input names/types in a trial-and-error loop
// against niagara.set_module_input. Per the ticket's own dedup rule and the plugin's
// dual-surface convention the fix EXTENDS the shared per-module stack builder
// (NiagaraDumpBuilder::BuildStackModuleJson, consumed by niagara.inspect's stack aspect and
// the niagara_stack.json dump) with a `moduleInputs` array, rather than adding a parallel
// niagara.get_module_inputs verb. This test drives the production builder that produces that
// array (NiagaraDumpBuilder::BuildModuleInputsJson) directly, so it exercises the exact code
// the handler path runs.
//
// Fixture: a transient system with an emitter and the stock SpawnRate module (float input
// "SpawnRate"), the same proven fixture the sibling NIR override tests use. Correct behavior:
// (1) the readback lists SpawnRate by name with a non-empty type and a value mode; (2) after
// binding SpawnRate to a linked parameter (User.Speed) the readback reports valueMode
// "linked" naming that parameter. Pre-fix BuildModuleInputsJson does not exist, so the tree
// does not compile without the fix — the differential dependency is by construction.

#include "Misc/AutomationTest.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/Niagara/TestNIRFixtures.h"
#include "Tests/TestUtils.h"

#include "Handlers/Niagara/NiagaraDumpBuilder.h"

#include "NiagaraCommon.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraModuleInputsSchemaTest,
    "PinWright.niagara.ModuleInputsSchema",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraModuleInputsSchemaTest::RunTest(const FString& Parameters)
{
    // A missing required fixture is a FAILURE, not a skip.
    const TCHAR* SpawnRatePath = TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate");
    UNiagaraScript* SpawnRateScript = LoadObject<UNiagaraScript>(nullptr, SpawnRatePath);
    if (!TestNotNull(TEXT("Emitter SpawnRate module script loads"), SpawnRateScript))
    {
        return false;
    }

    UNiagaraSystem* System = NIRTestFixtures::BuildEmptySystemWithEmitter(FName(TEXT("ModuleInputsSchema")));
    if (!TestNotNull(TEXT("Transient system with emitter created"), System))
    {
        return false;
    }
    NiagaraEditTestUtils::FAuthorableSystemRoots Roots;
    Roots.System = System;
    for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
    {
        Roots.Emitter = Handle.GetInstance().Emitter.Get();
        break;
    }

    UNiagaraNodeFunctionCall* ModuleNode = NIRTestFixtures::AddModuleToStack(
        System,
        ENiagaraScriptUsage::ParticleSpawnScript,
        SpawnRateScript);
    if (!TestNotNull(TEXT("SpawnRate module added to stack"), ModuleNode))
    {
        NIRTestFixtures::DestroyFixture(System);
        return false;
    }

    // (1) The typed schema lists the SpawnRate stack input with a type and a value mode.
    {
        TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
        Wrapper->SetArrayField(TEXT("moduleInputs"), NiagaraDumpBuilder::BuildModuleInputsJson(ModuleNode));

        TSharedPtr<FJsonObject> SpawnRate =
            JsonArrayFindObjectByStringField(Wrapper, TEXT("moduleInputs"), TEXT("name"), TEXT("SpawnRate"));
        if (TestTrue(TEXT("moduleInputs lists the SpawnRate stack input by name"), SpawnRate.IsValid()))
        {
            FString InputType;
            const bool bHasType = SpawnRate->TryGetStringField(TEXT("type"), InputType);
            TestTrue(TEXT("SpawnRate carries a non-empty type"), bHasType && !InputType.IsEmpty());

            FString ValueMode;
            const bool bHasMode = SpawnRate->TryGetStringField(TEXT("valueMode"), ValueMode);
            TestTrue(TEXT("SpawnRate carries a non-empty valueMode"), bHasMode && !ValueMode.IsEmpty());
        }
        else
        {
            // Surface what the readback actually produced so a naming mismatch is diagnosable.
            const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
            Wrapper->TryGetArrayField(TEXT("moduleInputs"), Arr);
            FString Names;
            if (Arr)
            {
                for (const TSharedPtr<FJsonValue>& V : *Arr)
                {
                    const TSharedPtr<FJsonObject>* O = nullptr;
                    FString N;
                    if (V.IsValid() && V->TryGetObject(O) && O && (*O)->TryGetStringField(TEXT("name"), N))
                    {
                        Names += N + TEXT(", ");
                    }
                }
            }
            AddError(FString::Printf(TEXT("moduleInputs had %d entries: [%s]"), Arr ? Arr->Num() : -1, *Names));
        }
    }

    // (2) After binding SpawnRate to a linked parameter, the readback reports the binding —
    // the value-mode discrimination the ticket needs (was a chain/linked param assigned?).
    NIRTestFixtures::SetModuleInputLinkedParam(ModuleNode, FName(TEXT("SpawnRate")), FName(TEXT("User.Speed")));
    {
        TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
        Wrapper->SetArrayField(TEXT("moduleInputs"), NiagaraDumpBuilder::BuildModuleInputsJson(ModuleNode));

        TSharedPtr<FJsonObject> SpawnRate =
            JsonArrayFindObjectByStringField(Wrapper, TEXT("moduleInputs"), TEXT("name"), TEXT("SpawnRate"));
        if (TestTrue(TEXT("moduleInputs still lists SpawnRate after linking"), SpawnRate.IsValid()))
        {
            FString ValueMode;
            SpawnRate->TryGetStringField(TEXT("valueMode"), ValueMode);
            TestEqual(TEXT("SpawnRate value mode is linked"), ValueMode, FString(TEXT("linked")));

            FString LinkedParameter;
            SpawnRate->TryGetStringField(TEXT("linkedParameter"), LinkedParameter);
            TestTrue(TEXT("linked parameter names the bound User parameter"),
                LinkedParameter.Contains(TEXT("Speed")));
        }
    }

    NIRTestFixtures::DestroyFixture(System);
    return true;
}
