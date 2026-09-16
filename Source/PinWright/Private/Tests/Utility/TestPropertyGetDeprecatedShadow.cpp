// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-property-get-deprecated-field-silent-null.
//
// When a UE field migrates out of its owner into a subobject, UHT strips the
// `_DEPRECATED` suffix from the reflected FName and leaves an inert CPF_Deprecated
// shadow under the original name. The canonical case is UMaterial's input pins
// (BaseColor / Opacity / Roughness / …), which moved to UMaterialEditorOnlyData in
// UE 5.5+. A top-level `property.get` on the intuitive name (`BaseColor`) does a
// strict FindPropertyByName and resolves the deprecated shadow, serializing it as a
// benign-empty value (Expression: null) that looks unwired — with no signal it is
// the dead shadow rather than the live field, and no pointer at the live nested
// path. The reporter burned 4 of 7 property.get calls on this dead-end plus an
// out-of-band engine-source read.
//
// The fix surfaces `deprecated: true` UNCONDITIONALLY (includeMetadata defaults off,
// and the whole point is the silent read) on any CPF_Deprecated resolution, and a
// `movedTo` hint when the root exposes an EditorOnlyData object property whose
// subobject class declares a same-named live property.
//
// Counterfactual: reverting AddDeprecatedResolutionHint drops both fields, so the
// deprecated/movedTo assertions below fail and the silent-null read returns.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "Tests/TestUtils.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyGetDeprecatedShadowFlaggedTest,
    "PinWright.property.get.DeprecatedShadowFlaggedWithMovedToHint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyGetDeprecatedShadowFlaggedTest::RunTest(const FString& Parameters)
{
    UMaterial* Material = NewObject<UMaterial>(
        GetTransientPackage(),
        *FString::Printf(TEXT("M_DeprShadowTest_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    TestNotNull(TEXT("Transient material created"), Material);
    if (!Material)
    {
        return false;
    }

    // Precondition: top-level BaseColor must resolve to the CPF_Deprecated shadow
    // (the live field having migrated to UMaterialEditorOnlyData). If a future engine
    // re-exposes a live top-level BaseColor this precondition fails loudly rather than
    // silently passing a no-longer-relevant test.
    FProperty* BaseColorProp =
        Material->GetClass()->FindPropertyByName(TEXT("BaseColor"));
    TestNotNull(TEXT("BaseColor resolves on UMaterial"), BaseColorProp);
    if (!BaseColorProp)
    {
        return false;
    }
    TestTrue(TEXT("top-level BaseColor carries CPF_Deprecated"),
        BaseColorProp->HasAnyPropertyFlags(CPF_Deprecated));

    // The default-path read: NO includeMetadata (defaults off). The deprecated signal
    // must still surface — that is the crux of the "silent" complaint.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Material->GetPathName());
        Payload->SetStringField(TEXT("propertyName"), TEXT("BaseColor"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("property.get handler found"),
            InvokeHandlerWithCapture(TEXT("property.get"), Payload, Capture));
        TestTrue(TEXT("property.get succeeded"), Capture.bSuccess);
        if (!Capture.bSuccess || !Capture.Result.IsValid())
        {
            return false;
        }

        bool bDeprecated = false;
        TestTrue(TEXT("response carries deprecated marker"),
            Capture.Result->TryGetBoolField(TEXT("deprecated"), bDeprecated));
        TestTrue(TEXT("deprecated marker is true on the *_DEPRECATED shadow"), bDeprecated);

        FString MovedTo;
        TestTrue(TEXT("response carries movedTo hint"),
            Capture.Result->TryGetStringField(TEXT("movedTo"), MovedTo));
        TestEqual(TEXT("movedTo points at the EditorOnlyData nested path"),
            MovedTo, FString(TEXT("EditorOnlyData.BaseColor")));
    }

    // A non-deprecated, live top-level property (BlendMode) must NOT pick up the
    // deprecated/movedTo signal — guards against the marker being attached spuriously.
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), Material->GetPathName());
        Payload->SetStringField(TEXT("propertyName"), TEXT("BlendMode"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("property.get handler found (live field)"),
            InvokeHandlerWithCapture(TEXT("property.get"), Payload, Capture));
        TestTrue(TEXT("property.get succeeded (live field)"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            bool bDeprecated = true;
            // Either the field is absent or it is explicitly false; both mean "not flagged".
            if (Capture.Result->TryGetBoolField(TEXT("deprecated"), bDeprecated))
            {
                TestFalse(TEXT("live BlendMode is not flagged deprecated"), bDeprecated);
            }
            FString MovedTo;
            TestFalse(TEXT("live BlendMode carries no movedTo hint"),
                Capture.Result->TryGetStringField(TEXT("movedTo"), MovedTo));
        }
    }

    return true;
}
