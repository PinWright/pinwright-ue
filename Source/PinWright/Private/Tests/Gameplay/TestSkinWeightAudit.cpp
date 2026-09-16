// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for skeleton.audit_skin_weights.
//
// The suite is built as MATCHED PAIRS and no single test is meaningful alone. An audit that is
// switched off, misconfigured, or reading an empty vertex array reports zero findings - which
// is indistinguishable from clean skinning - so a test that only asserts "a clean mesh passes"
// proves nothing whatsoever. Every defect fixture below has a clean twin that differs in one
// value, and the clean fixture deliberately contains a coincident vertex PAIR so the seam check
// reports status "pass" rather than "unmeasured": a check that never ran must not be able to
// masquerade as a check that ran and found nothing.
//
// Every test drives the PRODUCTION handler through InvokeHandlerWithCapture rather than calling
// the analysis helpers directly. A helper-level test keeps passing after the handler stops
// calling the helper, which is how a shipped opacity test in this codebase kept passing with its
// production call site deleted.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Tests/TestUtils.h"

#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "GPUSkinPublicDefs.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkeletalMeshTypes.h"
#include "UObject/Package.h"

#include <initializer_list>

namespace
{
    // Component-space joint positions the fixture skeleton is built from. Chosen so the
    // non-degenerate bone segments are 60, 80 and ~51 units long, giving a median of exactly
    // 60 - the reach normalizer. A hand vertex is then 158 units from the foot joint, i.e.
    // 2.64 median bone lengths, comfortably either side of a 1.0 threshold.
    const FVector SkinAuditRootPos(0.0, 0.0, 0.0);
    const FVector SkinAuditSpineLocal(0.0, 0.0, 60.0);
    const FVector SkinAuditHandLocal(50.0, 0.0, 10.0);
    const FVector SkinAuditFootLocal(0.0, 0.0, -80.0);

    const FVector3f SkinAuditHandPos(50.0f, 0.0f, 70.0f);
    const FVector3f SkinAuditSpinePos(0.0f, 0.0f, 60.0f);

    enum class ESkinAuditBone : int32
    {
        Root = 0,
        Spine = 1,
        Hand = 2,
        Foot = 3,
    };

    // A section-LOCAL influence: LocalBone is a slot into Section.BoneMap, exactly as the
    // importer writes it, which is the whole point of the BoneMap-resolution test below. A
    // plain aggregate rather than TPair so the braced fixture literals below are unambiguous.
    struct FSkinAuditInfluence
    {
        int32 LocalBone;
        float Weight;
    };

    void SetSkinAuditVertex(FSkelMeshSection& Section, int32 VertexIndex, const FVector3f& Position,
        std::initializer_list<FSkinAuditInfluence> LocalBoneWeights)
    {
        FSoftSkinVertex& Vertex = Section.SoftVertices[VertexIndex];
        FMemory::Memzero(Vertex.InfluenceBones, sizeof(Vertex.InfluenceBones));
        FMemory::Memzero(Vertex.InfluenceWeights, sizeof(Vertex.InfluenceWeights));
        Vertex.Position = Position;

        int32 Slot = 0;
        for (const FSkinAuditInfluence& Influence : LocalBoneWeights)
        {
            if (Slot >= MAX_TOTAL_INFLUENCES)
            {
                break;
            }
            Vertex.InfluenceBones[Slot] = static_cast<uint16>(Influence.LocalBone);
            Vertex.InfluenceWeights[Slot] =
                static_cast<uint16>(FMath::RoundToInt(FMath::Clamp(Influence.Weight, 0.0f, 1.0f) * 65535.0f));
            ++Slot;
        }
    }

