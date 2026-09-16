// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Render/CaptureSubject.h"

#include "Handlers/ErrorCodes.h"
// FScopedUnattendedRpc -- held across the deferred close, because tearing a toolkit down can raise
// an engine modal and the ticker pass that runs it is outside FRpcDispatcher's own scope.
#include "Dispatch/ScopedUnattendedRpc.h"
// FScopedSharedProfiles -- the viewport-free snapshot/restore guard wrapped around the asset-editor
// open below. See the comment at that call site for why the open itself is a write.
#include "Handlers/Render/PreviewSceneRig.h"
#include "Handlers/Render/PreviewViewportCaptureUtils.h"

#include "Animation/AnimationAsset.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/ChildrenBase.h"
#include "Misc/PackageName.h"
#include "SAssetEditorViewport.h"
#include "SEditorViewport.h"
#include "SLevelViewport.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Templates/UnrealTypeTraits.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/IToolkitHost.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

DEFINE_LOG_CATEGORY_STATIC(LogPinWrightCaptureSubject, Log, All);

namespace PinWrightCaptureSubject
{

// ---------------------------------------------------------------------------------------------
// Kind vocabulary
// ---------------------------------------------------------------------------------------------

namespace
{
    // Index-aligned with ESubjectKind. The one place these strings are written; ToWireName,
    // ParseKindName and every refusal message read from here, so a rename cannot leave one of the
    // three spelling it the old way.
    const TCHAR* const GKindWireNames[] =
    {
        TEXT("world"),
        TEXT("actor"),
        TEXT("staticMesh"),
        TEXT("skeletalMesh"),
        TEXT("animation"),
        TEXT("niagara")
    };
    constexpr int32 GKindCount = UE_ARRAY_COUNT(GKindWireNames);
    static_assert(static_cast<int32>(ESubjectKind::Niagara) + 1 == GKindCount,
        "GKindWireNames is index-aligned with ESubjectKind; adding a kind means adding its wire spelling.");

