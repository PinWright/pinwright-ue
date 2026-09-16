// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Utils/HttpResponseSpill.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    FString MakeSpillTestRoot()
    {
        FString Root = FPaths::ProjectIntermediateDir() /
            TEXT("PinWrightTests/HttpResponseSpill") /
            FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Root = FPaths::ConvertRelativePathToFull(Root);
        FPaths::NormalizeDirectoryName(Root);
        return Root;
    }

    FString SerializeJsonObject(const TSharedRef<FJsonObject>& Object)
    {
        FString Body;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
        FJsonSerializer::Serialize(Object, Writer);
        Writer->Close();
        return Body;
    }

    TSharedRef<FJsonObject> MakeResponseWithText(int32 Id, const FString& Text)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("text"), Text);

        TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
        Response->SetObjectField(TEXT("result"), Result);
        Response->SetNumberField(TEXT("id"), Id);
        return Response;
    }

    // The policy the WIRE uses (Transport/SocketHttpServer.cpp:72), so a test can state the size a
    // reader actually pays rather than the size a pretty-printer would have produced.
    FString SerializeCondensedForTest(const TSharedRef<FJsonObject>& Object)
    {
        FString Body;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
        FJsonSerializer::Serialize(Object, Writer);
        Writer->Close();
        return Body;
    }

    // The MCP tools/call success shape, built the way Transport/McpRequestCore.cpp's
    // MakeToolCallSuccess builds it -- payload in `structuredContent` AND, serialized, in
    // `content[0].text`. Duplicated here deliberately: the tests below are ABOUT that duplication,
    // so a helper that hid it would hide the thing under test.
    TSharedRef<FJsonObject> MakeToolResultForTest(const TSharedRef<FJsonObject>& Payload)
    {
        TSharedRef<FJsonObject> TextBlock = MakeShared<FJsonObject>();
        TextBlock->SetStringField(TEXT("type"), TEXT("text"));
        TextBlock->SetStringField(TEXT("text"), SerializeCondensedForTest(Payload));

        TArray<TSharedPtr<FJsonValue>> Content;
        Content.Add(MakeShared<FJsonValueObject>(TextBlock));

        TSharedRef<FJsonObject> ToolResult = MakeShared<FJsonObject>();
        ToolResult->SetArrayField(TEXT("content"), Content);
        ToolResult->SetObjectField(TEXT("structuredContent"), Payload);
        ToolResult->SetBoolField(TEXT("isError"), false);
        return ToolResult;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpResponseSpillBypassFlagTest,
    "PinWright.infra.http_response_spill.BypassFlag",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHttpResponseSpillBypassFlagTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetBoolField(HttpResponseSpill::GetInternalSkipParamName(), true);

    TestTrue(TEXT("Bypass flag is detected"),
        HttpResponseSpill::ShouldSkipHttpResponseSpill(Params));

    Params->SetBoolField(HttpResponseSpill::GetInternalSkipParamName(), false);
    TestFalse(TEXT("False bypass flag is ignored"),
        HttpResponseSpill::ShouldSkipHttpResponseSpill(Params));

    TestFalse(TEXT("Missing params do not bypass"),
        HttpResponseSpill::ShouldSkipHttpResponseSpill(nullptr));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpResponseSpillWritesReferenceTest,
    "PinWright.infra.http_response_spill.WritesReference",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHttpResponseSpillWritesReferenceTest::RunTest(const FString& Parameters)
{
    const FString Root = MakeSpillTestRoot();
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(Root);

    const TSharedRef<FJsonObject> Response =
        MakeResponseWithText(42, FString::ChrN(2048, TEXT('x')));
    const FString OriginalBody = SerializeJsonObject(Response);

    const TSharedPtr<FJsonObject> Spilled =
        HttpResponseSpill::MaybeBuildSpilledResponse(
            Response,
            /*ThresholdCharacters=*/1024,
            /*bSkipSpill=*/false);

    TestTrue(TEXT("Spill response is valid"), Spilled.IsValid());
    if (!Spilled.IsValid())
    {
        HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
        return true;
    }

    const TSharedPtr<FJsonObject>* Result = nullptr;
    TestTrue(TEXT("Spill response has result object"),
        Spilled->TryGetObjectField(TEXT("result"), Result) && Result && (*Result).IsValid());

    int32 Id = 0;
    TestTrue(TEXT("Spill response preserves id"), Spilled->TryGetNumberField(TEXT("id"), Id));
    TestEqual(TEXT("Preserved id"), Id, 42);

    const TSharedPtr<FJsonObject>* File = nullptr;
    if (Result && (*Result).IsValid())
    {
        bool bOutputTooLong = false;
        TestTrue(TEXT("outputTooLong true"),
            (*Result)->TryGetBoolField(TEXT("outputTooLong"), bOutputTooLong) && bOutputTooLong);
        TestTrue(TEXT("Spill response has file object"),
            (*Result)->TryGetObjectField(TEXT("file"), File) && File && (*File).IsValid());
    }

    FString Path;
    if (File && (*File).IsValid())
    {
        TestTrue(TEXT("Spill file has path"), (*File)->TryGetStringField(TEXT("path"), Path));
        TestTrue(TEXT("Spill path stays under override root"), Path.StartsWith(Root + TEXT("/")));

        int32 Characters = 0;
        TestTrue(TEXT("Spill file reports character count"),
            (*File)->TryGetNumberField(TEXT("characters"), Characters));
        TestEqual(TEXT("Character count matches serialized body"), Characters, OriginalBody.Len());
    }

    FString StoredBody;
    TestTrue(TEXT("Spill file loads"),
        !Path.IsEmpty() && FFileHelper::LoadFileToString(StoredBody, *Path));
    TestEqual(TEXT("Spill file stores original response"), StoredBody, OriginalBody);

    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
    IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpResponseSpillPrunesOnlyOldDatetimeDirsTest,
    "PinWright.infra.http_response_spill.PrunesOnlyOldDatetimeDirs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHttpResponseSpillPrunesOnlyOldDatetimeDirsTest::RunTest(const FString& Parameters)
{
    const FString Root = MakeSpillTestRoot();
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(Root);

    IFileManager& FileManager = IFileManager::Get();
    const FString OldDir = Root / TEXT("20000101T000000Z");
    const FString InvalidDir = Root / TEXT("not-a-spill-dir");
    FileManager.MakeDirectory(*OldDir, /*Tree=*/true);
    FileManager.MakeDirectory(*InvalidDir, /*Tree=*/true);

    HttpResponseSpill::PruneOldHttpResponseSpills();

    TestFalse(TEXT("Old datetime directory is pruned"),
        FileManager.DirectoryExists(*OldDir));
    TestTrue(TEXT("Non-datetime directory is not pruned"),
        FileManager.DirectoryExists(*InvalidDir));

    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
    FileManager.DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    return true;
}

// =================================================================================================
// MarkOversizedToolResult measures what a READER pays, once, condensed
// =================================================================================================
//
// THE DEFECT. The gate compared the threshold against SerializeJsonObject(ToolResult), and that
// string is not what anyone reads:
//
//   1. it is PRETTY-PRINTED. TJsonWriterFactory<> defaults to TPrettyJsonPrintPolicy -- tab indent,
//      CRLF line ends, a space after every colon -- while the transport has always written the wire
//      body condensed (Transport/SocketHttpServer.cpp:72). Nothing downstream ever wanted the
//      pretty form; it was a default nobody chose.
//   2. it counts the payload TWICE. MakeToolCallSuccess puts the same object into `structuredContent`
//      and, serialized and JSON-escaped, into `content[0].text`, because MCP asks a tool returning
//      structured output to also return an equivalent text block for clients that do not read it
//      (2025-06-18). A reader sees whichever one their client renders -- never both.
//   3. the two compound. Escaping the pretty copy into a JSON string turns each tab into `\t` and
//      each CRLF into `\r\n`: two characters per character of whitespace nobody asked for.
//
// MEASURED on 147 real spilled responses from a host project: a capture whose payload was 4,618
// characters reached this gate as 14,228 -- 3.1x -- and 92 of 92 single-still captures spilled to
// disk, each costing the caller an extra file read on a verb the mandatory vision-verification loop
// runs dozens of times per asset. None of those was a real overflow.
//
// WHAT THESE TESTS PIN. That the gate measures one condensed copy of the reader-facing payload; that
// a result which fits is left completely untouched; that the two published numbers mean two
// different things and both are present; and -- the direction that must not be lost -- that a
// genuinely oversized payload still spills.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpResponseSpillToolResultMeasuresOneCopyTest,
    "PinWright.infra.http_response_spill.ToolResultGateMeasuresOneCondensedCopy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHttpResponseSpillToolResultMeasuresOneCopyTest::RunTest(const FString& Parameters)
{
    const FString Root = MakeSpillTestRoot();
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    // A payload that is comfortably UNDER the threshold on its own and comfortably OVER it once
    // pretty-printed and counted twice -- i.e. squarely in the band the old gate got wrong. The
    // nesting matters: pretty-printing costs per LINE, so a flat blob would not reproduce the
    // inflation that a real capture response (dozens of nested measurement objects) suffers.
    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    for (int32 GroupIndex = 0; GroupIndex < 24; ++GroupIndex)
    {
        TSharedRef<FJsonObject> Group = MakeShared<FJsonObject>();
        for (int32 FieldIndex = 0; FieldIndex < 12; ++FieldIndex)
        {
            Group->SetNumberField(FString::Printf(TEXT("measurement%02d"), FieldIndex),
                static_cast<double>(GroupIndex * 100 + FieldIndex) / 7.0);
        }
        Payload->SetObjectField(FString::Printf(TEXT("shot%02d"), GroupIndex), Group);
    }

    const FString CondensedPayload = SerializeCondensedForTest(Payload);
    const FString PrettyPayload = SerializeJsonObject(Payload);
    // The premise, asserted rather than assumed: if pretty-printing did NOT inflate this fixture,
    // every assertion below would pass for the wrong reason.
    if (!TestTrue(
        FString::Printf(TEXT("Precondition: pretty-printing inflates the fixture (%d -> %d chars)"),
            CondensedPayload.Len(), PrettyPayload.Len()),
        PrettyPayload.Len() > CondensedPayload.Len() + 200))
    {
        return true;
    }

    // The threshold sits above one condensed copy and below the old measurement, so the two
    // behaviours are distinguishable by this test and not merely by inspection.
    const int32 Threshold = CondensedPayload.Len() + 64;

    const TSharedRef<FJsonObject> ToolResult = MakeToolResultForTest(Payload);
    const FString OldMeasurement = SerializeJsonObject(ToolResult);
    if (!TestTrue(
        FString::Printf(TEXT("Precondition: the old measurement exceeds the threshold (%d > %d)"),
            OldMeasurement.Len(), Threshold),
        OldMeasurement.Len() > Threshold))
    {
        return true;
    }

    HttpResponseSpill::MarkOversizedToolResult(ToolResult, Threshold);

    // UNABLE TO FAIL IF the payload were tiny -- which is why the two preconditions above are
    // hard-stops rather than warnings. Against the old gate this fails: the result would have been
    // rewritten into the {outputTooLong, file} shape and `shot00` would be gone.
    TestTrue(TEXT("a result that fits ONE condensed copy is left inline"),
        ToolResult->HasField(TEXT("structuredContent")));
    const TSharedPtr<FJsonObject>* Structured = nullptr;
    if (TestTrue(TEXT("structuredContent survives"),
            ToolResult->TryGetObjectField(TEXT("structuredContent"), Structured) &&
            Structured != nullptr && Structured->IsValid()))
    {
        TestTrue(TEXT("the payload is untouched, not merely present"),
            (*Structured)->HasField(TEXT("shot00")) && (*Structured)->HasField(TEXT("shot23")));
        TestFalse(TEXT("no outputTooLong marker on an inline result"),
            (*Structured)->HasField(TEXT("outputTooLong")));
    }

    return true;
}

// The other direction, and the one that must never be lost to an over-clever measurement: a payload
// that really is too big still spills, and the reference it leaves behind says which number was
// compared against the threshold and which number is on disk.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpResponseSpillToolResultStillSpillsTest,
    "PinWright.infra.http_response_spill.ToolResultOverflowStillSpillsAndReportsBothSizes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHttpResponseSpillToolResultStillSpillsTest::RunTest(const FString& Parameters)
{
    const FString Root = MakeSpillTestRoot();
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("blob"), FString::ChrN(8192, TEXT('x')));
    const TSharedRef<FJsonObject> ToolResult = MakeToolResultForTest(Payload);

    HttpResponseSpill::MarkOversizedToolResult(ToolResult, /*ThresholdCharacters=*/2048);

    const TSharedPtr<FJsonObject>* Structured = nullptr;
    if (!TestTrue(TEXT("an oversized result is rewritten"),
            ToolResult->TryGetObjectField(TEXT("structuredContent"), Structured) &&
            Structured != nullptr && Structured->IsValid()))
    {
        return true;
    }
    bool bTooLong = false;
    TestTrue(TEXT("outputTooLong is set"),
        (*Structured)->TryGetBoolField(TEXT("outputTooLong"), bTooLong) && bTooLong);

    const TSharedPtr<FJsonObject>* File = nullptr;
    if (!TestTrue(TEXT("the spill reference carries a file object"),
            (*Structured)->TryGetObjectField(TEXT("file"), File) &&
            File != nullptr && (*File).IsValid()))
    {
        return true;
    }

    FString Path;
    TestTrue(TEXT("the reference names a path"), (*File)->TryGetStringField(TEXT("path"), Path));
    TestTrue(TEXT("the path stays under the override root"), Path.StartsWith(Root + TEXT("/")));
    TestTrue(TEXT("the spilled file exists on disk"),
        !Path.IsEmpty() && IFileManager::Get().FileExists(*Path));

    // TWO numbers, two questions. Conflating them is what made the old gate wrong, so the test
    // asserts they are BOTH present and that the on-disk number stays within one copy of the
    // measured one -- if it ran to double, the file would have gone back to holding the wrapper.
    int32 MeasuredCharacters = 0;
    int32 FileCharacters = 0;
    TestTrue(TEXT("characters reports what was measured against the threshold"),
        (*File)->TryGetNumberField(TEXT("characters"), MeasuredCharacters));
    TestTrue(TEXT("fileCharacters reports what is actually on disk"),
        (*File)->TryGetNumberField(TEXT("fileCharacters"), FileCharacters));
    // The file holds ONE copy of the payload, pretty-printed, so it lands just above the condensed
    // measurement. The envelope it used to hold carried the payload twice and came in past double.
    TestTrue(FString::Printf(
        TEXT("the on-disk copy is one pretty-printed copy of the payload (%d on disk vs %d ")
        TEXT("measured), not the envelope that carried it twice"), FileCharacters, MeasuredCharacters),
        FileCharacters >= MeasuredCharacters && FileCharacters < MeasuredCharacters + 1024);
    TestTrue(TEXT("the measured count is the payload, not the wrapper"),
        MeasuredCharacters > 8192 && MeasuredCharacters < 8192 + 512);

    // The notice a caller reads must quote the number that was actually compared, or the next
    // person to tune a response budget tunes it against a number no gate uses.
    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    if (TestTrue(TEXT("the content array is rewritten to a notice"),
            ToolResult->TryGetArrayField(TEXT("content"), Content) &&
            Content != nullptr && Content->Num() == 1))
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        FString Text;
        if ((*Content)[0].IsValid() && (*Content)[0]->TryGetObject(Block) &&
            Block != nullptr && (*Block).IsValid())
        {
            (*Block)->TryGetStringField(TEXT("text"), Text);
        }
        TestTrue(TEXT("the notice keeps its recognisable prefix"),
            Text.Contains(TEXT("Response exceeds")));
        TestTrue(TEXT("the notice quotes the MEASURED count"),
            Text.Contains(FString::Printf(TEXT("(%d chars"), MeasuredCharacters)));
    }

    return true;
}

