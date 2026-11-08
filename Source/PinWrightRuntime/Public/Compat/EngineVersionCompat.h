// Copyright (c) 2026 Alexander Penkin. MIT License.

// EngineVersionCompat.h
// Pulls in the engine's version-comparison macros and back-fills the ones that are
// missing on older engines so the plugin can use a single comparison vocabulary on
// UE 5.4-5.7. Include this instead of "Misc/EngineVersionComparison.h" wherever the
// UE_VERSION_* macros are used.
//
// Through UE 5.5, Misc/EngineVersionComparison.h defines only UE_VERSION_NEWER_THAN and
// UE_VERSION_OLDER_THAN; UE_VERSION_NEWER_THAN_OR_EQUAL was added in 5.6. We define it
// here on engines that lack it, reusing the engine's own UE_GREATER_SORT / ENGINE_*_VERSION
// machinery so the result matches the 5.5+ definition exactly (tiebreaker = true).

#pragma once

#include "Misc/EngineVersionComparison.h"

#ifndef UE_VERSION_NEWER_THAN_OR_EQUAL
#define UE_VERSION_NEWER_THAN_OR_EQUAL(MajorVersion, MinorVersion, PatchVersion) \
    UE_GREATER_SORT(ENGINE_MAJOR_VERSION, MajorVersion, UE_GREATER_SORT(ENGINE_MINOR_VERSION, MinorVersion, UE_GREATER_SORT(ENGINE_PATCH_VERSION, PatchVersion, true)))
#endif

// EAllowShrinking arrived in UE 5.4 (Containers/AllowShrinking.h); before that, the matching
// container/string APIs (RemoveAt, Pop, SetNum, LeftChopInline, RemoveSwap, ...) took a plain
// `bool bAllowShrinking`. Back-fill a plain (implicitly-bool-convertible) enum so call sites can
// keep writing `EAllowShrinking::No` / `EAllowShrinking::Yes` on every supported engine. On 5.4+
// the engine defines the real `enum class EAllowShrinking`, so this is only compiled on 5.3.
// Values match the engine's: No=0 (don't shrink, == bAllowShrinking=false), Yes=1.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
enum EAllowShrinking : uint8 { No = 0, Yes = 1 };
#endif

// UE 5.8 removed the ResetLoaders call from UObject::Rename and deprecated
// REN_ForceNoResetLoaders; REN_AllowPackageLinkerMismatch is the 5.8 spelling for
// "intentionally leave the package linker referencing the old names". Pre-5.8 keeps
// the old flag so Rename still skips the ResetLoaders it would otherwise run.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
#define MCP_REN_NO_RESET_LOADERS REN_AllowPackageLinkerMismatch
#else
#define MCP_REN_NO_RESET_LOADERS REN_ForceNoResetLoaders
#endif

// UE 5.8 deprecated the FCoreDelegates::OnPostEngineInit static member in favor of the
// GetOnPostEngineInit() accessor (which does not exist on older engines). Expands to the
// delegate reference on every supported engine.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
#define MCP_ON_POST_ENGINE_INIT FCoreDelegates::GetOnPostEngineInit()
#else
#define MCP_ON_POST_ENGINE_INIT FCoreDelegates::OnPostEngineInit
#endif

// ForEachObjectWithPackage's `bool bIncludeNestedObjects` parameter was deprecated in
// UE 5.8 in favor of the EGetObjectsFlags enum (which does not exist on older engines).
// This constant spells "exclude nested objects" on every supported engine.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
#define MCP_FOREACH_EXCLUDE_NESTED_OBJECTS EGetObjectsFlags::None
#else
#define MCP_FOREACH_EXCLUDE_NESTED_OBJECTS false
#endif

// UE 5.5 moved the "editor is loading a package" flag behind UE::GetIsEditorLoadingPackage()
// and deprecated the GIsEditorLoadingPackage global, which on 5.4 is still a plain bool.
// Reads the same flag on every supported engine.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
#define MCP_IS_EDITOR_LOADING_PACKAGE UE::GetIsEditorLoadingPackage()
#else
#define MCP_IS_EDITOR_LOADING_PACKAGE GIsEditorLoadingPackage
#endif

