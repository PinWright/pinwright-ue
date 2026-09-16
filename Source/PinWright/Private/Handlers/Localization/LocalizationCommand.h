// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

namespace PinWrightLocalization
{
    enum class EOperation : uint8
    {
        Gather,
        Compile
    };

    /**
     * Normalizes and validates a localization target name. Targets are used for
     * selecting the default config file and are deliberately restricted to a
     * single safe filename component.
     */
    bool ValidateTarget(const FString& InTarget, FString& OutTarget, FString& OutError);

    /**
     * Resolves a project-relative localization config and rejects path traversal,
     * absolute paths, non-INI files, and files outside Config/Localization.
     * An empty ConfigInput selects <Target>_<Operation>.ini under that directory.
     */
    bool ResolveConfig(const FString& ProjectDir, const FString& Target,
        EOperation Operation, const FString& ConfigInput,
        FString& OutRelativePath, FString& OutFullPath, FString& OutError);

    /** Verdict on an already-resolved config's contents. */
    enum class EConfigContentResult : uint8
    {
        /** The config runs the expected commandlet for the target's manifest. */
        Valid,
        /** The config does not run the commandlet class this operation requires. */
        OperationMismatch,
        /** The config's manifest belongs to a different localization target. */
        TargetMismatch
    };

    /**
     * Validates the contents of an already-resolved localization config: it must
     * run the commandlet class the operation expects and its ManifestName must
     * belong to the requested target. Pure text inspection with no file IO, so the
     * rule is testable without a config shipped by the host project.
     */
    EConfigContentResult ValidateConfigContents(const FString& ConfigText,
        const FString& Target, EOperation Operation);

    /** Returns the commandlet class marker expected for an operation. */
    const TCHAR* ExpectedCommandletClass(EOperation Operation);

    /** Returns a stable wire/display name for an operation. */
    const TCHAR* OperationName(EOperation Operation);
}

