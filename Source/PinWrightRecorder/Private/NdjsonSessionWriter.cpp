// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "NdjsonSessionWriter.h"

#include "Containers/StringConv.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"

namespace
{
    /** Stable wire name for a value kind (emitted as the `kind` field). */
    const TCHAR* KindName(EJournalKind Kind)
    {
        switch (Kind)
        {
        case EJournalKind::Float:   return TEXT("Float");
        case EJournalKind::Int:     return TEXT("Int");
        case EJournalKind::Bool:    return TEXT("Bool");
        case EJournalKind::Vec2:    return TEXT("Vec2");
        case EJournalKind::Vec3:    return TEXT("Vec3");
        case EJournalKind::Vec4:    return TEXT("Vec4");
        case EJournalKind::Quat:    return TEXT("Quat");
        case EJournalKind::Rotator: return TEXT("Rotator");
        case EJournalKind::Enum:    return TEXT("Enum");
        case EJournalKind::String:  return TEXT("String");
        default:                      return TEXT("Float");
        }
    }

    /** Stable wire name for a capture domain (emitted as the `dom` / `domain` field). */
    const TCHAR* DomainName(EJournalDomain Domain)
    {
        switch (Domain)
        {
        case EJournalDomain::Physics: return TEXT("Physics");
        case EJournalDomain::Render:  return TEXT("Render");
        case EJournalDomain::Net:     return TEXT("Net");
        case EJournalDomain::UI:      return TEXT("UI");
        case EJournalDomain::Loading: return TEXT("Loading");
        case EJournalDomain::None:
        default:                        return TEXT("None");
        }
    }

    /** Stable wire name for an event severity (emitted as the `sev` field). */
    const TCHAR* SeverityName(EJournalSeverity Severity)
    {
        switch (Severity)
        {
        case EJournalSeverity::Trace:   return TEXT("Trace");
        case EJournalSeverity::Debug:   return TEXT("Debug");
        case EJournalSeverity::Warning: return TEXT("Warning");
        case EJournalSeverity::Error:   return TEXT("Error");
        case EJournalSeverity::Fatal:   return TEXT("Fatal");
        case EJournalSeverity::Info:
        default:                          return TEXT("Info");
        }
    }

    /** Round-trippable invariant-culture number; NaN/Inf collapse to JSON null. */
    FString FormatNumber(double Value)
    {
        if (!FMath::IsFinite(Value))
        {
            return TEXT("null");
        }
        // %.17g round-trips a double; the C locale keeps '.' as the decimal point.
        return FString::Printf(TEXT("%.17g"), Value);
    }
}

FNdjsonSessionWriter::FNdjsonSessionWriter(const FString& FilePath)
{
    // Truncating writer with shared read access, so external tools can tail the file
    // mid-session. A raw IFileHandle (not IFileManager::CreateFileWriter) because the
    // FArchive file writer's Flush() always ends in IFileHandle::Flush() — FlushFileBuffers
    // on Windows, a physical-media sync the per-drain Flush() here must never pay
    // (see Flush() in the header).
    Handle.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*FilePath, /*bAppend*/ false, /*bAllowRead*/ true));
}

FNdjsonSessionWriter::~FNdjsonSessionWriter()
{
    // Push any unflushed lines to the OS; deleting the handle closes the file. The OS
    // writes its cache back lazily — no device flush on close either.
    Flush();
    Handle.Reset();
}

void FNdjsonSessionWriter::WriteHeader(const FString& SessionId, int32 Fmt, const FString& EngineVersion, const FString& StartUtcIso, double T0)
{
    FString Line;
    Line.Reserve(256);
    Line.AppendChar(TEXT('{'));
    AppendString(Line, TEXT("k"), TEXT("header")); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("session"), SessionId); Line.AppendChar(TEXT(','));
    AppendRaw(Line, TEXT("fmt"), FString::FromInt(Fmt)); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("engine"), EngineVersion); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("startUtc"), StartUtcIso); Line.AppendChar(TEXT(','));
    AppendRaw(Line, TEXT("t0"), FormatNumber(T0)); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("axis"), TEXT("time")); Line.AppendChar(TEXT(','));
    AppendRaw(Line, TEXT("domains"), TEXT("[\"None\",\"Physics\",\"Render\",\"Net\",\"UI\",\"Loading\"]"));
    Line.AppendChar(TEXT('}'));
    WriteLine(Line);
}

