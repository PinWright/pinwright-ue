// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/FormatterRegistration.h"
#include "Handlers/FormatterJsonUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

using FormatterJsonUtils::TryGetArrayField;

// Compact text envelope for asset.dump's success response. The handler returns
// {writtenPaths[], skipped[], dumpDir, mode} — no in-memory asset content. The
// formatter renders that envelope as a short summary plus path lists. Long
// arrays are truncated to keep the wire format compact.
static bool FormatAssetDump(const TSharedPtr<FJsonObject>& R, FString& OutText)
{
    if (!R.IsValid())
    {
        return false;
    }

    constexpr int32 MaxArrayEntries = 8;

    TArray<FString> Lines;

    FString DumpDir;
    R->TryGetStringField(TEXT("dumpDir"), DumpDir);

    FString Mode;
    R->TryGetStringField(TEXT("mode"), Mode);

    // Heading uses the trailing path segment of the dump directory; falls back
    // to the asset path baked into the directory tail.
    FString Name;
    if (!DumpDir.IsEmpty())
    {
        FString Trimmed = DumpDir;
        while (Trimmed.EndsWith(TEXT("/")) || Trimmed.EndsWith(TEXT("\\")))
        {
            Trimmed.LeftChopInline(1);
        }
        Name = FPaths::GetCleanFilename(Trimmed);
    }
    if (Name.IsEmpty()) Name = TEXT("AssetDump");
    Lines.Add(Name);

    if (!DumpDir.IsEmpty())
    {
        Lines.Add(FString::Printf(TEXT("DumpDir: %s"), *DumpDir));
    }
    if (!Mode.IsEmpty())
    {
        Lines.Add(FString::Printf(TEXT("Mode: %s"), *Mode));
    }

    if (const TArray<TSharedPtr<FJsonValue>>* Written = TryGetArrayField(R, TEXT("writtenPaths")))
    {
        const int32 N = Written->Num();
        Lines.Add(TEXT(""));
        Lines.Add(FString::Printf(TEXT("Files (%d):"), N));
        const int32 Show = FMath::Min(N, MaxArrayEntries);
        for (int32 I = 0; I < Show; ++I)
        {
            const TSharedPtr<FJsonValue>& V = (*Written)[I];
            const FString Path = V.IsValid() ? V->AsString() : FString();
            Lines.Add(FString::Printf(TEXT("  %s"), *FPaths::GetCleanFilename(Path)));
        }
        if (N > Show)
        {
            Lines.Add(FString::Printf(TEXT("  ... (+%d more)"), N - Show));
        }
    }

    if (const TArray<TSharedPtr<FJsonValue>>* Skipped = TryGetArrayField(R, TEXT("skipped")))
    {
        const int32 N = Skipped->Num();
        if (N > 0)
        {
            Lines.Add(TEXT(""));
            Lines.Add(FString::Printf(TEXT("Skipped (%d):"), N));
            const int32 Show = FMath::Min(N, MaxArrayEntries);
            for (int32 I = 0; I < Show; ++I)
            {
                const TSharedPtr<FJsonObject>* Entry = nullptr;
                const TSharedPtr<FJsonValue>& V = (*Skipped)[I];
                if (V.IsValid() && V->TryGetObject(Entry) && Entry && (*Entry).IsValid())
                {
                    FString P, Reason;
                    (*Entry)->TryGetStringField(TEXT("path"), P);
                    (*Entry)->TryGetStringField(TEXT("reason"), Reason);
                    Lines.Add(FString::Printf(TEXT("  %s — %s"), *FPaths::GetCleanFilename(P), *Reason));
                }
                else
                {
                    // Surface corruption rather than hiding it: keep the slot
                    // visible so user-facing counts match the heading and the
                    // trailing "(+K more)" delta.
                    Lines.Add(TEXT("  <invalid entry>"));
                }
            }
            if (N > Show)
            {
                Lines.Add(FString::Printf(TEXT("  ... (+%d more)"), N - Show));
            }
        }
    }

    OutText = FString::Join(Lines, TEXT("\n"));
    return true;
}

REGISTER_RPC_FORMATTER("asset.dump", &FormatAssetDump);