// FAutomationTestBase::GetTestFlags returns a plain uint32 through UE 5.4; 5.5 turned
// EAutomationTestFlags from a struct of uint32 constants into an enum class and changed the
// return type with it. A hand-rolled FAutomationTestBase subclass must spell the override's
// return type per engine.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
#define MCP_AUTOMATION_TEST_FLAGS EAutomationTestFlags
#else
#define MCP_AUTOMATION_TEST_FLAGS uint32
#endif

// Through UE 5.7, UDataLayerInstance shadows UObject::GetFName() with a private override and the
// instance name is read through the public GetDataLayerFName(). UE 5.8 dropped the private
// shadow and deprecated GetDataLayerFName in favour of GetFName.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
#define MCP_DATA_LAYER_INSTANCE_FNAME(Instance) ((Instance)->GetFName())
#else
#define MCP_DATA_LAYER_INSTANCE_FNAME(Instance) ((Instance)->GetDataLayerFName())
#endif

// UE 5.8 reduced FSoundWaveProxy to a handle on the wave's published FSoundWaveData: USoundWave
// gained GetSoundWaveProxy() (CreateSoundWaveProxy() is deprecated there) and the proxy's
// GetSoundWaveData() was renamed GetSoundWaveDataRef(). Both spellings reach the same
// FSoundWaveData; the expansion is a shared pointer pre-5.8 and a shared reference from 5.8, so
// bind it with `auto`.
// Pre-5.8 the wave's cached Proxy is a TSharedPtr that starts null and stays null whenever
// FApp::CanEverRenderAudio() is false (-nosound), which CreateSoundWaveProxy() returns as-is, so
// going through the wave's accessor dereferences null under an audio-less automation run. From
// 5.8 that member is a TSharedRef and cannot be null. Construct the proxy directly instead: it
// takes a handle on the wave's own FSoundWaveData, the object UpdatePlatformData() publishes to.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
#define MCP_SOUND_WAVE_DATA(Wave) ((Wave)->GetSoundWaveProxy()->GetSoundWaveDataRef())
#else
#define MCP_SOUND_WAVE_DATA(Wave) (FSoundWaveProxy(Wave).GetSoundWaveData())
#endif

// FSoundWaveData::GetGUID() arrived in UE 5.4, reading a WaveGuid the engine copies straight from
// USoundWave::CompressedDataGuid (SoundWave.cpp: `WaveGuid = InWave.CompressedDataGuid`). On 5.3
// FSoundWaveData carries no such member and CompressedDataGuid on the wave is the only spelling.
// Reads the same DDC key - the value a compression refresh re-rolls - on every supported engine.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
#define MCP_SOUND_WAVE_GUID(Wave) (MCP_SOUND_WAVE_DATA(Wave)->GetGUID())
#else
#define MCP_SOUND_WAVE_GUID(Wave) ((Wave)->CompressedDataGuid)
#endif

// FViewport::AsSceneViewport() arrived in UE 5.8. Before that, the game viewport's FSceneViewport
// is reached through UGameViewportClient::GetGameViewport(); it is the same object the client's
// Viewport points at whenever that viewport is a scene viewport, so the identity test reproduces
// the 5.8 accessor's "null unless this really is an FSceneViewport" contract. Expands to an
// FSceneViewport* and requires Slate/SceneViewport.h at the call site.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
#define MCP_GAME_SCENE_VIEWPORT(Client) \
    ((Client)->Viewport ? (Client)->Viewport->AsSceneViewport() : nullptr)
#else
#define MCP_GAME_SCENE_VIEWPORT(Client) \
    (static_cast<FViewport*>((Client)->GetGameViewport()) == (Client)->Viewport \
        ? (Client)->GetGameViewport() : nullptr)
#endif

// IGameLayerManager::AsWidget() arrived in UE 5.8, where SGameLayerManager implements it as
// SharedThis(this). Before that the interface has no widget accessor, so recover the widget with
// the same side-cast: SGameLayerManager is the engine's only implementation and derives from both
// SCompoundWidget and IGameLayerManager. Expands to a TSharedRef<SWidget>-convertible value and
// requires Slate/SGameLayerManager.h at the call site.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
#define MCP_GAME_LAYER_MANAGER_WIDGET(ManagerPtr) ((ManagerPtr)->AsWidget())
#else
#define MCP_GAME_LAYER_MANAGER_WIDGET(ManagerPtr) \
    (StaticCastSharedPtr<SGameLayerManager>(ManagerPtr).ToSharedRef())
