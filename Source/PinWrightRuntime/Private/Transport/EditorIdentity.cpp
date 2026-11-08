// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Transport/EditorIdentity.h"

#include "Compat/JsonKeyCompat.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Handlers/ErrorCodes.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace PinWrightEditorIdentity
{
namespace
{
    // Unity-build safety: this whole file shares a translation unit with its neighbours, so every
    // helper below carries the PwEditorIdentity_ prefix rather than a generic name.

    const FString PwEditorIdentity_FieldPid            = TEXT("pid");
    const FString PwEditorIdentity_FieldInstanceId     = TEXT("instance_id");
    const FString PwEditorIdentity_FieldProjectFile    = TEXT("project_file");
    const FString PwEditorIdentity_FieldProjectName    = TEXT("project_name");
    const FString PwEditorIdentity_FieldEngineVersion  = TEXT("engine_version");
    const FString PwEditorIdentity_FieldPluginBuild    = TEXT("plugin_build");
    const FString PwEditorIdentity_FieldExecutablePath = TEXT("executable_path");
    const FString PwEditorIdentity_FieldCommandLine    = TEXT("command_line");

    // Paths are folded to lowercase before comparison for the same reason
    // UPinWrightSettings::DerivePortFromPath folds them (PinWrightSettings.cpp:84): this plugin
    // is editor-only and its port derivation - the mechanism that puts two editors on one port -
    // already treats project paths as case-insensitive. Comparing them case-sensitively here
    // would refuse an assertion that names the same project the port hash considers identical.
    FString PwEditorIdentity_NormalizePath(const FString& InPath)
    {
        FString Out = FPaths::ConvertRelativePathToFull(InPath);
        FPaths::NormalizeFilename(Out);
        Out.ToLowerInline();
        return Out;
    }

    // Renders an asserted value for the refusal message. Non-scalars are rejected before this
    // runs, so only string/number/bool reach it.
    FString PwEditorIdentity_ValueToDisplay(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("null");
        }
        FString AsString;
        if (Value->TryGetString(AsString))
        {
            return AsString;
        }
        return TEXT("<unreadable>");
    }

    bool PwEditorIdentity_IsScalar(const TSharedPtr<FJsonValue>& Value)
    {
        return Value.IsValid() &&
            (Value->Type == EJson::String || Value->Type == EJson::Number ||
             Value->Type == EJson::Boolean);
    }
}

const FIdentity& Measured()
{
    // Function-local static: measured once, on the first call, from this process only.
    static const FIdentity Identity = []() -> FIdentity
    {
        FIdentity Out;
        Out.ProcessId = FPlatformProcess::GetCurrentProcessId();
        // Minted here rather than taken from FApp::GetInstanceId(), which `-InstanceId=` can
        // force to a caller-supplied value (App.cpp:182). A token an outside process can dictate
        // cannot separate two editors launched by the same script - see EditorIdentity.h.
        Out.InstanceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
        Out.ProjectFilePath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
        Out.ProjectName = FApp::GetProjectName();
        Out.EngineVersion = FEngineVersion::Current().ToString();
        Out.PluginBuild = FString(TEXT(__DATE__)) + TEXT(" ") + TEXT(__TIME__);
        Out.ExecutablePath = FPlatformProcess::ExecutablePath();
        Out.CommandLine = FCommandLine::Get();
        return Out;
    }();
    return Identity;
}

TSharedRef<FJsonObject> ToJson(const FIdentity& Identity)
{
    TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetNumberField(PwEditorIdentity_FieldPid, static_cast<double>(Identity.ProcessId));
    Out->SetStringField(PwEditorIdentity_FieldInstanceId, Identity.InstanceId);
    Out->SetStringField(PwEditorIdentity_FieldProjectFile, Identity.ProjectFilePath);
    Out->SetStringField(PwEditorIdentity_FieldProjectName, Identity.ProjectName);
    Out->SetStringField(PwEditorIdentity_FieldEngineVersion, Identity.EngineVersion);
    Out->SetStringField(PwEditorIdentity_FieldPluginBuild, Identity.PluginBuild);
    Out->SetStringField(PwEditorIdentity_FieldExecutablePath, Identity.ExecutablePath);
    Out->SetStringField(PwEditorIdentity_FieldCommandLine, Identity.CommandLine);
    return Out;
}

