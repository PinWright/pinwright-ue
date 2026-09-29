// Copyright (c) 2026 Alexander Penkin. MIT License.

// blueprint.graph.connect_pins_batch: many links in one call and one transaction. Every test
// reads the graph back rather than trusting the response, and the failure-direction tests assert
// that a bad entry is reported per item while the good entries around it still land.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tests/TestUtils.h"
#include "Tests/AutomationSuiteMaintenance.h"
#include "Tests/Bpir/BpirGraphTestHelpers.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

// Named namespace: a Unity blob merges this TU with other blueprint test files.
namespace ConnectPinsBatchTestUtils
{
    const TCHAR* const Method = TEXT("blueprint.graph.connect_pins_batch");

    // In-memory actor Blueprint under the suite scratch root, with three PrintString nodes in
    // its EventGraph. Never saved; the caller releases it with CleanupTestAsset.
    struct FFixture
    {
        FString PackagePath;
        UBlueprint* Blueprint = nullptr;
        UEdGraph* Graph = nullptr;
        UK2Node_CallFunction* Print[3] = {nullptr, nullptr, nullptr};

        bool Create()
        {
            PackagePath = FString::Printf(TEXT("%s/ConnectPinsBatch_%s"),
                PinWrightSuiteMaintenance::ScratchRootPackagePath(),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits));
            UPackage* Package = CreatePackage(*PackagePath);
            if (!Package)
            {
                return false;
            }
            Blueprint = FKismetEditorUtilities::CreateBlueprint(
                AActor::StaticClass(), Package,
                FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
                BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
            Graph = Blueprint && Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
            if (!Graph)
            {
                return false;
            }
            for (UK2Node_CallFunction*& Node : Print)
            {
                Node = BpirGraphTestHelpers::AddPrintStringNode(Graph);
                if (!Node)
                {
                    return false;
                }
            }
            return true;
        }

        void Release() const
        {
            CleanupTestAsset(PackagePath);
        }

        static FString Id(const UEdGraphNode* Node)
        {
            return Node->NodeGuid.ToString();
        }

        static bool Linked(UEdGraphNode* From, const FName FromPin, UEdGraphNode* To, const FName ToPin)
        {
            UEdGraphPin* A = From->FindPin(FromPin);
            UEdGraphPin* B = To->FindPin(ToPin);
            return A && B && A->LinkedTo.Contains(B);
        }
    };

    TSharedPtr<FJsonValue> Link(const FString& FromNodeId, const FString& FromPin,
        const FString& ToNodeId, const FString& ToPin)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("fromNodeId"), FromNodeId);
        Obj->SetStringField(TEXT("fromPinName"), FromPin);
        Obj->SetStringField(TEXT("toNodeId"), ToNodeId);
        Obj->SetStringField(TEXT("toPinName"), ToPin);
        return MakeShared<FJsonValueObject>(Obj);
    }

    TSharedPtr<FJsonObject> Payload(const FFixture& Fixture, const TArray<TSharedPtr<FJsonValue>>& Links)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("assetPath"), Fixture.PackagePath);
        Out->SetArrayField(TEXT("links"), Links);
        return Out;
    }

    TSharedPtr<FJsonObject> ResultAt(const FTestResponseCapture& Capture, int32 Index)
    {
        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        if (!Capture.Result.IsValid() || !Capture.Result->TryGetArrayField(TEXT("results"), Results)
            || !Results->IsValidIndex(Index))
        {
            return nullptr;
        }
        return (*Results)[Index]->AsObject();
    }

    FString StringAt(const FTestResponseCapture& Capture, int32 Index, const TCHAR* Field)
    {
        const TSharedPtr<FJsonObject> Item = ResultAt(Capture, Index);
        FString Out;
        if (Item.IsValid())
        {
            Item->TryGetStringField(Field, Out);
        }
        return Out;
    }

    int32 IntField(const FTestResponseCapture& Capture, const TCHAR* Field)
    {
        int32 Out = -1;
        if (Capture.Result.IsValid())
        {
            Capture.Result->TryGetNumberField(Field, Out);
        }
        return Out;
    }
}

namespace CPB = ConnectPinsBatchTestUtils;

