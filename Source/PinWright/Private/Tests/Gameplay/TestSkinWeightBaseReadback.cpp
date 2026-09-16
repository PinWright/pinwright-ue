// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the BASE-skinning half of skeleton.describe_skin_weights, and for the bone
// index-space contract the weight mutators now hold.
//
// WHY THESE TESTS LOOK AT THE SECTIONS AND NOT AT THE PROFILE
//
// The defect these guard against was a CLOSED LOOP, not a wrong number. The four weight
// mutators wrote a named alternate skin-weight profile; the readback documented as their
// "verification counterpart" read the same profile; so a write and its check agreed with each
// other while both disagreed with the mesh. Every existing weight test in this suite is
// helper-level (TestAnimationHandlers.cpp:3157, :3290, :3375, :3517, :3614) and therefore
// shares the write path's assumption, which is the structural reason the defect survived.
//
// So every fixture below is built so that BASE SKINNING AND THE PROFILE DISAGREE, and the
// assertions are on the base side. A test that passed merely because the readback echoed the
// write would be worthless here - that agreement IS the bug.
//
// No test in this file calls a mutator that reaches USkeletalMesh::Build(). That is deliberate:
// Build() hands the work to a thread pool whenever FSkinnedAssetCompilingManager reports async
// compilation allowed (UE 5.8 SkeletalMesh.cpp:2361-2373), so inspecting LODModels straight
// after a mutator would be racing a worker. The mutator coverage here is on the pre-flight
// rejection paths, which return before anything is written, plus pure index-space math.

#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/Animation/SkinWeightTransferUtils.h"
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
// SkinWeightProfile.h (FSkinWeightProfileInfo / FRawSkinWeight) moved from Animation/ to
// Rendering/ in UE 5.8.
#if __has_include("Rendering/SkinWeightProfile.h")
#include "Rendering/SkinWeightProfile.h"
#elif __has_include("Animation/SkinWeightProfile.h")
#include "Animation/SkinWeightProfile.h"
#endif

namespace
{
    // Reference-skeleton indices of the fixture bones. Named so an assertion reads as a bone
    // rather than as a magic number, and so the section-local slot (always 0 below) is visibly
    // a different thing from the real bone.
    enum class EBaseReadbackBone : int32
    {
        Root = 0,
        Spine = 1,
        HandL = 2,
        FootL = 3,
    };

    // Per-file unique names throughout: this module builds with Unity enabled, so a helper
    // sharing a name with TestSkinWeightAudit.cpp's twin would collide when the TUs merge.

    // A real on-disk package, because every handler under test resolves its mesh through
    // StaticLoadObject and a bare transient object would not satisfy that. GUID-suffixed so
    // repeat and parallel runs never collide. Caller must CleanupTestAsset(OutPackagePath).
    USkeletalMesh* MakeBaseReadbackMesh(FAutomationTestBase& Test, FString& OutPackagePath, FString& OutMeshPath)
    {
        OutPackagePath = FString::Printf(TEXT("/Game/PinWrightTest_BaseWeights_%s"),
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
                FTransform::Identity, true);
            Modifier.Add(FMeshBoneInfo(FName(TEXT("spine_01")), TEXT("spine_01"), 0),
                FTransform(FVector(0.0, 0.0, 60.0)));
            Modifier.Add(FMeshBoneInfo(FName(TEXT("hand_l")), TEXT("hand_l"), 1),
                FTransform(FVector(50.0, 0.0, 10.0)));
            Modifier.Add(FMeshBoneInfo(FName(TEXT("foot_l")), TEXT("foot_l"), 0),
                FTransform(FVector(0.0, 0.0, -80.0)));
        }

