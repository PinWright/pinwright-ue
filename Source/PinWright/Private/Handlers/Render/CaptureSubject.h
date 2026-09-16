// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

class AActor;
class FEditorViewportClient;
class FJsonObject;
class FSceneViewport;
class IAssetEditorInstance;
class SEditorViewport;
class SWidget;
class UObject;

// THE CAPTURE SUBJECT RESOLVER: one acquisition step for every kind of thing a camera can be
// pointed at.
//
// WHAT WAS ACTUALLY MISSING. PinWrightRenderCapture::CaptureEditorViewportToPng already renders
// any FEditorViewportClient, and PinWrightPoseCapture::CaptureCameraPoses already drives a list of
// cameras through it; neither knows or cares what is in front of the lens. The step that was
// duplicated per verb is the one ABOVE them - turning "a static mesh at /Game/X" or "the actor
// named Rock_12" into (a viewport client, a scene viewport, a bounding sphere, and optionally a
// way to move the subject to an instant). That step existed in four independent copies, and every
// copy carried its own bugs: the orbit-camera suppression defect was diagnosed and fixed twice
// (AnimationPreviewCaptureHandler.cpp:34-45 and again for the static-mesh path), and the widget
// downcast below was wrong in both of them.
//
// KINDS REGISTER THEMSELVES. A provider is a static-init RegisterProvider() call from one file per
// kind, exactly the shape FAutoRegisterHandler uses for RPC handlers. There is deliberately no
// central switch on ESubjectKind anywhere in this file: adding a kind creates a file and edits
// none. Resolve() looks the provider up by kind and knows nothing else about it.
//
// BOUNDS COME FROM A STATIC SOURCE, NEVER FROM THE POSED SUBJECT. Each provider chooses, and says
// which it chose in FResolvedSubject::BoundsSource. That is not a style preference: a camera
// solved from a posed skeletal mesh's bounds moves under the subject between frames of a burst and
// destroys the comparison the burst exists to support (AnimationShotsHandler.cpp:23-29), while an
// animation preview must use the MESH ASSET's bounds so the same shot is framed identically at
// every instant (AnimationPreviewCaptureHandler.cpp:42-44).
//
// RELEASE IS STRUCTURAL, NOT DISCIPLINE. FResolvedSubject releases itself in its destructor, so a
// verb that returns early - including through an error path it did not anticipate - cannot leave
// an asset editor window open. That matters beyond tidiness: an asset editor still open when the
// editor exits faults in ~FStaticMeshEditor (docs/lessons.md:166). A verb that needs the release to
// happen before it builds its response (so that `assetEditorClosed` is MEASURED rather than
// predicted) calls ReleaseSubject() explicitly; the destructor then does nothing.
namespace PinWrightCaptureSubject
{
    // ---- the six domains ----
    //
    // The wire spellings are world | actor | staticMesh | skeletalMesh | animation | niagara.
    // ToWireName is the single producer of those strings; nothing else should spell them.
    enum class ESubjectKind : uint8
    {
        World,
        Actor,
        StaticMesh,
        SkeletalMesh,
        Animation,
        Niagara
    };

    // The `kind` string as it appears on the wire and in the `subject` response block.
    const TCHAR* ToWireName(ESubjectKind Kind);

    // Wire string -> kind. Case-insensitive on the first letter only would be a trap (`staticmesh`
    // and `staticMesh` reading differently is the kind of thing a caller cannot see), so the whole
    // comparison is case-insensitive.
    bool ParseKindName(const FString& Name, ESubjectKind& OutKind);

    // Number of kinds, so a caller can walk them without knowing the last one's name:
    //   for (int32 i = 0; i < SubjectKindCount; ++i) { ESubjectKind K = (ESubjectKind)i; ... }
    inline constexpr int32 SubjectKindCount = static_cast<int32>(ESubjectKind::Niagara) + 1;

    // Every wire spelling joined for a refusal message that lists what IS accepted.
    FString AllKindWireNamesJoined(const TCHAR* Separator = TEXT(" | "));

    // ---- the request: parsed from the wire, before anything is opened ----
    struct FSubjectRequest
    {
        // ---- §2.2 of docs/capture-subject-convergence.md, frozen; do not rename or retype ----
        ESubjectKind Kind = ESubjectKind::World;
        FString      AssetPath;
        FString      ActorName;
        FString      AnimationPath;
        FVector      Point = FVector::ZeroVector;
        float        Radius = 0.0f;        // <= 0 means "solve from bounds"
        bool         bCloseAfterCapture = true;