// ============================================================================
// N links in one call all land, and nothing compiles unless asked.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectPinsBatchWiresEveryLinkTest,
    "PinWright.blueprint.graph.connect_pins_batch.WiresEveryLink",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConnectPinsBatchWiresEveryLinkTest::RunTest(const FString& Parameters)
{
    CPB::FFixture F;
    if (!TestTrue(TEXT("Fixture Blueprint with three PrintString nodes"), F.Create()))
    {
        F.Release();
        return false;
    }

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler registered"), InvokeHandlerWithCapture(CPB::Method, CPB::Payload(F, {
        CPB::Link(CPB::FFixture::Id(F.Print[0]), TEXT("then"), CPB::FFixture::Id(F.Print[1]), TEXT("execute")),
        CPB::Link(CPB::FFixture::Id(F.Print[1]), TEXT("then"), CPB::FFixture::Id(F.Print[2]), TEXT("execute")),
    }), Capture));

    TestTrue(TEXT("Call succeeded"), Capture.bSuccess);
    TestEqual(TEXT("totalLinks"), CPB::IntField(Capture, TEXT("totalLinks")), 2);
    TestEqual(TEXT("successCount"), CPB::IntField(Capture, TEXT("successCount")), 2);
    TestEqual(TEXT("failureCount"), CPB::IntField(Capture, TEXT("failureCount")), 0);
    TestEqual(TEXT("Entry 0 is a direct link"), CPB::StringAt(Capture, 0, TEXT("connection")), FString(TEXT("direct")));
    TestTrue(TEXT("Graph: Print0.then -> Print1.execute"), CPB::FFixture::Linked(F.Print[0], UEdGraphSchema_K2::PN_Then, F.Print[1], UEdGraphSchema_K2::PN_Execute));
    TestTrue(TEXT("Graph: Print1.then -> Print2.execute"), CPB::FFixture::Linked(F.Print[1], UEdGraphSchema_K2::PN_Then, F.Print[2], UEdGraphSchema_K2::PN_Execute));

    bool bCompiled = true;
    TestTrue(TEXT("compiled field present"), Capture.Result.IsValid() && Capture.Result->TryGetBoolField(TEXT("compiled"), bCompiled));
    TestFalse(TEXT("Nothing compiled without compile=true"), bCompiled);
    TestEqual(TEXT("Blueprint left dirty, not recompiled"), static_cast<int32>(F.Blueprint->Status), static_cast<int32>(BS_Dirty));

    F.Release();
    return true;
}

// ============================================================================
// FAILURE DIRECTION: bad entries are reported per item and do not roll back good ones.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectPinsBatchPartialFailureKeepsSuccessesTest,
    "PinWright.blueprint.graph.connect_pins_batch.PartialFailureKeepsSuccesses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConnectPinsBatchPartialFailureKeepsSuccessesTest::RunTest(const FString& Parameters)
{
    CPB::FFixture F;
    if (!TestTrue(TEXT("Fixture Blueprint with three PrintString nodes"), F.Create()))
    {
        F.Release();
        return false;
    }

    const FString P0 = CPB::FFixture::Id(F.Print[0]);
    const FString P1 = CPB::FFixture::Id(F.Print[1]);
    const FString P2 = CPB::FFixture::Id(F.Print[2]);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(CPB::Method, CPB::Payload(F, {
        CPB::Link(P0, TEXT("then"), P1, TEXT("execute")),
        CPB::Link(TEXT("NoSuchNode"), TEXT("then"), P2, TEXT("execute")),
        CPB::Link(P1, TEXT("NoSuchPin"), P2, TEXT("execute")),
        CPB::Link(P1, TEXT("then"), P2, TEXT("InString")),
        CPB::Link(P1, TEXT("then"), P2, TEXT("execute")),
    }), Capture);

    TestTrue(TEXT("Call succeeded with per-item results"), Capture.bSuccess);
    TestEqual(TEXT("successCount"), CPB::IntField(Capture, TEXT("successCount")), 2);
    TestEqual(TEXT("failureCount"), CPB::IntField(Capture, TEXT("failureCount")), 3);
    TestEqual(TEXT("Unknown node"), CPB::StringAt(Capture, 1, TEXT("error")), FString(TEXT("NODE_NOT_FOUND")));
    TestEqual(TEXT("Unknown pin"), CPB::StringAt(Capture, 2, TEXT("error")), FString(TEXT("PIN_NOT_FOUND")));
    const TSharedPtr<FJsonObject> PinMiss = CPB::ResultAt(Capture, 2);
    TestTrue(TEXT("Unknown pin carries the live source-pin list"), PinMiss.IsValid() && PinMiss->HasField(TEXT("sourcePinLookup")));
    TestEqual(TEXT("Exec into string rejected by the schema"), CPB::StringAt(Capture, 3, TEXT("error")), FString(TEXT("CONNECTION_FAILED")));

    TestTrue(TEXT("Graph: first good link kept"), CPB::FFixture::Linked(F.Print[0], UEdGraphSchema_K2::PN_Then, F.Print[1], UEdGraphSchema_K2::PN_Execute));
    TestTrue(TEXT("Graph: last good link kept"), CPB::FFixture::Linked(F.Print[1], UEdGraphSchema_K2::PN_Then, F.Print[2], UEdGraphSchema_K2::PN_Execute));
    UEdGraphPin* InString = F.Print[2]->FindPin(TEXT("InString"));
    TestTrue(TEXT("Graph: rejected link absent"), InString && InString->LinkedTo.Num() == 0);

    F.Release();
    return true;
}