// A tools/call ERROR carries no structuredContent unless the handler attached a payload, so the text
// block is then the only copy there is and must be what the gate counts. Getting this wrong in the
// other direction -- measuring an absent structuredContent as zero -- would make every error
// response unspillable however large it grew.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpResponseSpillToolResultTextOnlyIsMeasuredTest,
    "PinWright.infra.http_response_spill.ToolResultWithoutStructuredContentMeasuresItsText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHttpResponseSpillToolResultTextOnlyIsMeasuredTest::RunTest(const FString& Parameters)
{
    const FString Root = MakeSpillTestRoot();
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    TSharedRef<FJsonObject> TextBlock = MakeShared<FJsonObject>();
    TextBlock->SetStringField(TEXT("type"), TEXT("text"));
    TextBlock->SetStringField(TEXT("text"), FString::ChrN(4096, TEXT('e')));
    TArray<TSharedPtr<FJsonValue>> Content;
    Content.Add(MakeShared<FJsonValueObject>(TextBlock));

    TSharedRef<FJsonObject> ToolResult = MakeShared<FJsonObject>();
    ToolResult->SetArrayField(TEXT("content"), Content);
    ToolResult->SetBoolField(TEXT("isError"), true);

    HttpResponseSpill::MarkOversizedToolResult(ToolResult, /*ThresholdCharacters=*/2048);

    const TSharedPtr<FJsonObject>* Structured = nullptr;
    bool bTooLong = false;
    TestTrue(TEXT("a text-only result over threshold still spills"),
        ToolResult->TryGetObjectField(TEXT("structuredContent"), Structured) &&
        Structured != nullptr && Structured->IsValid() &&
        (*Structured)->TryGetBoolField(TEXT("outputTooLong"), bTooLong) && bTooLong);
    // The counter-case, so "it spills" is not just "it always spills": the same shape under the
    // threshold is left alone.
    {
        TSharedRef<FJsonObject> SmallBlock = MakeShared<FJsonObject>();
        SmallBlock->SetStringField(TEXT("type"), TEXT("text"));
        SmallBlock->SetStringField(TEXT("text"), FString::ChrN(128, TEXT('e')));
        TArray<TSharedPtr<FJsonValue>> SmallContent;
        SmallContent.Add(MakeShared<FJsonValueObject>(SmallBlock));

        TSharedRef<FJsonObject> SmallResult = MakeShared<FJsonObject>();
        SmallResult->SetArrayField(TEXT("content"), SmallContent);
        SmallResult->SetBoolField(TEXT("isError"), true);

        HttpResponseSpill::MarkOversizedToolResult(SmallResult, /*ThresholdCharacters=*/2048);
        TestFalse(TEXT("a small text-only result is left alone"),
            SmallResult->HasField(TEXT("structuredContent")));
    }

    return true;
}