    bool IsAssetKind(ESubjectKind Kind)
    {
        return Kind == ESubjectKind::StaticMesh || Kind == ESubjectKind::SkeletalMesh ||
               Kind == ESubjectKind::Animation || Kind == ESubjectKind::Niagara;
    }

}

const TCHAR* ToWireName(ESubjectKind Kind)
{
    const int32 Index = static_cast<int32>(Kind);
    return (Index >= 0 && Index < GKindCount) ? GKindWireNames[Index] : TEXT("world");
}

bool ParseKindName(const FString& Name, ESubjectKind& OutKind)
{
    for (int32 Index = 0; Index < GKindCount; ++Index)
    {
        if (Name.Equals(GKindWireNames[Index], ESearchCase::IgnoreCase))
        {
            OutKind = static_cast<ESubjectKind>(Index);
            return true;
        }
    }
    return false;
}

FString AllKindWireNamesJoined(const TCHAR* Separator)
{
    TArray<FString> Names;
    Names.Reserve(GKindCount);
    for (int32 Index = 0; Index < GKindCount; ++Index)
    {
        Names.Add(GKindWireNames[Index]);
    }
    return FString::Join(Names, Separator);
}

// ---------------------------------------------------------------------------------------------
// Provider registry
// ---------------------------------------------------------------------------------------------

namespace
{
    // Function-local static, not a file-scope one: providers register from OTHER translation units
    // at static init, and a file-scope array here would be constructed in an order the standard
    // does not fix. This is the same shape FAutoRegisterHandler::GetPendingRegistrations() uses,
    // and for the same reason.
    TArray<FSubjectProvider>& ProviderRegistry()
    {
        static TArray<FSubjectProvider> Registry;
        return Registry;
    }
}

void RegisterProvider(const FSubjectProvider& Provider)
{
    TArray<FSubjectProvider>& Registry = ProviderRegistry();
    for (FSubjectProvider& Existing : Registry)
    {
        if (Existing.Kind == Provider.Kind)
        {
            // Replaced and reported. Silently keeping the first would mean the provider an author
            // just wrote is not the one running, which is invisible from the outside; silently
            // keeping the last would hide that two files claim the same kind.
            UE_LOG(LogPinWrightCaptureSubject, Warning,
                TEXT("Two capture-subject providers registered for kind '%s'; the later registration wins."),
                ToWireName(Provider.Kind));
            Existing = Provider;
            return;
        }
    }
    Registry.Add(Provider);
}

const FSubjectProvider* FindProvider(ESubjectKind Kind)
{
    for (const FSubjectProvider& Provider : ProviderRegistry())
    {
        if (Provider.Kind == Kind)
        {
            return &Provider;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// FResolvedSubject lifetime
// ---------------------------------------------------------------------------------------------

FResolvedSubject::~FResolvedSubject()
{
    ReleaseSubject(*this);
}

FResolvedSubject::FResolvedSubject(FResolvedSubject&& Other)
    : ViewportClient(Other.ViewportClient)
    , SceneViewport(MoveTemp(Other.SceneViewport))
    , BoundsOrigin(Other.BoundsOrigin)
    , BoundsRadius(Other.BoundsRadius)
    , bTimeSupported(Other.bTimeSupported)
    , TimeStartSeconds(Other.TimeStartSeconds)
    , TimeEndSeconds(Other.TimeEndSeconds)
    , CaptureSource(MoveTemp(Other.CaptureSource))
    , BoundsSource(MoveTemp(Other.BoundsSource))
    , bEditorWasAlreadyOpen(Other.bEditorWasAlreadyOpen)
    , bEditorClosed(Other.bEditorClosed)
    , bTimeReproducible(Other.bTimeReproducible)
    , Kind(Other.Kind)
    , AssetPath(MoveTemp(Other.AssetPath))
    , ActorName(MoveTemp(Other.ActorName))
    , BoundsWarning(MoveTemp(Other.BoundsWarning))
    , ReproducibilityWarning(MoveTemp(Other.ReproducibilityWarning))
    , ProviderState(MoveTemp(Other.ProviderState))
    , ReleaseFunc(MoveTemp(Other.ReleaseFunc))
    , bReleased(Other.bReleased)
{
    // The release responsibility moved with the state. Marking the source released is what keeps
    // "exactly once" true across a move.
    Other.bReleased = true;
    Other.ReleaseFunc.Reset();
    Other.ViewportClient = nullptr;
}

FResolvedSubject& FResolvedSubject::operator=(FResolvedSubject&& Other)
{
    if (this != &Other)
    {
        // Whatever this object was still holding is released BEFORE it is overwritten; otherwise
        // an assignment leaks an open asset editor window, which is the shutdown-crash
        // precondition (docs/lessons.md:166).
        ReleaseSubject(*this);

        ViewportClient = Other.ViewportClient;
        SceneViewport = MoveTemp(Other.SceneViewport);
        BoundsOrigin = Other.BoundsOrigin;
        BoundsRadius = Other.BoundsRadius;
        bTimeSupported = Other.bTimeSupported;
        TimeStartSeconds = Other.TimeStartSeconds;
        TimeEndSeconds = Other.TimeEndSeconds;
        CaptureSource = MoveTemp(Other.CaptureSource);
        BoundsSource = MoveTemp(Other.BoundsSource);
        bEditorWasAlreadyOpen = Other.bEditorWasAlreadyOpen;
        bEditorClosed = Other.bEditorClosed;
        bTimeReproducible = Other.bTimeReproducible;
        Kind = Other.Kind;
        AssetPath = MoveTemp(Other.AssetPath);
        ActorName = MoveTemp(Other.ActorName);
        BoundsWarning = MoveTemp(Other.BoundsWarning);
        ReproducibilityWarning = MoveTemp(Other.ReproducibilityWarning);
        ProviderState = MoveTemp(Other.ProviderState);
        ReleaseFunc = MoveTemp(Other.ReleaseFunc);
        bReleased = Other.bReleased;

        Other.bReleased = true;
        Other.ReleaseFunc.Reset();
        Other.ViewportClient = nullptr;
    }
    return *this;
}

void ReleaseSubject(FResolvedSubject& Subject)
{
    if (Subject.bReleased)
    {
        return;
    }
    // Set BEFORE the call, not after: a provider whose Release throws or re-enters must not be
    // able to run twice, and "exactly once" is the acceptance criterion this guard carries.
    Subject.bReleased = true;

    // THE SUBJECT'S OWN VIEWPORT REFERENCES GO HERE, not in each provider's Release.
    //
    // A provider's Release ends in CloseAssetEditor, and closing an asset editor destroys its
    // SEditorViewport - whose destructor asserts check(SceneViewport.IsUnique()) after resetting
    // its client (UE 5.8 Editor/UnrealEd/Private/SEditorViewport.cpp:65). A surviving
    // TSharedPtr<FSceneViewport> is therefore not a leak, it is a hard crash. Three providers
    // remembered to clear these two fields by hand and the Niagara one did not, which is exactly
    // the per-provider discipline this resolver exists to abolish: clearing centrally means a
    // provider CANNOT reintroduce the defect by forgetting.
    //
    // Cleared BEFORE Release runs, not after, so a provider cannot hand the stale pair to anything
    // it calls. No provider reads either field in its Release - each one carries what it needs in
    // its own FSubjectReleaseState - and the release path is the point past which the contract
    // says they are dead anyway.
    Subject.ViewportClient = nullptr;
    Subject.SceneViewport.Reset();

    if (Subject.ReleaseFunc)
    {
        TFunction<void(FResolvedSubject&)> Release = MoveTemp(Subject.ReleaseFunc);
        Subject.ReleaseFunc.Reset();
        Release(Subject);
    }
    // ProviderState is deliberately NOT cleared here. Release may have recorded a measured outcome
    // into the subject (assetEditorClosed above all), and a verb that releases early so its
    // response can report a measured close reads those fields afterwards.
}

void ReleaseSubjectAndViewportRefs(FResolvedSubject& Subject,
    FEditorViewportClient*& InOutViewportClient, TSharedPtr<FSceneViewport>& InOutSceneViewport)
{
    // The caller's copies die FIRST, then the subject releases. Both halves are required and the
    // order between them is the whole point: ReleaseSubject clears the subject's own pair, but a
    // verb that copied them into locals still owns a reference the resolver cannot see, and one
    // surviving reference is what turns the close into check(SceneViewport.IsUnique()).
    InOutViewportClient = nullptr;
    InOutSceneViewport.Reset();
    ReleaseSubject(Subject);
}

// ---------------------------------------------------------------------------------------------
// Resolve
// ---------------------------------------------------------------------------------------------

bool ResolveWithProvider(const FSubjectProvider& Provider, const FSubjectRequest& Request,
    FResolvedSubject& OutSubject, FSubjectTimeSetter& OutTimeSetter,
    FString& OutErrCode, FString& OutErrMsg)
{
    OutErrCode.Reset();
    OutErrMsg.Reset();

    // Filled BEFORE Acquire runs, so a provider that fails halfway still produces a subject whose
    // release routes to the right place and whose `kind` is reportable.
    OutSubject.Kind = Request.Kind;
    OutSubject.AssetPath = Request.AssetPath;
    OutSubject.ActorName = Request.ActorName;
    OutSubject.ReleaseFunc = Provider.Release;
    OutSubject.bReleased = false;

    if (!Provider.Acquire)
    {
        OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
        OutErrMsg = FString::Printf(
            TEXT("The capture-subject provider registered for kind '%s' has no Acquire step, so nothing ")
            TEXT("can be resolved for it. This is a provider defect, not a bad argument."),
            ToWireName(Request.Kind));
        ReleaseSubject(OutSubject);
        return false;
    }

    if (!Provider.Acquire(Request, OutSubject, OutTimeSetter, OutErrCode, OutErrMsg))
    {
        if (OutErrCode.IsEmpty())
        {
            // A provider that refuses without saying why would surface as an empty error code the
            // dispatcher cannot classify, so the failure is named here rather than passed on blank.
            OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
            OutErrMsg = FString::Printf(
                TEXT("The capture-subject provider for kind '%s' refused without reporting a reason."),
                ToWireName(Request.Kind));
        }
        // The frozen contract says Release runs on EVERY exit path, error paths included: a
        // provider that opened an asset editor and then failed to find its viewport would
        // otherwise leave the window open, which is the shutdown-crash precondition.
        ReleaseSubject(OutSubject);
        return false;
    }

    if (!OutTimeSetter)
    {
        // LAST RESORT ONLY. Every shipped provider fills OutTimeSetter itself, including when its
        // kind has no time axis - the level provider installs its own no-time-axis setter
        // (CaptureSubjectProviders_Level.cpp:336) - so this branch is reached only by a provider
        // that forgot. It exists because the contract promises callers a callable setter, and
        // invoking an empty TFunction is a crash, not a refusal.
        //
        // WHY UNSUPPORTED_ASSET_EDITOR AND NOT INVALID_ARGUMENT. The two codes mean different
        // things and the split is deliberate:
        //   - INVALID_ARGUMENT is for a subject that HAS a time axis where the caller merely did
        //     not name the time source (a world/actor subject with no `sequencePath`). That is
        //     caller-fixable, and it is the code the level provider's own setter uses.
        //   - UNSUPPORTED_ASSET_EDITOR is for a subject that has no time axis at all, or whose
        //     provider shipped no driver. Neither is fixable by changing an argument.
        // This fallback can only ever be the second case, so it never tells a caller that a
        // fixable problem is unfixable - which would make them stop instead of supplying the
        // argument. The message still branches on the MEASURED bTimeSupported rather than assuming,
        // so a provider that claimed a time axis and then supplied no driver is reported as the
        // provider defect it is instead of being described as a kind with no time axis.
        const ESubjectKind Kind = Request.Kind;
        const bool bClaimedTimeSupport = OutSubject.bTimeSupported;
        OutTimeSetter = [Kind, bClaimedTimeSupport](
            double TimeSeconds, FString& OutSetterErrCode, FString& OutSetterErrMsg)
        {
            OutSetterErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
            OutSetterErrMsg = bClaimedTimeSupport
                ? FString::Printf(
                    TEXT("The capture-subject provider for kind '%s' reported a time axis but supplied ")
                    TEXT("no time driver, so it cannot be set to %.4f seconds. This is a provider ")
                    TEXT("defect, not a bad argument."),
                    ToWireName(Kind), TimeSeconds)
                : FString::Printf(
                    TEXT("A '%s' subject has no time axis, so it cannot be set to %.4f seconds. ")
                    TEXT("Time series are available on the animation and niagara kinds, and on a world ")
                    TEXT("or actor subject driven by a Level Sequence."),
                    ToWireName(Kind), TimeSeconds);
            return false;
        };
    }

    return true;
}

bool Resolve(const FSubjectRequest& Request, FResolvedSubject& OutSubject,
    FSubjectTimeSetter& OutTimeSetter, FString& OutErrCode, FString& OutErrMsg)
{
    const FSubjectProvider* Provider = FindProvider(Request.Kind);
    if (!Provider)
    {
        OutSubject.Kind = Request.Kind;
        TArray<FString> RegisteredNames;
        for (const FSubjectProvider& Registered : ProviderRegistry())
        {
            RegisteredNames.Add(ToWireName(Registered.Kind));
        }
        const FString RegisteredList = RegisteredNames.Num() > 0
            ? FString::Join(RegisteredNames, TEXT(", "))
            : FString(TEXT("(none)"));
        OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
        OutErrMsg = FString::Printf(
            TEXT("No capture-subject provider is registered for kind '%s'. Registered kinds: %s."),
            ToWireName(Request.Kind), *RegisteredList);
        return false;
    }
    // The provider is copied into the call rather than referenced across it: RegisterProvider can
    // reallocate the registry array, and a reference into it would dangle if a provider registered
    // during a resolve.
    const FSubjectProvider ProviderCopy = *Provider;
    return ResolveWithProvider(ProviderCopy, Request, OutSubject, OutTimeSetter, OutErrCode, OutErrMsg);
}

// ---------------------------------------------------------------------------------------------
// ParseSubject
// ---------------------------------------------------------------------------------------------

namespace
{
    // Which half of the wire a key identifies. Ambiguity is a clash between two DIFFERENT roles;
    // two spellings of the same role are aliases and are not a contradiction.
    enum class EKeyRole : uint8 { Asset, Actor, Point };

    struct FSeenKey
    {
        FString Spelling;
        EKeyRole Role = EKeyRole::Asset;
    };

    bool ReadStringKey(const TSharedPtr<FJsonObject>& Source, const TCHAR* Key, FString& OutValue)
    {
        FString Value;
        if (Source->TryGetStringField(Key, Value) && !Value.IsEmpty())
        {
            OutValue = Value;
            return true;
        }
        return false;
    }

    // LoadObject on the caller's spelling, then on the /Game/Foo -> /Game/Foo.Foo expansion the
    // asset registry uses, because both spellings reach every other asset verb in this plugin.
    // Quiet: a path that does not resolve is a typed refusal here, not a log-spamming failure.
    UObject* LoadSubjectAsset(const FString& AssetPath)
    {
        if (AssetPath.IsEmpty())
        {
            return nullptr;
        }
        if (UObject* Direct = LoadObject<UObject>(nullptr, *AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
        {
            return Direct;
        }
        if (!AssetPath.Contains(TEXT(".")))
        {
            const FString ObjectPath =
                FString::Printf(TEXT("%s.%s"), *AssetPath, *FPackageName::GetShortName(AssetPath));
            return LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
        }
        return nullptr;
    }

    // The asset's UClass decides the kind, per §2.1. Niagara is reached by REFLECTION rather than
    // by including NiagaraSystem.h: the Niagara modules are public dependencies
    // (PinWright.Build.cs:25) so linking would work, but keeping the resolver core free of any
    // one kind's headers is what makes "adding a kind creates a file and edits none" true.
    bool ClassifyAssetKind(UObject* Asset, ESubjectKind& OutKind)
    {
        UClass* AssetClass = Asset ? Asset->GetClass() : nullptr;
        if (!AssetClass)
        {
            return false;
        }
        if (AssetClass->IsChildOf(UStaticMesh::StaticClass()))
        {
            OutKind = ESubjectKind::StaticMesh;
            return true;
        }
        if (AssetClass->IsChildOf(USkeletalMesh::StaticClass()))
        {
            OutKind = ESubjectKind::SkeletalMesh;
            return true;
        }
        if (AssetClass->IsChildOf(UAnimationAsset::StaticClass()))
        {
            OutKind = ESubjectKind::Animation;
            return true;
        }
        if (UClass* NiagaraSystemClass =
                FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraSystem")))
        {
            if (AssetClass->IsChildOf(NiagaraSystemClass))
            {
                OutKind = ESubjectKind::Niagara;
                return true;
            }
        }
        return false;
    }
}

bool ParseSubject(const TSharedPtr<FJsonObject>& Payload, FSubjectRequest& Out,
    FString& OutErrCode, FString& OutErrMsg)
{
    OutErrCode.Reset();
    OutErrMsg.Reset();

    if (!Payload.IsValid())
    {
        return true;   // nothing on the wire: the level itself, bProvided stays false
    }

    const TSharedPtr<FJsonObject>* NestedPtr = nullptr;
    const bool bNested = Payload->TryGetObjectField(TEXT("subject"), NestedPtr) &&
        NestedPtr != nullptr && NestedPtr->IsValid();
    const TSharedPtr<FJsonObject> Source = bNested ? *NestedPtr : Payload;

    // ---- a `subject` object does NOT absorb a legacy target key beside it ----
    //
    // This is the ambiguity that actually happens. `{subject:{path:...}, actorName:"X"}` reads as
    // a caller who moved half their payload into the new object and left the other half behind,
    // and reading only the nested object would silently ignore a target they named - handing back
    // a successful capture of something they did not ask for, which is precisely the failure this
    // resolver exists to remove. It is refused here rather than in each verb: five verbs take
    // `subject`, and a rule written five times drifts four ways.
    //
    // `radius` is DELIBERATELY NOT in this list. It is a modifier, not a target: a bare radius
    // names no subject, and camera.orbit_shots has taken a top-level `radius` since it shipped, so
    // refusing `subject` + `radius` would break a legitimate call shape to catch nothing. `point`
    // IS in the list, because a point is a target.
    if (bNested)
    {
        // The four actor spellings are ActorNameParamUtils::ActorNameKeys(), matching the
        // handler-level refusal camera.frame_actor already emits through ResolveActorName - so the
        // two cannot disagree about what counts as "the caller named an actor".
        const TCHAR* const LegacyTargetKeys[] =
        {
            TEXT("assetPath"), TEXT("actorName"), TEXT("actor_name"), TEXT("actorPath"),
            TEXT("objectPath"), TEXT("point")
        };
        for (const TCHAR* LegacyKey : LegacyTargetKeys)
        {
            bool bPresentAtTopLevel = false;
            if (FCString::Strcmp(LegacyKey, TEXT("point")) == 0)
            {
                const TSharedPtr<FJsonObject>* TopLevelPoint = nullptr;
                bPresentAtTopLevel = Payload->TryGetObjectField(TEXT("point"), TopLevelPoint) &&
                    TopLevelPoint != nullptr && TopLevelPoint->IsValid();
            }
            else
            {
                FString TopLevelValue;
                bPresentAtTopLevel = Payload->TryGetStringField(LegacyKey, TopLevelValue) &&
                    !TopLevelValue.IsEmpty();
            }
            if (bPresentAtTopLevel)
            {
                OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutErrMsg = FString::Printf(
                    TEXT("Ambiguous capture subject: 'subject' and '%s' both name a target, and a ")
                    TEXT("`subject` object does not absorb the legacy key beside it. Pass one - drop ")
                    TEXT("'%s' to capture the subject, or drop 'subject' to keep the legacy target. ")
                    TEXT("Modifiers such as `radius` are unaffected."),
                    LegacyKey, LegacyKey);
                return false;
            }
        }
    }

    TArray<FSeenKey> SeenKeys;

    // ---- the asset slot ----
    // `path` is the nested spelling; `assetPath` is the legacy top-level one and is accepted in
    // both places so a caller can move a payload into `subject` without renaming anything.
    if (bNested && ReadStringKey(Source, TEXT("path"), Out.AssetPath))
    {
        SeenKeys.Add({TEXT("path"), EKeyRole::Asset});
    }
    else if (ReadStringKey(Source, TEXT("assetPath"), Out.AssetPath))
    {
        SeenKeys.Add({TEXT("assetPath"), EKeyRole::Asset});
    }

    // ---- the actor slot ----
    //
    // The alias set NARROWS at the top level, deliberately. Inside `subject` the key can only mean
    // an actor, so every ActorNameParamUtils spelling is safe. At the top level `objectPath` and
    // `actorPath` also read as ASSET spellings elsewhere on this surface, and a resolver that
    // guessed which one a caller meant would be choosing silently between two subjects. Only the
    // two spellings that cannot mean an asset are read there.
    {
        const TCHAR* const NestedActorKeys[] =
            { TEXT("name"), TEXT("actorName"), TEXT("objectPath"), TEXT("actorPath"), TEXT("actor_name") };
        const TCHAR* const TopLevelActorKeys[] = { TEXT("actorName"), TEXT("actor_name") };
        const TArrayView<const TCHAR* const> ActorKeys = bNested
            ? TArrayView<const TCHAR* const>(NestedActorKeys, UE_ARRAY_COUNT(NestedActorKeys))
            : TArrayView<const TCHAR* const>(TopLevelActorKeys, UE_ARRAY_COUNT(TopLevelActorKeys));
        for (const TCHAR* Key : ActorKeys)
        {
            if (ReadStringKey(Source, Key, Out.ActorName))
            {
                SeenKeys.Add({Key, EKeyRole::Actor});
                break;
            }
        }
    }

    // ---- the point slot ----
    {
        const TSharedPtr<FJsonObject>* PointObject = nullptr;
        if (Source->TryGetObjectField(TEXT("point"), PointObject) &&
            PointObject != nullptr && PointObject->IsValid())
        {
            Out.bPointProvided = true;
            SeenKeys.Add({TEXT("point"), EKeyRole::Point});
            double Component = 0.0;
            Out.Point.X = (*PointObject)->TryGetNumberField(TEXT("x"), Component) ? Component : 0.0;
            Out.Point.Y = (*PointObject)->TryGetNumberField(TEXT("y"), Component) ? Component : 0.0;
            Out.Point.Z = (*PointObject)->TryGetNumberField(TEXT("z"), Component) ? Component : 0.0;
        }
    }

    // ---- non-identifying values ----
    {
        double RadiusValue = 0.0;
        if (Source->TryGetNumberField(TEXT("radius"), RadiusValue))
        {
            Out.bRadiusProvided = true;
            Out.Radius = static_cast<float>(RadiusValue);
        }
    }
    if (bNested)
    {
        if (!ReadStringKey(Source, TEXT("animation"), Out.AnimationPath))
        {
            ReadStringKey(Source, TEXT("animationPath"), Out.AnimationPath);
        }
    }
    {
        bool bClose = true;
        if (Source->TryGetBoolField(TEXT("closeAfterCapture"), bClose))
        {
            Out.bCloseAfterCapture = bClose;
            Out.bCloseAfterCaptureProvided = true;
        }
    }

    Out.bProvided = bNested || SeenKeys.Num() > 0;

    // ---- explicit kind wins; it is the tie-break the ambiguity refusal tells the caller about ----
    FString KindName;
    if (ReadStringKey(Source, TEXT("kind"), KindName))
    {
        ESubjectKind ExplicitKind = ESubjectKind::World;
        if (!ParseKindName(KindName, ExplicitKind))
        {
            OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
            OutErrMsg = FString::Printf(
                TEXT("subject.kind '%s' is not a subject kind. Valid kinds: %s."),
                *KindName, *AllKindWireNamesJoined());
            return false;
        }
        Out.Kind = ExplicitKind;
        Out.bKindProvided = true;
        Out.bProvided = true;
        return true;
    }

    // ---- inference from exactly one identifying key ----
    for (int32 First = 0; First < SeenKeys.Num(); ++First)
    {
        for (int32 Second = First + 1; Second < SeenKeys.Num(); ++Second)
        {
            if (SeenKeys[First].Role != SeenKeys[Second].Role)
            {
                // Refused rather than ranked. A caller who wrote both believes one of them is in
                // force, and picking one silently is exactly the failure rpc-design.md:66
                // prohibits: the response would describe a subject the caller did not ask for.
                // Both spellings appear verbatim so the message names what the caller actually
                // typed rather than a canonical form they never wrote.
                OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
                OutErrMsg = FString::Printf(
                    TEXT("Ambiguous capture subject: '%s' and '%s' name two different subjects and ")
                    TEXT("`kind` was not given to break the tie. Pass exactly one of them, or spell ")
                    TEXT("`kind` explicitly (%s)."),
                    *SeenKeys[First].Spelling, *SeenKeys[Second].Spelling, *AllKindWireNamesJoined());
                return false;
            }
        }
    }

    if (SeenKeys.Num() == 0)
    {
        Out.Kind = ESubjectKind::World;   // no identifying key: the level itself
        return true;
    }

    switch (SeenKeys[0].Role)
    {
    case EKeyRole::Actor:
        Out.Kind = ESubjectKind::Actor;
        return true;
    case EKeyRole::Point:
        Out.Kind = ESubjectKind::World;
        return true;
    case EKeyRole::Asset:
    default:
        break;
    }

    // The asset's class decides. This LOADS the asset, which is not the same as opening its editor
    // - nothing is shown and no toolkit is created - but it does mean a path that names nothing is
    // refused here rather than three layers down inside a provider.
    UObject* Asset = LoadSubjectAsset(Out.AssetPath);
    if (!Asset)
    {
        OutErrCode = ErrorCodes::ERR_ASSET_NOT_FOUND;
        OutErrMsg = FString::Printf(
            TEXT("Capture subject asset not found: %s. Pass an object path (/Game/Path/Asset.Asset) ")
            TEXT("or a package path (/Game/Path/Asset)."),
            *Out.AssetPath);
        return false;
    }
    ESubjectKind InferredKind = ESubjectKind::StaticMesh;
    if (!ClassifyAssetKind(Asset, InferredKind))
    {
        OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
        OutErrMsg = FString::Printf(
            TEXT("'%s' is a %s, which is not a capture subject kind. Asset subjects are a Static Mesh, ")
            TEXT("a Skeletal Mesh, an animation asset, or a Niagara System; spell `kind` explicitly if ")
            TEXT("a provider for this class has since been registered."),
            *Out.AssetPath, *Asset->GetClass()->GetName());
        return false;
    }
    Out.Kind = InferredKind;
    return true;
}

// ---------------------------------------------------------------------------------------------
// The `subject` response block
// ---------------------------------------------------------------------------------------------

TSharedPtr<FJsonObject> MakeSubjectInfoObject(const FResolvedSubject& Subject)
{
    TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();

    // Unconditional: `kind` is what makes every other field in the block interpretable.
    Info->SetStringField(TEXT("kind"), ToWireName(Subject.Kind));

    if (!Subject.AssetPath.IsEmpty())
    {
        Info->SetStringField(TEXT("path"), Subject.AssetPath);
    }
    if (!Subject.ActorName.IsEmpty())
    {
        Info->SetStringField(TEXT("name"), Subject.ActorName);
    }
    if (!Subject.CaptureSource.IsEmpty())
    {
        Info->SetStringField(TEXT("captureSource"), Subject.CaptureSource);
    }
    if (!Subject.BoundsSource.IsEmpty())
    {
        Info->SetStringField(TEXT("boundsSource"), Subject.BoundsSource);
    }
    // Bounds appear only when they were measured. A zeroed origin with a zero radius emitted
    // anyway would read as "this subject is a point at the world origin", which is a different
    // claim from "nothing measured its size" - and EvaluateBoundsFraming already reports
    // bEvaluated=false for the same input.
    if (Subject.BoundsRadius > 0.0)
    {
        Info->SetObjectField(TEXT("boundsOrigin"),
            PinWrightRenderCapture::MakeVectorObject(Subject.BoundsOrigin));
        Info->SetNumberField(TEXT("boundsRadius"), Subject.BoundsRadius);
    }

    // A definite fact about the kind rather than a warning, so it is always present: its absence
    // would be indistinguishable from "false", and decision 6 turns on a caller being able to see
    // that a kind has no time axis BEFORE it asks for an instant.
    Info->SetBoolField(TEXT("timeSupported"), Subject.bTimeSupported);
    if (Subject.bTimeSupported)
    {
        Info->SetNumberField(TEXT("timeStartSeconds"), Subject.TimeStartSeconds);
        Info->SetNumberField(TEXT("timeEndSeconds"), Subject.TimeEndSeconds);
    }

    // Window state only where there is a window. On a world or actor subject there is no asset
    // editor, and a `false` for both would invite the reader to conclude one was left open.
    if (IsAssetKind(Subject.Kind))
    {
        Info->SetBoolField(TEXT("assetEditorWasAlreadyOpen"), Subject.bEditorWasAlreadyOpen);
        Info->SetBoolField(TEXT("assetEditorClosed"), Subject.bEditorClosed);
        // UNCONDITIONAL beside the other two, because without it `assetEditorClosed: false` has
        // two meanings a caller acts on differently: "left open, as you asked" and "queued, and
        // gone on the next tick". The close is deferred off the release stack (see
        // ScheduleDeferredAssetEditorClose), so the second case is now the normal one and the
        // ambiguity would be permanent. Read from the queue rather than from a field, so every
        // provider publishes it without a line of its own.
        Info->SetBoolField(TEXT("assetEditorCloseDeferred"),
            HasPendingDeferredAssetEditorClose(Subject.AssetPath));
    }

    if (!Subject.BoundsWarning.IsEmpty())
    {
        Info->SetStringField(TEXT("boundsWarning"), Subject.BoundsWarning);
    }
    // There is deliberately NO `reproducible` field of any spelling, including a
    // `timeReproducible` mirror of FResolvedSubject::bTimeReproducible. §4.2: Niagara determinism
    // is off by default at all three scopes and void under a variable tick delta, so the response
    // reports what was done and warns about what is not guaranteed. A boolean would be read as a
    // promise no code here can keep.
    if (!Subject.ReproducibilityWarning.IsEmpty())
    {
        Info->SetStringField(TEXT("reproducibilityWarning"), Subject.ReproducibilityWarning);
    }

    return Info;
}

// ---------------------------------------------------------------------------------------------
// The asset-editor viewport walk (§2.5) - the Bug 2 fix
// ---------------------------------------------------------------------------------------------

namespace
{
    // Compile-time half of the allow-list proof. Only usable for a type reachable from a PUBLIC
    // engine header; the leaf widget classes of most asset editors live in Private/ and cannot be
    // included from a plugin at all, which is why FEditorViewportTypeEntry carries
    // bCompileTimeVerified rather than pretending every entry is checked the same way.
    template <typename TWidget>
    constexpr const TCHAR* VerifiedEditorViewportTypeName(const TCHAR* TypeName)
    {
        static_assert(TIsDerivedFrom<TWidget, SEditorViewport>::Value,
            "An allow-list entry must derive from SEditorViewport, or the StaticCastSharedRef in "
            "FindEditorViewportInWidgetTree is undefined behaviour.");
        return TypeName;
    }

    // ---- the allow-list ----
    //
    // Verified-but-not-enrolled, each a checked ": public SEditorViewport" in UE 5.8, so adding one
    // is a one-line change with its citation already in hand: SSCSEditorViewport
    // (Editor/Kismet/Private/SSCSEditorViewport.h:19), SMaterialEditor3DPreviewViewport
    // (Editor/MaterialEditor/Private/SMaterialEditorViewport.h:36), SNiagaraSimCacheViewport
    // (Plugins/FX/Niagara/.../SNiagaraSimCacheViewport.h:11), SNiagaraBakerViewport
    // (.../SNiagaraBakerViewport.h:14), SNiagaraBaselineViewport (.../SNiagaraSystemViewport.h:180).
    // They are left out because no verb in this plugin walks into those toolkits today, and an
    // allow-list whose entries nobody exercises is a list nobody re-checks.
    const FEditorViewportTypeEntry GEditorViewportTypeAllowList[] =
    {
        // The base itself. A widget can be exactly this only in a host that subclasses it without
        // renaming, but it was the first clause of the shipped predicate and dropping it would be
        // a silent narrowing.
        { VerifiedEditorViewportTypeName<SEditorViewport>(TEXT("SEditorViewport")),
          TEXT("Editor/UnrealEd/Public/SEditorViewport.h:27"), true },
        { VerifiedEditorViewportTypeName<SAssetEditorViewport>(TEXT("SAssetEditorViewport")),
          TEXT("Editor/UnrealEd/Public/SAssetEditorViewport.h:14"), true },
        { VerifiedEditorViewportTypeName<SLevelViewport>(TEXT("SLevelViewport")),
          TEXT("Editor/LevelEditor/Public/SLevelViewport.h:53"), true },

        // render.capture_asset_preview's viewport. SNew(SStaticMeshEditorViewport) makes the type
        // name exact; the class is in a private engine header, so the citation is the proof.
        { TEXT("SStaticMeshEditorViewport"),
          TEXT("Editor/StaticMeshEditor/Private/SStaticMeshEditorViewport.h:42 (: public SAssetEditorViewport)"), false },

        // The Persona family's viewport, shared by SkeletalMeshEditor / AnimationEditor /
        // SkeletonEditor / AnimationBlueprintEditor. Constructed at
        // Editor/Persona/Private/SAnimationEditorViewport.cpp:741.
        { TEXT("SAnimationEditorViewport"),
          TEXT("Editor/Persona/Private/SAnimationEditorViewport.h:56 (: public SEditorViewport)"), false },

        // THE ENTRY THE SUFFIX RULE COULD NEVER MATCH, and the reason Niagara looked unreachable.
        // Constructed as SNew(SNiagaraSystemViewport, ...) at
        // Plugins/FX/Niagara/.../NiagaraSystemToolkitModeBase.cpp:497, so the stored type name is
        // "SNiagaraSystemViewport" - which does not end in "EditorViewport".
        { TEXT("SNiagaraSystemViewport"),
          TEXT("Plugins/FX/Niagara/Source/NiagaraEditor/Private/Widgets/SNiagaraSystemViewport.h:28 (: public SEditorViewport)"), false }
    };

    // Toolkit names whose IAssetEditorInstance* is known to be an FAssetEditorToolkit*. See the
    // header for why the gate is mandatory rather than advisory.
    const FName GSupportedAssetEditorToolkitNames[] =
    {
        // FStaticMeshEditor : IStaticMeshEditor : FAssetEditorToolkit
        // (Editor/StaticMeshEditor/Public/IStaticMeshEditor.h:23)
        FName(TEXT("StaticMeshEditor")),
        // ISkeletalMeshEditor : FPersonaAssetEditorToolkit : FWorkflowCentricApplication :
        // FAssetEditorToolkit (ISkeletalMeshEditor.h:11, PersonaAssetEditorToolkit.h:17,
        // WorkflowCentricApplication.h:19)
        FName(TEXT("SkeletalMeshEditor")),
        FName(TEXT("AnimationEditor")),        // IAnimationEditor.h:18, same chain
        FName(TEXT("SkeletonEditor")),         // ISkeletonEditor.h:11, same chain
        // IAnimationBlueprintEditor : FBlueprintEditor : IBlueprintEditor :
        // FWorkflowCentricApplication (IAnimationBlueprintEditor.h:12, BlueprintEditor.h:189,
        // BlueprintEditorModule.h:132)
        FName(TEXT("AnimationBlueprintEditor")),
        // FNiagaraSystemToolkit : FWorkflowCentricApplication (NiagaraSystemToolkit.h:48). The name
        // is FName("Niagara"), not "NiagaraEditor": GetToolkitFName returns it literally
        // (NiagaraSystemToolkit.cpp:351-354) and FAssetEditorToolkit::GetEditorName forwards to it.
        FName(TEXT("Niagara"))
    };

    // Every FEditorViewportClient alive right now, mapped from the widget it points back at.
    // Built once per search rather than per widget: GetAllViewportClients() is small (one entry
    // per open viewport) but the widget tree is not.
    void CollectRegisteredEditorViewportWidgets(TMap<const SWidget*, TSharedPtr<SEditorViewport>>& Out)
    {
        if (!GEditor)
        {
            return;
        }
        for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
        {
            if (!Client)
            {
                continue;
            }
            // TYPED by the engine, not by us: FEditorViewportClient stores a
            // TWeakPtr<SEditorViewport> and hands it back through a public inline accessor
            // (EditorViewportClient.h:1299). Nothing here downcasts.
            TSharedPtr<SEditorViewport> Widget = Client->GetEditorViewportWidget();
            if (Widget.IsValid())
            {
                Out.Add(static_cast<const SWidget*>(Widget.Get()), Widget);
            }
        }
    }

    // A type name that would have tripped the old suffix predicate, or that reads as a viewport and
    // might belong on the allow-list. SViewport itself is excluded: every SEditorViewport contains
    // one, so reporting it would bury the name that actually matters.
    bool LooksLikeAViewportType(const FString& TypeName)
    {
        return TypeName.EndsWith(TEXT("Viewport")) && TypeName != TEXT("SViewport");
    }

    // Cap on the names carried into a refusal message. A pathological tree must not turn one error
    // string into a kilobyte of widget names.
    constexpr int32 GMaxReportedUnverifiedTypes = 8;

    void SearchWidgetTree(const TSharedRef<SWidget>& Widget,
        const TMap<const SWidget*, TSharedPtr<SEditorViewport>>& RegisteredWidgets,
        FEditorViewportSearch& InOutResult)
    {
        if (InOutResult.Viewport.IsValid())
        {
            return;
        }

        const FString TypeName = Widget->GetTypeAsString();

        // ---- verification 1: the engine's own client registry, no cast ----
        if (const TSharedPtr<SEditorViewport>* Registered = RegisteredWidgets.Find(&Widget.Get()))
        {
            TSharedPtr<SEditorViewport> Candidate = *Registered;
            if (Candidate.IsValid() && Candidate->GetViewportClient().IsValid() &&
                Candidate->GetSceneViewport().IsValid())
            {
                InOutResult.Viewport = Candidate;
                InOutResult.bVerifiedByClientRegistry = true;
                return;
            }
        }
        // ---- verification 2: the exact-name allow-list ----
        else if (IsAllowListedEditorViewportType(TypeName))
        {
            // Defined behaviour because of the line above and only because of it: the name is an
            // exact match against an entry whose ": public SEditorViewport" was checked in the
            // engine source and cited beside it.
            TSharedRef<SEditorViewport> Candidate = StaticCastSharedRef<SEditorViewport>(Widget);
            if (Candidate->GetViewportClient().IsValid() && Candidate->GetSceneViewport().IsValid())
            {
                InOutResult.Viewport = Candidate;
                InOutResult.bVerifiedByAllowList = true;
                return;
            }
        }
        else if (LooksLikeAViewportType(TypeName))
        {
            // NOT CAST. This is the branch that used to be undefined behaviour: the old predicate
            // reached StaticCastSharedRef on nothing more than a name suffix, and stock UE 5.8
            // ships five SCompoundWidgets whose names end in "EditorViewport".
            if (InOutResult.UnverifiedTypeNames.Num() < GMaxReportedUnverifiedTypes)
            {
                InOutResult.UnverifiedTypeNames.AddUnique(TypeName);
            }
        }

        FChildren* Children = Widget->GetAllChildren();
        if (!Children)
        {
            return;
        }
        for (int32 Index = 0; Index < Children->Num(); ++Index)
        {
            SearchWidgetTree(Children->GetChildAt(Index), RegisteredWidgets, InOutResult);
            if (InOutResult.Viewport.IsValid())
            {
                return;
            }
        }
    }
}

TArrayView<const FEditorViewportTypeEntry> GetEditorViewportTypeAllowList()
{
    return TArrayView<const FEditorViewportTypeEntry>(
        GEditorViewportTypeAllowList, UE_ARRAY_COUNT(GEditorViewportTypeAllowList));
}

bool IsAllowListedEditorViewportType(const FString& WidgetTypeName)
{
    for (const FEditorViewportTypeEntry& Entry : GEditorViewportTypeAllowList)
    {
        // EXACT, case-sensitive. A prefix or suffix test is what this function replaces, and a
        // case-insensitive match would let a host's "SNiagarasystemViewport" through on a name the
        // engine never produces.
        if (WidgetTypeName.Equals(Entry.TypeName, ESearchCase::CaseSensitive))
        {
            return true;
        }
    }
    return false;
}

FEditorViewportSearch FindEditorViewportInWidgetTree(const TSharedRef<SWidget>& Root)
{
    FEditorViewportSearch Result;
    TMap<const SWidget*, TSharedPtr<SEditorViewport>> RegisteredWidgets;
    CollectRegisteredEditorViewportWidgets(RegisteredWidgets);
    SearchWidgetTree(Root, RegisteredWidgets, Result);
    return Result;
}

bool FindEditorViewportInWidgetTree(const TSharedRef<SWidget>& Root,
    TSharedPtr<SEditorViewport>& OutViewport, FString& OutErrCode, FString& OutErrMsg)
{
    const FEditorViewportSearch Result = FindEditorViewportInWidgetTree(Root);
    if (Result.Viewport.IsValid())
    {
        OutViewport = Result.Viewport;
        return true;
    }

    OutErrCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
    if (Result.UnverifiedTypeNames.Num() > 0)
    {
        OutErrMsg = FString::Printf(
            TEXT("No verified editor viewport in this widget tree. %s look like viewports but ")
            TEXT("neither registered an FEditorViewportClient nor appear in the verified widget-type ")
            TEXT("allow-list, so they were NOT cast: casting a widget on a name match alone is ")
            TEXT("undefined behaviour (SEditorViewport has no SLATE_DECLARE_WIDGET). If one of them ")
            TEXT("really derives from SEditorViewport, add it to the allow-list in CaptureSubject.cpp ")
            TEXT("with its engine header citation."),
            *FString::Join(Result.UnverifiedTypeNames, TEXT(", ")));
    }
    else
    {
        OutErrMsg = TEXT("No editor viewport in this widget tree: nothing in it registered an "
                         "FEditorViewportClient and no widget type matched the verified allow-list.");
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Asset editor acquisition
// ---------------------------------------------------------------------------------------------

TArrayView<const FName> GetSupportedAssetEditorToolkitNames()
{
    return TArrayView<const FName>(
        GSupportedAssetEditorToolkitNames, UE_ARRAY_COUNT(GSupportedAssetEditorToolkitNames));
}

bool IsSupportedAssetEditorToolkit(FName ToolkitName)
{
    for (const FName& Name : GSupportedAssetEditorToolkitNames)
    {
        if (Name == ToolkitName)
        {
            return true;
        }
    }
    return false;
}

TSharedPtr<SEditorViewport> FindAssetEditorViewport(IAssetEditorInstance* EditorInstance,
    UObject* Asset, TArrayView<const FName> AcceptedToolkitNames,
    FString& OutErrCode, FString& OutErrMsg)
{
    if (!EditorInstance)
    {
        OutErrCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
        OutErrMsg = TEXT("No asset editor instance to walk for a preview viewport.");
        return nullptr;
    }
    if (!FSlateApplication::IsInitialized())
    {
        OutErrCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
        OutErrMsg = TEXT("Slate is not initialized, so no asset editor window can be realized or walked.");
        return nullptr;
    }

    // ---- the toolkit gate, and it runs BEFORE any cast, which is the entire point ----
    //
    // GetEditorName is on IAssetEditorInstance itself (AssetEditorSubsystem.h:59), so this costs no
    // cast to evaluate. Two of the three stock implementers of IAssetEditorInstance are not
    // FAssetEditorToolkits - UAssetEditor is a UObject (Tools/UAssetEditor.h:22) and SMiniCurveEditor
    // is a Slate widget (MiniCurveEditor.h:15) - so an ungated static_cast is undefined behaviour on
    // either of them.
    const FName ToolkitName = EditorInstance->GetEditorName();
    bool bAccepted = false;
    for (const FName& Accepted : AcceptedToolkitNames)
    {
        if (Accepted == ToolkitName)
        {
            bAccepted = true;
            break;
        }
    }
    // Belt and braces: even a caller that passed a wider list than the verified one cannot reach the
    // cast for a toolkit nobody checked.
    if (bAccepted && !IsSupportedAssetEditorToolkit(ToolkitName))
    {
        bAccepted = false;
    }
    if (!bAccepted)
    {
        TArray<FString> AcceptedStrings;
        for (const FName& Accepted : AcceptedToolkitNames)
        {
            AcceptedStrings.Add(Accepted.ToString());
        }
        OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
        OutErrMsg = FString::Printf(
            TEXT("Asset editor '%s' is not one this capture path can drive (accepted: %s)."),
            *ToolkitName.ToString(),
            AcceptedStrings.Num() > 0 ? *FString::Join(AcceptedStrings, TEXT(", ")) : TEXT("(none)"));
        return nullptr;
    }

    // DEFINED because of the gate above, and only because of it.
    FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);

    EditorInstance->FocusWindow(Asset);
    FSlateApplication::Get().PumpMessages();
    FSlateApplication::Get().Tick(ESlateTickType::All);

    const TSharedRef<SWidget> ParentWidget = Toolkit->GetToolkitHost()->GetParentWidget();
    FEditorViewportSearch Search = FindEditorViewportInWidgetTree(ParentWidget);
    if (!Search.Viewport.IsValid())
    {
        // Same fallback the four shipped copies used: a toolkit whose viewport tab lives in a
        // floating window is not under the parent widget.
        TSharedPtr<SWindow> HostWindow = FSlateApplication::Get().FindWidgetWindow(ParentWidget);
        if (HostWindow.IsValid())
        {
            FEditorViewportSearch WindowSearch = FindEditorViewportInWidgetTree(HostWindow.ToSharedRef());
            for (const FString& Unverified : WindowSearch.UnverifiedTypeNames)
            {
                if (Search.UnverifiedTypeNames.Num() < GMaxReportedUnverifiedTypes)
                {
                    Search.UnverifiedTypeNames.AddUnique(Unverified);
                }
            }
            if (WindowSearch.Viewport.IsValid())
            {
                Search.Viewport = WindowSearch.Viewport;
                Search.bVerifiedByClientRegistry = WindowSearch.bVerifiedByClientRegistry;
                Search.bVerifiedByAllowList = WindowSearch.bVerifiedByAllowList;
            }
        }
    }

    if (!Search.Viewport.IsValid())
    {
        const FString AssetName = Asset ? Asset->GetPathName() : FString(TEXT("(no asset)"));
        OutErrCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
        OutErrMsg = Search.UnverifiedTypeNames.Num() > 0
            ? FString::Printf(
                TEXT("No verified preview viewport in the '%s' window for %s. Widget types that look ")
                TEXT("like viewports but were not verified, and so were NOT cast: %s. Add a type to the ")
                TEXT("allow-list in CaptureSubject.cpp with its engine header citation if it really ")
                TEXT("derives from SEditorViewport."),
                *ToolkitName.ToString(), *AssetName,
                *FString::Join(Search.UnverifiedTypeNames, TEXT(", ")))
            : FString::Printf(
                TEXT("No preview viewport in the '%s' window for %s."),
                *ToolkitName.ToString(), *AssetName);
        return nullptr;
    }
    return Search.Viewport;
}

TSharedPtr<SEditorViewport> FindAssetEditorViewport(IAssetEditorInstance* EditorInstance,
    UObject* Asset, FString& OutErrCode, FString& OutErrMsg)
{
    return FindAssetEditorViewport(EditorInstance, Asset, GetSupportedAssetEditorToolkitNames(),
        OutErrCode, OutErrMsg);
}

// ---------------------------------------------------------------------------------------------
// The deferred close, and the pool of one
// ---------------------------------------------------------------------------------------------
//
// Rationale for both is on the declarations in CaptureSubject.h. In short: destroying an asset
// editor toolkit from inside a capture's release path is an editor-killing access violation, and
// leaving every opened editor behind is an unbounded leak whose end state (an asset editor still
// open at exit) is its own documented crash. One queue answers both - the close is moved off the
// release stack onto the core ticker, and an editor a caller asked to keep is kept only until the
// next capture starts.

namespace
{
    struct FPendingAssetEditorClose
    {
        TWeakObjectPtr<UObject> Asset;
        // Kept BESIDE the weak pointer, not derived from it: a caller asks "is a close queued for
        // this path" after the release has already run, and the answer must survive the asset
        // being collected between the queue and the question.
        FString PackagePath;
    };

    TArray<FPendingAssetEditorClose>& PendingAssetEditorCloses()
    {
        static TArray<FPendingAssetEditorClose> GPending;
        return GPending;
    }

    // Asset editors THIS subsystem opened and has not queued a close for - the pool the
    // `closeAfterCapture: false` path is bounded by. A window the caller already had open never
    // enters it, so a caller's own tab is never evicted by somebody else's capture.
    TArray<TWeakObjectPtr<UObject>>& CaptureOpenedAssetEditors()
    {
        static TArray<TWeakObjectPtr<UObject>> GOpened;
        return GOpened;
    }

    bool& DeferredCloseTickerArmed()
    {
        static bool bGArmed = false;
        return bGArmed;
    }

    FString PackagePathOfAsset(const UObject* Asset)
    {
        const UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
        return Package ? Package->GetName() : FString();
    }

    // `/Game/X/SM_Foo.SM_Foo` and `/Game/X/SM_Foo` name the same asset, and both spellings reach
    // the verbs, so the queue is keyed on the package half that the two agree on.
    FString PackagePathOfWirePath(const FString& AssetPath)
    {
        int32 DotIndex = INDEX_NONE;
        return AssetPath.FindLastChar(TEXT('.'), DotIndex) ? AssetPath.Left(DotIndex) : AssetPath;
    }

    void ForgetCaptureOpenedAssetEditor(const UObject* Asset)
    {
        CaptureOpenedAssetEditors().RemoveAll([Asset](const TWeakObjectPtr<UObject>& Held)
        {
            return !Held.IsValid() || Held.Get() == Asset;
        });
    }

    bool TickDeferredAssetEditorCloses(float)
    {
        DeferredCloseTickerArmed() = false;
        FlushDeferredAssetEditorCloses();
        return false;   // one-shot; the next queue entry arms a fresh one
    }

    void ArmDeferredCloseTicker()
    {
        if (DeferredCloseTickerArmed())
        {
            return;
        }
        DeferredCloseTickerArmed() = true;
        // 0.0s: the very next core-ticker pass, which FEngineLoop::Tick runs after GEngine->Tick
        // has returned. That is one full unwind away from the release path, which is the whole
        // distance this mechanism buys.
        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateStatic(&TickDeferredAssetEditorCloses), 0.0f);
    }

    // THE POOL OF ONE. Called on every open, before the acquire can fail: an acquire that opens a
    // window and then refuses on a missing viewport used to leak it outright, because the
    // provider's Release had no state to close with. Entering the pool at the open makes that path
    // bounded too, without a per-provider line.
    void NoteAssetEditorOpenedByCapture(UObject* Asset, bool bWasAlreadyOpen)
    {
        if (!Asset)
        {
            return;
        }
        // Iterated over a COPY: scheduling mutates the pool.
        const TArray<TWeakObjectPtr<UObject>> Previous = CaptureOpenedAssetEditors();
        for (const TWeakObjectPtr<UObject>& Held : Previous)
        {
            UObject* Other = Held.Get();
            if (!Other || Other == Asset)
            {
                continue;
            }
            UE_LOG(LogPinWrightCaptureSubject, Log,
                TEXT("Capture opened an asset editor for %s while %s was still open from an earlier ")
                TEXT("capture; queueing the earlier one for close. At most one capture-opened asset ")
                TEXT("editor is kept, because an editor still open at exit faults during shutdown."),
                *Asset->GetPathName(), *Other->GetPathName());
            ScheduleDeferredAssetEditorClose(Other);
        }
        CaptureOpenedAssetEditors().RemoveAll([](const TWeakObjectPtr<UObject>& Held)
        {
            return !Held.IsValid();
        });
        if (bWasAlreadyOpen)
        {
            // The caller's own window. Never adopted into the pool - evicting it later would close
            // a tab this plugin did not open.
            return;
        }
        const bool bAlreadyPooled = CaptureOpenedAssetEditors().ContainsByPredicate(
            [Asset](const TWeakObjectPtr<UObject>& Held) { return Held.Get() == Asset; });
        if (!bAlreadyPooled)
        {
            CaptureOpenedAssetEditors().Add(TWeakObjectPtr<UObject>(Asset));
        }
    }
}

void ScheduleDeferredAssetEditorClose(UObject* Asset)
{
    if (!Asset)
    {
        return;
    }
    // Queued means "no longer this subsystem's to keep open", so it leaves the pool here rather
    // than when the tick runs - otherwise the next capture would queue it a second time.
    ForgetCaptureOpenedAssetEditor(Asset);
    if (GEditor)
    {
        // Nothing open, nothing to queue. Without this a pooled entry whose window somebody else
        // already closed would sit in the queue and be reported by
        // HasPendingDeferredAssetEditorClose as a close that is coming, which is a claim about a
        // window that is already gone.
        UAssetEditorSubsystem* AssetEditorSubsystem =
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        if (AssetEditorSubsystem &&
            AssetEditorSubsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false) == nullptr)
        {
            return;
        }
    }
    for (const FPendingAssetEditorClose& Pending : PendingAssetEditorCloses())
    {
        if (Pending.Asset.Get() == Asset)
        {
            return;
        }
    }
    FPendingAssetEditorClose Entry;
    Entry.Asset = Asset;
    Entry.PackagePath = PackagePathOfAsset(Asset);
    PendingAssetEditorCloses().Add(MoveTemp(Entry));
    ArmDeferredCloseTicker();
}

bool HasPendingDeferredAssetEditorClose(const FString& AssetPath)
{
    if (AssetPath.IsEmpty())
    {
        return false;
    }
    const FString Wanted = PackagePathOfWirePath(AssetPath);
    for (const FPendingAssetEditorClose& Pending : PendingAssetEditorCloses())
    {
        if (!Pending.PackagePath.IsEmpty() && Pending.PackagePath == Wanted)
        {
            return true;
        }
    }
    return false;
}

int32 NumPendingDeferredAssetEditorCloses()
{
    return PendingAssetEditorCloses().Num();
}

int32 FlushDeferredAssetEditorCloses()
{
    if (PendingAssetEditorCloses().Num() == 0)
    {
        return 0;
    }
    // Preconditions checked BEFORE the drain, so a queue that cannot be serviced yet is not
    // silently thrown away.
    if (!GEditor || !FSlateApplication::IsInitialized())
    {
        return 0;
    }
    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!AssetEditorSubsystem)
    {
        return 0;
    }

    // Drained before anything is closed: a toolkit destructor can re-enter this plugin, and a
    // second pass over a queue this one is already servicing would close the same asset twice.
    const TArray<FPendingAssetEditorClose> Pending = MoveTemp(PendingAssetEditorCloses());
    PendingAssetEditorCloses().Reset();

    // A toolkit teardown can raise a save prompt, and this pass runs outside FRpcDispatcher's own
    // unattended scope (Dispatch/ScopedUnattendedRpc.h: the scope spans only the synchronous
    // handler body). Without this a deferred close could park a modal on the game thread with no
    // caller left to answer it.
    FScopedUnattendedRpc UnattendedScope;

    int32 ClosedCount = 0;
    for (const FPendingAssetEditorClose& Entry : Pending)
    {
        UObject* Asset = Entry.Asset.Get();
        if (!Asset)
        {
            // Collected between the queue and the tick; its editor went with it.
            continue;
        }
        if (AssetEditorSubsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false) == nullptr)
        {
            // Somebody closed it first - the user, editor.quit, another verb. Nothing to do, and
            // nothing to claim.
            continue;
        }
        // RE-RUN AT EXECUTION TIME, not trusted from the queue. The holder count is a property of
        // this instant: a verb that resolved the same asset again between the queue and the tick
        // is holding its FSceneViewport right now, and closing under that aborts the process on
        // check(SceneViewport.IsUnique()).
        const int32 ExtraHolders = CountPreviewSceneViewportHolders(Asset);
        if (ExtraHolders > 0)
        {
            UE_LOG(LogPinWrightCaptureSubject, Error,
                TEXT("Deferred close of the asset editor for %s abandoned: %d other reference(s) to ")
                TEXT("its preview FSceneViewport are alive, and SEditorViewport's destructor asserts ")
                TEXT("that pointer is unique (UE 5.8 SEditorViewport.cpp:65). The window is left OPEN."),
                *Asset->GetPathName(), ExtraHolders);
            continue;
        }
        AssetEditorSubsystem->CloseAllEditorsForAsset(Asset);
        if (AssetEditorSubsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false) == nullptr)
        {
            ++ClosedCount;
        }
        else
        {
            // An asset editor can veto its own close. Reported, not retried: a queue that re-armed
            // itself against a toolkit that refuses would spin every frame for the session.
            UE_LOG(LogPinWrightCaptureSubject, Warning,
                TEXT("The asset editor for %s refused its deferred close and is still open."),
                *Asset->GetPathName());
        }
    }
    return ClosedCount;
}

bool AcquireAssetEditorViewport(UObject* Asset, TArrayView<const FName> AcceptedToolkitNames,
    FAssetEditorViewportAcquisition& OutAcquisition, FString& OutErrCode, FString& OutErrMsg)
{
    if (!Asset)
    {
        OutErrCode = ErrorCodes::ERR_INVALID_ARGUMENT;
        OutErrMsg = TEXT("No asset to open an editor for.");
        return false;
    }
    if (!GEditor)
    {
        OutErrCode = ErrorCodes::ERR_EDITOR_NOT_AVAILABLE;
        OutErrMsg = TEXT("Editor not available");
        return false;
    }
    if (!FSlateApplication::IsInitialized())
    {
        OutErrCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
        OutErrMsg = TEXT("Slate is not initialized, so no asset editor window can be realized or walked.");
        return false;
    }
    if (AcceptedToolkitNames.Num() == 0)
    {
        // An empty list is NOT "accept anything". The toolkit downcast below is only defined for a
        // toolkit that is one, so a caller that names none has asked for an unchecked cast.
        OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
        OutErrMsg = TEXT("A capture-subject provider must name the asset editor toolkits it accepts; "
                         "an empty list would mean casting an unknown IAssetEditorInstance.");
        return false;
    }
    for (const FName& Accepted : AcceptedToolkitNames)
    {
        if (!IsSupportedAssetEditorToolkit(Accepted))
        {
            OutErrCode = ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR;
            OutErrMsg = FString::Printf(
                TEXT("Toolkit '%s' is not in the verified FAssetEditorToolkit allow-list, so its ")
                TEXT("IAssetEditorInstance cannot be safely cast. Add it to ")
                TEXT("GetSupportedAssetEditorToolkitNames() with the engine citation that proves it ")
                TEXT("derives from FAssetEditorToolkit."),
                *Accepted.ToString());
            return false;
        }
    }

    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!AssetEditorSubsystem)
    {
        OutErrCode = ErrorCodes::ERR_SUBSYSTEM_MISSING;
        OutErrMsg = TEXT("AssetEditorSubsystem not available");
        return false;
    }

    // THE SHARED PREVIEW-SCENE PROFILE, SNAPSHOTTED BEFORE THE OPEN AND PUT BACK ON EVERY EXIT.
    //
    // OPENING AN ASSET EDITOR IS ITSELF A WRITE to the process-wide UAssetViewerSettings profile
    // array, with no capture involved and nothing this plugin asked for:
    //   * SNiagaraSystemViewport::Construct calls SetFloorVisibility(false) with `bDirect`
    //     defaulted false (UE 5.8 Plugins/FX/Niagara/.../SNiagaraSystemViewport.cpp:872), which
    //     assigns Profiles[CurrentProfileIndex].bShowFloor and fires PostEditChangeProperty
    //     (AdvancedPreviewScene.cpp:391-409). Every later static-mesh, skeletal-mesh, material and
    //     Persona preview in that session then loses its floor.
    //   * Merely CONSTRUCTING an FAdvancedPreviewScene can do it too. The constructor ends in
    //     UpdateScene(Profile), which compares GetLightDirection() -- DERIVED from the component's
    //     +X axis (PreviewScene.cpp:264-272), not stored -- against Profile.DirectionalLightRotation
    //     with an exact FRotator::operator!= and writes the component's rotation back into the
    //     shared profile when they differ (AdvancedPreviewScene.cpp:175-188). The engine's own
    //     comment at :175 concedes the two need not match.
    // Both are flushed to the COMMITTED Config/DefaultEditor.ini by the destructor of any "Preview
    // Scene Settings" details tab (SAdvancedPreviewDetailsTab.cpp:46), which is exactly what the
    // close below destroys.
    //
    // SO THE SNAPSHOT HAS TO BE TAKEN HERE, NOT IN THE CAPTURE UTIL. PinWrightRenderCapture::
    // CaptureEditorViewportToPng carries the same guard, but it runs AFTER the subject resolver has
    // already opened the editor -- the mutation is inside its snapshot at entry and is therefore
    // PRESERVED rather than undone. This guard is what closes that gap; the two nest harmlessly,
    // because the inner one restores to a state this one has already made clean.
    //
    // AND IT IS RESTORED WHEN THIS FUNCTION RETURNS, not when the capture ends, because the config
    // write fires on CLOSE. A guard held across the capture would put the profile back after the
    // details tab had already serialised the poisoned value. Restoring here means the profile is
    // clean for the capture, for the close, and for every other asset editor already open.
    //
    // ONE VISIBLE CONSEQUENCE, deliberate and recorded rather than discovered: putting bShowFloor
    // back to the user's value broadcasts, and the four-way UpdateScene it drives re-applies the
    // profile's floor visibility to every live preview scene (AdvancedPreviewScene.cpp:231). A
    // Niagara preview captured through this path therefore shows the floor the user's profile asks
    // for instead of the one SNiagaraSystemViewport::Construct hid behind the session's back. The
    // capture verbs' own `previewScene.showFloor` is how a caller chooses, per capture, and it is
    // restored with it.
    //
    // Costs nothing when nothing moved: RestoreSharedProfiles compares field-wise and returns
    // without writing or broadcasting if the array is untouched, which is every re-open of an
    // already-open editor.
    PinWrightPreviewSceneRig::FScopedSharedProfiles SharedProfileGuard;

    // Read BEFORE the open, because it is the field that separates a cold first frame from a warm
    // one (docs/wiki-src/render.md:164) and it drives the three-state close rule.
    OutAcquisition.bWasAlreadyOpen = AssetEditorSubsystem->FindEditorForAsset(Asset, false) != nullptr;
    if (!AssetEditorSubsystem->OpenEditorForAsset(Asset))
    {
        OutErrCode = ErrorCodes::ERR_OPEN_FAILED;
        OutErrMsg = FString::Printf(TEXT("Failed to open asset editor for: %s"), *Asset->GetPathName());
        return false;
    }

    // HERE, and not on the success path: the acquire can still refuse below on a viewport it
    // cannot find, and the window this call just opened is open either way. Registering it at the
    // open is what bounds the leak on every exit path rather than only the happy one.
    NoteAssetEditorOpenedByCapture(Asset, OutAcquisition.bWasAlreadyOpen);

    IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Asset, true);
    if (!EditorInstance)
    {
        OutErrCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
        OutErrMsg = FString::Printf(TEXT("Asset editor not found after opening: %s"), *Asset->GetPathName());
        return false;
    }

    OutAcquisition.ToolkitName = EditorInstance->GetEditorName();
    TSharedPtr<SEditorViewport> Found = FindAssetEditorViewport(
        EditorInstance, Asset, AcceptedToolkitNames, OutErrCode, OutErrMsg);
    if (!Found.IsValid())
    {
        return false;
    }

    OutAcquisition.ViewportWidget = Found;
    TSharedPtr<FEditorViewportClient> Client = Found->GetViewportClient();
    OutAcquisition.SceneViewport = Found->GetSceneViewport();
    if (!Client.IsValid() || !OutAcquisition.SceneViewport.IsValid())
    {
        OutErrCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
        OutErrMsg = FString::Printf(
            TEXT("The '%s' preview viewport for %s has no realized viewport client."),
            *OutAcquisition.ToolkitName.ToString(), *Asset->GetPathName());
        return false;
    }
    OutAcquisition.ViewportClient = Client.Get();
    return true;
}

// SEditorViewport::SceneViewport itself, plus the by-value pointer GetSceneViewport() hands the
// probe below. See the citation in CountPreviewSceneViewportHolders.
static constexpr int32 HealthyCloseSceneViewportRefCount = 2;

int32 CountPreviewSceneViewportHolders(UObject* Asset)
{
    if (!Asset || !GEditor || !FSlateApplication::IsInitialized())
    {
        return 0;
    }
    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!AssetEditorSubsystem)
    {
        return 0;
    }
    IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false);
    if (!EditorInstance)
    {
        return 0;
    }
    // The same verified walk the acquire used - no cast is performed off an unverified widget name.
    // A toolkit this walk declines is simply UNMEASURABLE here, which returns 0 and lets the close
    // proceed exactly as it always did; the guard narrows nothing it cannot see.
    FString ProbeErrCode;
    FString ProbeErrMsg;
    const TSharedPtr<SEditorViewport> ViewportWidget =
        FindAssetEditorViewport(EditorInstance, Asset, ProbeErrCode, ProbeErrMsg);
    if (!ViewportWidget.IsValid())
    {
        return 0;
    }
    const TSharedPtr<FSceneViewport> SceneViewport = ViewportWidget->GetSceneViewport();
    if (!SceneViewport.IsValid())
    {
        return 0;
    }
    // GetSceneViewport() returns the shared pointer BY VALUE (UE 5.8
    // Editor/UnrealEd/Public/SEditorViewport.h:96), so the probe above is itself one of the
    // references counted. A healthy close therefore reads exactly HealthyCloseSceneViewportRefCount:
    // SEditorViewport::SceneViewport (:356) plus this local. Anything beyond that is somebody else
    // still holding the viewport. SViewport keeps only a TWeakPtr to the interface
    // (Runtime/Slate/Public/Widgets/SViewport.h:256), which is why the engine's own
    // check(SceneViewport.IsUnique()) is satisfiable at all and why 2 is the right baseline here.
    return SceneViewport.GetSharedReferenceCount() - HealthyCloseSceneViewportRefCount;
}

