// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveHandlerCommon.h"

#include "Handlers/Drive/DriveEditorChrome.h"
#include "Handlers/Drive/DriveJson.h"
#include "Handlers/Drive/DriveLiveResolver.h"
#include "Handlers/Drive/DriveSetOfMarkRenderer.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ErrorCodes.h"

#include "JournalRecorder.h"
#include "JournalLiveTail.h"
#include "JournalTypes.h"

#include "Misc/DateTime.h"

namespace
{
    // Lowercase wire token for a recorder capture domain. The recorder's own NDJSON
    // writer uses TitleCase ("Physics"); the drive contract is lowercase, so this is
    // a deliberately separate mapping rather than a shared helper.
    FString DriveDomainToString(EJournalDomain Domain)
    {
        switch (Domain)
        {
        case EJournalDomain::Physics: return TEXT("physics");
        case EJournalDomain::Render:  return TEXT("render");
        case EJournalDomain::Net:     return TEXT("net");
        case EJournalDomain::UI:      return TEXT("ui");
        case EJournalDomain::Loading: return TEXT("loading");
        case EJournalDomain::None:
        default:                      return TEXT("none");
        }
    }

    // Lowercase wire token for an event severity. Matches the severity vocabulary
    // FDriveConditionEval ranks (trace < debug < info < warning < error < fatal).
    FString DriveSeverityToString(EJournalSeverity Severity)
    {
        switch (Severity)
        {
        case EJournalSeverity::Trace:   return TEXT("trace");
        case EJournalSeverity::Debug:   return TEXT("debug");
        case EJournalSeverity::Info:    return TEXT("info");
        case EJournalSeverity::Warning: return TEXT("warning");
        case EJournalSeverity::Error:   return TEXT("error");
        case EJournalSeverity::Fatal:   return TEXT("fatal");
        default:                        return TEXT("info");
        }
    }

    // %.17g round-trips a double and keeps '.' as the decimal separator, mirroring the
    // recorder's NDJSON number formatting so a value reads the same on both surfaces.
    FString DriveFormatNumber(double Value)
    {
        if (!FMath::IsFinite(Value))
        {
            return TEXT("null");
        }
        return FString::Printf(TEXT("%.17g"), Value);
    }

    // Flatten a recorded value to a single human-readable string for the drive contract.
    FString DriveRecordedValueToString(const FRecordedValue& Value)
    {
        switch (Value.Kind)
        {
        case EJournalKind::String:
            return Value.S;
        case EJournalKind::Enum:
            // The member name is the readable form; fall back to the underlying int.
            return Value.S.IsEmpty() ? DriveFormatNumber(Value.F0) : Value.S;
        case EJournalKind::Bool:
            return Value.F0 != 0.0 ? TEXT("true") : TEXT("false");
        case EJournalKind::Float:
        case EJournalKind::Int:
            return DriveFormatNumber(Value.F0);
        case EJournalKind::Vec2:
            return FString::Printf(TEXT("[%s,%s]"),
                *DriveFormatNumber(Value.F0), *DriveFormatNumber(Value.F1));
        case EJournalKind::Vec3:
        case EJournalKind::Rotator:
            return FString::Printf(TEXT("[%s,%s,%s]"),
                *DriveFormatNumber(Value.F0), *DriveFormatNumber(Value.F1), *DriveFormatNumber(Value.F2));
        case EJournalKind::Vec4:
        case EJournalKind::Quat:
            return FString::Printf(TEXT("[%s,%s,%s,%s]"),
                *DriveFormatNumber(Value.F0), *DriveFormatNumber(Value.F1),
                *DriveFormatNumber(Value.F2), *DriveFormatNumber(Value.F3));
        default:
            return FString();
        }
    }

    // Compose a single readable name for a variable series from its (Key, Tag) identity.
    // The recorder serializes both separately; here they collapse to "Key.Tag" (or just
    // the tag for object-less, global series).
    FString DriveVariableName(FName Key, FName Tag)
    {
        if (Key.IsNone())
        {
            return Tag.ToString();
        }
        return FString::Printf(TEXT("%s.%s"), *Key.ToString(), *Tag.ToString());
    }
}

EDriveSurface FDriveHandlerCommon::ResolveSurface(const FHandlerContext& Ctx)
{
    EDriveSurface Surface = EDriveSurface::Auto;
    // Empty or unrecognized tokens leave Surface at Auto; Auto resolves to Game for v1.
    FDriveJson::SurfaceFromString(Ctx.GetString(TEXT("surface")), Surface);
    if (Surface == EDriveSurface::Auto)
    {
        Surface = EDriveSurface::Game;
    }
    return Surface;
}

FDriveRootSelector FDriveHandlerCommon::ParseRootSelector(const FHandlerContext& Ctx)
{
    FDriveRootSelector Selector;
    // Same selector vocabulary (and camelCase aliases) as the live-UI snapshot RPCs.
    Selector.InstanceName = Ctx.GetStringFirstOf({ TEXT("instance_name"), TEXT("instanceName") });
    Selector.RootIndex = Ctx.GetIntFirstOf({ TEXT("root_index"), TEXT("rootIndex") });
    return Selector;
}

