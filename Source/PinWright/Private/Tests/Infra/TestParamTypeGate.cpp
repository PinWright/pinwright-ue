// Copyright (c) 2026 Alexander Penkin. MIT License.

// The dispatcher's declared-type gate: FRpcDispatcher::ValidateHandlerParams now reads
// FParamSpec::Type and refuses a value whose JSON shape cannot carry it (board
// B-param-type-never-validated). Until that landed, `Type` was declared on 5,418 parameters,
// printed into every wiki page as a contract, guarded by a registry-wide GRAMMAR test - and read
// by no runtime code, so a caller who spelled a key right and shaped it wrong got a success
// payload built on a coerced or defaulted value.
//
// WHY THESE TESTS ROUTE THROUGH DispatcherTestHelpers AND NOT InvokeHandler. Tests/TestUtils.h's
// InvokeHandler calls Reg.Func(Ctx) straight out of the registration list and reaches neither call
// site of ValidateHandlerParams, so it cannot see this gate at all. Every assertion here dispatches
// a real payload through FRpcDispatcher::ProcessRequest.
//
// THE FAILING-BEFORE PROPERTY, stated per test rather than assumed:
//   * the null and wrong-shape refusals below all answered `success` (or a DIFFERENT error raised
//     from inside the handler body) before the gate existed;
//   * WidgetReplaceClassRefusesNullPreserveProperties is the destructive instance C1 - without the
//     gate, `preserveProperties: null` cleared FJsonObject::HasField (UE 5.8 returns TRUE for
//     EJson::Null), coerced to `false`, and the handler ran on to skip the only call to
//     CopyMatchingProperties, discarding every text, brush, color, padding, font and style value
//     on the replaced widget while answering `{"success": true}` with the flag echoed nowhere.
//     The assertion is on the ERROR CODE, so a pre-fix run fails on the handler-body error it
//     produced instead (NOT_FOUND for the probe path) rather than passing by accident;
//   * AcceptsLosslessCoercions and MissingRequiredParamWinsOverTypeFault are the guards in the
//     other direction - they fail if the gate is ever tightened into a JSON-Schema-strict one, or
//     reordered ahead of the two passes whose answers a caller must act on first.

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dispatch/RpcDispatcher.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Tests/Infra/DispatcherTestHelpers.h"
#include "Tests/Infra/ParamSpecTestHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Test-only registrations. Deliberately read NO parameter: every assertion here is about whether
// the dispatcher lets the body run at all, and a body that reads nothing cannot mask a gate that
// failed to fire. `_test.` verbs are skipped by every registry-wide contract walk
// (TestContractConsistency.cpp:29/65/486).
// ---------------------------------------------------------------------------

static int32 GParamTypeGateBodyCallCount = 0;

REGISTER_RPC_HANDLER("_test.param_type_gate", "_test",
    "Declared-type gate fixture: one param of each shape the gate switches on.",
    RPC_PARAMS(
        RPC_PARAM_REQ("numericValue", "number", "Required number"),
        RPC_PARAM_OPT("wholeCount", "integer", "Optional whole int32"),
        RPC_PARAM_OPT("label", "string", "Optional string"),
        RPC_PARAM_OPT("flag", "bool", "Optional boolean, spelled with the legacy `bool` token"),
        RPC_PARAM_OPT("names", "array", "Optional array"),
        RPC_PARAM_OPT("options", "object", "Optional object"),
        RPC_PARAM_OPT("payload", "any", "Optional untyped slot"),
        RPC_PARAM_OPT("either", "array|string", "Optional union")
    ))
{
    GParamTypeGateBodyCallCount++;
    Ctx.SendSuccess(TEXT("param_type_gate ok"));
    return true;
}

// A typed alias whose declared type differs from its canonical's, which is the entire reason
// FParamAliasSpec carries a Type of its own (ParamSpec.h:11).
static int32 GParamTypeGateAliasBodyCallCount = 0;

REGISTER_RPC_HANDLER("_test.param_type_gate_alias", "_test",
    "Declared-type gate fixture: a typed alias that is an array where its canonical is a string.",
    RPC_PARAMS(
        FParamSpec{
            TEXT("path"),
            TEXT("string"),
            TEXT("Required path"),
            true,
            TEXT(""),
            TArray<FString>({TEXT("assetPath")}),
            TArray<FParamAliasSpec>({
                FParamAliasSpec{TEXT("pathCandidates"), TEXT("array"), TEXT("Candidate paths")}
            })
        }
    ))
{
    GParamTypeGateAliasBodyCallCount++;
    Ctx.SendSuccess(TEXT("param_type_gate_alias ok"));
    return true;
}

namespace ParamTypeGateTests
{
    const FString GExpectedCode = ErrorCodes::ERR_PARAM_TYPE_MISMATCH;

    struct FOutcome
    {
        bool bResponded = false;
        bool bSuccess = false;
        FString ErrorCode;
        FString Message;
    };

    // Dispatch one payload and settle any deferral, mirroring RequiredParamGate::Probe in
    // TestContractConsistency.cpp: ProcessRequest can park a request on PendingQueue (the
    // safe-point gate, or the Saving/GC gate) and a bare FRpcDispatcher has no subsystem ticker
    // to drain it, so without this the sink stays silent and every assertion reads a
    // default-constructed capture.
    FOutcome DispatchAndSettle(FRpcDispatcher& Dispatcher, DispatcherTestHelpers::FSinkPtr& Sink,
                               const FString& Method, const FString& RequestId,
                               const TSharedPtr<FJsonObject>& Payload)
    {
        FOutcome Out;
        bool bSuccess = true;
        FString ErrorCode;
        DispatcherTestHelpers::Dispatch(Dispatcher, Sink, Method, RequestId, Payload,
                                        bSuccess, ErrorCode);
        if (!Sink->bWasCalled)
        {
            Dispatcher.ProcessPendingRequests();
        }

        Out.bResponded = Sink->bWasCalled;
        Out.bSuccess = Sink->bSuccess;
        Out.ErrorCode = Sink->ErrorCode;
        Out.Message = Sink->Message;
        return Out;
    }

