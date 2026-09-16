// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace HttpResponseSpill
{
    PINWRIGHT_API const TCHAR* GetInternalSkipParamName();
    PINWRIGHT_API int32 GetDefaultThresholdCharacters();
    PINWRIGHT_API int32 ClampThresholdCharacters(int32 ThresholdCharacters);

    PINWRIGHT_API bool ShouldSkipHttpResponseSpill(
        const TSharedPtr<FJsonObject>& Params);

    PINWRIGHT_API FString GetHttpResponseSpillRoot();
    PINWRIGHT_API FString GetHttpResponseSpillDateTimeDir();

#if WITH_DEV_AUTOMATION_TESTS
    PINWRIGHT_API void SetHttpResponseSpillRootOverrideForTests(const FString& Root);
#endif

    PINWRIGHT_API bool WriteSpilledHttpResponse(
        const FString& Body,
        FString& OutAbsPath,
        FString& OutError);

    PINWRIGHT_API TSharedPtr<FJsonObject> BuildSpillReferenceResponse(
        int32 Characters,
        int32 ThresholdCharacters,
        const FString& Path);

    PINWRIGHT_API TSharedPtr<FJsonObject> MaybeBuildSpilledResponse(
        const TSharedRef<FJsonObject>& Response,
        int32 ThresholdCharacters,
        bool bSkipSpill);

    // For MCP `tools/call` success results, which bypass the envelope-level spill:
    // if the wrapped ToolResult serializes larger than the threshold, spill the
    // full payload to a file and rewrite it in place so content[0].text becomes a
    // short "Response exceeds display limit" notice and structuredContent carries
    // {outputTooLong:true, message, file:{path,...}}. No-op when within threshold.
    // The file holds the payload itself - the `structuredContent` object at the
    // file's top level, or the text content verbatim when there is no structured
    // half - never the MCP envelope; `file.payload` names which of the two it is.
    // An ERROR result always spills its text, which is the only place its
    // `[<CODE>] <message>` line exists (structuredContent holds the bare payload).
    PINWRIGHT_API void MarkOversizedToolResult(
        const TSharedRef<FJsonObject>& ToolResult,
        int32 ThresholdCharacters);

    PINWRIGHT_API void PruneOldHttpResponseSpills();
}
