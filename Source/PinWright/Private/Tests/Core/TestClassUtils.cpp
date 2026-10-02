// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for ClassUtils: ResolveClassByName
#include "Misc/AutomationTest.h"
#include "PinWrightHelpers.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "Camera/CameraActor.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"


// ============================================================================
// ResolveClassByName - exact name
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveClassExactNameTest,
    "PinWright.core.class.resolve_class_by_name.ExactName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolveClassExactNameTest::RunTest(const FString& Parameters)
{
    // "Actor" should resolve to AActor (common engine class)
    UClass* Found = ResolveClassByName(TEXT("Actor"));
    TestTrue(TEXT("Actor class resolved"), Found != nullptr);
    if (Found)
    {
        TestEqual(TEXT("Found class is AActor"), Found->GetName(), TEXT("Actor"));
    }
    return true;
}

// ============================================================================
// ResolveClassByName - short name (component)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveClassShortNameTest,
    "PinWright.core.class.resolve_class_by_name.ShortName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolveClassShortNameTest::RunTest(const FString& Parameters)
{
    // "StaticMeshComponent" should resolve by short name
    UClass* Found = ResolveClassByName(TEXT("StaticMeshComponent"));
    TestTrue(TEXT("StaticMeshComponent resolved"), Found != nullptr);
    if (Found)
    {
        TestTrue(TEXT("Found class is or derives from UActorComponent"),
            Found->IsChildOf(UActorComponent::StaticClass()));
    }
    return true;
}

// ============================================================================
// ResolveClassByName - full script path
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveClassFullPathTest,
    "PinWright.core.class.resolve_class_by_name.FullScriptPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolveClassFullPathTest::RunTest(const FString& Parameters)
{
    // Full /Script/ path should resolve directly
    UClass* Found = ResolveClassByName(TEXT("/Script/Engine.CameraActor"));
    TestTrue(TEXT("CameraActor resolved via full path"), Found != nullptr);
    if (Found)
    {
        TestEqual(TEXT("Class name matches"), Found->GetName(), TEXT("CameraActor"));
    }
    return true;
}