        Mesh->SetSkeleton(Skeleton);
        {
            // Mirrored onto the MESH's own reference skeleton: section BoneMap entries index
            // USkeletalMesh::RefSkeleton (Rendering/SkeletalMeshLODModel.h:74-75), not the
            // USkeleton's, and that is the skeleton the readback names bones from.
            FReferenceSkeletonModifier MeshModifier(Mesh->GetRefSkeleton(), Skeleton);
            MeshModifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE),
                FTransform::Identity, true);
            MeshModifier.Add(FMeshBoneInfo(FName(TEXT("spine_01")), TEXT("spine_01"), 0),
                FTransform(FVector(0.0, 0.0, 60.0)));
            MeshModifier.Add(FMeshBoneInfo(FName(TEXT("hand_l")), TEXT("hand_l"), 1),
                FTransform(FVector(50.0, 0.0, 10.0)));
            MeshModifier.Add(FMeshBoneInfo(FName(TEXT("foot_l")), TEXT("foot_l"), 0),
                FTransform(FVector(0.0, 0.0, -80.0)));
        }

        Mesh->AddLODInfo();
        OutMeshPath = Mesh->GetPathName();
        return Mesh;
    }

    // Writes one soft vertex: zero-fills both influence arrays (so trailing slots read as
    // "none"), then assigns slot 0 to SectionLocalBone at Weight. SectionLocalBone is an index
    // into the section's BoneMap, exactly as the importer writes it - which is the whole point
    // of these fixtures.
    void SetBaseReadbackVertex(FSkelMeshSection& Section, int32 VertexIndex,
        const FVector3f& Position, uint16 SectionLocalBone, float Weight)
    {
        FSoftSkinVertex& Vertex = Section.SoftVertices[VertexIndex];
        FMemory::Memzero(Vertex.InfluenceBones, sizeof(Vertex.InfluenceBones));
        FMemory::Memzero(Vertex.InfluenceWeights, sizeof(Vertex.InfluenceWeights));
        Vertex.Position = Position;
        Vertex.InfluenceBones[0] = SectionLocalBone;
        Vertex.InfluenceWeights[0] =
            static_cast<uint16>(FMath::RoundToInt(FMath::Clamp(Weight, 0.0f, 1.0f) * 65535.0f));
    }

    // THE FIXTURE THE WHOLE FILE TURNS ON.
    //
    // Two sections whose bone maps point the SAME section-local slot 0 at DIFFERENT real bones
    // (hand_l and foot_l). Any code that reads InfluenceBones as a reference-skeleton index
    // sees "both vertices weighted to bone 0" - the root - and reports a plausible, wrong,
    // clean-looking answer. Vertex 0's base weight is deliberately 0.5, so BASE skinning is
    // measurably degenerate while the profile written over it is not.
    void BuildSkinBaseTwoSectionLod(USkeletalMesh& Mesh)
    {
        FSkeletalMeshLODModel* LODModel = new FSkeletalMeshLODModel();

        FSkelMeshSection& SectionA = LODModel->Sections.AddDefaulted_GetRef();
        SectionA.BoneMap = { static_cast<FBoneIndexType>(EBaseReadbackBone::HandL) };
        SectionA.SoftVertices.SetNum(1);
        SectionA.NumVertices = 1;
        SectionA.BaseVertexIndex = 0;
        // Half weight: base skinning here sums to 0.5 and is therefore DEGENERATE.
        SetBaseReadbackVertex(SectionA, 0, FVector3f(50.0f, 0.0f, 70.0f), /*slot*/ 0, 0.5f);

        FSkelMeshSection& SectionB = LODModel->Sections.AddDefaulted_GetRef();
        SectionB.BoneMap = { static_cast<FBoneIndexType>(EBaseReadbackBone::FootL) };
        SectionB.SoftVertices.SetNum(1);
        SectionB.NumVertices = 1;
        SectionB.BaseVertexIndex = 1;
        SetBaseReadbackVertex(SectionB, 0, FVector3f(0.0f, 0.0f, -80.0f), /*slot*/ 0, 1.0f);

        LODModel->NumVertices = 2;
        Mesh.GetImportedModel()->LODModels.Add(LODModel);
    }

    // Registers a named alternate profile whose per-vertex weights are perfectly normalized -
    // i.e. exactly what skeleton.normalize_weights leaves behind. The profile therefore reads
    // as clean while the base skinning underneath is not, which is the disagreement the
    // readback must be able to show.
    void AddSkinBaseNormalizedProfile(USkeletalMesh& Mesh, const TCHAR* ProfileName)
    {
        FSkinWeightProfileInfo Info;
        Info.Name = FName(ProfileName);
        Mesh.AddSkinWeightProfile(Info);

        FImportedSkinWeightProfileData& ProfileData =
            Mesh.GetImportedModel()->LODModels[0].SkinWeightProfiles.FindOrAdd(FName(ProfileName));
        ProfileData.SkinWeights.SetNum(2);
        for (int32 VertIdx = 0; VertIdx < 2; ++VertIdx)
        {
            FMemory::Memzero(&ProfileData.SkinWeights[VertIdx], sizeof(FRawSkinWeight));
            // Section-local slot 0 in both sections, full weight: valid, normalized, and in a
            // different index space from the base readback's output.
            ProfileData.SkinWeights[VertIdx].InfluenceBones[0] = 0;
            ProfileData.SkinWeights[VertIdx].InfluenceWeights[0] = 65535;
        }
    }

    TSharedPtr<FJsonObject> MakeSkinBaseDescribePayload(const FString& MeshPath, int32 SampleCount)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), MeshPath);
        Payload->SetNumberField(TEXT("sampleCount"), SampleCount);
        return Payload;
    }

    // baseSkinning.lods[Index], or an invalid pointer.
    const TSharedPtr<FJsonObject>* FindSkinBaseLod(const TSharedPtr<FJsonObject>& Result, int32 Index)
    {
        const TSharedPtr<FJsonObject>* BaseObj = nullptr;
        if (!Result.IsValid() || !Result->TryGetObjectField(TEXT("baseSkinning"), BaseObj) || !BaseObj)
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Lods = nullptr;
        if (!(*BaseObj)->TryGetArrayField(TEXT("lods"), Lods) || !Lods || !Lods->IsValidIndex(Index))
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* LodObj = nullptr;
        if (!(*Lods)[Index]->TryGetObject(LodObj))
        {
            return nullptr;
        }
        return LodObj;
    }

    // profiles[ProfileIndex].lods[LodIndex], or an invalid pointer.
    const TSharedPtr<FJsonObject>* FindSkinBaseProfileLod(const TSharedPtr<FJsonObject>& Result,
        int32 ProfileIndex, int32 LodIndex)
    {
        const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
        if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("profiles"), Profiles) || !Profiles
            || !Profiles->IsValidIndex(ProfileIndex))
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* ProfileObj = nullptr;
        if (!(*Profiles)[ProfileIndex]->TryGetObject(ProfileObj) || !ProfileObj)
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Lods = nullptr;
        if (!(*ProfileObj)->TryGetArrayField(TEXT("lods"), Lods) || !Lods || !Lods->IsValidIndex(LodIndex))
        {
            return nullptr;
        }
        const TSharedPtr<FJsonObject>* LodObj = nullptr;
        if (!(*Lods)[LodIndex]->TryGetObject(LodObj))
        {
            return nullptr;
        }
        return LodObj;
    }

    // The boneName of sample[VertexIndex].influences[0] inside a per-LOD block, or empty.
    FString SkinBaseFirstSampledBoneName(const TSharedPtr<FJsonObject>* LodObj, int32 SampleIndex)
    {
        if (!LodObj || !(*LodObj).IsValid())
        {
            return FString();
        }
        const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
        if (!(*LodObj)->TryGetArrayField(TEXT("sample"), Samples) || !Samples
            || !Samples->IsValidIndex(SampleIndex))
        {
            return FString();
        }
        const TSharedPtr<FJsonObject>* SampleObj = nullptr;
        if (!(*Samples)[SampleIndex]->TryGetObject(SampleObj) || !SampleObj)
        {
            return FString();
        }
        const TArray<TSharedPtr<FJsonValue>>* Influences = nullptr;
        if (!(*SampleObj)->TryGetArrayField(TEXT("influences"), Influences) || !Influences
            || Influences->Num() == 0)
        {
            return FString();
        }
        const TSharedPtr<FJsonObject>* InfluenceObj = nullptr;
        if (!(*Influences)[0]->TryGetObject(InfluenceObj) || !InfluenceObj)
        {
            return FString();
        }
        FString BoneName;
        (*InfluenceObj)->TryGetStringField(TEXT("boneName"), BoneName);
        return BoneName;
    }

    bool RunSkinBaseDescribe(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Payload,
        FTestResponseCapture& Capture)
    {
        const bool bFound = InvokeHandlerWithCapture(TEXT("skeleton.describe_skin_weights"), Payload, Capture);
        Test.TestTrue(TEXT("skeleton.describe_skin_weights is registered"), bFound);
        Test.TestTrue(TEXT("handler responded"), Capture.bWasCalled);
        return bFound && Capture.bWasCalled;
    }
}

