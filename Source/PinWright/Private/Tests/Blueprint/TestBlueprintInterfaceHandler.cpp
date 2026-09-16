// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"

namespace
{
    FString ToBlueprintInterfaceHandlerObjectPath(const FString& PackagePath)
    {
        const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
        return AssetName.IsEmpty()
            ? PackagePath
            : FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
    }

    FString MakeUniqueAssetPath(const TCHAR* Prefix)
    {
        return FString::Printf(
            TEXT("/Game/__PW_GatewayTests/%s_%s"),
            Prefix,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    bool CreateBlueprintAsset(
        FAutomationTestBase& Test,
        const FString& AssetPath,
        const FString& BlueprintType,
        const TCHAR* Label)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(AssetPath));
        Payload->SetStringField(TEXT("savePath"), FPackageName::GetLongPackagePath(AssetPath));
        Payload->SetStringField(TEXT("blueprintType"), BlueprintType);

        FTestResponseCapture Capture;
        Test.TestTrue(FString::Printf(TEXT("%s blueprint.create handler found"), Label),
            InvokeHandlerWithCapture(TEXT("blueprint.create"), Payload, Capture));
        Test.TestTrue(FString::Printf(TEXT("%s blueprint.create responded"), Label), Capture.bWasCalled);
        if (!Test.TestTrue(FString::Printf(TEXT("%s blueprint.create succeeded"), Label), Capture.bSuccess))
        {
            Test.AddError(FString::Printf(TEXT("%s create error: %s %s"),
                Label, *Capture.ErrorCode, *Capture.Message));
            return false;
        }
        return true;
    }

    int32 CountImplementedInterface(const UBlueprint* Blueprint, const UClass* InterfaceClass)
    {
        int32 Count = 0;
        if (!Blueprint || !InterfaceClass)
        {
            return Count;
        }

        const FTopLevelAssetPath InterfacePath = InterfaceClass->GetClassPathName();
        for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
        {
            if (Description.Interface == InterfaceClass
                || (Description.Interface && Description.Interface->GetClassPathName() == InterfacePath))
            {
                ++Count;
            }
        }
        return Count;
    }