    TSharedPtr<FJsonObject> MakeGatePayload()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetNumberField(TEXT("numericValue"), 1.0);
        return Payload;
    }

    // Assert one payload is refused by the type gate, naming the offending key in the message.
    void TestRefused(FAutomationTestBase& Test, const FString& What, const FString& Method,
                     const TSharedPtr<FJsonObject>& Payload, const FString& OffendingKey,
                     int32& BodyCallCount)
    {
        FRpcDispatcher Dispatcher;
        DispatcherTestHelpers::FSinkPtr Sink;
        DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

        BodyCallCount = 0;
        const FOutcome Out = DispatchAndSettle(Dispatcher, Sink, Method,
                                               FString::Printf(TEXT("type-gate-%s"), *What), Payload);

        Test.TestTrue(*FString::Printf(TEXT("%s: dispatcher responded"), *What), Out.bResponded);
        Test.TestFalse(*FString::Printf(TEXT("%s: response is an error"), *What), Out.bSuccess);
        Test.TestEqual(*FString::Printf(TEXT("%s: error code (message: %s)"), *What, *Out.Message),
                       Out.ErrorCode, GExpectedCode);
        Test.TestTrue(*FString::Printf(TEXT("%s: message names '%s' (message: %s)"),
                                       *What, *OffendingKey, *Out.Message),
                      Out.Message.Contains(FString::Printf(TEXT("'%s'"), *OffendingKey)));
        Test.TestEqual(*FString::Printf(TEXT("%s: handler body did NOT run"), *What),
                       BodyCallCount, 0);
    }

    struct FIntegerReaderNames
    {
        TMap<FString, TSet<FString>> ByMethod;
        TSet<FString> Unscoped;
        FString PluginBaseDir;
        FString SourceRoot;
        bool bPluginBaseExists = false;
        bool bSourceRootExists = false;
        int32 SourceFilesScanned = 0;
        int32 SourceFilesLoaded = 0;
        int32 RegistrationLinesFound = 0;
        int32 ReaderCallsFound = 0;
        int32 FilesWithRegistrationToken = 0;
        int32 FilesWithFirstOfToken = 0;
        FString FirstSourceFile;
        int32 FirstSourceChars = 0;
        FString ActorSourceFile;
        int32 ActorSourceChars = 0;
        bool bActorHasFirstOfToken = false;
    };

    void AddReaderNames(FIntegerReaderNames& Out, const FString& Method,
        const TSet<FString>& Names)
    {
        TSet<FString>& Destination = Method.IsEmpty()
            ? Out.Unscoped
            : Out.ByMethod.FindOrAdd(Method);
        for (const FString& Name : Names)
        {
            Destination.Add(Name);
        }
    }

    void CollectLiteralNames(const FString& Text, const FString& Prefix,
        TSet<FString>& OutNames)
    {
        int32 SearchFrom = 0;
        while (true)
        {
            const int32 Start = Text.Find(Prefix, ESearchCase::CaseSensitive,
                ESearchDir::FromStart, SearchFrom);
            if (Start == INDEX_NONE)
            {
                break;
            }
            const int32 KeyStart = Start + Prefix.Len();
            const int32 KeyEnd = Text.Find(TEXT("\""), ESearchCase::CaseSensitive,
                ESearchDir::FromStart, KeyStart);
            if (KeyEnd == INDEX_NONE)
            {
                break;
            }
            OutNames.Add(Text.Mid(KeyStart, KeyEnd - KeyStart));
            SearchFrom = KeyEnd + 1;
        }
    }

    bool TryGetRegisteredMethod(const FString& Line, FString& OutMethod)
    {
        const FString Prefix(TEXT("REGISTER_RPC_HANDLER(\""));
        const int32 Start = Line.Find(Prefix, ESearchCase::CaseSensitive);
        if (Start == INDEX_NONE)
        {
            return false;
        }
        const int32 MethodStart = Start + Prefix.Len();
        const int32 MethodEnd = Line.Find(TEXT("\""), ESearchCase::CaseSensitive,
            ESearchDir::FromStart, MethodStart);
        if (MethodEnd == INDEX_NONE)
        {
            return false;
        }
        OutMethod = Line.Mid(MethodStart, MethodEnd - MethodStart);
        return true;
    }

    void CollectIntegerReaderNames(FIntegerReaderNames& Out)
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        Out.PluginBaseDir = Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
        Out.SourceRoot = Plugin.IsValid() ? (Plugin->GetBaseDir() / TEXT("Source")) : FString();
        Out.bPluginBaseExists = !Out.PluginBaseDir.IsEmpty()
            && IFileManager::Get().DirectoryExists(*Out.PluginBaseDir);
        Out.bSourceRootExists = !Out.SourceRoot.IsEmpty()
            && IFileManager::Get().DirectoryExists(*Out.SourceRoot);
        if (!Out.bSourceRootExists)
        {
            return;
        }
        TArray<FString> HeaderFiles;
        TArray<FString> SourceFiles;
        IFileManager::Get().FindFilesRecursive(
            HeaderFiles, *Out.SourceRoot, TEXT("*.h"), true, false, false);
        IFileManager::Get().FindFilesRecursive(
            SourceFiles, *Out.SourceRoot, TEXT("*.cpp"), true, false, false);
        TArray<FString> Files;
        Files.Append(SourceFiles);
        Files.Append(HeaderFiles);
        Out.SourceFilesScanned = Files.Num();
        const TArray<FString> ReaderPrefixes = {
            TEXT("Ctx.GetInt(TEXT(\""),
            TEXT("Ctx.GetIntOr(TEXT(\""),
            TEXT("Ctx.RequireInt(TEXT(\"")
        };

        for (const FString& File : Files)
        {
            FString Source;
            if (!FFileHelper::LoadFileToString(Source, *File))
            {
                continue;
            }
            ++Out.SourceFilesLoaded;
            if (Out.SourceFilesLoaded == 1)
            {
                Out.FirstSourceFile = File;
                Out.FirstSourceChars = Source.Len();
            }
            if (File.EndsWith(TEXT("InstancedMeshHandler.cpp")))
            {
                Out.ActorSourceFile = File;
                Out.ActorSourceChars = Source.Len();
                Out.bActorHasFirstOfToken = Source.Contains(TEXT("Ctx.GetIntFirstOf("));
            }
            if (Source.Contains(TEXT("REGISTER_RPC_HANDLER")))
            {
                ++Out.FilesWithRegistrationToken;
            }
            if (Source.Contains(TEXT("Ctx.GetIntFirstOf(")))
            {
                ++Out.FilesWithFirstOfToken;
            }

            TArray<FString> Lines;
            Source.ParseIntoArrayLines(Lines);
            FString CurrentMethod;
            FString PendingFirstOf;
            for (const FString& Line : Lines)
            {
                FString RegisteredMethod;
                if (TryGetRegisteredMethod(Line, RegisteredMethod))
                {
                    CurrentMethod = RegisteredMethod;
                    PendingFirstOf.Empty();
                    ++Out.RegistrationLinesFound;
                    continue;
                }

                TSet<FString> Names;
                for (const FString& Prefix : ReaderPrefixes)
                {
                    CollectLiteralNames(Line, Prefix, Names);
                }

                const int32 FirstOfStart = Line.Find(TEXT("Ctx.GetIntFirstOf("),
                    ESearchCase::CaseSensitive);
                if (FirstOfStart != INDEX_NONE)
                {
                    PendingFirstOf = Line.Mid(FirstOfStart);
                }
                else if (!PendingFirstOf.IsEmpty())
                {
                    PendingFirstOf += TEXT(" ");
                    PendingFirstOf += Line;
                }

                if (!PendingFirstOf.IsEmpty() && PendingFirstOf.Contains(TEXT(");")))
                {
                    CollectLiteralNames(PendingFirstOf, TEXT("TEXT(\""), Names);
                    PendingFirstOf.Empty();
                }

                if (Names.Num() > 0)
                {
                    ++Out.ReaderCallsFound;
                    AddReaderNames(Out, CurrentMethod, Names);
                }
            }
        }
    }

    bool IsDiscreteReaderName(const FString& ReaderName)
    {
        TArray<FString> Tokens;
        FString Current;
        for (int32 Index = 0; Index < ReaderName.Len(); ++Index)
        {
            const TCHAR Character = ReaderName[Index];
            const bool bUppercase = Character >= TEXT('A') && Character <= TEXT('Z');
            const bool bPreviousLowercase = Index > 0
                && ReaderName[Index - 1] >= TEXT('a') && ReaderName[Index - 1] <= TEXT('z');
            if (Character == TEXT('_') || Character == TEXT('-') || (bUppercase && bPreviousLowercase))
            {
                if (!Current.IsEmpty())
                {
                    Current.ToLowerInline();
                    Tokens.Add(Current);
                    Current.Empty();
                }
            }
            if (Character != TEXT('_') && Character != TEXT('-'))
            {
                Current.AppendChar(Character);
            }
        }
        if (!Current.IsEmpty())
        {
            Current.ToLowerInline();
            Tokens.Add(Current);
        }

        static const TSet<FString> DiscreteTokens = {
            TEXT("index"), TEXT("count"), TEXT("slot"), TEXT("lod"), TEXT("level"),
            TEXT("layer"), TEXT("frame")
        };
        bool bHasDiscreteToken = false;
        for (const FString& Token : Tokens)
        {
            if (DiscreteTokens.Contains(Token))
            {
                bHasDiscreteToken = true;
                break;
            }
        }
        if (!bHasDiscreteToken)
        {
            return false;
        }

        // These names contain a discrete-looking token but carry a path/name,
        // continuous frame/LOD value, or string enum instead of an integer selector.
        if (Tokens.Contains(TEXT("path")) || Tokens.Contains(TEXT("name"))
            || Tokens.Contains(TEXT("rate")) || Tokens.Contains(TEXT("time"))
            || Tokens.Contains(TEXT("duration")) || Tokens.Contains(TEXT("distance"))
            || ReaderName.Equals(TEXT("autoIndex"), ESearchCase::IgnoreCase)
            // A gameplay tag is a dotted name, never an ordinal: layerTag pairs `tag` with the
            // discrete token `layer`, and classifying it on the `layer` half alone is what
            // declared it `integer` and made every tag value unreachable at the wire gate.
            || Tokens.Contains(TEXT("tag"))
            // lodType selects a named LOD variant and is declared/read as a string enum.
            || ReaderName.Equals(TEXT("lodType"), ESearchCase::IgnoreCase))
        {
            return false;
        }
        return true;
    }

    // This is called only for names collected from GetInt/GetIntOr/RequireInt. Keep the
    // registration-shape classifier's lodType exclusion, but re-admit the exact token here so a
    // future integer reader cannot bypass declaration correlation.
    bool IsIntegerReaderDeclarationCandidate(const FString& ReaderName)
    {
        return IsDiscreteReaderName(ReaderName)
            || ReaderName.Equals(TEXT("lodType"), ESearchCase::IgnoreCase);
    }

    bool ResolveReaderDeclaration(const TArray<FParamSpec>& Params, const FString& ReaderName,
        FString& OutOwnerName, FString& OutType)
    {
        for (const FParamSpec& Param : Params)
        {
            if (Param.Name == ReaderName || Param.Aliases.Contains(ReaderName))
            {
                OutOwnerName = Param.Name;
                OutType = Param.Type;
                return true;
            }
            for (const FParamAliasSpec& Alias : Param.TypedAliases)
            {
                if (Alias.Name == ReaderName)
                {
                    OutOwnerName = Param.Name;
                    OutType = Alias.Type.IsEmpty() ? Param.Type : Alias.Type;
                    return true;
                }
            }
        }
        return false;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParamTypeGateDiscreteDeclarationRatchetTest,
    "PinWright.infra.dispatcher.ParamTypeGate.DiscreteReadersHaveIntegerDeclarations",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FParamTypeGateDiscreteDeclarationRatchetTest::RunTest(const FString& Parameters)
{
    ParamTypeGateTests::FIntegerReaderNames ReaderNames;
    ParamTypeGateTests::CollectIntegerReaderNames(ReaderNames);
    AddInfo(FString::Printf(
        TEXT("Discrete reader scanner plugin_base='%s' plugin_base_exists=%s source_root='%s' "
             "source_root_exists=%s source_files_scanned=%d source_files_loaded=%d "
             "registration_files=%d first_of_files=%d registration_lines=%d reader_calls=%d "
             "methods=%d unscoped_names=%d first_file='%s' first_chars=%d actor_file='%s' "
             "actor_chars=%d actor_has_first_of=%s"),
        *ReaderNames.PluginBaseDir,
        ReaderNames.bPluginBaseExists ? TEXT("true") : TEXT("false"),
        *ReaderNames.SourceRoot,
        ReaderNames.bSourceRootExists ? TEXT("true") : TEXT("false"),
        ReaderNames.SourceFilesScanned,
        ReaderNames.SourceFilesLoaded,
        ReaderNames.FilesWithRegistrationToken,
        ReaderNames.FilesWithFirstOfToken,
        ReaderNames.RegistrationLinesFound,
        ReaderNames.ReaderCallsFound,
        ReaderNames.ByMethod.Num(),
        ReaderNames.Unscoped.Num(),
        *ReaderNames.FirstSourceFile,
        ReaderNames.FirstSourceChars,
        *ReaderNames.ActorSourceFile,
        ReaderNames.ActorSourceChars,
        ReaderNames.bActorHasFirstOfToken ? TEXT("true") : TEXT("false")));

    auto CheckDerivedReaderShape = [this, &ReaderNames](const TCHAR* Label,
        const FString& Method, bool bSharedReader)
    {
        const TSet<FString> AcceptedNames =
            ParamSpecTestHelpers::CollectAcceptedParamNames(Method);
        TSet<FString> ExpectedDiscreteNames;
        for (const FString& Name : AcceptedNames)
        {
            if (ParamTypeGateTests::IsDiscreteReaderName(Name))
            {
                ExpectedDiscreteNames.Add(Name);
            }
        }
        const TSet<FString>* ScannedNames = bSharedReader
            ? &ReaderNames.Unscoped
            : ReaderNames.ByMethod.Find(Method);
        bool bAllExpectedNamesScanned = ExpectedDiscreteNames.Num() > 0 && ScannedNames != nullptr;
        if (bAllExpectedNamesScanned)
        {
            for (const FString& Name : ExpectedDiscreteNames)
            {
                if (!ScannedNames->Contains(Name))
                {
                    bAllExpectedNamesScanned = false;
                    break;
                }
            }
        }
        return TestTrue(FString::Printf(TEXT("%s (derived %d accepted discrete names)"),
            Label, ExpectedDiscreteNames.Num()), bAllExpectedNamesScanned);
    };

    // Shape coverage protects the scanner itself: the expected aliases come from the live
    // registration and the same discrete classifier used by the ratchet, while the observed names
    // come from the real scoped/shared reader source.
    CheckDerivedReaderShape(TEXT("GetIntFirstOf aliases are scanned for actor.set_instance_transforms"),
        TEXT("actor.set_instance_transforms"), false);
    CheckDerivedReaderShape(TEXT("window selector aliases are scanned from the shared reader"),
        TEXT("drive.observe"), true);
    CheckDerivedReaderShape(TEXT("GetIntFirstOf aliases are scanned for sequencer.set_playhead"),
        TEXT("sequencer.set_playhead"), false);

    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        if (Reg.MethodName != TEXT("_test.param_type_gate_alias"))
        {
            continue;
        }
        FString TypedOwnerName;
        FString TypedOwnerType;
        TestTrue(TEXT("typed alias owner is inspected"),
            ParamTypeGateTests::ResolveReaderDeclaration(Reg.Params, TEXT("pathCandidates"),
                TypedOwnerName, TypedOwnerType));
        TestEqual(TEXT("typed alias keeps its own declared type"), TypedOwnerType,
            FString(TEXT("array")));
    }

    for (const FHandlerRegistration& Reg : FAutoRegisterHandler::GetPendingRegistrations())
    {
        TSet<FString> AcceptedReaderNames;
        if (const TSet<FString>* Scoped = ReaderNames.ByMethod.Find(Reg.MethodName))
        {
            for (const FString& Name : *Scoped)
            {
                AcceptedReaderNames.Add(Name);
            }
        }

        const TSet<FString> AcceptedNames =
            ParamSpecTestHelpers::CollectAcceptedParamNames(Reg.MethodName);
        for (const FString& ReaderName : AcceptedReaderNames)
        {
            if (!AcceptedNames.Contains(ReaderName)
                || !ParamTypeGateTests::IsIntegerReaderDeclarationCandidate(ReaderName))
            {
                continue;
            }

            FString OwnerName;
            FString OwnerType;
            const bool bOwnerFound = ParamTypeGateTests::ResolveReaderDeclaration(
                Reg.Params, ReaderName, OwnerName, OwnerType);

            TestTrue(FString::Printf(TEXT("%s reader key '%s' is declared"),
                *Reg.MethodName, *ReaderName), bOwnerFound);
            if (bOwnerFound)
            {
                TestTrue(FString::Printf(TEXT("%s.%s is declared integer (declared '%s')"),
                    *Reg.MethodName, *OwnerName, *OwnerType),
                    OwnerType.Equals(TEXT("integer"), ESearchCase::IgnoreCase));
            }
        }
    }

    return true;
}