// ---------------------------------------------------------------------------------------
// THE TEST THAT WOULD HAVE CAUGHT THE ORIGINAL DEFECT.
//
// The profile says the mesh is perfectly skinned. The sections say one of its two vertices
// sums to 0.5. Before this change the readback could only tell you the first of those, and it
// was documented as the way to verify a weight write - so an agent could normalize weights,
// read them back, see exactly what it asked for, and ship a mesh whose real skinning was
// untouched. The assertions below are on the BASE side; the profile assertions are here only
// to prove the two blocks really do disagree in the same response.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkinWeightsBaseDisagreesWithProfileTest,
    "PinWright.skeleton.describe_skin_weights.ReportsBaseSkinningNotJustProfiles",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkinWeightsBaseDisagreesWithProfileTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    FString MeshPath;
    USkeletalMesh* Mesh = MakeBaseReadbackMesh(*this, PackagePath, MeshPath);
    if (!Mesh)
    {
        return false;
    }
    Mesh->AddToRoot();
    ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

    BuildSkinBaseTwoSectionLod(*Mesh);
    AddSkinBaseNormalizedProfile(*Mesh, TEXT("NormalizedWeights"));

    FTestResponseCapture Capture;
    if (!RunSkinBaseDescribe(*this, MakeSkinBaseDescribePayload(MeshPath, /*sampleCount*/ 2), Capture))
    {
        return false;
    }
    TestTrue(FString::Printf(TEXT("describe succeeded (errorCode='%s')"), *Capture.ErrorCode), Capture.bSuccess);
    if (!Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    // --- The profile's own story: clean. This is what the old readback returned, alone. ---
    TestEqual(TEXT("one alternate profile is registered"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("profileCount"))), 1);
    const TSharedPtr<FJsonObject>* ProfileLod = FindSkinBaseProfileLod(Capture.Result, 0, 0);
    if (!ProfileLod || !(*ProfileLod).IsValid())
    {
        AddError(TEXT("profiles[0].lods[0] missing from the response"));
        return false;
    }
    TestEqual(TEXT("the profile reports both vertices normalized"),
        static_cast<int32>((*ProfileLod)->GetNumberField(TEXT("normalizedVertexCount"))), 2);
    TestEqual(TEXT("the profile reports nothing degenerate"),
        static_cast<int32>((*ProfileLod)->GetNumberField(TEXT("degenerateVertexCount"))), 0);
    // The profile's stored slots are section-local, and the response must say so rather than
    // emitting a bare number the caller has to guess about.
    TestEqual(TEXT("profile block declares its bone index space"),
        (*ProfileLod)->GetStringField(TEXT("boneIndexSpace")), FString(TEXT("sectionLocal")));

    // --- The mesh's actual story: one vertex is only half weighted. ---
    const TSharedPtr<FJsonObject>* BaseLod = FindSkinBaseLod(Capture.Result, 0);
    if (!BaseLod || !(*BaseLod).IsValid())
    {
        // The whole point: before this change there was no baseSkinning block at all, and the
        // response above was a clean success.
        AddError(TEXT("baseSkinning.lods[0] missing - the readback still cannot see base skinning"));
        return false;
    }
    TestEqual(TEXT("base readback covers every section vertex"),
        static_cast<int32>((*BaseLod)->GetNumberField(TEXT("vertexCount"))), 2);
    TestEqual(TEXT("base readback saw both sections"),
        static_cast<int32>((*BaseLod)->GetNumberField(TEXT("sectionCount"))), 2);
    TestEqual(TEXT("BASE skinning has one degenerate vertex the profile does not"),
        static_cast<int32>((*BaseLod)->GetNumberField(TEXT("degenerateVertexCount"))), 1);
    TestEqual(TEXT("BASE skinning has one normalized vertex"),
        static_cast<int32>((*BaseLod)->GetNumberField(TEXT("normalizedVertexCount"))), 1);
    TestEqual(TEXT("no influence was dropped for want of a bone map entry"),
        static_cast<int32>((*BaseLod)->GetNumberField(TEXT("unmappedInfluences"))), 0);
    TestEqual(TEXT("base block declares reference-skeleton bone indices"),
        (*BaseLod)->GetStringField(TEXT("boneIndexSpace")), FString(TEXT("referenceSkeleton")));

    // --- The BoneMap indirection, on both blocks. ---
    //
    // Both sections store section-local slot 0. Reading that as a reference-skeleton index
    // yields bone 0 - "root" - for both vertices, which is a plausible and completely wrong
    // answer. Resolution through FSkelMeshSection::BoneMap is the only way to get hand_l and
    // foot_l, so these four assertions are what stands between the bug and a shipped readback.
    TestEqual(TEXT("base vertex 0 resolves to hand_l, not root"),
        SkinBaseFirstSampledBoneName(BaseLod, 0), FString(TEXT("hand_l")));
    TestEqual(TEXT("base vertex 1 resolves to foot_l, not root"),
        SkinBaseFirstSampledBoneName(BaseLod, 1), FString(TEXT("foot_l")));
    TestEqual(TEXT("profile vertex 0's section-local slot 0 resolves to hand_l"),
        SkinBaseFirstSampledBoneName(ProfileLod, 0), FString(TEXT("hand_l")));
    TestEqual(TEXT("profile vertex 1's section-local slot 0 resolves to foot_l"),
        SkinBaseFirstSampledBoneName(ProfileLod, 1), FString(TEXT("foot_l")));

    return true;
}

