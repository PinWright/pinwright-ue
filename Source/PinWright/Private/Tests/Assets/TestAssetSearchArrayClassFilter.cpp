// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board ticket B-asset-search-array-class-filter-silently-dropped.
//
// asset.search declared classFilter as "string" and read it with Ctx.GetString alone. An
// array value reached FJsonValue::AsString, which returns "" on a type mismatch, so the
// filter never ran: every asset the registry returned was kept, the classFilter echo was
// suppressed by that same emptiness, and classFilterMode was still published — a full
// result set answered as a filtered one. The array spelling is not exotic: the sibling
// blueprint.build_api_index declares this same parameter name AS an array.
//
// These are failure-direction tests. Restore the single Ctx.GetString read and
// ArrayClassFilterExcludesOtherClasses fails on the probes of the OTHER two classes coming
// back, and MalformedArrayClassFilterIsRefused fails on the malformed arrays answering
// success. A test that only asserted the call succeeded would pass against the defect.
//
// Fixtures are in-memory probes of three different classes under a folder unique to this
// file, registered with FAssetRegistryModule::AssetCreated, so nothing here depends on
// host-project content and the excluded rows are known by construction.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestAssetTeardown.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Engine/DataTable.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace PwAssetSearchArrayClassFilterProbes
{
    // One folder, three probes, three classes. The query matches all three by name, so the
    // class filter is the only thing that can separate them.
    static const TCHAR* ProbeFolder = TEXT("/Game/PwArrayClassFilterProbe");
    static const TCHAR* TableProbe = TEXT("PwArrayClassFilterProbeTable");
    static const TCHAR* CurveProbe = TEXT("PwArrayClassFilterProbeCurve");
    static const TCHAR* ColorProbe = TEXT("PwArrayClassFilterProbeColor");
    static const TCHAR* ProbeQuery = TEXT("PwArrayClassFilterProbe");

    inline FString ObjectPathFor(const FString& AssetName)
    {
        return FString::Printf(TEXT("%s/%s.%s"), ProbeFolder, *AssetName, *AssetName);
    }

    template <typename TAsset>
    void Create(const FString& AssetName)
    {
        UPackage* Pkg = CreatePackage(*(FString(ProbeFolder) / AssetName));
        if (!Pkg)
        {
            return;
        }
        if (TAsset* Asset = NewObject<TAsset>(Pkg, FName(*AssetName), RF_Public | RF_Standalone))
        {
            FAssetRegistryModule::AssetCreated(Asset);
        }
    }

    inline void CreateAll()
    {
        Create<UDataTable>(TableProbe);
        Create<UCurveFloat>(CurveProbe);
        Create<UCurveLinearColor>(ColorProbe);
    }

    inline void DestroyAll()
    {
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPathFor(TableProbe));
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPathFor(CurveProbe));
        PwTestAssetTeardown::DiscardCreatedAssetByObjectPath(ObjectPathFor(ColorProbe));
    }

    // Runs asset.search over the probe folder and returns row name -> row class.
    inline TMap<FString, FString> SearchRows(FAutomationTestBase& Test, const FString& Label,
        const TSharedPtr<FJsonObject>& Payload, TSharedPtr<FJsonObject>& OutResult)
    {
        Payload->SetStringField(TEXT("query"), ProbeQuery);
        Payload->SetStringField(TEXT("path"), ProbeFolder);

        TMap<FString, FString> Rows;
        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture))
        {
            Test.AddError(TEXT("asset.search handler is not registered"));
            return Rows;
        }
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            Test.AddError(FString::Printf(TEXT("asset.search (%s) failed: %s %s"),
                *Label, *Capture.ErrorCode, *Capture.Message));
            return Rows;
        }
        OutResult = Capture.Result;

        const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
        if (Capture.Result->TryGetArrayField(TEXT("assets"), Assets) && Assets)
        {
            for (const TSharedPtr<FJsonValue>& Value : *Assets)
            {
                const TSharedPtr<FJsonObject>* Row = nullptr;
                if (Value.IsValid() && Value->TryGetObject(Row) && Row)
                {
                    FString Name, Class;
                    (*Row)->TryGetStringField(TEXT("name"), Name);
                    (*Row)->TryGetStringField(TEXT("class"), Class);
                    Rows.Add(Name, Class);
                }
            }
        }
        return Rows;
    }

    inline TSharedPtr<FJsonObject> PayloadWithArrayFilter(const TArray<FString>& ClassNames)
    {
        TArray<TSharedPtr<FJsonValue>> Filter;
        for (const FString& ClassName : ClassNames)
        {
            Filter.Add(MakeShared<FJsonValueString>(ClassName));
        }
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetArrayField(TEXT("classFilter"), Filter);
        return Payload;
    }
}

