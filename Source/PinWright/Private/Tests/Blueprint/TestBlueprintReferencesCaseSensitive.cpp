// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for board ticket B-blueprint-references-casesensitive-noop.
//
// `caseSensitive` only chose whether both operands were lowercased before comparing.
// The exactTarget branch survived that, because it pinned Equals(ESearchCase::CaseSensitive).
// The DEFAULT substring branch did not: FString::Contains defaults to ESearchCase::IgnoreCase,
// so leaving both strings in their original case changed nothing and the match stayed
// case-insensitive whether the flag was set or not - on `targetPath` AND on `nodeType`.
// The caller got no rejection and no contradicting echo, i.e. positive confirmation of a
// case-exact reference set that was never case-exact.
//
// These are failure-direction tests. Each lowercases a fragment that only matches
// case-INSENSITIVELY, and requires the case-sensitive call to return FEWER rows than the
// case-insensitive one. Reinstate the pre-normalisation and the two counts become equal.

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"

namespace BlueprintReferencesCaseTestUtils
{
    // A synthetic actor blueprint, created in memory only. Nothing about this verb's
    // matching depends on what the blueprint contains beyond it having graph nodes, so
    // the fixture stays content-free.
    inline FString MakePackagePath()
    {
        return FString::Printf(TEXT("/Game/PinWrightTests/BP_RefCaseProbe_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Short));
    }

    inline UBlueprint* MakeActorBlueprint(const FString& PackagePath)
    {
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            return nullptr;
        }
        return FKismetEditorUtilities::CreateBlueprint(
            AActor::StaticClass(), Package,
            FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
            BPTYPE_Normal, UBlueprint::StaticClass(),
            UBlueprintGeneratedClass::StaticClass());
    }

    // Every payload asks for node class paths so the blueprint's own graph nodes become
    // references; that keeps the fixture free of any asset dependency.
    inline TSharedPtr<FJsonObject> MakePayload(const FString& ObjectPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("assetPath"), ObjectPath);
        Payload->SetBoolField(TEXT("includeNodeTypePath"), true);
        return Payload;
    }

    // Returns -1 when the call did not succeed, so a caller can tell a zero-row answer
    // from a refusal instead of scoring a refusal as "filtered everything out".
    inline int32 CountReferences(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Payload,
                                 const TCHAR* Label)
    {
        FTestResponseCapture Capture;
        if (!InvokeHandlerWithCapture(TEXT("blueprint.references"), Payload, Capture))
        {
            Test.AddError(TEXT("blueprint.references handler is not registered"));
            return -1;
        }
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            Test.AddError(FString::Printf(TEXT("%s call failed: %s %s"),
                Label, *Capture.ErrorCode, *Capture.Message));
            return -1;
        }
        double Count = 0.0;
        Capture.Result->TryGetNumberField(TEXT("totalMatchedCount"), Count);
        return static_cast<int32>(Count);
    }
}

// ============================================================================
// caseSensitive must narrow the SUBSTRING path, on both filters.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintReferencesCaseSensitiveNarrowsTest,
    "PinWright.blueprint.references.CaseSensitiveNarrowsTheSubstringMatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintReferencesCaseSensitiveNarrowsTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintReferencesCaseTestUtils;

    const FString PackagePath = MakePackagePath();
    UBlueprint* Blueprint = MakeActorBlueprint(PackagePath);
    if (!Blueprint)
    {
        AddError(TEXT("could not create the probe blueprint"));
        return false;
    }
    Blueprint->AddToRoot();
    ON_SCOPE_EXIT
    {
        Blueprint->RemoveFromRoot();
    };
    const FString ObjectPath = Blueprint->GetPathName();

    // Calibration. Both fragments are the lowercase spelling of text that only ever
    // appears mixed-case in a node class name (K2Node_*) and its script path
    // (/Script/BlueprintGraph.K2Node_*), so a case-insensitive match finds them and a
    // case-sensitive one cannot.
    const TCHAR* const LowercaseNodeType = TEXT("k2node");
    const TCHAR* const LowercaseTargetFragment = TEXT("/script/blueprintgraph");

    {
        TSharedPtr<FJsonObject> Payload = MakePayload(ObjectPath);
        Payload->SetStringField(TEXT("nodeType"), LowercaseNodeType);
        const int32 Insensitive = CountReferences(*this, Payload, TEXT("nodeType default-case"));
        if (Insensitive <= 0)
        {
            // No graph nodes on this host's default blueprint means there is nothing to
            // narrow, and a 0 == 0 comparison would pass while measuring nothing.
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-graph-nodes-in-probe-blueprint"),
                FString::Printf(TEXT("case-insensitive nodeType:'%s' matched %d references on a "
                                     "freshly created actor blueprint; the case-sensitive "
                                     "comparison has nothing to narrow."),
                    LowercaseNodeType, Insensitive));
            return true;
        }

        Payload->SetBoolField(TEXT("caseSensitive"), true);
        const int32 Sensitive = CountReferences(*this, Payload, TEXT("nodeType caseSensitive"));
        TestTrue(TEXT("caseSensitive:true narrows the nodeType filter (it used to be a no-op, "
                      "returning the identical count)"),
            Sensitive >= 0 && Sensitive < Insensitive);
    }

    {
        TSharedPtr<FJsonObject> Payload = MakePayload(ObjectPath);
        Payload->SetStringField(TEXT("targetPath"), LowercaseTargetFragment);
        const int32 Insensitive = CountReferences(*this, Payload, TEXT("targetPath default-case"));
        if (Insensitive <= 0)
        {
            PinWrightTestSkip::SkipAssertions(*this, TEXT("no-node-class-path-references"),
                FString::Printf(TEXT("case-insensitive targetPath:'%s' matched %d references; the "
                                     "case-sensitive comparison has nothing to narrow."),
                    LowercaseTargetFragment, Insensitive));
            return true;
        }

        Payload->SetBoolField(TEXT("caseSensitive"), true);
        const int32 Sensitive = CountReferences(*this, Payload, TEXT("targetPath caseSensitive"));
        TestTrue(TEXT("caseSensitive:true narrows the targetPath substring filter (it used to be "
                      "a no-op, returning the identical count)"),
            Sensitive >= 0 && Sensitive < Insensitive);
    }

    return true;
}

