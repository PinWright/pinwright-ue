// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Niagara/NiagaraTickPreflight.h"

#include "Utils/ActorUtils.h"

#include "Engine/World.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "UObject/UObjectIterator.h"

namespace PinWrightNiagara
{
    int32 FindTickUnsafeNiagaraSystems(TArray<FTickUnsafeNiagaraSystem>& OutSystems)
    {
        OutSystems.Reset();

        FString ResolvedWorldMode;
        const UWorld* EditorWorld = McpActorUtils::ResolveQueryWorld(TEXT("editor"), ResolvedWorldMode);
        if (!EditorWorld)
        {
            // No editor world means nothing in a level is ticking anything. Returning empty here is
            // "nothing to detonate", not "could not look".
            return 0;
        }

        TMap<const UNiagaraSystem*, int32> RowBySystem;
        for (TObjectIterator<UNiagaraComponent> It; It; ++It)
        {
            const UNiagaraComponent* Component = *It;
            if (!IsValid(Component) || Component->IsTemplate())
            {
                continue;
            }
            if (Component->GetWorld() != EditorWorld)
            {
                continue;
            }
            // A component with no instance controller runs no simulation, so it cannot reach the
            // VectorVM. Counting it would refuse scrubs over levels that are merely holding the
            // asset.
            if (!Component->GetSystemInstanceController().IsValid())
            {
                continue;
            }
            const UNiagaraSystem* System = Component->GetAsset();
            if (!System)
            {
                continue;
            }

            if (const int32* ExistingIndex = RowBySystem.Find(System))
            {
                // INDEX_NONE marks a system already measured and found clean, so N components of
                // one clean system cost one measurement rather than N.
                if (*ExistingIndex != INDEX_NONE)
                {
                    ++OutSystems[*ExistingIndex].LiveComponents;
                }
                continue;
            }

            // Measured once per system, not once per component.
            TArray<FDataInterfaceCountMismatch> Mismatches;
            const EDataInterfaceConsistency Verdict = CheckDataInterfaceCounts(*System, Mismatches);
            if (Verdict != EDataInterfaceConsistency::Mismatched)
            {
                RowBySystem.Add(System, INDEX_NONE);
                continue;
            }

            FTickUnsafeNiagaraSystem& Row = OutSystems.AddDefaulted_GetRef();
            Row.SystemPath = System->GetPathName();
            Row.LiveComponents = 1;
            Row.Mismatches = MoveTemp(Mismatches);
            RowBySystem.Add(System, OutSystems.Num() - 1);
        }

        OutSystems.Sort([](const FTickUnsafeNiagaraSystem& A, const FTickUnsafeNiagaraSystem& B)
        {
            return A.SystemPath < B.SystemPath;
        });
        return OutSystems.Num();
    }

    FString DescribeTickUnsafeNiagaraSystems(const TArray<FTickUnsafeNiagaraSystem>& Systems)
    {
        TArray<FString> Parts;
        Parts.Reserve(Systems.Num());
        for (const FTickUnsafeNiagaraSystem& Row : Systems)
        {
            Parts.Add(FString::Printf(TEXT("%s (%d live component(s); %s)"),
                *Row.SystemPath,
                Row.LiveComponents,
                *DescribeDataInterfaceMismatches(Row.Mismatches)));
        }
        return FString::Join(Parts, TEXT("; "));
    }
}
