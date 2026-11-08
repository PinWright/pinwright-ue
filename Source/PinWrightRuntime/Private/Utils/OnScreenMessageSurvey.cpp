// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/OnScreenMessageSurvey.h"

#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CoreDelegates.h"
#include "RHIGlobals.h"

namespace PinWrightOnScreenMessages
{
    namespace
    {
        // The engine's own severity->colour mapping when it draws these
        // (UnrealEngine.cpp:13448-13462), so the published colour is what the frame carries rather
        // than a convention invented here.
        FLinearColor SeverityColor(const FCoreDelegates::EOnScreenMessageSeverity Severity)
        {
            switch (Severity)
            {
            case FCoreDelegates::EOnScreenMessageSeverity::Error:   return FLinearColor::Red;
            case FCoreDelegates::EOnScreenMessageSeverity::Warning: return FLinearColor::Yellow;
            default:                                                return FLinearColor::White;
            }
        }

        const TCHAR* SeverityKey(const FCoreDelegates::EOnScreenMessageSeverity Severity)
        {
            switch (Severity)
            {
            case FCoreDelegates::EOnScreenMessageSeverity::Error:   return TEXT("error");
            case FCoreDelegates::EOnScreenMessageSeverity::Warning: return TEXT("warning");
            default:                                               return TEXT("info");
            }
        }

        TSharedPtr<FJsonObject> MakeColorObject(const FLinearColor& Color)
        {
            TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
            Object->SetNumberField(TEXT("r"), Color.R);
            Object->SetNumberField(TEXT("g"), Color.G);
            Object->SetNumberField(TEXT("b"), Color.B);
            return Object;
        }
    }

    bool ShouldReportDemotedLocalMemory(const uint64 DemotedLocalBytes, const int32 WarningCVarValue)
    {
        // Both halves are the renderer's, verbatim (SceneRendering.cpp:4452-4457): the cvar is
        // compared against 1 exactly, not treated as a truthy flag.
        return WarningCVarValue == 1 && DemotedLocalBytes > 0;
    }

    FString MakeDemotedLocalMemoryText(const uint64 DemotedLocalBytes)
    {
        // Byte-for-byte the renderer's format string (SceneRendering.cpp:4804), including the
        // float narrowing, so the published text matches what is burned into the pixels and a
        // caller can grep one against the other.
        return FString::Printf(
            TEXT("Video memory has been exhausted (%.3f MB over budget). Expect extremely poor performance."),
            float(DemotedLocalBytes) / 1048576.0f);
    }

    FOnScreenMessageSurvey Survey()
    {
        FOnScreenMessageSurvey Surveyed;
        Surveyed.bMeasured = true;
        Surveyed.bScreenMessagesEnabled = GAreScreenMessagesEnabled;
        Surveyed.bMapWarningsSuppressed = GEngine && GEngine->bSuppressMapWarnings;
        if (!Surveyed.bScreenMessagesEnabled || Surveyed.bMapWarningsSuppressed)
        {
            // The gate the engine itself applies before drawing any of this
            // (SceneRendering.cpp:4634). With it against them, an empty list is a measurement.
            return Surveyed;
        }

        FCoreDelegates::FSeverityMessageMap DelegateMessages;
        FCoreDelegates::OnGetOnScreenMessages.Broadcast(DelegateMessages);
        for (const TPair<FCoreDelegates::EOnScreenMessageSeverity, FText>& Pair : DelegateMessages)
        {
            FOnScreenMessage& Message = Surveyed.Messages.AddDefaulted_GetRef();
            Message.Severity = SeverityKey(Pair.Key);
            Message.Source = TEXT("coreDelegate");
            Message.Text = Pair.Value.ToString();
            Message.Color = SeverityColor(Pair.Key);
        }

        static const IConsoleVariable* DemotedWarningCVar =
            IConsoleManager::Get().FindConsoleVariable(TEXT("r.DemotedLocalMemoryWarning"));
        const int32 DemotedWarningValue = DemotedWarningCVar ? DemotedWarningCVar->GetInt() : 0;
        if (ShouldReportDemotedLocalMemory(GDemotedLocalMemorySize, DemotedWarningValue))
        {
            FOnScreenMessage& Message = Surveyed.Messages.AddDefaulted_GetRef();
            // Red, and an error: FScreenMessageWriter::DrawLine defaults to
            // FLinearColor(1.0, 0.05, 0.05) (CanvasTypes.h:874) and the renderer passes no colour
            // for this line.
            Message.Severity = TEXT("error");
            Message.Source = TEXT("renderer");
            Message.Text = MakeDemotedLocalMemoryText(GDemotedLocalMemorySize);
            Message.Color = FLinearColor(1.0f, 0.05f, 0.05f, 1.0f);
        }

        return Surveyed;
    }

    void AddOnScreenMessageFields(const FOnScreenMessageSurvey& Surveyed,
                                  const TSharedPtr<FJsonObject>& Viewport)
    {
        if (!Surveyed.bMeasured || !Viewport.IsValid())
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> Entries;
        Entries.Reserve(Surveyed.Messages.Num());
        for (const FOnScreenMessage& Message : Surveyed.Messages)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("severity"), Message.Severity);
            Entry->SetStringField(TEXT("source"), Message.Source);
            Entry->SetStringField(TEXT("text"), Message.Text);
            Entry->SetObjectField(TEXT("color"), MakeColorObject(Message.Color));
            Entries.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Viewport->SetArrayField(TEXT("onScreenMessages"), Entries);
        Viewport->SetNumberField(TEXT("onScreenMessageCount"), Surveyed.Messages.Num());
        Viewport->SetBoolField(TEXT("screenMessagesEnabled"), Surveyed.bScreenMessagesEnabled);
        Viewport->SetBoolField(TEXT("mapWarningsSuppressed"), Surveyed.bMapWarningsSuppressed);

        if (Surveyed.Messages.Num() > 0)
        {
            FString First = Surveyed.Messages[0].Text;
            First.ReplaceInline(TEXT("\n"), TEXT(" "));
            Viewport->SetStringField(TEXT("onScreenMessageWarning"), FString::Printf(
                TEXT("The engine drew %d on-screen message(s) into this frame, so this text is IN "
                     "the pixels and also in `imageStats` -- the first reads: \"%s\". No show flag, "
                     "game view or hideEditorSprites removes it, and every other contamination "
                     "field can read clean on a frame carrying it. A frame with a renderer error "
                     "banner across it cannot be used for a blind A/B against reference stills: "
                     "clear the underlying condition (a VRAM-over-budget banner usually needs a "
                     "fresh editor) and re-shoot."),
                Surveyed.Messages.Num(), *First));
        }
    }
}