FDriveWindowSelector FDriveHandlerCommon::ParseWindowSelector(const FHandlerContext& Ctx)
{
    FDriveWindowSelector Selector;
    // Distinct vocabulary from the game root selector: editor chrome is addressed by
    // window title (substring) and ordered window index, not by UMG instance_name.
    Selector.Title = Ctx.GetStringFirstOf({ TEXT("title"), TEXT("window_title") });
    Selector.Index = Ctx.GetIntFirstOf({ TEXT("window_index"), TEXT("index") });
    return Selector;
}

bool FDriveHandlerCommon::ParseScreenshotToFile(const FHandlerContext& Ctx)
{
    // screenshot_mode=file writes the PNG out and returns its path instead of inlining base64.
    return Ctx.GetString(TEXT("screenshot_mode"), TEXT("inline")).Equals(TEXT("file"), ESearchCase::IgnoreCase);
}

bool FDriveHandlerCommon::GetElementsForSurface(
    EDriveSurface Surface,
    const FDriveRootSelector& Selector,
    TArray<FDriveElement>& OutElements,
    FString& OutRootName,
    FString& OutErrorCode,
    FString& OutErrorMessage,
    const FDriveWindowSelector& WindowSelector)
{
    // The single provider switch. New surfaces are wired by adding a case here; the
    // synchronous handlers and the async verbs all funnel through this point.
    switch (Surface)
    {
    case EDriveSurface::Game:
        return FDriveLiveResolver::BuildElementList(
            Selector, OutElements, OutRootName, OutErrorCode, OutErrorMessage);

    case EDriveSurface::EditorChrome:
        // BuildElementList writes the resolved window title into OutRootName, mirroring
        // the game surface where OutRootName is the live UMG root name.
        return FDriveEditorChrome::BuildElementList(
            WindowSelector, OutElements, OutRootName, OutErrorCode, OutErrorMessage);

    case EDriveSurface::Web:
    case EDriveSurface::Auto:
    default:
        OutErrorCode = ErrorCodes::ERR_SURFACE_NOT_SUPPORTED;
        OutErrorMessage = FString::Printf(
            TEXT("Drive surface '%s' is not supported yet; it is added in a later phase. Only 'game' and 'editor_chrome' are available in this phase."),
            *FDriveJson::SurfaceToString(Surface));
        return false;
    }
}

FDriveResolveResult FDriveHandlerCommon::ResolveForSurface(
    EDriveSurface Surface,
    const FHandlerContext& Ctx,
    const FString& Handle)
{
    switch (Surface)
    {
    case EDriveSurface::Game:
        return FDriveLiveResolver::ResolveHandle(ParseRootSelector(Ctx), Handle);

    case EDriveSurface::EditorChrome:
        return FDriveEditorChrome::ResolveHandle(ParseWindowSelector(Ctx), Handle);

    case EDriveSurface::Web:
    case EDriveSurface::Auto:
    default:
    {
        // Unsupported surfaces have no live UI to walk; report the canonical code via
        // the resolver's NoLiveUi channel so the action flow forwards it unchanged.
        FDriveResolveResult Result;
        Result.Status = EDriveResolveStatus::NoLiveUi;
        Result.ErrorCode = ErrorCodes::ERR_SURFACE_NOT_SUPPORTED;
        Result.ErrorMessage = FString::Printf(
            TEXT("Drive surface '%s' is not supported yet; it is added in a later phase. Only 'game' and 'editor_chrome' are available in this phase."),
            *FDriveJson::SurfaceToString(Surface));
        return Result;
    }
    }
}

FDriveJournalDelta FDriveHandlerCommon::MapJournalDelta(const FLiveTailDelta& Delta)
{
    FDriveJournalDelta Out;
    Out.Cursor = static_cast<int64>(Delta.Cursor);

    Out.Events.Reserve(Delta.Events.Num());
    for (const FLiveTailEvent& Event : Delta.Events)
    {
        FDriveJournalEvent Mapped;
        Mapped.Id = FString::Printf(TEXT("%llu"), Event.Seq);
        Mapped.Ts = Event.Ts;
        Mapped.Domain = DriveDomainToString(Event.Domain);
        Mapped.Severity = DriveSeverityToString(Event.Severity);
        Mapped.Name = Event.Name.ToString();
        Mapped.Props.Reserve(Event.Props.Num());
        for (const TPair<FName, FRecordedValue>& Prop : Event.Props)
        {
            FDriveJournalProp MappedProp;
            MappedProp.Key = Prop.Key.ToString();
            MappedProp.Value = DriveRecordedValueToString(Prop.Value);
            Mapped.Props.Add(MoveTemp(MappedProp));
        }
        Out.Events.Add(MoveTemp(Mapped));
    }

    Out.ChangedVariables.Reserve(Delta.Variables.Num());
    for (const FLiveTailVariable& Variable : Delta.Variables)
    {
        FDriveJournalVariable Mapped;
        Mapped.Name = DriveVariableName(Variable.Key, Variable.Tag);
        Mapped.Value = DriveRecordedValueToString(Variable.Value);
        Out.ChangedVariables.Add(MoveTemp(Mapped));
    }

    return Out;
}