        // ---- additive, beyond the frozen list; nothing in the frozen set changes meaning ----
        //
        // A `subject` object (or a legacy spelling that normalises into one) was on the wire at
        // all. The `subject` RESPONSE block is emitted only when this is true AND the resolve
        // succeeded - an empty block on a verb the caller gave no subject to would become the
        // fourteenth assert-absent case nobody knew about (§3.3 of the plan).
        bool bProvided = false;
        // `kind` was spelled explicitly rather than inferred. Inference reads the asset's UClass,
        // so it can fail; an explicit kind never does.
        bool bKindProvided = false;
        // A `point` was spelled explicitly. Point defaults to the origin, so "point at the world
        // origin" and "no point at all" are otherwise the same value and the point-without-radius
        // refusal (CameraFrameHandler.cpp:466-468) is unreachable. ParseSubject sets this whenever
        // a `point` object is on the wire, whatever its components are.
        //
        // ONE RESIDUAL EDGE, documented rather than papered over: a caller that reaches a provider
        // WITHOUT going through ParseSubject - filling FSubjectRequest by hand, which the level
        // provider supports - leaves both flags false, and the provider then infers "a point was
        // meant" from a non-zero point or a positive radius. That inference is right in every case
        // except a point at EXACTLY the world origin with no radius, which falls through to level
        // bounds instead of refusing. Framing the level is a defensible answer to an unanswerable
        // payload; it is called out here so nobody rediscovers it as a bug.
        bool bPointProvided = false;
        bool bRadiusProvided = false;
        // `closeAfterCapture` was spelled explicitly. The semantics are THREE-STATE, not two:
        // absent closes only a window this call opened; an explicit true closes one the caller
        // already had open as well; false leaves it. Carried verbatim from
        // RenderHandler.cpp:395-412, where dropping the distinction silently made the second
        // capture of the same mesh stop cleaning up.
        bool bCloseAfterCaptureProvided = false;
    };

    // ---- what a capture needs, and nothing more ----

    // Provider-private per-call state, type-erased so this header pulls in no asset editor, no
    // UObject and no Slate. A provider that must remember something between Acquire and Release -
    // which asset it opened, the playhead it found, the Niagara bounds it pinned - subclasses this
    // and stores it in FResolvedSubject::ProviderState. The frozen FSubjectProvider::Release takes
    // only FResolvedSubject&, and providers are registered once at static init, so there is
    // nowhere else per-call state can live.
    struct FSubjectReleaseState
    {
        virtual ~FSubjectReleaseState() = default;
    };

    // ---- SHOW OR HIDE THE SUBJECT, WITHOUT TOUCHING ANYTHING ELSE IN THE SCENE ---------------
    //
    // The seam the subject-coverage differential is built on: draw the shot, hide the subject,
    // draw the same pose again, and the pixels that changed ARE the subject. It answers the one
    // question no other published signal can - "is the thing I asked for actually in this
    // picture" - because over pure backdrop `boundsInFrame`, `blank` and `litPixelFraction` all
    // read healthy (the bounds ARE geometrically in frame; the backdrop IS lit).
    //
    // WHAT A PROVIDER MUST GUARANTEE TO BIND ONE:
    //   * it hides the SUBJECT and nothing else. Hiding the preview scene's floor, sky or lights
    //     would put backdrop pixels into the differential and over-report coverage;
    //   * bVisible=true restores what the component had when the subject was acquired, not a
    //     hardcoded true - a preview component that arrived hidden must not be revealed by the
    //     act of measuring it;
    //   * the provider's Release restores visibility too, so an abandoned capture cannot leave an
    //     artist's tab holding an invisible asset.
    //
    // UNBOUND IS A LEGAL ANSWER and means "this kind cannot be measured this way". The primitive
    // takes the CONJUNCTION of asked-and-able (PoseListCapture.cpp:67), so an unbound setter
    // yields an ABSENT `subjectCoverage` - never a zero, which would be a measurement claim
    // nobody made.
    using FSubjectVisibilitySetter =
        TFunction<bool(bool bVisible, FString& OutErrCode, FString& OutErrMsg)>;