// =================================================================================================
// The spilled FILE is the payload, not the envelope around it
// =================================================================================================
//
// THE DEFECT. The file was SerializeJsonObject(ToolResult) -- the MCP wrapper. Its top level is
// ['content','isError','structuredContent'], which shares NO key with the payload the same call
// returns inline when it happens to fit. So the documented recovery
//
//     r = call(...); if r["outputTooLong"]: r = json.load(open(r["file"]["path"]))
//     value = r.get("dataInterfaceCheck")
//
// yields None for every field, with no error and no exception. "Absent" reads as "not healthy",
// so the answer does not merely go missing, it INVERTS. Measured: a pre-capture sweep of 13
// Niagara systems for `dataInterfaceCheck` reported 10 unhealthy; the 3 that answered were simply
// the 3 whose payload stayed under the threshold. All 13 were consistent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpResponseSpillFileIsThePayloadTest,
    "PinWright.infra.http_response_spill.SpillFileIsThePayloadNotTheEnvelope",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHttpResponseSpillFileIsThePayloadTest::RunTest(const FString& Parameters)
{
    const FString Root = MakeSpillTestRoot();
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    // Shaped like the response that produced the inverted sweep: a verdict field a caller branches
    // on, plus enough bulk to cross the threshold.
    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetBoolField(TEXT("valid"), true);
    Payload->SetStringField(TEXT("dataInterfaceCheck"), TEXT("consistent"));
    Payload->SetStringField(TEXT("details"), FString::ChrN(4096, TEXT('d')));

    const TSharedRef<FJsonObject> ToolResult = MakeToolResultForTest(Payload);
    HttpResponseSpill::MarkOversizedToolResult(ToolResult, /*ThresholdCharacters=*/2048);

    const TSharedPtr<FJsonObject>* Structured = nullptr;
    const TSharedPtr<FJsonObject>* File = nullptr;
    if (!TestTrue(TEXT("the oversized result spilled and left a file reference"),
            ToolResult->TryGetObjectField(TEXT("structuredContent"), Structured) &&
            Structured != nullptr && Structured->IsValid() &&
            (*Structured)->TryGetObjectField(TEXT("file"), File) &&
            File != nullptr && (*File).IsValid()))
    {
        return true;
    }

    FString ContentType;
    (*File)->TryGetStringField(TEXT("contentType"), ContentType);
    TestEqual(TEXT("a structured payload is announced as JSON"), ContentType, TEXT("application/json"));
    FString PayloadField;
    TestTrue(TEXT("the stub names which half of the response the file body is"),
        (*File)->TryGetStringField(TEXT("payload"), PayloadField));
    TestEqual(TEXT("and names it structuredContent"), PayloadField, TEXT("structuredContent"));

    FString Path;
    (*File)->TryGetStringField(TEXT("path"), Path);
    FString StoredBody;
    if (!TestTrue(TEXT("the spill file loads"),
            !Path.IsEmpty() && FFileHelper::LoadFileToString(StoredBody, *Path)))
    {
        return true;
    }

    TSharedPtr<FJsonObject> Parsed;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(StoredBody);
    if (!TestTrue(TEXT("the spill file parses as a JSON object"),
            FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid()))
    {
        return true;
    }

    // THE ASSERTION THIS TEST EXISTS FOR: the caller's own field names, read straight off the file
    // with no unwrapping. Against the old body every one of these is absent.
    bool bValid = false;
    TestTrue(TEXT("the payload's `valid` is readable at the file's top level"),
        Parsed->TryGetBoolField(TEXT("valid"), bValid) && bValid);
    FString Check;
    TestTrue(TEXT("the payload's `dataInterfaceCheck` is readable at the file's top level"),
        Parsed->TryGetStringField(TEXT("dataInterfaceCheck"), Check));
    TestEqual(TEXT("and carries its real value, not an absence a caller reads as unhealthy"),
        Check, TEXT("consistent"));
    TestTrue(TEXT("the bulk field survives too"), Parsed->HasField(TEXT("details")));

    // The other half of "is the payload": none of the envelope's keys are there to be mistaken for
    // it. A file carrying both would still let the wrong reach succeed by accident.
    TestFalse(TEXT("no `structuredContent` wrapper key"), Parsed->HasField(TEXT("structuredContent")));
    TestFalse(TEXT("no `content` wrapper key"), Parsed->HasField(TEXT("content")));
    TestFalse(TEXT("no `isError` wrapper key"), Parsed->HasField(TEXT("isError")));

    return true;
}