// ============================================================================
// The response has to say which semantics ran, and a modifier with nothing to
// modify is refused rather than accepted and ignored.
// ============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintReferencesCaseModeContractTest,
    "PinWright.blueprint.references.CaseModifierIsEchoedOrRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintReferencesCaseModeContractTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintReferencesCaseTestUtils;

    const FString PackagePath = MakePackagePath();
    UBlueprint* Blueprint = MakeActorBlueprint(PackagePath);
    if (!Blueprint)
    {
        AddError(TEXT("could not create the probe blueprint"));
        return false;
    }
    Blueprint->AddToRoot();
    ON_SCOPE_EXIT
    {
        Blueprint->RemoveFromRoot();
    };
    const FString ObjectPath = Blueprint->GetPathName();

    // A filtered call must publish the case mode its rows were produced under.
    {
        TSharedPtr<FJsonObject> Payload = MakePayload(ObjectPath);
        Payload->SetStringField(TEXT("nodeType"), TEXT("K2Node"));
        Payload->SetBoolField(TEXT("caseSensitive"), true);
        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.references handler registered"),
            InvokeHandlerWithCapture(TEXT("blueprint.references"), Payload, Capture));
        TestTrue(TEXT("a filtered call succeeds"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bEchoed = false;
            TestTrue(TEXT("the response echoes caseSensitive when a filter is active"),
                Capture.Result->TryGetBoolField(TEXT("caseSensitive"), bEchoed));
            TestTrue(TEXT("and echoes the value that actually ran"), bEchoed);
        }
    }

    // An unfiltered call must not silently accept a case modifier: it would return every
    // reference while looking like a filtered answer.
    {
        TSharedPtr<FJsonObject> Payload = MakePayload(ObjectPath);
        Payload->SetBoolField(TEXT("caseSensitive"), true);
        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.references handler registered"),
            InvokeHandlerWithCapture(TEXT("blueprint.references"), Payload, Capture));
        TestFalse(TEXT("caseSensitive with no filter is not answered as a success"),
            Capture.bSuccess);
        TestEqual(TEXT("caseSensitive with no filter is refused with INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(TEXT("and the refusal names the filters it applies to"),
            Capture.Message.Contains(TEXT("targetPath")) && Capture.Message.Contains(TEXT("nodeType")));
    }

    // Same rule for exactTarget, whose only operand is targetPath.
    {
        TSharedPtr<FJsonObject> Payload = MakePayload(ObjectPath);
        Payload->SetBoolField(TEXT("exactTarget"), true);
        FTestResponseCapture Capture;
        TestTrue(TEXT("blueprint.references handler registered"),
            InvokeHandlerWithCapture(TEXT("blueprint.references"), Payload, Capture));
        TestFalse(TEXT("exactTarget with no targetPath is not answered as a success"),
            Capture.bSuccess);
        TestEqual(TEXT("exactTarget with no targetPath is refused with INVALID_ARGUMENT"),
            Capture.ErrorCode, FString(TEXT("INVALID_ARGUMENT")));
        TestTrue(TEXT("and the refusal names targetPath"),
            Capture.Message.Contains(TEXT("targetPath")));
    }

    return true;
}
