// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared param-alias helpers for Editor/*.cpp handler files.
// Header-only. Aliases the editor boolean-toggle slot (realtime / enabled) so sibling
// viewport-toggle RPCs accept the same wire name, and shares the active
// level-editor viewport-client resolution + synchronous-redraw idioms across
// the Editor handlers and their tests.
#pragma once

#include "CoreMinimal.h"
#include "Handlers/ParamSpec.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "IAssetViewport.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "UnrealClient.h"

namespace EditorHandlerUtils
{

// Resolves the active level-editor viewport client. Several Editor handlers
// (editor.set_camera, the bookmark handlers, ForceRedrawActiveViewport),
// system.inspect.get_viewport_info, actor.nudge and their tests need exactly this
// client, so it lives here once instead of being re-cloned per call site. With
// bRequireWorld the returned client is guaranteed to have a resolvable world —
// bookmarks need this because IBookmarkTypeTools reaches
// GetWorldSettings()->GetBookmarks() through the client's world. Null means "no
// level-editor viewport right now"; every caller has to answer that case itself.
//
// NEVER resolve this through GEditor->GetActiveViewport()->GetClient(). That
// returns an FViewportClient*, and while PIE runs inside the level viewport
// SLevelViewport::StartPlayInEditorSession replaces the active viewport with an
// FSceneViewport built over the UGameViewportClient. That class is a SIBLING of
// FEditorViewportClient — they meet only at FCommonViewportClient — so a downcast
// lands on the wrong branch, cannot be null-checked (GetClient() is non-null, which
// makes the guard dead code), and reading camera members through it walks off the
// end of the allocation and kills the editor. A checked cast is not available
// either: FGameplayViewportClient is a non-primary base of UGameViewportClient, so
// Cast<UGameViewportClient> on an FViewportClient* cannot recover the UObject.
//
// FLevelEditorModule::GetFirstActiveViewport() -> IAssetViewport::GetAssetViewportClient()
// is typed by the engine instead (SLevelViewport hands back its own
// FLevelEditorViewportClient), and it keeps returning the EDITOR client through PIE:
// StartPlayInEditorSession only parks the editor's scene viewport in InactiveViewport,
// it never touches LevelViewportClient. So the editor camera stays readable while PIE
// runs rather than the caller losing the fields — or the editor.
inline FEditorViewportClient* ResolveActiveLevelViewportClient(bool bRequireWorld = false)
{
    if (!GEditor)
    {
        return nullptr;
    }
    FLevelEditorModule* LevelEditorModule =
        FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
    if (!LevelEditorModule)
    {
        return nullptr;
    }
    const TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule->GetFirstActiveViewport();
    if (!ActiveViewport.IsValid())
    {
        return nullptr;
    }
    FEditorViewportClient* Client = &ActiveViewport->GetAssetViewportClient();
    if (bRequireWorld && !Client->GetWorld())
    {
        return nullptr;
    }
    return Client;
}

// True when the viewport GEditor->GetActiveViewport() hands back is PIE's game
// viewport rather than a level-editor one. StartPlayInEditorSession stamps the flag
// on the scene viewport it builds over the game client
// (ActiveViewport->SetPlayInEditorViewport(true)), so this is the discriminator that
// survives the swap — the client pointer itself carries no usable type information.
// Callers that REPORT viewport geometry use it to say which viewport they measured;
// callers that need a camera just take ResolveActiveLevelViewportClient(), which
// resolves the editor client either way and is deliberately unaffected by this flag.
inline bool IsActiveViewportPlayInEditor()
{
    const FViewport* ActiveViewport = GEditor ? GEditor->GetActiveViewport() : nullptr;
    return ActiveViewport != nullptr && ActiveViewport->IsPlayInEditorViewport();
}

// Forces the given viewport client to render synchronously into its framebuffer
// so a subsequent observation (screenshot / camera read) sees the just-applied
// pose instead of the stale previous frame. Invalidating alone only marks the
// viewport dirty for the next engine tick; Draw() flushes the frame now. This is
// the FEditorViewportClient* overload of the ForceRedrawActiveViewport() idiom so
// callers that already resolved the client (e.g. jump_to_bookmark) avoid a second
// GetActiveViewport() walk.
inline void ForceRedrawViewportClient(FEditorViewportClient* ViewportClient)
{
    if (!ViewportClient)
    {
        return;
    }
    ViewportClient->Invalidate(true, true);
    if (FViewport* Viewport = ViewportClient->Viewport)
    {
        Viewport->Draw();
    }
}

// Candidate wire names for the editor boolean-toggle slot, canonical first. Used both
// to populate FParamSpec aliases at registration and to read the value body-side via
// FHandlerContext::GetBoolFirstOf.
inline const TArray<FString>& ToggleKeys()
{
    static const TArray<FString> Keys = {
        TEXT("enabled"),
        TEXT("realtime")
    };
    return Keys;
}

inline FParamSpec EditorToggleParamOpt(const TCHAR* Name, const TCHAR* Desc)
{
    FParamSpec Spec{FString(Name), TEXT("boolean"), FString(Desc), false, TEXT("")};
    const FString Canonical(Name);
    for (const FString& Key : ToggleKeys())
    {
        if (!Key.Equals(Canonical))
        {
            Spec.Aliases.Add(Key);
        }
    }
    return Spec;
}

} // namespace EditorHandlerUtils