    // A real on-disk package, because the handler resolves its mesh through StaticLoadObject
    // and a bare transient object would not satisfy that. GUID-suffixed so repeat and parallel
    // runs never collide. Returns the mesh path; the caller must CleanupTestAsset(OutPackagePath).
    USkeletalMesh* MakeSkinAuditMesh(FAutomationTestBase& Test, FString& OutPackagePath, FString& OutMeshPath)
    {
        OutPackagePath = FString::Printf(TEXT("/Game/PinWrightTest_SkinAudit_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        const FString AssetName = FPackageName::GetLongPackageAssetName(OutPackagePath);

        UPackage* Package = CreatePackage(*OutPackagePath);
        Test.TestNotNull(TEXT("test package created"), Package);
        if (!Package)
        {
            return nullptr;
        }

        USkeleton* Skeleton = NewObject<USkeleton>(Package, *(AssetName + TEXT("_Skeleton")),
            RF_Public | RF_Standalone | RF_Transactional);
        USkeletalMesh* Mesh = NewObject<USkeletalMesh>(Package, *AssetName,
            RF_Public | RF_Standalone | RF_Transactional);
        if (!Skeleton || !Mesh)
        {
            return nullptr;
        }

        {
            FReferenceSkeletonModifier Modifier(Skeleton);
            Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE),
                FTransform(SkinAuditRootPos), true);
            Modifier.Add(FMeshBoneInfo(FName(TEXT("spine_01")), TEXT("spine_01"), 0),
                FTransform(SkinAuditSpineLocal));
            Modifier.Add(FMeshBoneInfo(FName(TEXT("hand_l")), TEXT("hand_l"), 1),
                FTransform(SkinAuditHandLocal));
            Modifier.Add(FMeshBoneInfo(FName(TEXT("foot_l")), TEXT("foot_l"), 0),
                FTransform(SkinAuditFootLocal));
        }

        Mesh->SetSkeleton(Skeleton);
        {
            // Mirrored onto the mesh's OWN reference skeleton: the audit resolves bone names
            // through USkeletalMesh::GetRefSkeleton(), not through the USkeleton.
            FReferenceSkeletonModifier MeshModifier(Mesh->GetRefSkeleton(), Skeleton);
            MeshModifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE),
                FTransform(SkinAuditRootPos), true);
            MeshModifier.Add(FMeshBoneInfo(FName(TEXT("spine_01")), TEXT("spine_01"), 0),
                FTransform(SkinAuditSpineLocal));
            MeshModifier.Add(FMeshBoneInfo(FName(TEXT("hand_l")), TEXT("hand_l"), 1),
                FTransform(SkinAuditHandLocal));
            MeshModifier.Add(FMeshBoneInfo(FName(TEXT("foot_l")), TEXT("foot_l"), 0),
                FTransform(SkinAuditFootLocal));
        }

        Mesh->AddLODInfo();
        OutMeshPath = Mesh->GetPathName();
        return Mesh;
    }

    // Appends one LOD carrying a single section with an identity bone map over the four
    // fixture bones, sized for VertexCount vertices. Callers fill the vertices in.
    FSkelMeshSection& AddSkinAuditLod(USkeletalMesh& Mesh, int32 VertexCount)
    {
        FSkeletalMeshLODModel* LODModel = new FSkeletalMeshLODModel();
        FSkelMeshSection& Section = LODModel->Sections.AddDefaulted_GetRef();
        Section.BoneMap = { 0, 1, 2, 3 };
        Section.SoftVertices.SetNum(VertexCount);
        Section.NumVertices = VertexCount;
        Section.BaseVertexIndex = 0;
        LODModel->NumVertices = VertexCount;
        Mesh.GetImportedModel()->LODModels.Add(LODModel);
        return Mesh.GetImportedModel()->LODModels[0].Sections[0];
    }

    TSharedPtr<FJsonObject> MakeAuditPayload(const FString& MeshPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), MeshPath);
        return Payload;
    }

    // The named check object out of the response's checks[] array, or nullptr.
    const TSharedPtr<FJsonObject>* FindAuditCheck(const TSharedPtr<FJsonObject>& Result, const TCHAR* Name)
    {
        const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("checks"), Checks) || !Checks)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Checks)
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            FString CheckName;
            if (Value.IsValid() && Value->TryGetObject(Object) && Object && (*Object).IsValid()
                && (*Object)->TryGetStringField(TEXT("name"), CheckName) && CheckName == Name)
            {
                return Object;
            }
        }
        return nullptr;
    }

    FString AuditCheckStatus(const TSharedPtr<FJsonObject>& Result, const TCHAR* Name)
    {
        const TSharedPtr<FJsonObject>* Check = FindAuditCheck(Result, Name);
        FString Status;
        if (Check && (*Check).IsValid())
        {
            (*Check)->TryGetStringField(TEXT("status"), Status);
        }
        return Status;
    }

    bool RunSkinAudit(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Payload,
        FTestResponseCapture& Capture)
    {
        const bool bFound = InvokeHandlerWithCapture(TEXT("skeleton.audit_skin_weights"), Payload, Capture);
        Test.TestTrue(TEXT("skeleton.audit_skin_weights is registered"), bFound);
        Test.TestTrue(TEXT("handler responded"), Capture.bWasCalled);
        return bFound && Capture.bWasCalled;
    }

    // Builds a fresh fixture mesh + LOD, lets Fill write the vertices, lets Configure add any
    // extra arguments, runs the audit and tears the package down. The mesh dies with the call;
    // the captured JSON outlives it, which is all any assertion below needs. Extracted because
    // the coverage and reach tests below each need the SAME fixture under two different
    // argument sets, and a matched pair that shares no fixture proves nothing.
    bool RunSkinAuditOnFixture(FAutomationTestBase& Test, int32 VertexCount,
        TFunctionRef<void(FSkelMeshSection&)> Fill,
        TFunctionRef<void(const TSharedPtr<FJsonObject>&)> Configure,
        FTestResponseCapture& OutCapture)
    {
        FString PackagePath;
        FString MeshPath;
        USkeletalMesh* Mesh = MakeSkinAuditMesh(Test, PackagePath, MeshPath);
        if (!Mesh)
        {
            return false;
        }
        Mesh->AddToRoot();
        ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

        FSkelMeshSection& Section = AddSkinAuditLod(*Mesh, VertexCount);
        Fill(Section);

        const TSharedPtr<FJsonObject> Payload = MakeAuditPayload(MeshPath);
        Configure(Payload);
        if (!RunSkinAudit(Test, Payload, OutCapture))
        {
            return false;
        }
        Test.TestTrue(FString::Printf(TEXT("audit succeeded (errorCode='%s')"), *OutCapture.ErrorCode),
            OutCapture.bSuccess);
        return OutCapture.bSuccess && OutCapture.Result.IsValid();
    }

    void SkinAuditNoExtraArgs(const TSharedPtr<FJsonObject>&)
    {
    }

    // One row out of boneCoverage's bones[] array, or nullptr when that bone owns no geometry.
    const TSharedPtr<FJsonObject>* FindSkinAuditBoneRow(const TSharedPtr<FJsonObject>* Check,
        const TCHAR* BoneName)
    {
        const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
        if (!Check || !(*Check).IsValid() || !(*Check)->TryGetArrayField(TEXT("bones"), Bones) || !Bones)
        {
            return nullptr;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Bones)
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            FString Name;
            if (Value.IsValid() && Value->TryGetObject(Object) && Object && (*Object).IsValid()
                && (*Object)->TryGetStringField(TEXT("bone"), Name) && Name == BoneName)
            {
                return Object;
            }
        }
        return nullptr;
    }

    // A numeric field off one bones[] row. Returns INDEX_NONE when the row or the field is
    // missing, so an absent row fails an equality assertion instead of silently reading 0 -
    // which is the value most of these assertions are testing FOR.
    int32 SkinAuditBoneRowValue(const TSharedPtr<FJsonObject>* Check, const TCHAR* BoneName,
        const TCHAR* Field)
    {
        const TSharedPtr<FJsonObject>* Row = FindSkinAuditBoneRow(Check, BoneName);
        double Value = 0.0;
        if (!Row || !(*Row).IsValid() || !(*Row)->TryGetNumberField(Field, Value))
        {
            return INDEX_NONE;
        }
        return static_cast<int32>(Value);
    }

    // ---- Shared fixture fills -----------------------------------------------------------
    //
    // Every one of these writes a CLEAN coincident pair, because a fixture whose seam check
    // reports "unmeasured" fails the top-level pass for a reason that has nothing to do with
    // what the test is measuring.

    // Two blended vertices at one point plus one vertex rigidly bound to hand_l. foot_l is
    // deliberately left with nothing weighted to it - the invisible-bone defect.
    void FillSkinAuditFootUnweighted(FSkelMeshSection& Section)
    {
        SetSkinAuditVertex(Section, 0, FVector3f(0.0f, 0.0f, 30.0f),
            { { static_cast<int32>(ESkinAuditBone::Spine), 0.75f },
              { static_cast<int32>(ESkinAuditBone::Root), 0.25f } });
        SetSkinAuditVertex(Section, 1, FVector3f(0.0f, 0.0f, 30.0f),
            { { static_cast<int32>(ESkinAuditBone::Spine), 0.75f },
              { static_cast<int32>(ESkinAuditBone::Root), 0.25f } });
        SetSkinAuditVertex(Section, 2, SkinAuditHandPos,
            { { static_cast<int32>(ESkinAuditBone::Hand), 1.0f } });
    }

    // The same shape with foot_l given geometry of its own, so every bone in the reference
    // skeleton is weighted to something. The twin of the fixture above.
    void FillSkinAuditEveryBoneWeighted(FSkelMeshSection& Section)
    {
        FillSkinAuditFootUnweighted(Section);
        SetSkinAuditVertex(Section, 3, FVector3f(0.0f, 0.0f, -80.0f),
            { { static_cast<int32>(ESkinAuditBone::Foot), 1.0f } });
    }
}