// ---------------------------------------------------------------------------------------
// The other half of the same hole: a mesh with NO authored profile. Every FBX import and every
// mesh produced by geometry.convert_to_skeletal_mesh is in this state. The old readback
// answered profileCount:0 with an empty array and a success status, having measured nothing
// whatsoever - a clean-looking response about a mesh it had not looked at.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkinWeightsProfilelessMeshStillMeasuredTest,
    "PinWright.skeleton.describe_skin_weights.ProfilelessMeshStillReportsBaseSkinning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkinWeightsProfilelessMeshStillMeasuredTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    FString MeshPath;
    USkeletalMesh* Mesh = MakeBaseReadbackMesh(*this, PackagePath, MeshPath);
    if (!Mesh)
    {
        return false;
    }
    Mesh->AddToRoot();
    ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

    BuildSkinBaseTwoSectionLod(*Mesh);   // deliberately NO profile

    FTestResponseCapture Capture;
    if (!RunSkinBaseDescribe(*this, MakeSkinBaseDescribePayload(MeshPath, /*sampleCount*/ 2), Capture)
        || !Capture.bSuccess || !Capture.Result.IsValid())
    {
        return false;
    }

    TestEqual(TEXT("no alternate profiles, as for any freshly imported mesh"),
        static_cast<int32>(Capture.Result->GetNumberField(TEXT("profileCount"))), 0);

    const TSharedPtr<FJsonObject>* BaseLod = FindSkinBaseLod(Capture.Result, 0);
    if (!BaseLod || !(*BaseLod).IsValid())
    {
        AddError(TEXT("profileCount:0 with no baseSkinning is the 'measured nothing' response this verb must never send again"));
        return false;
    }
    TestEqual(TEXT("base skinning is still fully measured"),
        static_cast<int32>((*BaseLod)->GetNumberField(TEXT("vertexCount"))), 2);
    TestEqual(TEXT("and the degenerate vertex is visible with no profile in sight"),
        static_cast<int32>((*BaseLod)->GetNumberField(TEXT("degenerateVertexCount"))), 1);
    TestEqual(TEXT("bone names still resolve through the section bone map"),
        SkinBaseFirstSampledBoneName(BaseLod, 0), FString(TEXT("hand_l")));

    return true;
}

// ---------------------------------------------------------------------------------------
// A mesh with nothing to read must ERROR, not succeed with an empty block. A cooked-only or
// not-yet-built mesh has no imported model, and "I could not look" must never render the same
// as "I looked and there was nothing" - the rule skeleton.audit_skin_weights already follows.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkinWeightsUnreadableMeshErrorsTest,
    "PinWright.skeleton.describe_skin_weights.UnreadableMeshErrorsRatherThanPasses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkinWeightsUnreadableMeshErrorsTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    FString MeshPath;
    USkeletalMesh* Mesh = MakeBaseReadbackMesh(*this, PackagePath, MeshPath);
    if (!Mesh)
    {
        return false;
    }
    Mesh->AddToRoot();
    ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

    // Deliberately no LOD model at all.
    FTestResponseCapture Capture;
    if (!RunSkinBaseDescribe(*this, MakeSkinBaseDescribePayload(MeshPath, 0), Capture))
    {
        return false;
    }
    TestFalse(TEXT("an unreadable mesh must not report success"), Capture.bSuccess);
    TestEqual(TEXT("and it must say why"), Capture.ErrorCode, FString(TEXT("NO_LOD_MODELS")));
    return true;
}

