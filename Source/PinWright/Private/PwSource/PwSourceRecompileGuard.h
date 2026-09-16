// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared source-of-truth guard for .pwmodel, .pwskel and .pwanim recompiles.
#pragma once

#include "CoreMinimal.h"

#include "PwSource/PwDiagnostic.h"

struct FPwSourceStateEntry
{
    // Stable machine key stored in the provenance stamp.
    FString Key;

    // Short user-facing field name, for example previewMesh or socket[Grip].
    FString Field;

    // Canonical value. Missing entries and empty strings are intentionally distinct only when
    // the producer supplies a key for the empty state.
    FString Value;

    // How state outside the source can normally appear. This is printed in the diagnostic so a
    // caller sees both what would be lost and where to look for the write that introduced it.
    FString Origin;

    // True when Value names an asset. The live side of such a field reads GetPathName(), which is
    // the OBJECT path /Pkg.Object, while a source file spells the same asset as the PACKAGE path
    // /Pkg. A raw string compare then reports a loss for one asset written two ways, and the
    // diagnostic's own remedy ("put the state in the source") cannot be carried out because no
    // source spelling matches. Comparison normalizes both sides for these entries only; every
    // other value is compared verbatim.
    bool bAssetPath = false;
};

struct FPwSourceRecompileGuardRequest
{
    FString FormatName;
    FString AssetPath;
    bool bSameSourceRecompile = false;
    bool bTakeover = false;
    bool bOverwrite = false;
    FString CurrentSourcePath;
    FString RequestedSourcePath;
    int32 Line = -1;
    int32 Column = -1;

    int32 BaselineVersion = 0;
    const TMap<FString, FString>* Baseline = nullptr;
    TArray<FPwSourceStateEntry> Current;
    TArray<FPwSourceStateEntry> Desired;
};

namespace PwSourceRecompileGuard
{
    inline constexpr int32 StateVersion = 1;

    // Checks both same-source recompiles and takeovers. Same-source checks trust a matching
    // generated-state baseline to distinguish an ordinary source edit from an out-of-band live
    // edit. A takeover cannot trust another source's baseline, so every current value the incoming
    // source would replace or omit is named. Returns false when overwrite=true is still required;
    // the exact same code is a warning when overwrite grants permission.
    PINWRIGHT_API bool Check(const FPwSourceRecompileGuardRequest& Request,
                            TArray<FPwDiagnostic>& OutDiagnostics);

    // Canonical map written into the provenance stamp after a successful compile.
    PINWRIGHT_API TMap<FString, FString> MakeStateMap(
        TArrayView<const FPwSourceStateEntry> State);
}