const FString& AssertionParamName()
{
    static const FString Name = TEXT("_expect_editor");
    return Name;
}

const TArray<FString>& AssertableFields()
{
    // executable_path and command_line are published by ToJson but deliberately NOT assertable:
    // both are identical across two editors launched the same way, so asserting on them would
    // read as verification while proving nothing about WHICH process answered.
    static const TArray<FString> Fields = {
        PwEditorIdentity_FieldPid,
        PwEditorIdentity_FieldInstanceId,
        PwEditorIdentity_FieldProjectFile,
        PwEditorIdentity_FieldProjectName,
        PwEditorIdentity_FieldEngineVersion,
        PwEditorIdentity_FieldPluginBuild,
    };
    return Fields;
}

FAssertionVerdict CheckAssertion(const FIdentity& Actual, const TSharedPtr<FJsonValue>& Assertion)
{
    FAssertionVerdict Verdict;

    const FString Vocabulary = FString::Join(AssertableFields(), TEXT(", "));

    if (!Assertion.IsValid() || Assertion->Type != EJson::Object || !Assertion->AsObject().IsValid())
    {
        Verdict.bAccepted = false;
        Verdict.ErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        Verdict.Message = FString::Printf(
            TEXT("'%s' must be an object naming the editor you mean, for example {\"pid\": 1234}. ")
            TEXT("Assertable fields: %s. Call system.identity with no assertion first to read this ")
            TEXT("editor's measured identity, then pin the fields you care about."),
            *AssertionParamName(), *Vocabulary);
        return Verdict;
    }

    const TSharedPtr<FJsonObject> Object = Assertion->AsObject();

    if (Object->Values.Num() == 0)
    {
        // An empty assertion is refused rather than accepted. Accepting it would hand a caller a
        // green answer for a check that compared nothing - the exact false confidence this gate
        // exists to remove.
        Verdict.bAccepted = false;
        Verdict.ErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        Verdict.Message = FString::Printf(
            TEXT("'%s' was empty, so it asserts nothing. Name at least one field: %s."),
            *AssertionParamName(), *Vocabulary);
        return Verdict;
    }

    TArray<FString> UnknownFields;
    TArray<FString> NonScalarFields;
    for (const auto& Pair : Object->Values)
    {
        const FString Key = EARGCompat::JsonKeyToString(Pair.Key);
        if (!AssertableFields().Contains(Key))
        {
            UnknownFields.Add(Key);
        }
        else if (!PwEditorIdentity_IsScalar(Pair.Value))
        {
            NonScalarFields.Add(Key);
        }
    }

    if (UnknownFields.Num() > 0)
    {
        // A misspelled assertion field must not be quietly dropped. Dropping it would leave the
        // caller believing it pinned the target while the request ran wherever it landed.
        Verdict.bAccepted = false;
        Verdict.ErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        Verdict.Message = FString::Printf(
            TEXT("'%s' names field(s) this editor cannot verify: %s. Nothing was compared and the ")
            TEXT("request was NOT executed, because an ignored assertion reads as a passed one. ")
            TEXT("Assertable fields: %s."),
            *AssertionParamName(), *FString::Join(UnknownFields, TEXT(", ")), *Vocabulary);
        return Verdict;
    }

    if (NonScalarFields.Num() > 0)
    {
        Verdict.bAccepted = false;
        Verdict.ErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
        Verdict.Message = FString::Printf(
            TEXT("'%s' field(s) %s must be a string or a number; objects and arrays cannot be ")
            TEXT("compared against a measured identity."),
            *AssertionParamName(), *FString::Join(NonScalarFields, TEXT(", ")));
        return Verdict;
    }

    TSharedPtr<FJsonObject> Expected = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> ActualJson = MakeShared<FJsonObject>();
    TArray<FString> Disagreements;

    auto CompareStringField = [&](const FString& FieldName, const FString& ActualValue,
                                  bool bComparePaths)
    {
        const TSharedPtr<FJsonValue> Value = Object->TryGetField(FieldName);
        if (!Value.IsValid())
        {
            return;
        }
        const FString ExpectedValue = PwEditorIdentity_ValueToDisplay(Value);
        Expected->SetStringField(FieldName, ExpectedValue);
        ActualJson->SetStringField(FieldName, ActualValue);

        const bool bMatches = bComparePaths
            ? PwEditorIdentity_NormalizePath(ExpectedValue) == PwEditorIdentity_NormalizePath(ActualValue)
            : ExpectedValue.Equals(ActualValue, ESearchCase::IgnoreCase);
        if (!bMatches)
        {
            Disagreements.Add(FString::Printf(
                TEXT("%s: expected '%s', this process is '%s'"),
                *FieldName, *ExpectedValue, *ActualValue));
        }
    };

    if (const TSharedPtr<FJsonValue> PidValue = Object->TryGetField(PwEditorIdentity_FieldPid))
    {
        // Accepted as a number or as a numeric string: JSON clients that route ids through
        // string maps would otherwise be unable to assert on the one field that needs no prior
        // handshake. Compared numerically, so 1234 and "1234" and 1234.0 all agree.
        const FString ExpectedDisplay = PwEditorIdentity_ValueToDisplay(PidValue);
        // Both sides of the diff are rendered as strings, including the pid, so `expected` and
        // `actual` are the same shape and a caller can copy `actual` straight back into a
        // corrected assertion (the pid comparison accepts a numeric string).
        Expected->SetStringField(PwEditorIdentity_FieldPid, ExpectedDisplay);
        ActualJson->SetStringField(PwEditorIdentity_FieldPid,
                                   FString::Printf(TEXT("%u"), Actual.ProcessId));

        uint64 ExpectedPid = 0;
        const bool bPidIsNumeric =
            (PidValue->Type == EJson::Number || PidValue->Type == EJson::String) &&
            PidValue->TryGetNumber(ExpectedPid);
        if (!bPidIsNumeric)
        {
            Verdict.bAccepted = false;
            Verdict.ErrorCode = ErrorCodes::ERR_INVALID_PARAMS;
            Verdict.Message = FString::Printf(
                TEXT("'%s.pid' must be a process id, given as a number or a numeric string; got '%s'."),
                *AssertionParamName(), *ExpectedDisplay);
            return Verdict;
        }
        if (ExpectedPid != static_cast<uint64>(Actual.ProcessId))
        {
            Disagreements.Add(FString::Printf(
                TEXT("pid: expected %llu, this process is %u"), ExpectedPid, Actual.ProcessId));
        }
    }

    CompareStringField(PwEditorIdentity_FieldInstanceId, Actual.InstanceId, /*bComparePaths=*/false);
    CompareStringField(PwEditorIdentity_FieldProjectFile, Actual.ProjectFilePath, /*bComparePaths=*/true);
    CompareStringField(PwEditorIdentity_FieldProjectName, Actual.ProjectName, /*bComparePaths=*/false);
    CompareStringField(PwEditorIdentity_FieldEngineVersion, Actual.EngineVersion, /*bComparePaths=*/false);
    CompareStringField(PwEditorIdentity_FieldPluginBuild, Actual.PluginBuild, /*bComparePaths=*/false);

    if (Disagreements.Num() == 0)
    {
        return Verdict;
    }

    Verdict.bAccepted = false;
    Verdict.ErrorCode = ErrorCodes::ERR_EDITOR_IDENTITY_MISMATCH;
    Verdict.Expected = Expected;
    Verdict.Actual = ActualJson;
    Verdict.Message = FString::Printf(
        TEXT("Refused: this editor is not the one you asserted. %s. The MCP port is derived from ")
        TEXT("the PROJECT PATH, so every editor of this project competes for the same port and ")
        TEXT("whichever won the bind answers all of them - you are talking to that one. The ")
        TEXT("request was NOT executed. Read the intended editor's identity from system.identity ")
        TEXT("on the endpoint it actually bound, or stop the editor that holds this port."),
        *FString::Join(Disagreements, TEXT("; ")));
    return Verdict;
}

FAssertionVerdict CheckRequestAssertion(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return FAssertionVerdict();
    }
    const TSharedPtr<FJsonValue> Assertion = Params->TryGetField(AssertionParamName());
    if (!Assertion.IsValid())
    {
        // Asserting is opt-in. A caller that names no editor is served exactly as before.
        return FAssertionVerdict();
    }
    return CheckAssertion(Measured(), Assertion);
}

} // namespace PinWrightEditorIdentity