// ---------------------------------------------------------------------------------------
// skeleton.set_vertex_weights: the bone index space is now stated AND enforced.
//
// Before this, boneIndex was cast straight to FBoneIndexType with no validation, in a space the
// schema never named - and because the buffer it writes is section-local while the list rebuilt
// from it is reference-skeleton, no value a caller could pass was correct in both places. Each
// case below must be REJECTED, and must leave the mesh untouched: a half-applied weight edit is
// worse than a refused one, and the old code silently skipped bad entries while reporting the
// same success.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSetVertexWeightsRejectsUnusableBonesTest,
    "PinWright.skeleton.set_vertex_weights.RejectsUnusableBoneAndVertexIndices",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSetVertexWeightsRejectsUnusableBonesTest::RunTest(const FString& Parameters)
{
    FString PackagePath;
    FString MeshPath;
    USkeletalMesh* Mesh = MakeBaseReadbackMesh(*this, PackagePath, MeshPath);
    if (!Mesh)
    {
        return false;
    }
    Mesh->AddToRoot();
    ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

    BuildSkinBaseTwoSectionLod(*Mesh);
    const int32 ProfilesBefore = Mesh->GetSkinWeightProfiles().Num();

    // Builds {weights:[{vertexIndex, influences:[<one influence object>]}]}.
    auto MakePayload = [&MeshPath](int32 VertexIndex, const TSharedPtr<FJsonObject>& Influence)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("vertexIndex"), VertexIndex);
        TArray<TSharedPtr<FJsonValue>> Influences;
        if (Influence.IsValid())
        {
            Influences.Add(MakeShared<FJsonValueObject>(Influence));
        }
        Entry->SetArrayField(TEXT("influences"), Influences);

        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("skeletalMeshPath"), MeshPath);
        TArray<TSharedPtr<FJsonValue>> Weights;
        Weights.Add(MakeShared<FJsonValueObject>(Entry));
        Payload->SetArrayField(TEXT("weights"), Weights);
        return Payload;
    };

    auto MakeInfluence = [](const TCHAR* BoneName, int32 BoneIndex, double Weight)
    {
        TSharedPtr<FJsonObject> Influence = MakeShared<FJsonObject>();
        if (BoneName)
        {
            Influence->SetStringField(TEXT("boneName"), BoneName);
        }
        if (BoneIndex != INDEX_NONE)
        {
            Influence->SetNumberField(TEXT("boneIndex"), BoneIndex);
        }
        Influence->SetNumberField(TEXT("weight"), Weight);
        return Influence;
    };

    auto ExpectRejection = [this](const TSharedPtr<FJsonObject>& Payload, const TCHAR* ExpectedCode,
        const TCHAR* What)
    {
        FTestResponseCapture Capture;
        const bool bFound = InvokeHandlerWithCapture(TEXT("skeleton.set_vertex_weights"), Payload, Capture);
        TestTrue(TEXT("skeleton.set_vertex_weights is registered"), bFound);
        TestFalse(*FString::Printf(TEXT("%s is rejected, not silently skipped"), What), Capture.bSuccess);
        TestEqual(*FString::Printf(TEXT("%s reports %s"), What, ExpectedCode),
            Capture.ErrorCode, FString(ExpectedCode));
    };

    // Vertex 0 lives in section 0, whose bone map carries hand_l ONLY. foot_l is a perfectly
    // real bone on this skeleton, so this is not BONE_NOT_FOUND - there is simply no
    // section-local slot for it on that vertex, and inventing one would weight the vertex to
    // hand_l while reporting foot_l.
    ExpectRejection(MakePayload(0, MakeInfluence(TEXT("foot_l"), INDEX_NONE, 1.0)),
        TEXT("BONE_NOT_IN_SECTION"), TEXT("a bone the vertex's section does not carry"));

    // A bone that is not on the skeleton at all.
    ExpectRejection(MakePayload(0, MakeInfluence(TEXT("no_such_bone"), INDEX_NONE, 1.0)),
        TEXT("BONE_NOT_FOUND"), TEXT("an unknown bone name"));

    // boneIndex is a REFERENCE-SKELETON index; 99 is past this 4-bone skeleton. The old code
    // stored it verbatim as an influence bone.
    ExpectRejection(MakePayload(0, MakeInfluence(nullptr, 99, 1.0)),
        TEXT("BONE_NOT_FOUND"), TEXT("an out-of-range boneIndex"));

    // A vertexIndex past the LOD's section coverage. Previously skipped in silence, so the
    // response reported success for an edit that touched nothing.
    ExpectRejection(MakePayload(99, MakeInfluence(TEXT("hand_l"), INDEX_NONE, 1.0)),
        TEXT("INDEX_OUT_OF_RANGE"), TEXT("a vertexIndex past the end of the LOD"));

    // Zero is not "no influence" in this storage: the influence arrays are zero-TERMINATED, so
    // a zero weight truncates the vertex's influence list at that slot.
    ExpectRejection(MakePayload(0, MakeInfluence(TEXT("hand_l"), INDEX_NONE, 0.0)),
        TEXT("INVALID_ARGUMENT"), TEXT("a zero weight"));

    // An empty influence list would leave the vertex weighted to nothing, which renders it at
    // the component origin.
    ExpectRejection(MakePayload(0, TSharedPtr<FJsonObject>()),
        TEXT("INVALID_ARGUMENT"), TEXT("an empty influence list"));

    // Nothing was written by any of the six: the pre-flight runs before the profile write and
    // before Build(), so a rejected call cannot leave a half-authored profile behind.
    TestEqual(TEXT("no profile was registered by any rejected call"),
        Mesh->GetSkinWeightProfiles().Num(), ProfilesBefore);
    TestEqual(TEXT("no profile data was written to the LOD"),
        Mesh->GetImportedModel()->LODModels[0].SkinWeightProfiles.Num(), 0);

    return true;
}

