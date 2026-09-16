// Copyright (c) 2026 Alexander Penkin. MIT License.

// Guards the param-name alias table in ResolveExplicitBlueprintPath (the static
// FJsonObject overload — the FHandlerContext variant needs a live subsystem).
// If "assetPath" is removed from the FieldNames table, AcceptsAssetPath fails.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Handlers/Blueprint/BlueprintHandlerUtils.h"

namespace
{
    constexpr const TCHAR* kKnownPath = TEXT("/Engine/EditorBlueprintResources/StandardMacros");

    TSharedRef<FJsonObject> MakePayloadWith(const TCHAR* Field, const TCHAR* Value)
    {
        TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(Field, Value);
        return Payload;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveBlueprintPathAcceptsPath,
    "PinWright.Blueprint.ResolvePath.AcceptsPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FResolveBlueprintPathAcceptsPath::RunTest(const FString& Parameters)
{
    TSharedRef<FJsonObject> Payload = MakePayloadWith(TEXT("path"), kKnownPath);
    const FString Resolved = BlueprintHandlerUtils::ResolveExplicitBlueprintPath(Payload, /*bNormalize=*/false);
    TestFalse(TEXT("'path' alias resolves to non-empty"), Resolved.IsEmpty());
    TestEqual(TEXT("'path' alias passes through unchanged when not normalizing"), Resolved, FString(kKnownPath));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveBlueprintPathAcceptsAssetPath,
    "PinWright.Blueprint.ResolvePath.AcceptsAssetPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FResolveBlueprintPathAcceptsAssetPath::RunTest(const FString& Parameters)
{
    TSharedRef<FJsonObject> Payload = MakePayloadWith(TEXT("assetPath"), kKnownPath);
    const FString Resolved = BlueprintHandlerUtils::ResolveExplicitBlueprintPath(Payload, /*bNormalize=*/false);
    TestFalse(TEXT("'assetPath' alias resolves to non-empty"), Resolved.IsEmpty());
    TestEqual(TEXT("'assetPath' alias passes through unchanged when not normalizing"), Resolved, FString(kKnownPath));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveBlueprintPathAcceptsBlueprintPath,
    "PinWright.Blueprint.ResolvePath.AcceptsBlueprintPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FResolveBlueprintPathAcceptsBlueprintPath::RunTest(const FString& Parameters)
{
    TSharedRef<FJsonObject> Payload = MakePayloadWith(TEXT("blueprintPath"), kKnownPath);
    const FString Resolved = BlueprintHandlerUtils::ResolveExplicitBlueprintPath(Payload, /*bNormalize=*/false);
    TestFalse(TEXT("'blueprintPath' alias resolves to non-empty"), Resolved.IsEmpty());
    TestEqual(TEXT("'blueprintPath' alias passes through unchanged when not normalizing"), Resolved, FString(kKnownPath));
    return true;
}

// ResolveExplicitBlueprintPath does NOT accept "name" (that alias is only in
// ResolveBlueprintPath's FieldNames table, which requires a live subsystem).
// Cover the next-best static alias instead: requestedPath.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveBlueprintPathAcceptsRequestedPath,
    "PinWright.Blueprint.ResolvePath.AcceptsRequestedPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FResolveBlueprintPathAcceptsRequestedPath::RunTest(const FString& Parameters)
{
    TSharedRef<FJsonObject> Payload = MakePayloadWith(TEXT("requestedPath"), kKnownPath);
    const FString Resolved = BlueprintHandlerUtils::ResolveExplicitBlueprintPath(Payload, /*bNormalize=*/false);
    TestFalse(TEXT("'requestedPath' alias resolves to non-empty"), Resolved.IsEmpty());
    TestEqual(TEXT("'requestedPath' alias passes through unchanged when not normalizing"), Resolved, FString(kKnownPath));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveBlueprintPathEmptyWhenAllMissing,
    "PinWright.Blueprint.ResolvePath.EmptyWhenAllMissing",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FResolveBlueprintPathEmptyWhenAllMissing::RunTest(const FString& Parameters)
{
    TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
    const FString Resolved = BlueprintHandlerUtils::ResolveExplicitBlueprintPath(Payload, /*bNormalize=*/false);
    TestTrue(TEXT("empty payload yields empty resolved path"), Resolved.IsEmpty());
    return true;
}