    bool InvokeInterfaceMutation(
        FAutomationTestBase& Test,
        const TCHAR* HandlerName,
        const FString& BlueprintPath,
        const FString& InterfaceClass,
        bool bExpectedChanged,
        FTestResponseCapture& Capture)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), BlueprintPath);
        Payload->SetStringField(TEXT("interfaceClass"), InterfaceClass);

        Test.TestTrue(FString::Printf(TEXT("%s handler found"), HandlerName),
            InvokeHandlerWithCapture(HandlerName, Payload, Capture));
        Test.TestTrue(FString::Printf(TEXT("%s responded"), HandlerName), Capture.bWasCalled);
        if (!Test.TestTrue(FString::Printf(TEXT("%s succeeded"), HandlerName), Capture.bSuccess))
        {
            Test.AddError(FString::Printf(TEXT("%s error: %s %s"),
                HandlerName, *Capture.ErrorCode, *Capture.Message));
            return false;
        }
        if (Capture.Result.IsValid())
        {
            Test.TestEqual(FString::Printf(TEXT("%s changed flag"), HandlerName),
                Capture.Result->GetBoolField(TEXT("changed")), bExpectedChanged);
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInterfaceAddRemoveHandlerTest,
    "PinWright.blueprint.interface.AddRemove",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintInterfaceAddRemoveHandlerTest::RunTest(const FString& Parameters)
{
    const FString TargetPath = MakeUniqueAssetPath(TEXT("BP_InterfaceTarget"));
    const FString InterfacePath = MakeUniqueAssetPath(TEXT("BPI_AddRemove"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(TargetPath);
        CleanupTestAsset(InterfacePath);
    };

    if (!CreateBlueprintAsset(*this, TargetPath, TEXT("actor"), TEXT("target")))
    {
        return true;
    }
    if (!CreateBlueprintAsset(*this, InterfacePath, TEXT("interface"), TEXT("interface")))
    {
        return true;
    }

    UBlueprint* TargetBlueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(ToBlueprintInterfaceHandlerObjectPath(TargetPath)));
    UBlueprint* InterfaceBlueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(ToBlueprintInterfaceHandlerObjectPath(InterfacePath)));
    if (!TestNotNull(TEXT("target blueprint loads"), TargetBlueprint)
        || !TestNotNull(TEXT("interface blueprint loads"), InterfaceBlueprint)
        || !TestNotNull(TEXT("interface generated class exists"),
            InterfaceBlueprint ? InterfaceBlueprint->GeneratedClass.Get() : nullptr))
    {
        return true;
    }

    UClass* InterfaceClass = InterfaceBlueprint->GeneratedClass.Get();
    const FString InterfaceShortName = FPackageName::GetLongPackageAssetName(InterfacePath);
    TestEqual(TEXT("target starts without interface"),
        CountImplementedInterface(TargetBlueprint, InterfaceClass), 0);

    FTestResponseCapture Capture;
    if (!InvokeInterfaceMutation(
        *this,
        TEXT("blueprint.add_interface"),
        TargetPath,
        InterfaceShortName,
        true,
        Capture))
    {
        return true;
    }
    TestEqual(TEXT("add populates ImplementedInterfaces"),
        CountImplementedInterface(TargetBlueprint, InterfaceClass), 1);

    const int32 CountAfterAdd = CountImplementedInterface(TargetBlueprint, InterfaceClass);
    if (!InvokeInterfaceMutation(
        *this,
        TEXT("blueprint.add_interface"),
        TargetPath,
        InterfaceShortName,
        false,
        Capture))
    {
        return true;
    }
    TestEqual(TEXT("re-add keeps implemented interface count unchanged"),
        CountImplementedInterface(TargetBlueprint, InterfaceClass), CountAfterAdd);

    if (!InvokeInterfaceMutation(
        *this,
        TEXT("blueprint.remove_interface"),
        TargetPath,
        InterfaceShortName,
        true,
        Capture))
    {
        return true;
    }
    TestEqual(TEXT("remove clears ImplementedInterfaces"),
        CountImplementedInterface(TargetBlueprint, InterfaceClass), 0);

    if (!InvokeInterfaceMutation(
        *this,
        TEXT("blueprint.remove_interface"),
        TargetPath,
        InterfaceShortName,
        false,
        Capture))
    {
        return true;
    }
    TestEqual(TEXT("second remove keeps implemented interface count at zero"),
        CountImplementedInterface(TargetBlueprint, InterfaceClass), 0);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintInterfaceRejectsNonInterfaceClassTest,
    "PinWright.blueprint.interface.RejectsNonInterfaceClass",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintInterfaceRejectsNonInterfaceClassTest::RunTest(const FString& Parameters)
{
    const FString TargetPath = MakeUniqueAssetPath(TEXT("BP_InterfaceRejectNonInterface"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(TargetPath);
    };

    if (!CreateBlueprintAsset(*this, TargetPath, TEXT("actor"), TEXT("target")))
    {
        return true;
    }

    UBlueprint* TargetBlueprint = Cast<UBlueprint>(UEditorAssetLibrary::LoadAsset(ToBlueprintInterfaceHandlerObjectPath(TargetPath)));
    if (!TestNotNull(TEXT("target blueprint loads"), TargetBlueprint))
    {
        return true;
    }

    const int32 InitialImplementedInterfaceCount = TargetBlueprint->ImplementedInterfaces.Num();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blueprintPath"), TargetPath);
    Payload->SetStringField(TEXT("interfaceClass"), TEXT("/Script/Engine.Actor"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("blueprint.add_interface handler found"),
        InvokeHandlerWithCapture(TEXT("blueprint.add_interface"), Payload, Capture));
    TestTrue(TEXT("blueprint.add_interface responded"), Capture.bWasCalled);
    TestFalse(TEXT("blueprint.add_interface rejects non-interface class"), Capture.bSuccess);
    TestEqual(TEXT("blueprint.add_interface error code"),
        Capture.ErrorCode, FString(TEXT("INVALID_INTERFACE_CLASS")));
    TestEqual(TEXT("ImplementedInterfaces unchanged after rejection"),
        TargetBlueprint->ImplementedInterfaces.Num(), InitialImplementedInterfaceCount);

    return true;
}
