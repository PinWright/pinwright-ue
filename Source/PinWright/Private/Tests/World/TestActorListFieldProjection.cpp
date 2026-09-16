// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board ticket B-actor-list-fields-unknown-key-silently-dropped.
//
// actor.list's `fields` allow-list read the caller's set verbatim and then probed it for
// exactly four keys (label/name/path/class). Any other entry was accepted, matched nothing,
// and was never mentioned again — while still raising Fields.Num(), which is what switches
// projection ON. Two grades of damage followed from that one line:
//
//   1. fields:["name","folder"] returned rows carrying only `name`. Two columns asked for,
//      one delivered, nothing in the response saying so.
//   2. fields:["folder"] returned rows that were EMPTY JSON OBJECTS, alongside count and
//      totalMatches reporting the matched actors — a success-shaped answer indistinguishable
//      from "this actor has no such data".
//
// The fix does both halves: `folder` became an emittable key (opt-in, so it does not widen
// the default row of a verb that already spills), and any key the row builder cannot emit is
// now refused by name with INVALID_PARAMS naming the valid set — the courtesy the dispatcher's
// UNKNOWN_PARAMS gate already extends to top-level parameter names but cannot extend to values
// inside an array.
//
// Counterfactual: before the fix, every UnknownFieldIsRejected case below returned bSuccess,
// and the FolderIsProjectable row was `{}`.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Tests/TestSkipReporting.h"

namespace ActorListFieldProjectionTestUtils
{
    // Every probe below narrows to actors spawned by this test, so the assertions do not
    // depend on whatever else the open map contains.
    const TCHAR* const ProbeLabel = TEXT("ActorListFieldProjectionProbe");

    // A synthetic Outliner folder — no host-project vocabulary, created implicitly by
    // SetFolderPath, and discarded with the actor by FScopedEditorWorldActorGuard.
    const TCHAR* const ProbeFolder = TEXT("PinWrightProbe/Nested");

    TSharedPtr<FJsonObject> MakeFieldsPayload(const TArray<FString>& Fields)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        // Only the first row is ever inspected, and the unfiltered over-rejection probe
        // would otherwise build a full row for every actor in the open map.
        Payload->SetNumberField(TEXT("limit"), 1);
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& Field : Fields)
        {
            Arr.Add(MakeShared<FJsonValueString>(Field));
        }
        Payload->SetArrayField(TEXT("fields"), Arr);
        return Payload;
    }

    // The first row of a list response, or nullptr when the response carried no rows.
    const TSharedPtr<FJsonObject>* FirstRow(FTestResponseCapture& Capture)
    {
        const TArray<TSharedPtr<FJsonValue>>* ActorsArr = nullptr;
        if (!Capture.Result.IsValid()
            || !Capture.Result->TryGetArrayField(TEXT("actors"), ActorsArr)
            || !ActorsArr || ActorsArr->Num() == 0)
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* Row = nullptr;
        if ((*ActorsArr)[0].IsValid() && (*ActorsArr)[0]->TryGetObject(Row) && Row)
        {
            return Row;
        }
        return nullptr;
    }

    AActor* FindActorByLabel(UWorld* World, const FString& Label)
    {
        if (!World)
        {
            return nullptr;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (AActor* Actor = *It)
            {
                if (Actor->GetActorLabel() == Label)
                {
                    return Actor;
                }
            }
        }
        return nullptr;
    }
}

