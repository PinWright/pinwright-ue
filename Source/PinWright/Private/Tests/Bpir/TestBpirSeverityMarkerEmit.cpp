// Copyright (c) 2026 Alexander Penkin. MIT License.

// Counterfactual: if EBpirWarningSeverity::Error is mapped to `BPIR_WARN:` in
// FormatBpirWarningMarkers, or the helper is removed and the emit reverts to the
// blanket `BPIR_FAILED:` prefix, the BPIR_ERROR assertion below fails.

#include "Misc/AutomationTest.h"

#include "Decompiler/BpirDecompiler.h"
#include "Utils/AssetDumpBuilder.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBpirSeverityMarkerEmitTest,
    "PinWright.bpir.SeverityMarkerEmit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBpirSeverityMarkerEmitTest::RunTest(const FString& Parameters)
{
    TArray<FBpirWarning> Warnings;
    Warnings.Add(FBpirWarning{TEXT("synthetic-warn-message"), EBpirWarningSeverity::Warn});
    Warnings.Add(FBpirWarning{TEXT("synthetic-error-message"), EBpirWarningSeverity::Error});

    FString Emitted;
    AssetDumpBuilder::FormatBpirWarningMarkers(Warnings, Emitted);

    TestTrue(TEXT("Emitted text contains BPIR_WARN: marker for Warn-tagged entry"),
        Emitted.Contains(TEXT("# BPIR_WARN: synthetic-warn-message")));
    TestTrue(TEXT("Emitted text contains BPIR_ERROR: marker for Error-tagged entry"),
        Emitted.Contains(TEXT("# BPIR_ERROR: synthetic-error-message")));

    // Negative: the legacy collapse prefix must not appear when the severity-tagged
    // formatter is in use.
    TestFalse(TEXT("Emitted text does not contain legacy BPIR_FAILED: prefix"),
        Emitted.Contains(TEXT("BPIR_FAILED:")));

    // Negative: error-tagged entries must not be tagged as warn (and vice versa).
    TestFalse(TEXT("Error-tagged message is not emitted under BPIR_WARN: prefix"),
        Emitted.Contains(TEXT("# BPIR_WARN: synthetic-error-message")));
    TestFalse(TEXT("Warn-tagged message is not emitted under BPIR_ERROR: prefix"),
        Emitted.Contains(TEXT("# BPIR_ERROR: synthetic-warn-message")));

    return true;
}
