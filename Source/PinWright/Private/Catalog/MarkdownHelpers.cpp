// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Catalog/MarkdownHelpers.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"

namespace MarkdownHelpers
{
    FString EscapeMarkdownText(const FString& InText)
    {
        FString Result = InText;
        Result.ReplaceInline(TEXT("\r\n"), TEXT(" "));
        Result.ReplaceInline(TEXT("\n"), TEXT(" "));
        Result.ReplaceInline(TEXT("\r"), TEXT(" "));
        Result.ReplaceInline(TEXT("|"), TEXT("\\|"));
        return Result.TrimStartAndEnd();
    }

    FString RenderParamBullet(const FParamSpec& Param)
    {
        const TCHAR* Requirement = Param.bRequired ? TEXT("required") : TEXT("optional");
        FString Line = FString::Printf(TEXT("- `%s` (`%s`, %s)"),
            *Param.Name,
            *EscapeMarkdownText(Param.Type),
            Requirement);

        const FString Description = EscapeMarkdownText(Param.Description);
        if (!Description.IsEmpty())
        {
            Line += FString::Printf(TEXT(": %s"), *Description);
        }

        if (!Param.Default.IsEmpty())
        {
            Line += FString::Printf(TEXT(" Default: `%s`."), *EscapeMarkdownText(Param.Default));
        }

        if (Param.Aliases.Num() > 0)
        {
            TArray<FString> EscapedAliases;
            EscapedAliases.Reserve(Param.Aliases.Num());
            for (const FString& Alias : Param.Aliases)
            {
                EscapedAliases.Add(FString::Printf(TEXT("`%s`"), *EscapeMarkdownText(Alias)));
            }
            Line += FString::Printf(TEXT(" Aliases: %s."), *FString::Join(EscapedAliases, TEXT(", ")));
        }

        if (Param.TypedAliases.Num() > 0)
        {
            TArray<FString> EscapedTypedAliases;
            EscapedTypedAliases.Reserve(Param.TypedAliases.Num());
            for (const FParamAliasSpec& Alias : Param.TypedAliases)
            {
                EscapedTypedAliases.Add(FString::Printf(TEXT("`%s` (`%s`)"),
                    *EscapeMarkdownText(Alias.Name),
                    *EscapeMarkdownText(Alias.Type)));
            }
            Line += FString::Printf(TEXT(" Typed aliases: %s."), *FString::Join(EscapedTypedAliases, TEXT(", ")));
        }

        Line += TEXT("\n");
        return Line;
    }

    FString RenderParamList(const TArray<FParamSpec>& Params)
    {
        if (Params.Num() == 0)
        {
            return TEXT("Parameters: none.\n\n");
        }

        FString Out = TEXT("**Parameters**\n\n");
        for (const FParamSpec& Param : Params)
        {
            Out += RenderParamBullet(Param);
        }
        Out += TEXT("\n");
        return Out;
    }
}