// ============================================================================
// A key the row builder cannot emit is refused by name, never dropped
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorListFieldProjectionUnknownFieldRejectedTest,
    "PinWright.actor.list.FieldProjection.UnknownFieldIsRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorListFieldProjectionUnknownFieldRejectedTest::RunTest(const FString& Parameters)
{
    using namespace ActorListFieldProjectionTestUtils;

    // No world fixture: the allow-list is validated before the actor walk, so this half of
    // the contract holds on any host.
    auto ListWithFields = [&](const TArray<FString>& Fields) -> FTestResponseCapture
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("actor.list handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.list"), MakeFieldsPayload(Fields), Capture));
        return Capture;
    };

    // Grade 2 from the ticket, before folder was emittable: a projection made ENTIRELY of
    // keys the handler cannot emit. This used to return success with `{}` rows.
    {
        FTestResponseCapture Capture = ListWithFields({TEXT("location")});
        TestTrue(TEXT("an all-unknown projection is answered"), Capture.bWasCalled);
        TestFalse(TEXT("an all-unknown projection is NOT a success"), Capture.bSuccess);
        TestEqual(TEXT("an unknown fields entry is rejected as INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        TestTrue(TEXT("the message names the offending key"),
            Capture.Message.Contains(TEXT("location")));
        TestTrue(TEXT("the message names the valid set"),
            Capture.Message.Contains(TEXT("Valid fields")));
        for (const TCHAR* Valid : {TEXT("label"), TEXT("name"), TEXT("path"), TEXT("class"),
                                   TEXT("folder")})
        {
            TestTrue(FString::Printf(TEXT("the valid set lists '%s'"), Valid),
                Capture.Message.Contains(Valid));
        }
    }

    // Grade 1: a MIXED projection must fail too. Honouring the recognised half and dropping
    // the rest is what silently answered a two-column question with one column.
    {
        FTestResponseCapture Capture = ListWithFields({TEXT("name"), TEXT("location")});
        TestFalse(TEXT("a partially-unknown projection is NOT a success"), Capture.bSuccess);
        TestEqual(TEXT("a partially-unknown projection is rejected as INVALID_PARAMS"),
            Capture.ErrorCode, FString(TEXT("INVALID_PARAMS")));
        TestTrue(TEXT("the message names only the offending key"),
            Capture.Message.Contains(TEXT("[location]")));
    }

    // A typo is the same defect wearing different clothes: 'clas' matched nothing and was
    // eaten, while the correctly-spelled 'Label' folds to a valid key by case.
    {
        FTestResponseCapture Capture = ListWithFields({TEXT("Label"), TEXT("clas")});
        TestFalse(TEXT("a mistyped fields entry is NOT a success"), Capture.bSuccess);
        TestTrue(TEXT("the message names the typo and nothing else"),
            Capture.Message.Contains(TEXT("[clas]")));
    }

    // The gate must not over-reject: every emittable key, including the mixed case the
    // allow-list folds, still succeeds.
    {
        FTestResponseCapture Capture = ListWithFields(
            {TEXT("label"), TEXT("Name"), TEXT("path"), TEXT("class"), TEXT("folder")});
        TestTrue(TEXT("a projection of only emittable keys still succeeds"), Capture.bSuccess);
    }

    return true;
}

