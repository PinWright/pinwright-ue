// Copyright (c) 2026 Alexander Penkin. MIT License.

// Contract tests for PinWright::PackageDirty and its reflected skin
// UPinWrightPackageLibrary (board ticket E-python-cannot-mark-package-dirty).
//
// The property under test is not "the call returned true" but "the package is
// observably dirty afterwards". UObject::MarkPackageDirty() returns true for a
// transient object and for an object with no package (UObjectBaseUtility.cpp:244,283)
// without dirtying anything, so a test that trusted the return value would pass
// against a completely broken implementation.
//
// Every test that dirties a package clears the flag again before returning: a stray
// dirty /Temp package would be picked up by FEditorFileUtils' dirty-package walk and
// surface in editor.save_all / the quit prompt for the rest of the session.

#include "Misc/AutomationTest.h"

#include "PinWrightPackageLibrary.h"
#include "Utils/PackageDirtyUtils.h"

#include "McpGenericDataAsset.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace PinWrightPackageDirtyTest
{
    // Scratch in-memory package under /Temp. Rooted so a stray GC cannot collect it
    // mid-test, un-dirtied and unrooted on scope exit so the session is left clean.
    struct FScratchPackage
    {
        UPackage* Package = nullptr;
        FString Name;

        FScratchPackage()
        {
            Name = FString::Printf(TEXT("/Temp/PinWrightMarkDirtyTest_%s"),
                *FGuid::NewGuid().ToString(EGuidFormats::Digits));
            Package = CreatePackage(*Name);
            if (Package)
            {
                Package->AddToRoot();
                // Deterministic starting point: CreatePackage is not documented to
                // leave the flag in any particular state.
                Package->SetDirtyFlag(false);
            }
        }

        ~FScratchPackage()
        {
            if (Package)
            {
                Package->SetDirtyFlag(false);
                Package->RemoveFromRoot();
            }
        }

        FScratchPackage(const FScratchPackage&) = delete;
        FScratchPackage& operator=(const FScratchPackage&) = delete;
    };
}