    struct FResolvedSubject
    {
        // ---- §2.2, frozen; do not rename or retype ----
        FEditorViewportClient*     ViewportClient = nullptr;   // never null on success
        TSharedPtr<FSceneViewport> SceneViewport;              // never invalid on success
        FVector  BoundsOrigin = FVector::ZeroVector;
        double   BoundsRadius = 0.0;       // 0 => EvaluateBoundsFraming reports bEvaluated=false
        bool     bTimeSupported = false;
        double   TimeStartSeconds = 0.0;
        double   TimeEndSeconds   = 0.0;
        FString  CaptureSource;            // "staticMeshEditorPreview" etc. - verbatim existing strings
        // Where the bounding sphere came from. THE COMPLETE SET, read off the landed providers
        // rather than copied from the plan - §2.2's list omitted "levelBounds" while C2's own
        // acceptance criterion required it, so the frozen prose contradicted itself:
        //   "levelBounds"       - the world kind with no point: an ALevelBounds actor when the
        //                         persistent level has one, else the union of every actor's
        //                         bounds (CaptureSubjectProviders_Level.h:54)
        //   "point"             - the world kind with a bare point + radius (:57)
        //   "actorBounds"       - one actor at one instant (:59)
        //   "sampledUnion"      - one subject unioned across every sampled instant (:61, and the
        //                         animation provider's own union)
        //   "assetBounds"       - the mesh ASSET's bounds, never the posed component
        //                         (CaptureSubjectProviders_Mesh.h:50)
        //   "pinnedFixedBounds" - Niagara, pinned with SetSystemFixedBounds for the call
        //                         (CaptureSubjectProviders_Niagara.h:406)
        // A new provider adds its own spelling here in the same commit that emits it.
        FString  BoundsSource;
        bool     bEditorWasAlreadyOpen = false;
        bool     bEditorClosed = false;
        bool     bTimeReproducible = false;// FALSE for Niagara, always. §4.2

        // ---- additive, beyond the frozen list ----
        //
        // REQUIRED, not convenience: MakeSubjectInfoObject must publish `subject.kind`, and the
        // frozen list has no field it could read that from. C11's acceptance criterion
        // (`subject.kind == "niagara"`) is unmeetable without it.
        ESubjectKind Kind = ESubjectKind::World;
        // Echoed into the `subject` block so the response names what was framed. Empty for the
        // kind that does not use it, and omitted from the JSON when empty.
        FString AssetPath;
        FString ActorName;
        // The bounds are known to be wrong or unmeasurable and the response must say so rather
        // than silently mis-frame. Set by the Niagara provider for a GPU emitter with no authored
        // fixed bounds, which reports a hardcoded 200 cm cube regardless of its real extent
        // (NiagaraEmitter.h:449, NiagaraEmitterInstanceImpl.cpp:960-995).
        FString BoundsWarning;
        // Which determinism scope is off (§4.2). There is deliberately NO `reproducible: true`
        // field anywhere: Niagara determinism defaults off at all three scopes and is void under a
        // variable tick delta, so the response reports what was done and warns, never promises.
        FString ReproducibilityWarning;

        // Per-call state the provider's Release needs. See FSubjectReleaseState.
        TSharedPtr<FSubjectReleaseState> ProviderState;

        // Bound by the provider that acquired this subject when the kind can be hidden without
        // hiding the backdrop; left unbound when it cannot. See FSubjectVisibilitySetter.
        //
        // ON THE SUBJECT rather than an out-parameter of Acquire on purpose: FSubjectProvider's
        // three-argument Acquire signature is frozen (§2.2) and shared by five providers, so a
        // fourth out-parameter would be a five-file signature change to carry one optional
        // capability. A member is additive - a provider that binds nothing keeps every existing
        // caller byte-identical.
        FSubjectVisibilitySetter VisibilitySetter;

        // Installed by Resolve from the provider that acquired this subject. Held here rather
        // than looked up by Kind so that a provider supplied directly to ResolveWithProvider -
        // which is how the release discipline is tested without mutating the global registry -
        // releases through the same path a registered one does.
        TFunction<void(FResolvedSubject&)> ReleaseFunc;
        // Release has already run. Makes "exactly once" a property of the code: the explicit
        // ReleaseSubject(), the failure path inside Resolve, and the destructor all funnel through
        // the same guard.
        bool bReleased = false;

        FResolvedSubject() = default;
        // Releases if nothing released it first. Non-inline so this header needs no definition of
        // anything the release touches.
        ~FResolvedSubject();

        // NON-COPYABLE. A copy would carry the release responsibility twice and either
        // double-release or release the viewport out from under the capture. Deleting the copy
        // turns that into a compile error instead of a runtime one.
        FResolvedSubject(const FResolvedSubject&) = delete;
        FResolvedSubject& operator=(const FResolvedSubject&) = delete;
        FResolvedSubject(FResolvedSubject&& Other);
        FResolvedSubject& operator=(FResolvedSubject&& Other);
    };