// ============================================================================
// `folder` is projectable, and only when asked for
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FActorListFieldProjectionFolderProjectableTest,
    "PinWright.actor.list.FieldProjection.FolderIsProjectable",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FActorListFieldProjectionFolderProjectableTest::RunTest(const FString& Parameters)
{
    using namespace ActorListFieldProjectionTestUtils;

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("GEditor->GetEditorWorldContext().World() is null; actor.list folder "
                 "projection needs a placed actor to read GetFolderPath from."));
        return true;
    }

    // A normal (non-transient) level actor: UEditorActorSubsystem::GetAllLevelActors filters
    // RF_Transient out, so a transient probe would be invisible to the verbs below.
    FScopedEditorWorldActorGuard WorldGuard;
    {
        FTestResponseCapture SpawnCapture;
        TSharedPtr<FJsonObject> SpawnPayload = MakeShared<FJsonObject>();
        SpawnPayload->SetStringField(TEXT("classPath"), TEXT("/Script/Engine.PointLight"));
        SpawnPayload->SetStringField(TEXT("actorName"), ProbeLabel);
        TestTrue(TEXT("actor.spawn handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.spawn"), SpawnPayload, SpawnCapture));
        if (!TestTrue(TEXT("the probe actor spawned"), SpawnCapture.bSuccess))
        {
            return true;
        }
    }

    {
        FTestResponseCapture FolderCapture;
        TSharedPtr<FJsonObject> FolderPayload = MakeShared<FJsonObject>();
        FolderPayload->SetStringField(TEXT("actorName"), ProbeLabel);
        FolderPayload->SetStringField(TEXT("folderPath"), ProbeFolder);
        TestTrue(TEXT("actor.set_folder handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.set_folder"), FolderPayload, FolderCapture));
        TestTrue(TEXT("the probe actor was assigned a folder"), FolderCapture.bSuccess);
    }

    // Ground truth read straight off the actor, so the assertions below compare actor.list's
    // answer against the engine's rather than against a string this test hoped for.
    AActor* Probe = FindActorByLabel(World, ProbeLabel);
    if (!TestNotNull(TEXT("the probe actor is in the editor world"), Probe))
    {
        return true;
    }
    const FString ExpectedFolder = Probe->GetFolderPath().ToString();

    auto ListProbe = [&](const TArray<FString>* Fields) -> FTestResponseCapture
    {
        FTestResponseCapture Capture;
        TSharedPtr<FJsonObject> Payload = Fields
            ? MakeFieldsPayload(*Fields)
            : MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("world"), TEXT("editor"));
        Payload->SetStringField(TEXT("filter"), ProbeLabel);
        TestTrue(TEXT("actor.list handler is registered"),
            InvokeHandlerWithCapture(TEXT("actor.list"), Payload, Capture));
        TestTrue(TEXT("actor.list succeeded"), Capture.bSuccess);
        return Capture;
    };

    // The ticket's minimal repro: fields:["folder"] alone. The row used to be `{}`.
    {
        const TArray<FString> Fields = {TEXT("folder")};
        FTestResponseCapture Capture = ListProbe(&Fields);
        if (const TSharedPtr<FJsonObject>* Row = FirstRow(Capture))
        {
            TestTrue(TEXT("fields=[folder] returns a row that is not an empty object"),
                (*Row)->Values.Num() > 0);
            FString Folder;
            TestTrue(TEXT("fields=[folder] row carries folder"),
                (*Row)->TryGetStringField(TEXT("folder"), Folder));
            TestEqual(TEXT("the projected folder matches AActor::GetFolderPath"),
                Folder, ExpectedFolder);
        }
        else
        {
            AddError(TEXT("fields=[folder] returned no rows for the probe actor"));
        }
    }

    // The pairing the ticket was blocked on: one identity column plus the folder, without
    // falling back to actor.describe's full property and component tree.
    {
        const TArray<FString> Fields = {TEXT("name"), TEXT("folder")};
        FTestResponseCapture Capture = ListProbe(&Fields);
        if (const TSharedPtr<FJsonObject>* Row = FirstRow(Capture))
        {
            TestTrue(TEXT("fields=[name,folder] row keeps name"),
                (*Row)->HasField(TEXT("name")));
            TestTrue(TEXT("fields=[name,folder] row keeps folder"),
                (*Row)->HasField(TEXT("folder")));
            TestFalse(TEXT("fields=[name,folder] row drops the verbose path"),
                (*Row)->HasField(TEXT("path")));
        }
        else
        {
            AddError(TEXT("fields=[name,folder] returned no rows for the probe actor"));
        }
    }

    // folder is opt-in: an unprojected call keeps its prior four-column shape, so the fix
    // does not widen every row of a verb that already spills on a populated level.
    {
        FTestResponseCapture Capture = ListProbe(nullptr);
        if (const TSharedPtr<FJsonObject>* Row = FirstRow(Capture))
        {
            TestTrue(TEXT("the unprojected row still carries label"),
                (*Row)->HasField(TEXT("label")));
            TestTrue(TEXT("the unprojected row still carries path"),
                (*Row)->HasField(TEXT("path")));
            TestFalse(TEXT("the unprojected row does NOT carry folder"),
                (*Row)->HasField(TEXT("folder")));
        }
        else
        {
            AddError(TEXT("the unprojected call returned no rows for the probe actor"));
        }
    }

    return true;
}
