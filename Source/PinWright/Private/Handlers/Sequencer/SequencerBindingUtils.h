// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

// One place where a `sequencer.*` verb mints an object binding.
//
// Before this header there were six hand-rolled binding sites (SequenceHandler.cpp:669/804/929/1039,
// SequencerHandler.cpp:453/478), four of them byte-identical `AddPossessable(Label, Class)` +
// `BindPossessableObject(Guid, Object, EditorWorld)` pairs and none of them able to bind anything
// but an AActor. The four actor sites now call BindActor(); the two AddSpawnable sites are a
// different engine call and are deliberately left alone.
//
// COMPONENT BINDINGS. A component binding in Sequencer is a possessable whose PARENT is the owning
// actor's binding. That parent link is not decoration: MovieSceneHelpers::GetResolutionContext
// (MovieSceneCommonHelpers.cpp:1272-1297) substitutes the resolved parent object as the locator's
// resolution context only when `Possessable->GetParent().IsValid() && AreParentContextsSignificant()`.
// A component possessable without it resolves against the world instead of against its actor, finds
// nothing, and every track keyed to it drives nothing - silently. That is why BindComponent() treats
// a missing parent as a hard failure and rolls the binding back out of the sequence.
//
// The engine already implements the whole path and it is headless-viable (unlike
// FSequencerUtilities::CreateBinding, which needs a live ISequencer): ULevelSequence::FindOrAddBinding
// (LevelSequence.cpp:865-941) resolves GetParentObject -> owning actor, recursively binds or REUSES
// that actor's possessable, mints the child, calls FMovieScenePossessable::SetParent(ParentGuid,
// MovieScene) at :930, and binds the child with the ACTOR as context at :925/:937.
//
// FindOrAddBinding is `protected` on ULevelSequence (LevelSequence.h:129, inside #if WITH_EDITOR), so
// it is reached through the PUBLIC base declaration UMovieSceneSequence::CreatePossessable
// (MovieSceneSequence.h:294) - C++ checks member access against the STATIC type of the call
// expression, so a base-typed pointer is a supported entry to the derived protected override, not a
// workaround. ULevelSequence::CreatePossessable (LevelSequence.cpp:943) forwards verbatim.

#include "CoreMinimal.h"

#include "Components/ActorComponent.h"
#include "Compat/EngineVersionCompat.h"
#include "Compilation/MovieSceneCompiledDataManager.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Handlers/ErrorCodes.h"
#include "LevelSequence.h"
#include "MovieScene.h"
// FMovieSceneBindingReferences and the whole universal-object-locator binding model arrived in
// UE 5.4; on 5.3 this header does not exist and bindings are stored as
// FLevelSequenceBindingReferences (path strings) on ULevelSequence itself. Everything in this file
// that reads or writes a locator is gated on the same version below.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
#include "MovieSceneBindingReferences.h"
#endif
#include "MovieSceneCommonHelpers.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSequenceID.h"
// FSharedPlaybackState ships from UE 5.4 on; the header does not exist on 5.3. It is used only by
// the 5.5+ resolve path in ResolveBoundObjects below, so the gate is on the header's existence.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
#include "EntitySystem/MovieSceneSharedPlaybackState.h"
#endif

// Named namespace (not anonymous) so a Unity merge of the sequencer TUs cannot ODR-collide these
// helpers with a sibling file's - the convention CLAUDE.md records for shared handler helpers.
namespace SequencerBindingUtils
{
    // Outcome of one binding attempt.
    //
    // Failure is the default (rpc-design.md Sec.2): a default-constructed FBindingOutcome carries an
    // invalid Guid AND a non-empty ErrorCode, so a code path that forgets to fill it in cannot be
    // mistaken for a success. IsOk() is a conjunction of two independently established facts.
    struct FBindingOutcome
    {
        /** The binding that was created (or reused). Invalid on every failure path. */
        FGuid Guid;

        /**
         * Read back off FMovieScenePossessable::GetParent() AFTER the write - never echoed from the
         * request and never assumed from what FindOrAddBinding was asked to do. Invalid for an actor
         * binding, which legitimately has no parent.
         */
        FGuid ParentGuid;

        /** The parent possessable's stored name, read off the movie scene. */
        FString ParentName;

        /**
         * True only when a resolution measurement was actually taken. The actor path does not take
         * one (it must behave exactly as it did before this header existed), so its callers must not
         * publish an unmeasured boolean under a measurement's name.
         */
        bool bResolutionMeasured = false;