// ---------------------------------------------------------------------------------------
// The index-space round trip itself, on the two-section fixture. Pure - no handler, no Build().
// A single-section mesh cannot fail these: its bone map is effectively the identity, which is
// exactly why the original defect was invisible until a real character came along.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkinWeightBoneSpaceRoundTripTest,
    "PinWright.skeleton.skin_weights.BoneIndexSpaceRoundTrip",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkinWeightBoneSpaceRoundTripTest::RunTest(const FString& Parameters)
{
    using namespace SkinWeightTransferUtils;

    FSkeletalMeshLODModel LODModel;
    {
        FSkelMeshSection& SectionA = LODModel.Sections.AddDefaulted_GetRef();
        SectionA.BoneMap = { static_cast<FBoneIndexType>(EBaseReadbackBone::HandL) };
        SectionA.SoftVertices.SetNum(1);
        SectionA.NumVertices = 1;
        SectionA.BaseVertexIndex = 0;
        SetBaseReadbackVertex(SectionA, 0, FVector3f(50.0f, 0.0f, 70.0f), /*slot*/ 0, 1.0f);

        FSkelMeshSection& SectionB = LODModel.Sections.AddDefaulted_GetRef();
        SectionB.BoneMap = { static_cast<FBoneIndexType>(EBaseReadbackBone::FootL) };
        SectionB.SoftVertices.SetNum(1);
        SectionB.NumVertices = 1;
        SectionB.BaseVertexIndex = 1;
        SetBaseReadbackVertex(SectionB, 0, FVector3f(0.0f, 0.0f, -80.0f), /*slot*/ 0, 1.0f);

        LODModel.NumVertices = 2;
    }

    // Vertex -> section. The engine's own GetSectionFromVertexIndex answers "last section" for
    // an out-of-range index because its guard is commented out, so this helper's INDEX_NONE is
    // load-bearing for every caller's range check.
    TestEqual(TEXT("vertex 0 is in section 0"), FindSectionForVertex(LODModel, 0), 0);
    TestEqual(TEXT("vertex 1 is in section 1"), FindSectionForVertex(LODModel, 1), 1);
    TestEqual(TEXT("a vertex past the end is detectable, not clamped to the last section"),
        FindSectionForVertex(LODModel, 2), INDEX_NONE);
    TestEqual(TEXT("a negative vertex is detectable"), FindSectionForVertex(LODModel, -1), INDEX_NONE);

    // Section-local -> reference-skeleton. Both sections use slot 0; the real bones differ.
    int32 RefBone = INDEX_NONE;
    TestTrue(TEXT("section 0 slot 0 resolves"), ResolveSectionLocalBone(LODModel, 0, 0, RefBone));
    TestEqual(TEXT("section 0 slot 0 is hand_l, not bone 0"),
        RefBone, static_cast<int32>(EBaseReadbackBone::HandL));
    TestTrue(TEXT("section 1 slot 0 resolves"), ResolveSectionLocalBone(LODModel, 1, 0, RefBone));
    TestEqual(TEXT("the SAME slot in section 1 is foot_l"),
        RefBone, static_cast<int32>(EBaseReadbackBone::FootL));

    // A slot with no bone map entry is refused rather than defaulted to 0 (the root), which
    // would fabricate an influence that passes every validity check downstream.
    RefBone = 12345;
    TestFalse(TEXT("an unmapped slot is refused"), ResolveSectionLocalBone(LODModel, 0, 7, RefBone));
    TestEqual(TEXT("and the out-param is not left holding a plausible bone"), RefBone, INDEX_NONE);

    // Reference-skeleton -> section-local, the direction set_vertex_weights needs.
    int32 LocalSlot = INDEX_NONE;
    TestTrue(TEXT("hand_l has a slot on vertex 0"),
        ResolveBoneToSectionLocalSlot(LODModel, 0, static_cast<int32>(EBaseReadbackBone::HandL), LocalSlot));
    TestEqual(TEXT("and it is slot 0"), LocalSlot, 0);
    TestFalse(TEXT("foot_l has NO slot on vertex 0, because section 0 does not carry it"),
        ResolveBoneToSectionLocalSlot(LODModel, 0, static_cast<int32>(EBaseReadbackBone::FootL), LocalSlot));

    // The write path: section-local slots out, reference-skeleton indices in the rebuilt list.
    // An implementation that skips the indirection reports bone 0 twice here.
    TArray<FRawSkinWeight> BaseWeights;
    TArray<FSoftSkinVertex> Vertices;
    LODModel.GetVertices(Vertices);
    CaptureBaseSkinWeights(Vertices, BaseWeights);
    TestEqual(TEXT("captured slots are still section-local"),
        static_cast<int32>(BaseWeights[1].InfluenceBones[0]), 0);

    TArray<SkeletalMeshImportData::FVertInfluence> Influences;
    TestEqual(TEXT("nothing dropped on a consistent LOD"),
        RebuildSourceModelInfluences(LODModel, BaseWeights, Influences), 0);
    TestEqual(TEXT("one influence per vertex"), Influences.Num(), 2);
    if (Influences.Num() == 2)
    {
        TestEqual(TEXT("vertex 0's rebuilt bone is hand_l"),
            static_cast<int32>(Influences[0].BoneIndex), static_cast<int32>(EBaseReadbackBone::HandL));
        TestEqual(TEXT("vertex 1's rebuilt bone is foot_l, NOT the same slot number"),
            static_cast<int32>(Influences[1].BoneIndex), static_cast<int32>(EBaseReadbackBone::FootL));
    }

    // The read path: base skinning surfaces reference-skeleton indices directly.
    FBaseSkinningReadback Readback;
    ReadBaseSkinningRefSkeletonSpace(LODModel, Readback);
    TestEqual(TEXT("base readback covers both vertices"), Readback.SkinWeights.Num(), 2);
    TestEqual(TEXT("nothing unmapped"), Readback.UnmappedInfluences, 0);
    if (Readback.SkinWeights.Num() == 2)
    {
        TestEqual(TEXT("base vertex 0 reads as hand_l"),
            static_cast<int32>(Readback.SkinWeights[0].InfluenceBones[0]),
            static_cast<int32>(EBaseReadbackBone::HandL));
        TestEqual(TEXT("base vertex 1 reads as foot_l"),
            static_cast<int32>(Readback.SkinWeights[1].InfluenceBones[0]),
            static_cast<int32>(EBaseReadbackBone::FootL));
    }

    return true;
}