// ============================================================================
// FAILURE DIRECTION: a later entry that replaces an earlier link on a single-link pin turns the
// earlier entry into a failure, because success is measured on the final graph.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectPinsBatchLaterEntrySupersedesTest,
    "PinWright.blueprint.graph.connect_pins_batch.LaterEntrySupersedesEarlier",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConnectPinsBatchLaterEntrySupersedesTest::RunTest(const FString& Parameters)
{
    CPB::FFixture F;
    if (!TestTrue(TEXT("Fixture Blueprint with three PrintString nodes"), F.Create()))
    {
        F.Release();
        return false;
    }

    // An exec output keeps one link, so the second entry breaks the first.
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(CPB::Method, CPB::Payload(F, {
        CPB::Link(CPB::FFixture::Id(F.Print[0]), TEXT("then"), CPB::FFixture::Id(F.Print[1]), TEXT("execute")),
        CPB::Link(CPB::FFixture::Id(F.Print[0]), TEXT("then"), CPB::FFixture::Id(F.Print[2]), TEXT("execute")),
    }), Capture);

    TestTrue(TEXT("Call succeeded with per-item results"), Capture.bSuccess);
    TestEqual(TEXT("successCount"), CPB::IntField(Capture, TEXT("successCount")), 1);
    TestEqual(TEXT("Replaced entry reports LINK_SUPERSEDED"), CPB::StringAt(Capture, 0, TEXT("error")), FString(TEXT("LINK_SUPERSEDED")));
    const TSharedPtr<FJsonObject> Winner = CPB::ResultAt(Capture, 1);
    bool bBroke = false;
    TestTrue(TEXT("Winning entry says it broke an existing link"),
        Winner.IsValid() && Winner->TryGetBoolField(TEXT("brokeExistingLinks"), bBroke) && bBroke);
    TestFalse(TEXT("Graph: superseded link absent"), CPB::FFixture::Linked(F.Print[0], UEdGraphSchema_K2::PN_Then, F.Print[1], UEdGraphSchema_K2::PN_Execute));
    TestTrue(TEXT("Graph: winning link present"), CPB::FFixture::Linked(F.Print[0], UEdGraphSchema_K2::PN_Then, F.Print[2], UEdGraphSchema_K2::PN_Execute));

    F.Release();
    return true;
}

// ============================================================================
// A retried batch converges: existing links are reported, not re-made.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectPinsBatchRetryConvergesTest,
    "PinWright.blueprint.graph.connect_pins_batch.RetryReportsAlreadyConnected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConnectPinsBatchRetryConvergesTest::RunTest(const FString& Parameters)
{
    CPB::FFixture F;
    if (!TestTrue(TEXT("Fixture Blueprint with three PrintString nodes"), F.Create()))
    {
        F.Release();
        return false;
    }

    const TSharedPtr<FJsonObject> Request = CPB::Payload(F, {
        CPB::Link(CPB::FFixture::Id(F.Print[0]), TEXT("then"), CPB::FFixture::Id(F.Print[1]), TEXT("execute")),
    });
    FTestResponseCapture First;
    InvokeHandlerWithCapture(CPB::Method, Request, First);
    FTestResponseCapture Second;
    InvokeHandlerWithCapture(CPB::Method, Request, Second);

    TestEqual(TEXT("First call makes the link"), CPB::StringAt(First, 0, TEXT("connection")), FString(TEXT("direct")));
    TestEqual(TEXT("Retry succeeds"), CPB::IntField(Second, TEXT("successCount")), 1);
    TestEqual(TEXT("Retry reports the existing link"), CPB::StringAt(Second, 0, TEXT("connection")), FString(TEXT("alreadyConnected")));
    TestEqual(TEXT("Graph: exactly one link on the exec output"), F.Print[0]->FindPin(UEdGraphSchema_K2::PN_Then)->LinkedTo.Num(), 1);

    F.Release();
    return true;
}