        /** Measured: the binding resolved, through the runtime path, to the object the caller named. */
        bool bResolvesToTarget = false;

        /** Path of the object the binding actually resolved to; empty when it resolved to nothing. */
        FString ResolvedObjectPath;

        FString ErrorCode = ErrorCodes::ERR_BINDING_CREATION_FAILED;
        FString ErrorMessage = TEXT("binding was never attempted");

        bool IsOk() const { return Guid.IsValid() && ErrorCode.IsEmpty(); }

        void MarkOk()
        {
            ErrorCode.Reset();
            ErrorMessage.Reset();
        }

        void Fail(const TCHAR* InCode, const FString& InMessage)
        {
            Guid.Invalidate();
            ErrorCode = InCode;
            ErrorMessage = InMessage;
        }
    };

    enum class ERepointReadbackPhase : uint8
    {
        BeforeWrite,
        AfterWrite,
        AfterRollback
    };

    struct FRepointOutcome
    {
        bool bSuccess = false;
        FGuid BindingGuid;
        FGuid ParentGuid;
        int32 LocatorCount = 0;
        FString BindingName;
        UClass* AuthoredClass = nullptr;
        UClass* NewObjectClass = nullptr;
        FString AuthoredClassPath;
        FString NewObjectClassPath;
        FString OldObjectPath;
        FString ResolvedObjectPath;
        bool bReadbackMeasured = false;
        bool bClassMismatch = false;
        bool bResolvesToNewActor = false;
        bool bOldObjectUnbound = false;
        bool bRollbackAttempted = false;
        bool bRollbackSucceeded = false;
        FString RestoredResolutionStatus = TEXT("not_attempted");
        FString RestoredResolvedObjectPath;
        ERepointReadbackPhase FailureReadbackPhase = ERepointReadbackPhase::BeforeWrite;
        TArray<FString> Warnings;
        FString ErrorCode = ErrorCodes::ERR_BINDING_FAILED;
        FString ErrorMessage = TEXT("repoint was not attempted");

        void Fail(const FString& Code, const FString& Message)
        {
            bSuccess = false;
            ErrorCode = Code;
            ErrorMessage = Message;
        }

        void MarkOk()
        {
            if (bReadbackMeasured && bResolvesToNewActor && bOldObjectUnbound)
            {
                bSuccess = true;
                ErrorCode.Reset();
                ErrorMessage.Reset();
            }
        }
    };

    /** The world every `sequencer.*` binding site resolves against. */
    inline UWorld* GetBindingWorld()
    {
        return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    }

    inline void InvalidateCompiledSequence(ULevelSequence* LevelSequence)
    {
        if (LevelSequence)
        {
            if (UMovieSceneCompiledDataManager* CompiledDataManager =
                    UMovieSceneCompiledDataManager::GetPrecompiledData())
            {
                CompiledDataManager->Reset(LevelSequence);
            }
        }
    }

    /**
     * Every component name on the actor, in GetComponents() order. Used to make a
     * COMPONENT_NOT_FOUND error name the way out (rpc-design.md Sec.7) instead of leaving the caller
     * to guess at spelling.
     */
    inline TArray<FString> ListComponentNames(const AActor* Actor)
    {
        TArray<FString> Names;
        if (!Actor)
        {
            return Names;
        }
        for (const UActorComponent* Component : Actor->GetComponents())
        {
            if (Component)
            {
                Names.Add(Component->GetName());
            }
        }
        Names.Sort();
        return Names;
    }

    /**
     * Exact, case-insensitive match on UActorComponent::GetName() - the internal subobject name,
     * which is unique within one actor (unlike an actor's display label, rpc-design.md Sec.15) and is
     * the name the Details panel and the component list show. Returns nullptr when nothing matches;
     * there is no substring or prefix fallback, because a component binding that silently attaches to
     * a near-miss is exactly the wrong-target defect this plugin keeps paying for.
     */
    inline UActorComponent* FindComponentByName(AActor* Actor, const FString& ComponentName)
    {
        if (!Actor || ComponentName.IsEmpty())
        {
            return nullptr;
        }
        for (UActorComponent* Component : Actor->GetComponents())
        {
            if (Component && Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
            {
                return Component;
            }
        }
        return nullptr;
    }

    // ------------------------------------------------------------------------------------------
    // Locator-backed binding surgery: UE 5.4+ only.
    //
    // `sequencer.repoint_actor` rewrites the FUniversalObjectLocator stored on a possessable's
    // binding reference. On 5.3 there is no locator, no FMovieSceneBindingReferences and no
    // UMovieSceneSequence::MakeLocatorForObject - bindings are FLevelSequenceBindingReferences
    // holding package/object path strings, a different storage model rather than an older spelling
    // of the same one. Rather than reimplement the verb against that model, the machinery is
    // compiled out here and SequenceHandler.cpp refuses the verb by name with
    // UNSUPPORTED_ENGINE_VERSION, which is how this plugin reports every other engine-gated
    // capability.
    // ------------------------------------------------------------------------------------------
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)

