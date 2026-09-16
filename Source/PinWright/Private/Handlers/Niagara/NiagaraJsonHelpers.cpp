// Copyright (c) 2026 Alexander Penkin. MIT License.

// NiagaraJsonHelpers.cpp
//
// Out-of-line definitions for helpers declared in NiagaraJsonHelpers.h.
// Kept out of the header so callers don't drag in NiagaraTypes / NiagaraGraph
// transitively and so editor-only bodies can be conditionally compiled.

#include "Handlers/Niagara/NiagaraJsonHelpers.h"

#include "Handlers/HandlerContext.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "Handlers/Niagara/NiagaraInstanceUtils.h"
#include "Utils/JsonBuilders.h"

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraEmitter.h"
#include "NiagaraGraph.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"

namespace NiagaraJsonHelpers
{
bool IsDynamicAddPin(const UEdGraphPin* Pin)
{
    if (!Pin)
    {
        return false;
    }
    static const FName DynamicAddPinSubCategory(TEXT("DynamicAddPin"));
    return Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc
        && Pin->PinType.PinSubCategory == DynamicAddPinSubCategory;
}

TSharedPtr<FJsonObject> BuildOwnerRecord(const FString& Kind, const FString& DisplayName, const UObject* Owner)
{
    TSharedPtr<FJsonObject> Obj = MakeObject();
    Obj->SetStringField(TEXT("kind"), Kind);
    Obj->SetStringField(TEXT("displayName"), DisplayName);
    Obj->SetStringField(TEXT("objectPath"), GetObjectPathSafe(Owner));
    Obj->SetStringField(TEXT("class"), GetClassPathSafe(Owner));
    return Obj;
}

TSharedPtr<FJsonObject> BuildVector3fJson(const FVector3f& Value)
{
    TSharedPtr<FJsonObject> Obj = MakeObject();
    Obj->SetNumberField(TEXT("x"), Value.X);
    Obj->SetNumberField(TEXT("y"), Value.Y);
    Obj->SetNumberField(TEXT("z"), Value.Z);
    return Obj;
}

TSharedPtr<FJsonObject> BuildVector4fJson(const FVector4f& Value)
{
    TSharedPtr<FJsonObject> Obj = MakeObject();
    Obj->SetNumberField(TEXT("x"), Value.X);
    Obj->SetNumberField(TEXT("y"), Value.Y);
    Obj->SetNumberField(TEXT("z"), Value.Z);
    Obj->SetNumberField(TEXT("w"), Value.W);
    return Obj;
}

TSharedPtr<FJsonObject> BuildQuat4fJson(const FQuat4f& Value)
{
    TSharedPtr<FJsonObject> Obj = MakeObject();
    Obj->SetNumberField(TEXT("x"), Value.X);
    Obj->SetNumberField(TEXT("y"), Value.Y);
    Obj->SetNumberField(TEXT("z"), Value.Z);
    Obj->SetNumberField(TEXT("w"), Value.W);
    return Obj;
}

bool SendNiagaraEditError(FHandlerContext& Ctx, const FNiagaraEditError& Error)
{
    if (Error.HasError())
    {
        Ctx.SendError(Error.Code, Error.Message);
        return true;
    }
    return false;
}

UNiagaraGraph* GetGraphFromScript(UNiagaraScript* Script)
{
    if (!Script)
    {
        return nullptr;
    }
    if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource()))
    {
        return Source->NodeGraph;
    }
    return nullptr;
}

const UNiagaraGraph* GetGraphFromScript(const UNiagaraScript* Script)
{
    if (!Script)
    {
        return nullptr;
    }
    if (const UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource()))
    {
        return Source->NodeGraph;
    }
    return nullptr;
}

TSharedPtr<FJsonObject> BuildTypeModel(const FNiagaraTypeDefinition& Type)
{
    TSharedPtr<FJsonObject> Obj = MakeObject();
    Obj->SetStringField(TEXT("name"), Type.GetName());
    Obj->SetNumberField(TEXT("sizeBytes"), Type.GetSize());
    Obj->SetBoolField(TEXT("isDataInterface"), Type.IsDataInterface());
    Obj->SetBoolField(TEXT("isUObject"), Type.IsUObject());
    Obj->SetBoolField(TEXT("isEnum"), Type.IsEnum());
    if (UStruct* Struct = Type.GetStruct())
    {
        Obj->SetStringField(TEXT("struct"), Struct->GetPathName());
    }
    if (UClass* Class = Type.GetClass())
    {
        Obj->SetStringField(TEXT("class"), Class->GetPathName());
    }
    if (UEnum* Enum = Type.GetEnum())
    {
        Obj->SetStringField(TEXT("enum"), Enum->GetPathName());
    }
    return Obj;
}

void BeginEmitterMutationScope(FNiagaraResolvedTarget& Target)
{
    // The other pre-mutation seam, so the verbs that open their transaction here report the same
    // before/after data-interface delta as the ones that go through ModifyResolvedTarget.
    NiagaraEdit::RecordDataInterfaceVerdictBefore(Target);
    if (Target.System)
    {
        // Record what the kill cost the caller's neighbours: this sweep takes down every running
        // instance of the asset, an open toolkit's preview viewport included, and a preview that
        // is not auto-activating does not come back on its own.
        Target.QuiescedInstances += PinWrightNiagara::KillSystemInstances(*Target.System);
        Target.System->Modify();
    }
    if (Target.Emitter)
    {
        Target.Emitter->Modify();
    }
}

TSharedPtr<FJsonObject> EndEmitterMutationScope(
    const TCHAR* Operation,
    FNiagaraResolvedTarget& Target,
    const FNiagaraEditOptions& Options)
{
    bool bCompiled = false;
    bool bSaved = false;
    NiagaraEdit::NotifyNiagaraObjectChanged(Target, nullptr);
    NiagaraEdit::FinalizeNiagaraEdit(Target, Options, bCompiled, bSaved);
    return NiagaraEdit::MakeMutationResult(Operation, Target, Options, bCompiled, bSaved);
}
} // namespace NiagaraJsonHelpers
