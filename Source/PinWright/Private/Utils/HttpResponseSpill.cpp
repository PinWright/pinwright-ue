// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/HttpResponseSpill.h"

#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogHttpResponseSpill, Log, All);

namespace HttpResponseSpill
{
namespace
{
    const TCHAR* InternalSkipParamName =
        TEXT("__pinWrightInternal_clientHandlesLargeResponses_skipHttpSpill_v1");
    constexpr int32 DefaultThresholdCharacters = 10000;
    constexpr int32 MinimumThresholdCharacters = 1024;
    constexpr int32 RetentionHours = 24;

    FString FormatUtcDateTime(const FDateTime& DateTime)
    {
        return FString::Printf(TEXT("%04d%02d%02dT%02d%02d%02dZ"),
            DateTime.GetYear(),
            DateTime.GetMonth(),
            DateTime.GetDay(),
            DateTime.GetHour(),
            DateTime.GetMinute(),
            DateTime.GetSecond());
    }

    FString& CurrentDateTimeDirName()
    {
        static FString DateTimeDirName;
        return DateTimeDirName;
    }

#if WITH_DEV_AUTOMATION_TESTS
    FString& RootOverrideForTests()
    {
        static FString RootOverride;
        return RootOverride;
    }
#endif

    const FString& EnsureCurrentDateTimeDirName()
    {
        FString& DateTimeDirName = CurrentDateTimeDirName();
        if (DateTimeDirName.IsEmpty())
        {
            DateTimeDirName = FormatUtcDateTime(FDateTime::UtcNow());
        }
        return DateTimeDirName;
    }

    // Pretty, deliberately: this is what gets WRITTEN TO THE SPILL FILE, and a spill file exists
    // to be opened and read. It is no longer what the size gate measures - see
    // MeasureReaderFacingCharacters.
    FString SerializeJsonObject(const TSharedRef<FJsonObject>& Object)
    {
        FString Body;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
        FJsonSerializer::Serialize(Object, Writer);
        Writer->Close();
        return Body;
    }

    FString SerializeCondensed(const TSharedRef<FJsonObject>& Object)
    {
        FString Body;
        const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
        FJsonSerializer::Serialize(Object, Writer);
        Writer->Close();
        return Body;
    }

    void CollectTextBlocks(const TSharedRef<FJsonObject>& ToolResult, TArray<FString>& OutTexts)
    {
        const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
        if (!ToolResult->TryGetArrayField(TEXT("content"), Content) || Content == nullptr)
        {
            return;
        }

        for (const TSharedPtr<FJsonValue>& Entry : *Content)
        {
            const TSharedPtr<FJsonObject>* Block = nullptr;
            if (Entry.IsValid() && Entry->TryGetObject(Block) &&
                Block != nullptr && Block->IsValid())
            {
                FString Text;
                if ((*Block)->TryGetStringField(TEXT("text"), Text))
                {
                    OutTexts.Add(MoveTemp(Text));
                }
            }
        }
    }

    // ---- WHAT THE GATE MEASURES, AND WHY IT IS NOT THE SERIALIZED WRAPPER --------------------
    //
    // The threshold exists to bound how much of an agent's context ONE response consumes. An
    // agent reads the payload once - whichever of the two copies its client renders.
    //
    // The wire carries it twice. MCP asks a tool that returns `structuredContent` to also return
    // the same data serialized into a text block, for clients that do not read structured output
    // (2025-06-18: "For backwards compatibility, a tool that returns structured content SHOULD
    // also return functionally equivalent unstructured content"). That duplicate is a protocol
    // artifact, not a reading cost, and counting it made every response look about twice its size.
    //
    // The old measurement did worse than double-count: it serialized the whole wrapper through
    // TJsonWriterFactory<>, whose default policy is TPrettyJsonPrintPolicy, so it also counted tab
    // indentation and CRLF line ends the transport never emits - SocketHttpServer.cpp:72 has
    // always written the body condensed - and the text block's own whitespace was counted at TWO
    // characters per character, because JSON-escaping turns each tab into `\t` and each CRLF into
    // `\r\n`.
    //
    // MEASURED on 147 real spilled responses in a host project: a capture whose payload was 4,618
    // characters reached this gate as 14,228 - 3.1x - and 92 of 92 single-still captures spilled
    // to disk. Every one of those spills cost the caller an extra file read on a verb the
    // mandatory vision-verification loop runs dozens of times per asset, and not one of them was
    // a real overflow.
    //
    // So: ONE copy, CONDENSED, of the payload the reader actually sees. `structuredContent` when
    // there is one; otherwise the sum of the text blocks, which is then the only copy there is
    // (a tools/call error carries no structuredContent unless the handler attached a payload).
    int32 MeasureReaderFacingCharacters(const TSharedRef<FJsonObject>& ToolResult)
    {
        const TSharedPtr<FJsonObject>* Structured = nullptr;
        if (ToolResult->TryGetObjectField(TEXT("structuredContent"), Structured) &&
            Structured != nullptr && Structured->IsValid())
        {
            return SerializeCondensed(Structured->ToSharedRef()).Len();
        }

        TArray<FString> Texts;
        CollectTextBlocks(ToolResult, Texts);

        int32 TextCharacters = 0;
        for (const FString& Text : Texts)
        {
            TextCharacters += Text.Len();
        }
        return TextCharacters;
    }

