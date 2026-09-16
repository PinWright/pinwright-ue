// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for NiagaraDumpBuilder::BuildSystemScalabilityModel.
// Constructs a transient UNiagaraSystem, populates one scalability override entry,
// and asserts the JSON shape exposes the override flags + values under
// the documented keys.
#include "Misc/AutomationTest.h"

#include "Handlers/Niagara/NiagaraDumpBuilder.h"

#include "NiagaraEffectType.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraDumpScalabilityTest,
    "PinWright.niagara.dump.Scalability",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraDumpScalabilityTest::RunTest(const FString& Parameters)
{
    // Null input still yields a present-tagged object so consumers can rely on the key.
    {
        const TSharedPtr<FJsonObject> Result = NiagaraDumpBuilder::BuildSystemScalabilityModel(nullptr);
        TestTrue(TEXT("Null system yields a non-null object"), Result.IsValid());
    }

    {
        UNiagaraSystem* System = NewObject<UNiagaraSystem>(GetTransientPackage());
        TestNotNull(TEXT("Transient UNiagaraSystem constructed"), System);

        FNiagaraSystemScalabilityOverride Override;
        Override.bOverrideDistanceSettings = 1;
        Override.MaxDistance = 1234.0f;
        System->GetScalabilityOverrides().Overrides.Add(Override);

        const TSharedPtr<FJsonObject> Result = NiagaraDumpBuilder::BuildSystemScalabilityModel(System);
        TestTrue(TEXT("Result is non-null"), Result.IsValid());

        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        const bool bHasArray = Result->TryGetArrayField(TEXT("systemScalability"), Entries);
        TestTrue(TEXT("'systemScalability' array present"), bHasArray);
        if (bHasArray && Entries)
        {
            TestEqual(TEXT("systemScalability has one entry"), Entries->Num(), 1);
            if (Entries->Num() == 1)
            {
                const TSharedPtr<FJsonObject> Entry = (*Entries)[0]->AsObject();
                TestTrue(TEXT("Entry is an object"), Entry.IsValid());
                if (Entry.IsValid())
                {
                    bool bOverrideDistance = false;
                    Entry->TryGetBoolField(TEXT("bOverrideDistanceSettings"), bOverrideDistance);
                    TestTrue(TEXT("bOverrideDistanceSettings true"), bOverrideDistance);

                    double MaxDistance = 0.0;
                    Entry->TryGetNumberField(TEXT("maxDistance"), MaxDistance);
                    TestEqual(TEXT("maxDistance round-trip"), MaxDistance, 1234.0);
                }
            }
        }
    }

    return true;
}