    // Set the subject to one instant. Returns false and fills the error pair when the kind has no
    // time axis; a caller that never asks for a time never sees the refusal.
    //
    // TWO REFUSAL CODES, DELIBERATELY NOT UNIFIED. A setter that refuses is saying one of two
    // different things and the caller acts differently on each:
    //   - ERR_INVALID_ARGUMENT  - the subject HAS a time axis, the caller just did not name the
    //                             time source (a world/actor subject with no `sequencePath`, or a
    //                             non-finite instant). CALLER-FIXABLE: supply the argument. Matches
    //                             camera.animation_shots' existing "sequencePath is required".
    //   - ERR_UNSUPPORTED_ASSET_EDITOR - the kind has no time axis at all (decision 6's typed
    //                             refusal), or its provider shipped no driver. NOT caller-fixable.
    // Collapsing them would report an unfixable-sounding error for a fixable problem, and a caller
    // that reads "unsupported" stops instead of adding the argument that would have worked.
    using FSubjectTimeSetter = TFunction<bool(double TimeSeconds, FString& OutErrCode, FString& OutErrMsg)>;

    struct FSubjectProvider
    {
        ESubjectKind Kind;
        TFunction<bool(const FSubjectRequest&, FResolvedSubject&, FSubjectTimeSetter&,
                       FString& OutErrCode, FString& OutErrMsg)> Acquire;
        TFunction<void(FResolvedSubject&)> Release;   // runs on EVERY exit path, error paths included
    };

    // ---- the resolver ----

    // Read the wire shape. Accepts BOTH spellings, and they mean the same thing:
    //   - the nested object: {"subject": {kind, path, name, point, radius, animation, closeAfterCapture}}
    //   - the legacy top-level keys every capture verb already ships: assetPath, actorName (and
    //     its ActorNameParamUtils aliases), point + radius
    // `kind` is inferred when omitted, from exactly ONE present identifying key. A payload naming
    // two of them is refused with INVALID_ARGUMENT listing both key names as the caller spelled
    // them - ranking them silently is how a caller ends up believing the other one is in force.
    //
    // A `subject` object DOES NOT absorb a legacy target key sitting beside it: `{subject:{...},
    // actorName:"X"}` is refused naming both, not resolved to the nested one. That shape is the
    // likelier caller mistake of the two - half a payload moved into the new object - and reading
    // only the nested half would hand back a successful capture of something the caller did not
    // ask for. `radius` is exempt: it is a modifier, not a target, and camera.orbit_shots has
    // taken a top-level `radius` since it shipped.
    //
    // Returns true with Kind=World and bProvided=false when the payload names no subject at all,
    // which is what "the level itself" means; a caller that needs to distinguish that from an
    // explicit world subject reads bProvided.
    bool ParseSubject(const TSharedPtr<FJsonObject>& Payload, FSubjectRequest& Out,
                      FString& OutErrCode, FString& OutErrMsg);

    // Acquire the subject through its registered provider.
    //
    // ON FAILURE the provider's Release has ALREADY RUN and OutSubject is marked released, so a
    // caller that keeps the object around cannot double-release it. On success the caller owns the
    // release: either explicitly through ReleaseSubject (do this when the response must report a
    // measured `assetEditorClosed`) or implicitly when OutSubject goes out of scope.
    //
    // OutTimeSetter is ALWAYS filled, so a caller never invokes an empty TFunction. Every provider
    // installs its own - including the no-time-axis refusal for a kind that has none, and the
    // caller-fixable ERR_INVALID_ARGUMENT for a kind that has one but was given no time source.
    // Resolve's own fallback is reached only by a provider that supplied nothing at all, and it
    // reports that as the provider defect it is; it can never turn a fixable missing-argument case
    // into an unfixable-sounding one, because the provider that owns that case supplies its own
    // setter and the fallback never runs.
    bool Resolve(const FSubjectRequest& Request, FResolvedSubject& OutSubject,
                 FSubjectTimeSetter& OutTimeSetter, FString& OutErrCode, FString& OutErrMsg);

    // Resolve against a provider supplied directly instead of one looked up in the registry.
    // Resolve() is this function plus the lookup. Exposed because the release discipline has to be
    // testable - a test that registered a fake provider globally would change what every other
    // test in the process resolves.
    bool ResolveWithProvider(const FSubjectProvider& Provider, const FSubjectRequest& Request,
                             FResolvedSubject& OutSubject, FSubjectTimeSetter& OutTimeSetter,
                             FString& OutErrCode, FString& OutErrMsg);

