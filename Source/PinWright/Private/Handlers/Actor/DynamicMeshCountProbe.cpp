// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Actor/DynamicMeshCountProbe.h"

#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/Script.h"
#include "UObject/UnrealType.h"

namespace
{
    // FindObject (never LoadObject/LoadClass): resolving the class must not force the
    // GeometryFramework module to load in a host that deliberately disabled the
    // GeometryScripting plugin. Null simply means "no dynamic meshes possible here".
    UClass* FindDynamicMeshComponentClass()
    {
        return FindObject<UClass>(nullptr, TEXT("/Script/GeometryFramework.DynamicMeshComponent"));
    }

    // Calls UDynamicMesh::GetTriangleCount by reflection.
    //
    // GetTriangleCount is a real UFUNCTION(BlueprintCallable) on every supported engine
    // (UDynamicMesh.h:159-160 on 5.8, :140-141 on 5.3), unlike
    // UDynamicMeshComponent::GetDynamicMesh whose UFUNCTION specifier is commented out —
    // that accessor is NOT reflection-callable, which is why the MeshObject property is
    // read directly instead.
    bool TryCallGetTriangleCount(UObject* MeshObject, int32& OutTriangles)
    {
        UFunction* Fn = MeshObject->FindFunction(TEXT("GetTriangleCount"));
        if (!Fn)
        {
            return false;
        }

        // Walk the reflected parameter frame instead of assuming a bare `struct { int32
        // ReturnValue; }`: ProcessEvent copies exactly Fn->ParmsSize bytes, so a
        // hand-rolled struct that disagrees with a future signature would corrupt the
        // stack. Any *input* parameter means the signature drifted beyond what this probe
        // can synthesize, so bail rather than guess a value.
        FIntProperty* ReturnProp = nullptr;
        for (TFieldIterator<FProperty> It(Fn); It && (It->PropertyFlags & CPF_Parm); ++It)
        {
            if (!(It->PropertyFlags & CPF_ReturnParm))
            {
                return false;
            }
            ReturnProp = CastField<FIntProperty>(*It);
        }
        if (!ReturnProp || Fn->ParmsSize == 0)
        {
            return false;
        }

        void* Parms = FMemory::Malloc(Fn->ParmsSize);
        FMemory::Memzero(Parms, Fn->ParmsSize);
        // The frame is a single trivially destructible int32 (enforced by the loop above),
        // so a plain Free is sufficient; no DestroyValue_InContainer pass is needed.
        ON_SCOPE_EXIT { FMemory::Free(Parms); };

        FEditorScriptExecutionGuard ScriptGuard;
        MeshObject->ProcessEvent(Fn, Parms);
        OutTriangles = ReturnProp->GetPropertyValue_InContainer(Parms);
        return true;
    }
}

bool DynamicMeshCountProbe::TryGetTotalTriangleCount(const AActor* Actor, int64& OutTriangles)
{
    if (!IsValid(Actor))
    {
        return false;
    }

    UClass* DMCClass = FindDynamicMeshComponentClass();
    if (!DMCClass)
    {
        return false;
    }

    // MeshObject is `UPROPERTY(Instanced) TObjectPtr<UDynamicMesh>` on the component
    // (DynamicMeshComponent.h:259 on 5.8, :129 on 5.3) — reflection-visible on 5.3-5.8.
    FObjectProperty* MeshProp =
        CastField<FObjectProperty>(DMCClass->FindPropertyByName(TEXT("MeshObject")));
    if (!MeshProp)
    {
        return false;
    }

    int64 Total = 0;
    bool bAnyCounted = false;

    TInlineComponentArray<UActorComponent*> Components;
    Actor->GetComponents(Components);
    for (UActorComponent* Comp : Components)
    {
        if (!IsValid(Comp) || !Comp->IsA(DMCClass))
        {
            continue;
        }

        // A dynamic-mesh component we can see but cannot read fails the WHOLE probe
        // rather than contributing 0: a partial total would compare unequal against the
        // source and manufacture a false "substituted" verdict, which is the one outcome
        // this probe must never invent.
        UObject* MeshObject = MeshProp->GetObjectPropertyValue_InContainer(Comp);
        int32 Tris = 0;
        if (!IsValid(MeshObject) || !TryCallGetTriangleCount(MeshObject, Tris))
        {
            return false;
        }

        Total += Tris;
        bAnyCounted = true;
    }

    if (!bAnyCounted)
    {
        return false;
    }

    OutTriangles = Total;
    return true;
}

DynamicMeshCountProbe::EDuplicateMeshVerdict DynamicMeshCountProbe::ClassifyDuplicate(
    bool bSourceProbed, bool bDupProbed, int64 SourceTris, int64 DupTris)
{
    if (!bSourceProbed || !bDupProbed)
    {
        return EDuplicateMeshVerdict::NotProbed;
    }
    return (SourceTris == DupTris) ? EDuplicateMeshVerdict::Ok
                                   : EDuplicateMeshVerdict::Substituted;
}

DynamicMeshCountProbe::FScopedDupeTriThresholdRaise::FScopedDupeTriThresholdRaise(
    int64 RequiredTriangles)
{
    CVar = IConsoleManager::Get().FindConsoleVariable(
        TEXT("geometry.DynamicMesh.TextBasedDupeTriThreshold"));
    if (!CVar)
    {
        return;
    }

    PreviousThreshold = CVar->GetInt();

    // The engine gates the Base64 payload on the triangle count being under the
    // threshold, so the ceiling must sit strictly above the source count. Clamped to
    // int32 because that is the cvar's declared type (TAutoConsoleVariable<int32>).
    const int64 Wanted = RequiredTriangles + 1;
    const int32 NewThreshold = (Wanted >= (int64)MAX_int32) ? MAX_int32 : (int32)Wanted;

    // Only ever raise. If the host already runs a more permissive threshold, leave the
    // global alone rather than round-tripping it through a lower value.
    if (NewThreshold > PreviousThreshold)
    {
        CVar->Set(NewThreshold);
        bRaised = true;
    }
}

DynamicMeshCountProbe::FScopedDupeTriThresholdRaise::~FScopedDupeTriThresholdRaise()
{
    if (CVar && bRaised)
    {
        CVar->Set(PreviousThreshold);
    }
}
