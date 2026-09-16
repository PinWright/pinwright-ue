// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression: character.configure_sprint must WRITE the passed sprintSpeed onto
// the SprintSpeed member variable's default, not just onto the movement CDO.
// Guards B-configure-sprint-speed-var-unset. The handler creates a "Sprint Speed"
// float variable (CharacterHandler.cpp L1244) but never calls SetBPVarDefaultValue
// on it — the passed speed only lands on MaxCustomMovementSpeed (L1252), so the
// created variable keeps its empty (zero) default. A blueprint graph reading the
// SprintSpeed variable therefore sprints at zero speed — silent wrong data on the
// exact variable the method exists to populate.
//
// This mirrors the sibling FCharacterFootstepDefaultsPersistedTest pattern: it
// drives the real handler against a persisted ACharacter blueprint and asserts the
// variable's stored default carries the configured speed. It fails pre-fix (the
// default is left empty) and passes once configure_sprint sets the default like its
// sibling add_custom_movement_mode does for its <Mode>Speed variable.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"
#include "Tests/TestUtils.h"
#include "Tests/Bpir/CompilerTestUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Character.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
    // Creates a real, persisted ACharacter blueprint the path-based handlers can
    // LoadObject by path (CompilerTestUtils only makes transient BPs, which these
    // handlers can't resolve). Uniquely named to avoid a Unity ODR collision with
    // TestCharacterHandlers.cpp's anonymous-namespace CreatePersistedCharacterBP.
    // Writes the package path to OutPackagePath; the caller owns
    // CleanupTestAsset(OutPackagePath).
    UBlueprint* CreatePersistedSprintCharacterBP(FAutomationTestBase& Test, FString& OutPackagePath)
    {
        OutPackagePath = FString::Printf(
            TEXT("/Game/__PW_GatewayTests/SprintSpeedDefault_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        UPackage* Pkg = CreatePackage(*OutPackagePath);
        Test.TestNotNull(TEXT("package created"), Pkg);
        if (!Pkg) return nullptr;

        UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
            ACharacter::StaticClass(), Pkg,
            FName(*FPackageName::GetLongPackageAssetName(OutPackagePath)),
            BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
        Test.TestNotNull(TEXT("character blueprint created"), BP);
        return BP;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCharacterConfigureSprintSpeedDefaultPersistedTest,
    "PinWright.character.configure_sprint.SprintSpeedDefaultPersisted",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCharacterConfigureSprintSpeedDefaultPersistedTest::RunTest(const FString& Parameters)
{
    // Create a real ACharacter blueprint the handler can LoadObject by path.
    FString PackagePath;
    UBlueprint* BP = CreatePersistedSprintCharacterBP(*this, PackagePath);
    if (!BP)
    {
        AddError(TEXT("Required fixture ACharacter blueprint could not be created"));
        CleanupTestAsset(PackagePath);
        return true;
    }

    // Configure sprint with a distinctive, non-default speed. Deliberately NOT 900
    // (the handler's own Ctx.GetNumber fallback) so the assertion proves the PASSED
    // argument is what lands on the variable default — not a hardcoded/defaulted 900.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("blueprintPath"), PackagePath);
        Payload->SetNumberField(TEXT("sprintSpeed"), 750.0);
        TestTrue(TEXT("configure_sprint handler found"),
            InvokeHandler(TEXT("character.configure_sprint"), Payload));
    }

    // The SprintSpeed member variable must exist AND carry the configured speed as
    // its default. Pre-fix the variable is created but its default is left empty
    // (the float zero default), so both value assertions below fail.
    const FBPVariableDescription* SprintVar =
        CompilerTestUtils::FindNewVariableByName(BP, TEXT("SprintSpeed"));
    TestNotNull(TEXT("SprintSpeed member variable was created"), SprintVar);
    if (SprintVar)
    {
        const FString SprintDefault = SprintVar->DefaultValue;
        TestTrue(TEXT("SprintSpeed default is stored (not empty/zero)"),
            !SprintDefault.IsEmpty());
        TestTrue(TEXT("SprintSpeed default carries the configured 750"),
            SprintDefault.Contains(TEXT("750")));
    }

    CleanupTestAsset(PackagePath);
    return true;
}