    // ---- WHAT THE FILE CONTAINS ---------------------------------------------------------------
    //
    // The spill exists so that following `file.path` is equivalent to having read the response
    // inline. So the file holds the PAYLOAD, at its own top level.
    //
    // It used to hold the serialized MCP wrapper instead, and the wrapper's keys (`content`,
    // `structuredContent`, `isError`) share NONE of the payload's. A caller that loaded the file
    // and read a field off it therefore got "absent" for every field, with no error and no
    // exception - which for a health check reads as "not healthy". A pre-capture sweep of 13
    // Niagara systems reported 10 unhealthy that way; all 13 were fine, and the answer had
    // inverted purely on whether a payload crossed a character count.
    //
    // Returns true when the body is the `structuredContent` object (JSON), false when it is the
    // text content. Text is written verbatim rather than re-escaped into a JSON string: escaping
    // is what made a spilled markdown page ungreppable and doubled its size.
    //
    // AN ERROR IS THE EXCEPTION and takes its text even when it has a structuredContent. The two
    // halves of an error are not copies of each other: MakeToolCallError (Transport/
    // McpRequestCore.cpp) puts ONLY the handler payload in structuredContent, while the
    // `[<CODE>] <message>` line - the code, the message, the report hint, the Docs pointer, and
    // the payload again - exists only in the text block. The caller's inline text block is about
    // to be overwritten with the spill notice, so taking structuredContent here would leave the
    // code and the message written nowhere at all: an over-threshold ERR_PARSE_FAILED would spill
    // its diagnostics and lose the sentence saying what failed.
    bool BuildSpillBody(const TSharedRef<FJsonObject>& ToolResult, FString& OutBody)
    {
        bool bIsError = false;
        const bool bIsErrorResult =
            ToolResult->TryGetBoolField(TEXT("isError"), bIsError) && bIsError;

        const TSharedPtr<FJsonObject>* Structured = nullptr;
        const bool bHasStructured =
            ToolResult->TryGetObjectField(TEXT("structuredContent"), Structured) &&
            Structured != nullptr && Structured->IsValid();

        if (bHasStructured && !bIsErrorResult)
        {
            OutBody = SerializeJsonObject(Structured->ToSharedRef());
            return true;
        }

        TArray<FString> Texts;
        CollectTextBlocks(ToolResult, Texts);
        FString TextBody = FString::Join(Texts, TEXT("\n"));

        // An error with a payload but no text block at all is not a shape this transport builds;
        // if one ever reaches here, the payload beats an empty file.
        if (TextBody.IsEmpty() && bHasStructured)
        {
            OutBody = SerializeJsonObject(Structured->ToSharedRef());
            return true;
        }

        OutBody = MoveTemp(TextBody);
        return false;
    }

    bool IsDigit(TCHAR Ch)
    {
        return Ch >= TEXT('0') && Ch <= TEXT('9');
    }

    bool TryReadInt(const FString& Text, int32 Start, int32 Count, int32& OutValue)
    {
        for (int32 Index = Start; Index < Start + Count; ++Index)
        {
            if (!Text.IsValidIndex(Index) || !IsDigit(Text[Index]))
            {
                return false;
            }
        }

        OutValue = FCString::Atoi(*Text.Mid(Start, Count));
        return true;
    }

