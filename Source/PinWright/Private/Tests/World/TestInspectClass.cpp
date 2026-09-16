// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/PropertyUtils.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectClassEnumeratesMembersTest,
    "PinWright.system.inspect.inspect_class.EnumeratesMembers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FInspectClassEnumeratesMembersTest::RunTest(const FString& Parameters)
{
    // Assert 1: DecodePropertyFlags surfaces UE-style tags from a CPF bitfield.
    {
        const EPropertyFlags Flags = static_cast<EPropertyFlags>(
            CPF_Edit | CPF_BlueprintVisible | CPF_Transient);
        TArray<FString> Tags = DecodePropertyFlags(Flags);
        TestTrue(TEXT("DecodePropertyFlags contains EditAnywhere"),
            Tags.Contains(TEXT("EditAnywhere")));
        // CPF_BlueprintVisible without CPF_BlueprintReadOnly → BlueprintReadWrite.
        TestTrue(TEXT("DecodePropertyFlags contains BlueprintReadWrite"),
            Tags.Contains(TEXT("BlueprintReadWrite")));
        TestTrue(TEXT("DecodePropertyFlags contains Transient"),
            Tags.Contains(TEXT("Transient")));
    }

    // Assert 2: FunctionToInspectJson on UActorComponent::SetActive.
    UFunction* SetActiveFn = UActorComponent::StaticClass()->FindFunctionByName(TEXT("SetActive"));
    TestNotNull(TEXT("UActorComponent::SetActive resolves"), SetActiveFn);
    if (SetActiveFn)
    {
        TSharedPtr<FJsonObject> Obj = FunctionToInspectJson(SetActiveFn);
        TestTrue(TEXT("FunctionToInspectJson returned a valid object"), Obj.IsValid());
        if (Obj.IsValid())
        {
            FString Name;
            Obj->TryGetStringField(TEXT("name"), Name);
            TestEqual(TEXT("function name is SetActive"), Name, FString(TEXT("SetActive")));

            // flags contains "BlueprintCallable".
            bool bFoundBPCallable = false;
            const TArray<TSharedPtr<FJsonValue>>* FlagsArr = nullptr;
            if (Obj->TryGetArrayField(TEXT("flags"), FlagsArr) && FlagsArr)
            {
                for (const TSharedPtr<FJsonValue>& V : *FlagsArr)
                {
                    if (V.IsValid() && V->AsString() == TEXT("BlueprintCallable"))
                    {
                        bFoundBPCallable = true;
                        break;
                    }
                }
            }
            TestTrue(TEXT("SetActive flags contain BlueprintCallable"), bFoundBPCallable);

            // params contains entry { name=bNewActive, direction=in }.
            bool bFoundParam = false;
            const TArray<TSharedPtr<FJsonValue>>* ParamsArr = nullptr;
            if (Obj->TryGetArrayField(TEXT("params"), ParamsArr) && ParamsArr)
            {
                for (const TSharedPtr<FJsonValue>& V : *ParamsArr)
                {
                    if (!V.IsValid()) { continue; }
                    TSharedPtr<FJsonObject> POBj = V->AsObject();
                    if (!POBj.IsValid()) { continue; }
                    FString PName, PDir;
                    POBj->TryGetStringField(TEXT("name"), PName);
                    POBj->TryGetStringField(TEXT("direction"), PDir);
                    if (PName == TEXT("bNewActive") && PDir == TEXT("in"))
                    {
                        bFoundParam = true;
                        break;
                    }
                }
            }
            TestTrue(TEXT("SetActive params include bNewActive with direction=in"), bFoundParam);
        }
    }

    // Assert 3: PropertyToInspectJson over UActorComponent properties yields ≥1 entry
    // with flags containing "EditAnywhere".
    {
        int32 PropertyCount = 0;
        bool bFoundEditAnywhere = false;
        for (TFieldIterator<FProperty> PropIt(UActorComponent::StaticClass()); PropIt; ++PropIt)
        {
            FProperty* P = *PropIt;
            if (!P) { continue; }
            ++PropertyCount;
            TSharedPtr<FJsonObject> Entry = PropertyToInspectJson(P);
            if (!Entry.IsValid()) { continue; }
            const TArray<TSharedPtr<FJsonValue>>* FlagsArr = nullptr;
            if (Entry->TryGetArrayField(TEXT("flags"), FlagsArr) && FlagsArr)
            {
                for (const TSharedPtr<FJsonValue>& V : *FlagsArr)
                {
                    if (V.IsValid() && V->AsString() == TEXT("EditAnywhere"))
                    {
                        bFoundEditAnywhere = true;
                        break;
                    }
                }
            }
            if (bFoundEditAnywhere) { break; }
        }
        TestTrue(TEXT("UActorComponent has ≥1 reflected property"), PropertyCount > 0);
        TestTrue(TEXT("at least one UActorComponent property carries EditAnywhere"),
            bFoundEditAnywhere);
    }

    return true;
}