    /**
     * FMovieSceneBindingReference::CustomBinding arrived in UE 5.5. On 5.4 no reference can carry
     * a custom binding, so "is this a custom binding" is answerable without the field.
     */
    inline bool HasCustomBinding(const FMovieSceneBindingReference& Reference)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        return Reference.CustomBinding != nullptr;
#else
        (void)Reference;
        return false;
#endif
    }

    /**
     * The non-const GetAllReferences() overload arrived in UE 5.5; 5.4 declares only the const one.
     * The view is over a plain UPROPERTY array on a non-const FMovieSceneBindingReferences, so the
     * const is on the accessor rather than on the storage, and the cast reaches exactly what the
     * 5.5 overload returns.
     */
    inline TArrayView<FMovieSceneBindingReference> GetMutableBindingReferences(
        FMovieSceneBindingReferences& References)
    {
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        return References.GetAllReferences();
#else
        const TArrayView<const FMovieSceneBindingReference> ConstView = References.GetAllReferences();
        return TArrayView<FMovieSceneBindingReference>(
            const_cast<FMovieSceneBindingReference*>(ConstView.GetData()), ConstView.Num());
#endif
    }

#endif // UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0) - binding-reference accessors

    /**
     * Resolve a binding GUID the way playback resolves it, not the way it was written.
     *
     * MovieSceneHelpers::GetBoundObjects walks the possessable's parent chain, asks
     * GetResolutionContext for the context the locator must be resolved against, and runs the
     * universal-object-locator resolve. It shares nothing with the write path beyond the stored
     * binding reference, which is what makes it a verification the write path cannot fake
     * (rpc-design.md Sec.4).
     */
    inline TArray<UObject*> ResolveBoundObjects(ULevelSequence* LevelSeq, const FGuid& Guid)
    {
        TArray<UObject*> Resolved;
        UWorld* World = GetBindingWorld();
        if (!LevelSeq || !World || !Guid.IsValid())
        {
            return Resolved;
        }
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
        TSharedRef<UE::MovieScene::FSharedPlaybackState> PlaybackState =
            MovieSceneHelpers::CreateTransientSharedPlaybackState(World, LevelSeq);
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
        return MovieSceneHelpers::GetBoundObjects(MovieSceneSequenceID::Root, Guid, PlaybackState);
#else
        // GetBoundObjects arrived in 5.7. Before it, the same resolve is reached through
        // GetSingleBoundObject, which runs the identical parent-chain + locator path and returns
        // only the first resolved object (pre-5.7 bindings resolve to at most one).
        if (UObject* Bound = MovieSceneHelpers::GetSingleBoundObject(LevelSeq, Guid, PlaybackState))
        {
            Resolved.Add(Bound);
        }
        return Resolved;
#endif
#else
        // CreateTransientSharedPlaybackState / GetSingleBoundObject both arrived in 5.5. On 5.4
        // the same read is UMovieSceneSequence::LocateBoundObjects, which forwards straight to
        // FMovieSceneBindingReferences::ResolveBinding - the identical universal-object-locator
        // resolve, and equally independent of the write path. The one thing the shared-playback-
        // state path supplies that LocateBoundObjects does not is the resolution context, so it
        // is derived here the way MovieSceneHelpers::GetResolutionContext derives it: the
        // resolved PARENT object for a parented possessable whose sequence says parent contexts
        // are significant, the world otherwise.
        UMovieSceneSequence* SequenceBase = static_cast<UMovieSceneSequence*>(LevelSeq);
        UObject* ResolutionContext = World;
        if (UMovieScene* MovieScene = LevelSeq->GetMovieScene())
        {
            const FMovieScenePossessable* Possessable = MovieScene->FindPossessable(Guid);
            if (Possessable && Possessable->GetParent().IsValid()
                && SequenceBase->AreParentContextsSignificant())
            {
                const TArray<UObject*> ParentObjects = ResolveBoundObjects(LevelSeq, Possessable->GetParent());
                if (ParentObjects.Num() > 0 && ParentObjects[0])
                {
                    ResolutionContext = ParentObjects[0];
                }
            }
        }

        TArray<UObject*, TInlineAllocator<1>> Located;
        // The universal-object-locator model - and with it the FResolveParams overload of
        // LocateBoundObjects - arrived in UE 5.4. 5.3 takes the resolution context directly. The
        // context derived above is the same either way; only the call signature differs.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
        SequenceBase->LocateBoundObjects(
            Guid, UE::UniversalObjectLocator::FResolveParams(ResolutionContext), Located);
#else
        SequenceBase->LocateBoundObjects(Guid, ResolutionContext, Located);
#endif
        for (UObject* Object : Located)
        {
            if (Object)
            {
                Resolved.Add(Object);
            }
        }
        return Resolved;
#endif
    }