// ============================================================================
// Marking a clean package
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPackageDirtyMarksCleanPackageTest,
    "PinWright.core.package_dirty.MarkCleanPackageSetsDirty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPackageDirtyMarksCleanPackageTest::RunTest(const FString& Parameters)
{
    using namespace PinWright::PackageDirty;

    PinWrightPackageDirtyTest::FScratchPackage Scratch;
    if (!TestNotNull(TEXT("Scratch package created"), Scratch.Package))
    {
        return false;
    }
    TestFalse(TEXT("Scratch package starts clean"), Scratch.Package->IsDirty());

    const FOutcome First = MarkPackageDirty(Scratch.Package);
    TestEqual(TEXT("First mark reports Dirtied"),
        static_cast<int32>(First.Status), static_cast<int32>(EMarkDirtyStatus::Dirtied));
    TestTrue(TEXT("First mark reports dirty now"), First.IsDirtyNow());
    TestEqual(TEXT("Outcome carries the resolved package name"), First.PackageName, Scratch.Name);
    TestTrue(TEXT("Reason is empty on success"), First.Reason.IsEmpty());

    // The load-bearing assertion: the flag actually took on the package itself.
    TestTrue(TEXT("UPackage::IsDirty() is true after marking"), Scratch.Package->IsDirty());

    // Re-marking an already-dirty package is a no-op that still reports the
    // post-condition, so idempotent scripts do not read it as a failure.
    const FOutcome Second = MarkPackageDirty(Scratch.Package);
    TestEqual(TEXT("Second mark reports AlreadyDirty"),
        static_cast<int32>(Second.Status), static_cast<int32>(EMarkDirtyStatus::AlreadyDirty));
    TestTrue(TEXT("Second mark still reports dirty now"), Second.IsDirtyNow());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPackageDirtyMarksObjectPackageTest,
    "PinWright.core.package_dirty.MarkObjectDirtiesItsPackage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPackageDirtyMarksObjectPackageTest::RunTest(const FString& Parameters)
{
    using namespace PinWright::PackageDirty;

    PinWrightPackageDirtyTest::FScratchPackage Scratch;
    if (!TestNotNull(TEXT("Scratch package created"), Scratch.Package))
    {
        return false;
    }

    UMcpGenericDataAsset* Asset =
        NewObject<UMcpGenericDataAsset>(Scratch.Package, TEXT("PinWrightMarkDirtyProbe"));
    if (!TestNotNull(TEXT("Probe object created in the scratch package"), Asset))
    {
        return false;
    }
    // NewObject dirties nothing on its own; re-establish the clean baseline so the
    // transition being asserted is caused by MarkObjectDirty alone.
    Scratch.Package->SetDirtyFlag(false);

    TestTrue(TEXT("No blocker for an ordinary object"), DescribeBlocker(Asset).IsEmpty());
    TestEqual(TEXT("GetPackageName resolves the owning package"),
        GetPackageName(Asset), Scratch.Name);
    TestFalse(TEXT("IsObjectPackageDirty is false before marking"), IsObjectPackageDirty(Asset));

    const FOutcome Outcome = MarkObjectDirty(Asset);
    TestTrue(TEXT("MarkObjectDirty succeeded"), Outcome.IsDirtyNow());
    TestTrue(TEXT("Owning package is dirty"), Scratch.Package->IsDirty());
    TestTrue(TEXT("IsObjectPackageDirty agrees"), IsObjectPackageDirty(Asset));

    return true;
}

// ============================================================================
// Refusals: null, garbage, transient
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPackageDirtyNullObjectRefusedTest,
    "PinWright.core.package_dirty.NullObjectRefusedWithoutCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPackageDirtyNullObjectRefusedTest::RunTest(const FString& Parameters)
{
    using namespace PinWright::PackageDirty;

    const FOutcome Outcome = MarkObjectDirty(nullptr);
    TestFalse(TEXT("Null object is not reported dirty"), Outcome.IsDirtyNow());
    TestEqual(TEXT("Null object is Refused"),
        static_cast<int32>(Outcome.Status), static_cast<int32>(EMarkDirtyStatus::Refused));
    TestFalse(TEXT("Refusal carries a printable reason"), Outcome.Reason.IsEmpty());

    TestFalse(TEXT("DescribeBlocker reports a blocker for null"), DescribeBlocker(nullptr).IsEmpty());
    TestFalse(TEXT("IsObjectPackageDirty is false for null"), IsObjectPackageDirty(nullptr));
    TestTrue(TEXT("GetPackageName is empty for null"), GetPackageName(nullptr).IsEmpty());

    // Same contract through the reflected surface Python actually calls.
    TestFalse(TEXT("Library MarkPackageDirty(nullptr) is false"),
        UPinWrightPackageLibrary::MarkPackageDirty(nullptr));
    TestFalse(TEXT("Library MarkActorPackageDirty(nullptr) is false"),
        UPinWrightPackageLibrary::MarkActorPackageDirty(nullptr));
    TestFalse(TEXT("Library IsPackageDirty(nullptr) is false"),
        UPinWrightPackageLibrary::IsPackageDirty(nullptr));
    TestFalse(TEXT("Library DescribeMarkDirtyBlocker(nullptr) is non-empty"),
        UPinWrightPackageLibrary::DescribeMarkDirtyBlocker(nullptr).IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPackageDirtyTransientObjectRefusedTest,
    "PinWright.core.package_dirty.TransientObjectRefusedAndTransientPackageStaysClean",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPackageDirtyTransientObjectRefusedTest::RunTest(const FString& Parameters)
{
    using namespace PinWright::PackageDirty;

    UMcpGenericDataAsset* Transient = NewObject<UMcpGenericDataAsset>(GetTransientPackage());
    if (!TestNotNull(TEXT("Transient probe object created"), Transient))
    {
        return false;
    }

    const FOutcome Outcome = MarkObjectDirty(Transient);
    // This is the case UObject::MarkPackageDirty() returns TRUE for while doing
    // nothing (UObjectBaseUtility.cpp:242-245). Forwarding that would be the exact
    // silent-success failure this surface exists to prevent.
    TestFalse(TEXT("Transient object is refused, not reported dirtied"), Outcome.IsDirtyNow());
    TestFalse(TEXT("Refusal carries a printable reason"), Outcome.Reason.IsEmpty());
    TestFalse(TEXT("Library agrees"), UPinWrightPackageLibrary::MarkPackageDirty(Transient));

    TestFalse(TEXT("The transient package was not dirtied"), GetTransientPackage()->IsDirty());

    return true;
}

// ============================================================================
// Path resolution
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPackageDirtyBadPathRefusedTest,
    "PinWright.core.package_dirty.BadPathRefusedWithoutCrash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPackageDirtyBadPathRefusedTest::RunTest(const FString& Parameters)
{
    using namespace PinWright::PackageDirty;

    const TArray<FString> BadPaths = {
        TEXT(""),                                   // empty
        TEXT("   "),                                // whitespace only
        TEXT("not a path"),                         // no leading slash
        TEXT("C:/Windows/System32"),                // filesystem path
        TEXT("/Game/PinWright/DefinitelyNotLoaded_zzz"), // well-formed but not loaded
    };

    for (const FString& BadPath : BadPaths)
    {
        FString Reason;
        UPackage* Resolved = FindLoadedPackageForPath(BadPath, Reason);
        TestNull(*FString::Printf(TEXT("'%s' resolves to no package"), *BadPath), Resolved);
        TestFalse(*FString::Printf(TEXT("'%s' yields a printable reason"), *BadPath), Reason.IsEmpty());

        const FOutcome Outcome = MarkPathDirty(BadPath);
        TestFalse(*FString::Printf(TEXT("'%s' is not reported dirty"), *BadPath), Outcome.IsDirtyNow());
        TestFalse(*FString::Printf(TEXT("'%s' refusal has a reason"), *BadPath), Outcome.Reason.IsEmpty());

        TestFalse(*FString::Printf(TEXT("Library MarkPackageDirtyByPath('%s') is false"), *BadPath),
            UPinWrightPackageLibrary::MarkPackageDirtyByPath(BadPath));
        TestFalse(*FString::Printf(TEXT("Library IsPackageDirtyByPath('%s') is false"), *BadPath),
            UPinWrightPackageLibrary::IsPackageDirtyByPath(BadPath));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPackageDirtyPathFormsAcceptedTest,
    "PinWright.core.package_dirty.PackageAndObjectPathFormsResolveAlike",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPackageDirtyPathFormsAcceptedTest::RunTest(const FString& Parameters)
{
    using namespace PinWright::PackageDirty;

    PinWrightPackageDirtyTest::FScratchPackage Scratch;
    if (!TestNotNull(TEXT("Scratch package created"), Scratch.Package))
    {
        return false;
    }

    FString Reason;
    TestTrue(TEXT("Package path resolves"),
        FindLoadedPackageForPath(Scratch.Name, Reason) == Scratch.Package);

    // "/Temp/Pkg.Object" and "/Temp/Pkg.Object:Sub" must both fold to the package.
    const FString ObjectPath = Scratch.Name + TEXT(".SomeObject");
    TestTrue(TEXT("Object path folds to the same package"),
        FindLoadedPackageForPath(ObjectPath, Reason) == Scratch.Package);
    TestTrue(TEXT("Subobject path folds to the same package"),
        FindLoadedPackageForPath(ObjectPath + TEXT(":Sub"), Reason) == Scratch.Package);

    TestFalse(TEXT("Package starts clean"), Scratch.Package->IsDirty());
    TestTrue(TEXT("MarkPathDirty via object path succeeds"), MarkPathDirty(ObjectPath).IsDirtyNow());
    TestTrue(TEXT("Package is dirty afterwards"), Scratch.Package->IsDirty());
    TestTrue(TEXT("IsPackageDirtyByPath agrees"),
        UPinWrightPackageLibrary::IsPackageDirtyByPath(Scratch.Name));

    return true;
}
