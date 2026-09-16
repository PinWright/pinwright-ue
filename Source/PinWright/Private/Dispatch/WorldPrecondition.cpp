// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Dispatch/WorldPrecondition.h"

#include "Dispatch/SafePoint.h"
#include "Handlers/ErrorCodes.h"
#include "Handlers/HandlerContext.h"
#include "Utils/ActorUtils.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "UObject/Package.h"

namespace PinWrightWorldPrecondition
{
namespace
{
    UWorld* ResolvePieAwareWorld()
    {
        FString ResolvedMode;
        return McpActorUtils::ResolveQueryWorld(TEXT("auto"), ResolvedMode);
    }

    bool UsesPieAwareWorld(const FString& Method)
    {
        return Method.Equals(TEXT("effect.spawn_niagara"), ESearchCase::IgnoreCase) ||
            Method.StartsWith(TEXT("ui."), ESearchCase::IgnoreCase);
    }

    FString GetWorldPackageId(const UWorld* World)
    {
        return World && World->GetOutermost()
            ? World->GetOutermost()->GetName()
            : FString();
    }
}

UWorld* ResolveTargetWorldForMethod(const FString& Method)
{
    if (UsesPieAwareWorld(Method))
    {
        return ResolvePieAwareWorld();
    }
    return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

bool IsMutatingMethod(const FString& Method)
{
    // The dispatcher owns the registration flag. Keep this symbol as a narrow
    // compatibility answer for legacy callers without consulting any leaf-name
    // heuristic; only the explicit safe-point effect table can answer here.
    return PinWrightSafePoint::IsTickUnsafeMethod(Method);
}

FString GetActiveWorldId()
{
    const UWorld* World = ResolveTargetWorldForMethod(TEXT(""));
    return World ? World->GetPathName() : FString();
}

FString GetWorldIdForMethod(const FString& Method)
{
    const UWorld* World = ResolveTargetWorldForMethod(Method);
    return World ? World->GetPathName() : FString();
}

namespace
{
    bool ValidateExpectedWorld(
        const FString& Method,
        const TSharedPtr<FJsonObject>& Params,
        const TFunction<void(const FString&, const FString&, const TSharedPtr<FJsonObject>&)>& SendError)
    {
        if (!Params.IsValid() || !Params->HasField(ParamName))
        {
            return true;
        }

        FString ExpectedWorld;
        if (!Params->TryGetStringField(ParamName, ExpectedWorld) || ExpectedWorld.IsEmpty())
        {
            TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
            Details->SetStringField(TEXT("world"), GetWorldIdForMethod(Method));
            SendError(ErrorCodes::ERR_INVALID_PARAMS,
                TEXT("expectWorld must be a non-empty world object path or package path"),
                Details);
            return false;
        }

        const UWorld* TargetWorld = ResolveTargetWorldForMethod(Method);
        const FString ActualWorld = TargetWorld ? TargetWorld->GetPathName() : FString();
        const FString ActualPackage = GetWorldPackageId(TargetWorld);
        if (ExpectedWorld.Equals(ActualWorld, ESearchCase::IgnoreCase) ||
            ExpectedWorld.Equals(ActualPackage, ESearchCase::IgnoreCase))
        {
            return true;
        }

        TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
        Details->SetStringField(TEXT("expectedWorld"), ExpectedWorld);
        Details->SetStringField(TEXT("world"), ActualWorld);
        SendError(ErrorCodes::ERR_WORLD_MISMATCH,
            FString::Printf(
                TEXT("World precondition failed: expected '%s', but the target world is '%s'"),
                *ExpectedWorld, ActualWorld.IsEmpty() ? TEXT("<none>") : *ActualWorld),
            Details);
        return false;
    }
}

bool Validate(FHandlerContext& Ctx, const TSharedPtr<FJsonObject>& Params)
{
    return ValidateExpectedWorld(Ctx.GetMethod(), Params,
        [&Ctx](const FString& ErrorCode, const FString& Message,
               const TSharedPtr<FJsonObject>& Details)
        {
            Ctx.SendError(ErrorCode, Message, Details);
        });
}

bool Validate(FAsyncResponseToken& Token, const TSharedPtr<FJsonObject>& Params)
{
    return ValidateExpectedWorld(Token.Method, Params,
        [&Token](const FString& ErrorCode, const FString& Message,
                 const TSharedPtr<FJsonObject>& Details)
        {
            Token.SendError(ErrorCode, Message, Details);
        });
}

void AddWorldField(const TSharedPtr<FJsonObject>& Result)
{
    AddWorldField(Result, FString());
}

void AddWorldField(const TSharedPtr<FJsonObject>& Result, const FString& Method)
{
    if (!Result.IsValid() || Result->HasField(TEXT("world")))
    {
        if (Result.IsValid() && Result->HasField(TEXT("world")))
        {
            FString ExistingWorld;
            const FString ResolvedWorld = GetWorldIdForMethod(Method);
            if (Result->TryGetStringField(TEXT("world"), ExistingWorld) &&
                !ResolvedWorld.IsEmpty())
            {
                ensureMsgf(ExistingWorld.Equals(ResolvedWorld, ESearchCase::IgnoreCase),
                    TEXT("Handler-defined world '%s' disagrees with resolved target '%s'"),
                    *ExistingWorld, *ResolvedWorld);
            }
        }
        return;
    }
    Result->SetStringField(TEXT("world"), GetWorldIdForMethod(Method));
}
}