void FNdjsonSessionWriter::WriteObject(const FObjectRecord& Record)
{
    FString Line;
    Line.Reserve(256);
    Line.AppendChar(TEXT('{'));
    AppendString(Line, TEXT("k"), TEXT("obj")); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("key"), Record.Key.ToString()); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("label"), Record.Label); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("path"), Record.Path); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("type"), Record.Type); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("parent"), Record.ParentKey.IsNone() ? FString() : Record.ParentKey.ToString()); Line.AppendChar(TEXT(','));
    AppendRaw(Line, TEXT("tFirst"), FormatNumber(Record.TFirst));
    Line.AppendChar(TEXT('}'));
    WriteLine(Line);
}

void FNdjsonSessionWriter::WriteVariable(const FVariableManifest& Manifest)
{
    FString Line;
    Line.Reserve(256);
    Line.AppendChar(TEXT('{'));
    AppendString(Line, TEXT("k"), TEXT("var")); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("tag"), Manifest.Tag.ToString()); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("kind"), KindName(Manifest.Kind)); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("unit"), Manifest.Unit); Line.AppendChar(TEXT(','));
    AppendRaw(Line, TEXT("eps"), FormatNumber(Manifest.Epsilon)); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("domain"), DomainName(Manifest.Domain)); Line.AppendChar(TEXT(','));
    AppendRaw(Line, TEXT("tFirst"), FormatNumber(Manifest.TFirst));
    Line.AppendChar(TEXT('}'));
    WriteLine(Line);
}

void FNdjsonSessionWriter::WriteValue(double Ts, EJournalDomain Domain, double DomainTime, int64 DomainFrame, FName Key, FName Tag, const FRecordedValue& Value)
{
    FString Line;
    Line.Reserve(256);
    Line.AppendChar(TEXT('{'));
    AppendString(Line, TEXT("k"), TEXT("val")); Line.AppendChar(TEXT(','));
    AppendRaw(Line, TEXT("t"), FormatNumber(Ts)); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("dom"), DomainName(Domain)); Line.AppendChar(TEXT(','));
    AppendRaw(Line, TEXT("dt"), FormatNumber(DomainTime)); Line.AppendChar(TEXT(','));
    if (DomainFrame >= 0)
    {
        AppendRaw(Line, TEXT("df"), FString::Printf(TEXT("%lld"), DomainFrame)); Line.AppendChar(TEXT(','));
    }
    AppendString(Line, TEXT("key"), Key.IsNone() ? FString() : Key.ToString()); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("tag"), Tag.ToString()); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("kind"), KindName(Value.Kind)); Line.AppendChar(TEXT(','));
    AppendValuePayload(Line, Value);
    Line.AppendChar(TEXT('}'));
    WriteLine(Line);
}

void FNdjsonSessionWriter::WriteEvent(int32 EventId, double Ts, EJournalDomain Domain, FName Key, FName Name, EJournalSeverity Severity, const TArray<TPair<FName, FRecordedValue>>& Props)
{
    FString Line;
    Line.Reserve(256);
    Line.AppendChar(TEXT('{'));
    AppendString(Line, TEXT("k"), TEXT("evt")); Line.AppendChar(TEXT(','));
    AppendRaw(Line, TEXT("id"), FString::FromInt(EventId)); Line.AppendChar(TEXT(','));
    AppendRaw(Line, TEXT("t"), FormatNumber(Ts)); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("dom"), DomainName(Domain)); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("key"), Key.IsNone() ? FString() : Key.ToString()); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("name"), Name.ToString()); Line.AppendChar(TEXT(','));
    AppendString(Line, TEXT("sev"), SeverityName(Severity)); Line.AppendChar(TEXT(','));
    Line.Append(TEXT("\"props\":"));
    AppendPropsObject(Line, Props);
    Line.AppendChar(TEXT('}'));
    WriteLine(Line);
}

void FNdjsonSessionWriter::Flush()
{
    if (!Handle.IsValid() || PendingBytes.Num() == 0)
    {
        return;
    }

    // One plain write() into the OS page cache per drain tick; the OS syncs to the device on
    // its own schedule. Success or failure, drop the buffered bytes — a best-effort journal
    // must not grow the buffer without bound behind a persistently failing handle.
    Handle->Write(PendingBytes.GetData(), PendingBytes.Num());
    PendingBytes.Reset();
}

void FNdjsonSessionWriter::AppendString(FString& Out, const FString& Name, const FString& Value) const
{
    AppendEscapedString(Out, Name);
    Out.AppendChar(TEXT(':'));
    AppendEscapedString(Out, Value);
}

