// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#if __has_include("MetasoundFrontendSearchEngine.h")
#define MCP_HAS_METASOUND_SEARCH_ENGINE 1
#else
#define MCP_HAS_METASOUND_SEARCH_ENGINE 0
#endif

#if __has_include("Metasound.h") \
    && __has_include("MetasoundDocumentInterface.h") \
    && __has_include("MetasoundFrontendDocument.h") \
    && __has_include("MetasoundFrontendDocumentBuilder.h") \
    && MCP_HAS_METASOUND_SEARCH_ENGINE

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Metasound.h"
#if __has_include("MetasoundSource.h")
#include "MetasoundSource.h"
#define MCP_TEST_HAS_METASOUND_SOURCE 1
#else
#define MCP_TEST_HAS_METASOUND_SOURCE 0
#endif
#include "MetasoundDocumentInterface.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendSearchEngine.h"
#include "Misc/Guid.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"
// Shared 5.6 paged-graphs compat helpers (interface Version/UClassOptions moved into Metadata).
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"

namespace
{
    // Shared transient-MetaSound-asset factory: GUID-named package under
    // /Game/PinWrightTests/, NewObject with the standard
    // RF_Public|RF_Standalone|RF_Transient flags, root it, init the builder document,
    // and (optionally) run an extra builder step (e.g. AddInterface) before finishing.
    // Single source of the creation convention so the per-type factories below cannot
    // drift on flags/rooting/builder lifecycle.
    template <typename TMetaSound>
    TMetaSound* NewTransientMetaSound(
        const TCHAR* NamePrefix,
        FString& OutObjectPath,
        FString* OutPackageName = nullptr,
        TFunctionRef<void(FMetaSoundFrontendDocumentBuilder&)> Configure =
            [](FMetaSoundFrontendDocumentBuilder&) {})
    {
        const FString AssetName = FString::Printf(
            TEXT("%s_%s"), NamePrefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString PackageName = FString::Printf(
            TEXT("/Game/PinWrightTests/%s"), *AssetName);
        UPackage* Package = CreatePackage(*PackageName);
        TMetaSound* Asset = NewObject<TMetaSound>(
            Package,
            TMetaSound::StaticClass(),
            *AssetName,
            RF_Public | RF_Standalone | RF_Transient);
        if (!Asset)
        {
            return nullptr;
        }

        Asset->AddToRoot();

        TScriptInterface<IMetaSoundDocumentInterface> DocInterface(Asset);
        FMetaSoundFrontendDocumentBuilder Builder(DocInterface);
        Builder.InitDocument();
        Configure(Builder);
        PW_METASOUND_FINISH_BUILDING(Builder);

        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
        if (OutPackageName)
        {
            *OutPackageName = PackageName;
        }
        return Asset;
    }

    UMetaSoundPatch* NewTransientInterfaceTestPatch(FString& OutObjectPath, FString* OutPackageName = nullptr)
    {
        return NewTransientMetaSound<UMetaSoundPatch>(TEXT("MS_InterfacePatch"), OutObjectPath, OutPackageName);
    }

#if MCP_TEST_HAS_METASOUND_SOURCE
    // Build a transient UMetaSoundSource with UE.OutputFormat.Mono declared. Mono is
    // a default output-format interface for a Source and is registered as
    // non-modifiable for UMetaSoundSource, so it is attached but cannot be detached —
    // the exact attached-but-non-modifiable case the remove handler must report
    // accurately (INTERFACE_NOT_REMOVABLE, not the misleading INTERFACE_NOT_FOUND).
    UMetaSoundSource* NewTransientSourceWithMonoInterface(FString& OutObjectPath)
    {
        return NewTransientMetaSound<UMetaSoundSource>(
            TEXT("MS_InterfaceSource"), OutObjectPath, nullptr,
            [](FMetaSoundFrontendDocumentBuilder& Builder)
            {
                // AddInterface(FName) does not enforce per-UClass modifiability, so it
                // attaches Mono even though it is non-modifiable for removal; if
                // InitDocument already declared it as the Source default, this is a
                // harmless no-op.
                Builder.AddInterface(FName(TEXT("UE.OutputFormat.Mono")));
            });
    }
#endif

#if __has_include("MetasoundFrontendSearchEngine.h")
    bool IsPatchModifiableInterface(const FMetasoundFrontendInterface& Interface)
    {
        // Delegates to the shared per-UClass modifiability rule (same definition the
        // remove handler uses) so the test and handler cannot disagree on the
        // default-when-no-match policy.
        return PinWright::MetaSound::IsInterfaceModifiableForClass(
            Interface, UMetaSoundPatch::StaticClass()->GetClassPathName());
    }

    bool FindPatchModifiableInterfaceName(FString& OutInterfaceName)
    {
        TArray<FMetasoundFrontendInterface> Interfaces =
            PinWright::MetaSound::FindAllFrontendInterfaces();

        for (const FMetasoundFrontendInterface& Interface : Interfaces)
        {
            // 5.6 relocated Version into Interface.Metadata; accessor reads per version.
            if (PinWright::MetaSound::GetInterfaceVersion(Interface).Name == FName(TEXT("UE.OutputFormat.Mono"))
                && IsPatchModifiableInterface(Interface))
            {
                OutInterfaceName = PinWright::MetaSound::GetInterfaceVersion(Interface).Name.ToString();
                return true;
            }
        }

        for (const FMetasoundFrontendInterface& Interface : Interfaces)
        {
            const FString Name = PinWright::MetaSound::GetInterfaceVersion(Interface).Name.ToString();
            if (Name.StartsWith(TEXT("UE.OutputFormat.")) && IsPatchModifiableInterface(Interface))
            {
                OutInterfaceName = Name;
                return true;
            }
        }

        for (const FMetasoundFrontendInterface& Interface : Interfaces)
        {
            if (IsPatchModifiableInterface(Interface))
            {
                OutInterfaceName = PinWright::MetaSound::GetInterfaceVersion(Interface).Name.ToString();
                return true;
            }
        }

        return false;
    }

    bool FindAbsentPatchModifiableInterfaceName(UMetaSoundPatch* Patch, FString& OutInterfaceName)
    {
        if (!Patch)
        {
            return false;
        }

        TScriptInterface<IMetaSoundDocumentInterface> DocInterface(Patch);
        FMetaSoundFrontendDocumentBuilder Builder(DocInterface);

        TArray<FMetasoundFrontendInterface> Interfaces =
            PinWright::MetaSound::FindAllFrontendInterfaces();

        for (const FMetasoundFrontendInterface& Interface : Interfaces)
        {
            if (IsPatchModifiableInterface(Interface)
                && !Builder.IsInterfaceDeclared(PinWright::MetaSound::GetInterfaceVersion(Interface)))
            {
                OutInterfaceName = PinWright::MetaSound::GetInterfaceVersion(Interface).Name.ToString();
                PW_METASOUND_FINISH_BUILDING(Builder);
                return true;
            }
        }

        PW_METASOUND_FINISH_BUILDING(Builder);
        return false;
    }

    // Reads the interface set off the MetaSound document itself.
    //
    // The add/remove RPCs echo the requested interface name straight back in
    // their JSON result, so `TestEqual(returned interfaceName, requested)` is an
    // echo check, not a state check: gutting the one line that does the work in
    // MetaSoundInterfaceHandler.cpp — `bool bAdded = Builder.AddInterface(...)`
    // followed by PW_METASOUND_FINISH_BUILDING — while leaving the response
    // fields in place keeps every assertion in this file green over a document
    // that was never modified. Name is distinctive because Unity merges TUs and
    // this lives in an anonymous namespace.
    bool MetaSoundInterfaceOpsTest_DocumentDeclaresInterfaceNamed(
        UObject* MetaSound, const FString& InterfaceName)
    {
        TScriptInterface<IMetaSoundDocumentInterface> DocInterface;
        if (!PinWright::MetaSound::TryMakeMetaSoundDocumentInterface(MetaSound, DocInterface))
        {
            return false;
        }
        const FMetasoundFrontendDocument& Document = PW_METASOUND_GET_CONST_DOCUMENT(DocInterface);
        for (const FMetasoundFrontendVersion& Version : Document.Interfaces)
        {
            if (Version.Name.ToString() == InterfaceName)
            {
                return true;
            }
        }
        return false;
    }
#endif
}

#if __has_include("MetasoundFrontendSearchEngine.h")
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FListMetaSoundInterfacesReturnsRealInterfaceNamesTest,
    "PinWright.Assets.MetaSound.Interfaces.ListReturnsRealInterfaceNames",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FListMetaSoundInterfacesReturnsRealInterfaceNamesTest::RunTest(const FString& Parameters)
{
    FTestResponseCapture Capture;
    TestTrue(TEXT("audio.authoring.list_metasound_interfaces handler found"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.list_metasound_interfaces"), MakeShared<FJsonObject>(), Capture));
    TestTrue(TEXT("audio.authoring.list_metasound_interfaces sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("audio.authoring.list_metasound_interfaces succeeded"), Capture.bSuccess);
    TestTrue(TEXT("audio.authoring.list_metasound_interfaces result present"), Capture.Result.IsValid());

    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Interfaces = nullptr;
    TestTrue(TEXT("interfaces array exists"), Capture.Result->TryGetArrayField(TEXT("interfaces"), Interfaces));

    double TotalCount = 0.0;
    TestTrue(TEXT("totalCount exists"), Capture.Result->TryGetNumberField(TEXT("totalCount"), TotalCount));
    TestTrue(TEXT("totalCount is positive"), TotalCount > 0.0);

    bool bFoundRealInterfaceName = false;
    if (Interfaces)
    {
        for (const TSharedPtr<FJsonValue>& InterfaceValue : *Interfaces)
        {
            const TSharedPtr<FJsonObject> InterfaceObject = InterfaceValue.IsValid() ? InterfaceValue->AsObject() : nullptr;
            FString InterfaceName;
            if (InterfaceObject.IsValid() && InterfaceObject->TryGetStringField(TEXT("name"), InterfaceName))
            {
                if (InterfaceName.StartsWith(TEXT("UE.Source"))
                    || InterfaceName.StartsWith(TEXT("UE.OutputFormat.")))
                {
                    bFoundRealInterfaceName = true;
                    break;
                }
            }
        }
    }
    TestTrue(TEXT("interfaces include a real UE frontend interface name"), bFoundRealInterfaceName);

    FString Note;
    if (Capture.Result->TryGetStringField(TEXT("note"), Note))
    {
        TestFalse(TEXT("note does not recommend MetaSoundSource as an interface name"),
            Note.Contains(TEXT("MetaSoundSource")));
        TestFalse(TEXT("note does not recommend MetaSoundPatch as an interface name"),
            Note.Contains(TEXT("MetaSoundPatch")));
    }

    return Capture.bSuccess && TotalCount > 0.0 && bFoundRealInterfaceName;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddMetaSoundInterfaceAcceptsListedPatchInterfaceTest,
    "PinWright.Assets.MetaSound.Interfaces.AddAcceptsListedPatchInterface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddMetaSoundInterfaceAcceptsListedPatchInterfaceTest::RunTest(const FString& Parameters)
{
    FString InterfaceName;
    if (!TestTrue(TEXT("patch-modifiable MetaSound interface found"),
        FindPatchModifiableInterfaceName(InterfaceName)))
    {
        return false;
    }

    FString ObjectPath;
    UMetaSoundPatch* Patch = NewTransientInterfaceTestPatch(ObjectPath);
    TestNotNull(TEXT("Transient MetaSoundPatch created"), Patch);
    if (!Patch)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("interfaceName"), InterfaceName);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("audio.authoring.add_metasound_interface handler found"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.add_metasound_interface"), Payload, Capture));
    TestTrue(TEXT("audio.authoring.add_metasound_interface sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("audio.authoring.add_metasound_interface succeeded"), Capture.bSuccess);
    TestTrue(TEXT("audio.authoring.add_metasound_interface result present"), Capture.Result.IsValid());

    bool bInterfaceNameReturned = false;
    if (Capture.Result.IsValid())
    {
        FString ReturnedInterfaceName;
        bInterfaceNameReturned = Capture.Result->TryGetStringField(TEXT("interfaceName"), ReturnedInterfaceName);
        TestTrue(TEXT("interfaceName returned"), bInterfaceNameReturned);
        TestEqual(TEXT("interfaceName matches requested listed interface"), ReturnedInterfaceName, InterfaceName);
    }

    // The assertions above only read the response envelope, which the handler
    // fills in from the request. This one reads the asset.
    TestTrue(TEXT("interface is declared on the MetaSound document after the add"),
        MetaSoundInterfaceOpsTest_DocumentDeclaresInterfaceNamed(Patch, InterfaceName));

    Patch->RemoveFromRoot();
    return Capture.bSuccess && bInterfaceNameReturned;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAddMetaSoundInterfaceAcceptsBarePackagePathTest,
    "PinWright.Assets.MetaSound.Interfaces.AddAcceptsBarePackagePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAddMetaSoundInterfaceAcceptsBarePackagePathTest::RunTest(const FString& Parameters)
{
    // Regression: bare long-package paths (the form audio.authoring.create_metasound
    // returns, e.g. "/Game/.../MS_Foo") must resolve to the inner document asset,
    // not the UPackage. Without the LoadMetaSoundDocumentAsset retry fix in
    // MetaSoundPathUtils.cpp, StaticFindObject hands back the UPackage for the
    // bare path and add_metasound_interface rejects with INTERFACE_ERROR.
    FString InterfaceName;
    if (!TestTrue(TEXT("patch-modifiable MetaSound interface found"),
        FindPatchModifiableInterfaceName(InterfaceName)))
    {
        return false;
    }

    FString ObjectPath;
    FString PackageName;
    UMetaSoundPatch* Patch = NewTransientInterfaceTestPatch(ObjectPath, &PackageName);
    TestNotNull(TEXT("Transient MetaSoundPatch created"), Patch);
    if (!Patch)
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), PackageName);
    Payload->SetStringField(TEXT("interfaceName"), InterfaceName);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("audio.authoring.add_metasound_interface handler found"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.add_metasound_interface"), Payload, Capture));
    TestTrue(TEXT("audio.authoring.add_metasound_interface sent a response"), Capture.bWasCalled);
    TestTrue(TEXT("bare package path resolved to MetaSound document asset"), Capture.bSuccess);
    TestNotEqual(TEXT("bare package path did not fall back to INTERFACE_ERROR"),
        Capture.ErrorCode,
        FString(TEXT("INTERFACE_ERROR")));

    bool bInterfaceNameReturned = false;
    if (Capture.bSuccess && Capture.Result.IsValid())
    {
        FString ReturnedInterfaceName;
        bInterfaceNameReturned = Capture.Result->TryGetStringField(TEXT("interfaceName"), ReturnedInterfaceName);
        TestTrue(TEXT("interfaceName returned"), bInterfaceNameReturned);
        TestEqual(TEXT("interfaceName matches requested listed interface"), ReturnedInterfaceName, InterfaceName);
    }

    // Path resolution is what this test is named for, and a bare-package path
    // that resolved to the UPackage instead of the document would still let the
    // handler echo the name back. Only reading the document proves the write
    // landed on the right object.
    TestTrue(TEXT("interface is declared on the MetaSound document after the bare-path add"),
        MetaSoundInterfaceOpsTest_DocumentDeclaresInterfaceNamed(Patch, InterfaceName));

    Patch->RemoveFromRoot();
    return Capture.bSuccess && bInterfaceNameReturned;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRemoveMetaSoundInterfaceRejectsAbsentRegisteredInterfaceTest,
    "PinWright.Assets.MetaSound.Interfaces.RemoveRejectsAbsentRegisteredInterface",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRemoveMetaSoundInterfaceRejectsAbsentRegisteredInterfaceTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UMetaSoundPatch* Patch = NewTransientInterfaceTestPatch(ObjectPath);
    TestNotNull(TEXT("Transient MetaSoundPatch created"), Patch);
    if (!Patch)
    {
        return false;
    }

    FString InterfaceName;
    const bool bFoundAbsentInterface = FindAbsentPatchModifiableInterfaceName(Patch, InterfaceName);
    TestTrue(TEXT("absent registered patch-modifiable MetaSound interface found"), bFoundAbsentInterface);
    if (!bFoundAbsentInterface)
    {
        Patch->RemoveFromRoot();
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("interfaceName"), InterfaceName);
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("audio.authoring.remove_metasound_interface handler found"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.remove_metasound_interface"), Payload, Capture));
    TestTrue(TEXT("audio.authoring.remove_metasound_interface sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("audio.authoring.remove_metasound_interface rejected absent registered interface"), Capture.bSuccess);
    TestEqual(TEXT("absent registered interface reports INTERFACE_NOT_FOUND"),
        Capture.ErrorCode,
        FString(TEXT("INTERFACE_NOT_FOUND")));

    Patch->RemoveFromRoot();
    return Capture.bWasCalled
        && !Capture.bSuccess
        && Capture.ErrorCode == TEXT("INTERFACE_NOT_FOUND");
}

#if MCP_TEST_HAS_METASOUND_SOURCE
// Regression for B-metasound-remove-interface-not-found: removing an interface that
// IS attached but is a mandatory/non-modifiable default for the asset's UClass
// (UE.OutputFormat.Mono on a UMetaSoundSource) must report INTERFACE_NOT_REMOVABLE
// with an accurate "is attached but ... cannot be detached" message — NOT the
// misleading INTERFACE_NOT_FOUND "is not attached" the handler emitted before the
// fix (the engine's RemoveInterface refuses non-modifiable interfaces AFTER
// confirming they are present on the document).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRemoveMetaSoundInterfaceReportsNonModifiableDefaultTest,
    "PinWright.Assets.MetaSound.Interfaces.RemoveReportsNonModifiableDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRemoveMetaSoundInterfaceReportsNonModifiableDefaultTest::RunTest(const FString& Parameters)
{
    FString ObjectPath;
    UMetaSoundSource* Source = NewTransientSourceWithMonoInterface(ObjectPath);
    TestNotNull(TEXT("Transient MetaSoundSource with Mono interface created"), Source);
    if (!Source)
    {
        return false;
    }

    // Confirm the interface really is declared on the document — the bug is about an
    // ATTACHED-but-non-modifiable interface, so the test is only meaningful if Mono
    // is present.
    TScriptInterface<IMetaSoundDocumentInterface> DocInterface(Source);
    FMetaSoundFrontendDocumentBuilder VerifyBuilder(DocInterface);
    const bool bMonoDeclared = VerifyBuilder.IsInterfaceDeclared(FName(TEXT("UE.OutputFormat.Mono")));
    PW_METASOUND_FINISH_BUILDING(VerifyBuilder);
    TestTrue(TEXT("UE.OutputFormat.Mono is declared on the Source document"), bMonoDeclared);
    if (!bMonoDeclared)
    {
        Source->RemoveFromRoot();
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("assetPath"), ObjectPath);
    Payload->SetStringField(TEXT("interfaceName"), TEXT("UE.OutputFormat.Mono"));
    Payload->SetBoolField(TEXT("save"), false);

    FTestResponseCapture Capture;
    TestTrue(TEXT("audio.authoring.remove_metasound_interface handler found"),
        InvokeHandlerWithCapture(TEXT("audio.authoring.remove_metasound_interface"), Payload, Capture));
    TestTrue(TEXT("audio.authoring.remove_metasound_interface sent a response"), Capture.bWasCalled);
    TestFalse(TEXT("removing a non-modifiable default interface fails"), Capture.bSuccess);
    // The core regression assertion: the error must be INTERFACE_NOT_REMOVABLE, NOT the
    // misleading INTERFACE_NOT_FOUND "is not attached" (an exact match here already
    // proves the code is not INTERFACE_NOT_FOUND).
    TestEqual(TEXT("attached non-modifiable interface reports INTERFACE_NOT_REMOVABLE"),
        Capture.ErrorCode,
        FString(TEXT("INTERFACE_NOT_REMOVABLE")));

    Source->RemoveFromRoot();
    return Capture.bWasCalled
        && !Capture.bSuccess
        && Capture.ErrorCode == TEXT("INTERFACE_NOT_REMOVABLE");
}
#endif
#endif

#endif // MetaSound interface test dependencies