// ---------------------------------------------------------------------------------------
// The clean twin. Its coincident PAIR is load-bearing: without it the seam check would report
// status "unmeasured", and a suite that only ever saw "unmeasured" there would never notice
// the check silently doing nothing.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonAuditSkinWeightsCleanMeshPassesTest,
    "PinWright.skeleton.audit_skin_weights.CleanMeshPasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonAuditSkinWeightsCleanMeshPassesTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    FString MeshPath;
    USkeletalMesh* Mesh = MakeSkinAuditMesh(*this, PackagePath, MeshPath);
    if (!Mesh)
    {
        return false;
    }
    Mesh->AddToRoot();
    ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

    FSkelMeshSection& Section = AddSkinAuditLod(*Mesh, 4);
    SetSkinAuditVertex(Section, 0, SkinAuditSpinePos, { { static_cast<int32>(ESkinAuditBone::Spine), 1.0f } });
    SetSkinAuditVertex(Section, 1, SkinAuditHandPos, { { static_cast<int32>(ESkinAuditBone::Hand), 1.0f } });
    // A UV/material seam duplicate: same bind position, IDENTICAL influences. This is the
    // correct authoring and must read as a measured pass, not as "nothing to check".
    SetSkinAuditVertex(Section, 2, SkinAuditHandPos, { { static_cast<int32>(ESkinAuditBone::Hand), 1.0f } });
    // A blended vertex at a position nothing else occupies, so its two-bone falloff is not
    // mistaken for a seam disagreement with vertex 0.
    SetSkinAuditVertex(Section, 3, FVector3f(0.0f, 0.0f, 30.0f),
        { { static_cast<int32>(ESkinAuditBone::Spine), 0.75f }, { static_cast<int32>(ESkinAuditBone::Root), 0.25f } });

    FTestResponseCapture Capture;
    if (!RunSkinAudit(*this, MakeAuditPayload(MeshPath), Capture))
    {
        return false;
    }
    TestTrue(FString::Printf(TEXT("audit succeeded (errorCode='%s')"), *Capture.ErrorCode), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bPass = false;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestTrue(TEXT("a correctly skinned mesh passes"), bPass);

    TestEqual(TEXT("every vertex was read"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("vertexCount"))), 4);
    TestEqual(TEXT("zero-influence check ran and passed"),
        AuditCheckStatus(Capture.Result, TEXT("zeroInfluence")), FString(TEXT("pass")));
    TestEqual(TEXT("weight-sum check ran and passed"),
        AuditCheckStatus(Capture.Result, TEXT("weightSum")), FString(TEXT("pass")));
    TestEqual(TEXT("influence-count check ran and passed"),
        AuditCheckStatus(Capture.Result, TEXT("influenceCount")), FString(TEXT("pass")));
    // The assertion that stops "unmeasured" from passing for "clean".
    TestEqual(TEXT("seam check MEASURED the coincident pair rather than reporting unmeasured"),
        AuditCheckStatus(Capture.Result, TEXT("coincidentSplit")), FString(TEXT("pass")));
    // Reach carries no default threshold, so it reports and does not vote.
    TestEqual(TEXT("reach reports without a verdict when no threshold is supplied"),
        AuditCheckStatus(Capture.Result, TEXT("influenceReach")), FString(TEXT("reported")));
    TestEqual(TEXT("nothing was left unmeasured"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("unmeasuredChecks"))), 0);

    return true;
}