// ============================================================================
// A required parameter sent as JSON null is refused, not coerced
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParamTypeGateRequiredNullTest,
    "PinWright.infra.dispatcher.ParamTypeGate.RefusesRequiredParamSentAsNull",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FParamTypeGateRequiredNullTest::RunTest(const FString& Parameters)
{
    // The gate logs its refusal through LogRpcDispatcher at Warning, exactly as the unknown-name
    // pass above it does. UAutomationControllerSettings::bElevateLogWarningsToErrors defaults to
    // TRUE, so a warning raised inside a running test is promoted to an error - the same reason
    // FDispatcherUnknownParamsDiscoveryGuidanceTest sets this flag (Tests/Infra/TestDispatcher.cpp).
    bSuppressLogWarnings = true;

    // UE 5.8's FJsonObject::HasField tests only that the shared pointer is valid and returns TRUE
    // for EJson::Null, so this payload cleared the required-param gate and landed in the handler
    // as 0. It must now be refused, and refused as a TYPE fault rather than as a missing one -
    // the caller did send the key, and "omit it" is different advice from "you forgot it".
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetField(TEXT("numericValue"), MakeShared<FJsonValueNull>());

    ParamTypeGateTests::TestRefused(*this, TEXT("required-null"), TEXT("_test.param_type_gate"),
                                    Payload, TEXT("numericValue"), GParamTypeGateBodyCallCount);

    return true;
}