// ============================================================================
// An array class filter EXCLUDES. The measured defect was 38 rows of which none
// was of the requested class, so the assertion that carries this test is the
// absence of the other two probes - not that the call succeeded.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchArrayClassFilterTest,
    "PinWright.asset.search.ArrayClassFilterExcludesOtherClasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetSearchArrayClassFilterTest::RunTest(const FString& Parameters)
{
    using namespace PwAssetSearchArrayClassFilterProbes;

    CreateAll();

    // Control: unfiltered, all three probes are in range of the query. Without this the
    // filtered assertions below could pass on a folder that never held the excluded rows.
    {
        TSharedPtr<FJsonObject> Result;
        const TMap<FString, FString> Rows =
            SearchRows(*this, TEXT("unfiltered"), MakeShared<FJsonObject>(), Result);
        TestTrue(TEXT("the DataTable probe is in range of the query"), Rows.Contains(TableProbe));
        TestTrue(TEXT("the CurveFloat probe is in range of the query"), Rows.Contains(CurveProbe));
        TestTrue(TEXT("the CurveLinearColor probe is in range of the query"),
            Rows.Contains(ColorProbe));

        // No filter ran, so nothing should describe how one was matched.
        FString Mode;
        TestFalse(TEXT("a search with no class filter does not echo classFilterMode"),
            Result.IsValid() && Result->TryGetStringField(TEXT("classFilterMode"), Mode));
    }

    // One-element array: the two probes of other classes must be GONE.
    {
        TSharedPtr<FJsonObject> Result;
        const TMap<FString, FString> Rows = SearchRows(*this, TEXT("array [DataTable]"),
            PayloadWithArrayFilter({TEXT("DataTable")}), Result);

        TestTrue(TEXT("classFilter:[\"DataTable\"] keeps the DataTable probe"),
            Rows.Contains(TableProbe));
        TestFalse(TEXT("and EXCLUDES the CurveFloat probe (an array filter used to be read as "
                       "the empty string, so every row came back)"),
            Rows.Contains(CurveProbe));
        TestFalse(TEXT("and EXCLUDES the CurveLinearColor probe"), Rows.Contains(ColorProbe));

        // The echo has to name the filter that ran, or a caller cannot tell a filtered
        // page from an unfiltered one - which is how the drop stayed invisible.
        const TArray<TSharedPtr<FJsonValue>>* Echo = nullptr;
        TestTrue(TEXT("the applied classFilter is echoed back as the array it was sent as"),
            Result.IsValid() && Result->TryGetArrayField(TEXT("classFilter"), Echo) && Echo &&
                Echo->Num() == 1);
    }

    // The string form is the shape that always worked; the array must not diverge from it.
    {
        TSharedPtr<FJsonObject> StringPayload = MakeShared<FJsonObject>();
        StringPayload->SetStringField(TEXT("classFilter"), TEXT("DataTable"));
        TSharedPtr<FJsonObject> StringResult;
        const TMap<FString, FString> StringRows =
            SearchRows(*this, TEXT("string DataTable"), StringPayload, StringResult);

        TSharedPtr<FJsonObject> ArrayResult;
        const TMap<FString, FString> ArrayRows = SearchRows(*this, TEXT("array [DataTable]"),
            PayloadWithArrayFilter({TEXT("DataTable")}), ArrayResult);

        TestEqual(TEXT("a one-element array filters exactly as the equivalent string does"),
            ArrayRows.Num(), StringRows.Num());
    }

    // Two elements are an OR, and the class named by neither is still excluded.
    {
        TSharedPtr<FJsonObject> Result;
        const TMap<FString, FString> Rows = SearchRows(*this, TEXT("array [DataTable,CurveFloat]"),
            PayloadWithArrayFilter({TEXT("DataTable"), TEXT("CurveFloat")}), Result);

        TestTrue(TEXT("a two-element array keeps the DataTable probe"), Rows.Contains(TableProbe));
        TestTrue(TEXT("and keeps the CurveFloat probe"), Rows.Contains(CurveProbe));
        TestFalse(TEXT("and still excludes the class named by neither element"),
            Rows.Contains(ColorProbe));
    }

    DestroyAll();
    return true;
}

// ============================================================================
// The shapes that cannot be filtered with are REFUSED, not quietly turned into
// "no filter". A third silent behaviour is what this ticket exists to prevent.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetSearchMalformedArrayClassFilterTest,
    "PinWright.asset.search.MalformedArrayClassFilterIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetSearchMalformedArrayClassFilterTest::RunTest(const FString& Parameters)
{
    using namespace PwAssetSearchArrayClassFilterProbes;

    CreateAll();

    // An empty array, a number element and an empty-string element all match every asset
    // if they are allowed through, which is indistinguishable from no filter at all.
    const auto ExpectRefusal = [this](const TCHAR* Label,
        const TArray<TSharedPtr<FJsonValue>>& Filter)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("query"), ProbeQuery);
        Payload->SetStringField(TEXT("path"), ProbeFolder);
        Payload->SetArrayField(TEXT("classFilter"), Filter);

        FTestResponseCapture Capture;
        TestTrue(TEXT("asset.search handler is registered"),
            InvokeHandlerWithCapture(TEXT("asset.search"), Payload, Capture));
        TestFalse(*FString::Printf(
            TEXT("classFilter as an %s is not answered with an unfiltered success"), Label),
            Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("classFilter as an %s is refused with INVALID_ARGUMENT"),
            Label), Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(*FString::Printf(TEXT("the refusal of an %s names the parameter"), Label),
            Capture.Message.Contains(TEXT("classFilter")));
    };

    ExpectRefusal(TEXT("empty array"), TArray<TSharedPtr<FJsonValue>>());

    TArray<TSharedPtr<FJsonValue>> NumberElement;
    NumberElement.Add(MakeShared<FJsonValueNumber>(1.0));
    ExpectRefusal(TEXT("number element"), NumberElement);

    TArray<TSharedPtr<FJsonValue>> EmptyStringElement;
    EmptyStringElement.Add(MakeShared<FJsonValueString>(FString()));
    ExpectRefusal(TEXT("empty string element"), EmptyStringElement);

    DestroyAll();
    return true;
}