    // Run the provider's Release now, at most once per resolved subject. Idempotent, and a no-op
    // on a subject that was never resolved.
    //
    // CLEARS ViewportClient AND SceneViewport BEFORE the provider's Release runs. That is not
    // tidiness: the Release ends in CloseAssetEditor, and destroying an asset editor's
    // SEditorViewport asserts check(SceneViewport.IsUnique()) (UE 5.8
    // Editor/UnrealEd/Private/SEditorViewport.cpp:65), so a surviving reference aborts the process
    // rather than leaking. Clearing here rather than in each provider is what stops a new provider
    // from reintroducing the crash by forgetting.
    void ReleaseSubject(FResolvedSubject& Subject);

    // ReleaseSubject for a caller that COPIED the subject's viewport pair into its own variables.
    //
    // READ THIS BEFORE WRITING `SceneViewport = Resolved.SceneViewport` IN A VERB. The copy is a
    // real reference the resolver cannot reach, so ReleaseSubject alone is not enough: the close
    // inside the provider's Release then destroys the SEditorViewport while the caller's copy is
    // still alive and the engine's check() aborts the editor. That is precisely how a full suite
    // run died from camera.orbit_shots. This drops the caller's pair first, then releases, so the
    // ordering is carried by the call instead of by whoever remembers to write two lines.
    //
    // Both parameters are cleared. Pass the exact variables the verb captured; a verb that never
    // copied them calls plain ReleaseSubject.
    void ReleaseSubjectAndViewportRefs(FResolvedSubject& Subject,
        FEditorViewportClient*& InOutViewportClient, TSharedPtr<FSceneViewport>& InOutSceneViewport);

    // How many references to an open asset editor's preview FSceneViewport are alive BEYOND the
    // two a healthy close sees (the widget's own, and the probe's). Zero means closing the editor
    // is safe; anything higher means CloseAllEditorsForAsset would abort the process on
    // check(SceneViewport.IsUnique()).
    //
    // Returns 0 - "no reason to refuse" - whenever the answer is unmeasurable: no editor open, a
    // toolkit the verified walk declines, Slate down. The guard can only ever refuse on a POSITIVE
    // measurement, so it cannot narrow a close it does not understand.
    int32 CountPreviewSceneViewportHolders(UObject* Asset);

    // The `subject` response block. Present-only-when-it-has-something-to-say, per §3.3: `kind` is
    // unconditional, everything else appears only when it was measured. Callers emit the block
    // itself only when a subject was actually resolved.
    TSharedPtr<FJsonObject> MakeSubjectInfoObject(const FResolvedSubject& Subject);

    // Called from static init, one file per kind. A second registration for a kind replaces the
    // first and warns; it is never silently ignored, because "the provider I wrote is not the one
    // running" is otherwise invisible.
    void RegisterProvider(const FSubjectProvider& Provider);

    // The provider registered for a kind, or nullptr. For a verb that wants to refuse a kind
    // before doing any work rather than after.
    const FSubjectProvider* FindProvider(ESubjectKind Kind);

