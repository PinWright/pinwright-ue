// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression for B-set-default-softclass-missing-c-suffix: blueprint.set_default resolved a
// TSoftClassPtr value to the Blueprint's generated class only when the path started with
// "/Game/". Any other mount (a plugin content root) was stored verbatim as the bare package
// path, which names the UBlueprint asset rather than a UClass, so the soft pointer never
// resolved - and the verb reported success. These tests drive the real handler with a
// never-saved target Blueprint on the plugin's own "/PinWright/" mount, i.e. a non-/Game root
// that needs no host-project content.
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Tests/TestAssetTeardown.h"
#include "Tests/TestUtils.h"

namespace SetDefaultSoftClassTestHelpers
{
    const TCHAR* const SoftVarName = TEXT("TargetActorClass");

    // Transient Actor Blueprint holding a TSoftClassPtr<AActor> member variable.
    UBlueprint* CreateHolderBP(FAutomationTestBase& Test)
    {
        UBlueprint* BP = CompilerTestUtils::CreateTransientTestBP(TEXT("SoftClassHolder"));
        if (!Test.TestNotNull(TEXT("holder Blueprint created"), BP))
        {
            return nullptr;
        }
        FEdGraphPinType PinType;
        PinType.PinCategory = UEdGraphSchema_K2::PC_SoftClass;
        PinType.PinSubCategoryObject = AActor::StaticClass();
        Test.TestTrue(TEXT("soft-class member variable added"),
            FBlueprintEditorUtils::AddMemberVariable(BP, SoftVarName, PinType));
        FKismetEditorUtilities::CompileBlueprint(BP);
        return BP;
    }

    // Never-saved Actor Blueprint in a package on the plugin mount (not /Game/).
    UBlueprint* CreatePluginMountTargetBP(FAutomationTestBase& Test, FString& OutPackagePath)
    {
        const FString AssetName = FString::Printf(TEXT("BP_SoftClassTarget_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        OutPackagePath = TEXT("/PinWright/__PW_SoftClassTests/") + AssetName;
        UPackage* Package = CreatePackage(*OutPackagePath);
        if (!Test.TestNotNull(TEXT("plugin-mount target package created"), Package))
        {
            return nullptr;
        }
        UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Package, FName(*AssetName), BPTYPE_Normal,
            UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
        if (BP)
        {
            FKismetEditorUtilities::CompileBlueprint(BP);
        }
        Package->SetDirtyFlag(false);
        Test.TestNotNull(TEXT("plugin-mount target Blueprint created"), BP);
        return BP;
    }

    FTestResponseCapture SetDefault(FAutomationTestBase& Test, UBlueprint* Holder, const FString& Value)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), Holder->GetPathName());
        Payload->SetStringField(TEXT("propertyName"), SoftVarName);
        Payload->SetStringField(TEXT("value"), Value);
        FTestResponseCapture Capture;
        Test.TestTrue(TEXT("blueprint.set_default handler found"),
            InvokeHandlerWithCapture(TEXT("blueprint.set_default"), Payload, Capture));
        return Capture;
    }

    // The soft path currently stored on the holder's post-compile CDO.
    FSoftObjectPath ReadStoredPath(UBlueprint* Holder)
    {
        UObject* CDO = Holder->GeneratedClass ? Holder->GeneratedClass->GetDefaultObject() : nullptr;
        FSoftClassProperty* Prop = CDO ? FindFProperty<FSoftClassProperty>(CDO->GetClass(), SoftVarName) : nullptr;
        return Prop ? Prop->ContainerPtrToValuePtr<FSoftObjectPtr>(CDO)->ToSoftObjectPath() : FSoftObjectPath();
    }
}