// ============================================================================
// An optional parameter sent as JSON null is refused too
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParamTypeGateOptionalNullTest,
    "PinWright.infra.dispatcher.ParamTypeGate.RefusesOptionalParamSentAsNull",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FParamTypeGateOptionalNullTest::RunTest(const FString& Parameters)
{
    // The gate logs its refusal through LogRpcDispatcher at Warning, exactly as the unknown-name
    // pass above it does. UAutomationControllerSettings::bElevateLogWarningsToErrors defaults to
    // TRUE, so a warning raised inside a running test is promoted to an error - the same reason
    // FDispatcherUnknownParamsDiscoveryGuidanceTest sets this flag (Tests/Infra/TestDispatcher.cpp).
    bSuppressLogWarnings = true;

    // The optional half is where the damage lived: a client whose serializer emits nulls for unset
    // optionals turned "I did not set this" into "I explicitly set this to the destructive value"
    // on every optional parameter in the registry. Absent is spelled by OMITTING the key.
    TSharedPtr<FJsonObject> Payload = ParamTypeGateTests::MakeGatePayload();
    Payload->SetField(TEXT("flag"), MakeShared<FJsonValueNull>());

    ParamTypeGateTests::TestRefused(*this, TEXT("optional-null"), TEXT("_test.param_type_gate"),
                                    Payload, TEXT("flag"), GParamTypeGateBodyCallCount);

    // An `any` slot is not an escape hatch for null: it cannot distinguish a caller who means null
    // from the serializer above, so neither can the gate.
    TSharedPtr<FJsonObject> AnyPayload = ParamTypeGateTests::MakeGatePayload();
    AnyPayload->SetField(TEXT("payload"), MakeShared<FJsonValueNull>());

    ParamTypeGateTests::TestRefused(*this, TEXT("any-null"), TEXT("_test.param_type_gate"),
                                    AnyPayload, TEXT("payload"), GParamTypeGateBodyCallCount);

    return true;
}