bool FDriveHandlerCommon::GetJournalDelta(uint64 SinceCursor, FDriveJournalDelta& Out)
{
    Out = FDriveJournalDelta();

    // GetLiveTail() is null whenever no recording session/PIE is active. That is the
    // clean "nothing to report" path, not an error: leave Out empty (cursor 0).
    FJournalLiveTail* Tail = FJournalRecorder::GetLiveTail();
    if (!Tail)
    {
        return false;
    }

    Out = MapJournalDelta(Tail->QuerySince(SinceCursor));
    return true;
}

int32 FDriveHandlerCommon::EstimateElementJsonBytes(const FDriveElement& Element)
{
    // Approximate upper bound on FDriveJson::WriteElement()'s size; see the header for the
    // over-count rationale and its non-ASCII limits.
    constexpr int32 FixedOverheadBytes = 256;
    return Element.Handle.Len() + Element.Type.Len() + Element.Label.Len()
        + Element.Value.Len() + FixedOverheadBytes;
}

void FDriveHandlerCommon::FilterObservationElements(
    TArray<FDriveElement>& Elements,
    bool bInteractablesOnly,
    int32 MaxElements,
    int32 MaxBytes,
    int32& OutOmittedCount)
{
    OutOmittedCount = 0;

    if (bInteractablesOnly)
    {
        Elements.RemoveAll([](const FDriveElement& Element) { return !Element.bInteractable; });
    }

    if (MaxElements > 0 && Elements.Num() > MaxElements)
    {
        OutOmittedCount = Elements.Num() - MaxElements;
        Elements.SetNum(MaxElements);
    }

    // Byte-aware cap (see the header for why MaxElements can't bound payload bytes): keep adding
    // elements until the estimated serialized size would exceed MaxBytes, then drop the rest into
    // OutOmittedCount. The first element is always kept (a lone over-budget element is returned
    // rather than an empty list), matching the list verbs' "at least one row" behavior. The empty
    // array is a no-op here: the loop leaves Keep == 0 and the drop condition below is false.
    if (MaxBytes > 0)
    {
        int32 RunningBytes = 0;
        int32 Keep = 0;
        for (; Keep < Elements.Num(); ++Keep)
        {
            const int32 Cost = EstimateElementJsonBytes(Elements[Keep]);
            if (Keep > 0 && RunningBytes + Cost > MaxBytes)
            {
                break;
            }
            RunningBytes += Cost;
        }
        if (Keep < Elements.Num())
        {
            OutOmittedCount += Elements.Num() - Keep;
            Elements.SetNum(Keep);
        }
    }
}

bool FDriveHandlerCommon::BuildObservation(
    EDriveSurface Surface,
    const FDriveRootSelector& Selector,
    bool bScreenshot,
    int32 MarkCap,
    bool bIncludeJournal,
    uint64 JournalSince,
    FDriveObservation& Out,
    FString& OutErrorCode,
    FString& OutErrorMessage,
    const FDriveWindowSelector& WindowSelector,
    bool bInteractablesOnly,
    int32 MaxElements,
    bool bScreenshotToFile,
    int32 MaxBytes)
{
    Out = FDriveObservation();
    Out.Surface = Surface;

    // Elements are required: a failure here fails the whole observation.
    if (!GetElementsForSurface(Surface, Selector, Out.Elements, Out.RootName, OutErrorCode, OutErrorMessage, WindowSelector))
    {
        return false;
    }

    // Compact the list before anything reads it (so the screenshot marks the same set the
    // caller receives). A no-op unless the explicit observe asked for filtering.
    FilterObservationElements(Out.Elements, bInteractablesOnly, MaxElements, MaxBytes, Out.OmittedCount);

    // Screenshot is best-effort: a capture failure leaves the screenshot unset and
    // the observation still succeeds with its element list. The surface + window
    // selector pick the capture source (game viewport vs. the selected editor window).
    if (bScreenshot)
    {
        FDriveScreenshot Screenshot;
        FString ScreenshotErrorCode;
        if (FDriveSetOfMarkRenderer::CaptureAnnotated(Out.Elements, MarkCap, Screenshot, ScreenshotErrorCode, Surface, WindowSelector, bScreenshotToFile))
        {
            Out.Screenshot = MoveTemp(Screenshot);
        }
    }

    // Journal is optional but, when requested, always attached: an absent live tail
    // yields an empty delta (cursor 0) rather than omitting the field.
    if (bIncludeJournal)
    {
        FDriveJournalDelta Delta;
        GetJournalDelta(JournalSince, Delta);
        Out.Journal = MoveTemp(Delta);
    }

    Out.Frame = static_cast<int64>(GFrameCounter);
    Out.Timestamp = FDateTime::UtcNow();
    return true;
}