// Every accepted spelling of a plugin-mount Blueprint (bare package path, Package.Asset,
// Package.Asset_C) must store the generated-class path and resolve to the UClass.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetDefaultSoftClassPluginMountResolvesTest,
    "PinWright.blueprint.set_default.SoftClassPluginMountPathResolvesToGeneratedClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetDefaultSoftClassPluginMountResolvesTest::RunTest(const FString& Parameters)
{
    using namespace SetDefaultSoftClassTestHelpers;

    UBlueprint* Holder = CreateHolderBP(*this);
    FString TargetPackage;
    UBlueprint* Target = CreatePluginMountTargetBP(*this, TargetPackage);
    if (!Holder || !Target || !TestNotNull(TEXT("target has a generated class"), Target->GeneratedClass.Get()))
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(TargetPackage));
        return false;
    }
    const FString AssetName = Target->GetName();
    const FString ExpectedClassPath = Target->GeneratedClass->GetPathName();

    const FString Spellings[] = {
        TargetPackage,                                   // /PinWright/Dir/BP_X
        TargetPackage + TEXT(".") + AssetName,           // /PinWright/Dir/BP_X.BP_X
        ExpectedClassPath                                // /PinWright/Dir/BP_X.BP_X_C
    };
    for (const FString& Spelling : Spellings)
    {
        // Reset between spellings so a stale value cannot satisfy the assertion.
        SetDefault(*this, Holder, TEXT("None"));

        const FTestResponseCapture Capture = SetDefault(*this, Holder, Spelling);
        TestTrue(FString::Printf(TEXT("set_default '%s' succeeds (err=%s %s)"),
            *Spelling, *Capture.ErrorCode, *Capture.Message), Capture.bSuccess);

        const FSoftObjectPath Stored = ReadStoredPath(Holder);
        TestEqual(FString::Printf(TEXT("'%s' stored as the generated-class path"), *Spelling),
            Stored.ToString(), ExpectedClassPath);
        TestTrue(FString::Printf(TEXT("'%s' stored path resolves to the generated class"), *Spelling),
            Stored.ResolveObject() == Target->GeneratedClass);
    }

    PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ToObjectPath(TargetPackage));
    return true;
}

// Failure direction: a path that names no UClass, or a class outside the property's
// meta-class, must be refused with CONVERSION_FAILED and must leave the stored value alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSetDefaultSoftClassUnresolvableFailsTest,
    "PinWright.blueprint.set_default.SoftClassUnresolvableValueFailsLoudly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintSetDefaultSoftClassUnresolvableFailsTest::RunTest(const FString& Parameters)
{
    using namespace SetDefaultSoftClassTestHelpers;

    UBlueprint* Holder = CreateHolderBP(*this);
    if (!Holder)
    {
        return false;
    }

    // A native class path is a valid soft-class value and seeds a known stored value.
    const FString SeedPath = APawn::StaticClass()->GetPathName();
    const FTestResponseCapture Seed = SetDefault(*this, Holder, SeedPath);
    TestTrue(FString::Printf(TEXT("native class path accepted (err=%s %s)"),
        *Seed.ErrorCode, *Seed.Message), Seed.bSuccess);
    TestEqual(TEXT("native class path stored verbatim"), ReadStoredPath(Holder).ToString(), SeedPath);

    const FString Missing = FString::Printf(TEXT("/PinWright/__PW_SoftClassTests/BP_Missing_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString Rejected[] = {
        Missing,                                          // no such asset on a plugin mount
        Missing + TEXT(".BP_Missing_C"),                  // explicit class path, still absent
        UStaticMesh::StaticClass()->GetPathName()         // a UClass, but not a child of AActor
    };
    for (const FString& Value : Rejected)
    {
        const FTestResponseCapture Capture = SetDefault(*this, Holder, Value);
        TestFalse(FString::Printf(TEXT("'%s' is refused, not reported as success"), *Value), Capture.bSuccess);
        TestEqual(FString::Printf(TEXT("'%s' refusal code"), *Value), Capture.ErrorCode, FString(TEXT("CONVERSION_FAILED")));
        TestEqual(FString::Printf(TEXT("'%s' leaves the stored value unchanged"), *Value),
            ReadStoredPath(Holder).ToString(), SeedPath);
    }
    return true;
}
