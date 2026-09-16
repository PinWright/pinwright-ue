// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for B-property-path-silent-world-fallback.
//
// The shared property.* object resolver (ResolveObjectForProperty in
// UtilityPropertyHandler.cpp) used to accept FindObject/StaticLoadObject's
// silent ancestor fallback: handed a `…:PersistentLevel.<Actor>.<bad-tail>`
// path whose trailing segment does not resolve, UE's ResolveName climbs to the
// last resolvable outer (the World) and FindObject returns THAT object. The
// resolver returned it unchecked, so property.list/get reported ok:true with the
// World's property set (and existsAfter:true) instead of OBJECT_NOT_FOUND — the
// "silent success / wrong-object" bug.
//
// Counterfactual: reverting the IsAncestorFallback guard lets the direct
// FindObject hit (the World) through; property.list then succeeds with
// className "World" and these tests' OBJECT_NOT_FOUND assertions fail.

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "Tests/TestUtils.h"
#include "Tests/TestWorldUtils.h"
#include "Utils/AssetUtils.h"

namespace
{
    // Build a property.list payload that exercises only the resolver: all the
    // value/default/metadata enrichment is disabled so the test isolates the
    // object-resolution path the ticket is about.
    TSharedPtr<FJsonObject> MakeListPayload(const FString& ObjectPath)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), ObjectPath);
        Payload->SetBoolField(TEXT("includeValues"), false);
        Payload->SetBoolField(TEXT("includeDefault"), false);
        Payload->SetBoolField(TEXT("includeOverrideState"), false);
        Payload->SetBoolField(TEXT("includeMetadata"), false);
        return Payload;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPropertyResolverAncestorFallbackTest,
    "PinWright.property.resolver.AncestorFallbackErrors",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPropertyResolverAncestorFallbackTest::RunTest(const FString& Parameters)
{
    // Spawn a real actor into the editor world; the guard destroys it and
    // restores the level dirty flag so the open map is left untouched.
    FScopedEditorWorldActorGuard WorldGuard;

    const FString ActorLabel = FString::Printf(TEXT("PW_ResolverFallback_%s"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    AActor* Actor = SpawnActorInActiveWorld<AActor>(
        AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, ActorLabel);
    if (!TestNotNull(TEXT("actor spawned into editor world"), Actor))
    {
        return false;
    }

    const FString ActorPath = Actor->GetPathName();
    if (!TestTrue(TEXT("actor path is a level-style subobject path"),
            ActorPath.Contains(TEXT(":"))))
    {
        return false;
    }

    // --- Positive control: the correct actor path still resolves to the actor. ---
    {
        FTestResponseCapture Capture;
        TestTrue(TEXT("property.list handler found (good path)"),
            InvokeHandlerWithCapture(TEXT("property.list"), MakeListPayload(ActorPath), Capture));
        TestTrue(TEXT("property.list succeeds on the real actor path"), Capture.bSuccess);
        if (Capture.bSuccess && Capture.Result.IsValid())
        {
            FString ClassName;
            Capture.Result->TryGetStringField(TEXT("className"), ClassName);
            TestNotEqual(TEXT("real actor path does not resolve to the World"),
                ClassName, FString(TEXT("World")));
        }
    }

    // --- Bad SUBOBJECT tail: actor exists, trailing subobject does not. ---
    // The old resolver silently climbed to the World and returned ok:true.
    {
        const FString BadSubobjectPath = ActorPath + TEXT(".NonExistentSubobject_BADTAIL");
        FTestResponseCapture Capture;
        TestTrue(TEXT("property.list handler found (bad subobject tail)"),
            InvokeHandlerWithCapture(TEXT("property.list"), MakeListPayload(BadSubobjectPath), Capture));
        TestFalse(TEXT("property.list does NOT silently succeed on a bad subobject tail"),
            Capture.bSuccess);
        TestEqual(TEXT("bad subobject tail returns OBJECT_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("OBJECT_NOT_FOUND")));
    }

    // --- Bad ACTOR tail: nonexistent actor segment under the same level. ---
    {
        const FString LevelPrefix = ActorPath.Left(ActorPath.Find(
            TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd) + 1);
        const FString BadActorPath = LevelPrefix + TEXT("NonExistentActor_99.Whatever");
        FTestResponseCapture Capture;
        TestTrue(TEXT("property.list handler found (bad actor tail)"),
            InvokeHandlerWithCapture(TEXT("property.list"), MakeListPayload(BadActorPath), Capture));
        TestFalse(TEXT("property.list does NOT silently succeed on a bad actor tail"),
            Capture.bSuccess);
        TestEqual(TEXT("bad actor tail returns OBJECT_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("OBJECT_NOT_FOUND")));
    }

    // --- property.get shares the resolver: a bad subobject tail must not
    //     resolve to the World and then fail downstream against it. ---
    {
        const FString BadSubobjectPath = ActorPath + TEXT(".TotallyBogusSubobjectXYZ");
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("objectPath"), BadSubobjectPath);
        Payload->SetStringField(TEXT("propertyName"), TEXT("FogDensity"));

        FTestResponseCapture Capture;
        TestTrue(TEXT("property.get handler found (bad subobject tail)"),
            InvokeHandlerWithCapture(TEXT("property.get"), Payload, Capture));
        TestFalse(TEXT("property.get does NOT silently succeed on a bad subobject tail"),
            Capture.bSuccess);
        TestEqual(TEXT("property.get bad subobject tail returns OBJECT_NOT_FOUND"),
            Capture.ErrorCode, FString(TEXT("OBJECT_NOT_FOUND")));
    }

    return true;
}
