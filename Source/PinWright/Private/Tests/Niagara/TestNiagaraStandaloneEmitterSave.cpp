// Copyright (c) 2026 Alexander Penkin. MIT License.

// B-niagara-emitter-save-guard-deadlocks-unused-emitter.
//
// `{compile: true, save: true}` on a standalone Niagara Emitter asset that no loaded system uses
// refused the save every time: FinalizeNiagaraEdit demanded a landed compile, Niagara compiles an
// emitter only through the systems that use it, so with none nothing could be requested — by this
// call or by the niagara.compile the refusal told the caller to run — while asset.save wrote the
// same package. The gate now recognises that case (nothing in flight, nothing compiled to
// invalidate) and saves, and the envelope says why `compiled` is false.
//
// Driven through FinalizeNiagaraEdit, the one seam every save:true niagara mutation verb reaches
// disk through, on a real package under the scratch root that has never been written, so the
// on-disk assertion cannot be satisfied by an earlier save.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Tests/Assets/NiagaraEditTestUtils.h"
#include "Tests/TestUtils.h"

#include "Handlers/Niagara/NiagaraCompileWait.h"
#include "Handlers/Niagara/NiagaraEditTypes.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// ---------------------------------------------------------------------------
// Counterfactual: restore `bCompileIsPersistable = bOutCompiled && MayPersistAfterCompileWait(Wait)`
// and the save is refused — bSaved false, SaveState Failed, no .uasset on disk — which is the
// verbatim refusal both encounters on the ticket recorded. The narrowing is pinned from the other
// side by PinWright.niagara.CompileSave.ReportsAssetSaveState: a SYSTEM whose compile did not land
// is still refused.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraStandaloneEmitterNoSystemSaveTest,
    "PinWright.niagara.CompileSave.StandaloneEmitterNoLoadedSystemSaves",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraStandaloneEmitterNoSystemSaveTest::RunTest(const FString& Parameters)
{
    const FString AssetName = NiagaraEditTestUtils::MakeAssetName(TEXT("NE_NoSystemSave"));
    const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *AssetName);
    ON_SCOPE_EXIT { CleanupTestAsset(PackageName); };

    UPackage* Package = CreatePackage(*PackageName);
    if (!TestNotNull(TEXT("fixture package created"), Package))
    {
        return false;
    }
    TStrongObjectPtr<UNiagaraEmitter> Emitter(NewObject<UNiagaraEmitter>(
        Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional));
    if (!TestNotNull(TEXT("fixture emitter created"), Emitter.Get()))
    {
        return false;
    }
    // The same initialisation niagara.create_emitter gives a new emitter asset.
    UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter.Get(), false);
    Emitter->MarkPackageDirty();

    FString Filename;
    if (!TestTrue(TEXT("fixture package maps to a filename"),
            FPackageName::TryConvertLongPackageNameToFilename(
                PackageName, Filename, FPackageName::GetAssetPackageExtension())))
    {
        return false;
    }

    FNiagaraResolvedTarget Target;
    Target.Asset = Emitter.Get();
    Target.Emitter = Emitter.Get();
    Target.EmitterData = Emitter->GetLatestEmitterData();
    Target.AssetPath = Emitter->GetPathName();
    Target.AssetKind = TEXT("NiagaraEmitter");

    // Preconditions: the shape the ticket describes, stated so a broken fixture cannot pass as a fix.
    TestEqual(TEXT("precondition: no loaded system uses the fixture emitter"),
        PinWrightNiagara::ObserveEmitterCompiles(*Emitter, NiagaraEdit::ResolveEmitterVersionGuid(Target))
            .AffectedSystemCount, 0);
    TestFalse(TEXT("precondition: the fixture has never been written to disk"),
        IFileManager::Get().FileSize(*Filename) >= 0);

    const FNiagaraEditOptions Options{/*bCompile=*/true, /*bSave=*/true};
    bool bCompiled = true;
    bool bSaved = false;
    NiagaraEdit::FinalizeNiagaraEdit(Target, Options, bCompiled, bSaved);

    TestFalse(TEXT("no compile is claimed: none could be requested"), bCompiled);
    TestTrue(TEXT("the edit records that no loaded system uses the emitter"), Target.bNoLoadedSystemUsesEmitter);
    TestTrue(TEXT("save:true is honoured on an emitter no loaded system uses"), bSaved);
    TestTrue(TEXT("save state is Written"), Target.SaveState == EAssetSaveState::Written);
    TestTrue(TEXT("the .uasset is on disk"), IFileManager::Get().FileSize(*Filename) >= 0);

    const TSharedPtr<FJsonObject> Result =
        NiagaraEdit::MakeMutationResult(TEXT("test_standalone_emitter_save"), Target, Options, bCompiled, bSaved);
    FString CompileSkipped;
    TestTrue(TEXT("the envelope says why compiled is false"),
        Result.IsValid() && Result->TryGetStringField(TEXT("compileSkipped"), CompileSkipped));
    TestEqual(TEXT("compileSkipped names the reason"), CompileSkipped, FString(TEXT("noLoadedSystemUsesEmitter")));
    bool bSavedField = false;
    TestTrue(TEXT("the envelope reports saved:true"),
        Result.IsValid() && Result->TryGetBoolField(TEXT("saved"), bSavedField) && bSavedField);
    return true;
}
