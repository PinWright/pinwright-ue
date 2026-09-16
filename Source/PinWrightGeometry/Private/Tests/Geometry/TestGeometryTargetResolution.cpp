// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression coverage for geometry target identity. Display labels are editor-facing text and
// can be shared by several DynamicMeshActors; geometry handlers must refuse that label instead
// of mutating whichever actor the level iterator happened to return first. Exact internal names
// and full object paths remain deterministic recovery keys.
#include "Misc/AutomationTest.h"

#include "Handlers/Geometry/GeometryTarget.h"
#include "Handlers/Geometry/GeometryUtils.h"
#include "Handlers/HandlerContext.h"
#include "Utils/PathUtils.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"

#include "Dom/JsonObject.h"
#include "DynamicMeshActor.h"
#include "Components/DynamicMeshComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"

namespace
{
    FString GeometryTargetTestSharedLabel()
    {
        return FString::Printf(TEXT("PWGeometryTarget_%sX"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    ADynamicMeshActor* GeometryTargetTestSpawnActor(
        UWorld* World, const FString& Label, const FVector& Location)
    {
        if (!World)
        {
            return nullptr;
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        ADynamicMeshActor* Actor = World->SpawnActor<ADynamicMeshActor>(
            ADynamicMeshActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
        if (Actor)
        {
            Actor->SetActorLabel(Label);
        }
        return Actor;
    }

    AActor* GeometryTargetTestSpawnNonMeshActor(
        UWorld* World, const FString& Label, const FVector& Location)
    {
        if (!World)
        {
            return nullptr;
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* Actor = World->SpawnActor<AActor>(
            AActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
        if (Actor)
        {
            Actor->SetActorLabel(Label);
        }
        return Actor;
    }

    bool GeometryTargetTestCandidatePathsContain(
        FAutomationTestBase& Test,
        const TArray<TSharedPtr<FJsonValue>>* CandidateValues,
        int32 ExpectedCount,
        const FString& FirstPath,
        const FString& SecondPath)
    {
        bool bSawFirst = false;
        bool bSawSecond = false;
        int32 EmittedCandidateCount = 0;
        if (CandidateValues)
        {
            for (const TSharedPtr<FJsonValue>& Value : *CandidateValues)
            {
                const TSharedPtr<FJsonObject>* Candidate = nullptr;
                if (!Value.IsValid() || !Value->TryGetObject(Candidate) || !Candidate || !Candidate->IsValid())
                {
                    continue;
                }

                FString Label;
                FString Name;
                FString Path;
                FString Class;
                const bool bHasFields = (*Candidate)->TryGetStringField(TEXT("label"), Label)
                    && (*Candidate)->TryGetStringField(TEXT("name"), Name)
                    && (*Candidate)->TryGetStringField(TEXT("path"), Path)
                    && (*Candidate)->TryGetStringField(TEXT("class"), Class)
                    && !Label.IsEmpty() && !Name.IsEmpty() && !Path.IsEmpty() && !Class.IsEmpty();
                Test.TestTrue(TEXT("each ambiguity candidate has label/name/path/class"), bHasFields);
                ++EmittedCandidateCount;

                if ((*Candidate)->TryGetStringField(TEXT("path"), Path))
                {
                    bSawFirst |= Path == FirstPath;
                    bSawSecond |= Path == SecondPath;
                }
            }
        }

        Test.TestEqual(TEXT("candidateCount equals emitted candidate records"),
            EmittedCandidateCount, CandidateValues ? CandidateValues->Num() : 0);
        Test.TestEqual(TEXT("candidate payload has the expected number of records"),
            EmittedCandidateCount, ExpectedCount);
        Test.TestTrue(TEXT("ambiguity candidates include the first actor path"), bSawFirst);
        Test.TestTrue(TEXT("ambiguity candidates include the second actor path"), bSawSecond);
        return bSawFirst && bSawSecond;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryTargetDuplicateLabelResolutionTest,
    "PinWright.geometry.target.DuplicateLabelReturnsAmbiguousCandidates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryTargetDuplicateLabelResolutionTest::RunTest(const FString& Parameters)
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-editor-world"),
            TEXT("No editor world available; skipping geometry target identity regression."));
        return true;
    }

    FScopedEditorWorldActorGuard Guard;
    const FString SharedLabel = GeometryTargetTestSharedLabel();
    ADynamicMeshActor* First = GeometryTargetTestSpawnActor(World, SharedLabel, FVector::ZeroVector);
    ADynamicMeshActor* Second = GeometryTargetTestSpawnActor(World, SharedLabel, FVector(200.0, 0.0, 0.0));
    AActor* NonMesh = GeometryTargetTestSpawnNonMeshActor(
        World, SharedLabel, FVector(400.0, 0.0, 0.0));
    const FString UniqueLabel = GeometryTargetTestSharedLabel();
    ADynamicMeshActor* SubstringOnly = GeometryTargetTestSpawnActor(
        World, UniqueLabel + TEXT("_suffix"), FVector(600.0, 0.0, 0.0));
    if (!First || !Second || !NonMesh || !SubstringOnly)
    {
        AddError(TEXT("Failed to spawn the geometry target identity probes."));
        return false;
    }

    const FString FirstPath = First->GetPathName();
    const FString SecondPath = Second->GetPathName();
    const McpActorUtils::FActorResolution MeshResolution =
        GeometryTarget::ResolveMeshActor(World, SharedLabel);
    TestTrue(TEXT("duplicate DynamicMeshActor labels resolve as ambiguous"),
        MeshResolution.IsAmbiguous());
    TestFalse(TEXT("ambiguous geometry target has no selected actor"), MeshResolution.IsResolved());
    TestEqual(TEXT("both duplicate-label mesh candidates are returned"),
        MeshResolution.Candidates.Num(), 2);
    TestTrue(TEXT("mesh candidates include the first actor"), MeshResolution.Candidates.Contains(First));
    TestTrue(TEXT("mesh candidates include the second actor"), MeshResolution.Candidates.Contains(Second));
    TestFalse(TEXT("mesh resolver filters a non-DynamicMesh actor sharing the label"),
        MeshResolution.Candidates.Contains(NonMesh));
    TestNull(TEXT("nullable mesh wrapper refuses an ambiguous label"),
        GeometryTarget::FindMeshActor(World, SharedLabel));

    const McpActorUtils::FActorResolution AnyResolution =
        GeometryTarget::ResolveAnyActor(World, SharedLabel);
    TestTrue(TEXT("any-actor lookup keeps duplicate labels ambiguous"), AnyResolution.IsAmbiguous());
    TestEqual(TEXT("any-actor lookup returns both duplicate-label candidates"),
        AnyResolution.Candidates.Num(), 3);
    TestTrue(TEXT("any-actor candidates include the non-DynamicMesh actor"),
        AnyResolution.Candidates.Contains(NonMesh));
    TestNull(TEXT("nullable any-actor wrapper refuses an ambiguous label"),
        GeometryTarget::FindAnyActor(World, SharedLabel));

    const McpActorUtils::FActorResolution SubstringResolution =
        GeometryTarget::ResolveMeshActor(World, UniqueLabel);
    TestEqual(TEXT("geometry target does not use label substrings"),
        SubstringResolution.Status, McpActorUtils::EActorResolveStatus::NotFound);
    const McpActorUtils::FActorResolution LabelOnlyNameResolution =
        McpActorUtils::ResolveActorFiltered(World, First->GetName(),
            [](AActor*) { return true; }, McpActorUtils::EActorResolvePolicy::ExactLabel);
    TestEqual(TEXT("label-only resolver does not reinterpret an object name"),
        LabelOnlyNameResolution.Status, McpActorUtils::EActorResolveStatus::NotFound);

    // Exercise the production geometry error path so a nullable helper cannot hide the typed
    // ambiguity behind ACTOR_NOT_FOUND.
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("actorName"), SharedLabel);
    FTestResponseCapture Capture;
    FHandlerContext Context = FHandlerContext::MakeTestContextWithCapture(
        TEXT("geometry-target-test"), TEXT("geometry.get_mesh_info"), Payload, &Capture);
    FGeometryTarget Target;
    Target.Actor = First;
    Target.Component = First->GetDynamicMeshComponent();
    Target.Mesh = Target.Component ? Target.Component->GetDynamicMesh() : nullptr;
    TestFalse(TEXT("ambiguous geometry target is rejected by production resolver"),
        GeometryTarget::ResolveOrSendError(Context, SharedLabel, Target));
    TestNull(TEXT("failed resolution clears the stale actor"), Target.Actor);
    TestNull(TEXT("failed resolution clears the stale component"), Target.Component);
    TestNull(TEXT("failed resolution clears the stale mesh"), Target.Mesh);
    TestEqual(TEXT("ambiguous geometry target reports the typed error"),
        Capture.ErrorCode, FString(TEXT("AMBIGUOUS_ACTOR_NAME")));

    const TArray<TSharedPtr<FJsonValue>>* CandidateValues = nullptr;
    TestTrue(TEXT("typed ambiguity response carries candidate records"),
        Capture.Result.IsValid() &&
        Capture.Result->TryGetArrayField(TEXT("candidates"), CandidateValues) && CandidateValues);
    GeometryTargetTestCandidatePathsContain(*this, CandidateValues, 2, FirstPath, SecondPath);

    FString MatchedBy;
    TestTrue(TEXT("typed ambiguity payload identifies label matching"),
        Capture.Result.IsValid()
        && Capture.Result->TryGetStringField(TEXT("matchedBy"), MatchedBy)
        && MatchedBy == TEXT("label"));
    double CandidateCount = 0.0;
    TestTrue(TEXT("typed ambiguity payload reports candidate count"),
        Capture.Result.IsValid()
        && Capture.Result->TryGetNumberField(TEXT("candidateCount"), CandidateCount));
    TestEqual(TEXT("typed ambiguity payload count matches mesh candidates"),
        static_cast<int32>(CandidateCount), MeshResolution.Candidates.Num());

    // Drive a real geometry mutator through its registered handler with a full object path.
    // The request echo remains the path supplied by the caller, while the canonical response
    // fields expose the actor identity the production resolver actually selected.
    TSharedPtr<FJsonObject> HandlerPayload = MakeShared<FJsonObject>();
    HandlerPayload->SetStringField(TEXT("actorName"), FirstPath);
    FTestResponseCapture HandlerCapture;
    const bool bAppendHandlerFound = InvokeHandlerWithCapture(
        TEXT("geometry.append_vertex"), HandlerPayload, HandlerCapture);
    TestTrue(TEXT("geometry.append_vertex handler is registered"), bAppendHandlerFound);
    TestTrue(TEXT("path-resolved geometry mutator succeeds"), HandlerCapture.bSuccess);
    if (HandlerCapture.bSuccess && HandlerCapture.Result.IsValid())
    {
        FString ActorNameEcho;
        FString ActorPath;
        FString ActorObjectName;
        TestTrue(TEXT("mutator preserves the actorName request echo"),
            HandlerCapture.Result->TryGetStringField(TEXT("actorName"), ActorNameEcho)
            && ActorNameEcho == FirstPath);
        TestTrue(TEXT("mutator returns the resolved actor path"),
            HandlerCapture.Result->TryGetStringField(TEXT("actorPath"), ActorPath)
            && ActorPath == FirstPath);
        TestTrue(TEXT("mutator returns the resolved actor object name"),
            HandlerCapture.Result->TryGetStringField(TEXT("actorObjectName"), ActorObjectName)
            && ActorObjectName == First->GetName());
    }

    // boolean_trim resolves both operands before running the operation, but must report the
    // target's missing identity before a duplicate trim label's ambiguity.
    TSharedPtr<FJsonObject> OrderPayload = MakeShared<FJsonObject>();
    OrderPayload->SetStringField(TEXT("actorName"), TEXT("PWGeometryTarget_missing_target"));
    OrderPayload->SetStringField(TEXT("trimActorName"), SharedLabel);
    FTestResponseCapture OrderCapture;
    const bool bTrimHandlerFound = InvokeHandlerWithCapture(
        TEXT("geometry.boolean_trim"), OrderPayload, OrderCapture);
    TestTrue(TEXT("geometry.boolean_trim handler is registered"), bTrimHandlerFound);
    TestFalse(TEXT("boolean_trim rejects a missing target"), OrderCapture.bSuccess);
    TestEqual(TEXT("boolean_trim reports target missing before trim ambiguity"),
        OrderCapture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));

    // The advanced spline path must keep the same operand order: a missing source is reported
    // before an ambiguous spline, even though both identifiers are resolved up front.
    TSharedPtr<FJsonObject> SplineOrderPayload = MakeShared<FJsonObject>();
    SplineOrderPayload->SetStringField(TEXT("actorName"), TEXT("PWGeometryTarget_missing_source"));
    SplineOrderPayload->SetStringField(TEXT("splineActorName"), SharedLabel);
    FTestResponseCapture SplineOrderCapture;
    const bool bExtrudeHandlerFound = InvokeHandlerWithCapture(
        TEXT("geometry.extrude_along_spline"), SplineOrderPayload, SplineOrderCapture);
    TestTrue(TEXT("geometry.extrude_along_spline handler is registered"), bExtrudeHandlerFound);
    TestFalse(TEXT("extrude_along_spline rejects a missing source"), SplineOrderCapture.bSuccess);
    TestEqual(TEXT("extrude_along_spline reports source missing before spline ambiguity"),
        SplineOrderCapture.ErrorCode, FString(TEXT("ACTOR_NOT_FOUND")));

    const McpActorUtils::FActorResolution ByFirstName =
        GeometryTarget::ResolveMeshActor(World, First->GetName());
    TestTrue(TEXT("exact internal object name resolves"), ByFirstName.IsResolved());
    TestTrue(TEXT("internal object name reaches the actor it names"), ByFirstName.Actor == First);
    TestTrue(TEXT("internal object name uses the object-name tier"),
        ByFirstName.MatchedBy == McpActorUtils::EActorMatchKind::ObjectName);

    const McpActorUtils::FActorResolution BySecondPath =
        GeometryTarget::ResolveMeshActor(World, SecondPath);
    TestTrue(TEXT("exact object path resolves"), BySecondPath.IsResolved());
    TestTrue(TEXT("object path reaches the actor it names"), BySecondPath.Actor == Second);
    TestTrue(TEXT("object path uses the object-path tier"),
        BySecondPath.MatchedBy == McpActorUtils::EActorMatchKind::ObjectPath);

    // The bake handlers omit assetPath by default. Derive that default through the production
    // helper after resolving a full object path, then run the same path validation used by both
    // handlers; the caller's package/object delimiters must never reach the asset-name slot.
    const FString DefaultAssetPath = GeometryUtils::MakeDefaultGeometryAssetPath(BySecondPath.Actor);
    TestFalse(TEXT("object-path default asset path contains no object delimiter"),
        DefaultAssetPath.Contains(TEXT(":")) || DefaultAssetPath.Contains(TEXT(".")));
    TestEqual(TEXT("object-path default asset path passes production validation"),
        SanitizeProjectRelativePath(DefaultAssetPath), DefaultAssetPath);
    return true;
}