    int32 DaysInMonth(int32 Year, int32 Month)
    {
        static const int32 DaysPerMonth[] =
        {
            31, 28, 31, 30, 31, 30,
            31, 31, 30, 31, 30, 31
        };

        if (Month == 2 && FDateTime::IsLeapYear(Year))
        {
            return 29;
        }
        return DaysPerMonth[Month - 1];
    }

    bool TryParseSpillDateTimeDir(const FString& Name, FDateTime& OutDateTime)
    {
        if (Name.Len() != 16 || Name[8] != TEXT('T') || Name[15] != TEXT('Z'))
        {
            return false;
        }

        int32 Year = 0;
        int32 Month = 0;
        int32 Day = 0;
        int32 Hour = 0;
        int32 Minute = 0;
        int32 Second = 0;
        if (!TryReadInt(Name, 0, 4, Year) ||
            !TryReadInt(Name, 4, 2, Month) ||
            !TryReadInt(Name, 6, 2, Day) ||
            !TryReadInt(Name, 9, 2, Hour) ||
            !TryReadInt(Name, 11, 2, Minute) ||
            !TryReadInt(Name, 13, 2, Second))
        {
            return false;
        }

        if (Year < 1 || Month < 1 || Month > 12 ||
            Day < 1 || Day > DaysInMonth(Year, Month) ||
            Hour < 0 || Hour > 23 ||
            Minute < 0 || Minute > 59 ||
            Second < 0 || Second > 59)
        {
            return false;
        }

        OutDateTime = FDateTime(Year, Month, Day, Hour, Minute, Second);
        return true;
    }

}

const TCHAR* GetInternalSkipParamName()
{
    return InternalSkipParamName;
}

int32 GetDefaultThresholdCharacters()
{
    return DefaultThresholdCharacters;
}

int32 ClampThresholdCharacters(int32 ThresholdCharacters)
{
    return FMath::Max(MinimumThresholdCharacters, ThresholdCharacters);
}

bool ShouldSkipHttpResponseSpill(const TSharedPtr<FJsonObject>& Params)
{
    bool bSkip = false;
    return Params.IsValid() &&
        Params->TryGetBoolField(InternalSkipParamName, bSkip) &&
        bSkip;
}

FString GetHttpResponseSpillRoot()
{
#if WITH_DEV_AUTOMATION_TESTS
    const FString& RootOverride = RootOverrideForTests();
    if (!RootOverride.IsEmpty())
    {
        FString Root = FPaths::ConvertRelativePathToFull(RootOverride);
        FPaths::NormalizeDirectoryName(Root);
        return Root;
    }
#endif

    FString Root = FPaths::ProjectSavedDir() / TEXT("PinWright/HttpResponses");
    Root = FPaths::ConvertRelativePathToFull(Root);
    FPaths::NormalizeDirectoryName(Root);
    return Root;
}

#if WITH_DEV_AUTOMATION_TESTS
void SetHttpResponseSpillRootOverrideForTests(const FString& Root)
{
    RootOverrideForTests() = Root;
}
#endif

FString GetHttpResponseSpillDateTimeDir()
{
    FString Dir = GetHttpResponseSpillRoot() / EnsureCurrentDateTimeDirName();
    Dir = FPaths::ConvertRelativePathToFull(Dir);
    FPaths::NormalizeDirectoryName(Dir);
    return Dir;
}

bool WriteSpilledHttpResponse(const FString& Body, FString& OutAbsPath, FString& OutError)
{
    OutAbsPath.Reset();
    OutError.Reset();

    const FString Root = GetHttpResponseSpillRoot();
    const FString Dir = GetHttpResponseSpillDateTimeDir();
    if (!Dir.StartsWith(Root + TEXT("/")))
    {
        OutError = FString::Printf(TEXT("Response spill directory escapes root: %s"), *Dir);
        return false;
    }

    IFileManager& FileManager = IFileManager::Get();
    FileManager.MakeDirectory(*Dir, /*Tree=*/true);

    const FString Stamp = FormatUtcDateTime(FDateTime::UtcNow());
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    FString FinalPath = Dir / FString::Printf(TEXT("%s_%s.json"), *Stamp, *Guid);
    FinalPath = FPaths::ConvertRelativePathToFull(FinalPath);
    FPaths::NormalizeFilename(FinalPath);

    if (!FinalPath.StartsWith(Dir + TEXT("/")))
    {
        OutError = FString::Printf(TEXT("Response spill path escapes directory: %s"), *FinalPath);
        return false;
    }

    const FString TmpPath = FinalPath + TEXT(".tmp");
    const bool bSaved = FFileHelper::SaveStringToFile(
        Body,
        *TmpPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    if (!bSaved)
    {
        FileManager.Delete(*TmpPath, /*RequireExists=*/false);
        OutError = FString::Printf(TEXT("Failed to write response spill tmp file: %s"), *TmpPath);
        return false;
    }

    if (!FileManager.Move(*FinalPath, *TmpPath, /*bReplace=*/true, /*bEvenReadOnly=*/false))
    {
        FileManager.Delete(*TmpPath, /*RequireExists=*/false);
        OutError = FString::Printf(TEXT("Failed to move response spill tmp file to final path: %s"), *FinalPath);
        return false;
    }

    OutAbsPath = FinalPath;
    return true;
}

TSharedPtr<FJsonObject> BuildSpillReferenceResponse(
    int32 Characters,
    int32 ThresholdCharacters,
    const FString& Path)
{
    TSharedPtr<FJsonObject> File = MakeShared<FJsonObject>();
    File->SetStringField(TEXT("path"), Path);
    File->SetStringField(TEXT("contentType"), TEXT("application/json"));
    File->SetNumberField(TEXT("characters"), Characters);
    File->SetNumberField(TEXT("threshold"), ThresholdCharacters);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("outputTooLong"), true);
    Result->SetStringField(TEXT("message"),
        TEXT("Response was written to a file because it exceeded the direct HTTP response threshold."));
    Result->SetObjectField(TEXT("file"), File);

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetObjectField(TEXT("result"), Result);
    return Response;
}

TSharedPtr<FJsonObject> MaybeBuildSpilledResponse(
    const TSharedRef<FJsonObject>& Response,
    int32 ThresholdCharacters,
    bool bSkipSpill)
{
    const int32 ClampedThreshold = ClampThresholdCharacters(ThresholdCharacters);
    if (bSkipSpill)
    {
        return Response;
    }

    const FString Body = SerializeJsonObject(Response);
    if (Body.Len() <= ClampedThreshold)
    {
        return Response;
    }

    FString SpillPath;
    FString SpillError;
    if (!WriteSpilledHttpResponse(Body, SpillPath, SpillError))
    {
        UE_LOG(LogHttpResponseSpill, Warning,
            TEXT("Failed to spill oversized HTTP response (%d chars, threshold %d): %s"),
            Body.Len(),
            ClampedThreshold,
            *SpillError);
        return Response;
    }

    TSharedPtr<FJsonObject> SpilledResponse = BuildSpillReferenceResponse(
        Body.Len(),
        ClampedThreshold,
        SpillPath);

    // Preserve any top-level fields from the original response other than the
    // replaced "result" payload (e.g. correlation fields like "id") so callers
    // can still match the spilled response to their request.
    if (SpilledResponse.IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>> Field : Response->Values)
        {
            if (Field.Key.Equals(TEXT("result"), ESearchCase::IgnoreCase))
            {
                continue;
            }
            SpilledResponse->SetField(Field.Key, Field.Value);
        }
    }