// ============================================================================
// Wrong-shaped values are refused rather than silently coerced
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParamTypeGateWrongShapeTest,
    "PinWright.infra.dispatcher.ParamTypeGate.RefusesWrongShapedValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FParamTypeGateWrongShapeTest::RunTest(const FString& Parameters)
{
    // The gate logs its refusal through LogRpcDispatcher at Warning, exactly as the unknown-name
    // pass above it does. UAutomationControllerSettings::bElevateLogWarningsToErrors defaults to
    // TRUE, so a warning raised inside a running test is promoted to an error - the same reason
    // FDispatcherUnknownParamsDiscoveryGuidanceTest sets this flag (Tests/Infra/TestDispatcher.cpp).
    bSuppressLogWarnings = true;

    // Every row here is one line of the ticket's coercion matrix, each of which reached a handler
    // as ""/0/false with only an editor-only LogJson line the caller never sees.

    // number <- a string that is not a number. This is C2/C5/C12: "rear-left", "2s", "120s" all
    // arrive as 0, and a range guard written `Index < 0 || >= Num()` then PASSES on 0.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("numericValue"), TEXT("2s"));
        ParamTypeGateTests::TestRefused(*this, TEXT("number-from-unit-string"),
                                        TEXT("_test.param_type_gate"), Payload, TEXT("numericValue"),
                                        GParamTypeGateBodyCallCount);
    }

    // number <- array.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetArrayField(TEXT("numericValue"), TArray<TSharedPtr<FJsonValue>>());
        ParamTypeGateTests::TestRefused(*this, TEXT("number-from-array"),
                                        TEXT("_test.param_type_gate"), Payload, TEXT("numericValue"),
                                        GParamTypeGateBodyCallCount);
    }

    // string <- array. This is C8: `modifiers: ["ctrl","shift"]` became "" and the plain keystroke
    // went into the live editor with no modifiers held.
    {
        TSharedPtr<FJsonObject> Payload = ParamTypeGateTests::MakeGatePayload();
        Payload->SetArrayField(TEXT("label"), TArray<TSharedPtr<FJsonValue>>());
        ParamTypeGateTests::TestRefused(*this, TEXT("string-from-array"),
                                        TEXT("_test.param_type_gate"), Payload, TEXT("label"),
                                        GParamTypeGateBodyCallCount);
    }

    // array <- a bare string. This is C3/C9/C10/C11/C14: GetArray/TryGetArrayField return nullptr,
    // which is indistinguishable from absent, and the absent branch is permissive by design -
    // no filter, default set, invented defaults.
    {
        TSharedPtr<FJsonObject> Payload = ParamTypeGateTests::MakeGatePayload();
        Payload->SetStringField(TEXT("names"), TEXT("Red"));
        ParamTypeGateTests::TestRefused(*this, TEXT("array-from-string"),
                                        TEXT("_test.param_type_gate"), Payload, TEXT("names"),
                                        GParamTypeGateBodyCallCount);
    }

    // object <- a bare string.
    {
        TSharedPtr<FJsonObject> Payload = ParamTypeGateTests::MakeGatePayload();
        Payload->SetStringField(TEXT("options"), TEXT("fast"));
        ParamTypeGateTests::TestRefused(*this, TEXT("object-from-string"),
                                        TEXT("_test.param_type_gate"), Payload, TEXT("options"),
                                        GParamTypeGateBodyCallCount);
    }

    // boolean <- a string FCString::ToBool silently reads as false. "y", "enabled", "always" and
    // "default" are all plausible caller spellings and all landed as false.
    {
        TSharedPtr<FJsonObject> Payload = ParamTypeGateTests::MakeGatePayload();
        Payload->SetStringField(TEXT("flag"), TEXT("enabled"));
        ParamTypeGateTests::TestRefused(*this, TEXT("boolean-from-word"),
                                        TEXT("_test.param_type_gate"), Payload, TEXT("flag"),
                                        GParamTypeGateBodyCallCount);
    }

    // A union accepts its members and nothing else: `array|string` still refuses an object.
    {
        TSharedPtr<FJsonObject> Payload = ParamTypeGateTests::MakeGatePayload();
        Payload->SetObjectField(TEXT("either"), MakeShared<FJsonObject>());
        ParamTypeGateTests::TestRefused(*this, TEXT("union-outside-members"),
                                        TEXT("_test.param_type_gate"), Payload, TEXT("either"),
                                        GParamTypeGateBodyCallCount);
    }

    return true;
}