// The text-payload half of the same defect, observed on `material.decompile_mgir`: with no
// structuredContent to take, the body must be the text content itself. Written verbatim, not
// JSON-escaped into a string field -- escaping is what made a spilled markdown page fail a plain
// grep (`\"class\":` on disk, `"class":` in the pattern) and inflated it past a reader's limits.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpResponseSpillFileForTextOnlyResultTest,
    "PinWright.infra.http_response_spill.SpillFileForTextOnlyResultIsTheTextVerbatim",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHttpResponseSpillFileForTextOnlyResultTest::RunTest(const FString& Parameters)
{
    const FString Root = MakeSpillTestRoot();
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    const FString PayloadText =
        FString(TEXT("MGIR-BEGIN\n\"class\": \"MaterialExpressionAdd\"\n")) +
        FString::ChrN(4096, TEXT('m'));

    TSharedRef<FJsonObject> TextBlock = MakeShared<FJsonObject>();
    TextBlock->SetStringField(TEXT("type"), TEXT("text"));
    TextBlock->SetStringField(TEXT("text"), PayloadText);
    TArray<TSharedPtr<FJsonValue>> Content;
    Content.Add(MakeShared<FJsonValueObject>(TextBlock));

    TSharedRef<FJsonObject> ToolResult = MakeShared<FJsonObject>();
    ToolResult->SetArrayField(TEXT("content"), Content);
    ToolResult->SetBoolField(TEXT("isError"), false);

    HttpResponseSpill::MarkOversizedToolResult(ToolResult, /*ThresholdCharacters=*/2048);

    const TSharedPtr<FJsonObject>* Structured = nullptr;
    const TSharedPtr<FJsonObject>* File = nullptr;
    if (!TestTrue(TEXT("the oversized text-only result spilled and left a file reference"),
            ToolResult->TryGetObjectField(TEXT("structuredContent"), Structured) &&
            Structured != nullptr && Structured->IsValid() &&
            (*Structured)->TryGetObjectField(TEXT("file"), File) &&
            File != nullptr && (*File).IsValid()))
    {
        return true;
    }

    FString ContentType;
    (*File)->TryGetStringField(TEXT("contentType"), ContentType);
    TestEqual(TEXT("a text payload is announced as text, not JSON"), ContentType, TEXT("text/plain"));
    FString PayloadField;
    (*File)->TryGetStringField(TEXT("payload"), PayloadField);
    TestEqual(TEXT("the stub names the body as the text content"), PayloadField, TEXT("text"));

    FString Path;
    (*File)->TryGetStringField(TEXT("path"), Path);
    FString StoredBody;
    if (!TestTrue(TEXT("the spill file loads"),
            !Path.IsEmpty() && FFileHelper::LoadFileToString(StoredBody, *Path)))
    {
        return true;
    }

    // Byte-for-byte what content[0].text would have carried inline. Against the old body this is
    // the serialized envelope, so the equality fails and the escaped `\"class\"` in it defeats the
    // grep a caller would run next.
    TestEqual(TEXT("the file is the response text verbatim"), StoredBody, PayloadText);

    return true;
}