#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)

    inline FRepointOutcome RepointTopLevelPossessableActorImpl(
        ULevelSequence* LevelSequence,
        const FGuid& BindingGuid,
        AActor* OldActor,
        AActor* NewActor,
        TFunctionRef<TArray<UObject*>(ULevelSequence*, const FGuid&, ERepointReadbackPhase)> Readback)
    {
        FRepointOutcome Out;
        if (BindingGuid.IsValid())
        {
            Out.BindingGuid = BindingGuid;
        }
        else
        {
            Out.Fail(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("binding GUID is invalid"));
            return Out;
        }

        if (!LevelSequence)
        {
            Out.Fail(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, TEXT("sequence is null"));
            return Out;
        }

        UMovieScene* MovieScene = LevelSequence->GetMovieScene();
        if (!MovieScene)
        {
            Out.Fail(ErrorCodes::ERR_SEQUENCE_NOT_FOUND, TEXT("sequence has no MovieScene"));
            return Out;
        }

        FMovieScenePossessable* Possessable = MovieScene->FindPossessable(BindingGuid);
        if (!Possessable)
        {
            const bool bBindingExists = MovieScene->FindBinding(BindingGuid) != nullptr
                || MovieScene->FindSpawnable(BindingGuid) != nullptr;
            Out.Fail(
                bBindingExists ? ErrorCodes::ERR_UNSUPPORTED_OPERATION : ErrorCodes::ERR_BINDING_NOT_FOUND,
                bBindingExists ? TEXT("binding is not a possessable actor binding") : TEXT("binding was not found"));
            return Out;
        }

        Out.BindingName = Possessable->GetName();
        Out.ParentGuid = Possessable->GetParent();
        Out.AuthoredClass = const_cast<UClass*>(Possessable->GetPossessedObjectClass());
        Out.AuthoredClassPath = Out.AuthoredClass ? Out.AuthoredClass->GetPathName() : FString();

        FMovieSceneBindingReferences* BindingReferences =
            static_cast<UMovieSceneSequence*>(LevelSequence)->GetBindingReferences();
        if (!BindingReferences)
        {
            Out.Fail(ErrorCodes::ERR_UNSUPPORTED_OPERATION, TEXT("sequence has no possessable binding references"));
            return Out;
        }

        const TArrayView<const FMovieSceneBindingReference> References =
            BindingReferences->GetReferences(BindingGuid);
        Out.LocatorCount = References.Num();
        if (References.Num() != 1 || HasCustomBinding(References[0]) || References[0].Locator.IsEmpty())
        {
            Out.Fail(ErrorCodes::ERR_UNSUPPORTED_OPERATION,
                TEXT("repoint requires exactly one non-custom possessable locator"));
            return Out;
        }

        if (Out.ParentGuid.IsValid())
        {
            Out.Fail(ErrorCodes::ERR_UNSUPPORTED_OPERATION, TEXT("parented possessable bindings are unsupported"));
            return Out;
        }

        if (!OldActor || !NewActor || OldActor == NewActor)
        {
            Out.Fail(ErrorCodes::ERR_INVALID_ARGUMENT, TEXT("old and new actors must be distinct and non-null"));
            return Out;
        }

        Out.OldObjectPath = OldActor->GetPathName();
        Out.NewObjectClass = NewActor->GetClass();
        Out.NewObjectClassPath = Out.NewObjectClass ? Out.NewObjectClass->GetPathName() : FString();
        Out.bClassMismatch = Out.AuthoredClass != Out.NewObjectClass;
        if (Out.bClassMismatch)
        {
            Out.Warnings.Add(FString::Printf(
                TEXT("possessable authored class %s differs from new object class %s; authored class metadata retained"),
                *Out.AuthoredClassPath,
                *Out.NewObjectClassPath));
        }

        const FString AuthoredBindingName = Possessable->GetName();
        const UClass* AuthoredBindingClass = Possessable->GetPossessedObjectClass();
        const FGuid AuthoredParentGuid = Possessable->GetParent();
        FMovieSceneBindingReference SavedReference = References[0];

        const TArray<UObject*> BeforeWriteObjects = Readback(
            LevelSequence, BindingGuid, ERepointReadbackPhase::BeforeWrite);
        Out.bReadbackMeasured = true;
        for (UObject* Object : BeforeWriteObjects)
        {
            if (Object)
            {
                Out.ResolvedObjectPath = Object->GetPathName();
                break;
            }
        }
        if (!BeforeWriteObjects.Contains(OldActor))
        {
            Out.FailureReadbackPhase = ERepointReadbackPhase::BeforeWrite;
            Out.Fail(ErrorCodes::ERR_BINDING_UNRESOLVED,
                TEXT("binding does not currently resolve to the old actor"));
            return Out;
        }

        auto RestoreAndMeasure = [&]()
        {
            Out.bRollbackAttempted = true;
            LevelSequence->Modify();
            MovieScene->Modify();

            FMovieSceneBindingReferences* MutableBindingReferences =
                static_cast<UMovieSceneSequence*>(LevelSequence)->GetBindingReferences();
            if (MutableBindingReferences)
            {
                for (FMovieSceneBindingReference& Reference : GetMutableBindingReferences(*MutableBindingReferences))
                {
                    if (Reference.ID == BindingGuid)
                    {
                        Reference.ID = SavedReference.ID;
                        Reference.Locator = SavedReference.Locator;
                        Reference.ResolveFlags = SavedReference.ResolveFlags;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
                        Reference.CustomBinding = SavedReference.CustomBinding;
#endif
                        break;
                    }
                }
            }

            if (FMovieScenePossessable* RestoredPossessable = MovieScene->FindPossessable(BindingGuid))
            {
                RestoredPossessable->SetName(AuthoredBindingName);
                RestoredPossessable->SetPossessedObjectClass(const_cast<UClass*>(AuthoredBindingClass));
            }

            InvalidateCompiledSequence(LevelSequence);
            const TArray<UObject*> AfterRollbackObjects = Readback(
                LevelSequence, BindingGuid, ERepointReadbackPhase::AfterRollback);
            Out.bReadbackMeasured = true;
            Out.RestoredResolvedObjectPath.Reset();
            for (UObject* Object : AfterRollbackObjects)
            {
                if (Object == OldActor)
                {
                    Out.RestoredResolvedObjectPath = Object->GetPathName();
                    break;
                }
                if (Out.RestoredResolvedObjectPath.IsEmpty() && Object)
                {
                    Out.RestoredResolvedObjectPath = Object->GetPathName();
                }
            }
            Out.bRollbackSucceeded = AfterRollbackObjects.Contains(OldActor)
                && !AfterRollbackObjects.Contains(NewActor);
            Out.RestoredResolutionStatus = Out.bRollbackSucceeded
                ? TEXT("restored_old_actor")
                : TEXT("not_restored");
        };

        auto RollbackAndFail = [&](const FString& Code, const FString& Message) -> FRepointOutcome
        {
            RestoreAndMeasure();
            Out.Fail(Code, Message);
            return Out;
        };

        FUniversalObjectLocator NewLocator;
        UMovieSceneSequence* SequenceBase = static_cast<UMovieSceneSequence*>(LevelSequence);
        if (!SequenceBase->MakeLocatorForObject(NewActor, GetBindingWorld(), NewLocator)
            || NewLocator.IsEmpty())
        {
            Out.Fail(ErrorCodes::ERR_BINDING_FAILED,
                TEXT("engine could not create a locator for the replacement actor"));
            return Out;
        }

        FMovieSceneBindingReference* MutableReference = nullptr;
        for (FMovieSceneBindingReference& Reference : GetMutableBindingReferences(*BindingReferences))
        {
            if (Reference.ID == BindingGuid)
            {
                MutableReference = &Reference;
                break;
            }
        }
        if (!MutableReference)
        {
            Out.Fail(ErrorCodes::ERR_BINDING_FAILED,
                TEXT("binding locator disappeared before replacement"));
            return Out;
        }

        LevelSequence->Modify();
        MovieScene->Modify();
        MutableReference->Locator = MoveTemp(NewLocator);
        InvalidateCompiledSequence(LevelSequence);

        FMovieScenePossessable* RepointedPossessable = MovieScene->FindPossessable(BindingGuid);
        const bool bSameBinding = RepointedPossessable
            && RepointedPossessable->GetGuid() == BindingGuid;

        const bool bMetadataPreserved = bSameBinding
            && RepointedPossessable->GetName() == AuthoredBindingName
            && RepointedPossessable->GetParent() == AuthoredParentGuid
            && RepointedPossessable->GetPossessedObjectClass() == AuthoredBindingClass;

        const TArray<UObject*> AfterWriteObjects = Readback(
            LevelSequence, BindingGuid, ERepointReadbackPhase::AfterWrite);
        Out.bReadbackMeasured = true;
        Out.FailureReadbackPhase = ERepointReadbackPhase::AfterWrite;
        Out.ResolvedObjectPath.Reset();
        for (UObject* Object : AfterWriteObjects)
        {
            if (Object)
            {
                Out.ResolvedObjectPath = Object->GetPathName();
                break;
            }
        }
        Out.bResolvesToNewActor = AfterWriteObjects.Contains(NewActor);
        Out.bOldObjectUnbound = Out.bResolvesToNewActor && !AfterWriteObjects.Contains(OldActor);

        if (!bMetadataPreserved || !Out.bResolvesToNewActor || !Out.bOldObjectUnbound)
        {
            return RollbackAndFail(ErrorCodes::ERR_BINDING_UNRESOLVED,
                TEXT("post-write readback did not prove the target binding"));
        }

        Out.MarkOk();
        return Out;
    }

    inline FRepointOutcome RepointTopLevelPossessableActor(
        ULevelSequence* LevelSequence,
        const FGuid& BindingGuid,
        AActor* OldActor,
        AActor* NewActor)
    {
        auto ProductionReadback = [](ULevelSequence* InSequence, const FGuid& InGuid,
            ERepointReadbackPhase) -> TArray<UObject*>
        {
            return ResolveBoundObjects(InSequence, InGuid);
        };
        return RepointTopLevelPossessableActorImpl(
            LevelSequence, BindingGuid, OldActor, NewActor, ProductionReadback);
    }