    // ---- §2.5: the asset-editor viewport walk, once ----
    //
    // THE OLD PREDICATE WAS UNDEFINED BEHAVIOUR, not merely incomplete. Four copies of this walk
    // shipped the same two lines:
    //
    //     if (WidgetType == TEXT("SEditorViewport") || WidgetType.EndsWith(TEXT("EditorViewport")))
    //         TSharedRef<SEditorViewport> V = StaticCastSharedRef<SEditorViewport>(Widget);
    //
    // SEditorViewport carries no SLATE_DECLARE_WIDGET, so there is no runtime hierarchy check to
    // fall back on and the suffix IS the whole test. Stock UE 5.8 ships five widget types whose
    // names end in "EditorViewport" and which derive from SCompoundWidget, not SEditorViewport:
    // STextureEditorViewport (Editor/TextureEditor/Private/Widgets/STextureEditorViewport.h:19),
    // SFontEditorViewport (Editor/FontEditor/Private/SFontEditorViewport.h:23),
    // SCurveEditorViewport (Editor/DistCurveEditor/Private/SCurveEditorViewport.h:23),
    // SMediaPlayerEditorViewport (Plugins/Media/MediaPlayerEditor/.../SMediaPlayerEditorViewport.h:17)
    // and SSimulcamEditorViewport (Plugins/VirtualProduction/CameraCalibration/.../SSimulcamEditorViewport.h:15).
    // Walking a Texture editor's window with the old predicate casts one of them and then calls
    // GetViewportClient() through a pointer into unrelated memory.
    //
    // AND IT MISSED THE ONE THING THAT MATTERS MOST. SNiagaraSystemViewport derives from
    // SEditorViewport (Plugins/FX/Niagara/.../SNiagaraSystemViewport.h:28) but its name does not
    // end in "EditorViewport", so the suffix rule refused the widget that made every Niagara
    // capture look impossible. The allow-list is not a narrowing; it is what made that reachable.
    //
    // WHAT REPLACES IT. Two independent verifications, in this order:
    //   1. THE ENGINE'S OWN REGISTRY, which needs no cast at all. Every FEditorViewportClient adds
    //      itself to GEditor->GetAllViewportClients() in its constructor and removes itself in its
    //      destructor (EditorViewportClient.cpp:601, :681), and each one holds a TYPED
    //      TWeakPtr<SEditorViewport> back to its widget (EditorViewportClient.h:1299). Matching the
    //      walked widget's address against that set identifies it as an SEditorViewport on the
    //      engine's authority, and hands back a pointer the engine already typed - so nothing is
    //      downcast.
    //   2. THE EXACT-NAME ALLOW-LIST below, for a real SEditorViewport whose client has not
    //      registered (or when GEditor is unavailable). Each entry is justified by a checked
    //      ": public SEditorViewport" in C:\UE_5.8\Engine, cited in the entry itself.
    // A name that satisfies neither is REPORTED, never cast: FEditorViewportSearch collects it and
    // the caller refuses with PREVIEW_VIEWPORT_NOT_FOUND naming the type, so the next kind is a
    // one-line allow-list addition with an engine citation rather than a crash.

    struct FEditorViewportTypeEntry
    {
        // Exact SWidget::GetTypeAsString(), which SNew stores verbatim from the stringified type
        // (SWidget.cpp:1398-1400). Never a prefix or suffix test.
        const TCHAR* TypeName = nullptr;
        // header:line of the ": public SEditorViewport" (or ": public SAssetEditorViewport") that
        // justifies this entry, under C:\UE_5.8\Engine\Source or \Plugins.
        const TCHAR* EngineEvidence = nullptr;
        // The type is reachable from a PUBLIC engine header, so CaptureSubject.cpp carries a
        // static_assert(TIsDerivedFrom<T, SEditorViewport>::Value) for it and the claim cannot rot.
        // False means the type lives in a private engine header that cannot be included from a
        // plugin; the evidence string is then the whole proof, and it is re-checked by hand when
        // the engine version moves. Deliberately not hidden: an entry that is only human-verified
        // should look different from one the compiler checks.
        bool bCompileTimeVerified = false;
    };

    TArrayView<const FEditorViewportTypeEntry> GetEditorViewportTypeAllowList();
    bool IsAllowListedEditorViewportType(const FString& WidgetTypeName);

    struct FEditorViewportSearch
    {
        // Valid only when a widget was verified. Never produced by a cast off an unverified name.
        TSharedPtr<SEditorViewport> Viewport;
        // Types encountered that LOOK like a viewport (they end in "EditorViewport", or they carry
        // "Viewport" in the name) but satisfied neither verification. These are what the refusal
        // message names, and they are exactly the set the old code would have cast.
        TArray<FString> UnverifiedTypeNames;
        // Which verification accepted the widget, so a test can tell the two apart.
        bool bVerifiedByClientRegistry = false;
        bool bVerifiedByAllowList = false;
    };

    // Depth-first, first match wins - the same order the four copies used, so a Persona layout with
    // four preview tabs still returns the one it always returned.
    FEditorViewportSearch FindEditorViewportInWidgetTree(const TSharedRef<SWidget>& Root);

    // FindEditorViewportInWidgetTree plus the typed refusal. OutErrCode is
    // ERR_PREVIEW_VIEWPORT_NOT_FOUND and the message names every unverified type the walk saw.
    bool FindEditorViewportInWidgetTree(const TSharedRef<SWidget>& Root,
        TSharedPtr<SEditorViewport>& OutViewport, FString& OutErrCode, FString& OutErrMsg);

    // ---- asset editor acquisition ----

