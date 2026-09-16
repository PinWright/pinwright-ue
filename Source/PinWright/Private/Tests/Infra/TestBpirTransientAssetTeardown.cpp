// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace BpirTransientAssetTeardownTestHelpers
{
    bool ExtractHelperBody(
        const FString& Source, const FString& SignatureText, FString& OutBody)
    {
        const FString Neutralized = NeutralizeSourceText(Source);
        const int32 Signature = Neutralized.Find(SignatureText);
        const int32 OpenBrace = Signature == INDEX_NONE
            ? INDEX_NONE
            : Neutralized.Find(TEXT("{"), ESearchCase::CaseSensitive,
                ESearchDir::FromStart, Signature);
        if (OpenBrace == INDEX_NONE)
        {
            return false;
        }

        int32 Depth = 0;
        for (int32 Index = OpenBrace; Index < Neutralized.Len(); ++Index)
        {
            if (Neutralized[Index] == TEXT('{'))
            {
                ++Depth;
            }
            else if (Neutralized[Index] == TEXT('}') && --Depth == 0)
            {
                OutBody = Neutralized.Mid(OpenBrace + 1, Index - OpenBrace - 1);
                return true;
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirTransientAssetTeardownContractTest,
    "PinWright.infra.contract.TestAssetTeardown.BpirTransientDiscard",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirTransientAssetTeardownContractTest::RunTest(const FString& Parameters)
{
    const FString AssetName = FString::Printf(TEXT("BP_BpirTeardown_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackagePath = TEXT("/Game/__PW_GatewayTests/") + AssetName;
    const FString ObjectPath = ToObjectPath(PackagePath);

    FString PackageFilename;
    if (!TestTrue(TEXT("BPIR fixture package has a deterministic .uasset filename"),
            FPackageName::TryConvertLongPackageNameToFilename(
                PackagePath, PackageFilename, FPackageName::GetAssetPackageExtension())))
    {
        return false;
    }
    TestFalse(TEXT("BPIR teardown fixture has no file before creation"),
        IFileManager::Get().FileExists(*PackageFilename));

    UPackage* Package = CreatePackage(*PackagePath);
    if (!TestNotNull(TEXT("Never-saved BPIR package created"), Package))
    {
        return false;
    }
    UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Package, FName(*AssetName), BPTYPE_Normal,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
    if (!TestNotNull(TEXT("Never-saved BPIR Blueprint created"), Blueprint))
    {
        return false;
    }
    FAssetRegistryModule::AssetCreated(Blueprint);

    // Retain reinstancing artifacts until the helper's GC. This is the state that used to make
    // UBlueprint::Rename collide the current SKEL_ CDO with a REINST_SKEL_ CDO in /Engine/Transient.
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
    TestNotNull(TEXT("Compiled BPIR Blueprint has a generated class"),
        Blueprint->GeneratedClass.Get());
    TestNotNull(TEXT("Compiled BPIR Blueprint has a skeleton generated class"),
        Blueprint->SkeletonGeneratedClass.Get());
    TestNotNull(TEXT("Compiled BPIR generated class has a CDO"),
        Blueprint->GeneratedClass
            ? Blueprint->GeneratedClass->GetDefaultObject(false)
            : nullptr);
    UObject* SkeletonClassDefaultObject = Blueprint->SkeletonGeneratedClass
        ? Blueprint->SkeletonGeneratedClass->GetDefaultObject(false)
        : nullptr;
    TestNotNull(TEXT("Compiled BPIR skeleton class has a CDO"),
        SkeletonClassDefaultObject);
    UObject* RetainedReinstancedSkeletonDefaultObject = SkeletonClassDefaultObject
        ? StaticFindObjectFast(UObject::StaticClass(), GetTransientPackage(),
            SkeletonClassDefaultObject->GetFName())
        : nullptr;
    TestNotNull(TEXT("SkipGarbageCollection retains the conflicting reinstanced skeleton CDO"),
        RetainedReinstancedSkeletonDefaultObject);
    TestTrue(TEXT("Retained skeleton CDO belongs to a REINST_ class"),
        RetainedReinstancedSkeletonDefaultObject
            && RetainedReinstancedSkeletonDefaultObject->GetClass()->GetName().StartsWith(
                TEXT("REINST_")));
    Package->SetDirtyFlag(false);

    TestNotNull(TEXT("BPIR asset is findable before discard"),
        FindObject<UObject>(nullptr, *ObjectPath));
    TestNotNull(TEXT("BPIR package is resident before discard"),
        FindPackage(nullptr, *PackagePath));
    TestFalse(TEXT("Creating the BPIR fixture does not save a .uasset"),
        IFileManager::Get().FileExists(*PackageFilename));

    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(PackagePath));

    TestNull(TEXT("Discard removes the BPIR asset from the object hash"),
        FindObject<UObject>(nullptr, *ObjectPath));
    TestNull(TEXT("Discard releases the never-saved BPIR package"),
        FindPackage(nullptr, *PackagePath));
    TestFalse(TEXT("Discard does not create or retain a .uasset file"),
        IFileManager::Get().FileExists(*PackageFilename));

    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPath);
    TestNull(TEXT("Discard is idempotent for an already-released BPIR asset"),
        FindObject<UObject>(nullptr, *ObjectPath));
    TestNull(TEXT("Second discard keeps the BPIR package released"),
        FindPackage(nullptr, *PackagePath));

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
    if (!TestTrue(TEXT("PinWright plugin resolved for teardown source contract"), Plugin.IsValid()))
    {
        return false;
    }
    FString TeardownSource;
    const FString TeardownHeader = Plugin->GetBaseDir()
        / TEXT("Source/PinWright/Private/Tests/TestAssetTeardown.h");
    if (!TestTrue(TEXT("Read TestAssetTeardown.h for the non-forcing path contract"),
            FFileHelper::LoadFileToString(TeardownSource, *TeardownHeader)))
    {
        return false;
    }
    FString WrapperBody;
    if (!TestTrue(TEXT("Located DiscardCreatedAssetByObjectPath implementation"),
            BpirTransientAssetTeardownTestHelpers::ExtractHelperBody(
                TeardownSource,
                TEXT("inline void DiscardCreatedAssetByObjectPath(const FString& ObjectPath)"),
                WrapperBody)))
    {
        return false;
    }
    FString CoreBody;
    if (!TestTrue(TEXT("Located DiscardLoadedAssetNoGc implementation"),
            BpirTransientAssetTeardownTestHelpers::ExtractHelperBody(
                TeardownSource, TEXT("inline UPackage* DiscardLoadedAssetNoGc(UObject* Asset)"),
                CoreBody)))
    {
        return false;
    }

    const auto CheckForbiddenPaths = [this](const FString& Body, const TCHAR* Label)
    {
        TestFalse(*FString::Printf(TEXT("%s cannot call ObjectTools::ForceDeleteObjects"), Label),
            Body.Contains(TEXT("ForceDeleteObjects")));
        TestFalse(*FString::Printf(TEXT("%s cannot enter EditorAssetLibrary delete paths"), Label),
            Body.Contains(TEXT("DeleteAsset")) || Body.Contains(TEXT("DeleteLoadedAsset")));
        TestFalse(*FString::Printf(TEXT("%s cannot open a modal/dialog path"), Label),
            Body.Contains(TEXT("MessageDialog")) || Body.Contains(TEXT("Modal"))
                || Body.Contains(TEXT("OpenDialog")));
    };
    CheckForbiddenPaths(WrapperBody, TEXT("Discard wrapper"));
    CheckForbiddenPaths(CoreBody, TEXT("Discard core"));

    const auto FindAfter = [](const FString& Body, const TCHAR* Needle, int32 StartPosition)
    {
        return StartPosition == INDEX_NONE
            ? INDEX_NONE
            : Body.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, StartPosition);
    };
    const int32 AssetRename = CoreBody.Find(TEXT("const bool bAssetRenamed = Asset->Rename"));
    const int32 AssetFailure = FindAfter(CoreBody, TEXT("if (!bAssetRenamed)"), AssetRename);
    const int32 AssetLog = FindAfter(CoreBody, TEXT("UE_LOG(LogTemp, Log"), AssetFailure);
    const int32 AssetReturn = FindAfter(CoreBody, TEXT("return nullptr"), AssetLog);
    const int32 SourcePackageScan = CoreBody.Find(TEXT("bool bSourcePackageHasAssets"));
    const int32 PackageDeleted = CoreBody.Find(TEXT("PackageDeleted"));
    TestTrue(TEXT("Asset rename failure logs at Log and returns before package cleanup"),
        AssetRename != INDEX_NONE && AssetFailure > AssetRename && AssetLog > AssetFailure
            && AssetReturn > AssetLog && SourcePackageScan > AssetReturn
            && PackageDeleted > AssetReturn);

    const int32 PackageRename = CoreBody.Find(
        TEXT("const bool bPackageRenamed = SourcePackage->Rename"));
    const int32 PackageRenameReady = CoreBody.Find(
        TEXT("const bool bPackageRenameReady = SourcePackage->Rename"));
    const int32 PackageRenamePreflight = FindAfter(
        CoreBody, TEXT("PackageRenameFlags | REN_Test"), PackageRenameReady);
    const int32 PackageReadyFailure = FindAfter(
        CoreBody, TEXT("if (!bPackageRenameReady)"), PackageRenameReady);
    const int32 PackageReadyLog = FindAfter(
        CoreBody, TEXT("UE_LOG(LogTemp, Log"), PackageReadyFailure);
    const int32 PackageReadyReturn = FindAfter(
        CoreBody, TEXT("return nullptr"), PackageReadyLog);
    const int32 PackageFailure = FindAfter(CoreBody, TEXT("if (!bPackageRenamed)"), PackageRename);
    const int32 PackageLog = FindAfter(CoreBody, TEXT("UE_LOG(LogTemp, Log"), PackageFailure);
    const int32 PackageReturn = FindAfter(CoreBody, TEXT("return nullptr"), PackageLog);
    TestTrue(TEXT("Package registry removal stays at the original name after validated preflight"),
        PackageRenameReady != INDEX_NONE && PackageRenamePreflight > PackageRenameReady
            && PackageReadyFailure > PackageRenamePreflight
            && PackageReadyLog > PackageReadyFailure && PackageReadyReturn > PackageReadyLog
            && PackageDeleted > PackageReadyReturn && PackageRename > PackageDeleted);
    TestTrue(TEXT("Package rename failure logs at Log and returns before wrapper cleanup"),
        PackageRename != INDEX_NONE && PackageFailure > PackageRename
            && PackageLog > PackageFailure && PackageReturn > PackageLog
            && PackageReturn > PackageFailure);

    int32 LogCount = 0;
    int32 SearchFrom = 0;
    for (;;)
    {
        const int32 Match = CoreBody.Find(
            TEXT("UE_LOG(LogTemp, Log"), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, SearchFrom);
        if (Match == INDEX_NONE)
        {
            break;
        }
        ++LogCount;
        SearchFrom = Match + 1;
    }
    TestEqual(TEXT("Discard core emits one Log-level diagnostic per rename failure path"), LogCount, 3);
    TestFalse(TEXT("Discard core does not log rename failures as warnings"),
        CoreBody.Contains(TEXT("UE_LOG(LogTemp, Warning")));

    TestTrue(TEXT("Discard core removes Blueprint generated classes before rename"),
        CoreBody.Contains(TEXT("RemoveGeneratedClasses")));
    TestTrue(TEXT("Discard core skips generated-class rename during asset move"),
        CoreBody.Contains(TEXT("REN_SkipGeneratedClasses")));
    TestTrue(TEXT("Discard core uses a collision-free transient asset name"),
        CoreBody.Contains(TEXT("MakeUniqueObjectName")));
    TestTrue(TEXT("Discard wrapper retains immediate garbage collection"),
        WrapperBody.Contains(TEXT("CollectGarbage")));
    TestFalse(TEXT("Discard core does not collect garbage"),
        CoreBody.Contains(TEXT("CollectGarbage")));
    return true;
}