// ---------------------------------------------------------------------------------------
// Defect twin 1: a vertex with no influence at all. It renders at the component origin.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonAuditSkinWeightsZeroInfluenceFailsTest,
    "PinWright.skeleton.audit_skin_weights.ZeroInfluenceFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonAuditSkinWeightsZeroInfluenceFailsTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    FString MeshPath;
    USkeletalMesh* Mesh = MakeSkinAuditMesh(*this, PackagePath, MeshPath);
    if (!Mesh)
    {
        return false;
    }
    Mesh->AddToRoot();
    ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

    FSkelMeshSection& Section = AddSkinAuditLod(*Mesh, 4);
    SetSkinAuditVertex(Section, 0, SkinAuditSpinePos, { { static_cast<int32>(ESkinAuditBone::Spine), 1.0f } });
    // A sound seam pair, so the other checks stay MEASURED and the run differs from the clean
    // twin in exactly one respect.
    SetSkinAuditVertex(Section, 1, SkinAuditHandPos, { { static_cast<int32>(ESkinAuditBone::Hand), 1.0f } });
    SetSkinAuditVertex(Section, 2, SkinAuditHandPos, { { static_cast<int32>(ESkinAuditBone::Hand), 1.0f } });
    // No influences whatsoever - the zero-fill signature. Placed where nothing else sits so it
    // cannot also trip the seam check.
    SetSkinAuditVertex(Section, 3, FVector3f(0.0f, 0.0f, 30.0f), {});

    FTestResponseCapture Capture;
    if (!RunSkinAudit(*this, MakeAuditPayload(MeshPath), Capture) || !Capture.bSuccess
        || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("a mesh with an unskinned vertex must not pass"), bPass);
    TestEqual(TEXT("zero-influence check failed"),
        AuditCheckStatus(Capture.Result, TEXT("zeroInfluence")), FString(TEXT("fail")));

    const TSharedPtr<FJsonObject>* Check = FindAuditCheck(Capture.Result, TEXT("zeroInfluence"));
    if (Check && (*Check).IsValid())
    {
        TestEqual(TEXT("exactly one unskinned vertex counted"),
            static_cast<int32>((*Check)->GetNumberField(TEXT("zeroInfluenceVertices"))), 1);
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Defect twin 2: influences that do not sum to 1. A half-weighted vertex collapses halfway
// toward the component origin under every pose.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonAuditSkinWeightsUnnormalizedFailsTest,
    "PinWright.skeleton.audit_skin_weights.UnnormalizedWeightsFail",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonAuditSkinWeightsUnnormalizedFailsTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    FString MeshPath;
    USkeletalMesh* Mesh = MakeSkinAuditMesh(*this, PackagePath, MeshPath);
    if (!Mesh)
    {
        return false;
    }
    Mesh->AddToRoot();
    ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

    FSkelMeshSection& Section = AddSkinAuditLod(*Mesh, 3);
    // A sound seam pair keeps the seam check MEASURED, so the only thing this run changes
    // relative to the clean twin is one vertex's weight sum.
    SetSkinAuditVertex(Section, 0, SkinAuditSpinePos, { { static_cast<int32>(ESkinAuditBone::Spine), 1.0f } });
    SetSkinAuditVertex(Section, 1, SkinAuditSpinePos, { { static_cast<int32>(ESkinAuditBone::Spine), 1.0f } });
    SetSkinAuditVertex(Section, 2, SkinAuditHandPos, { { static_cast<int32>(ESkinAuditBone::Hand), 0.5f } });

    FTestResponseCapture Capture;
    if (!RunSkinAudit(*this, MakeAuditPayload(MeshPath), Capture) || !Capture.bSuccess
        || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("a mesh with un-normalized weights must not pass"), bPass);
    TestEqual(TEXT("weight-sum check failed"),
        AuditCheckStatus(Capture.Result, TEXT("weightSum")), FString(TEXT("fail")));

    const TSharedPtr<FJsonObject>* Check = FindAuditCheck(Capture.Result, TEXT("weightSum"));
    if (Check && (*Check).IsValid())
    {
        TestEqual(TEXT("exactly one un-normalized vertex counted"),
            static_cast<int32>((*Check)->GetNumberField(TEXT("unnormalizedVertices"))), 1);
        TestTrue(TEXT("the half-weighted sum is reported as the minimum"),
            FMath::IsNearlyEqual((*Check)->GetNumberField(TEXT("minWeightSum")), 0.5, 1.0e-3));
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Defect twin 3: two vertices at one bind position with different influences. The exact clean
// twin of this is vertex 2 in CleanMeshPasses, which differs only in the bone it names.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonAuditSkinWeightsSeamSplitFailsTest,
    "PinWright.skeleton.audit_skin_weights.CoincidentSeamSplitFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonAuditSkinWeightsSeamSplitFailsTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    FString MeshPath;
    USkeletalMesh* Mesh = MakeSkinAuditMesh(*this, PackagePath, MeshPath);
    if (!Mesh)
    {
        return false;
    }
    Mesh->AddToRoot();
    ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

    FSkelMeshSection& Section = AddSkinAuditLod(*Mesh, 3);
    SetSkinAuditVertex(Section, 0, SkinAuditSpinePos, { { static_cast<int32>(ESkinAuditBone::Spine), 1.0f } });
    SetSkinAuditVertex(Section, 1, SkinAuditHandPos, { { static_cast<int32>(ESkinAuditBone::Hand), 1.0f } });
    // Same position as vertex 1, entirely different bone: the seam splits by the full
    // relative travel of hand_l and foot_l, i.e. a split coefficient of 2.0.
    SetSkinAuditVertex(Section, 2, SkinAuditHandPos, { { static_cast<int32>(ESkinAuditBone::Foot), 1.0f } });

    FTestResponseCapture Capture;
    if (!RunSkinAudit(*this, MakeAuditPayload(MeshPath), Capture) || !Capture.bSuccess
        || !Capture.Result.IsValid())
    {
        return false;
    }

    bool bPass = true;
    Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
    TestFalse(TEXT("a splitting seam must not pass"), bPass);
    TestEqual(TEXT("seam check failed"),
        AuditCheckStatus(Capture.Result, TEXT("coincidentSplit")), FString(TEXT("fail")));

    const TSharedPtr<FJsonObject>* Check = FindAuditCheck(Capture.Result, TEXT("coincidentSplit"));
    if (!Check || !(*Check).IsValid())
    {
        AddError(TEXT("coincidentSplit check missing from the response"));
        return false;
    }
    TestEqual(TEXT("exactly one splitting group"),
        static_cast<int32>((*Check)->GetNumberField(TEXT("splittingGroups"))), 1);
    TestTrue(TEXT("split coefficient is the full 2.0 of two disjoint full-weight influences"),
        FMath::IsNearlyEqual((*Check)->GetNumberField(TEXT("maxSplitCoefficient")), 2.0, 1.0e-3));

    // The bones must come back by NAME. A group reported with bare indices is unusable, and
    // an index reported without BoneMap resolution would be the wrong bone entirely.
    const TArray<TSharedPtr<FJsonValue>>* Offenders = nullptr;
    if ((*Check)->TryGetArrayField(TEXT("offenders"), Offenders) && Offenders && Offenders->Num() > 0)
    {
        const TSharedPtr<FJsonObject>* Group = nullptr;
        if ((*Offenders)[0]->TryGetObject(Group) && Group && (*Group).IsValid())
        {
            TestTrue(TEXT("hand_l named among the disagreeing bones"),
                JsonStringArrayContains(*Group, TEXT("disagreeingBones"), TEXT("hand_l")));
            TestTrue(TEXT("foot_l named among the disagreeing bones"),
                JsonStringArrayContains(*Group, TEXT("disagreeingBones"), TEXT("foot_l")));
        }
    }
    else
    {
        AddError(TEXT("splitting group was counted but not reported in offenders"));
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// The BoneMap indirection. Two sections whose bone maps point slot 0 at DIFFERENT real bones.
// An implementation that read InfluenceBones as reference-skeleton indices - the obvious and
// wrong reading, and the one FSkeletalMeshLODModel::GetVertices leaves in place because it
// memcpys the section vertices verbatim - would see both vertices weighted to bone 0 and
// report a clean pass. This test is the only thing standing between that bug and a shipped
// audit that launders real defects as clean.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonAuditSkinWeightsResolvesSectionBoneMapTest,
    "PinWright.skeleton.audit_skin_weights.ResolvesSectionBoneMap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonAuditSkinWeightsResolvesSectionBoneMapTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    FString MeshPath;
    USkeletalMesh* Mesh = MakeSkinAuditMesh(*this, PackagePath, MeshPath);
    if (!Mesh)
    {
        return false;
    }
    Mesh->AddToRoot();
    ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

    FSkeletalMeshLODModel* LODModel = new FSkeletalMeshLODModel();

    // Section 0: local slot 0 -> hand_l.
    FSkelMeshSection& SectionA = LODModel->Sections.AddDefaulted_GetRef();
    SectionA.BoneMap = { static_cast<FBoneIndexType>(ESkinAuditBone::Hand) };
    SectionA.SoftVertices.SetNum(1);
    SectionA.NumVertices = 1;
    SectionA.BaseVertexIndex = 0;
    SetSkinAuditVertex(SectionA, 0, SkinAuditHandPos, { { 0, 1.0f } });

    // Section 1: the SAME local slot 0 -> foot_l, at the same bind position. Section-local
    // slot numbers are identical; the real bones are not.
    FSkelMeshSection& SectionB = LODModel->Sections.AddDefaulted_GetRef();
    SectionB.BoneMap = { static_cast<FBoneIndexType>(ESkinAuditBone::Foot) };
    SectionB.SoftVertices.SetNum(1);
    SectionB.NumVertices = 1;
    SectionB.BaseVertexIndex = 1;
    SetSkinAuditVertex(SectionB, 0, SkinAuditHandPos, { { 0, 1.0f } });

    LODModel->NumVertices = 2;
    Mesh->GetImportedModel()->LODModels.Add(LODModel);

    FTestResponseCapture Capture;
    if (!RunSkinAudit(*this, MakeAuditPayload(MeshPath), Capture) || !Capture.bSuccess
        || !Capture.Result.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("both sections were read"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("sectionCount"))), 2);
    TestEqual(TEXT("cross-section seam split detected, so the bone map WAS resolved"),
        AuditCheckStatus(Capture.Result, TEXT("coincidentSplit")), FString(TEXT("fail")));

    const TSharedPtr<FJsonObject>* Check = FindAuditCheck(Capture.Result, TEXT("coincidentSplit"));
    if (!Check || !(*Check).IsValid())
    {
        AddError(TEXT("coincidentSplit check missing from the response"));
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* Offenders = nullptr;
    if ((*Check)->TryGetArrayField(TEXT("offenders"), Offenders) && Offenders && Offenders->Num() > 0)
    {
        const TSharedPtr<FJsonObject>* Group = nullptr;
        if ((*Offenders)[0]->TryGetObject(Group) && Group && (*Group).IsValid())
        {
            TestTrue(TEXT("section 0's slot 0 resolved to hand_l, not to bone 0"),
                JsonStringArrayContains(*Group, TEXT("disagreeingBones"), TEXT("hand_l")));
            TestTrue(TEXT("section 1's slot 0 resolved to foot_l, not to bone 0"),
                JsonStringArrayContains(*Group, TEXT("disagreeingBones"), TEXT("foot_l")));
            TestFalse(TEXT("the unresolved slot number never leaks through as the root bone"),
                JsonStringArrayContains(*Group, TEXT("disagreeingBones"), TEXT("root")));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Reach: reported by default, gated only on request. The pair is the point - the same fixture
// must produce "reported" with no verdict and "fail" once a threshold is supplied, which is
// what proves the absent threshold is a deliberate abstention rather than a silent pass.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonAuditSkinWeightsReachGatesOnlyOnRequestTest,
    "PinWright.skeleton.audit_skin_weights.ReachGatesOnlyOnRequest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonAuditSkinWeightsReachGatesOnlyOnRequestTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    FString MeshPath;
    USkeletalMesh* Mesh = MakeSkinAuditMesh(*this, PackagePath, MeshPath);
    if (!Mesh)
    {
        return false;
    }
    Mesh->AddToRoot();
    ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

    FSkelMeshSection& Section = AddSkinAuditLod(*Mesh, 3);
    // A sound seam pair, so every other check is measured and passes. The two runs below then
    // differ in exactly one input: the presence of maxReachDistance.
    SetSkinAuditVertex(Section, 0, SkinAuditSpinePos, { { static_cast<int32>(ESkinAuditBone::Spine), 1.0f } });
    SetSkinAuditVertex(Section, 1, SkinAuditSpinePos, { { static_cast<int32>(ESkinAuditBone::Spine), 1.0f } });
    // A hand vertex fully weighted to the FOOT: 158 units from the foot joint against a
    // 60-unit median bone length, i.e. 2.64 normalized. This is the implausible-influence
    // defect stated as a number.
    SetSkinAuditVertex(Section, 2, SkinAuditHandPos, { { static_cast<int32>(ESkinAuditBone::Foot), 1.0f } });

    {
        FTestResponseCapture Capture;
        if (!RunSkinAudit(*this, MakeAuditPayload(MeshPath), Capture) || !Capture.bSuccess
            || !Capture.Result.IsValid())
        {
            return false;
        }
        TestEqual(TEXT("without a threshold, reach reports and does not judge"),
            AuditCheckStatus(Capture.Result, TEXT("influenceReach")), FString(TEXT("reported")));
        bool bPass = false;
        Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
        TestTrue(TEXT("an ungated reach measurement does not fail the audit on its own"), bPass);

        const TSharedPtr<FJsonObject>* Check = FindAuditCheck(Capture.Result, TEXT("influenceReach"));
        if (!Check || !(*Check).IsValid())
        {
            AddError(TEXT("influenceReach check missing from the response"));
            return false;
        }
        TestTrue(TEXT("the reach pass looked at every vertex"),
            (*Check)->GetBoolField(TEXT("exhaustive")));
        TestTrue(TEXT("median bone length measured as 60"),
            FMath::IsNearlyEqual((*Check)->GetNumberField(TEXT("medianBoneLength")), 60.0, 0.5));

        const TSharedPtr<FJsonObject>* Percentiles = nullptr;
        if ((*Check)->TryGetObjectField(TEXT("normalizedDistance"), Percentiles) && Percentiles)
        {
            TestTrue(TEXT("the foot-on-hand influence measures ~2.6 median bone lengths"),
                FMath::IsNearlyEqual((*Percentiles)->GetNumberField(TEXT("max")), 2.635, 0.05));
        }
        else
        {
            AddError(TEXT("reach percentiles missing"));
        }
    }

    {
        TSharedPtr<FJsonObject> Payload = MakeAuditPayload(MeshPath);
        Payload->SetNumberField(TEXT("maxReachDistance"), 1.0);
        FTestResponseCapture Capture;
        if (!RunSkinAudit(*this, Payload, Capture) || !Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }
        TestEqual(TEXT("with a threshold, the same measurement becomes a verdict"),
            AuditCheckStatus(Capture.Result, TEXT("influenceReach")), FString(TEXT("fail")));
        bool bPass = true;
        Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
        TestFalse(TEXT("the gated run does not pass"), bPass);
    }

    return true;
}

// ---------------------------------------------------------------------------------------
// A mesh with nothing to read must ERROR, not pass. This is the single most important
// behaviour in the verb: a cooked-only or not-yet-built mesh has no imported model, and an
// audit that answered pass:true with zero findings there would be a licence to ship anything.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonAuditSkinWeightsUnreadableMeshErrorsTest,
    "PinWright.skeleton.audit_skin_weights.UnreadableMeshErrorsRatherThanPasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonAuditSkinWeightsUnreadableMeshErrorsTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    FString MeshPath;
    USkeletalMesh* Mesh = MakeSkinAuditMesh(*this, PackagePath, MeshPath);
    if (!Mesh)
    {
        return false;
    }
    Mesh->AddToRoot();
    ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

    // Deliberately no LOD model at all.
    FTestResponseCapture Capture;
    if (!RunSkinAudit(*this, MakeAuditPayload(MeshPath), Capture))
    {
        return false;
    }
    TestFalse(TEXT("an unreadable mesh must not report success"), Capture.bSuccess);
    TestEqual(TEXT("and it must say why"), Capture.ErrorCode, FString(TEXT("NO_LOD_MODELS")));
    return true;
}

// ---------------------------------------------------------------------------------------
// A skeleton path is a real asset, but it is not an audit target. The handler must distinguish
// that from a missing path and make the mesh-only recovery actionable through the skeleton's
// preview slot.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonAuditSkinWeightsRejectsSkeletonPathTest,
    "PinWright.skeleton.audit_skin_weights.RejectsSkeletonPathWithPreviewRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonAuditSkinWeightsRejectsSkeletonPathTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    FString MeshPath;
    USkeletalMesh* Mesh = MakeSkinAuditMesh(*this, PackagePath, MeshPath);
    if (!Mesh)
    {
        return false;
    }
    USkeleton* Skeleton = Mesh->GetSkeleton();
    if (!TestNotNull(TEXT("fixture skeleton created"), Skeleton))
    {
        return false;
    }

    Mesh->AddToRoot();
    Skeleton->AddToRoot();
    ON_SCOPE_EXIT
    {
        Skeleton->RemoveFromRoot();
        Mesh->RemoveFromRoot();
        CleanupTestAsset(PackagePath);
    };

    {
        FTestResponseCapture Capture;
        if (!RunSkinAudit(*this, MakeAuditPayload(Skeleton->GetPathName()), Capture))
        {
            return false;
        }
        TestFalse(TEXT("a USkeleton path is rejected, not audited"), Capture.bSuccess);
        TestEqual(TEXT("a present USkeleton reports INVALID_ASSET_TYPE"), Capture.ErrorCode,
            FString(TEXT("INVALID_ASSET_TYPE")));
        TestTrue(TEXT("the error names the actual USkeleton type"), Capture.Message.Contains(TEXT("USkeleton")));
        TestTrue(TEXT("the error reports that no preview mesh is configured"),
            Capture.Message.Contains(TEXT("(none)")));
        TestTrue(TEXT("the error tells the caller to configure a preview mesh first"),
            Capture.Message.Contains(TEXT("configure a preview mesh first")));
        TestFalse(TEXT("the skeleton path is not mislabeled as MESH_NOT_FOUND"),
            Capture.ErrorCode == TEXT("MESH_NOT_FOUND"));
        TestFalse(TEXT("a rejected skeleton path produces no audit result"), Capture.Result.IsValid());
    }

    Skeleton->SetPreviewMesh(Mesh, /*bMarkAsDirty=*/false);
    {
        FTestResponseCapture Capture;
        if (!RunSkinAudit(*this, MakeAuditPayload(Skeleton->GetPathName()), Capture))
        {
            return false;
        }
        TestFalse(TEXT("a USkeleton with a preview mesh is still rejected as an audit target"), Capture.bSuccess);
        TestEqual(TEXT("the configured-preview rejection is typed"), Capture.ErrorCode,
            FString(TEXT("INVALID_ASSET_TYPE")));
        TestTrue(TEXT("the configured preview mesh path is included in the error"),
            Capture.Message.Contains(Mesh->GetPathName()));
        TestTrue(TEXT("the error tells the caller to retry with the preview mesh path"),
            Capture.Message.Contains(TEXT("Retry with skeletalMeshPath")));
        TestFalse(TEXT("the configured-preview rejection produces no audit result"), Capture.Result.IsValid());
    }

    {
        FTestResponseCapture Capture;
        if (!RunSkinAudit(*this, MakeAuditPayload(MeshPath + TEXT("_missing")), Capture))
        {
            return false;
        }
        TestFalse(TEXT("an absent asset path is rejected"), Capture.bSuccess);
        TestEqual(TEXT("an absent asset path remains MESH_NOT_FOUND"), Capture.ErrorCode,
            FString(TEXT("MESH_NOT_FOUND")));
    }

    return true;
}

// Bone coverage: the bones nothing is weighted to, named.
//
// This is the defect that makes a hand-authored animation move nothing - the curve drives a
// bone no vertex is weighted to - and it is invisible to every other check in this verb: the
// weights sum, nothing is zero-filled, no seam splits. A matched pair, because a list that is
// always empty and a list that is always full both look correct against a single fixture: the
// twins differ in exactly one vertex, and the assertion is that the list follows it.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonAuditSkinWeightsBoneCoverageNamesUninfluencedBonesTest,
    "PinWright.skeleton.audit_skin_weights.BoneCoverageNamesUninfluencedBones",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonAuditSkinWeightsBoneCoverageNamesUninfluencedBonesTest::RunTest(const FString& Parameters)
{
    {
        FTestResponseCapture Capture;
        if (!RunSkinAuditOnFixture(*this, 4, &FillSkinAuditEveryBoneWeighted, &SkinAuditNoExtraArgs, Capture))
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* Check = FindAuditCheck(Capture.Result, TEXT("boneCoverage"));
        if (!Check || !(*Check).IsValid())
        {
            AddError(TEXT("boneCoverage check missing from the response"));
            return false;
        }
        TestEqual(TEXT("coverage was measured, not skipped"),
            AuditCheckStatus(Capture.Result, TEXT("boneCoverage")), FString(TEXT("reported")));

        // The load-bearing half: the field is PRESENT and empty. An absent array cannot be
        // told apart from a build that never looked for uninfluenced bones.
        const TArray<TSharedPtr<FJsonValue>>* Uninfluenced = nullptr;
        const bool bPresent = (*Check)->TryGetArrayField(TEXT("bonesWithNoInfluence"), Uninfluenced);
        TestTrue(TEXT("bonesWithNoInfluence is emitted even when it is empty"), bPresent);
        if (bPresent && Uninfluenced)
        {
            TestEqual(TEXT("and it is empty when every bone owns geometry"), Uninfluenced->Num(), 0);
        }
        TestEqual(TEXT("the count agrees with the array"),
            static_cast<int32>((*Check)->GetNumberField(TEXT("bonesWithNoInfluenceCount"))), 0);
        TestEqual(TEXT("all four fixture bones are influenced"),
            static_cast<int32>((*Check)->GetNumberField(TEXT("influencedBoneCount"))), 4);
        TestEqual(TEXT("against a four-bone reference skeleton"),
            static_cast<int32>((*Check)->GetNumberField(TEXT("boneCount"))), 4);
    }

    {
        // The twin: identical but for the one vertex that gave foot_l its geometry.
        FTestResponseCapture Capture;
        if (!RunSkinAuditOnFixture(*this, 3, &FillSkinAuditFootUnweighted, &SkinAuditNoExtraArgs, Capture))
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* Check = FindAuditCheck(Capture.Result, TEXT("boneCoverage"));
        if (!Check || !(*Check).IsValid())
        {
            AddError(TEXT("boneCoverage check missing from the response"));
            return false;
        }
        TestEqual(TEXT("exactly one bone owns no geometry"),
            static_cast<int32>((*Check)->GetNumberField(TEXT("bonesWithNoInfluenceCount"))), 1);
        TestTrue(TEXT("and it is named, not merely counted"),
            JsonStringArrayContains(*Check, TEXT("bonesWithNoInfluence"), TEXT("foot_l")));
        // The other direction: a bone that DOES own geometry must never be listed. A list that
        // named every bone would satisfy the assertion above and be useless.
        TestFalse(TEXT("a bone with vertices weighted to it is not listed"),
            JsonStringArrayContains(*Check, TEXT("bonesWithNoInfluence"), TEXT("hand_l")));
        TestFalse(TEXT("nor is the blended spine bone"),
            JsonStringArrayContains(*Check, TEXT("bonesWithNoInfluence"), TEXT("spine_01")));
        TestEqual(TEXT("three of four bones are influenced"),
            static_cast<int32>((*Check)->GetNumberField(TEXT("influencedBoneCount"))), 3);
    }

    return true;
}

// ---------------------------------------------------------------------------------------
// Coverage reports by default and judges only on request. The pair is the point: the SAME
// fixture must produce "reported" with pass:true and "fail" with pass:false, which is what
// proves the default abstention is deliberate rather than a check that cannot fire.
//
// Why it abstains: real skeletons carry bones no vertex is ever weighted to on purpose - IK
// targets, attachment bones, twist drivers, a root that only carries motion. Failing on their
// existence would manufacture exactly the false-alarm class this work removed from reach.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonAuditSkinWeightsBoneCoverageGatesOnlyOnRequestTest,
    "PinWright.skeleton.audit_skin_weights.BoneCoverageGatesOnlyOnRequest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonAuditSkinWeightsBoneCoverageGatesOnlyOnRequestTest::RunTest(const FString& Parameters)
{
    const auto RequireAllBones = [](const TSharedPtr<FJsonObject>& Payload)
    {
        Payload->SetBoolField(TEXT("requireAllBonesInfluenced"), true);
    };

    {
        FTestResponseCapture Capture;
        if (!RunSkinAuditOnFixture(*this, 3, &FillSkinAuditFootUnweighted, &SkinAuditNoExtraArgs, Capture))
        {
            return false;
        }
        TestEqual(TEXT("without the flag, coverage reports and does not judge"),
            AuditCheckStatus(Capture.Result, TEXT("boneCoverage")), FString(TEXT("reported")));
        bool bPass = false;
        Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
        TestTrue(TEXT("an uninfluenced bone does not fail the audit on its own"), bPass);
    }

    {
        FTestResponseCapture Capture;
        if (!RunSkinAuditOnFixture(*this, 3, &FillSkinAuditFootUnweighted, RequireAllBones, Capture))
        {
            return false;
        }
        TestEqual(TEXT("with the flag, the same measurement becomes a verdict"),
            AuditCheckStatus(Capture.Result, TEXT("boneCoverage")), FString(TEXT("fail")));
        bool bPass = true;
        Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
        TestFalse(TEXT("and the gated run does not pass"), bPass);
    }

    // The other direction for the gate itself: with every bone influenced, asking for the gate
    // must PASS. A gate wired to fail unconditionally would satisfy the assertion above.
    {
        FTestResponseCapture Capture;
        if (!RunSkinAuditOnFixture(*this, 4, &FillSkinAuditEveryBoneWeighted, RequireAllBones, Capture))
        {
            return false;
        }
        TestEqual(TEXT("a fully covered skeleton passes the gate rather than failing it"),
            AuditCheckStatus(Capture.Result, TEXT("boneCoverage")), FString(TEXT("pass")));
        bool bPass = false;
        Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
        TestTrue(TEXT("and the audit passes overall"), bPass);
    }

    return true;
}

// ---------------------------------------------------------------------------------------
// Rigid vertices, counted PER BONE. A single total ("four rigid vertices") cannot separate one
// part bound outright to one bone from four vertices scattered one-each across four bones, and
// those are opposite facts about a rig. So the assertions below are all per bone, and the ones
// that matter are the ZEROES: a blended vertex must contribute no rigid count to either of the
// bones it names, and a sub-epsilon influence must neither make its bone "influenced" nor cost
// the vertex its rigidity.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonAuditSkinWeightsRigidVerticesCountedPerBoneTest,
    "PinWright.skeleton.audit_skin_weights.RigidVerticesAreCountedPerBone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonAuditSkinWeightsRigidVerticesCountedPerBoneTest::RunTest(const FString& Parameters)
{
    const auto FillRigidPart = [](FSkelMeshSection& Section)
    {
        // A four-vertex part bound outright to hand_l. Vertices 0 and 1 are a clean coincident
        // pair, so the seam check stays measured.
        SetSkinAuditVertex(Section, 0, SkinAuditHandPos,
            { { static_cast<int32>(ESkinAuditBone::Hand), 1.0f } });
        SetSkinAuditVertex(Section, 1, SkinAuditHandPos,
            { { static_cast<int32>(ESkinAuditBone::Hand), 1.0f } });
        SetSkinAuditVertex(Section, 2, FVector3f(60.0f, 0.0f, 70.0f),
            { { static_cast<int32>(ESkinAuditBone::Hand), 1.0f } });
        // Still rigid: the foot influence is BELOW minInfluenceWeight (0.01), so it moves this
        // vertex by at most half a percent of the foot's travel and is not a second binding.
        SetSkinAuditVertex(Section, 3, FVector3f(70.0f, 0.0f, 70.0f),
            { { static_cast<int32>(ESkinAuditBone::Hand), 0.995f },
              { static_cast<int32>(ESkinAuditBone::Foot), 0.005f } });
        // Genuinely blended between two bones: rigid to neither.
        SetSkinAuditVertex(Section, 4, FVector3f(0.0f, 0.0f, 30.0f),
            { { static_cast<int32>(ESkinAuditBone::Spine), 0.75f },
              { static_cast<int32>(ESkinAuditBone::Root), 0.25f } });
    };

    FTestResponseCapture Capture;
    if (!RunSkinAuditOnFixture(*this, 5, FillRigidPart, &SkinAuditNoExtraArgs, Capture))
    {
        return false;
    }

    const TSharedPtr<FJsonObject>* Check = FindAuditCheck(Capture.Result, TEXT("boneCoverage"));
    if (!Check || !(*Check).IsValid())
    {
        AddError(TEXT("boneCoverage check missing from the response"));
        return false;
    }

    TestEqual(TEXT("four of the five vertices belong to exactly one bone"),
        static_cast<int32>((*Check)->GetNumberField(TEXT("rigidVertexCount"))), 4);
    TestTrue(TEXT("the epsilon the counts used is published"),
        FMath::IsNearlyEqual((*Check)->GetNumberField(TEXT("weightEpsilon")), 0.01, 1.0e-6));

    // The rigid part, readable directly: hand_l owns all four of its vertices outright.
    TestEqual(TEXT("hand_l influences four vertices"),
        SkinAuditBoneRowValue(Check, TEXT("hand_l"), TEXT("influencedVertices")), 4);
    TestEqual(TEXT("and owns all four of them outright, which is what deliberately rigid means"),
        SkinAuditBoneRowValue(Check, TEXT("hand_l"), TEXT("rigidVertices")), 4);

    // The zeroes. An implementation that counted every influence as rigid would report 1 here.
    TestEqual(TEXT("spine_01 influences the blended vertex"),
        SkinAuditBoneRowValue(Check, TEXT("spine_01"), TEXT("influencedVertices")), 1);
    TestEqual(TEXT("but owns none of it: a blended vertex is not rigid"),
        SkinAuditBoneRowValue(Check, TEXT("spine_01"), TEXT("rigidVertices")), 0);
    TestEqual(TEXT("root influences the blended vertex too"),
        SkinAuditBoneRowValue(Check, TEXT("root"), TEXT("influencedVertices")), 1);
    TestEqual(TEXT("and owns none of it either"),
        SkinAuditBoneRowValue(Check, TEXT("root"), TEXT("rigidVertices")), 0);

    // The epsilon, in the failure direction: vertex 3's 0.005 foot influence must not make
    // foot_l an influencing bone. An implementation reading every non-zero slot would give
    // foot_l a row here and drop it from bonesWithNoInfluence.
    TestEqual(TEXT("a sub-epsilon influence does not make its bone an influencing bone"),
        SkinAuditBoneRowValue(Check, TEXT("foot_l"), TEXT("influencedVertices")), static_cast<int32>(INDEX_NONE));
    TestTrue(TEXT("so foot_l is reported as owning no geometry"),
        JsonStringArrayContains(*Check, TEXT("bonesWithNoInfluence"), TEXT("foot_l")));
    TestEqual(TEXT("three bones own geometry"),
        static_cast<int32>((*Check)->GetNumberField(TEXT("influencedBoneCount"))), 3);

    return true;
}

// ---------------------------------------------------------------------------------------
// The reach false alarm, and the defect it must not take with it.
//
// A part bound deliberately and entirely to one bone - a prop, a weapon, an eyeball, a cape on
// one attachment bone - sits at whatever distance its own silhouette puts it from that bone,
// and a leaf bone's segment is a POINT, which makes the number larger still. The reach verdict
// reads the MAXIMUM normalized distance, so every one of those vertices used to fail the audit
// and fill the offender list, crowding out the defect a caller was looking for.
//
// Both halves run here on fixtures that differ by ONE vertex:
//   * a rigid part 350 units (5.83 median bone lengths) from hand_l, with hand_l the nearest
//     bone to it - the intended authoring. Must not fail a 1.0 threshold, and must still be
//     REPORTED, in its own bucket, rather than silently discarded.
//   * the same plus one vertex rigidly bound to foot_l at the hand's position, where three
//     bones are strictly closer - the wrong-bone defect. Must still fail.
// Without the second half this test would be indistinguishable from raising the threshold
// until the alarm stopped, which is the fix that was explicitly not wanted.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkeletonAuditSkinWeightsRigidNearestBindIsNotAReachDefectTest,
    "PinWright.skeleton.audit_skin_weights.RigidNearestBindIsNotAReachDefect",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkeletonAuditSkinWeightsRigidNearestBindIsNotAReachDefectTest::RunTest(const FString& Parameters)
{
    // hand_l sits at (50, 0, 70) and is a leaf, so its segment is that single point. The two
    // prop vertices are 300 and 350 units out along +X from it: 5.0 and 5.83 median bone
    // lengths, and hand_l is the nearest bone to both.
    const auto FillProp = [](FSkelMeshSection& Section)
    {
        SetSkinAuditVertex(Section, 0, FVector3f(0.0f, 0.0f, 30.0f),
            { { static_cast<int32>(ESkinAuditBone::Spine), 0.75f },
              { static_cast<int32>(ESkinAuditBone::Root), 0.25f } });
        SetSkinAuditVertex(Section, 1, FVector3f(0.0f, 0.0f, 30.0f),
            { { static_cast<int32>(ESkinAuditBone::Spine), 0.75f },
              { static_cast<int32>(ESkinAuditBone::Root), 0.25f } });
        SetSkinAuditVertex(Section, 2, FVector3f(350.0f, 0.0f, 70.0f),
            { { static_cast<int32>(ESkinAuditBone::Hand), 1.0f } });
        SetSkinAuditVertex(Section, 3, FVector3f(400.0f, 0.0f, 70.0f),
            { { static_cast<int32>(ESkinAuditBone::Hand), 1.0f } });
    };
    const auto FillPropPlusWrongBone = [&FillProp](FSkelMeshSection& Section)
    {
        FillProp(Section);
        // A hand vertex bound rigidly to the FOOT. Rigid, but three bones are strictly closer,
        // so it is a bind error rather than a rigid part and must stay judged.
        SetSkinAuditVertex(Section, 4, SkinAuditHandPos,
            { { static_cast<int32>(ESkinAuditBone::Foot), 1.0f } });
    };
    const auto GateReachAtOne = [](const TSharedPtr<FJsonObject>& Payload)
    {
        Payload->SetNumberField(TEXT("maxReachDistance"), 1.0);
    };

    {
        FTestResponseCapture Capture;
        if (!RunSkinAuditOnFixture(*this, 4, FillProp, GateReachAtOne, Capture))
        {
            return false;
        }

        TestEqual(TEXT("a rigid part 5.8 bone lengths from its own bone is not a reach defect"),
            AuditCheckStatus(Capture.Result, TEXT("influenceReach")), FString(TEXT("pass")));
        bool bPass = false;
        Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
        TestTrue(TEXT("so the audit passes"), bPass);

        const TSharedPtr<FJsonObject>* Check = FindAuditCheck(Capture.Result, TEXT("influenceReach"));
        if (!Check || !(*Check).IsValid())
        {
            AddError(TEXT("influenceReach check missing from the response"));
            return false;
        }

        const int32 Considered = static_cast<int32>((*Check)->GetNumberField(TEXT("influencesConsidered")));
        const int32 Gated = static_cast<int32>((*Check)->GetNumberField(TEXT("gatedInfluences")));
        const TSharedPtr<FJsonObject>* RigidBind = nullptr;
        if (!(*Check)->TryGetObjectField(TEXT("rigidBind"), RigidBind) || !RigidBind || !(*RigidBind).IsValid())
        {
            AddError(TEXT("influenceReach carries no rigidBind bucket"));
            return false;
        }
        const int32 Rigid = static_cast<int32>((*RigidBind)->GetNumberField(TEXT("influences")));

        TestEqual(TEXT("the two prop influences went to the rigid bucket"), Rigid, 2);
        TestEqual(TEXT("two rigid vertices, counted once each"),
            static_cast<int32>((*RigidBind)->GetNumberField(TEXT("vertices"))), 2);
        TestEqual(TEXT("the four blended influences stayed gated"), Gated, 4);
        // The buckets PARTITION the measurement: an influence that fell out of both would be
        // an exclusion nobody could see.
        TestEqual(TEXT("gated + rigid accounts for every influence considered"),
            Gated + Rigid, Considered);

        // Excluded is not discarded. The rigid bucket carries its own distribution, so a caller
        // can see exactly what the gate declined to judge and disagree with it.
        const TSharedPtr<FJsonObject>* RigidPercentiles = nullptr;
        if ((*RigidBind)->TryGetObjectField(TEXT("normalizedDistance"), RigidPercentiles) && RigidPercentiles)
        {
            TestTrue(TEXT("the excluded reach is still MEASURED and reported at ~5.83 bone lengths"),
                FMath::IsNearlyEqual((*RigidPercentiles)->GetNumberField(TEXT("max")), 5.833, 0.05));
        }
        else
        {
            AddError(TEXT("rigidBind carries no distribution, so the exclusion is invisible"));
        }
        const TArray<TSharedPtr<FJsonValue>>* RigidOffenders = nullptr;
        TestTrue(TEXT("and the excluded influences are listed by name"),
            (*RigidBind)->TryGetArrayField(TEXT("offenders"), RigidOffenders)
                && RigidOffenders && RigidOffenders->Num() == 2);

        // The gated distribution no longer contains the prop at all - the blended vertices top
        // out at 30 units against a 60-unit median.
        const TSharedPtr<FJsonObject>* Percentiles = nullptr;
        if ((*Check)->TryGetObjectField(TEXT("normalizedDistance"), Percentiles) && Percentiles)
        {
            TestTrue(TEXT("the gated distribution tops out at the blended vertices, not the prop"),
                FMath::IsNearlyEqual((*Percentiles)->GetNumberField(TEXT("max")), 0.5, 0.05));
        }
        else
        {
            AddError(TEXT("reach percentiles missing"));
        }
    }

    {
        FTestResponseCapture Capture;
        if (!RunSkinAuditOnFixture(*this, 5, FillPropPlusWrongBone, GateReachAtOne, Capture))
        {
            return false;
        }

        TestEqual(TEXT("a rigid bind to a bone three others are nearer to is STILL a reach defect"),
            AuditCheckStatus(Capture.Result, TEXT("influenceReach")), FString(TEXT("fail")));
        bool bPass = true;
        Capture.Result->TryGetBoolField(TEXT("pass"), bPass);
        TestFalse(TEXT("and the audit does not pass"), bPass);

        const TSharedPtr<FJsonObject>* Check = FindAuditCheck(Capture.Result, TEXT("influenceReach"));
        if (!Check || !(*Check).IsValid())
        {
            AddError(TEXT("influenceReach check missing from the response"));
            return false;
        }
        // The prop is still excused; only the wrong-bone influence was added to the gated set.
        TestEqual(TEXT("the prop is still not judged"),
            static_cast<int32>((*Check)->GetNumberField(TEXT("gatedInfluences"))), 5);
        const TSharedPtr<FJsonObject>* RigidBind = nullptr;
        if ((*Check)->TryGetObjectField(TEXT("rigidBind"), RigidBind) && RigidBind && (*RigidBind).IsValid())
        {
            TestEqual(TEXT("and the rigid bucket is unchanged by the added defect"),
                static_cast<int32>((*RigidBind)->GetNumberField(TEXT("influences"))), 2);
        }

        const TArray<TSharedPtr<FJsonValue>>* Offenders = nullptr;
        if ((*Check)->TryGetArrayField(TEXT("offenders"), Offenders) && Offenders && Offenders->Num() > 0)
        {
            const TSharedPtr<FJsonObject>* Worst = nullptr;
            if ((*Offenders)[0]->TryGetObject(Worst) && Worst && (*Worst).IsValid())
            {
                FString BoneName;
                (*Worst)->TryGetStringField(TEXT("bone"), BoneName);
                TestEqual(TEXT("the worst gated offender is the wrong-bone influence, not the prop"),
                    BoneName, FString(TEXT("foot_l")));
                TestTrue(TEXT("and it is reported with the bones that lie closer to it"),
                    static_cast<int32>((*Worst)->GetNumberField(TEXT("closerBones"))) > 0);
            }
        }
        else
        {
            AddError(TEXT("the reach check failed but named no offender"));
        }
    }

    return true;
}
