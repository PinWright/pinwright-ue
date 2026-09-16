// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Tests/Assets/TestPropertyExportNoiseFiltering.h"

#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"
#include "Utils/PropertyExport.h"
#include "UObject/UnrealType.h"

namespace
{
    FProperty* FindReflectedProperty(const TCHAR* OwnerPath, const TCHAR* PropertyName)
    {
        UStruct* Owner = FindObject<UStruct>(nullptr, OwnerPath);
        return Owner ? FindFProperty<FProperty>(Owner, PropertyName) : nullptr;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyExportNestedNoiseFlagsTest,
    "PinWright.Assets.PropertyExport.NestedNoiseFlags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyExportNestedNoiseFlagsTest::RunTest(const FString& Parameters)
{
    UTestPropertyExportNoiseHost* Host = NewObject<UTestPropertyExportNoiseHost>();
    FStructProperty* ValueProperty = FindFProperty<FStructProperty>(
        UTestPropertyExportNoiseHost::StaticClass(),
        GET_MEMBER_NAME_CHECKED(UTestPropertyExportNoiseHost, Value));
    if (!TestNotNull(TEXT("Fixture struct property resolved"), ValueProperty))
    {
        return false;
    }

    FProperty* DuplicateTransientProperty = FindFProperty<FProperty>(
        FPropertyExportNoiseFixture::StaticStruct(),
        GET_MEMBER_NAME_CHECKED(FPropertyExportNoiseFixture, DuplicateTransientValue));
    if (!TestNotNull(TEXT("DuplicateTransient fixture property resolved"), DuplicateTransientProperty))
    {
        return false;
    }
    DuplicateTransientProperty->SetPropertyFlags(CPF_DuplicateTransient);

    const TSharedPtr<FJsonValue> Value = ExportPropertyToJsonValue(Host, ValueProperty);
    const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
    if (!TestTrue(TEXT("Nested struct exported as object"), Object.IsValid()))
    {
        return false;
    }

    TestTrue(TEXT("Stable nested field remains"), Object->HasField(TEXT("StableValue")));
    TestTrue(TEXT("Same-name field on unrelated owner remains"), Object->HasField(TEXT("Signature")));
    TestFalse(TEXT("Transient nested field omitted"), Object->HasField(TEXT("TransientValue")));
    TestFalse(TEXT("DuplicateTransient nested field omitted"), Object->HasField(TEXT("DuplicateTransientValue")));
    TestFalse(TEXT("SkipSerialization nested field omitted"), Object->HasField(TEXT("SkipSerializationValue")));
    TestFalse(TEXT("Deprecated nested field omitted"), Object->HasField(TEXT("DeprecatedValue_DEPRECATED")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyExportKnownDerivedOwnerFieldsTest,
    "PinWright.Assets.PropertyExport.KnownDerivedOwnerFields",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyExportKnownDerivedOwnerFieldsTest::RunTest(const FString& Parameters)
{
    struct FCase
    {
        const TCHAR* OwnerPath;
        const TCHAR* PropertyName;
    };

    const FCase Cases[] =
    {
        { TEXT("/Script/Engine.StaticMeshSourceModel"), TEXT("CacheMeshDescriptionTrianglesCount") },
        { TEXT("/Script/Engine.StaticMeshSourceModel"), TEXT("CacheMeshDescriptionVerticesCount") },
        { TEXT("/Script/MovieScene.MovieSceneSignedObject"), TEXT("Signature") },
        { TEXT("/Script/Engine.MaterialInterface"), TEXT("TextureStreamingData") },
        // UNiagaraSystem::ScriptRuntimeCompiledDataForEditor was added in UE 5.8; on 5.3-5.7 the
        // field does not exist to be filtered, so asserting it resolves would fail on a property
        // the engine never had. The skip rule itself matches by name and needs no guard.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
        { TEXT("/Script/Niagara.NiagaraSystem"), TEXT("ScriptRuntimeCompiledDataForEditor") },
#endif
        { TEXT("/Script/Niagara.NiagaraSystem"), TEXT("SystemCompiledData") },
        // A material instance's per-parameter ExpressionGUID is the cached link to the
        // parent material's expression: all-zero as serialized until something reconciles
        // the instance against its parent (material editor, update_material_instance, a
        // parent change), resolved afterwards. Whichever value a dump captured recorded the
        // editor session rather than the package, so it is not authored state.
        // FStaticParameterBase declares the field for every static switch / component mask.
        { TEXT("/Script/Engine.ScalarParameterValue"), TEXT("ExpressionGUID") },
        { TEXT("/Script/Engine.VectorParameterValue"), TEXT("ExpressionGUID") },
        { TEXT("/Script/Engine.DoubleVectorParameterValue"), TEXT("ExpressionGUID") },
        { TEXT("/Script/Engine.TextureParameterValue"), TEXT("ExpressionGUID") },
        { TEXT("/Script/Engine.RuntimeVirtualTextureParameterValue"), TEXT("ExpressionGUID") },
        { TEXT("/Script/Engine.FontParameterValue"), TEXT("ExpressionGUID") },
        { TEXT("/Script/Engine.StaticParameterBase"), TEXT("ExpressionGUID") },
    };

    for (const FCase& TestCase : Cases)
    {
        FProperty* Property = FindReflectedProperty(TestCase.OwnerPath, TestCase.PropertyName);
        const FString Label = FString::Printf(TEXT("%s.%s"), TestCase.OwnerPath, TestCase.PropertyName);
        if (TestNotNull(*FString::Printf(TEXT("%s reflected property resolved"), *Label), Property))
        {
            TestTrue(*FString::Printf(TEXT("%s is omitted from authored mirror"), *Label),
                ShouldSkipNonSemanticDumpProperty(Property));
        }
    }

    FProperty* UnrelatedSignature = FindFProperty<FProperty>(
        FPropertyExportNoiseFixture::StaticStruct(),
        GET_MEMBER_NAME_CHECKED(FPropertyExportNoiseFixture, Signature));
    if (TestNotNull(TEXT("Unrelated Signature fixture property resolved"), UnrelatedSignature))
    {
        TestFalse(TEXT("Signature on an unrelated owner is retained"),
            ShouldSkipNonSemanticDumpProperty(UnrelatedSignature));
    }

    // Control for the ExpressionGUID rule: an unrelated FGuid on a material owner is
    // authored state and must survive. The rule matches owner struct + property name
    // together, never the property type.
    FProperty* LightingGuid = FindReflectedProperty(
        TEXT("/Script/Engine.MaterialInterface"), TEXT("LightingGuid"));
    if (TestNotNull(TEXT("MaterialInterface.LightingGuid resolved"), LightingGuid))
    {
        TestFalse(TEXT("An unrelated material FGuid is retained"),
            ShouldSkipNonSemanticDumpProperty(LightingGuid));
    }
    return true;
}
