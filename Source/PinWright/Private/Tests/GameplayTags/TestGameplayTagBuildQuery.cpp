// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tests/GameplayTags/TestGameplayTagQueryTargetFixture.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"
#include "UObject/Package.h"

namespace
{
TSharedPtr<FJsonObject> MakeLeafExpression(const FString& Op, const TArray<FString>& Tags)
{
    TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
    Node->SetStringField(TEXT("op"), Op);
    TArray<TSharedPtr<FJsonValue>> TagValues;
    TagValues.Reserve(Tags.Num());
    for (const FString& Tag : Tags)
    {
        TagValues.Add(MakeShared<FJsonValueString>(Tag));
    }
    Node->SetArrayField(TEXT("tags"), TagValues);
    return Node;
}

TSharedPtr<FJsonObject> MakeCompositeExpression(const FString& Op, const TArray<TSharedPtr<FJsonObject>>& Children)
{
    TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
    Node->SetStringField(TEXT("op"), Op);
    TArray<TSharedPtr<FJsonValue>> ChildValues;
    ChildValues.Reserve(Children.Num());
    for (const TSharedPtr<FJsonObject>& Child : Children)
    {
        ChildValues.Add(MakeShared<FJsonValueObject>(Child));
    }
    Node->SetArrayField(TEXT("expressions"), ChildValues);
    return Node;
}

// Discover a tag from the live registry so the test does not depend on a specific project ini.
FString FindRegisteredTagForTest()
{
    UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
    const TCHAR* Preferred[] = {
        TEXT("Ability.Type"),
        TEXT("Ability.Type.Action"),
        TEXT("Ability.Activation"),
    };
    for (const TCHAR* Candidate : Preferred)
    {
        if (Manager.RequestGameplayTag(FName(Candidate), false).IsValid())
        {
            return FString(Candidate);
        }
    }

    FGameplayTagContainer All;
    Manager.RequestAllGameplayTags(All, true);
    const TArray<FGameplayTag>& Array = All.GetGameplayTagArray();
    if (Array.Num() > 0)
    {
        return Array[0].ToString();
    }
    return FString();
}

// Two registered tags with NO ancestor/descendant relation, for the ApplyOpKind semantics
// cases below. Sibling of FindRegisteredTagForTest, which the pre-existing cases still use.
//
// Container matching is hierarchical: FQueryEvaluator resolves every leaf op through
// FGameplayTagContainer::HasTag, so a container holding Ability.Type.Action reports
// HasTag(Ability.Type) as true. A parent/child pair would therefore collapse the
// "container that does NOT hold the tag" half of every truth table and silently
// un-discriminate the ops. Gameplay tag parentage is exactly dotted-name prefixing, so the
// string test is exact and avoids an O(n^2) sweep through
// UGameplayTagsManager::RequestGameplayTagParents. Prefix matching is case-insensitive so an
// unexpected casing can only make an unrelated pair look related (skip it and keep looking),
// never make a related pair look unrelated.
bool FindTwoUnrelatedRegisteredTagsForTest(FGameplayTag& OutFirst, FGameplayTag& OutSecond)
{
    FGameplayTagContainer All;
    UGameplayTagsManager::Get().RequestAllGameplayTags(All, true);
    const TArray<FGameplayTag>& Array = All.GetGameplayTagArray();

    for (int32 First = 0; First < Array.Num(); ++First)
    {
        const FString FirstName = Array[First].ToString();
        for (int32 Second = First + 1; Second < Array.Num(); ++Second)
        {
            const FString SecondName = Array[Second].ToString();
            if (FirstName.Equals(SecondName, ESearchCase::IgnoreCase)
                || FirstName.StartsWith(SecondName + TEXT("."), ESearchCase::IgnoreCase)
                || SecondName.StartsWith(FirstName + TEXT("."), ESearchCase::IgnoreCase))
            {
                continue;
            }
            OutFirst = Array[First];
            OutSecond = Array[Second];
            return true;
        }
    }
    return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameplayTagBuildQueryHandlerTest,
    "PinWright.GameplayTags.BuildQuery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGameplayTagBuildQueryHandlerTest::RunTest(const FString& Parameters)
{
    const FString RegisteredTag = FindRegisteredTagForTest();
    if (!TestFalse(TEXT("Test requires at least one registered gameplay tag"), RegisteredTag.IsEmpty()))
    {
        return false;
    }

    FTestResponseCapture Capture;

    {
        const TSharedPtr<FJsonObject> Any = MakeLeafExpression(TEXT("any_tags_match"), {RegisteredTag});
        const TSharedPtr<FJsonObject> No = MakeLeafExpression(TEXT("no_tags_match"), {RegisteredTag});
        const TSharedPtr<FJsonObject> Root = MakeCompositeExpression(TEXT("all_expressions_match"), {Any, No});

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("expression"), Root);

        const bool bFound = InvokeHandlerWithCapture(TEXT("gameplay_tags.build_query"), Payload, Capture);
        TestTrue(TEXT("build_query handler found"), bFound);
        TestTrue(TEXT("composite case returned response"), Capture.bWasCalled);
        TestTrue(TEXT("composite case succeeded"), Capture.bSuccess);
        TestTrue(TEXT("composite case returned a result"), Capture.Result.IsValid());

        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            FString Description;
            TestTrue(TEXT("description field present"), Capture.Result->TryGetStringField(TEXT("description"), Description));
            TestFalse(TEXT("description is non-empty"), Description.IsEmpty());

            // Pin the exact synthesized string, not merely non-emptiness. With no caller
            // `description`, the handler falls back to SynthesizeDescription()
            // (GameplayTagBuildQueryHandler.cpp:292), which emits "<op>[<child>,<child>]"
            // for a composite and "<op>(<tag>,<tag>)" for a leaf, using the caller's own op
            // strings and tag strings verbatim. Note what this does NOT pin: the string is
            // derived from the request JSON, never from the built FGameplayTagQuery, so it
            // stays identical no matter what ApplyOpKind maps the op to. The Matches-based
            // cases at the end of this test are what pin that.
            const FString ExpectedDescription = FString::Printf(
                TEXT("all_expressions_match[any_tags_match(%s),no_tags_match(%s)]"),
                *RegisteredTag, *RegisteredTag);
            TestEqual(TEXT("description is the exact synthesized summary"), Description, ExpectedDescription);

            int32 TokenStreamBytes = 0;
            TestTrue(TEXT("tokenStreamBytes field present"),
                Capture.Result->TryGetNumberField(TEXT("tokenStreamBytes"), TokenStreamBytes));
            TestTrue(TEXT("tokenStreamBytes > 0"), TokenStreamBytes > 0);

            bool bWrote = true;
            TestTrue(TEXT("wrote field present"), Capture.Result->TryGetBoolField(TEXT("wrote"), bWrote));
            TestFalse(TEXT("wrote is false without target"), bWrote);
        }
    }