#endif

// USkeleton::AddCurveMetaData gained a `bool bTransact = true` parameter in UE 5.5. On 5.4 the
// call always opens a transaction under WITH_EDITOR and there is no way to ask it not to, so the
// pre-5.5 expansion drops the argument rather than pretending the choice exists.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
#define MCP_ADD_CURVE_META_DATA(Skeleton, CurveName) \
    (Skeleton)->AddCurveMetaData((CurveName), /*bTransact=*/false)
#else
#define MCP_ADD_CURVE_META_DATA(Skeleton, CurveName) (Skeleton)->AddCurveMetaData(CurveName)
#endif

// UE 5.4 split the World Partition actor descriptor into a persistent FWorldPartitionActorDesc and
// a per-instance FWorldPartitionActorDescInstance, moving the instance type into its own header and
// renaming FWorldPartitionHelpers::ForEachActorDesc to ForEachActorDescInstance. Both spellings take
// a TFunctionRef<bool(const T*)>, so a call site written with a generic lambda (`const auto* Desc`)
// compiles unchanged on either engine; only the function name has to be selected here. IsLoaded()
// exists on both descriptor types.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
#define MCP_FOR_EACH_ACTOR_DESC ForEachActorDescInstance
#else
#define MCP_FOR_EACH_ACTOR_DESC ForEachActorDesc
#endif

// AWorldDataLayers::ForEachDataLayerInstance arrived in UE 5.4, which deprecated the identically
// shaped ForEachDataLayer (5.4's version is a one-line forward to it). Both take
// TFunctionRef<bool(UDataLayerInstance*)>, so only the name has to be selected here.
// UDataLayerManager's same-named method is a different class's API and exists on every supported
// engine; this macro is for the AWorldDataLayers actor.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
#define MCP_FOR_EACH_DATA_LAYER_INSTANCE ForEachDataLayerInstance
#else
#define MCP_FOR_EACH_DATA_LAYER_INSTANCE ForEachDataLayer
#endif

// MIN_ORTHOZOOM / MAX_ORTHOZOOM became public engine defines in UE 5.4
// (Runtime/Engine/Public/EngineDefines.h), when FViewportCameraTransform::SetOrthoZoom also gained
// the ensure() that bounds a viewport's ortho zoom to them. On 5.3 MIN_ORTHOZOOM has no public
// definition at all and MAX_ORTHOZOOM exists only inside the private EditorViewportClient.cpp, so
// a plugin TU cannot see either. Back-filled with the 5.4 values so a clamp written against the
// engine's bounds means the same thing on every supported engine; 5.3's own SetOrthoZoom only
// requires a non-zero value, which [1.0, 1e25] satisfies.
#if UE_VERSION_OLDER_THAN(5, 4, 0)
#ifndef MIN_ORTHOZOOM
#define MIN_ORTHOZOOM 1.0
#endif
#ifndef MAX_ORTHOZOOM
#define MAX_ORTHOZOOM 1e25
#endif
#endif

// FOverlapResult::GetItemIndex() arrived in UE 5.7, when the engine stopped gating the stored
// item index on the component's bMultiBodyOverlap at fill time and deferred that decision to the
// accessor. Through 5.6 the public ItemIndex member already carries the gated value
// (CollisionConversions.cpp: `OutOverlap.ItemIndex = OwnerComponent->bMultiBodyOverlap ?
// BodyInst->InstanceBodyIndex : INDEX_NONE`), so reading the member pre-5.7 yields exactly what
// the 5.7+ accessor returns: the per-body index for a multi-body component, INDEX_NONE otherwise.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 7, 0)
#define MCP_OVERLAP_ITEM_INDEX(Overlap) ((Overlap).GetItemIndex())
#else
#define MCP_OVERLAP_ITEM_INDEX(Overlap) ((Overlap).ItemIndex)
#endif

