// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/TestUtils.h"

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