// An ERROR's two halves are not copies of each other, and that is what makes "prefer
// structuredContent" wrong for it. MakeToolCallError (Transport/McpRequestCore.cpp) puts the bare
// handler payload in structuredContent and the `[<CODE>] <message>` line -- plus the serialized
// payload and the Docs pointer -- only in the text block. The inline text block is then overwritten
// by the spill notice, so a rule that took structuredContent would leave the error code and the
// message written NOWHERE: a >10k ERR_PARSE_FAILED would spill its diagnostics and lose the
// sentence saying what failed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHttpResponseSpillFileForErrorResultTest,
    "PinWright.infra.http_response_spill.SpillFileForErrorResultKeepsCodeAndMessage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHttpResponseSpillFileForErrorResultTest::RunTest(const FString& Parameters)
{
    const FString Root = MakeSpillTestRoot();
    HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(Root);
    ON_SCOPE_EXIT
    {
        HttpResponseSpill::SetHttpResponseSpillRootOverrideForTests(TEXT(""));
        IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
    };

    // The shape MakeToolCallError builds: payload alone in structuredContent, and a text block that
    // opens with the code and message and then repeats the payload.
    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("diagnostics"), FString::ChrN(4096, TEXT('g')));

    const FString ErrorText =
        FString(TEXT("[ERR_PARSE_FAILED] Compile failed at line 3\n")) +
        SerializeCondensedForTest(Payload);

    TSharedRef<FJsonObject> TextBlock = MakeShared<FJsonObject>();
    TextBlock->SetStringField(TEXT("type"), TEXT("text"));
    TextBlock->SetStringField(TEXT("text"), ErrorText);
    TArray<TSharedPtr<FJsonValue>> Content;
    Content.Add(MakeShared<FJsonValueObject>(TextBlock));

    TSharedRef<FJsonObject> ToolResult = MakeShared<FJsonObject>();
    ToolResult->SetArrayField(TEXT("content"), Content);
    ToolResult->SetObjectField(TEXT("structuredContent"), Payload);
    ToolResult->SetBoolField(TEXT("isError"), true);

    HttpResponseSpill::MarkOversizedToolResult(ToolResult, /*ThresholdCharacters=*/2048);

    const TSharedPtr<FJsonObject>* Structured = nullptr;
    const TSharedPtr<FJsonObject>* File = nullptr;
    if (!TestTrue(TEXT("the oversized error spilled and left a file reference"),
            ToolResult->TryGetObjectField(TEXT("structuredContent"), Structured) &&
            Structured != nullptr && Structured->IsValid() &&
            (*Structured)->TryGetObjectField(TEXT("file"), File) &&
            File != nullptr && (*File).IsValid()))
    {
        return true;
    }

    // The inline text block is gone -- overwritten by the notice -- which is precisely why the file
    // has to be the half that carried the code.
    const TArray<TSharedPtr<FJsonValue>>* RewrittenContent = nullptr;
    if (ToolResult->TryGetArrayField(TEXT("content"), RewrittenContent) &&
        RewrittenContent != nullptr && RewrittenContent->Num() == 1)
    {
        const TSharedPtr<FJsonObject>* Block = nullptr;
        FString InlineText;
        if ((*RewrittenContent)[0].IsValid() && (*RewrittenContent)[0]->TryGetObject(Block) &&
            Block != nullptr && (*Block).IsValid())
        {
            (*Block)->TryGetStringField(TEXT("text"), InlineText);
        }
        TestFalse(TEXT("the inline text no longer carries the error code"),
            InlineText.Contains(TEXT("[ERR_PARSE_FAILED]")));
    }

    FString PayloadField;
    (*File)->TryGetStringField(TEXT("payload"), PayloadField);
    TestEqual(TEXT("an error's file body is its text, not its bare payload"),
        PayloadField, TEXT("text"));

    FString Path;
    (*File)->TryGetStringField(TEXT("path"), Path);
    FString StoredBody;
    if (!TestTrue(TEXT("the spill file loads"),
            !Path.IsEmpty() && FFileHelper::LoadFileToString(StoredBody, *Path)))
    {
        return true;
    }

    // THE ASSERTION THIS TEST EXISTS FOR. With structuredContent preferred for errors, the file is
    // {"diagnostics": "..."} and neither of these strings is anywhere in the response.
    TestTrue(TEXT("the spilled file keeps the error code"),
        StoredBody.Contains(TEXT("[ERR_PARSE_FAILED]")));
    TestTrue(TEXT("the spilled file keeps the error message"),
        StoredBody.Contains(TEXT("Compile failed at line 3")));
    TestTrue(TEXT("and still carries the diagnostics payload the text repeats"),
        StoredBody.Contains(TEXT("diagnostics")));

    return true;
}


#endif