// ============================================================================
// Integer declarations refuse fractional and out-of-range JSON numbers
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParamTypeGateIntegerPolicyTest,
    "PinWright.infra.dispatcher.ParamTypeGate.RefusesNonIntegralIntegerValues",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FParamTypeGateIntegerPolicyTest::RunTest(const FString& Parameters)
{
    bSuppressLogWarnings = true;

    TSharedPtr<FJsonObject> FractionalPayload = ParamTypeGateTests::MakeGatePayload();
    FractionalPayload->SetNumberField(TEXT("wholeCount"), 1.5);
    ParamTypeGateTests::TestRefused(*this, TEXT("fractional-integer"),
        TEXT("_test.param_type_gate"), FractionalPayload, TEXT("wholeCount"),
        GParamTypeGateBodyCallCount);

    TSharedPtr<FJsonObject> OverflowPayload = ParamTypeGateTests::MakeGatePayload();
    OverflowPayload->SetNumberField(TEXT("wholeCount"), 2147483648.0);
    ParamTypeGateTests::TestRefused(*this, TEXT("overflowing-integer"),
        TEXT("_test.param_type_gate"), OverflowPayload, TEXT("wholeCount"),
        GParamTypeGateBodyCallCount);

    return true;
}

// ============================================================================
// The lossless coercions stay accepted
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParamTypeGateLosslessTest,
    "PinWright.infra.dispatcher.ParamTypeGate.AcceptsLosslessCoercions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FParamTypeGateLosslessTest::RunTest(const FString& Parameters)
{
    // The gate logs its refusal through LogRpcDispatcher at Warning, exactly as the unknown-name
    // pass above it does. UAutomationControllerSettings::bElevateLogWarningsToErrors defaults to
    // TRUE, so a warning raised inside a running test is promoted to an error - the same reason
    // FDispatcherUnknownParamsDiscoveryGuidanceTest sets this flag (Tests/Infra/TestDispatcher.cpp).
    bSuppressLogWarnings = true;

    // The gate is directional on purpose. A blanket strict rule would refuse every working caller
    // who sends "limit": "100" or "force": "true" - routine LLM-client output, and lossless: the
    // accessor recovers the intended value exactly. This test is what stops a later tightening
    // from breaking them.
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("numericValue"), TEXT("42"));          // number  <- numeric string
    Payload->SetStringField(TEXT("flag"), TEXT("true"));         // boolean <- "true"
    Payload->SetNumberField(TEXT("label"), 7.0);                 // string  <- number
    Payload->SetBoolField(TEXT("payload"), true);                // any     <- anything
    Payload->SetStringField(TEXT("either"), TEXT("one"));        // union   <- its string member

    GParamTypeGateBodyCallCount = 0;
    const ParamTypeGateTests::FOutcome Out = ParamTypeGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("_test.param_type_gate"), TEXT("type-gate-lossless"), Payload);

    TestTrue(TEXT("dispatcher responded"), Out.bResponded);
    TestTrue(*FString::Printf(TEXT("lossless payload accepted (code: %s, message: %s)"),
                              *Out.ErrorCode, *Out.Message), Out.bSuccess);
    TestEqual(TEXT("handler body ran"), GParamTypeGateBodyCallCount, 1);

    // A signed decimal is a number too. The accepted set is exactly FString::IsNumeric, because
    // that is the predicate TJsonValueString::TryGetNumber gates on - so "1e5" is deliberately NOT
    // accepted (IsNumeric rejects the exponent, and the accessor would hand the handler 0).
    TSharedPtr<FJsonObject> NumericPayload = MakeShared<FJsonObject>();
    NumericPayload->SetStringField(TEXT("numericValue"), TEXT("-1500.25"));

    GParamTypeGateBodyCallCount = 0;
    const ParamTypeGateTests::FOutcome NumericOut = ParamTypeGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("_test.param_type_gate"), TEXT("type-gate-lossless-signed"),
        NumericPayload);

    TestTrue(*FString::Printf(TEXT("signed decimal string accepted (code: %s)"),
                              *NumericOut.ErrorCode), NumericOut.bSuccess);
    TestEqual(TEXT("handler body ran for the signed decimal"), GParamTypeGateBodyCallCount, 1);

    // An ARRAY where `object` is declared is accepted, and that is a measured carve-out rather
    // than JSON semantics: 205 vector-shaped slots across 36 handler files declare `object` while
    // ExtractVectorField -> ReadVectorFieldImpl (Utils/JsonUtils.cpp:16-47) reads {x,y,z} OR
    // [x,y,z] at 111 call sites. Pinned here so tightening it later is a deliberate act that
    // starts by correcting those declarations. See Handlers/ParamTypeCheck.h.
    TSharedPtr<FJsonObject> ArrayForObjectPayload = ParamTypeGateTests::MakeGatePayload();
    ArrayForObjectPayload->SetArrayField(TEXT("options"), TArray<TSharedPtr<FJsonValue>>());

    GParamTypeGateBodyCallCount = 0;
    const ParamTypeGateTests::FOutcome ArrayForObjectOut = ParamTypeGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("_test.param_type_gate"), TEXT("type-gate-array-for-object"),
        ArrayForObjectPayload);

    TestTrue(*FString::Printf(TEXT("array accepted where object is declared (code: %s)"),
                              *ArrayForObjectOut.ErrorCode), ArrayForObjectOut.bSuccess);
    TestEqual(TEXT("handler body ran for the array-shaped object slot"),
              GParamTypeGateBodyCallCount, 1);

    return true;
}

