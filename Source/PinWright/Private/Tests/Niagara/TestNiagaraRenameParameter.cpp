// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for niagara.rename_parameter.
// The dispatcher path must exercise the production exported-API helper, not a
// direct FNiagaraParameterStore::RenameParameter copy inside the test.
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestUtils.h"

#include "NiagaraParameterStore.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraRenameParameterTest,
    "PinWright.niagara.rename_parameter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraRenameParameterTest::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("niagara.rename_parameter is registered"),
        IsRegistered(TEXT("niagara.rename_parameter")));

    FString SystemObjectPath;
    UNiagaraSystem* System = NiagaraEditTestUtils::NewTransientSystem(SystemObjectPath);
    if (!TestNotNull(TEXT("Transient UNiagaraSystem constructed"), System))
    {
        return false;
    }
    ON_SCOPE_EXIT
    {
        if (System)
        {
            System->RemoveFromRoot();
        }
    };

    const FNiagaraTypeDefinition ColorType = FNiagaraTypeDefinition::GetColorDef();
    const FName OldParamName(TEXT("User.Colour"));
    const FName NewParamName(TEXT("User.Color"));
    const FNiagaraVariable OldVar(ColorType, OldParamName);
    const FNiagaraVariable NewVar(ColorType, NewParamName);

    System->GetExposedParameters().AddParameter(OldVar);
    TestNotEqual(
        TEXT("User.Colour present in store before rename"),
        System->GetExposedParameters().IndexOf(OldVar),
        static_cast<int32>(INDEX_NONE));

    UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System->GetEditorData());
    if (!TestNotNull(TEXT("System editor data exists"), EditorData))
    {
        return false;
    }

    UNiagaraScriptVariable* UserScriptVariable = EditorData->FindOrAddUserScriptVariable(OldVar, *System);
    if (!TestNotNull(TEXT("User.Colour user script metadata exists before rename"), UserScriptVariable))
    {
        return false;
    }
    TestEqual(
        TEXT("User script metadata starts on old name"),
        UserScriptVariable->Variable.GetName().ToString(),
        FString(TEXT("User.Colour")));

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), SystemObjectPath);
    Payload->SetStringField(TEXT("scope"), TEXT("user"));
    Payload->SetStringField(TEXT("oldName"), TEXT("User.Colour"));
    Payload->SetStringField(TEXT("newName"), TEXT("User.Color"));
    Payload->SetBoolField(TEXT("compile"), false);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    const bool bSucceeded = NiagaraEditTestUtils::InvokeExpectSuccess(
        *this,
        TEXT("niagara.rename_parameter"),
        Payload,
        Capture);
    if (!bSucceeded)
    {
        return false;
    }

    bool bRenamed = false;
    TestTrue(TEXT("renamed field returned"), Capture.Result->TryGetBoolField(TEXT("renamed"), bRenamed));
    TestTrue(TEXT("rename reports success"), bRenamed);

    TestNotEqual(
        TEXT("User.Color present in store after rename"),
        System->GetExposedParameters().IndexOf(NewVar),
        static_cast<int32>(INDEX_NONE));
    TestEqual(
        TEXT("User.Colour absent from store after rename"),
        System->GetExposedParameters().IndexOf(OldVar),
        INDEX_NONE);

    TestEqual(
        TEXT("User script metadata renamed through handler path"),
        UserScriptVariable->Variable.GetName().ToString(),
        FString(TEXT("User.Color")));

    const TSharedPtr<FJsonObject>* ReferencesUpdated = nullptr;
    TestTrue(
        TEXT("referencesUpdated object returned"),
        Capture.Result->TryGetObjectField(TEXT("referencesUpdated"), ReferencesUpdated) &&
        ReferencesUpdated &&
        ReferencesUpdated->IsValid());
    if (ReferencesUpdated && ReferencesUpdated->IsValid())
    {
        const TSharedPtr<FJsonObject>& ReferencesObject = *ReferencesUpdated;
        bool bUserStore = false;
        bool bUserScriptMetadata = false;
        bool bSystemRenameHook = false;
        TestTrue(TEXT("userStore boolean returned"), ReferencesObject->TryGetBoolField(TEXT("userStore"), bUserStore));
        TestTrue(TEXT("userScriptMetadata boolean returned"), ReferencesObject->TryGetBoolField(TEXT("userScriptMetadata"), bUserScriptMetadata));
        TestTrue(TEXT("systemRenameHook boolean returned"), ReferencesObject->TryGetBoolField(TEXT("systemRenameHook"), bSystemRenameHook));
        TestTrue(TEXT("userStore reported renamed"), bUserStore);
        TestTrue(TEXT("userScriptMetadata reported renamed"), bUserScriptMetadata);
        TestTrue(TEXT("system rename hook reported invoked"), bSystemRenameHook);
        TestFalse(
            TEXT("rendererBindings is not a string claim such as engine-handled"),
            ReferencesObject->HasTypedField<EJson::String>(TEXT("rendererBindings")));
    }

    return true;
}
