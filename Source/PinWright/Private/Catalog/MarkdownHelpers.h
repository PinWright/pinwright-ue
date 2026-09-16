// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

struct FParamSpec;
struct FHandlerRegistration;

namespace MarkdownHelpers
{
    // Collapse newlines and escape `|` so a string is safe to embed inside a Markdown table cell or paragraph.
    PINWRIGHT_API FString EscapeMarkdownText(const FString& InText);

    // Render a single parameter as a bullet line: "- `name` (`type`, required): description Default: `value`."
    // Trailing newline included.
    PINWRIGHT_API FString RenderParamBullet(const FParamSpec& Param);

    // Render the **Parameters** block for a method: header + bullet list, or "Parameters: none." if empty.
    // Trailing blank line included.
    PINWRIGHT_API FString RenderParamList(const TArray<FParamSpec>& Params);
}