// ============================================================================
// A typed alias is checked against ITS OWN declared type
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParamTypeGateTypedAliasTest,
    "PinWright.infra.dispatcher.ParamTypeGate.TypedAliasUsesItsOwnDeclaredType",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FParamTypeGateTypedAliasTest::RunTest(const FString& Parameters)
{
    // The gate logs its refusal through LogRpcDispatcher at Warning, exactly as the unknown-name
    // pass above it does. UAutomationControllerSettings::bElevateLogWarningsToErrors defaults to
    // TRUE, so a warning raised inside a running test is promoted to an error - the same reason
    // FDispatcherUnknownParamsDiscoveryGuidanceTest sets this flag (Tests/Infra/TestDispatcher.cpp).
    bSuppressLogWarnings = true;

    // `pathCandidates` is declared `array` while its canonical `path` is `string`. Resolving the
    // gate per SPEC instead of per matched wire name would refuse the alias's own correct shape
    // and accept its wrong one - both directions are asserted here.
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> ArrayPayload = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Candidates;
    Candidates.Add(MakeShared<FJsonValueString>(TEXT("/Game/A")));
    ArrayPayload->SetArrayField(TEXT("pathCandidates"), Candidates);

    GParamTypeGateAliasBodyCallCount = 0;
    const ParamTypeGateTests::FOutcome ArrayOut = ParamTypeGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("_test.param_type_gate_alias"), TEXT("type-gate-alias-array"),
        ArrayPayload);

    TestTrue(*FString::Printf(TEXT("array-shaped typed alias accepted (code: %s, message: %s)"),
                              *ArrayOut.ErrorCode, *ArrayOut.Message), ArrayOut.bSuccess);
    TestEqual(TEXT("alias handler body ran"), GParamTypeGateAliasBodyCallCount, 1);

    TSharedPtr<FJsonObject> StringPayload = MakeShared<FJsonObject>();
    StringPayload->SetStringField(TEXT("pathCandidates"), TEXT("/Game/A"));
    ParamTypeGateTests::TestRefused(*this, TEXT("typed-alias-wrong-shape"),
                                    TEXT("_test.param_type_gate_alias"), StringPayload,
                                    TEXT("pathCandidates"), GParamTypeGateAliasBodyCallCount);

    // The untyped alias inherits the canonical's `string`, and is accepted as one.
    TSharedPtr<FJsonObject> UntypedAliasPayload = MakeShared<FJsonObject>();
    UntypedAliasPayload->SetStringField(TEXT("assetPath"), TEXT("/Game/A"));

    GParamTypeGateAliasBodyCallCount = 0;
    const ParamTypeGateTests::FOutcome UntypedOut = ParamTypeGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("_test.param_type_gate_alias"), TEXT("type-gate-alias-untyped"),
        UntypedAliasPayload);

    TestTrue(*FString::Printf(TEXT("untyped alias inherits the canonical type (code: %s)"),
                              *UntypedOut.ErrorCode), UntypedOut.bSuccess);

    return true;
}

