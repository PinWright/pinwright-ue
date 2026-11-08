// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Utils/PieSaveBlockGuard.h"
#include "Utils/PieState.h"

#include "Handlers/Editor/PieWorldSelector.h"

#include "Engine/World.h"

namespace PinWrightPieSaveBlock
{

bool ProbePieSaveBlock(FPieSaveBlock& OutBlock)
{
    OutBlock = FPieSaveBlock();

    // The engine's precondition, copied rather than approximated: this is exactly the test
    // EditorScriptingHelpers::CheckIfInEditorAndPIE() makes before UEditorAssetLibrary refuses
    // the write. Approximating it (PlayWorld alone, or a world-context scan) would let the two
    // disagree, and a guard that disagrees with the thing it guards is worse than none - it
    // would report a cause for a save that actually failed for some other reason.
    if (!PinWrightPieState::IsPlayInEditorActive())
    {
        return false;
    }
    OutBlock.bActive = true;

    // Only now walk the world contexts. The predicate above runs on every save; this does not.
    const TArray<PieWorldSelector::FPieContextInfo> Contexts = PieWorldSelector::GatherPieContexts();
    for (const PieWorldSelector::FPieContextInfo& Context : Contexts)
    {
        UWorld* World = Context.World;
        if (!World)
        {
            continue;
        }
        FPieWorldIdentity Identity;
        Identity.PieInstance = Context.PieInstance;
        Identity.MapName = UWorld::RemovePIEPrefix(World->GetMapName());
        Identity.WorldPath = World->GetPathName();
        OutBlock.Worlds.Add(MoveTemp(Identity));
    }

    return true;
}

void AddPieSaveBlockJson(const TSharedPtr<FJsonObject>& Result, const FPieSaveBlock& Block)
{
    if (!Result.IsValid() || !Block.bActive)
    {
        return;
    }

    Result->SetBoolField(TEXT("pieActive"), true);
    Result->SetStringField(TEXT("editorMode"), TEXT("PIE"));

    // Emitted even when empty, so `pieActive` is never accompanied by a silently missing key.
    TArray<TSharedPtr<FJsonValue>> WorldsArray;
    WorldsArray.Reserve(Block.Worlds.Num());
    for (const FPieWorldIdentity& World : Block.Worlds)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("pieInstance"), World.PieInstance);
        Entry->SetStringField(TEXT("mapName"), World.MapName);
        Entry->SetStringField(TEXT("worldPath"), World.WorldPath);
        WorldsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Result->SetArrayField(TEXT("pieWorlds"), WorldsArray);
}

void AddPieSaveBlockJsonIfBlocked(const TSharedPtr<FJsonObject>& Result)
{
    if (!Result.IsValid())
    {
        return;
    }
    FPieSaveBlock Block;
    if (ProbePieSaveBlock(Block))
    {
        AddPieSaveBlockJson(Result, Block);
    }
}

FString DescribePieSaveBlock(const FPieSaveBlock& Block)
{
    if (!Block.bActive)
    {
        return FString();
    }

    if (Block.Worlds.Num() == 0)
    {
        // bActive with no resolvable world context. Say so rather than printing an empty list:
        // the block is real either way, and the caller still needs to know a session is up.
        return TEXT("the editor is in play mode (no PIE world context resolved)");
    }

    TArray<FString> Names;
    Names.Reserve(Block.Worlds.Num());
    for (const FPieWorldIdentity& World : Block.Worlds)
    {
        Names.Add(FString::Printf(TEXT("%s (%s)"), *World.MapName, *World.WorldPath));
    }
    return FString::Printf(TEXT("the editor is in play mode; PIE world(s): %s"),
        *FString::Join(Names, TEXT(", ")));
}

} // namespace PinWrightPieSaveBlock
