// Copyright (c) 2026 Alexander Penkin. MIT License.

// TestMetaSoundPatchMutatorsAccept.cpp
//
// Regression test for board ticket B-metasound-patch-mutators-reject.
//
// The MetaSound authoring/IO mutators used to load their target with a TYPED gate
//   UMetaSoundSource* MetaSound = Cast<UMetaSoundSource>(
//       StaticLoadObject(UMetaSoundSource::StaticClass(), nullptr, *AssetPath));
// A UMetaSoundPatch is a *sibling* of UMetaSoundSource (both implement
// IMetaSoundDocumentInterface; neither derives from the other), so the typed load
// returned null for a Patch and the handler emitted a false [ASSET_NOT_FOUND] —
// even though describe_metasound reads the very same path fine. A created Patch was
// therefore write-only: it could never be populated.
//
// The fix routes those gates through PinWright::MetaSound::LoadMetaSoundDocumentObject,
// which loads generically and accepts any IMetaSoundDocumentInterface implementer
// (Source OR Patch). This test drives the PRODUCTION handler dispatch path
// (InvokeHandlerWithCapture -> the real audio.authoring.* handler) against a real
// UMetaSoundPatch asset and asserts the mutators succeed rather than rejecting the
// Patch with ASSET_NOT_FOUND.
//
// Counterfactual (proves the test guards the fix): revert any covered handler's load
// gate to the typed Cast<UMetaSoundSource>(StaticLoadObject(UMetaSoundSource...)) and
// add_metasound_input/add_metasound_node/validate_metasound on a Patch return
// ASSET_NOT_FOUND -> the corresponding TestNotEqual/TestTrue assertions fail.

#include "Misc/AutomationTest.h"

#if __has_include("MetasoundFactory.h")
#if __has_include("MetasoundSource.h")

#include "MetasoundSource.h"
#include "Metasound.h"
#include "MetasoundFactory.h"
#include "UObject/Package.h"
#include "Misc/Guid.h"
#include "Dom/JsonObject.h"

#include "Handlers/HandlerContext.h"
#include "Handlers/Audio/MetaSound/MetaSoundPathUtils.h"
#include "Tests/TestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMetaSoundPatchMutatorsAcceptTest,
    "PinWright.Assets.MetaSoundPatchMutatorsAccept",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMetaSoundPatchMutatorsAcceptTest::RunTest(const FString& Parameters)
{
    // --- 1. Create a real (non-transient) UMetaSoundPatch via the factory, so it is
    //        resolvable by package path the way the production load gate resolves it.
    //        UMetaSoundFactory::FactoryCreateNew runs InitAsset, which seeds the
    //        FMetasoundFrontendDocument's default page (otherwise the priming builder
    //        in the handlers would assert on an empty document). ---
    const FString PatchName = FString::Printf(
        TEXT("MS_PatchMutatorsTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString PackageName = FString::Printf(TEXT("/Game/PinWrightTests/%s"), *PatchName);

    UPackage* Package = CreatePackage(*PackageName);
    TestNotNull(TEXT("Patch package created"), Package);
    if (!Package)
    {
        return false;
    }

    UMetaSoundFactory* Factory = NewObject<UMetaSoundFactory>();
    UMetaSoundPatch* Patch = Cast<UMetaSoundPatch>(
        Factory->FactoryCreateNew(UMetaSoundPatch::StaticClass(), Package,
                                  FName(*PatchName), RF_Public | RF_Standalone,
                                  nullptr, GWarn));
    TestNotNull(TEXT("UMetaSoundFactory produced a UMetaSoundPatch"), Patch);
    if (!Patch)
    {
        Package->SetDirtyFlag(false);
        return false;
    }
    Patch->AddToRoot();

    // The production handlers resolve the asset by path; pass the object-path form
    // (Package.AssetName) so StaticLoadObject finds the in-memory document asset
    // rather than the UPackage.
    const FString AssetPath = ToObjectPath(PackageName);

    // Sanity: the Patch must be loadable as a MetaSound document by that path — this is
    // exactly what the production load gate does. If this is null the gate would (rightly)
    // report ASSET_NOT_FOUND for a genuinely-missing asset, which is NOT what we test.
#if __has_include("MetasoundDocumentInterface.h")
    TestNotNull(TEXT("LoadMetaSoundDocumentObject resolves the Patch by path"),
        PinWright::MetaSound::LoadMetaSoundDocumentObject(AssetPath));
#endif

    auto MakePayload = [&AssetPath]()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);
        // Never persist test artifacts to disk.
        Payload->SetBoolField(TEXT("save"), false);
        return Payload;
    };

    // --- 2. add_metasound_input on the Patch must succeed (was false ASSET_NOT_FOUND) ---
    {
        TSharedPtr<FJsonObject> Payload = MakePayload();
        Payload->SetStringField(TEXT("inputName"), TEXT("PatchGain"));
        Payload->SetStringField(TEXT("inputType"), TEXT("Float"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("add_metasound_input handler is registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.add_metasound_input"), Payload, Capture));
        // The load gate must not reject the Patch as missing.
        TestNotEqual(TEXT("add_metasound_input does NOT return ASSET_NOT_FOUND for a Patch"),
            Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
        TestTrue(TEXT("add_metasound_input succeeds on a Patch"), Capture.bSuccess);
    }

    // --- 3. add_metasound_node on the Patch must clear the load gate too. We assert only
    //        that it no longer rejects the Patch with ASSET_NOT_FOUND — whether the node
    //        itself resolves depends on the MetaSound node registry being populated, which
    //        is orthogonal to the load-gate defect this ticket is about. ---
    {
        TSharedPtr<FJsonObject> Payload = MakePayload();
        Payload->SetStringField(TEXT("nodeType"), TEXT("gain"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("add_metasound_node handler is registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.add_metasound_node"), Payload, Capture));
        TestNotEqual(TEXT("add_metasound_node does NOT return ASSET_NOT_FOUND for a Patch"),
            Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
    }

    // --- 4. validate_metasound (a different handler file: MetaSoundVariableHandler.cpp)
    //        must also accept the Patch, proving the fix spans the whole mutator family. ---
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), AssetPath);

        FTestResponseCapture Capture;
        TestTrue(TEXT("validate_metasound handler is registered"),
            InvokeHandlerWithCapture(TEXT("audio.authoring.validate_metasound"), Payload, Capture));
        TestNotEqual(TEXT("validate_metasound does NOT return ASSET_NOT_FOUND for a Patch"),
            Capture.ErrorCode, FString(TEXT("ASSET_NOT_FOUND")));
        TestTrue(TEXT("validate_metasound succeeds on a Patch"), Capture.bSuccess);
    }

    // Cleanup: never-saved real asset — clear dirty so no dirty package is left behind.
    Patch->RemoveFromRoot();
    Package->SetDirtyFlag(false);
    return true;
}

#endif // __has_include("MetasoundSource.h")
#endif // __has_include("MetasoundFactory.h")