void FNdjsonSessionWriter::AppendRaw(FString& Out, const FString& Name, const FString& RawJson) const
{
    AppendEscapedString(Out, Name);
    Out.AppendChar(TEXT(':'));
    Out.Append(RawJson);
}

void FNdjsonSessionWriter::AppendEscapedString(FString& Out, const FString& Value) const
{
    Out.AppendChar(TEXT('"'));
    for (const TCHAR Ch : Value)
    {
        switch (Ch)
        {
        case TEXT('"'):  Out.Append(TEXT("\\\"")); break;
        case TEXT('\\'): Out.Append(TEXT("\\\\")); break;
        case TEXT('\b'): Out.Append(TEXT("\\b")); break;
        case TEXT('\f'): Out.Append(TEXT("\\f")); break;
        case TEXT('\n'): Out.Append(TEXT("\\n")); break;
        case TEXT('\r'): Out.Append(TEXT("\\r")); break;
        case TEXT('\t'): Out.Append(TEXT("\\t")); break;
        default:
            if (Ch < 0x20)
            {
                Out.Append(FString::Printf(TEXT("\\u%04x"), static_cast<int32>(Ch)));
            }
            else
            {
                Out.AppendChar(Ch);
            }
            break;
        }
    }
    Out.AppendChar(TEXT('"'));
}

void FNdjsonSessionWriter::AppendValuePayload(FString& Out, const FRecordedValue& Value) const
{
    switch (Value.Kind)
    {
    case EJournalKind::String:
        AppendString(Out, TEXT("s"), Value.S);
        break;

    case EJournalKind::Enum:
        AppendRaw(Out, TEXT("v"), FormatNumber(Value.F0)); Out.AppendChar(TEXT(','));
        AppendString(Out, TEXT("s"), Value.S);
        break;

    case EJournalKind::Float:
    case EJournalKind::Int:
    case EJournalKind::Bool:
        AppendRaw(Out, TEXT("v"), FormatNumber(Value.F0));
        break;

    case EJournalKind::Vec2:
        AppendRaw(Out, TEXT("v"), FString::Printf(TEXT("[%s,%s]"), *FormatNumber(Value.F0), *FormatNumber(Value.F1)));
        break;

    case EJournalKind::Vec3:
    case EJournalKind::Rotator:
        AppendRaw(Out, TEXT("v"), FString::Printf(TEXT("[%s,%s,%s]"), *FormatNumber(Value.F0), *FormatNumber(Value.F1), *FormatNumber(Value.F2)));
        break;

    case EJournalKind::Vec4:
    case EJournalKind::Quat:
        AppendRaw(Out, TEXT("v"), FString::Printf(TEXT("[%s,%s,%s,%s]"), *FormatNumber(Value.F0), *FormatNumber(Value.F1), *FormatNumber(Value.F2), *FormatNumber(Value.F3)));
        break;
    }
}

void FNdjsonSessionWriter::AppendPropsObject(FString& Out, const TArray<TPair<FName, FRecordedValue>>& Props) const
{
    if (Props.Num() == 0)
    {
        Out.Append(TEXT("{}"));
        return;
    }

    Out.AppendChar(TEXT('{'));
    bool bFirst = true;
    for (const TPair<FName, FRecordedValue>& Prop : Props)
    {
        if (!bFirst)
        {
            Out.AppendChar(TEXT(','));
        }
        bFirst = false;
        AppendEscapedString(Out, Prop.Key.ToString());
        Out.AppendChar(TEXT(':'));

        const FRecordedValue& Value = Prop.Value;
        switch (Value.Kind)
        {
        case EJournalKind::String:
        case EJournalKind::Enum:
            AppendEscapedString(Out, Value.S);
            break;
        case EJournalKind::Bool:
            Out.Append(Value.F0 != 0.0 ? TEXT("true") : TEXT("false"));
            break;
        default:
            Out.Append(FormatNumber(Value.F0));
            break;
        }
    }
    Out.AppendChar(TEXT('}'));
}

void FNdjsonSessionWriter::WriteLine(const FString& Line)
{
    if (!Handle.IsValid())
    {
        return;
    }

    // Buffer as UTF-8 bytes (no BOM) followed by a single '\n'. No I/O per line — the bytes
    // reach the OS in one write() when FJournalSession::Flush() runs (once per drain, i.e.
    // once per game frame). Per-line I/O — and any device flush at all — stalled the game
    // thread under real write volume (see Flush() in the header).
    auto Utf8 = StringCast<UTF8CHAR>(*Line);
    PendingBytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length() * sizeof(UTF8CHAR));
    PendingBytes.Add(static_cast<uint8>('\n'));
}