#if WITH_DEV_AUTOMATION_TESTS
    inline FRepointOutcome RepointTopLevelPossessableActorForTest(
        ULevelSequence* LevelSequence,
        const FGuid& BindingGuid,
        AActor* OldActor,
        AActor* NewActor,
        TFunctionRef<TArray<UObject*>(ULevelSequence*, const FGuid&, ERepointReadbackPhase)> Readback)
    {
        return RepointTopLevelPossessableActorImpl(
            LevelSequence, BindingGuid, OldActor, NewActor, Readback);
    }
#endif

#endif // UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0) - locator-backed binding surgery

    /**
     * Actor possessable - byte-for-byte the pre-existing behaviour of the four hand-rolled sites this
     * replaces: AddPossessable(GetActorLabel(), GetClass()), confirm the possessable landed, then
     * BindPossessableObject with the editor world as context (AddPossessable alone mints an
     * object-UNBOUND possessable whose GUID resolves to nothing).
     *
     * Deliberately takes NO resolution measurement: `sequencer.add_actor` without a componentName has
     * to behave exactly as it did before, so this path adds neither a new gate nor a new response
     * field. bResolutionMeasured stays false and callers must not publish a resolution claim for it.
     */
    inline FBindingOutcome BindActor(ULevelSequence* LevelSeq, AActor* Actor)
    {
        FBindingOutcome Out;
        if (!LevelSeq || !Actor)
        {
            Out.Fail(ErrorCodes::ERR_BINDING_CREATION_FAILED, TEXT("No sequence or no actor to bind"));
            return Out;
        }
        UMovieScene* MovieScene = LevelSeq->GetMovieScene();
        if (!MovieScene)
        {
            Out.Fail(ErrorCodes::ERR_BINDING_CREATION_FAILED, TEXT("Sequence has no MovieScene"));
            return Out;
        }

        const FGuid NewGuid = MovieScene->AddPossessable(Actor->GetActorLabel(), Actor->GetClass());
        if (!MovieScene->FindPossessable(NewGuid))
        {
            Out.Fail(ErrorCodes::ERR_BINDING_CREATION_FAILED, TEXT("Failed to create possessable binding"));
            return Out;
        }

        MovieScene->Modify();
        LevelSeq->Modify();
        LevelSeq->BindPossessableObject(NewGuid, *Actor, GetBindingWorld());

        Out.Guid = NewGuid;
        Out.MarkOk();
        return Out;
    }

    /**
     * Component possessable, nested under its owning actor's binding.
     *
     * Delegates the whole nested path to the engine (see the file header), then VERIFIES rather than
     * assumes, in this order - existence before threshold, so an absent parent can never be reported
     * as a weak one (rpc-design.md Sec.7):
     *   1. the possessable exists on the movie scene under the returned GUID;
     *   2. GetParent(), read back off that possessable, is valid AND names a possessable that is
     *      really in this movie scene;
     *   3. the GUID resolves, through the runtime resolution path, to the component that was named.
     *
     * Any failure after step 1 rolls back every possessable this call added - including a parent
     * actor binding FindOrAddBinding may have minted on the way - so a failed call never leaves a
     * placeholder binding behind for a later verb to key tracks onto.
     */
    inline FBindingOutcome BindComponent(ULevelSequence* LevelSeq, UActorComponent* Component)
    {
        FBindingOutcome Out;
        if (!LevelSeq || !Component)
        {
            Out.Fail(ErrorCodes::ERR_BINDING_CREATION_FAILED, TEXT("No sequence or no component to bind"));
            return Out;
        }
        UMovieScene* MovieScene = LevelSeq->GetMovieScene();
        if (!MovieScene)
        {
            Out.Fail(ErrorCodes::ERR_BINDING_CREATION_FAILED, TEXT("Sequence has no MovieScene"));
            return Out;
        }
        if (!Component->GetWorld())
        {
            // FindOrAddBinding hard-returns an invalid GUID for an object with no world
            // (LevelSequence.cpp:868-873); say so rather than letting it read as a generic failure.
            Out.Fail(ErrorCodes::ERR_BINDING_CREATION_FAILED,
                FString::Printf(TEXT("Component '%s' is not in a world, so it cannot be possessed"),
                    *Component->GetName()));
            return Out;
        }

        // Snapshot the possessable set so a later failure can be rolled back exactly.
        TSet<FGuid> PreExisting;
        PreExisting.Reserve(MovieScene->GetPossessableCount());
        for (int32 Index = 0; Index < MovieScene->GetPossessableCount(); ++Index)
        {
            PreExisting.Add(MovieScene->GetPossessable(Index).GetGuid());
        }

        auto RollBack = [MovieScene, LevelSeq, &PreExisting]()
        {
            TArray<FGuid> Added;
            for (int32 Index = 0; Index < MovieScene->GetPossessableCount(); ++Index)
            {
                const FGuid& Guid = MovieScene->GetPossessable(Index).GetGuid();
                if (!PreExisting.Contains(Guid))
                {
                    Added.Add(Guid);
                }
            }
            for (const FGuid& Guid : Added)
            {
                LevelSeq->UnbindPossessableObjects(Guid);
                MovieScene->RemovePossessable(Guid);
            }
        };

        LevelSeq->Modify();
        MovieScene->Modify();

        UMovieSceneSequence* SequenceBase = LevelSeq;
        const FGuid NewGuid = SequenceBase->CreatePossessable(Component);
        if (!NewGuid.IsValid())
        {
            RollBack();
            Out.Fail(ErrorCodes::ERR_BINDING_CREATION_FAILED,
                FString::Printf(TEXT("ULevelSequence::FindOrAddBinding refused to possess component '%s'"),
                    *Component->GetName()));
            return Out;
        }

        FMovieScenePossessable* Child = MovieScene->FindPossessable(NewGuid);
        if (!Child)
        {
            RollBack();
            Out.Fail(ErrorCodes::ERR_BINDING_CREATION_FAILED,
                FString::Printf(TEXT("Binding %s was returned for component '%s' but is not present on the movie scene"),
                    *NewGuid.ToString(), *Component->GetName()));
            return Out;
        }

        const FGuid ReadBackParent = Child->GetParent();
        FMovieScenePossessable* Parent = ReadBackParent.IsValid()
            ? MovieScene->FindPossessable(ReadBackParent) : nullptr;
        if (!Parent)
        {
            const FString Detail = ReadBackParent.IsValid()
                ? FString::Printf(TEXT("names parent %s, which is not a possessable on this movie scene"),
                    *ReadBackParent.ToString())
                : TEXT("has no parent binding");
            RollBack();
            Out.Fail(ErrorCodes::ERR_BINDING_PARENT_NOT_SET,
                FString::Printf(TEXT("Component binding for '%s' %s. A component possessable without a valid "
                                     "parent resolves to nothing at playback, so the binding was removed rather "
                                     "than reported as created."),
                    *Component->GetName(), *Detail));
            return Out;
        }

        Out.Guid = NewGuid;
        Out.ParentGuid = ReadBackParent;
        Out.ParentName = Parent->GetName();

        const TArray<UObject*> Resolved = ResolveBoundObjects(LevelSeq, NewGuid);
        Out.bResolutionMeasured = true;
        Out.bResolvesToTarget = Resolved.Contains(static_cast<UObject*>(Component));
        for (UObject* Object : Resolved)
        {
            if (Object)
            {
                Out.ResolvedObjectPath = Object->GetPathName();
                break;
            }
        }

        if (!Out.bResolvesToTarget)
        {
            const FString Detail = Out.ResolvedObjectPath.IsEmpty()
                ? TEXT("resolved to nothing")
                : FString::Printf(TEXT("resolved to '%s'"), *Out.ResolvedObjectPath);
            RollBack();
            Out.Fail(ErrorCodes::ERR_BINDING_UNRESOLVED,
                FString::Printf(TEXT("Component binding for '%s' %s instead of the named component; "
                                     "the binding was removed rather than reported as created."),
                    *Component->GetName(), *Detail));
            return Out;
        }

        Out.MarkOk();
        return Out;
    }

    /**
     * Publish a component binding onto a JSON row. Every field here comes from FBindingOutcome, which
     * is filled by reading the sequence back - nothing is echoed from the request. `resolvesToTarget`
     * is emitted ONLY when a measurement was taken, so an absent measurement cannot render as a
     * negative one.
     */
    inline void FillComponentBindingFields(const TSharedPtr<FJsonObject>& Row,
        const FString& ResolvedComponentName, const FBindingOutcome& Outcome)
    {
        if (!Row.IsValid())
        {
            return;
        }
        Row->SetStringField(TEXT("componentName"), ResolvedComponentName);
        Row->SetStringField(TEXT("bindingKind"), TEXT("component"));
        Row->SetStringField(TEXT("bindingGuid"), Outcome.Guid.ToString());
        Row->SetStringField(TEXT("parentBindingGuid"), Outcome.ParentGuid.ToString());
        Row->SetStringField(TEXT("parentBindingName"), Outcome.ParentName);
        if (Outcome.bResolutionMeasured)
        {
            Row->SetBoolField(TEXT("resolvesToTarget"), Outcome.bResolvesToTarget);
            Row->SetStringField(TEXT("resolvedObjectPath"), Outcome.ResolvedObjectPath);
        }
    }

    /** COMPONENT_NOT_FOUND payload: the name asked for plus every name the actor really has. */
    inline void FillComponentNotFoundFields(const TSharedPtr<FJsonObject>& Row,
        const FString& RequestedName, const AActor* Actor)
    {
        if (!Row.IsValid())
        {
            return;
        }
        Row->SetStringField(TEXT("componentName"), RequestedName);
        TArray<TSharedPtr<FJsonValue>> Available;
        for (const FString& Name : ListComponentNames(Actor))
        {
            Available.Add(MakeShared<FJsonValueString>(Name));
        }
        Row->SetArrayField(TEXT("availableComponents"), Available);
    }

    /** Human-readable half of the same payload, for SendError's message string. */
    inline FString DescribeComponentNotFound(const FString& RequestedName, const AActor* Actor)
    {
        const TArray<FString> Names = ListComponentNames(Actor);
        const FString ActorLabel = Actor ? Actor->GetActorLabel() : TEXT("<null>");
        if (Names.Num() == 0)
        {
            return FString::Printf(
                TEXT("Actor '%s' has no component named '%s'; it has no components at all."),
                *ActorLabel, *RequestedName);
        }
        return FString::Printf(
            TEXT("Actor '%s' has no component named '%s'. Its components are: %s. Component names are "
                 "matched exactly (case-insensitively) against UActorComponent::GetName()."),
            *ActorLabel, *RequestedName, *FString::Join(Names, TEXT(", ")));
    }
}
