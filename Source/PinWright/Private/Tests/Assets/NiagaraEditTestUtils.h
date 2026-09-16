// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared fixture helpers for Niagara edit handler tests. Extracted from the original
// TestNiagaraEditHandler.cpp anonymous namespace so advanced-edit tests can reuse the
// same transient asset construction and JSON-RPC invocation plumbing.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Tests/TestUtils.h"

#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace NiagaraEditTestUtils
{
    inline FString MakeAssetName(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s_%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    // Path of the Niagara System asset duplicated as the fixture base. Must be a real saved
    // asset so it satisfies every editor invariant the FNiagaraSystemViewModel pipeline expects:
    // PostLoad GraphSource, populated VersionData, UNiagaraScriptSource on every emitter script,
    // and a non-zero MessageAssetKey set during asset save. Faking those on a
    // NewObject<UNiagaraSystem> hits an `Assertion failed: MessageAssetKey != FGuid()` in
    // NiagaraMessageManager that can't be honestly worked around.
    //
    // Deliberately an engine-plugin (`/Niagara/`) asset, not a `/Game/` project asset: CI runs
    // these tests against per-version Content Examples host projects that don't ship the dev
    // project's content, so a `/Game/...` seed fails to load and every fixture-dependent test fails
    // identically on all engines. SimpleExplosion is one of the stock Niagara template Systems
    // shipped with the plugin on UE 5.3-5.8 (the BehaviorExamples assets are emitters, not
    // systems, so they can't seed NewTransientSystem). It is a single CPU sprite-burst emitter
    // with fully-baked scripts, no GPU sim, and no exotic data interfaces.
    inline constexpr const TCHAR* FixtureSystemAssetPath = TEXT("/Niagara/DefaultAssets/Templates/Systems/SimpleExplosion.SimpleExplosion");

    // The fixture with its authored emitters left in place, unlike NewTransientSystem below, which
    // empties the handles. Tests that read emitter-derived state (sim targets, GPU scripts,
    // compile fan-out) need a system that actually has emitters. Not rooted: hold the result in a
    // TStrongObjectPtr and pair OutObjectPath with CleanupTestAsset.
    inline UNiagaraSystem* DuplicateFixtureSystemWithEmitters(
        const TCHAR* AssetNamePrefix,
        FString& OutObjectPath)
    {
        UNiagaraSystem* Source = LoadObject<UNiagaraSystem>(nullptr, FixtureSystemAssetPath);
        if (!Source)
        {
            return nullptr;
        }

        const FString AssetName = MakeAssetName(AssetNamePrefix);
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        // See NewTransientSystem below for why the package itself is RF_Transient.
        Package->SetFlags(RF_Transient);
        UNiagaraSystem* System = DuplicateObject<UNiagaraSystem>(Source, Package, FName(*AssetName));
        if (System)
        {
            System->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
            OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        }
        return System;
    }

    inline UNiagaraSystem* NewTransientSystem(FString& OutObjectPath)
    {
        UNiagaraSystem* Source = LoadObject<UNiagaraSystem>(nullptr, FixtureSystemAssetPath);
        if (!Source)
        {
            return nullptr;
        }

        const FString AssetName = MakeAssetName(TEXT("NS_AdvEdit"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        // Mark the package itself RF_Transient so FEditorFileUtils::GetDirtyContentPackages
        // (FileHelpers.cpp:5415) skips it during FPackageAutoSaver::AttemptAutoSave. Without
        // this, the autosaver picks up our dirty test packages and routes them through
        // UPackage::Save -> UNiagaraSystem::WaitForCompilationComplete -> RequestCompile,
        // which crashes inside FNiagaraCompilationGraphDigested::Digest on these
        // synthetic-then-mutated systems. Path stays under /Game/... so RPC handlers can
        // still resolve OutObjectPath via LoadObject<>(nullptr, *Path).
        Package->SetFlags(RF_Transient);
        UNiagaraSystem* System = DuplicateObject<UNiagaraSystem>(Source, Package, FName(*AssetName));
        if (!System)
        {
            return nullptr;
        }
        System->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
        // Reset emitter handles so tests start from a known-empty system. Tests in the suite
        // commonly assert "0 event handlers / 0 sim stages / 0 user parameters" pre-mutation;
        // the duplicated source ships emitters configured by the artist, so wipe them here and
        // let MakeAuthorableSystem repopulate via AddEmitterHandle when an emitter is needed.
        System->GetEmitterHandles().Empty();
        // Strip user-scope parameters too: NS_CharacterDash carries a few pre-configured user
        // parameters whose presence would break tests like add_data_interface that probe an
        // initially empty exposed-parameters store.
        FNiagaraParameterStore& UserParams = System->GetExposedParameters();
        TArray<FNiagaraVariable> ExistingParams;
        UserParams.GetParameters(ExistingParams);
        for (const FNiagaraVariable& Var : ExistingParams)
        {
            UserParams.RemoveParameter(Var);
        }
        System->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return System;
    }

    inline UNiagaraEmitter* NewTransientEmitter(FString& OutObjectPath)
    {
        // Reuse one of the fixture system's saved emitters as the duplication source. The real
        // emitter has fully-baked GraphSource, VersionData, UNiagaraScriptSource on all four
        // scripts, and a real MessageAssetKey — exactly what FNiagaraSystemViewModel needs.
        UNiagaraSystem* Source = LoadObject<UNiagaraSystem>(nullptr, FixtureSystemAssetPath);
        if (!Source)
        {
            return nullptr;
        }
        UNiagaraEmitter* SourceEmitter = nullptr;
        for (const FNiagaraEmitterHandle& H : Source->GetEmitterHandles())
        {
            if (UNiagaraEmitter* Candidate = H.GetInstance().Emitter)
            {
                SourceEmitter = Candidate;
                break;
            }
        }
        if (!SourceEmitter)
        {
            return nullptr;
        }

        const FString AssetName = MakeAssetName(TEXT("NE_AdvEdit"));
        const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        // See NewTransientSystem above for the rationale on RF_Transient at the package level.
        Package->SetFlags(RF_Transient);
        UNiagaraEmitter* Emitter = DuplicateObject<UNiagaraEmitter>(SourceEmitter, Package, FName(*AssetName));
        if (!Emitter)
        {
            return nullptr;
        }
        Emitter->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
        Emitter->AddToRoot();
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        return Emitter;
    }

    // Builds a transient system whose first emitter handle wraps an emitter with a usable
    // particle graph. Required because event-handler / sim-stage authoring (and handle-level
    // edits like bIsEnabled) run through a system that owns a real emitter handle, which must
    // exist before the RPC fires. Out pointers are co-owned by the caller (NewTransientSystem /
    // NewTransientEmitter root them); pair with FAuthorableSystemRoots to RemoveFromRoot at
    // end-of-test.
    inline bool MakeAuthorableSystem(FString& OutSystemPath, UNiagaraSystem*& OutSystem, UNiagaraEmitter*& OutSourceEmitter, FName& OutEmitterName)
    {
        OutSystem = NewTransientSystem(OutSystemPath);
        if (!OutSystem)
        {
            return false;
        }

        FString EmitterPath;
        OutSourceEmitter = NewTransientEmitter(EmitterPath);
        if (!OutSourceEmitter)
        {
            return false;
        }

        FNiagaraEmitterHandle Handle = OutSystem->AddEmitterHandle(*OutSourceEmitter, FName(TEXT("TestEmitter")), OutSourceEmitter->GetExposedVersion().VersionGuid);
        OutEmitterName = Handle.GetName();
        return true;
    }

    // RAII unroot for the AddToRoot pins set by NewTransientSystem / NewTransientEmitter so
    // the editor process doesn't accumulate rooted assets across the suite.
    struct FAuthorableSystemRoots
    {
        UNiagaraSystem* System = nullptr;
        UNiagaraEmitter* Emitter = nullptr;

        FAuthorableSystemRoots() = default;
        FAuthorableSystemRoots(UNiagaraSystem* InSystem, UNiagaraEmitter* InEmitter)
            : System(InSystem), Emitter(InEmitter)
        {
        }
        FAuthorableSystemRoots(const FAuthorableSystemRoots&) = delete;
        FAuthorableSystemRoots& operator=(const FAuthorableSystemRoots&) = delete;
        FAuthorableSystemRoots(FAuthorableSystemRoots&& Other) noexcept
            : System(Other.System), Emitter(Other.Emitter)
        {
            Other.System = nullptr;
            Other.Emitter = nullptr;
        }
        FAuthorableSystemRoots& operator=(FAuthorableSystemRoots&& Other) noexcept
        {
            if (this != &Other)
            {
                Release();
                System = Other.System;
                Emitter = Other.Emitter;
                Other.System = nullptr;
                Other.Emitter = nullptr;
            }
            return *this;
        }
        ~FAuthorableSystemRoots()
        {
            Release();
        }

    private:
        void Release()
        {
            if (System && System->IsRooted())
            {
                System->RemoveFromRoot();
            }
            if (Emitter && Emitter->IsRooted())
            {
                Emitter->RemoveFromRoot();
            }
            System = nullptr;
            Emitter = nullptr;
        }
    };

    inline bool InvokeExpectError(
        FAutomationTestBase& Test,
        const TCHAR* Method,
        const TSharedPtr<FJsonObject>& Payload,
        const TCHAR* ExpectedErrorCode)
    {
        FTestResponseCapture Capture;
        const FString MethodName(Method);
        Test.TestTrue(FString::Printf(TEXT("%s handler found"), Method), InvokeHandlerWithCapture(MethodName, Payload, Capture));
        Test.TestTrue(FString::Printf(TEXT("%s sent a response"), Method), Capture.bWasCalled);
        Test.TestFalse(FString::Printf(TEXT("%s failed"), Method), Capture.bSuccess);
        Test.TestEqual(FString::Printf(TEXT("%s error code"), Method), Capture.ErrorCode, FString(ExpectedErrorCode));
        Test.TestTrue(FString::Printf(TEXT("%s error message present"), Method), !Capture.Message.IsEmpty());
        return Capture.bWasCalled && !Capture.bSuccess && Capture.ErrorCode == ExpectedErrorCode;
    }

    inline bool InvokeExpectSuccess(
        FAutomationTestBase& Test,
        const TCHAR* Method,
        const TSharedPtr<FJsonObject>& Payload,
        FTestResponseCapture& Capture)
    {
        const FString MethodName(Method);
        Test.TestTrue(FString::Printf(TEXT("%s handler found"), Method), InvokeHandlerWithCapture(MethodName, Payload, Capture));
        Test.TestTrue(FString::Printf(TEXT("%s sent a response"), Method), Capture.bWasCalled);
        Test.TestTrue(FString::Printf(TEXT("%s succeeded"), Method), Capture.bSuccess);
        Test.TestNotNull(FString::Printf(TEXT("%s result present"), Method), Capture.Result.Get());
        return Capture.bWasCalled && Capture.bSuccess && Capture.Result.IsValid();
    }
}