bool CloseAssetEditor(UObject* Asset, bool bCloseAfterCapture, bool bCloseRequestedExplicitly,
    bool bWasAlreadyOpen)
{
    if (!Asset || !GEditor)
    {
        return false;
    }
    UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!AssetEditorSubsystem)
    {
        return false;
    }
    // Three states, not two, carried verbatim from RenderHandler.cpp:395-412. The default closes
    // only what this call opened, because a window the caller already had open is theirs. An
    // EXPLICIT true closes it either way: the previous guard silently did nothing on the second
    // capture of the same asset - the caller asked for cleanup, the already-open window from the
    // FIRST capture looked user-owned, and nothing said so.
    if (!bCloseAfterCapture || (bWasAlreadyOpen && !bCloseRequestedExplicitly))
    {
        // Left open ON PURPOSE, so nothing is queued here. What bounds it is the pool of one in
        // NoteAssetEditorOpenedByCapture: a window this subsystem opened and was told to keep
        // survives until the NEXT capture opens one, and no longer. Unbounded accumulation is not
        // a caller's choice to make - 29 of 37 asset editors leaked in one measured session, and
        // the end state of that leak is a fault during editor shutdown.
        return false;
    }

    // Nothing open, nothing to close, and "gone" is the measured truth rather than a claim about
    // work this call did. Also drops the asset from the pool: an editor that is not open cannot be
    // the one the next capture evicts.
    if (AssetEditorSubsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false) == nullptr)
    {
        ForgetCaptureOpenedAssetEditor(Asset);
        return true;
    }

    // THE LAST GATE BEFORE A CRASH, and the reason this defect cannot come back as a dead process.
    //
    // CloseAllEditorsForAsset tears the asset editor's layout down, which destroys the preview
    // SEditorViewport, whose destructor asserts check(SceneViewport.IsUnique()) - a check(), live
    // in Development, not an ensure. Any other live TSharedPtr<FSceneViewport> at that instant
    // kills the process, and it killed a full suite run mid-queue from camera.orbit_shots.
    //
    // Every holder inside this plugin is now dropped before the close, so this count is expected to
    // be zero forever. It is measured anyway because the failure mode it guards is unrecoverable and
    // invisible at the call site: a NEW verb that copies Resolved.SceneViewport into a local and
    // releases while holding it reintroduces the same crash, and no reviewer can see that from the
    // call site alone. REFUSING the close is strictly better than taking it: a window left open
    // faults only at editor shutdown (docs/lessons.md:166) and reports honestly as `false` here,
    // whereas closing kills the run and every test after it.
    const int32 ExtraHolders = CountPreviewSceneViewportHolders(Asset);
    if (ExtraHolders > 0)
    {
        UE_LOG(LogPinWrightCaptureSubject, Error,
            TEXT("Refusing to close the asset editor for %s: %d other reference(s) to its preview ")
            TEXT("FSceneViewport are still alive, and SEditorViewport's destructor asserts that ")
            TEXT("pointer is unique (UE 5.8 SEditorViewport.cpp:65) - closing now would abort the ")
            TEXT("process. Whatever copied FResolvedSubject::SceneViewport must drop it before ")
            TEXT("releasing the subject; PinWrightCaptureSubject::ReleaseSubjectAndViewportRefs ")
            TEXT("does both in the right order. The window is left OPEN and reported as not closed."),
            *Asset->GetPathName(), ExtraHolders);
        return false;
    }

    // NOT CLOSED ON THIS STACK, AND THAT IS THE FIX.
    //
    // CloseAllEditorsForAsset used to be called right here. It runs the toolkit's entire
    // destructor chain synchronously, and running that chain from a capture's release path is an
    // EXCEPTION_ACCESS_VIOLATION that takes the whole editor process down - twice in one session,
    // through two different providers, with every frame from this function outward identical
    // (board B-capture-asset-preview-no-safe-close-mode). The gate above defends a different
    // crash and cannot see this one. The full argument, and why the core ticker is the safe place
    // to run it instead, is on ScheduleDeferredAssetEditorClose in CaptureSubject.h.
    ScheduleDeferredAssetEditorClose(Asset);
    // FALSE, because the window is still there as this returns and `assetEditorClosed` means
    // measured, not intended. Callers publish `assetEditorCloseDeferred` beside it - see
    // MakeSubjectInfoObject - so "queued, gone next tick" is distinguishable from "left open".
    return false;
}

}   // namespace PinWrightCaptureSubject
