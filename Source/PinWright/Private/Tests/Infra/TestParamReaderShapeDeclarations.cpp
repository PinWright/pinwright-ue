// Copyright (c) 2026 Alexander Penkin. MIT License.

// A handler that reads a key as a bool, object or array must declare it with a type the
// dispatcher's declared-type gate admits for that shape. Otherwise the gate refuses the only
// values the body can use before the body runs (board B-omit-slot-chain-declared-integer:
// widget.export_xml.omit_slot_chain, widget.describe.include_slot and widget.duplicate.copySlot
// were declared `integer` and read with GetBool, so `true`/`false` drew PARAM_TYPE_MISMATCH;
// widget.set.slot was declared `integer` and read as an object, B-widget-set-slot-declared-integer).
//
// The verdict is the real gate's (PinWrightCheckDeclaredType), not a hand-kept atom table, so a
// declaration is flagged exactly when a caller sending the reader's shape would be refused.
// Reader sites are found in source text, scoped to the REGISTER_RPC_*HANDLER* registration above
// them and closed by the column-0 brace that ends its body; single-line call sites only.

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ParamTypeCheck.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace TestParamReaderShapeDeclarationsHelpers
{
    struct FReaderSite
    {
        FString Method;
        FString Key;
        FString Shape; // "boolean" | "object" | "array"
        FString Location;
    };

    // Reader call prefixes, each followed by TEXT("<key>"), mapped to the JSON shape they consume.
    struct FReaderPrefix { const TCHAR* Prefix; const TCHAR* Shape; };
    const FReaderPrefix ReaderPrefixes[] = {
        {TEXT("Ctx.GetBool(TEXT(\""), TEXT("boolean")},
        {TEXT("Ctx.RequireBool(TEXT(\""), TEXT("boolean")},
        {TEXT("Payload->TryGetBoolField(TEXT(\""), TEXT("boolean")},
        {TEXT("Ctx.GetObject(TEXT(\""), TEXT("object")},
        {TEXT("Ctx.RequireObject(TEXT(\""), TEXT("object")},
        {TEXT("Payload->TryGetObjectField(TEXT(\""), TEXT("object")},
        {TEXT("GetObjectField(Payload, TEXT(\""), TEXT("object")},
        {TEXT("Ctx.GetArray(TEXT(\""), TEXT("array")},
        {TEXT("Ctx.RequireArray(TEXT(\""), TEXT("array")},
        {TEXT("Ctx.GetStringSet(TEXT(\""), TEXT("array")},
        {TEXT("Payload->TryGetArrayField(TEXT(\""), TEXT("array")},
    };

    bool IsIdentifierChar(TCHAR Character)
    {
        return FChar::IsAlnum(Character) || Character == TEXT('_');
    }

    // True when Line opens a handler registration: any REGISTER_RPC_*HANDLER* macro name followed
    // by '(' (REGISTER_RPC_HANDLER, _MUTATING_HANDLER, _HANDLER_TICK_UNSAFE, ...). OutAfterParen
    // is the index just past the '('.
    bool TryFindRegistration(const FString& Line, int32& OutAfterParen)
    {
        const int32 Start = Line.Find(TEXT("REGISTER_RPC_"), ESearchCase::CaseSensitive);
        if (Start == INDEX_NONE || (Start > 0 && IsIdentifierChar(Line[Start - 1])))
        {
            return false;
        }
        int32 Index = Start;
        while (Index < Line.Len() && IsIdentifierChar(Line[Index]))
        {
            ++Index;
        }
        if (!Line.Mid(Start, Index - Start).Contains(TEXT("HANDLER"), ESearchCase::CaseSensitive))
        {
            return false;
        }
        while (Index < Line.Len() && FChar::IsWhitespace(Line[Index]))
        {
            ++Index;
        }
        if (Index >= Line.Len() || Line[Index] != TEXT('('))
        {
            return false;
        }
        OutAfterParen = Index + 1;
        return true;
    }

    bool TryFirstQuoted(const FString& Text, int32 From, FString& OutValue)
    {
        const int32 Open = Text.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
        const int32 Close = Open == INDEX_NONE ? INDEX_NONE
            : Text.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 1);
        if (Close == INDEX_NONE)
        {
            return false;
        }
        OutValue = Text.Mid(Open + 1, Close - Open - 1);
        return true;
    }

    TArray<FReaderSite> CollectReaderSites()
    {
        TArray<FReaderSite> Sites;
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        if (!Plugin.IsValid())
        {
            return Sites;
        }
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(
            Files, *(Plugin->GetBaseDir() / TEXT("Source")), TEXT("*.cpp"), true, false, false);

        for (const FString& File : Files)
        {
            FString Source;
            if (!FFileHelper::LoadFileToString(Source, *File) || !Source.Contains(TEXT("REGISTER_RPC_")))
            {
                continue;
            }
            TArray<FString> Lines;
            Source.ParseIntoArrayLines(Lines, false);
            FString Method;
            bool bMethodOnNextLine = false;
            for (int32 LineIndex = 0; LineIndex < Lines.Num(); ++LineIndex)
            {
                const FString& Line = Lines[LineIndex];
                int32 AfterParen = 0;
                const bool bRegistration = TryFindRegistration(Line, AfterParen);
                if (bRegistration || bMethodOnNextLine)
                {
                    // The method is the first string literal after the '(' or, for a
                    // multi-line registration, on the next non-blank line.
                    if (!bRegistration && Line.TrimStartAndEnd().IsEmpty())
                    {
                        continue;
                    }
                    Method.Empty();
                    const bool bFound = TryFirstQuoted(Line, bRegistration ? AfterParen : 0, Method);
                    bMethodOnNextLine = bRegistration && !bFound;
                    // Test-only verbs are skipped by every registry contract walk.
                    if (Method.StartsWith(TEXT("_test.")))
                    {
                        Method.Empty();
                    }
                    continue;
                }
                // A column-0 brace closes the handler body; file-scope code after it belongs to
                // no verb until the next registration.
                if (Line.StartsWith(TEXT("}")))
                {
                    Method.Empty();
                    continue;
                }
                if (Method.IsEmpty())
                {
                    continue;
                }
                for (const FReaderPrefix& Reader : ReaderPrefixes)
                {
                    const FString Prefix(Reader.Prefix);
                    int32 SearchFrom = 0;
                    while (true)
                    {
                        const int32 Start = Line.Find(Prefix, ESearchCase::CaseSensitive,
                            ESearchDir::FromStart, SearchFrom);
                        if (Start == INDEX_NONE)
                        {
                            break;
                        }
                        const int32 KeyStart = Start + Prefix.Len();
                        const int32 KeyEnd = Line.Find(TEXT("\""), ESearchCase::CaseSensitive,
                            ESearchDir::FromStart, KeyStart);
                        if (KeyEnd == INDEX_NONE)
                        {
                            break;
                        }
                        // Left word boundary: LocalPayload-> / RawPayload-> are other objects.
                        if (Start == 0 || !IsIdentifierChar(Line[Start - 1]))
                        {
                            Sites.Add({Method, Line.Mid(KeyStart, KeyEnd - KeyStart), Reader.Shape,
                                FString::Printf(TEXT("%s:%d"), *FPaths::GetCleanFilename(File), LineIndex + 1)});
                        }
                        SearchFrom = KeyEnd + 1;
                    }
                }
            }
        }
        return Sites;
    }

    // The declared type a wire key resolves to (canonical name, untyped alias, or typed alias).
    bool ResolveDeclaredType(const FString& Method, const FString& Key, FString& OutType)
    {
        for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
        {
            if (Reg.MethodName != Method)
            {
                continue;
            }
            for (const FParamSpec& Param : Reg.Params)
            {
                if (Param.Name == Key || Param.Aliases.Contains(Key))
                {
                    OutType = Param.Type;
                    return true;
                }
                for (const FParamAliasSpec& Alias : Param.TypedAliases)
                {
                    if (Alias.Name == Key)
                    {
                        OutType = Alias.Type.IsEmpty() ? Param.Type : Alias.Type;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    TSharedPtr<FJsonValue> MakeProbe(const FString& Shape)
    {
        if (Shape == TEXT("boolean"))
        {
            return MakeShared<FJsonValueBoolean>(true);
        }
        if (Shape == TEXT("object"))
        {
            return MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
        }
        return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParamReaderShapeDeclarationsTest,
    "PinWright.infra.dispatcher.ParamTypeGate.ShapedReadersDeclaredToAdmitTheirShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FParamReaderShapeDeclarationsTest::RunTest(const FString& Parameters)
{
    using namespace TestParamReaderShapeDeclarationsHelpers;
    using namespace PinWrightParamTypes;

    const TArray<FReaderSite> Sites = CollectReaderSites();

    // Scanner health: the four sites this contract was written for must be seen, or a scanner that
    // silently found nothing would pass.
    struct FKnownSite { const TCHAR* Method; const TCHAR* Key; const TCHAR* Shape; };
    const FKnownSite KnownSites[] = {
        {TEXT("widget.export_xml"), TEXT("omit_slot_chain"), TEXT("boolean")},
        {TEXT("widget.describe"), TEXT("include_slot"), TEXT("boolean")},
        {TEXT("widget.duplicate"), TEXT("copySlot"), TEXT("boolean")},
        {TEXT("widget.set"), TEXT("slot"), TEXT("object")},
    };
    for (const FKnownSite& Known : KnownSites)
    {
        TestTrue(FString::Printf(TEXT("scanner sees %s reader %s.%s"), Known.Shape, Known.Method, Known.Key),
            Sites.ContainsByPredicate([&Known](const FReaderSite& Site)
            {
                return Site.Method == Known.Method && Site.Key == Known.Key && Site.Shape == Known.Shape;
            }));
    }

    int32 Checked = 0;
    for (const FReaderSite& Site : Sites)
    {
        FString DeclaredType;
        // An undeclared key is the declared-param gate's business (UNKNOWN_PARAMS), not this one's.
        if (!ResolveDeclaredType(Site.Method, Site.Key, DeclaredType))
        {
            continue;
        }
        ++Checked;
        TestTrue(FString::Printf(TEXT("%s.%s is read as %s (%s) but declared '%s', which the type gate refuses for that shape"),
                *Site.Method, *Site.Key, *Site.Shape, *Site.Location, *DeclaredType),
            PinWrightCheckDeclaredType(DeclaredType, MakeProbe(Site.Shape)) == EDeclaredTypeVerdict::Accepted);
    }
    AddInfo(FString::Printf(TEXT("reader sites scanned=%d declared=%d"), Sites.Num(), Checked));
    TestTrue(TEXT("scanner correlated a registry-scale number of declared reader sites"), Checked > 500);
    return true;
}
