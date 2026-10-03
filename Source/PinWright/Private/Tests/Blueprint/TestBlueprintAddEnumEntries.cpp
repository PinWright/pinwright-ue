// Copyright (c) 2026 Alexander Penkin. MIT License.

// blueprint.add_enum_entries: append enumerators to an existing user-defined enum without
// renaming the ones Blueprints already reference. The fixture is a scratch enum plus an actor
// Blueprint whose Switch-on-enum node has an existing case pin wired to a PrintString; the test
// reads the enum and the graph back rather than trusting the response.

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
#include "Engine/UserDefinedEnum.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_EnumLiteral.h"
#include "K2Node_SwitchEnum.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

// Named namespace: a Unity blob merges this TU with other blueprint test files.
namespace AddEnumEntriesTestUtils
{
    const TCHAR* const Method = TEXT("blueprint.add_enum_entries");

    TArray<TSharedPtr<FJsonValue>> Strings(std::initializer_list<const TCHAR*> Items)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (const TCHAR* Item : Items)
        {
            Values.Add(MakeShared<FJsonValueString>(Item));
        }
        return Values;
    }

    struct FFixture
    {
        FString EnumPath;
        FString BlueprintPath;
        UUserDefinedEnum* Enum = nullptr;
        UBlueprint* Blueprint = nullptr;
        UK2Node_SwitchEnum* Switch = nullptr;
        UK2Node_CallFunction* Print = nullptr;
        UK2Node_EnumLiteral* Literal = nullptr;

        // Enum [Idle, Scanning] created through blueprint.create_enum; Blueprint with a
        // Switch on that enum whose "Scanning" case is wired to a PrintString, plus an enum
        // literal on it (reconstructed on enum change, but has no per-entry pins).
        bool Create(FAutomationTestBase& Test)
        {
            const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
            EnumPath = FString::Printf(TEXT("%s/E_AddEntries_%s"), PinWrightSuiteMaintenance::ScratchRootPackagePath(), *Suffix);
            BlueprintPath = FString::Printf(TEXT("%s/BP_AddEntries_%s"), PinWrightSuiteMaintenance::ScratchRootPackagePath(), *Suffix);

            TSharedPtr<FJsonObject> CreatePayload = MakeShared<FJsonObject>();
            CreatePayload->SetStringField(TEXT("path"), EnumPath);
            CreatePayload->SetArrayField(TEXT("entries"), Strings({ TEXT("Idle"), TEXT("Scanning") }));
            FTestResponseCapture CreateCapture;
            InvokeHandlerWithCapture(TEXT("blueprint.create_enum"), CreatePayload, CreateCapture);
            Enum = FindObject<UUserDefinedEnum>(nullptr, *FString::Printf(TEXT("%s.%s"),
                *EnumPath, *FPackageName::GetLongPackageAssetName(EnumPath)));
            if (!Test.TestTrue(TEXT("fixture: create_enum succeeded"), CreateCapture.bSuccess) ||
                !Test.TestNotNull(TEXT("fixture: enum exists"), Enum))
            {
                return false;
            }

            UPackage* Package = CreatePackage(*BlueprintPath);
            Blueprint = FKismetEditorUtilities::CreateBlueprint(
                AActor::StaticClass(), Package,
                FName(*FPackageName::GetLongPackageAssetName(BlueprintPath)),
                BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
            UEdGraph* Graph = Blueprint && Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
            if (!Test.TestNotNull(TEXT("fixture: Blueprint event graph"), Graph))
            {
                return false;
            }

            FGraphNodeCreator<UK2Node_SwitchEnum> Creator(*Graph);
            Switch = Creator.CreateNode(false);
            Switch->SetEnum(Enum);
            Creator.Finalize();
            Print = BpirGraphTestHelpers::AddPrintStringNode(Graph);
            FGraphNodeCreator<UK2Node_EnumLiteral> LiteralCreator(*Graph);
            Literal = LiteralCreator.CreateNode(false);
            Literal->Enum = Enum;
            LiteralCreator.Finalize();

            UEdGraphPin* ScanningCase = Switch->FindPin(TEXT("Scanning"));
            UEdGraphPin* PrintExec = Print ? Print->GetExecPin() : nullptr;
            if (!Test.TestTrue(TEXT("fixture: enum literal depends on the enum"), Literal->GetEnum() == Enum) ||
                !Test.TestTrue(TEXT("fixture: Switch has Idle and Scanning case pins"),
                    Switch->FindPin(TEXT("Idle")) != nullptr && ScanningCase != nullptr) ||
                !Test.TestNotNull(TEXT("fixture: PrintString exec pin"), PrintExec) ||
                !Test.TestTrue(TEXT("fixture: Scanning case wired to PrintString"),
                    GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(ScanningCase, PrintExec)))
            {
                return false;
            }
            return true;
        }

        void Release()
        {
            CleanupTestAsset(BlueprintPath);
            CleanupTestAsset(EnumPath);
        }
    };

    TSharedPtr<FJsonObject> MakePayload(const FString& EnumPath, std::initializer_list<const TCHAR*> DisplayNames)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("path"), EnumPath);
        Payload->SetArrayField(TEXT("displayNames"), Strings(DisplayNames));
        return Payload;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddEnumEntriesKeepsNamesAndRefreshesDependentsTest,
    "PinWright.blueprint.add_enum_entries.KeepsNamesAndRefreshesDependents",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddEnumEntriesKeepsNamesAndRefreshesDependentsTest::RunTest(const FString& Parameters)
{
    using namespace AddEnumEntriesTestUtils;
    FFixture Fixture;
    if (!Fixture.Create(*this))
    {
        Fixture.Release();
        return false;
    }

    FTestResponseCapture Capture;
    TestTrue(TEXT("handler registered"),
        InvokeHandlerWithCapture(Method, MakePayload(Fixture.EnumPath, { TEXT("Complete"), TEXT("Failed") }), Capture));
    TestTrue(*FString::Printf(TEXT("add_enum_entries succeeded (%s: %s)"), *Capture.ErrorCode, *Capture.Message),
        Capture.bSuccess);

    // Existing internal names keep their index, so every reference keyed by them still resolves.
    UUserDefinedEnum* Enum = Fixture.Enum;
    TestEqual(TEXT("enum now has 4 entries (+_MAX)"), Enum->NumEnums(), 5);
    TestEqual(TEXT("Idle kept index 0"), Enum->GetIndexByNameString(TEXT("Idle")), 0);
    TestEqual(TEXT("Scanning kept index 1"), Enum->GetIndexByNameString(TEXT("Scanning")), 1);
    TestEqual(TEXT("index 2 display name"), Enum->GetDisplayNameTextByIndex(2).ToString(), FString(TEXT("Complete")));
    TestEqual(TEXT("index 3 display name"), Enum->GetDisplayNameTextByIndex(3).ToString(), FString(TEXT("Failed")));
    const FString NewName2 = Enum->GetNameStringByIndex(2);
    const FString NewName3 = Enum->GetNameStringByIndex(3);

    // Response names what the engine minted, read off the asset.
    const TArray<TSharedPtr<FJsonValue>>* Added = nullptr;
    if (TestTrue(TEXT("response carries added[]"), Capture.Result.IsValid() && Capture.Result->TryGetArrayField(TEXT("added"), Added)) &&
        TestEqual(TEXT("two entries added"), Added->Num(), 2))
    {
        TestEqual(TEXT("added[0].name is the minted internal name"), (*Added)[0]->AsObject()->GetStringField(TEXT("name")), NewName2);
        TestEqual(TEXT("added[1].index"), static_cast<int32>((*Added)[1]->AsObject()->GetNumberField(TEXT("index"))), 3);
    }

    // The dependent Switch was reconstructed by the engine's change broadcast: it has a case pin
    // per new enumerator, and the existing Scanning case is still wired. A SetEnums-only append
    // would leave the node with two pins.
    UEdGraphPin* ScanningCase = Fixture.Switch->FindPin(TEXT("Scanning"));
    TestTrue(TEXT("Switch kept its Idle case"), Fixture.Switch->FindPin(TEXT("Idle")) != nullptr);
    TestTrue(TEXT("Switch Scanning case still wired to PrintString"),
        ScanningCase && ScanningCase->LinkedTo.Num() == 1 && ScanningCase->LinkedTo[0]->GetOwningNode() == Fixture.Print);
    TestNotNull(TEXT("Switch gained a case pin for the first new enumerator"), Fixture.Switch->FindPin(*NewName2));
    TestNotNull(TEXT("Switch gained a case pin for the second new enumerator"), Fixture.Switch->FindPin(*NewName3));

    const TSharedPtr<FJsonObject>* Dependents = nullptr;
    if (TestTrue(TEXT("response carries dependents"), Capture.Result.IsValid() && Capture.Result->TryGetObjectField(TEXT("dependents"), Dependents)))
    {
        TestTrue(TEXT("dependents.nodes counts the Switch and the enum literal"), (*Dependents)->GetNumberField(TEXT("nodes")) >= 2);
        TestTrue(TEXT("dependents.blueprints names the fixture Blueprint"),
            JsonStringArrayContains(*Dependents, TEXT("blueprints"), Fixture.Blueprint->GetPathName()));
        // The enum literal has no per-entry pins, so pin-checking it would report it here.
        TestEqual(TEXT("no dependent node is missing the new pins (enum literal not pin-checked)"),
            (*Dependents)->GetArrayField(TEXT("nodesMissingNewPins")).Num(), 0);
    }

    Fixture.Release();
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddEnumEntriesRefusalsLeaveEnumUntouchedTest,
    "PinWright.blueprint.add_enum_entries.RefusalsLeaveEnumUntouched",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddEnumEntriesRefusalsLeaveEnumUntouchedTest::RunTest(const FString& Parameters)
{
    using namespace AddEnumEntriesTestUtils;
    FFixture Fixture;
    if (!Fixture.Create(*this))
    {
        Fixture.Release();
        return false;
    }
    UUserDefinedEnum* Enum = Fixture.Enum;

    // A display name the enum already uses: refused before anything is appended.
    FTestResponseCapture Duplicate;
    InvokeHandlerWithCapture(Method, MakePayload(Fixture.EnumPath, { TEXT("Complete"), TEXT("Scanning") }), Duplicate);
    TestFalse(TEXT("duplicate display name refused"), Duplicate.bSuccess);
    TestEqual(TEXT("duplicate refusal code"), Duplicate.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
    TestEqual(TEXT("enum unchanged after duplicate refusal (Complete not appended)"), Enum->NumEnums(), 3);

    // Duplicates inside one request are refused the same way.
    FTestResponseCapture InRequest;
    InvokeHandlerWithCapture(Method, MakePayload(Fixture.EnumPath, { TEXT("Complete"), TEXT("Complete") }), InRequest);
    TestFalse(TEXT("in-request duplicate refused"), InRequest.bSuccess);
    TestEqual(TEXT("enum unchanged after in-request duplicate"), Enum->NumEnums(), 3);

    // set_enum_entries still refuses the enum's own names, and now says where appending lives.
    TSharedPtr<FJsonObject> SetPayload = MakeShared<FJsonObject>();
    SetPayload->SetStringField(TEXT("path"), Fixture.EnumPath);
    SetPayload->SetArrayField(TEXT("entries"), Strings({ TEXT("Idle"), TEXT("Scanning"), TEXT("Complete") }));
    FTestResponseCapture SetCapture;
    InvokeHandlerWithCapture(TEXT("blueprint.set_enum_entries"), SetPayload, SetCapture);
    TestFalse(TEXT("set_enum_entries refuses an existing name"), SetCapture.bSuccess);
    TestTrue(TEXT("set_enum_entries refusal points at blueprint.add_enum_entries"),
        SetCapture.Message.Contains(TEXT("blueprint.add_enum_entries")));
    TestEqual(TEXT("Scanning still at index 1"), Enum->GetIndexByNameString(TEXT("Scanning")), 1);
    TestEqual(TEXT("enum unchanged after set_enum_entries refusal"), Enum->NumEnums(), 3);

    Fixture.Release();
    return true;
}