    struct FAssetEditorViewportAcquisition
    {
        TSharedPtr<SEditorViewport> ViewportWidget;
        FEditorViewportClient*      ViewportClient = nullptr;
        TSharedPtr<FSceneViewport>  SceneViewport;
        // IAssetEditorInstance::GetEditorName() of the toolkit that was opened.
        FName ToolkitName;
        // The caller already had this asset's editor open before the acquire. Drives the
        // three-state close rule and is the field that separates a cold first frame from a warm
        // one (docs/wiki-src/render.md:164).
        bool bWasAlreadyOpen = false;
    };

    // Toolkit names this walk will cast an IAssetEditorInstance* for. THE GATE IS MANDATORY and it
    // is the second half of the unchecked-downcast fix.
    //
    // static_cast<FAssetEditorToolkit*>(EditorInstance) is what reaches GetToolkitHost(), because
    // IAssetEditorInstance itself has no such accessor (AssetEditorSubsystem.h:55-96). Stock UE 5.8
    // has THREE direct implementers of IAssetEditorInstance and only one of them is an
    // FAssetEditorToolkit: FAssetEditorToolkit (Toolkits/AssetEditorToolkit.h:116),
    // UAssetEditor - a UObject (Tools/UAssetEditor.h:22) - and SMiniCurveEditor, a Slate widget
    // (MiniCurveEditor.h:15). Casting either of the other two is undefined behaviour, and
    // UAssetEditor is not exotic: it is the base of the newer interactive-tools asset editors.
    // Gating on a verified toolkit name before the cast is what makes it defined.
    TArrayView<const FName> GetSupportedAssetEditorToolkitNames();
    bool IsSupportedAssetEditorToolkit(FName ToolkitName);

    // Walk an ALREADY-OPEN asset editor for its preview viewport. This is the shared replacement
    // for the four copies of the walk (RenderHandler.cpp:50-99,
    // AnimationPreviewCaptureHandler.cpp:100-151, EditorWindowHandlers.cpp:187,
    // WidgetDesignerCaptureUtil.cpp:70) and it fixes both halves of the unchecked-downcast defect:
    // the toolkit name is gated before the FAssetEditorToolkit cast, and the widget is verified
    // before the SEditorViewport cast.
    //
    // Returns null with the error pair filled; the caller sends it verbatim.
    //
    // The two-argument form accepts every toolkit in GetSupportedAssetEditorToolkitNames(). A
    // provider that serves one domain passes its own narrower list instead, so a Static Mesh
    // subject cannot silently end up photographing a Persona viewport.
    TSharedPtr<SEditorViewport> FindAssetEditorViewport(IAssetEditorInstance* EditorInstance,
        UObject* Asset, FString& OutErrCode, FString& OutErrMsg);
    TSharedPtr<SEditorViewport> FindAssetEditorViewport(IAssetEditorInstance* EditorInstance,
        UObject* Asset, TArrayView<const FName> AcceptedToolkitNames,
        FString& OutErrCode, FString& OutErrMsg);

    // Open (or find) the asset's editor and hand back its preview viewport.
    //
    // AcceptedToolkitNames must be non-empty and every entry must be in
    // GetSupportedAssetEditorToolkitNames(); an empty list is refused rather than treated as
    // "accept anything", because "anything" is what the cast above cannot survive.
    bool AcquireAssetEditorViewport(UObject* Asset, TArrayView<const FName> AcceptedToolkitNames,
        FAssetEditorViewportAcquisition& OutAcquisition, FString& OutErrCode, FString& OutErrMsg);

    // The three-state close rule, once, so every provider spells it the same way. Returns the
    // MEASURED outcome - whether the editor is gone by the time this returns - because an asset
    // editor can refuse its own close, and a close reported but not performed leaves the
    // shutdown-crash precondition in place while the response says it is gone.
    //
    // IT NO LONGER DESTROYS THE TOOLKIT ON THE CALLER'S STACK; it queues the close and returns
    // false. See ScheduleDeferredAssetEditorClose below for why, and for what a caller publishes
    // instead. A caller that needs the close to have HAPPENED calls
    // FlushDeferredAssetEditorCloses() from a stack that is not a capture release path.
    //
    // ALSO REFUSES A CLOSE THAT WOULD ABORT THE PROCESS. CountPreviewSceneViewportHolders is
    // consulted first; a positive count logs an error naming the asset and returns false without
    // queueing anything. `false` is honest there in exactly the sense above - the window really is
    // still open - so the measured `assetEditorClosed` contract is unchanged.
    bool CloseAssetEditor(UObject* Asset, bool bCloseAfterCapture, bool bCloseRequestedExplicitly,
        bool bWasAlreadyOpen);

