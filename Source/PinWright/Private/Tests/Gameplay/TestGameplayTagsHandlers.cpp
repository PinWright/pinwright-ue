// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace
{
bool InvokeGameplayTagsHandler(
    FAutomationTestBase& Test,
    const FString& Method,
    const TSharedPtr<FJsonObject>& Payload,
    FTestResponseCapture& Capture)
{
    const bool bFound = InvokeHandlerWithCapture(Method, Payload, Capture);
    Test.TestTrue(FString::Printf(TEXT("%s handler found"), *Method), bFound);
    Test.TestTrue(FString::Printf(TEXT("%s sent a response"), *Method), Capture.bWasCalled);
    Test.TestTrue(FString::Printf(TEXT("%s succeeded"), *Method), Capture.bSuccess);
    Test.TestTrue(FString::Printf(TEXT("%s returned a result"), *Method), Capture.Result.IsValid());
    return bFound && Capture.bWasCalled && Capture.bSuccess && Capture.Result.IsValid();
}

bool FindTagRow(
    const TArray<TSharedPtr<FJsonValue>>& Rows,
    const FString& ExpectedTag,
    TSharedPtr<FJsonObject>& OutRow)
{
    for (const TSharedPtr<FJsonValue>& RowValue : Rows)
    {
        const TSharedPtr<FJsonObject>* RowObject = nullptr;
        if (RowValue.IsValid() && RowValue->TryGetObject(RowObject) && RowObject && RowObject->IsValid())
        {
            if ((*RowObject)->GetStringField(TEXT("name")) == ExpectedTag)
            {
                OutRow = *RowObject;
                return true;
            }
        }
    }

    return false;
}

FName SourceNameFromString(const FString& Source)
{
    return FName(*Source);
}

bool SetTagInSource(const FString& Source, const FString& Tag, const FString& Comment, const bool bPresent)
{
    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    const FName SourceName = SourceNameFromString(Source);
    const FName TagName(*Tag);

    FGameplayTagSource* TagSource = Manager.FindTagSource(SourceName);
    if (!TagSource || !TagSource->SourceTagList)
    {
        return false;
    }

    UGameplayTagsList* TagList = TagSource->SourceTagList;
    TagList->Modify();
    TagList->GameplayTagList.RemoveAll([TagName](const FGameplayTagTableRow& Row)
    {
        return Row.Tag == TagName;
    });

    if (bPresent)
    {
        TagList->GameplayTagList.Add(FGameplayTagTableRow(TagName, Comment));
        TagList->SortTags();
    }

    TagList->TryUpdateDefaultConfigFile(TagList->ConfigFileName);
    GConfig->LoadFile(TagList->ConfigFileName);
    Manager.EditorRefreshGameplayTagTree();
    return true;
}

// Removes a gameplay-tag source created by a test. UE 5.6 has no public
// remove-source API, so the source stays registered for the editor session; a
// later TryUpdateDefaultConfigFile on it would re-serialize the .ini after we
// delete it. Empty the in-memory tag list first, then delete the
// Config/Tags/<name>.ini file so the test leaves no artifact on disk.
void CleanupTagSource(const FString& Source)
{
    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    if (FGameplayTagSource* TagSource = Manager.FindTagSource(SourceNameFromString(Source)))
    {
        if (TagSource->SourceTagList)
        {
            TagSource->SourceTagList->GameplayTagList.Empty();
        }
    }

    const FString ConfigPath = FPaths::ProjectConfigDir() / TEXT("Tags") / Source;
    IFileManager::Get().Delete(*ConfigPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);
}

// Saves a Blueprint whose CDO stores Tag in an FGameplayTag member, then force-rescans its file so
// the asset registry holds the SearchableName edge (written by FGameplayTag::PostSerialize on save)
// that both gameplay_tags.find_referencers and the engine's delete check read. Tag must already
// be registered, or the member default does not import.
bool SaveTagReferencingBlueprint(FAutomationTestBase& Test, const FString& Tag, FString& OutPackagePath)
{
    OutPackagePath = FString::Printf(TEXT("/Game/PinWrightTests/PWTagRef_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    UPackage* Package = CreatePackage(*OutPackagePath);
    UBlueprint* Blueprint = Package ? FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(), Package, FName(*FPackageName::GetLongPackageAssetName(OutPackagePath)),
        BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()) : nullptr;
    if (!Test.TestNotNull(TEXT("fixture blueprint created"), Blueprint))
    {
        return false;
    }

    FEdGraphPinType PinType;
    PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
    PinType.PinSubCategoryObject = FGameplayTag::StaticStruct();
    FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("FixtureTag"), PinType,
        FString::Printf(TEXT("(TagName=\"%s\")"), *Tag));
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    // Known-good control: the CDO must really hold the tag, or an empty referencer list below
    // would be the fixture's failure, not the verb's.
    const FStructProperty* TagProperty = Blueprint->GeneratedClass
        ? FindFProperty<FStructProperty>(Blueprint->GeneratedClass, TEXT("FixtureTag")) : nullptr;
    const FGameplayTag* Stored = TagProperty
        ? TagProperty->ContainerPtrToValuePtr<FGameplayTag>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr;
    if (!Test.TestTrue(TEXT("fixture CDO stores the tag"), Stored && Stored->GetTagName() == FName(*Tag)))
    {
        return false;
    }

    FString Filename;
    if (!Test.TestTrue(TEXT("fixture saved"), UEditorAssetLibrary::SaveAsset(OutPackagePath, /*bOnlyIfIsDirty=*/false))
        || !Test.TestTrue(TEXT("fixture filename resolves"), FPackageName::TryConvertLongPackageNameToFilename(
            OutPackagePath, Filename, FPackageName::GetAssetPackageExtension())))
    {
        return false;
    }
    IAssetRegistry::GetChecked().ScanFilesSynchronous({Filename}, /*bForceRescan=*/true);
    return true;
}

bool ReferencersContainPackage(const TSharedPtr<FJsonObject>& Object, const FString& PackagePath)
{
    const TArray<TSharedPtr<FJsonValue>>* Referencers = nullptr;
    if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("referencers"), Referencers) || !Referencers)
    {
        return false;
    }
    for (const TSharedPtr<FJsonValue>& Value : *Referencers)
    {
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if (Value.IsValid() && Value->TryGetObject(Row) && Row && (*Row)->GetStringField(TEXT("packageName")) == PackagePath)
        {
            return true;
        }
    }
    return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameplayTagsRegistryRoundTripTest,
    "PinWright.gameplay_tags.RegistryRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameplayTagsRegistryRoundTripTest::RunTest(const FString& Parameters)
{
    const FString Source = TEXT("McpAutomationRoundTripTags.ini");
    const FString Tag = TEXT("Mcp.Automation.Registry.RoundTrip");
    const FString Comment = TEXT("PinWright registry round-trip test tag");
    const FString Prefix = TEXT("Mcp.Automation.Registry");

    // Remove the tag source this test creates (the .ini under Config/Tags) on every exit path.
    ON_SCOPE_EXIT { CleanupTagSource(Source); };

    FTestResponseCapture Capture;

    TSharedPtr<FJsonObject> AddSourcePayload = MakeShared<FJsonObject>();
    AddSourcePayload->SetStringField(TEXT("source"), Source);
    TestTrue(TEXT("add_source succeeds"),
        InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.add_source"), AddSourcePayload, Capture));

    TSharedPtr<FJsonObject> PreRemovePayload = MakeShared<FJsonObject>();
    PreRemovePayload->SetStringField(TEXT("tag"), Tag);
    PreRemovePayload->SetStringField(TEXT("source"), Source);
    InvokeHandlerWithCapture(TEXT("gameplay_tags.remove"), PreRemovePayload, Capture);

    TSharedPtr<FJsonObject> AddPayload = MakeShared<FJsonObject>();
    AddPayload->SetStringField(TEXT("tag"), Tag);
    AddPayload->SetStringField(TEXT("source"), Source);
    AddPayload->SetStringField(TEXT("comment"), Comment);
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.add"), AddPayload, Capture))
    {
        return false;
    }

    bool bAlreadyExisted = true;
    TestTrue(TEXT("add returns alreadyExisted"), Capture.Result->TryGetBoolField(TEXT("alreadyExisted"), bAlreadyExisted));
    TestFalse(TEXT("add reports new tag"), bAlreadyExisted);

    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("prefix"), Prefix);
    ListPayload->SetStringField(TEXT("source"), Source);
    ListPayload->SetBoolField(TEXT("includeTotal"), true);
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.list"), ListPayload, Capture))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    TestTrue(TEXT("list returns tags array"), Capture.Result->TryGetArrayField(TEXT("tags"), Rows) && Rows);

    TSharedPtr<FJsonObject> Row;
    TestTrue(TEXT("list contains round-trip tag"), Rows && FindTagRow(*Rows, Tag, Row));
    if (Row.IsValid())
    {
        TestEqual(TEXT("listed tag source"), Row->GetStringField(TEXT("source")), Source);
        TestEqual(TEXT("listed tag comment"), Row->GetStringField(TEXT("comment")), Comment);

        bool bIsExplicit = false;
        TestTrue(TEXT("listed tag has isExplicit"), Row->TryGetBoolField(TEXT("isExplicit"), bIsExplicit));
        TestTrue(TEXT("listed tag is explicit"), bIsExplicit);
    }

    TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
    RemovePayload->SetStringField(TEXT("tag"), Tag);
    RemovePayload->SetStringField(TEXT("source"), Source);
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.remove"), RemovePayload, Capture))
    {
        return false;
    }

    bool bRemoved = false;
    TestTrue(TEXT("remove returns removed"), Capture.Result->TryGetBoolField(TEXT("removed"), bRemoved));
    TestTrue(TEXT("remove reports tag removed"), bRemoved);

    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.list"), ListPayload, Capture))
    {
        return false;
    }

    Rows = nullptr;
    TestTrue(TEXT("follow-up list returns tags array"), Capture.Result->TryGetArrayField(TEXT("tags"), Rows) && Rows);
    TestFalse(TEXT("follow-up list does not contain removed tag"), Rows && FindTagRow(*Rows, Tag, Row));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameplayTagsMultiSourceRemoveTest,
    "PinWright.gameplay_tags.MultiSourceRemove",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameplayTagsMultiSourceRemoveTest::RunTest(const FString& Parameters)
{
    const FString SourceA = TEXT("McpAutomationMultiSourceA.ini");
    const FString SourceB = TEXT("McpAutomationMultiSourceB.ini");
    const FString Tag = TEXT("Mcp.Automation.Registry.MultiSource");
    const FString Prefix = TEXT("Mcp.Automation.Registry.MultiSource");

    // Remove both tag sources this test creates (the .ini files under Config/Tags) on every exit path.
    ON_SCOPE_EXIT { CleanupTagSource(SourceA); CleanupTagSource(SourceB); };

    FTestResponseCapture Capture;

    const FString Sources[] = {SourceA, SourceB};
    for (const FString& Source : Sources)
    {
        TSharedPtr<FJsonObject> AddSourcePayload = MakeShared<FJsonObject>();
        AddSourcePayload->SetStringField(TEXT("source"), Source);
        if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.add_source"), AddSourcePayload, Capture))
        {
            return false;
        }

        SetTagInSource(Source, Tag, FString(), false);
    }

    TSharedPtr<FJsonObject> AddTagPayload = MakeShared<FJsonObject>();
    AddTagPayload->SetStringField(TEXT("tag"), Tag);
    AddTagPayload->SetStringField(TEXT("source"), SourceA);
    AddTagPayload->SetStringField(TEXT("comment"), TEXT("source A"));
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.add"), AddTagPayload, Capture))
    {
        return false;
    }

    bool bAlreadyExisted = true;
    TestTrue(TEXT("source A add returns alreadyExisted"), Capture.Result->TryGetBoolField(TEXT("alreadyExisted"), bAlreadyExisted));
    TestFalse(TEXT("source A add reports new source entry"), bAlreadyExisted);

    AddTagPayload->SetStringField(TEXT("source"), SourceB);
    AddTagPayload->SetStringField(TEXT("comment"), TEXT("source B"));
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.add"), AddTagPayload, Capture))
    {
        return false;
    }

    bAlreadyExisted = true;
    TestTrue(TEXT("source B add returns alreadyExisted"), Capture.Result->TryGetBoolField(TEXT("alreadyExisted"), bAlreadyExisted));
    TestFalse(TEXT("source B add reports new source entry"), bAlreadyExisted);
    TestEqual(TEXT("source B add reports requested source"), Capture.Result->GetStringField(TEXT("source")), SourceB);

    TSharedPtr<FJsonObject> RemovePayload = MakeShared<FJsonObject>();
    RemovePayload->SetStringField(TEXT("tag"), Tag);
    RemovePayload->SetStringField(TEXT("source"), SourceA);
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.remove"), RemovePayload, Capture))
    {
        return false;
    }

    bool bRemoved = false;
    TestTrue(TEXT("multi-source remove returns removed"), Capture.Result->TryGetBoolField(TEXT("removed"), bRemoved));
    TestTrue(TEXT("multi-source remove reports true"), bRemoved);
    TestEqual(TEXT("multi-source remove reports requested source"), Capture.Result->GetStringField(TEXT("source")), SourceA);

    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("prefix"), Prefix);
    ListPayload->SetStringField(TEXT("source"), SourceA);
    ListPayload->SetBoolField(TEXT("includeTotal"), true);
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.list"), ListPayload, Capture))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    TSharedPtr<FJsonObject> Row;
    TestTrue(TEXT("source A list returns rows"), Capture.Result->TryGetArrayField(TEXT("tags"), Rows) && Rows);
    TestFalse(TEXT("source A no longer contains tag"), Rows && FindTagRow(*Rows, Tag, Row));

    ListPayload->SetStringField(TEXT("source"), SourceB);
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.list"), ListPayload, Capture))
    {
        return false;
    }

    Rows = nullptr;
    Row.Reset();
    TestTrue(TEXT("source B list returns rows"), Capture.Result->TryGetArrayField(TEXT("tags"), Rows) && Rows);
    TestTrue(TEXT("source B still contains tag"), Rows && FindTagRow(*Rows, Tag, Row));

    TSharedPtr<FJsonObject> CleanupPayload = MakeShared<FJsonObject>();
    CleanupPayload->SetStringField(TEXT("tag"), Tag);
    CleanupPayload->SetStringField(TEXT("source"), SourceB);
    InvokeHandlerWithCapture(TEXT("gameplay_tags.remove"), CleanupPayload, Capture);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameplayTagsListBoundedDefaultTest,
    "PinWright.gameplay_tags.ListBoundedDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameplayTagsListBoundedDefaultTest::RunTest(const FString& Parameters)
{
    const FString Source = TEXT("McpAutomationBoundedListTags.ini");
    const FString Prefix = TEXT("Mcp.Automation.Registry.Bounded.");
    const FString TagA = Prefix + TEXT("A");
    const FString TagB = Prefix + TEXT("B");

    // Remove the tag source this test creates (the .ini under Config/Tags) on every exit path.
    ON_SCOPE_EXIT { CleanupTagSource(Source); };

    FTestResponseCapture Capture;

    TSharedPtr<FJsonObject> AddSourcePayload = MakeShared<FJsonObject>();
    AddSourcePayload->SetStringField(TEXT("source"), Source);
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.add_source"), AddSourcePayload, Capture))
    {
        return false;
    }

    SetTagInSource(Source, TagA, FString(), false);
    SetTagInSource(Source, TagB, FString(), false);
    TestTrue(TEXT("bounded list setup writes tag A"), SetTagInSource(Source, TagA, TEXT("bounded A"), true));
    TestTrue(TEXT("bounded list setup writes tag B"), SetTagInSource(Source, TagB, TEXT("bounded B"), true));

    TSharedPtr<FJsonObject> ListPayload = MakeShared<FJsonObject>();
    ListPayload->SetStringField(TEXT("prefix"), Prefix);
    ListPayload->SetStringField(TEXT("source"), Source);
    ListPayload->SetNumberField(TEXT("limit"), 1);
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.list"), ListPayload, Capture))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    TestTrue(TEXT("bounded list returns tags array"), Capture.Result->TryGetArrayField(TEXT("tags"), Rows) && Rows);
    TestEqual(TEXT("bounded list returns only requested limit"), Rows ? Rows->Num() : 0, 1);

    int32 TotalMatches = 0;
    bool bTotalMatchesExact = true;
    bool bTruncated = false;
    TestTrue(TEXT("bounded list totalMatches present"), Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches));
    TestEqual(TEXT("bounded list reports collected lower-bound count"), TotalMatches, 1);
    TestTrue(TEXT("bounded list totalMatchesExact present"), Capture.Result->TryGetBoolField(TEXT("totalMatchesExact"), bTotalMatchesExact));
    TestFalse(TEXT("bounded list default total is not exact at limit"), bTotalMatchesExact);
    TestTrue(TEXT("bounded list truncated present"), Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
    TestTrue(TEXT("bounded list reports limit truncation"), bTruncated);

    TSharedPtr<FJsonObject> Row;
    TestTrue(TEXT("bounded list row is one of seeded tags"),
        Rows && ((FindTagRow(*Rows, TagA, Row) || FindTagRow(*Rows, TagB, Row))));

    SetTagInSource(Source, TagA, FString(), false);
    SetTagInSource(Source, TagB, FString(), false);

    return true;
}

// Regression for E-gameplay-tags-list-default-limit-spills: the per-row field
// projection (namesOnly / fields) must drop the byte-dominating configFile path
// + sourceType (the columns that push an un-prefixed overview past the inline
// budget), while the unprojected default keeps every column byte-identical to the
// prior shape. Reverting either the projection wiring or the namesOnly column set
// fails this test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameplayTagsListFieldProjectionTest,
    "PinWright.gameplay_tags.ListFieldProjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameplayTagsListFieldProjectionTest::RunTest(const FString& Parameters)
{
    const FString Source = TEXT("McpAutomationProjectionTags.ini");
    const FString Prefix = TEXT("Mcp.Automation.Registry.Projection.");
    const FString Tag = Prefix + TEXT("Row");
    const FString Comment = TEXT("projection test comment");

    // Remove the tag source this test creates (the .ini under Config/Tags) on every exit path.
    ON_SCOPE_EXIT { CleanupTagSource(Source); };

    FTestResponseCapture Capture;

    TSharedPtr<FJsonObject> AddSourcePayload = MakeShared<FJsonObject>();
    AddSourcePayload->SetStringField(TEXT("source"), Source);
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.add_source"), AddSourcePayload, Capture))
    {
        return false;
    }

    SetTagInSource(Source, Tag, FString(), false);
    TestTrue(TEXT("projection setup writes tag"), SetTagInSource(Source, Tag, Comment, true));

    // Builds a base prefix/source-narrowed list payload (the narrowing keeps the
    // result deterministic regardless of how many tags the host project carries).
    auto MakeListPayload = [&Prefix, &Source]() -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("prefix"), Prefix);
        Payload->SetStringField(TEXT("source"), Source);
        return Payload;
    };

    // Baseline: no projection -> every column present, including the fat configFile
    // path + sourceType. Confirms the projection path is opt-in and leaves the
    // default response shape unchanged.
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.list"), MakeListPayload(), Capture))
    {
        return false;
    }
    {
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        TSharedPtr<FJsonObject> Row;
        TestTrue(TEXT("baseline list returns tags array"), Capture.Result->TryGetArrayField(TEXT("tags"), Rows) && Rows);
        TestTrue(TEXT("baseline list contains seeded tag"), Rows && FindTagRow(*Rows, Tag, Row));
        if (Row.IsValid())
        {
            TestTrue(TEXT("baseline row has name"), Row->HasField(TEXT("name")));
            TestTrue(TEXT("baseline row has comment"), Row->HasField(TEXT("comment")));
            TestTrue(TEXT("baseline row has isExplicit"), Row->HasField(TEXT("isExplicit")));
            TestTrue(TEXT("baseline row has sourceType"), Row->HasField(TEXT("sourceType")));
            TestTrue(TEXT("baseline row has configFile"), Row->HasField(TEXT("configFile")));
        }
    }

    // namesOnly: drops the byte-dominating configFile + sourceType (and comment),
    // keeps name + source + isExplicit.
    {
        TSharedPtr<FJsonObject> Payload = MakeListPayload();
        Payload->SetBoolField(TEXT("namesOnly"), true);
        if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.list"), Payload, Capture))
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        TSharedPtr<FJsonObject> Row;
        TestTrue(TEXT("namesOnly list returns tags array"), Capture.Result->TryGetArrayField(TEXT("tags"), Rows) && Rows);
        TestTrue(TEXT("namesOnly list contains seeded tag"), Rows && FindTagRow(*Rows, Tag, Row));
        if (Row.IsValid())
        {
            TestTrue(TEXT("namesOnly row keeps name"), Row->HasField(TEXT("name")));
            TestTrue(TEXT("namesOnly row keeps source"), Row->HasField(TEXT("source")));
            TestTrue(TEXT("namesOnly row keeps isExplicit"), Row->HasField(TEXT("isExplicit")));
            TestFalse(TEXT("namesOnly row drops configFile"), Row->HasField(TEXT("configFile")));
            TestFalse(TEXT("namesOnly row drops sourceType"), Row->HasField(TEXT("sourceType")));
            TestFalse(TEXT("namesOnly row drops comment"), Row->HasField(TEXT("comment")));
        }
    }

    // fields allow-list: an explicit ["name"] wins over (and without) namesOnly,
    // emitting only the requested column.
    {
        TSharedPtr<FJsonObject> Payload = MakeListPayload();
        TArray<TSharedPtr<FJsonValue>> FieldArray;
        FieldArray.Add(MakeShared<FJsonValueString>(TEXT("name")));
        Payload->SetArrayField(TEXT("fields"), FieldArray);
        if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.list"), Payload, Capture))
        {
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        TSharedPtr<FJsonObject> Row;
        TestTrue(TEXT("fields list returns tags array"), Capture.Result->TryGetArrayField(TEXT("tags"), Rows) && Rows);
        TestTrue(TEXT("fields list contains seeded tag"), Rows && FindTagRow(*Rows, Tag, Row));
        if (Row.IsValid())
        {
            TestTrue(TEXT("fields=[name] row keeps name"), Row->HasField(TEXT("name")));
            TestFalse(TEXT("fields=[name] row drops source"), Row->HasField(TEXT("source")));
            TestFalse(TEXT("fields=[name] row drops isExplicit"), Row->HasField(TEXT("isExplicit")));
            TestFalse(TEXT("fields=[name] row drops configFile"), Row->HasField(TEXT("configFile")));
            TestFalse(TEXT("fields=[name] row drops sourceType"), Row->HasField(TEXT("sourceType")));
        }
    }

    // An out-of-range limit must be survivable and must not change the projected result.
    //
    // KNOWN GAP — the [1, 500] upper clamp itself is NOT pinned by this block, and cannot be at
    // this fixture's scale. The payload narrows by prefix AND source and the test seeds exactly
    // one tag, so the response carries one row whether the clamp is present, absent, or
    // hardcoded to 100000. Pinning the upper bound needs either a >500-tag fixture or a resolved
    // `limit` echoed in the handler's response (GameplayTagsHandler.cpp emits only tags /
    // totalMatches / totalMatchesExact / truncated, so there is nothing to read back today).
    // What IS asserted here is exact and falsifiable at this scale: the array must exist, and an
    // absurd limit must neither drop the row nor duplicate it, and must leave the honesty fields
    // reporting an untruncated exact count.
    {
        TSharedPtr<FJsonObject> Payload = MakeListPayload();
        Payload->SetBoolField(TEXT("namesOnly"), true);
        Payload->SetNumberField(TEXT("limit"), 100000);
        if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.list"), Payload, Capture))
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
        const bool bHasRows = Capture.Result->TryGetArrayField(TEXT("tags"), Rows) && Rows != nullptr;
        // No `!Rows ||` short-circuit: a missing tags array must fail here, not pass for free.
        TestTrue(TEXT("clamped list returns tags array"), bHasRows);
        TestEqual(TEXT("clamped list returns exactly the one seeded row"), bHasRows ? Rows->Num() : -1, 1);

        TSharedPtr<FJsonObject> ClampedRow;
        TestTrue(TEXT("clamped list contains seeded tag"), bHasRows && FindTagRow(*Rows, Tag, ClampedRow));

        double TotalMatches = -1.0;
        TestTrue(TEXT("clamped list reports totalMatches"),
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches));
        TestEqual(TEXT("clamped list totalMatches counts the one seeded row"),
            static_cast<int32>(TotalMatches), 1);

        bool bTotalMatchesExact = false;
        TestTrue(TEXT("clamped list reports totalMatchesExact"),
            Capture.Result->TryGetBoolField(TEXT("totalMatchesExact"), bTotalMatchesExact));
        TestTrue(TEXT("clamped list total is exact (scan was not cut short by the limit)"), bTotalMatchesExact);

        bool bTruncated = true;
        TestTrue(TEXT("clamped list reports truncated"),
            Capture.Result->TryGetBoolField(TEXT("truncated"), bTruncated));
        TestFalse(TEXT("clamped list is not truncated"), bTruncated);
    }

    SetTagInSource(Source, Tag, FString(), false);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameplayTagsFindReferencersSavedFixtureTest,
    "PinWright.gameplay_tags.FindReferencersListsSavedFixture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameplayTagsFindReferencersSavedFixtureTest::RunTest(const FString& Parameters)
{
    const FString Source = TEXT("McpAutomationTagRefFindTags.ini");
    const FString ReferencedTag = TEXT("Mcp.Automation.TagRefFind.Referenced");
    const FString FreeTag = TEXT("Mcp.Automation.TagRefFind.Free");
    const FString UnregisteredTag = TEXT("Mcp.Automation.TagRefFind.NeverRegistered");
    FString FixturePath;

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(FixturePath);
        SetTagInSource(Source, ReferencedTag, FString(), false);
        SetTagInSource(Source, FreeTag, FString(), false);
        CleanupTagSource(Source);
    };

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> AddSourcePayload = MakeShared<FJsonObject>();
    AddSourcePayload->SetStringField(TEXT("source"), Source);
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.add_source"), AddSourcePayload, Capture)
        || !TestTrue(TEXT("referenced tag registered"), SetTagInSource(Source, ReferencedTag, FString(), true))
        || !TestTrue(TEXT("free tag registered"), SetTagInSource(Source, FreeTag, FString(), true))
        || !SaveTagReferencingBlueprint(*this, ReferencedTag, FixturePath))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> TagValues;
    TagValues.Add(MakeShared<FJsonValueString>(ReferencedTag));
    TagValues.Add(MakeShared<FJsonValueString>(FreeTag));
    TagValues.Add(MakeShared<FJsonValueString>(UnregisteredTag));
    Payload->SetArrayField(TEXT("tags"), TagValues);
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.find_referencers"), Payload, Capture))
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
    if (!TestTrue(TEXT("one row per requested tag"),
        Capture.Result->TryGetArrayField(TEXT("tags"), Rows) && Rows && Rows->Num() == 3))
    {
        return false;
    }

    const TSharedPtr<FJsonObject> Referenced = (*Rows)[0]->AsObject();
    const TSharedPtr<FJsonObject> Free = (*Rows)[1]->AsObject();
    const TSharedPtr<FJsonObject> Unregistered = (*Rows)[2]->AsObject();
    TestEqual(TEXT("rows keep request order"), Referenced->GetStringField(TEXT("tag")), ReferencedTag);
    TestTrue(TEXT("referenced tag lists the saved fixture package"), ReferencersContainPackage(Referenced, FixturePath));
    TestTrue(TEXT("referenced tag count is positive"), Referenced->GetNumberField(TEXT("referencerCount")) >= 1);
    TestTrue(TEXT("referenced tag is registered"), Referenced->GetBoolField(TEXT("registered")));

    // Paired control from the same source: a registered tag nothing stores reads zero.
    TestEqual(TEXT("free tag has no referencers"), static_cast<int32>(Free->GetNumberField(TEXT("referencerCount"))), 0);
    TestTrue(TEXT("free tag is registered"), Free->GetBoolField(TEXT("registered")));
    TestFalse(TEXT("unregistered tag is reported unregistered, not dropped"), Unregistered->GetBoolField(TEXT("registered")));

    // An empty match set is an error, not a zero-row success.
    TSharedPtr<FJsonObject> EmptyPayload = MakeShared<FJsonObject>();
    EmptyPayload->SetArrayField(TEXT("tags"), TArray<TSharedPtr<FJsonValue>>());
    Capture.Reset();
    InvokeHandlerWithCapture(TEXT("gameplay_tags.find_referencers"), EmptyPayload, Capture);
    TestFalse(TEXT("empty tags is refused"), Capture.bSuccess);
    TestEqual(TEXT("empty tags error code"), Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameplayTagsRemoveReferencedTagTest,
    "PinWright.gameplay_tags.RemoveReferencedTagReportsReferencers",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGameplayTagsRemoveReferencedTagTest::RunTest(const FString& Parameters)
{
    const FString Source = TEXT("McpAutomationTagRefRemoveTags.ini");
    const FString ImplicitParent = TEXT("Mcp.Automation.TagRefRemove");
    const FString ReferencedTag = ImplicitParent + TEXT(".Referenced");
    const FString FreeTag = ImplicitParent + TEXT(".Free");
    FString FixturePath;

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(FixturePath);
        SetTagInSource(Source, ReferencedTag, FString(), false);
        SetTagInSource(Source, FreeTag, FString(), false);
        CleanupTagSource(Source);
    };

    FTestResponseCapture Capture;
    TSharedPtr<FJsonObject> AddSourcePayload = MakeShared<FJsonObject>();
    AddSourcePayload->SetStringField(TEXT("source"), Source);
    if (!InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.add_source"), AddSourcePayload, Capture)
        || !TestTrue(TEXT("referenced tag registered"), SetTagInSource(Source, ReferencedTag, FString(), true))
        || !TestTrue(TEXT("free tag registered"), SetTagInSource(Source, FreeTag, FString(), true))
        || !SaveTagReferencingBlueprint(*this, ReferencedTag, FixturePath))
    {
        return false;
    }

    auto MakeRemovePayload = [&Source](const FString& Tag)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("tag"), Tag);
        Payload->SetStringField(TEXT("source"), Source);
        return Payload;
    };

    // The engine refuses this delete; the verb must say so with the referencers, not succeed.
    Capture.Reset();
    InvokeHandlerWithCapture(TEXT("gameplay_tags.remove"), MakeRemovePayload(ReferencedTag), Capture);
    TestFalse(TEXT("referenced remove is not a success"), Capture.bSuccess);
    TestEqual(TEXT("referenced remove error code"), Capture.ErrorCode, FString(TEXT("TAG_IN_USE")));
    if (TestTrue(TEXT("referenced remove carries error data"), Capture.Result.IsValid()))
    {
        TestEqual(TEXT("reason is referenced"), Capture.Result->GetStringField(TEXT("reason")), FString(TEXT("referenced")));
        TestEqual(TEXT("blockingTag names the tag"), Capture.Result->GetStringField(TEXT("blockingTag")), ReferencedTag);
        TestFalse(TEXT("removed is false"), Capture.Result->GetBoolField(TEXT("removed")));
        TestTrue(TEXT("referencers list the saved fixture"), ReferencersContainPackage(Capture.Result, FixturePath));
    }

    FString Comment;
    TArray<FName> Sources;
    bool bIsExplicit = false;
    bool bIsRestricted = false;
    bool bAllowNonRestrictedChildren = true;
    UGameplayTagsManager::Get().GetTagEditorData(FName(*ReferencedTag), Comment, Sources, bIsExplicit, bIsRestricted, bAllowNonRestrictedChildren);
    TestTrue(TEXT("refused tag is still explicitly registered"), bIsExplicit);

    // Control: same call shape on an unreferenced sibling goes through.
    if (InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.remove"), MakeRemovePayload(FreeTag), Capture))
    {
        TestTrue(TEXT("unreferenced sibling is removed"), Capture.Result->GetBoolField(TEXT("removed")));
    }

    // Non-removals that are not refusals carry a reason instead of a bare removed:false.
    if (InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.remove"), MakeRemovePayload(ImplicitParent), Capture))
    {
        TestFalse(TEXT("implicit parent not removed"), Capture.Result->GetBoolField(TEXT("removed")));
        TestEqual(TEXT("implicit parent reason"), Capture.Result->GetStringField(TEXT("reason")), FString(TEXT("implicit")));
    }
    if (InvokeGameplayTagsHandler(*this, TEXT("gameplay_tags.remove"), MakeRemovePayload(ImplicitParent + TEXT(".NeverRegistered")), Capture))
    {
        TestFalse(TEXT("unregistered tag not removed"), Capture.Result->GetBoolField(TEXT("removed")));
        TestEqual(TEXT("unregistered reason"), Capture.Result->GetStringField(TEXT("reason")), FString(TEXT("not_registered")));
    }
    return true;
}