// ---------------------------------------------------------------------------------------
// skeleton.copy_weights across meshes whose sections number their bones differently.
//
// CopyClosestVertexWeights memcpys the matched source vertex's influence slots, and those slots
// index the SOURCE section's bone map. Persisting them unchanged reads slot N of a source
// section as slot N of a target section - two different real bones on any multi-section mesh,
// with no error and no failed return. The translation step is what makes the transfer mean the
// same bone on both sides; a bone the target section cannot carry is dropped and COUNTED.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCopyWeightsTranslatesSectionSpacesTest,
    "PinWright.skeleton.copy_weights.TranslatesBetweenSectionBoneMaps",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FCopyWeightsTranslatesSectionSpacesTest::RunTest(const FString& Parameters)
{
    using namespace SkinWeightTransferUtils;

    // Source: one section, bone map {hand_l, foot_l}. Vertex 0 is fully weighted to SLOT 1,
    // which is foot_l here.
    FSkeletalMeshLODModel SourceLOD;
    {
        FSkelMeshSection& Section = SourceLOD.Sections.AddDefaulted_GetRef();
        Section.BoneMap = {
            static_cast<FBoneIndexType>(EBaseReadbackBone::HandL),
            static_cast<FBoneIndexType>(EBaseReadbackBone::FootL),
        };
        Section.SoftVertices.SetNum(1);
        Section.NumVertices = 1;
        Section.BaseVertexIndex = 0;
        SetBaseReadbackVertex(Section, 0, FVector3f(0.0f, 0.0f, 0.0f), /*slot*/ 1, 1.0f);
        SourceLOD.NumVertices = 1;
    }

    // Target: one section whose bone map lists the SAME two bones in the OPPOSITE order, so
    // foot_l is slot 0 there. A verbatim copy would store slot 1 and silently weight the target
    // vertex to hand_l.
    FSkeletalMeshLODModel TargetLOD;
    {
        FSkelMeshSection& Section = TargetLOD.Sections.AddDefaulted_GetRef();
        Section.BoneMap = {
            static_cast<FBoneIndexType>(EBaseReadbackBone::FootL),
            static_cast<FBoneIndexType>(EBaseReadbackBone::HandL),
        };
        Section.SoftVertices.SetNum(1);
        Section.NumVertices = 1;
        Section.BaseVertexIndex = 0;
        SetBaseReadbackVertex(Section, 0, FVector3f(1.0f, 0.0f, 0.0f), /*slot*/ 0, 1.0f);
        TargetLOD.NumVertices = 1;
    }

    TArray<FSoftSkinVertex> SourceVertices;
    SourceLOD.GetVertices(SourceVertices);
    TArray<FSoftSkinVertex> TargetVertices;
    TargetLOD.GetVertices(TargetVertices);

    TArray<FRawSkinWeight> Copied;
    TArray<int32> MatchedSource;
    TestEqual(TEXT("one target vertex copied"),
        CopyClosestVertexWeights(SourceVertices, TargetVertices, Copied, &MatchedSource), 1);
    TestEqual(TEXT("the match was recorded so the source section is knowable"),
        MatchedSource.Num(), 1);
    TestEqual(TEXT("the raw copy carries the SOURCE section's slot"),
        static_cast<int32>(Copied[0].InfluenceBones[0]), 1);

    TestEqual(TEXT("nothing dropped: both meshes carry foot_l"),
        TranslateCopiedWeightsBetweenLODs(SourceLOD, TargetLOD, MatchedSource, Copied), 0);
    // Slot 1 on the source is foot_l; foot_l is slot 0 on the target. A verbatim copy leaves 1.
    TestEqual(TEXT("the translated slot names the same real bone on the target"),
        static_cast<int32>(Copied[0].InfluenceBones[0]), 0);
    TestEqual(TEXT("the weight is unchanged by the translation"),
        static_cast<int32>(Copied[0].InfluenceWeights[0]), 65535);

    // A target whose section cannot carry the source bone at all: dropped and counted, never
    // substituted. Under a verbatim copy this case is indistinguishable from a clean transfer.
    FSkeletalMeshLODModel NarrowTargetLOD;
    {
        FSkelMeshSection& Section = NarrowTargetLOD.Sections.AddDefaulted_GetRef();
        Section.BoneMap = { static_cast<FBoneIndexType>(EBaseReadbackBone::HandL) };
        Section.SoftVertices.SetNum(1);
        Section.NumVertices = 1;
        Section.BaseVertexIndex = 0;
        SetBaseReadbackVertex(Section, 0, FVector3f(1.0f, 0.0f, 0.0f), /*slot*/ 0, 1.0f);
        NarrowTargetLOD.NumVertices = 1;
    }

    TArray<FRawSkinWeight> CopiedNarrow;
    TArray<int32> MatchedNarrow;
    CopyClosestVertexWeights(SourceVertices, TargetVertices, CopiedNarrow, &MatchedNarrow);
    TestEqual(TEXT("the untranslatable influence is dropped and counted"),
        TranslateCopiedWeightsBetweenLODs(SourceLOD, NarrowTargetLOD, MatchedNarrow, CopiedNarrow), 1);
    TestEqual(TEXT("and it is not substituted with the root bone"),
        static_cast<int32>(CopiedNarrow[0].InfluenceWeights[0]), 0);

    return true;
}