// ============================================================================
// Ordering: a missing required slot is still the answer the caller gets first
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParamTypeGateOrderingTest,
    "PinWright.infra.dispatcher.ParamTypeGate.MissingRequiredParamWinsOverTypeFault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FParamTypeGateOrderingTest::RunTest(const FString& Parameters)
{
    // The gate logs its refusal through LogRpcDispatcher at Warning, exactly as the unknown-name
    // pass above it does. UAutomationControllerSettings::bElevateLogWarningsToErrors defaults to
    // TRUE, so a warning raised inside a running test is promoted to an error - the same reason
    // FDispatcherUnknownParamsDiscoveryGuidanceTest sets this flag (Tests/Infra/TestDispatcher.cpp).
    bSuppressLogWarnings = true;

    // The type gate runs last on purpose. A payload that both omits a required slot and misshapes
    // an optional one must still answer MISSING_REQUIRED_PARAM: that is the fault the caller has
    // to fix before any other can matter, and it is what the registry-wide
    // infra.contract.RequiredParamGate.EveryVerb walk asserts on ~940 verbs.
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetArrayField(TEXT("label"), TArray<TSharedPtr<FJsonValue>>());   // wrong shape
    // ... and no "count", which is required.

    GParamTypeGateBodyCallCount = 0;
    const ParamTypeGateTests::FOutcome Out = ParamTypeGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("_test.param_type_gate"), TEXT("type-gate-ordering"), Payload);

    TestFalse(TEXT("response is an error"), Out.bSuccess);
    TestEqual(*FString::Printf(TEXT("missing-required wins (message: %s)"), *Out.Message),
              Out.ErrorCode, FString(TEXT("MISSING_REQUIRED_PARAM")));
    TestEqual(TEXT("handler body did NOT run"), GParamTypeGateBodyCallCount, 0);

    // An UNKNOWN name likewise still wins: that caller needs the list of valid parameters, not a
    // shape complaint about a key they spelled correctly.
    TSharedPtr<FJsonObject> UnknownPayload = ParamTypeGateTests::MakeGatePayload();
    UnknownPayload->SetStringField(TEXT("nosuchparam"), TEXT("x"));
    UnknownPayload->SetStringField(TEXT("names"), TEXT("wrong shape"));

    const ParamTypeGateTests::FOutcome UnknownOut = ParamTypeGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("_test.param_type_gate"), TEXT("type-gate-ordering-unknown"),
        UnknownPayload);

    TestEqual(*FString::Printf(TEXT("unknown-param wins (message: %s)"), *UnknownOut.Message),
              UnknownOut.ErrorCode, FString(TEXT("UNKNOWN_PARAMS")));

    return true;
}

// ============================================================================
// The destructive instance: widget.replace_class / preserveProperties (ticket C1)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParamTypeGateWidgetReplaceClassTest,
    "PinWright.infra.dispatcher.ParamTypeGate.WidgetReplaceClassRefusesNullPreserveProperties",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FParamTypeGateWidgetReplaceClassTest::RunTest(const FString& Parameters)
{
    // The gate logs its refusal through LogRpcDispatcher at Warning, exactly as the unknown-name
    // pass above it does. UAutomationControllerSettings::bElevateLogWarningsToErrors defaults to
    // TRUE, so a warning raised inside a running test is promoted to an error - the same reason
    // FDispatcherUnknownParamsDiscoveryGuidanceTest sets this flag (Tests/Infra/TestDispatcher.cpp).
    bSuppressLogWarnings = true;

    // `widget.replace_class` declares preserveProperties as `bool` defaulting to true and reads it
    // with Ctx.GetBool(..., true). Sent as null it cleared HasField, coerced to FALSE, and
    // WidgetAuthoringUtils skipped the only call to CopyMatchingProperties - after the old widget
    // had already been renamed to a `_REPLACED_<guid>` scratch name and discarded. The response was
    // {"success": true, ...} with the flag echoed nowhere, so the loss was undetectable.
    //
    // The probe path does not exist, so the ONLY way this call can answer PARAM_TYPE_MISMATCH is if
    // the dispatcher refused before the body ran: a body that runs answers NOT_FOUND from its own
    // LoadWidgetBlueprint failure. Nothing is loaded, renamed or saved either way.
    FRpcDispatcher Dispatcher;
    DispatcherTestHelpers::FSinkPtr Sink;
    DispatcherTestHelpers::MakeDispatcher(Sink, Dispatcher);

    if (!Dispatcher.GetAutoRegisteredHandlers().Contains(TEXT("widget.replace_class")))
    {
        AddError(TEXT("widget.replace_class is not registered - the fixture this test is built on "
                      "has moved, and the assertion below would pass vacuously."));
        return false;
    }

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("widgetPath"),
        TEXT("/Game/PinWrightParamTypeGateProbe/W_DoesNotExist"));
    Payload->SetStringField(TEXT("targetName"), TEXT("RootWidget"));
    Payload->SetStringField(TEXT("newType"), TEXT("Overlay"));
    Payload->SetField(TEXT("preserveProperties"), MakeShared<FJsonValueNull>());

    const ParamTypeGateTests::FOutcome Out = ParamTypeGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("widget.replace_class"), TEXT("type-gate-widget-replace"), Payload);

    TestTrue(TEXT("dispatcher responded"), Out.bResponded);
    TestFalse(TEXT("response is an error, not a success"), Out.bSuccess);
    TestEqual(*FString::Printf(
                  TEXT("refused by the dispatcher before the body ran (message: %s)"), *Out.Message),
              Out.ErrorCode, ParamTypeGateTests::GExpectedCode);
    TestTrue(*FString::Printf(TEXT("message names 'preserveProperties' (message: %s)"), *Out.Message),
             Out.Message.Contains(TEXT("'preserveProperties'")));

    // The same slot sent as the string "y" is the other half of C1: FCString::ToBool reads it as
    // false, so it produced the identical total property loss.
    TSharedPtr<FJsonObject> WordPayload = MakeShared<FJsonObject>();
    WordPayload->SetStringField(TEXT("widgetPath"),
        TEXT("/Game/PinWrightParamTypeGateProbe/W_DoesNotExist"));
    WordPayload->SetStringField(TEXT("targetName"), TEXT("RootWidget"));
    WordPayload->SetStringField(TEXT("newType"), TEXT("Overlay"));
    WordPayload->SetStringField(TEXT("preserveProperties"), TEXT("y"));

    const ParamTypeGateTests::FOutcome WordOut = ParamTypeGateTests::DispatchAndSettle(
        Dispatcher, Sink, TEXT("widget.replace_class"), TEXT("type-gate-widget-replace-word"),
        WordPayload);

    TestEqual(*FString::Printf(TEXT("\"y\" is refused too (message: %s)"), *WordOut.Message),
              WordOut.ErrorCode, ParamTypeGateTests::GExpectedCode);

    return true;
}
