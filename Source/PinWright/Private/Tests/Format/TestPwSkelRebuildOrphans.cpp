// Copyright (c) 2026 Alexander Penkin. MIT License.

// Failure-direction coverage for generated-asset recompiles. Before the guard, both failing
// assertions below observed a successful, diagnostic-free rebuild that erased the mutation.
#include "Misc/AutomationTest.h"

#include "Compat/EngineVersionCompat.h"
#include "Tests/TestUtils.h"

#include "PwSkel/PwSkelAssetCreate.h"
#include "PwSkel/PwSkelParser.h"
#include "PwSource/PwDiagnostic.h"

#include "Animation/AnimCurveMetadata.h"
#include "Animation/BoneReference.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace PwSkelRecompileStateTests
{
    bool HasCode(const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
    {
        return Diagnostics.ContainsByPredicate([Code](const FPwDiagnostic& Diagnostic)
        {
            return Diagnostic.Code == Code;
        });
    }

    const FPwDiagnostic* FindCode(
        const TArray<FPwDiagnostic>& Diagnostics, const TCHAR* Code)
    {
        return Diagnostics.FindByPredicate([Code](const FPwDiagnostic& Diagnostic)
        {
            return Diagnostic.Code == Code;
        });
    }

    bool Parse(const FString& Source, FPwSkelDocument& OutDocument,
               FAutomationTestBase& Test)
    {
        TArray<FPwDiagnostic> Diagnostics;
        const bool bParsed = FPwSkelParser::Parse(Source, OutDocument, Diagnostics);
        if (!bParsed)
        {
            for (const FPwDiagnostic& Diagnostic : Diagnostics)
            {
                Test.AddError(Diagnostic.ToString());
            }
        }
        return bParsed;
    }

    FString UniquePath(const TCHAR* Stem)
    {
        return FString::Printf(TEXT("/Engine/Transient/%s_%s"), Stem,
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }

    USkeletalMesh* MakeCompatibleMesh(const FString& Path, USkeleton* Skeleton)
    {
        UPackage* Package = CreatePackage(*Path);
        USkeletalMesh* Mesh = NewObject<USkeletalMesh>(Package,
            FName(*FPackageName::GetLongPackageAssetName(Path)), RF_Public | RF_Standalone);
        if (Mesh)
        {
            Mesh->SetSkeleton(Skeleton);
        }
        return Mesh;
    }

    bool AddCurve(USkeleton* Skeleton)
    {
        const FName CurveName(TEXT("MotionAmount"));
        if (!MCP_ADD_CURVE_META_DATA(Skeleton, CurveName))
        {
            return false;
        }
        UAnimCurveMetaData* MetaData = Skeleton->GetAssetUserData<UAnimCurveMetaData>();
        if (!MetaData)
        {
            return false;
        }
        // USkeleton only gained the forwarding setter in UE 5.8.
        MetaData->SetCurveMetaDataMaterial(CurveName, true);
        TArray<FBoneReference> Links;
        FBoneReference& Link = Links.AddDefaulted_GetRef();
        Link.BoneName = FName(TEXT("root"));
        Link.Initialize(Skeleton);
        MetaData->SetCurveMetaDataBoneLinks(CurveName, Links, 2, Skeleton);
        return true;
    }

    FSkeletonCreateSpec Spec(const FString& AssetPath, bool bOverwrite = false)
    {
        FSkeletonCreateSpec Result;
        Result.AssetPath = AssetPath;
        Result.SourcePath = TEXT("Tests/RecompileState.pwskel");
        Result.SourceHash = TEXT("test");
        Result.bOverwrite = bOverwrite;
        Result.bSave = false;
        return Result;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelRecompileStateMustBeDescribedTest,
    "PinWright.Skeleton.Recompile.OutOfBandStateMustBeDescribed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelRecompileStateMustBeDescribedTest::RunTest(const FString& Parameters)
{
    using namespace PwSkelRecompileStateTests;
    const FString SkeletonPath = UniquePath(TEXT("PW_SkeletonState"));
    const FString PreviewMeshPath = UniquePath(TEXT("PW_PreviewMesh"));
    // Both fixtures are RF_Standalone, so the periodic suite GC keeps them alive; detach them so
    // they cannot answer a later /Engine/Transient asset-registry rescan.
    // Preview mesh first: it holds the skeleton reference.
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(PreviewMeshPath);
        CleanupTestAsset(SkeletonPath);
    };

    FPwSkelDocument Original;
    if (!Parse(TEXT("pwskel 0\nbone \"root\" {\n  bone \"child\" { }\n}\n"), Original, *this))
    {
        return false;
    }

    FSkeletonCreateResult First = CreateSkeleton(Original, Spec(SkeletonPath));
    if (!TestTrue(TEXT("first compile succeeds"), First.bSuccess) ||
        !TestNotNull(TEXT("first compile returns a skeleton"), First.Asset))
    {
        return false;
    }

    USkeletalMesh* Preview = MakeCompatibleMesh(PreviewMeshPath, First.Asset);
    if (!TestNotNull(TEXT("compatible preview mesh created"), Preview))
    {
        return false;
    }
    First.Asset->SetPreviewMesh(Preview, false);
    First.Asset->SetBoneTranslationRetargetingMode(
        0, EBoneTranslationRetargetingMode::Skeleton, false);
    if (!TestTrue(TEXT("curve metadata mutation applied"), AddCurve(First.Asset)))
    {
        return false;
    }

    const FSkeletonCreateResult Refused = CreateSkeleton(Original, Spec(SkeletonPath));
    TestFalse(TEXT("source that omits the live state is refused"), Refused.bSuccess);
    TestTrue(TEXT("refusal uses the shared exact diagnostic code"),
        HasCode(Refused.Diagnostics,
            PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE));
    if (const FPwDiagnostic* Diagnostic = FindCode(Refused.Diagnostics,
            PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE))
    {
        TestEqual(TEXT("default loss diagnostic is an error"),
            Diagnostic->Severity, EPwSeverity::Error);
    }
    TestEqual(TEXT("refusal does not clear the preview mesh"),
        static_cast<const USkeleton*>(First.Asset)->GetPreviewMesh(), Preview);

    const FString DescribedSource = FString::Printf(
        TEXT("pwskel 0\npreview_mesh path=\"%s\"\n")
        TEXT("curve \"MotionAmount\" material=true max_lod=2 {\n")
        TEXT("  linked_bone \"root\"\n}\n")
        TEXT("bone \"root\" retarget=skeleton {\n  bone \"child\" { }\n}\n"),
        *Preview->GetPathName());
    FPwSkelDocument Described;
    if (!Parse(DescribedSource, Described, *this))
    {
        return false;
    }

    const FSkeletonCreateResult Clean = CreateSkeleton(Described, Spec(SkeletonPath));
    TestTrue(TEXT("the same live state is clean once source describes it"), Clean.bSuccess);
    TestFalse(TEXT("a described state does not raise the loss code"),
        HasCode(Clean.Diagnostics,
            PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE));
    TestEqual(TEXT("preview mesh regenerates from source"),
        static_cast<const USkeleton*>(Clean.Asset)->GetPreviewMesh(), Preview);
    TestEqual(TEXT("retarget mode regenerates from source"),
        Clean.Asset->GetBoneTranslationRetargetingMode(0),
        EBoneTranslationRetargetingMode::Skeleton);
    TestNotNull(TEXT("curve metadata regenerates from source"),
        Clean.Asset->GetCurveMetaData(FName(TEXT("MotionAmount"))));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelRecompileOverwriteIsVisibleTest,
    "PinWright.Skeleton.Recompile.OverwriteIsVisibleAndDeterministic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelRecompileOverwriteIsVisibleTest::RunTest(const FString& Parameters)
{
    using namespace PwSkelRecompileStateTests;
    const FString SkeletonPath = UniquePath(TEXT("PW_SkeletonOverwrite"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SkeletonPath);
    };

    FPwSkelDocument Document;
    if (!Parse(TEXT("pwskel 0\nbone \"root\" { }\n"), Document, *this))
    {
        return false;
    }
    FSkeletonCreateResult First = CreateSkeleton(Document, Spec(SkeletonPath));
    if (!TestTrue(TEXT("first compile succeeds"), First.bSuccess))
    {
        return false;
    }

    USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(First.Asset);
    Socket->SocketName = FName(TEXT("Attachment"));
    Socket->BoneName = FName(TEXT("root"));
    First.Asset->Sockets.Add(Socket);

    const FSkeletonCreateResult Refused = CreateSkeleton(Document, Spec(SkeletonPath));
    TestFalse(TEXT("unexpressible socket blocks default recompile"), Refused.bSuccess);
    TestTrue(TEXT("default refusal names the exact code"), HasCode(Refused.Diagnostics,
        PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE));

    const FSkeletonCreateResult Allowed = CreateSkeleton(Document, Spec(SkeletonPath, true));
    TestTrue(TEXT("overwrite explicitly permits the deterministic rebuild"), Allowed.bSuccess);
    TestTrue(TEXT("permitted loss remains visible with the same code"),
        HasCode(Allowed.Diagnostics,
            PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE));
    if (const FPwDiagnostic* Diagnostic = FindCode(Allowed.Diagnostics,
            PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE))
    {
        TestEqual(TEXT("explicitly permitted loss is a warning"),
            Diagnostic->Severity, EPwSeverity::Warning);
    }
    TestEqual(TEXT("the unsupported socket is not silently preserved"),
        Allowed.Asset->Sockets.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwSkelTakeoverNamesUnmanagedStateTest,
    "PinWright.Skeleton.Recompile.TakeoverNamesUnmanagedState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwSkelTakeoverNamesUnmanagedStateTest::RunTest(const FString& Parameters)
{
    using namespace PwSkelRecompileStateTests;
    const FString SkeletonPath = UniquePath(TEXT("PW_SkeletonTakeover"));
    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SkeletonPath);
    };

    FPwSkelDocument Document;
    if (!Parse(TEXT("pwskel 0\nbone \"root\" { }\n"), Document, *this))
    {
        return false;
    }

    FSkeletonCreateSpec OriginalSpec = Spec(SkeletonPath);
    OriginalSpec.SourcePath = TEXT("Tests/Owners/original.pwskel");
    FSkeletonCreateResult First = CreateSkeleton(Document, OriginalSpec);
    if (!TestTrue(TEXT("the original source creates the asset"), First.bSuccess)
        || !TestNotNull(TEXT("the created skeleton is available"), First.Asset))
    {
        return false;
    }

    USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(First.Asset);
    Socket->SocketName = FName(TEXT("Attachment"));
    Socket->BoneName = FName(TEXT("root"));
    First.Asset->Sockets.Add(Socket);

    FSkeletonCreateSpec TakeoverSpec = Spec(SkeletonPath);
    TakeoverSpec.SourcePath = TEXT("Tests/Owners/replacement.pwskel");
    const FSkeletonCreateResult Refused = CreateSkeleton(Document, TakeoverSpec);
    TestFalse(TEXT("a different source still needs takeover permission"), Refused.bSuccess);
    TestEqual(TEXT("the outer refusal remains the ownership result"), Refused.ErrorCode,
        FString(TEXT("ASSET_ALREADY_EXISTS")));
    const FPwDiagnostic* RefusalDiagnostic = FindCode(Refused.Diagnostics,
        PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE);
    TestNotNull(TEXT("the takeover refusal carries the exact guard code"), RefusalDiagnostic);
    if (RefusalDiagnostic)
    {
        TestEqual(TEXT("the blocked takeover reports an error"),
            RefusalDiagnostic->Severity, EPwSeverity::Error);
        TestTrue(TEXT("the refusal names the socket that would be lost"),
            RefusalDiagnostic->Message.Contains(TEXT("socket[Attachment]")));
        TestTrue(TEXT("the ownership refusal includes the named unmanaged state"),
            Refused.ErrorMessage.Contains(TEXT("socket[Attachment]")));
    }

    TakeoverSpec.bOverwrite = true;
    const FSkeletonCreateResult Allowed = CreateSkeleton(Document, TakeoverSpec);
    TestTrue(TEXT("overwrite=true permits the takeover"), Allowed.bSuccess);
    const FPwDiagnostic* WarningDiagnostic = FindCode(Allowed.Diagnostics,
        PwSourceDiagnosticCodes::PWSRC_RECOMPILE_UNMANAGED_STATE);
    TestNotNull(TEXT("the permitted takeover keeps the exact guard code visible"),
        WarningDiagnostic);
    if (WarningDiagnostic)
    {
        TestEqual(TEXT("the permitted takeover reports a warning"),
            WarningDiagnostic->Severity, EPwSeverity::Warning);
        TestTrue(TEXT("the warning names the socket that was discarded"),
            WarningDiagnostic->Message.Contains(TEXT("socket[Attachment]")));
    }
    TestEqual(TEXT("the takeover does not silently preserve the unmanaged socket"),
        Allowed.Asset->Sockets.Num(), 0);
    return true;
}