// ============================================================================
// ResolveClassByName - empty/unknown
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveClassEmptyTest,
    "PinWright.core.class.resolve_class_by_name.EmptyReturnsNull",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolveClassEmptyTest::RunTest(const FString& Parameters)
{
    UClass* Found = ResolveClassByName(TEXT(""));
    TestTrue(TEXT("Empty input returns null"), Found == nullptr);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveClassUnknownTest,
    "PinWright.core.class.resolve_class_by_name.UnknownReturnsNull",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FResolveClassUnknownTest::RunTest(const FString& Parameters)
{
    UClass* Found = ResolveClassByName(TEXT("NonExistentClass_XYZ_9999"));
    TestTrue(TEXT("Unknown class returns null"), Found == nullptr);
    return true;
}

// ============================================================================
// ResolveUClass — regression tests for E-class-name-format-inconsistency and
// B-inspect-class-short-name-fails. The fix unifies resolution of short names,
// U/A-prefixed short names, /Script/ paths, and content-mount BP paths
// (/Game/, /Engine/, plugin mounts), returning GeneratedClass when the
// path points at a UBlueprint asset.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveUClassShortNameTest,
    "PinWright.core.class.resolve_uclass.ShortName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FResolveUClassShortNameTest::RunTest(const FString& Parameters)
{
    UClass* Found = ResolveUClass(TEXT("StaticMeshComponent"));
    TestNotNull(TEXT("StaticMeshComponent resolved by short name"), Found);
    if (Found)
    {
        TestTrue(TEXT("Resolved class derives from UActorComponent"),
            Found->IsChildOf(UActorComponent::StaticClass()));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveUClassUPrefixTest,
    "PinWright.core.class.resolve_uclass.UPrefixShortName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FResolveUClassUPrefixTest::RunTest(const FString& Parameters)
{
    UClass* Found = ResolveUClass(TEXT("UStaticMeshComponent"));
    TestNotNull(TEXT("UStaticMeshComponent resolved (U-prefix stripped)"), Found);
    if (Found)
    {
        TestTrue(TEXT("Resolved class derives from UActorComponent"),
            Found->IsChildOf(UActorComponent::StaticClass()));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveUClassFullScriptPathTest,
    "PinWright.core.class.resolve_uclass.FullScriptPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FResolveUClassFullScriptPathTest::RunTest(const FString& Parameters)
{
    UClass* Found = ResolveUClass(TEXT("/Script/Engine.CameraActor"));
    TestNotNull(TEXT("CameraActor resolved via /Script/ path"), Found);
    if (Found)
    {
        TestEqual(TEXT("Short class name matches"), Found->GetName(), FString(TEXT("CameraActor")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveUClassEngineBPPathTest,
    "PinWright.core.class.resolve_uclass.ContentMountPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FResolveUClassEngineBPPathTest::RunTest(const FString& Parameters)
{
    // An /Engine/ content-mount BP path exercises the new content-mount branch:
    // LoadObject<UClass> fails, then the code tries <Path>_C and finally falls back
    // to loading a UBlueprint and returning its GeneratedClass.
    UClass* Found = ResolveUClass(TEXT("/Engine/EditorBlueprintResources/StandardMacros"));
    TestNotNull(TEXT("StandardMacros engine-mount BP path resolved to a UClass"), Found);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveUClassNonExistentContentPathTest,
    "PinWright.core.class.resolve_uclass.NonExistentContentPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FResolveUClassNonExistentContentPathTest::RunTest(const FString& Parameters)
{
    // A qualified content-mount path that does not resolve must early-return nullptr,
    // NOT fall through to short-name iteration (which could pick up an unrelated class
    // whose name happens to be the package leaf).
    UClass* Found = ResolveUClass(TEXT("/Game/PinWrightTests/_Test/DoesNotExist_XYZ"));
    TestNull(TEXT("Non-existent content-mount path returns null"), Found);
    return true;
}

// ============================================================================
// ResolveUClass / ResolveClassByName — B-resolve-uclass-short-name-load-warns.
// A bare short name used to reach LoadObject<UClass> (ResolveUClass step 2 on the raw name, and
// both resolvers on /Script/<Pkg>.<Name>), which can never load and logged
// "LogUObjectGlobals: Warning: Failed to find object 'Class Object'" on every successful lookup.
// A log watch counts the line because a Warning only fails a test on hosts that elevate it.
// Discrimination: the step-2 load has a null outer, so it warns on EVERY call and a revert goes red
// here every run. The /Script/ loads mark their package known-missing after the first miss
// (UObjectGlobals.cpp StaticLoadObject), so a revert of only those is red only on the session's
// first such miss.
// ============================================================================
namespace ResolveUClassNoLoadWarnTest
{
    class FNeedleLogWatch : public FOutputDevice
    {
    public:
        explicit FNeedleLogWatch(const TCHAR* InNeedle) : Needle(InNeedle)
        {
            GLog->AddOutputDevice(this);
        }
        virtual ~FNeedleLogWatch() override
        {
            GLog->RemoveOutputDevice(this);
        }
        virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity,
            const FName& Category) override
        {
            if (FCString::Strstr(Message, *Needle))
            {
                FPlatformAtomics::InterlockedIncrement(&Hits);
            }
        }
        virtual bool CanBeUsedOnAnyThread() const override { return true; }
        virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
        int32 GetHits() const { return Hits; }

    private:
        FString Needle;
        volatile int32 Hits = 0;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveUClassBareNamesNoLoadWarningTest,
    "PinWright.core.class.resolve_uclass.BareNamesLogNoLoadWarning",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FResolveUClassBareNamesNoLoadWarningTest::RunTest(const FString& Parameters)
{
    const TCHAR* Names[] = {TEXT("Object"), TEXT("Actor"), TEXT("PointLight")};
    int32 Hits = 0;
    {
        ResolveUClassNoLoadWarnTest::FNeedleLogWatch Watch(TEXT("Failed to find object"));
        for (const TCHAR* Name : Names)
        {
            UClass* ByUClass = ResolveUClass(Name);
            TestNotNull(FString::Printf(TEXT("ResolveUClass resolves '%s'"), Name), ByUClass);
            if (ByUClass)
            {
                TestEqual(TEXT("ResolveUClass short name matches"), ByUClass->GetName(), FString(Name));
            }
            UClass* ByName = ResolveClassByName(Name);
            TestNotNull(FString::Printf(TEXT("ResolveClassByName resolves '%s'"), Name), ByName);
            if (ByName)
            {
                TestEqual(TEXT("ResolveClassByName short name matches"), ByName->GetName(), FString(Name));
            }
        }
        GLog->Flush();
        Hits = Watch.GetHits();
    }
    TestEqual(TEXT("no 'Failed to find object' load warning while resolving bare short names"),
        Hits, 0);
    return true;
}