// ---------------------------------------------------------------------------------------
// boneCoverage on the descriptive readback: which bones this LOD actually moves, and which
// vertices one bone owns outright.
//
// The defect it closes: an animation that drives a bone no vertex is weighted to moves
// NOTHING, and every number this verb already reported looks correct on that mesh — the
// weights sum, nothing is zero-filled, nothing is degenerate. The only evidence used to be a
// coincidence in an influence-count histogram, which cannot name a bone.
//
// Two fixtures differing in one vertex, because a list that is always full and a list that is
// always empty both look right against a single mesh. Note the two-section fixture also proves
// the naming goes through the section BoneMap: an implementation reading InfluenceBones as
// reference-skeleton indices would see both vertices on bone 0 and report hand_l and foot_l as
// uninfluenced — the exact opposite of the truth.
// ---------------------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkinWeightsBaseBoneCoverageNamesUninfluencedBonesTest,
    "PinWright.skeleton.describe_skin_weights.BoneCoverageNamesUninfluencedBones",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FSkinWeightsBaseBoneCoverageNamesUninfluencedBonesTest::RunTest(const FString& Parameters)
{
    // ---- Two bones own geometry, two own none ----
    {
        FString PackagePath;
        FString MeshPath;
        USkeletalMesh* Mesh = MakeBaseReadbackMesh(*this, PackagePath, MeshPath);
        if (!Mesh)
        {
            return false;
        }
        Mesh->AddToRoot();
        ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

        BuildSkinBaseTwoSectionLod(*Mesh);

        FTestResponseCapture Capture;
        if (!RunSkinBaseDescribe(*this, MakeSkinBaseDescribePayload(MeshPath, /*sampleCount*/ 0), Capture)
            || !Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* BaseLod = FindSkinBaseLod(Capture.Result, 0);
        if (!BaseLod || !(*BaseLod).IsValid())
        {
            AddError(TEXT("baseSkinning LOD 0 missing from the response"));
            return false;
        }
        const TSharedPtr<FJsonObject>* Coverage = nullptr;
        if (!(*BaseLod)->TryGetObjectField(TEXT("boneCoverage"), Coverage) || !Coverage
            || !(*Coverage).IsValid())
        {
            AddError(TEXT("baseSkinning LOD 0 carries no boneCoverage block"));
            return false;
        }

        TestEqual(TEXT("the four-bone reference skeleton was walked"),
            static_cast<int32>((*Coverage)->GetNumberField(TEXT("boneCount"))), 4);
        TestEqual(TEXT("two of its bones own geometry"),
            static_cast<int32>((*Coverage)->GetNumberField(TEXT("influencedBoneCount"))), 2);
        TestEqual(TEXT("and two own none"),
            static_cast<int32>((*Coverage)->GetNumberField(TEXT("bonesWithNoInfluenceCount"))), 2);
        TestTrue(TEXT("root is named as owning no geometry"),
            JsonStringArrayContains(*Coverage, TEXT("bonesWithNoInfluence"), TEXT("root")));
        TestTrue(TEXT("and so is spine_01"),
            JsonStringArrayContains(*Coverage, TEXT("bonesWithNoInfluence"), TEXT("spine_01")));

        // The BoneMap direction. Reading the stored slot as a reference-skeleton index would
        // put both vertices on bone 0 and list hand_l and foot_l here instead.
        TestFalse(TEXT("hand_l resolved through the section bone map, so it is not listed"),
            JsonStringArrayContains(*Coverage, TEXT("bonesWithNoInfluence"), TEXT("hand_l")));
        TestFalse(TEXT("foot_l likewise"),
            JsonStringArrayContains(*Coverage, TEXT("bonesWithNoInfluence"), TEXT("foot_l")));

        // Both fixture vertices carry exactly one influence, so both are rigid. Vertex 0's
        // weight is 0.5 - degenerate for the sum check, still a single binding for this one,
        // which is the point of measuring them separately.
        TestEqual(TEXT("both vertices belong to exactly one bone"),
            static_cast<int32>((*Coverage)->GetNumberField(TEXT("rigidVertexCount"))), 2);

        const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
        if ((*Coverage)->TryGetArrayField(TEXT("bones"), Bones) && Bones)
        {
            TestEqual(TEXT("one row per bone that owns geometry"), Bones->Num(), 2);
        }
        else
        {
            AddError(TEXT("boneCoverage carries no per-bone table"));
        }
    }

    // ---- Every bone owns geometry: the list must be PRESENT and EMPTY ----
    {
        FString PackagePath;
        FString MeshPath;
        USkeletalMesh* Mesh = MakeBaseReadbackMesh(*this, PackagePath, MeshPath);
        if (!Mesh)
        {
            return false;
        }
        Mesh->AddToRoot();
        ON_SCOPE_EXIT { Mesh->RemoveFromRoot(); CleanupTestAsset(PackagePath); };

        // One section whose bone map covers all four bones, one vertex per bone. The last is
        // blended across two, so rigidVertexCount separates "has an influence" from "belongs
        // to this bone outright" rather than counting every vertex.
        FSkeletalMeshLODModel* LODModel = new FSkeletalMeshLODModel();
        FSkelMeshSection& Section = LODModel->Sections.AddDefaulted_GetRef();
        Section.BoneMap = { 0, 1, 2, 3 };
        Section.SoftVertices.SetNum(4);
        Section.NumVertices = 4;
        Section.BaseVertexIndex = 0;
        SetBaseReadbackVertex(Section, 0, FVector3f(50.0f, 0.0f, 70.0f),
            static_cast<uint16>(EBaseReadbackBone::HandL), 1.0f);
        SetBaseReadbackVertex(Section, 1, FVector3f(0.0f, 0.0f, -80.0f),
            static_cast<uint16>(EBaseReadbackBone::FootL), 1.0f);
        SetBaseReadbackVertex(Section, 2, FVector3f(0.0f, 0.0f, 60.0f),
            static_cast<uint16>(EBaseReadbackBone::Spine), 1.0f);
        // Blended across root and spine_01: influenced by both, rigid to neither.
        SetBaseReadbackVertex(Section, 3, FVector3f(0.0f, 0.0f, 30.0f),
            static_cast<uint16>(EBaseReadbackBone::Root), 0.5f);
        Section.SoftVertices[3].InfluenceBones[1] = static_cast<uint16>(EBaseReadbackBone::Spine);
        Section.SoftVertices[3].InfluenceWeights[1] = 32767;
        LODModel->NumVertices = 4;
        Mesh->GetImportedModel()->LODModels.Add(LODModel);

        FTestResponseCapture Capture;
        if (!RunSkinBaseDescribe(*this, MakeSkinBaseDescribePayload(MeshPath, /*sampleCount*/ 0), Capture)
            || !Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }

        const TSharedPtr<FJsonObject>* BaseLod = FindSkinBaseLod(Capture.Result, 0);
        const TSharedPtr<FJsonObject>* Coverage = nullptr;
        if (!BaseLod || !(*BaseLod).IsValid()
            || !(*BaseLod)->TryGetObjectField(TEXT("boneCoverage"), Coverage) || !Coverage
            || !(*Coverage).IsValid())
        {
            AddError(TEXT("baseSkinning LOD 0 carries no boneCoverage block"));
            return false;
        }

        // The load-bearing assertion of the whole pair: the field is emitted even when there
        // is nothing to report. An absent array cannot be told apart from a build that never
        // looked for uninfluenced bones.
        const TArray<TSharedPtr<FJsonValue>>* Uninfluenced = nullptr;
        const bool bPresent = (*Coverage)->TryGetArrayField(TEXT("bonesWithNoInfluence"), Uninfluenced);
        TestTrue(TEXT("bonesWithNoInfluence is emitted even when it is empty"), bPresent);
        if (bPresent && Uninfluenced)
        {
            TestEqual(TEXT("and it is empty when every bone owns geometry"), Uninfluenced->Num(), 0);
        }
        TestEqual(TEXT("all four bones own geometry"),
            static_cast<int32>((*Coverage)->GetNumberField(TEXT("influencedBoneCount"))), 4);
        TestEqual(TEXT("three of the four vertices belong to one bone outright"),
            static_cast<int32>((*Coverage)->GetNumberField(TEXT("rigidVertexCount"))), 3);
        TestTrue(TEXT("the epsilon the counts used is published rather than implied"),
            (*Coverage)->GetNumberField(TEXT("weightEpsilon")) > 0.0);
    }

    return true;
}
