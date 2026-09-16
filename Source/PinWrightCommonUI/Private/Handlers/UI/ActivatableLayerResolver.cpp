// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/UI/ActivatableLayerResolver.h"

#include "Blueprint/UserWidget.h"
#include "Engine/LocalPlayer.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Widgets/CommonActivatableWidgetContainer.h"

namespace PinWrightUi
{
    UCommonActivatableWidgetContainerBase* ResolveStackFromLayersMap(
        UObject* LayoutInstance,
        const FGameplayTag& LayerTag,
        FString& OutErrorCode,
        FString& OutErrorMsg)
    {
        if (!LayoutInstance)
        {
            OutErrorCode = TEXT("LAYER_HOST_UNAVAILABLE");
            OutErrorMsg = TEXT("Null PrimaryGameLayout instance");
            return nullptr;
        }

        FMapProperty* LayersProp = FindFProperty<FMapProperty>(LayoutInstance->GetClass(), TEXT("Layers"));
        if (!LayersProp)
        {
            OutErrorCode = TEXT("LAYER_HOST_UNAVAILABLE");
            OutErrorMsg = TEXT("PrimaryGameLayout has no reflectable 'Layers' map on this build");
            return nullptr;
        }

        // Guard the map shape before reinterpreting key bytes: key must be an FGameplayTag
        // struct, value must be an object pointer. An unexpected layout degrades gracefully
        // rather than reading raw memory as the wrong type.
        FStructProperty* KeyStructProp = CastField<FStructProperty>(LayersProp->KeyProp);
        FObjectPropertyBase* ValueObjProp = CastField<FObjectPropertyBase>(LayersProp->ValueProp);
        if (!KeyStructProp || KeyStructProp->Struct != FGameplayTag::StaticStruct() || !ValueObjProp)
        {
            OutErrorCode = TEXT("LAYER_HOST_UNAVAILABLE");
            OutErrorMsg = TEXT("PrimaryGameLayout 'Layers' is not the expected TMap<FGameplayTag, UCommonActivatableWidgetContainerBase*> shape");
            return nullptr;
        }

        // Linear scan (bound by GetMaxIndex(), not Num(), to skip sparse gaps) comparing the
        // FGameplayTag key directly — independent of the tag's hash, so it does not rely on
        // GetTypeHash(FGameplayTag) matching between build/query. Mirrors the map-walk idiom
        // in Utils/PropertyExport.cpp.
        FScriptMapHelper Helper(LayersProp, LayersProp->ContainerPtrToValuePtr<void>(LayoutInstance));
        for (int32 i = 0; i < Helper.GetMaxIndex(); ++i)
        {
            if (!Helper.IsValidIndex(i))
            {
                continue;
            }

            const uint8* KeyPtr = Helper.GetKeyPtr(i);
            const FGameplayTag& KeyTag = *reinterpret_cast<const FGameplayTag*>(KeyPtr);
            if (KeyTag == LayerTag)
            {
                const uint8* ValuePtr = Helper.GetValuePtr(i);
                UObject* Obj = ValueObjProp->GetObjectPropertyValue(ValuePtr);
                UCommonActivatableWidgetContainerBase* Stack = Cast<UCommonActivatableWidgetContainerBase>(Obj);
                if (!Stack)
                {
                    OutErrorCode = TEXT("NOT_A_STACK");
                    OutErrorMsg = FString::Printf(
                        TEXT("Layer '%s' maps to a %s, not a UCommonActivatableWidgetContainerBase"),
                        *LayerTag.ToString(),
                        Obj ? *Obj->GetClass()->GetName() : TEXT("null"));
                    return nullptr;
                }
                return Stack;
            }
        }

        OutErrorCode = TEXT("LAYER_NOT_FOUND");
        OutErrorMsg = FString::Printf(TEXT("No layer registered for tag '%s' on the PrimaryGameLayout"), *LayerTag.ToString());
        return nullptr;
    }

    UCommonActivatableWidgetContainerBase* ResolveStackByLayerTagInPie(
        const FString& LayerTagStr,
        int32 PlayerIndex,
        FString& OutErrorCode,
        FString& OutErrorMsg)
    {
        // CommonGame's UPrimaryGameLayout carries no *_API export (COMMONGAME_API is Lyra-only
        // and unlinked here), so resolve the UCLASS by reflection per the house pattern for
        // non-exported cross-module types. Absent -> graceful LAYER_HOST_UNAVAILABLE.
        UClass* LayoutClass = FindObject<UClass>(nullptr, TEXT("/Script/CommonGame.PrimaryGameLayout"));
        if (!LayoutClass)
        {
            OutErrorCode = TEXT("LAYER_HOST_UNAVAILABLE");
            OutErrorMsg = TEXT("CommonGame's UPrimaryGameLayout is not present in this project; layerTag addressing needs a CommonGame/Lyra host. Use host+stack instead.");
            return nullptr;
        }

        // Find the live PrimaryGameLayout for the requested local player, falling back to the
        // first live instance when the owning local player is not resolvable.
        UUserWidget* Matched = nullptr;
        UUserWidget* Fallback = nullptr;
        for (TObjectIterator<UUserWidget> It; It; ++It)
        {
            UUserWidget* Widget = *It;
            if (!Widget || Widget->GetWorld() == nullptr || !Widget->IsA(LayoutClass))
            {
                continue;
            }
            if (!Fallback)
            {
                Fallback = Widget;
            }
            const ULocalPlayer* LocalPlayer = Widget->GetOwningLocalPlayer();
            if (LocalPlayer && LocalPlayer->GetLocalPlayerIndex() == PlayerIndex)
            {
                Matched = Widget;
                break;
            }
        }

        UUserWidget* Layout = Matched ? Matched : Fallback;
        if (!Layout)
        {
            OutErrorCode = TEXT("LAYER_HOST_UNAVAILABLE");
            OutErrorMsg = FString::Printf(
                TEXT("No live UPrimaryGameLayout for playerIndex %d; a CommonGame UI must be running in PIE"),
                PlayerIndex);
            return nullptr;
        }

        // GameplayTags is linked, so tag construction is trivial natively (the python
        // request_gameplay_tag limitation does not apply to a C++ handler). An unregistered
        // tag yields an invalid tag here.
        const FGameplayTag LayerTag = FGameplayTag::RequestGameplayTag(FName(*LayerTagStr), /*ErrorIfNotFound=*/false);
        if (!LayerTag.IsValid())
        {
            OutErrorCode = TEXT("LAYER_TAG_INVALID");
            OutErrorMsg = FString::Printf(TEXT("Gameplay tag '%s' is not registered in this project's tag registry"), *LayerTagStr);
            return nullptr;
        }

        return ResolveStackFromLayersMap(Layout, LayerTag, OutErrorCode, OutErrorMsg);
    }
}