    return SpilledResponse;
}

void MarkOversizedToolResult(
    const TSharedRef<FJsonObject>& ToolResult,
    int32 ThresholdCharacters)
{
    const int32 ClampedThreshold = ClampThresholdCharacters(ThresholdCharacters);

    // The gate is the READER-FACING size, not the serialized wrapper. See
    // MeasureReaderFacingCharacters for the measurement and the numbers behind it.
    const int32 ReaderCharacters = MeasureReaderFacingCharacters(ToolResult);
    if (ReaderCharacters <= ClampedThreshold)
    {
        return;
    }

    // Only now is anything serialized for the FILE, and what goes in it is the payload alone -
    // see BuildSpillBody for why the wrapper must not.
    FString Body;
    const bool bStructuredBody = BuildSpillBody(ToolResult, Body);

    FString SpillPath;
    FString SpillError;
    if (!WriteSpilledHttpResponse(Body, SpillPath, SpillError))
    {
        UE_LOG(LogHttpResponseSpill, Warning,
            TEXT("Failed to spill oversized MCP tool result (%d chars measured, threshold %d): %s"),
            ReaderCharacters,
            ClampedThreshold,
            *SpillError);
        return;
    }

    TSharedPtr<FJsonObject> File = MakeShared<FJsonObject>();
    File->SetStringField(TEXT("path"), SpillPath);
    File->SetStringField(TEXT("contentType"),
        bStructuredBody ? TEXT("application/json") : TEXT("text/plain"));
    // Which half of the response the file body is, named so a caller can branch on it instead of
    // sniffing the first byte.
    File->SetStringField(TEXT("payload"),
        bStructuredBody ? TEXT("structuredContent") : TEXT("text"));
    // TWO numbers, because they answer two questions and conflating them is what made the old
    // gate wrong. `characters` is what was MEASURED against the threshold - one condensed copy of
    // the payload a reader sees. `fileCharacters` is what is on disk: ONE copy either way, but
    // pretty-printed, and pretty-printing costs per LINE - a deeply nested payload lands well
    // above the condensed count, a flat one barely above it. An error's file is its text blocks
    // verbatim, which is a different (larger) selection than the structuredContent that was
    // measured.
    File->SetNumberField(TEXT("characters"), ReaderCharacters);
    File->SetNumberField(TEXT("fileCharacters"), Body.Len());
    File->SetNumberField(TEXT("threshold"), ClampedThreshold);

    const TCHAR* const ShapeNotice = bStructuredBody
        ? TEXT("That file IS the payload: its top level is this response's structuredContent ")
          TEXT("object, not an MCP envelope.")
        : TEXT("That file IS the payload: this response's text content, verbatim, not an MCP ")
          TEXT("envelope.");

    const FString Notice = FString::Printf(
        TEXT("Response exceeds display limit (%d chars, threshold %d); full payload written to %s. %s"),
        ReaderCharacters,
        ClampedThreshold,
        *SpillPath,
        ShapeNotice);

    TSharedPtr<FJsonObject> Structured = MakeShared<FJsonObject>();
    Structured->SetBoolField(TEXT("outputTooLong"), true);
    Structured->SetStringField(TEXT("message"), Notice);
    Structured->SetObjectField(TEXT("file"), File);

    TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
    TextBlock->SetStringField(TEXT("type"), TEXT("text"));
    TextBlock->SetStringField(TEXT("text"), Notice);

    TArray<TSharedPtr<FJsonValue>> Content;
    Content.Add(MakeShared<FJsonValueObject>(TextBlock));

    ToolResult->SetArrayField(TEXT("content"), Content);
    ToolResult->SetObjectField(TEXT("structuredContent"), Structured);
}

void PruneOldHttpResponseSpills()
{
    EnsureCurrentDateTimeDirName();

    const FString Root = GetHttpResponseSpillRoot();
    IFileManager& FileManager = IFileManager::Get();
    if (!FileManager.DirectoryExists(*Root))
    {
        return;
    }

    TArray<FString> Directories;
    FileManager.FindFiles(Directories, *(Root / TEXT("*")), /*Files=*/false, /*Directories=*/true);

    const FDateTime Now = FDateTime::UtcNow();
    for (const FString& DirectoryName : Directories)
    {
        FDateTime DirectoryTime;
        if (!TryParseSpillDateTimeDir(DirectoryName, DirectoryTime))
        {
            continue;
        }

        const FTimespan Age = Now - DirectoryTime;
        if (Age.GetTotalHours() <= RetentionHours)
        {
            continue;
        }

        FString AbsDir = Root / DirectoryName;
        AbsDir = FPaths::ConvertRelativePathToFull(AbsDir);
        FPaths::NormalizeDirectoryName(AbsDir);
        if (!AbsDir.StartsWith(Root + TEXT("/")))
        {
            continue;
        }

        FileManager.DeleteDirectory(*AbsDir, /*RequireExists=*/false, /*Tree=*/true);
    }
}
}
