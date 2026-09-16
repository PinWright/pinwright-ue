// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "PinWrightAssetImportTestFactory.generated.h"

UCLASS(Transient)
class UPinWrightAssetImportTestFactory : public UFactory
{
    GENERATED_BODY()

public:
    UPinWrightAssetImportTestFactory();

    virtual UObject* FactoryCreateFile(
        UClass* InClass,
        UObject* InParent,
        FName InName,
        EObjectFlags Flags,
        const FString& Filename,
        const TCHAR* Parms,
        FFeedbackContext* Warn,
        bool& bOutOperationCanceled) override;

    int32 CreateCallCount = 0;
};