// ============================================================================
// compile=true compiles once after the links; the same batch without it leaves the BP dirty
// (the differential half lives in WiresEveryLink).
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectPinsBatchCompileFlagTest,
    "PinWright.blueprint.graph.connect_pins_batch.CompileFlagCompiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConnectPinsBatchCompileFlagTest::RunTest(const FString& Parameters)
{
    CPB::FFixture F;
    if (!TestTrue(TEXT("Fixture Blueprint with three PrintString nodes"), F.Create()))
    {
        F.Release();
        return false;
    }

    TSharedPtr<FJsonObject> Request = CPB::Payload(F, {
        CPB::Link(CPB::FFixture::Id(F.Print[0]), TEXT("then"), CPB::FFixture::Id(F.Print[1]), TEXT("execute")),
    });
    Request->SetBoolField(TEXT("compile"), true);

    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(CPB::Method, Request, Capture);

    TestTrue(TEXT("Call succeeded"), Capture.bSuccess);
    bool bCompiled = false;
    TestTrue(TEXT("compiled reported true"),
        Capture.Result.IsValid() && Capture.Result->TryGetBoolField(TEXT("compiled"), bCompiled) && bCompiled);
    TestTrue(TEXT("compileErrors array present"), Capture.Result.IsValid() && Capture.Result->HasField(TEXT("compileErrors")));
    TestTrue(TEXT("Blueprint is up to date after the compile"),
        F.Blueprint->Status == BS_UpToDate || F.Blueprint->Status == BS_UpToDateWithWarnings);
    TestTrue(TEXT("Graph: link present"), CPB::FFixture::Linked(F.Print[0], UEdGraphSchema_K2::PN_Then, F.Print[1], UEdGraphSchema_K2::PN_Execute));

    F.Release();
    return true;
}

// ============================================================================
// FAILURE DIRECTION: an empty batch is an error, not a zero-item success.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectPinsBatchEmptyLinksRejectedTest,
    "PinWright.blueprint.graph.connect_pins_batch.EmptyLinksRejected",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConnectPinsBatchEmptyLinksRejectedTest::RunTest(const FString& Parameters)
{
    CPB::FFixture F;
    if (!TestTrue(TEXT("Fixture Blueprint with three PrintString nodes"), F.Create()))
    {
        F.Release();
        return false;
    }

    const int32 StatusBefore = static_cast<int32>(F.Blueprint->Status);
    FTestResponseCapture Capture;
    InvokeHandlerWithCapture(CPB::Method, CPB::Payload(F, {}), Capture);

    TestFalse(TEXT("Empty links is an error"), Capture.bSuccess);
    TestEqual(TEXT("Error code"), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestEqual(TEXT("Blueprint status untouched"), static_cast<int32>(F.Blueprint->Status), StatusBefore);

    F.Release();
    return true;
}

// ============================================================================
// FAILURE DIRECTION (single-link verb): PIN_NOT_FOUND carries the live pin list in its error
// data, in the same shape connect_pins_batch puts on its failed item. It used to build the
// lookup and send the error without it.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FConnectPinsPinNotFoundCarriesLookupTest,
    "PinWright.blueprint.graph.connect_pins.PinNotFoundCarriesLivePins",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FConnectPinsPinNotFoundCarriesLookupTest::RunTest(const FString& Parameters)
{
    CPB::FFixture F;
    if (!TestTrue(TEXT("Fixture Blueprint with three PrintString nodes"), F.Create()))
    {
        F.Release();
        return false;
    }

    TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("assetPath"), F.PackagePath);
    Request->SetStringField(TEXT("fromNodeId"), CPB::FFixture::Id(F.Print[0]));
    Request->SetStringField(TEXT("fromPinName"), TEXT("NoSuchPin"));
    Request->SetStringField(TEXT("toNodeId"), CPB::FFixture::Id(F.Print[1]));
    Request->SetStringField(TEXT("toPinName"), TEXT("execute"));

    FTestResponseCapture Capture;
    TestTrue(TEXT("Handler registered"),
        InvokeHandlerWithCapture(TEXT("blueprint.graph.connect_pins"), Request, Capture));
    TestFalse(TEXT("Unknown pin is an error"), Capture.bSuccess);
    TestEqual(TEXT("Error code"), Capture.ErrorCode, FString(TEXT("PIN_NOT_FOUND")));
    TestTrue(TEXT("Error carries data"), Capture.Result.IsValid());
    if (Capture.Result.IsValid())
    {
        const TSharedPtr<FJsonObject>* Lookup = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Available = nullptr;
        bool bListsThen = false;
        if (TestTrue(TEXT("Missing source pin: sourcePinLookup present"),
                Capture.Result->TryGetObjectField(TEXT("sourcePinLookup"), Lookup))
            && TestTrue(TEXT("sourcePinLookup.availablePins present"),
                (*Lookup)->TryGetArrayField(TEXT("availablePins"), Available)))
        {
            for (const TSharedPtr<FJsonValue>& Pin : *Available)
            {
                bListsThen |= Pin.IsValid() && Pin->AsString() == UEdGraphSchema_K2::PN_Then.ToString();
            }
        }
        TestTrue(TEXT("The live output pins include 'then'"), bListsThen);
        TestFalse(TEXT("Resolved target pin: no targetPinLookup"), Capture.Result->HasField(TEXT("targetPinLookup")));
    }
    TestEqual(TEXT("Graph: nothing linked"), F.Print[1]->FindPin(UEdGraphSchema_K2::PN_Execute)->LinkedTo.Num(), 0);

    F.Release();
    return true;
}
