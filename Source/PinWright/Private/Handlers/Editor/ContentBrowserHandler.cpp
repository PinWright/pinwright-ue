// Copyright (c) 2026 Alexander Penkin. MIT License.

// ContentBrowserHandler.cpp - Content Browser query handlers
// Handles editor.get_content_browser_selection, editor.get_content_browser_path

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Dom/JsonObject.h"

#include "ContentBrowserModule.h"
#include "ContentBrowserItemPath.h"
#include "IContentBrowserSingleton.h"

// ---- editor.get_content_browser_selection ----
REGISTER_RPC_HANDLER("editor.get_content_browser_selection", "editor", "Return the assets currently selected in the active Content Browser as a list of {name, path, class} entries. Useful when authoring against whatever the user has highlighted in the editor.",
    RPC_NO_PARAMS)
{
  FContentBrowserModule& CBM = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
  IContentBrowserSingleton& CB = CBM.Get();

  TArray<FAssetData> SelectedAssets;
  CB.GetSelectedAssets(SelectedAssets);

  TArray<TSharedPtr<FJsonValue>> AssetsArray;
  for (const FAssetData& AssetData : SelectedAssets)
  {
    TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
    AssetObj->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
    AssetObj->SetStringField(TEXT("path"), AssetData.GetObjectPathString());
    AssetObj->SetStringField(TEXT("class"), AssetData.AssetClassPath.ToString());
    AssetsArray.Add(MakeShared<FJsonValueObject>(AssetObj));
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetArrayField(TEXT("assets"), AssetsArray);
  Resp->SetNumberField(TEXT("count"), SelectedAssets.Num());
  Ctx.SendSuccess(Resp);
  return true;
}

// ---- editor.get_content_browser_path ----
REGISTER_RPC_HANDLER("editor.get_content_browser_path", "editor", "Return the folder path currently focused in the Content Browser (e.g. /Game/Foo). Falls back to the first selected path-view folder, then to /Game/ if neither is set.",
    RPC_NO_PARAMS)
{
  FContentBrowserModule& CBM = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
  IContentBrowserSingleton& CB = CBM.Get();

  FString ResultPath;
  const FContentBrowserItemPath CurrentPath = CB.GetCurrentPath();
  if (CurrentPath.HasInternalPath())
  {
    ResultPath = CurrentPath.GetInternalPathString();
  }

  if (ResultPath.IsEmpty())
  {
    TArray<FString> Paths;
    CB.GetSelectedPathViewFolders(Paths);
    if (Paths.Num() > 0)
    {
      ResultPath = Paths[0];
    }
  }

  if (ResultPath.IsEmpty())
  {
    ResultPath = TEXT("/Game/");
  }

  TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
  Resp->SetStringField(TEXT("path"), ResultPath);
  Ctx.SendSuccess(Resp);
  return true;
}
