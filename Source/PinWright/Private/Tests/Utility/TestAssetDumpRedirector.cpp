// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Utils/AssetDumpBuilder.h"
#include "Handlers/Asset/AssetDumpHandler.h"
#include "Handlers/Asset/AssetDumpHandlerInternal.h"
#include "Utils/AssetDumpWriter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"


#include "Materials/Material.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"

// ============================================================================
// asset.dump.RedirectorEmitsRedirectsTo
// Regression for B-asset-dump-redirector-not-followed: a UObjectRedirector must
// be dumped as a redirector record. meta.json carries `redirectsTo` pointing at
// the destination's path; properties.json is `{ "redirectsTo": "<path>" }` rather
// than an opaque empty `{}`.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetDumpRedirectorEmitsRedirectsToTest,
    "PinWright.asset.dump.RedirectorEmitsRedirectsTo",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FAssetDumpRedirectorEmitsRedirectsToTest::RunTest(const FString& Parameters)
{
    // Use the UMaterial CDO as a stable, always-loaded redirect target so the
    // test doesn't depend on any project asset existing on disk.
    UObject* TargetObject = GetMutableDefault<UMaterial>();
    TestNotNull(TEXT("UMaterial CDO target is valid"), TargetObject);
    if (!TargetObject) return false;
    const FString ExpectedTargetPath = TargetObject->GetPathName();

    UPackage* TransientPkg = GetTransientPackage();
    TestNotNull(TEXT("Transient package is valid"), TransientPkg);
    if (!TransientPkg) return false;

    UObjectRedirector* Redirector = NewObject<UObjectRedirector>(
        TransientPkg, NAME_None, RF_Transient);
    TestNotNull(TEXT("Redirector created"), Redirector);
    if (!Redirector) return false;
    Redirector->DestinationObject = TargetObject;

    // Counterfactual: if the redirector branch in BuildAllFilesForAsset and the
    // redirectsTo field in BuildMetaJson are reverted, properties.json becomes
    // empty {} and meta.json has no redirectsTo key.

    // --- meta.json: BuildMetaJson must surface className=ObjectRedirector AND redirectsTo ---
    {
        TSharedPtr<FJsonObject> Meta = AssetDumpBuilder::BuildMetaJson(Redirector);
        TestNotNull(TEXT("Meta JSON is not null"), Meta.Get());
        if (!Meta.IsValid()) return false;

        TestEqual(TEXT("meta.className == ObjectRedirector"),
            Meta->GetStringField(TEXT("className")), FString(TEXT("ObjectRedirector")));

        TestTrue(TEXT("meta has redirectsTo key"),
            Meta->HasField(TEXT("redirectsTo")));
        TestEqual(TEXT("meta.redirectsTo == target path"),
            Meta->GetStringField(TEXT("redirectsTo")), ExpectedTargetPath);
    }

    // --- properties.json: redirector aspect emits { redirectsTo } not {} ---
    {
        TArray<AssetDumpWriter::FDumpFile> Files;
        TArray<TPair<FString, FString>> Errors;
        AssetDumpHandler::BuildRedirectorPropertiesAspect_Internal(Redirector, Files, &Errors);

        const AssetDumpWriter::FDumpFile* PropsFile = Files.FindByPredicate(
            [](const AssetDumpWriter::FDumpFile& F){ return F.Name == DumpFileNames::Properties; });
        TestNotNull(TEXT("properties.json present in Files"), PropsFile);
        if (!PropsFile) return false;

        TSharedPtr<FJsonObject> Properties;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PropsFile->Content);
        TestTrue(TEXT("properties.json parses as JSON object"),
            FJsonSerializer::Deserialize(Reader, Properties) && Properties.IsValid());
        if (!Properties.IsValid()) return false;

        TestTrue(TEXT("properties has redirectsTo key (not empty {})"),
            Properties->HasField(TEXT("redirectsTo")));
        TestEqual(TEXT("properties.redirectsTo == target path"),
            Properties->GetStringField(TEXT("redirectsTo")), ExpectedTargetPath);
    }

    return true;
}
