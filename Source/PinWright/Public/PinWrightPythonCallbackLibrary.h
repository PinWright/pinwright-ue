// Copyright (c) 2026 Alexander Penkin. MIT License.

// PinWrightPythonCallbackLibrary.h - the reporting channel the Python tracking shim
// (Content/Python/pinwright_callbacks.py) uses to mirror registrations into C++.
//
// Nothing here is meant to be called from a user script. The shim wraps
// unreal.register_slate_post_tick_callback and its siblings and calls these functions on
// the caller's behalf; a script that calls them directly only invents records for
// callbacks the plugin cannot then clear, because clearing needs the engine handle the
// shim keeps. The user-facing surface is the python.callbacks RPC verb.
//
// Following PinWrightPackageLibrary: static and BlueprintCallable on a
// UBlueprintFunctionLibrary in an Editor module with LoadingPhase "Default", which is the
// ordinary route to `unreal.PinWrightPythonCallbackLibrary.<snake_case_name>()`. No
// out-parameters, for the reason recorded there: PyGenUtil folds a return value plus
// out-params into a Python tuple, and a non-empty tuple is always truthy.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PinWrightPythonCallbackLibrary.generated.h"

UCLASS()
class PINWRIGHT_API UPinWrightPythonCallbackLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Record a callback the shim just registered with the editor and return the id it is
     * filed under. Kind is one of slate_post_tick, slate_pre_tick, python_shutdown;
     * Source is the "<file>:<line> in <function>" of the registering frame.
     */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Python")
    static FString NotifyRegistered(const FString& Kind, const FString& Source);

    /** Drop the record for Id, because the callback is no longer registered. */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Python")
    static void NotifyUnregistered(const FString& Id);

    /**
     * True while a record for Id is still held. The shim reads it when re-installing after
     * a module reload, so it re-files only the callbacks this registry actually lost.
     */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Python")
    static bool IsTracked(const FString& Id);

    /**
     * Count one invocation of Id. Error carries the traceback when that invocation
     * raised and is empty otherwise; only the most recent one is kept.
     */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Python")
    static void NotifyInvoked(const FString& Id, const FString& Error);
};