// FBulkDataCookedIndex arrived in UE 5.5, when IPackageResourceManager's segment-taking
// DoesPackageExist / FileSize overloads gained a cooked-index parameter. On 5.4 the same
// overloads take (PackagePath, Segment) with no index, and FBulkDataCookedIndex does not exist
// at all. Expands to the leading argument list on either engine; from 5.5 the call site also
// needs Serialization/BulkDataCookedIndex.h.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
#define MCP_PACKAGE_RESOURCE_ARGS(PackagePath, Segment) \
    (PackagePath), FBulkDataCookedIndex::Default, (Segment)
#else
#define MCP_PACKAGE_RESOURCE_ARGS(PackagePath, Segment) (PackagePath), (Segment)
#endif

// FRigTransformElement::GetDirtyState() and FRigControlElement::GetOffsetDirtyState() arrived in
// UE 5.5, when the per-transform dirty flags moved out of FRigCurrentAndInitialTransform into a
// parallel FRigCurrentAndInitialDirtyState. Through 5.4 the flags live on the transform itself
// and are read with GetDirtyFlag(). Both spellings answer "is this one transform dirty" for a
// single ERigTransformType. Requires Rigs/RigHierarchyElements.h at the call site.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
#define MCP_RIG_TRANSFORM_IS_DIRTY(Element, TransformType) \
    ((Element)->GetDirtyState().IsDirty(TransformType))
#define MCP_RIG_CONTROL_OFFSET_IS_DIRTY(ControlElement, TransformType) \
    ((ControlElement)->GetOffsetDirtyState().IsDirty(TransformType))
#else
#define MCP_RIG_TRANSFORM_IS_DIRTY(Element, TransformType) \
    ((Element)->Pose.GetDirtyFlag(TransformType))
#define MCP_RIG_CONTROL_OFFSET_IS_DIRTY(ControlElement, TransformType) \
    ((ControlElement)->Offset.GetDirtyFlag(TransformType))
#endif

// FProperty::GetElementSize() arrived in UE 5.5, which moved the element size behind an
// accessor. Through 5.4 the only spelling is the public ElementSize member. Reads the same
// value on every supported engine.
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 5, 0)
#define MCP_PROPERTY_ELEMENT_SIZE(Property) ((Property)->GetElementSize())
#else
#define MCP_PROPERTY_ELEMENT_SIZE(Property) ((Property)->ElementSize)
#endif

// FAutomationTestBase::AddExpectedErrorPlain / AddExpectedMessagePlain arrived in UE 5.4,
// together with the `bool IsRegex` parameter on AddExpectedError. On 5.3 the only spelling is
// the regex one: FAutomationExpectedMessage stores the string as an FRegexPattern, so a literal
// containing regex metacharacters would not match itself. The 5.3 expansion therefore escapes the
// literal and forwards to the regex overload, which is exactly what the 5.4+ engine implementation
// does internally (AddExpectedErrorPlain -> AddExpectedMessagePlain -> AddExpectedMessage with
// IsRegex=false). Occurrences semantics are unchanged: >0 = exact, 0 = one-or-more on both.
//
// Compiled only on 5.3, where neither member exists, so this cannot shadow the engine's own
// declarations on 5.4+.
#if UE_VERSION_OLDER_THAN(5, 4, 0)

#include "Containers/UnrealString.h"

namespace PinWrightCompat
{
    /** Escapes every ECMAScript regex metacharacter so the result matches the input literally. */
    inline FString EscapeRegexLiteral(const FString& In)
    {
        FString Out;
        Out.Reserve(In.Len() * 2);
        for (const TCHAR Ch : In)
        {
            if (Ch == TCHAR('\\')
                || FCString::Strchr(TEXT("^$.|?*+()[]{}"), Ch) != nullptr)
            {
                Out.AppendChar(TCHAR('\\'));
            }
            Out.AppendChar(Ch);
        }
        return Out;
    }
}

#define AddExpectedErrorPlain(ExpectedString, ...) \
    AddExpectedError(PinWrightCompat::EscapeRegexLiteral(ExpectedString), __VA_ARGS__)
#define AddExpectedMessagePlain(ExpectedString, ...) \
    AddExpectedMessage(PinWrightCompat::EscapeRegexLiteral(ExpectedString), __VA_ARGS__)

#endif
