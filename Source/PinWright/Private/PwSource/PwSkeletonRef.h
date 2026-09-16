// Copyright (c) 2026 Alexander Penkin. MIT License.

// PwSkeletonRef.h - shared resolution of a `use skeleton from "..."` source reference.
//
// The path names a compiled Unreal asset, never a source file.  The format-specific callers
// supply their own diagnostic constants so .pwmodel and .pwanim keep parallel code sets while
// sharing the path, class, and reference-skeleton checks.
#pragma once

#include "CoreMinimal.h"

#include "PwDiagnostic.h"
#include "PwDocument.h"

class USkeleton;

namespace PwSkeletonRef
{
    // Identical failure meanings have format-specific prefixes.  Keeping the four code values
    // at the seam prevents either format from quietly growing a different resolver policy.
    struct FDiagnosticCodes
    {
        const TCHAR* NotAnAssetPath = nullptr;
        const TCHAR* NotFound = nullptr;
        const TCHAR* WrongKind = nullptr;
        const TCHAR* HasNoBones = nullptr;

        bool IsComplete() const
        {
            return NotAnAssetPath != nullptr && NotFound != nullptr
                && WrongKind != nullptr && HasNoBones != nullptr;
        }
    };

    struct FResult
    {
        // Non-null only when the reference passed all checks.
        USkeleton* Skeleton = nullptr;

        // The authored spelling is retained for response/diagnostic context.  ObjectPath is
        // the normalized package.object form used for the load, or the loaded object's actual
        // path on success.
        FString AuthoredPath;
        FString ObjectPath;

        bool IsResolved() const
        {
            return Skeleton != nullptr;
        }
    };

    // Resolve one FPwUse whose kind is `skeleton` against a mounted Unreal asset path.
    //
    // This function deliberately does not inspect a file extension, read source text, or infer
    // a source-vs-asset meaning.  Invalid path syntax, a missing object, a wrong UObject class,
    // and a zero-bone USkeleton are separate diagnostics supplied by Codes.
    PINWRIGHT_API FResult Resolve(const FPwUse& Use,
                                  const FDiagnosticCodes& Codes,
                                  TArray<FPwDiagnostic>& OutDiagnostics);
}