    {
        const TSharedPtr<FJsonObject> Root = MakeLeafExpression(TEXT("any_tags_match"), {});

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("expression"), Root);

        const bool bFound = InvokeHandlerWithCapture(TEXT("gameplay_tags.build_query"), Payload, Capture);
        TestTrue(TEXT("build_query handler found (empty tags)"), bFound);
        TestTrue(TEXT("empty tags case returned response"), Capture.bWasCalled);
        TestFalse(TEXT("empty tags case is an error"), Capture.bSuccess);
        TestEqual(TEXT("empty tags case emits EMPTY_TAGS"), Capture.ErrorCode, FString(TEXT("EMPTY_TAGS")));
    }

    // =====================================================================================
    // ApplyOpKind semantics — Private/Handlers/GameplayTags/GameplayTagBuildQueryHandler.cpp
    // lines 70-82, the switch mapping an op string to the query expression kind.
    //
    // Nothing above can observe that switch. `description` is produced by
    // SynthesizeDescription(), which walks the CALLER's JSON and never the built
    // FGameplayTagQuery, so it echoes the op string rather than the expression kind;
    // `tokenStreamBytes` is a byte count that a changed op leaves the same length; `wrote`
    // is unrelated to semantics. Swap any two cases in ApplyOpKind and all of them stay green.
    //
    // The handler's only surface that hands the built query back is the optional `target`
    // write, so each op below is built into a real FGameplayTagQuery UPROPERTY on a loadable
    // asset, read back, and EVALUATED with FGameplayTagQuery::Matches against four probe
    // containers: {A}, {B}, {A,B} and {}. The six expressions are chosen so their truth
    // tables over those four containers are pairwise distinct, including under the degenerate
    // forms a swap produces — a tag op that received children emits an empty tag set, and an
    // expr op that received tags emits an empty expression set, per
    // FGameplayTagQueryExpression::EmitTokens. Empty AnyTags/AnyExpr evaluate false for every
    // container; empty AllTags/NoTags/AllExpr/NoExpr evaluate true for every container. So
    // each of the 15 possible case swaps fails at least one assertion below.
    // =====================================================================================
    FGameplayTag TagA;
    FGameplayTag TagB;
    if (!FindTwoUnrelatedRegisteredTagsForTest(TagA, TagB))
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("too-few-registered-gameplay-tags"),
            TEXT("fewer than two unrelated registered gameplay tags; skipping GameplayTags.BuildQuery semantics cases."));
        return true;
    }

    // A real, loadable package: the handler resolves target.assetPath through LoadObject,
    // which a transient NewObject would not satisfy. GUID-suffixed so parallel or repeat runs
    // never collide.
    const FString TargetPkgPath = FString::Printf(TEXT("/Game/PinWrightTest_TagQuery_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString TargetAssetName = FPackageName::GetLongPackageAssetName(TargetPkgPath);

    UPackage* TargetPackage = CreatePackage(*TargetPkgPath);
    TestNotNull(TEXT("tag query target package created"), TargetPackage);
    if (!TargetPackage)
    {
        return false;
    }
    ON_SCOPE_EXIT { CleanupTestAsset(TargetPkgPath); };

    UTestGameplayTagQueryTarget* QueryTarget = NewObject<UTestGameplayTagQueryTarget>(
        TargetPackage, *TargetAssetName, RF_Public | RF_Standalone | RF_Transactional);
    TestNotNull(TEXT("tag query target asset created"), QueryTarget);
    if (!QueryTarget)
    {
        return false;
    }
    const FString TargetAssetPath = QueryTarget->GetPathName();

    const FString TagAName = TagA.ToString();
    const FString TagBName = TagB.ToString();

    FGameplayTagContainer HoldsA;
    HoldsA.AddTag(TagA);
    FGameplayTagContainer HoldsB;
    HoldsB.AddTag(TagB);
    FGameplayTagContainer HoldsBoth;
    HoldsBoth.AddTag(TagA);
    HoldsBoth.AddTag(TagB);
    const FGameplayTagContainer HoldsNeither;

    // Builds one expression through the handler's target write, then asserts the truth table
    // of the FGameplayTagQuery that actually landed in the fixture property.
    auto AssertQueryTruthTable = [&](const TCHAR* Label, const TSharedPtr<FJsonObject>& Root,
        bool bOnA, bool bOnB, bool bOnBoth, bool bOnNeither)
    {
        // Clear first: a handler that silently failed to write must not pass on the previous
        // case's query still sitting in the property.
        QueryTarget->Query.Clear();

        TSharedPtr<FJsonObject> TargetSpec = MakeShared<FJsonObject>();
        TargetSpec->SetStringField(TEXT("assetPath"), TargetAssetPath);
        TargetSpec->SetStringField(TEXT("propertyPath"), TEXT("Query"));

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetObjectField(TEXT("expression"), Root);
        Payload->SetObjectField(TEXT("target"), TargetSpec);

        FTestResponseCapture TargetCapture;
        TestTrue(*FString::Printf(TEXT("%s: build_query handler found"), Label),
            InvokeHandlerWithCapture(TEXT("gameplay_tags.build_query"), Payload, TargetCapture));
        if (!TargetCapture.bSuccess)
        {
            AddError(FString::Printf(
                TEXT("%s: build_query with target failed (error code %s); ApplyOpKind is unverified for this op."),
                Label, *TargetCapture.ErrorCode));
            return;
        }

        bool bWroteTarget = false;
        if (TargetCapture.Result.IsValid())
        {
            TargetCapture.Result->TryGetBoolField(TEXT("wrote"), bWroteTarget);
        }
        TestTrue(*FString::Printf(TEXT("%s: wrote is true when a target is supplied"), Label),
            bWroteTarget);

        const auto Probe = [&](const TCHAR* Which, const FGameplayTagContainer& Probed, bool bExpected)
        {
            const bool bActual = QueryTarget->Query.Matches(Probed);
            TestTrue(*FString::Printf(TEXT("%s: container %s expected %s, got %s"),
                Label, Which,
                bExpected ? TEXT("match") : TEXT("no match"),
                bActual ? TEXT("match") : TEXT("no match")),
                bActual == bExpected);
        };

        Probe(TEXT("{A}"), HoldsA, bOnA);
        Probe(TEXT("{B}"), HoldsB, bOnB);
        Probe(TEXT("{A,B}"), HoldsBoth, bOnBoth);
        Probe(TEXT("{}"), HoldsNeither, bOnNeither);
    };

    // Leaf ops, each over BOTH tags. Two tags, not one: with a single tag any_tags_match and
    // all_tags_match have identical truth tables, which is precisely the hole a swapped case
    // would hide in.
    //                                                          {A}    {B}    {A,B}  {}
    AssertQueryTruthTable(TEXT("any_tags_match{A,B}"),
        MakeLeafExpression(TEXT("any_tags_match"), {TagAName, TagBName}),
                                                                true,  true,  true,  false);
    AssertQueryTruthTable(TEXT("all_tags_match{A,B}"),
        MakeLeafExpression(TEXT("all_tags_match"), {TagAName, TagBName}),
                                                                false, false, true,  false);
    AssertQueryTruthTable(TEXT("no_tags_match{A,B}"),
        MakeLeafExpression(TEXT("no_tags_match"), {TagAName, TagBName}),
                                                                false, false, false, true);

    // Composite ops over the same two children, so the composite case under test is the only
    // thing that differs between them: all / any / no over {A} and {B} give AND / OR / NOR.
    const auto MakeUnrelatedTagChildren = [&]()
    {
        return TArray<TSharedPtr<FJsonObject>>{
            MakeLeafExpression(TEXT("any_tags_match"), {TagAName}),
            MakeLeafExpression(TEXT("any_tags_match"), {TagBName})};
    };

    //                                                          {A}    {B}    {A,B}  {}
    AssertQueryTruthTable(TEXT("all_expressions_match[any{A},any{B}]"),
        MakeCompositeExpression(TEXT("all_expressions_match"), MakeUnrelatedTagChildren()),
                                                                false, false, true,  false);
    AssertQueryTruthTable(TEXT("any_expressions_match[any{A},any{B}]"),
        MakeCompositeExpression(TEXT("any_expressions_match"), MakeUnrelatedTagChildren()),
                                                                true,  true,  true,  false);
    AssertQueryTruthTable(TEXT("no_expressions_match[any{A},any{B}]"),
        MakeCompositeExpression(TEXT("no_expressions_match"), MakeUnrelatedTagChildren()),
                                                                false, false, false, true);

    return true;
}