    // ---- the deferred close: why closing an asset editor from a release path kills the editor ----
    //
    // UAssetEditorSubsystem::CloseAllEditorsForAsset runs the toolkit's whole destructor chain
    // SYNCHRONOUSLY - FAssetEditorToolkit::CloseWindow -> the toolkit host's
    // ShutdownToolkitHost -> ~FNiagaraSystemToolkit / ~FStaticMeshEditor -> Slate window
    // destruction -> FLayoutSaveRestore::SaveToConfig. Running that chain from inside a capture's
    // release path took the whole editor process down twice in one session, through two DIFFERENT
    // providers, with every frame from CloseAssetEditor outward identical
    // (board B-capture-asset-preview-no-safe-close-mode): an EXCEPTION_ACCESS_VIOLATION reading
    // 0x0 in the Niagara stack view model, and one reading 0x3f800000 - the bit pattern of 1.0f
    // dereferenced as a pointer, i.e. a freed-and-reused allocation - in the mesh case.
    //
    // WHY THE EXISTING GATE DOES NOT COVER IT. CountPreviewSceneViewportHolders defends a
    // DIFFERENT crash: SEditorViewport's destructor asserting check(SceneViewport.IsUnique()).
    // These faults are inside the toolkit's own destructor chain, downstream of the close, which
    // that gate does not and cannot see. CloseAssetEditor looked carefully defended and was not.
    //
    // WHAT THE RELEASE PATH IS. The capture verbs pump Slate, draw the viewport and flush the
    // rendering commands three times per capture, from a handler that the dispatcher may be
    // running INSIDE the engine frame (UEditorEngine::Tick -> SimpleTickObjects -> a task-graph
    // named-thread wait -> FRpcDispatcher::ProcessRequest). PinWrightSafePoint::IsSafeNow() used to
    // read only bInTick and was blind to that stack; it now also tests IsInsideNamedThreadPump(), so
    // the dispatcher defers there rather than running inline. That narrows how a capture reaches this
    // path but does not remove the hazard - a direct in-editor caller is in no pump and still runs
    // inline, and the release sequence below is unsafe on its own terms. The provider's Release
    // restores preview-component
    // state - EnablePreview, SetVisibility, SetPosition, a Niagara component put back on its own
    // clock - and closes the toolkit in the same statement sequence, before any of it has been
    // through a frame. Tearing a toolkit and its Slate window down there is what faults.
    //
    // WHAT REPLACES IT. The asset is queued and closed from FTSTicker::GetCoreTicker(), which
    // FEngineLoop::Tick pumps AFTER GEngine->Tick returns - the same safe point
    // Dispatch/SafePoint.h proves and the whole plugin already defers world teardown onto. By
    // then the RPC stack has unwound, the provider state is dead and Slate is not inside the
    // frame the capture re-entered. The queued close re-runs the holder gate and the read-back at
    // execution time, because the state it gates on can move between the queue and the tick.
    //
    // THE HONESTY COST, PAID EXPLICITLY. `assetEditorClosed` still means "the window was gone
    // when this call answered", so on the deferred path it is FALSE. A response that said `true`
    // would be predicting the tick rather than measuring it. `assetEditorCloseDeferred` is the
    // field that separates "queued, and it will be gone next tick" from "left open by request";
    // MakeSubjectInfoObject publishes it for every asset kind by asking
    // HasPendingDeferredAssetEditorClose, so a provider gains it without a line of its own.
    void ScheduleDeferredAssetEditorClose(UObject* Asset);

    // Whether a close is queued for this asset. Accepts either wire spelling of the path -
    // `/Game/X/SM_Foo` and `/Game/X/SM_Foo.SM_Foo` are the same asset - because callers publish
    // whatever the payload spelled.
    bool HasPendingDeferredAssetEditorClose(const FString& AssetPath);

    // How many closes are queued. For a test that needs the count to be exact rather than
    // non-zero.
    int32 NumPendingDeferredAssetEditorCloses();

    // Run every queued close NOW and report how many editors are actually gone afterwards.
    //
    // The core ticker calls this on its own; call it directly only from a stack that is NOT a
    // capture release path - a test that must measure the close, or a shutdown path that cannot
    // wait for a tick. Draining the queue from inside a release is the very thing this mechanism
    // exists to stop.
    int32 FlushDeferredAssetEditorCloses();
}
