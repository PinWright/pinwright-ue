// Copyright (c) 2026 Alexander Penkin. MIT License.

// LevelDescribeOfflineHandler.cpp - level.describe_offline: a map's actors read off the .umap
// file, never by loading it.
//
// The contract is the point of the verb: it NEVER loads the world, never creates the map's
// UPackage, and never touches the active editor world, so an agent may call it while another
// stream holds the world. That is a structural property of this file, not a promise: the only
// engine calls here are a file reader, the package-header serializers (summary, name / import /
// export tables), FindObjectFast by FName (a hash lookup; no path string is parsed, so nothing can
// load) for in-memory class defaults, and an asset-registry path query. There is no LoadObject /
// LoadPackage / CreatePackage / string-path FindObject anywhere below.
//
// What the bytes can and cannot say:
//   * Per actor it reads the tagged properties the editor saved (ActorLabel, FolderPath, Tags,
//     ActorGuid, RootComponent) and walks the root component's attachment chain through the
//     package's own exports to compose the world transform the loaded actor would report.
//   * A tagged property is only written when it differs from the archetype. For a native actor
//     class the archetype (the class default object's component) is in memory and supplies the
//     missing values exactly. For a Blueprint class it is a template that would have to be
//     loaded, so a missing value falls back to the component class default and the actor says
//     so (transformExact:false + transformCaveats) instead of presenting a guess as a reading.
//   * properties / components blocks of pinwright.actor-describe.v1 need a live object and are
//     not emitted.

#include "Handlers/HandlerRegistration.h"
#include "Handlers/HandlerContext.h"
#include "Handlers/ParamSpec.h"
#include "Handlers/ErrorCodes.h"
#include "Utils/AssetDumpBuilder.h"
#include "Utils/JsonBuilders.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/EngineVersionComparison.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/ArchiveProxy.h"
#include "UObject/ObjectResource.h"
#include "UObject/ObjectVersion.h"
#include "UObject/PackageFileSummary.h"
#include "UObject/ReleaseObjectVersion.h"
#include "UObject/UObjectGlobals.h"

namespace PwLevelDescribeOffline
{
    // UE5 package file versions, spelled as numbers because the enumerators after
    // LARGE_WORLD_COORDINATES do not all exist on 5.3. They are file-format facts from an
    // append-only enum, so they never change; the static_asserts pin them where the names exist.
    constexpr int32 Ue5PropertyTagExtension = 1011;   // PROPERTY_TAG_EXTENSION_AND_OVERRIDABLE_SERIALIZATION
    constexpr int32 Ue5PropertyTagCompleteType = 1012; // PROPERTY_TAG_COMPLETE_TYPE_NAME
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 4, 0)
    static_assert(Ue5PropertyTagExtension
        == static_cast<int32>(EUnrealEngineObjectUE5Version::PROPERTY_TAG_EXTENSION_AND_OVERRIDABLE_SERIALIZATION));
    static_assert(Ue5PropertyTagCompleteType
        == static_cast<int32>(EUnrealEngineObjectUE5Version::PROPERTY_TAG_COMPLETE_TYPE_NAME));
#endif

    // Resolves name-table indices the way FLinkerLoad does; everything else goes to the file.
    class FNameTableArchive : public FArchiveProxy
    {
    public:
        FNameTableArchive(FArchive& Inner, const TArray<FName>& InNames)
            : FArchiveProxy(Inner), Names(InNames) {}

        virtual FArchive& operator<<(FName& Value) override
        {
            int32 Index = 0;
            int32 Number = 0;
            InnerArchive << Index << Number;
            // A negative number stringifies as "_-N", which no FName parse strips again, so such
            // a name cannot round-trip through its string. No real save writes one, so it is
            // refused as corrupt before it reaches the FName table (classes resolve by FName
            // through FindObjectFast, so no string path is built from it).
            if (!Names.IsValidIndex(Index) || Number < 0)
            {
                SetError();
                Value = NAME_None;
                return *this;
            }
            Value = FName(Names[Index], Number);
            return *this;
        }

        bool Failed() { return IsError() || InnerArchive.IsError(); }

    private:
        const TArray<FName>& Names;
    };

    // One name-table entry. FNameEntrySerialized logs an Error on an over-long length and leaves
    // its buffer unfilled on a failed read, and FName(Entry) then Strlen's that buffer into
    // FName's max-length checkf; so the length and the terminator are checked here first.
    bool ReadNameEntry(FArchive& Ar, FName& Out)
    {
        const int64 Start = Ar.Tell();
        int32 Length = 0;
        Ar << Length;
        if (Ar.IsError() || Length == 0 || Length < -NAME_SIZE || Length > NAME_SIZE)
        {
            return false;
        }
        Ar.Seek(Start);
        FNameEntrySerialized Entry(ENAME_LinkerConstructor);
        Ar << Entry;
        const int32 Last = FMath::Abs(Length) - 1;
        if (Ar.IsError() || (Entry.bIsWide ? Entry.WideName[Last] != 0 : Entry.AnsiName[Last] != 0))
        {
            return false;
        }
        Out = FName(Entry);
        return true;
    }

    struct FTag
    {
        FName Name;
        FName Type;
        FName Param; // StructName for StructProperty, InnerType for ArrayProperty
        int32 Size = 0;
        bool bBool = false;
        int64 ValueOffset = 0;
    };

    struct FPackageView
    {
        FString PackageName;
        TArray<FObjectImport> Imports;
        TArray<FObjectExport> Exports;
        int32 Ue5Version = 0;
        int32 Ue4Version = 0;
        int32 ReleaseObjectVersion = 0;
        int64 FileSize = 0;
    };

    void SkipTagExtensions(FArchive& Ar)
    {
        uint8 Extensions = 0;
        Ar << Extensions;
        if (Extensions & 0x02) // OverridableInformation
        {
            uint8 Operation = 0;
            bool bExperimentalOverridableLogic = false;
            Ar << Operation << bExperimentalOverridableLogic;
        }
        if (Extensions & 0x04) // HasExternalsObjects
        {
            bool bExternal = false;
            Ar << bExternal;
        }
    }

    // Walks one export's tagged-property stream (mirrors UE 5.8 PropertyTag.cpp for both the
    // complete-type-name format and the older one). Leaves the archive just past the None
    // terminator. False on any malformed read: a parse that lands outside the export is an
    // error, never a partial answer.
    bool ReadTags(FNameTableArchive& Ar, const FPackageView& View, const FObjectExport& Export, TArray<FTag>& Out)
    {
        static const FName NAME_StructPropertyName(TEXT("StructProperty"));
        static const FName NAME_BoolPropertyName(TEXT("BoolProperty"));
        static const FName NAME_BytePropertyName(TEXT("ByteProperty"));
        static const FName NAME_EnumPropertyName(TEXT("EnumProperty"));
        static const FName NAME_ArrayPropertyName(TEXT("ArrayProperty"));
        static const FName NAME_OptionalPropertyName(TEXT("OptionalProperty"));
        static const FName NAME_SetPropertyName(TEXT("SetProperty"));
        static const FName NAME_MapPropertyName(TEXT("MapProperty"));

        // Compared without adding the two file values first: their sum can overflow int64.
        if (Export.SerialOffset <= 0 || Export.SerialSize < 0 || Export.SerialOffset > View.FileSize
            || Export.SerialSize > View.FileSize - Export.SerialOffset)
        {
            return false;
        }
        const int64 End = Export.SerialOffset + Export.SerialSize;
        Ar.Seek(Export.SerialOffset);

        if (View.Ue5Version >= Ue5PropertyTagExtension)
        {
            uint8 Control = 0;
            Ar << Control;
            if (Control & 0x02) // OverridableSerializationInformation
            {
                uint8 Operation = 0;
                Ar << Operation;
            }
        }

        for (int32 Guard = 0; Guard < 100000; ++Guard)
        {
            FTag Tag;
            Ar << Tag.Name;
            if (Ar.Failed() || Ar.Tell() > End)
            {
                return false;
            }
            if (Tag.Name.IsNone())
            {
                return true;
            }

            if (View.Ue5Version >= Ue5PropertyTagCompleteType)
            {
                // FPropertyTypeName: a flattened tree of (name, innerCount) nodes.
                int32 Remaining = 1;
                int32 NodeIndex = 0;
                do
                {
                    FName Node;
                    int32 InnerCount = 0;
                    Ar << Node << InnerCount;
                    if (Ar.Failed() || InnerCount < 0 || InnerCount > 64 || NodeIndex > 64)
                    {
                        return false;
                    }
                    if (NodeIndex == 0) { Tag.Type = Node; }
                    if (NodeIndex == 1) { Tag.Param = Node; }
                    ++NodeIndex;
                    Remaining += InnerCount - 1;
                }
                while (Remaining > 0);

                uint8 Flags = 0;
                Ar << Tag.Size << Flags;
                if (Flags & 0x01) { int32 ArrayIndex = 0; Ar << ArrayIndex; }
                if (Flags & 0x02) { FGuid PropertyGuid; Ar << PropertyGuid; }
                if (Flags & 0x04) { SkipTagExtensions(Ar); }
                Tag.bBool = (Flags & 0x10) != 0;
            }
            else
            {
                int32 ArrayIndex = 0;
                Ar << Tag.Type << Tag.Size << ArrayIndex;
                if (Tag.Type == NAME_StructPropertyName)
                {
                    Ar << Tag.Param;
                    if (View.Ue4Version >= VER_UE4_STRUCT_GUID_IN_PROPERTY_TAG)
                    {
                        FGuid StructGuid;
                        Ar << StructGuid;
                    }
                }
                else if (Tag.Type == NAME_BoolPropertyName)
                {
                    uint8 BoolVal = 0;
                    Ar << BoolVal;
                    Tag.bBool = BoolVal != 0;
                }
                else if (Tag.Type == NAME_BytePropertyName || Tag.Type == NAME_EnumPropertyName)
                {
                    FName EnumName;
                    Ar << EnumName;
                }
                else if (Tag.Type == NAME_ArrayPropertyName)
                {
                    if (View.Ue4Version >= VAR_UE4_ARRAY_PROPERTY_INNER_TAGS)
                    {
                        Ar << Tag.Param;
                    }
                }
                else if (Tag.Type == NAME_OptionalPropertyName)
                {
                    Ar << Tag.Param;
                }
                else if (View.Ue4Version >= VER_UE4_PROPERTY_TAG_SET_MAP_SUPPORT)
                {
                    if (Tag.Type == NAME_SetPropertyName)
                    {
                        Ar << Tag.Param;
                    }
                    else if (Tag.Type == NAME_MapPropertyName)
                    {
                        FName ValueType;
                        Ar << Tag.Param << ValueType;
                    }
                }
                if (View.Ue4Version >= VER_UE4_PROPERTY_GUID_IN_PROPERTY_TAG)
                {
                    uint8 HasPropertyGuid = 0;
                    Ar << HasPropertyGuid;
                    if (HasPropertyGuid)
                    {
                        FGuid PropertyGuid;
                        Ar << PropertyGuid;
                    }
                }
                if (View.Ue5Version >= Ue5PropertyTagExtension)
                {
                    SkipTagExtensions(Ar);
                }
            }

            Tag.ValueOffset = Ar.Tell();
            if (Ar.Failed() || Tag.Size < 0 || Tag.ValueOffset + Tag.Size > End)
            {
                return false;
            }
            Out.Add(Tag);
            Ar.Seek(Tag.ValueOffset + Tag.Size);
        }
        return false;
    }

    const FTag* FindTag(const TArray<FTag>& Tags, const TCHAR* Name)
    {
        const FName Key(Name);
        return Tags.FindByPredicate([&Key](const FTag& Tag) { return Tag.Name == Key; });
    }

    // Vector / Rotator are immutable structs saved as native binary: three doubles since large
    // world coordinates, three floats before. The tag size says which; anything else is refused.
    bool ReadTriple(FNameTableArchive& Ar, const FTag& Tag, double Out[3])
    {
        Ar.Seek(Tag.ValueOffset);
        if (Tag.Size == 3 * sizeof(double))
        {
            Ar << Out[0] << Out[1] << Out[2];
        }
        else if (Tag.Size == 3 * sizeof(float))
        {
            float F[3] = {0.f, 0.f, 0.f};
            Ar << F[0] << F[1] << F[2];
            Out[0] = F[0]; Out[1] = F[1]; Out[2] = F[2];
        }
        else
        {
            return false;
        }
        // A NaN / Inf would be written out as 0 by the JSON builder: refuse it instead.
        return !Ar.Failed() && FMath::IsFinite(Out[0]) && FMath::IsFinite(Out[1]) && FMath::IsFinite(Out[2]);
    }

    // Out is 0 when the tag is absent. False when it is present but not a package index of this
    // file's import / export tables: a malformed reference is an error, not "no reference".
    bool ReadObjectIndex(FNameTableArchive& Ar, const FPackageView& View, const FTag* Tag, int32& Out)
    {
        Out = 0;
        if (!Tag)
        {
            return true;
        }
        if (Tag->Size != sizeof(int32))
        {
            return false;
        }
        Ar.Seek(Tag->ValueOffset);
        Ar << Out;
        return !Ar.Failed() && Out <= View.Exports.Num() && Out >= -View.Imports.Num();
    }

    // FString's own length check logs an Error and its allocation trusts the file, so bound the
    // length by the tag's own size before handing the read to it.
    bool ReadStringBounded(FNameTableArchive& Ar, const FTag& Tag, FString& Out)
    {
        Ar.Seek(Tag.ValueOffset);
        int32 Length = 0;
        Ar << Length;
        const int64 Bytes = Length < 0 ? -int64(Length) * 2 : int64(Length);
        if (Ar.Failed() || Bytes > int64(Tag.Size) - int64(sizeof(int32)))
        {
            return false;
        }
        Ar.Seek(Tag.ValueOffset);
        Ar << Out;
        return !Ar.Failed();
    }

    FString ResolveIndexPath(const FPackageView& View, int32 Index)
    {
        // Same delimiter rule as UObjectBaseUtility::GetPathName: ':' after a top-level object.
        TArray<FName> Chain;
        FString Package = View.PackageName;
        for (int32 Guard = 0; Index != 0 && Guard < 64; ++Guard)
        {
            if (Index > 0 && View.Exports.IsValidIndex(Index - 1))
            {
                const FObjectExport& Export = View.Exports[Index - 1];
                Chain.Insert(Export.ObjectName, 0);
                Index = Export.OuterIndex.ForDebugging();
            }
            else if (Index < 0 && View.Imports.IsValidIndex(-Index - 1))
            {
                const FObjectImport& Import = View.Imports[-Index - 1];
                if (Import.OuterIndex.IsNull())
                {
                    Package = Import.ObjectName.ToString(); // the import's package
                    break;
                }
                Chain.Insert(Import.ObjectName, 0);
                Index = Import.OuterIndex.ForDebugging();
            }
            else
            {
                return FString();
            }
        }
        FString Path = Package;
        for (int32 Position = 0; Position < Chain.Num(); ++Position)
        {
            Path += (Position == 1) ? TEXT(":") : TEXT(".");
            Path += Chain[Position].ToString();
        }
        return Path;
    }

    // The in-memory class a class reference names, or null. A class reference resolvable without a
    // load is an import Package.Class; both are looked up by FName (top-level package, then the
    // class inside it), never by a path string. An export (a class saved in this map) is null.
    const UClass* FindLoadedClass(const FPackageView& View, int32 ClassIndex)
    {
        if (ClassIndex >= 0 || !View.Imports.IsValidIndex(-ClassIndex - 1))
        {
            return nullptr;
        }
        const FObjectImport& ClassImport = View.Imports[-ClassIndex - 1];
        const int32 OuterIndex = ClassImport.OuterIndex.ForDebugging();
        if (OuterIndex >= 0 || !View.Imports.IsValidIndex(-OuterIndex - 1)
            || !View.Imports[-OuterIndex - 1].OuterIndex.IsNull())
        {
            return nullptr;
        }
        UPackage* Package = FindObjectFast<UPackage>(nullptr, View.Imports[-OuterIndex - 1].ObjectName);
        return Package ? FindObjectFast<UClass>(Package, ClassImport.ObjectName) : nullptr;
    }

    // The archetype values a missing tag stands for. Exact only when the actor's class is native
    // (its default object and component templates are always in memory).
    const USceneComponent* FindBaseline(const UClass* ActorClass, const UClass* ComponentClass,
        FName ComponentName, bool& bOutExact)
    {
        bOutExact = false;
        const bool bNativeActor = ActorClass && ActorClass->HasAnyClassFlags(CLASS_Native);
        if (bNativeActor)
        {
            if (const UObject* ActorDefaults = ActorClass->GetDefaultObject(false))
            {
                if (const USceneComponent* Template = FindObjectFast<USceneComponent>(
                        const_cast<UObject*>(ActorDefaults), ComponentName))
                {
                    bOutExact = true;
                    return Template;
                }
            }
        }
        if (const USceneComponent* ClassDefaults = ComponentClass
                ? Cast<USceneComponent>(ComponentClass->GetDefaultObject(false)) : nullptr)
        {
            // On a native actor a component that is not one of its default subobjects was added
            // per instance, and its archetype IS the component class default.
            bOutExact = bNativeActor && ComponentClass->HasAnyClassFlags(CLASS_Native);
            return ClassDefaults;
        }
        return GetDefault<USceneComponent>();
    }

    struct FComponentReader
    {
        const FPackageView& View;
        FNameTableArchive& Ar;
        TArray<FString>& Caveats;

        bool ComponentToWorld(int32 ComponentIndex, int32 Depth, FTransform& Out)
        {
            const FObjectExport& Export = View.Exports[ComponentIndex - 1];
            TArray<FTag> Tags;
            if (Depth > 32 || !ReadTags(Ar, View, Export, Tags))
            {
                return false;
            }

            const int32 OwnerIndex = Export.OuterIndex.ForDebugging();
            const UClass* OwnerClass = (OwnerIndex > 0 && View.Exports.IsValidIndex(OwnerIndex - 1))
                ? FindLoadedClass(View, View.Exports[OwnerIndex - 1].ClassIndex.ForDebugging())
                : nullptr;
            bool bExact = false;
            const USceneComponent* Baseline = FindBaseline(OwnerClass,
                FindLoadedClass(View, Export.ClassIndex.ForDebugging()), Export.ObjectName, bExact);

            FVector Location = Baseline->GetRelativeLocation();
            FRotator Rotation = Baseline->GetRelativeRotation();
            FVector Scale = Baseline->GetRelativeScale3D();
            bool bAbsolute[3] = {Baseline->IsUsingAbsoluteLocation(), Baseline->IsUsingAbsoluteRotation(),
                Baseline->IsUsingAbsoluteScale()};
            bool bAnyDefaulted = false;

            double Triple[3];
            if (const FTag* Tag = FindTag(Tags, TEXT("RelativeLocation")))
            {
                if (!ReadTriple(Ar, *Tag, Triple)) { return false; }
                Location = FVector(Triple[0], Triple[1], Triple[2]);
            }
            else { bAnyDefaulted = true; }
            if (const FTag* Tag = FindTag(Tags, TEXT("RelativeRotation")))
            {
                if (!ReadTriple(Ar, *Tag, Triple)) { return false; }
                Rotation = FRotator(Triple[0], Triple[1], Triple[2]); // saved Pitch, Yaw, Roll
            }
            else { bAnyDefaulted = true; }
            if (const FTag* Tag = FindTag(Tags, TEXT("RelativeScale3D")))
            {
                if (!ReadTriple(Ar, *Tag, Triple)) { return false; }
                Scale = FVector(Triple[0], Triple[1], Triple[2]);
            }
            else { bAnyDefaulted = true; }
            const TCHAR* AbsoluteNames[3] = {TEXT("bAbsoluteLocation"), TEXT("bAbsoluteRotation"), TEXT("bAbsoluteScale")};
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                if (const FTag* Tag = FindTag(Tags, AbsoluteNames[Axis])) { bAbsolute[Axis] = Tag->bBool; }
                else { bAnyDefaulted = true; }
            }
            if (bAnyDefaulted && !bExact)
            {
                Caveats.Add(FString::Printf(TEXT("%s: unsaved transform fields were taken from the component class "
                    "default; the Blueprint template that really supplies them is not readable without loading it"),
                    *Export.ObjectName.ToString()));
            }

            const FTransform Relative(Rotation, Location, Scale);
            int32 ParentIndex = 0;
            if (!ReadObjectIndex(Ar, View, FindTag(Tags, TEXT("AttachParent")), ParentIndex))
            {
                return false;
            }
            if (ParentIndex == 0)
            {
                Out = Relative;
                return true;
            }
            if (ParentIndex < 0)
            {
                Caveats.Add(FString::Printf(TEXT("%s is attached to an object outside this package; the transform "
                    "is relative to it"), *Export.ObjectName.ToString()));
                Out = Relative;
                return true;
            }
            if (const FTag* Socket = FindTag(Tags, TEXT("AttachSocketName")))
            {
                FName SocketName;
                Ar.Seek(Socket->ValueOffset);
                Ar << SocketName;
                if (!SocketName.IsNone())
                {
                    Caveats.Add(FString::Printf(TEXT("%s is attached to socket '%s'; the socket offset is not "
                        "applied"), *Export.ObjectName.ToString(), *SocketName.ToString()));
                }
            }

            FTransform Parent;
            if (!ComponentToWorld(ParentIndex, Depth + 1, Parent))
            {
                return false;
            }
            // USceneComponent's general case: compose, then let absolute axes keep the relative value.
            Out = Relative * Parent;
            if (bAbsolute[0]) { Out.SetTranslation(Relative.GetTranslation()); }
            if (bAbsolute[1]) { Out.SetRotation(Relative.GetRotation()); }
            if (bAbsolute[2]) { Out.SetScale3D(Relative.GetScale3D()); }
            return true;
        }
    };
}

REGISTER_RPC_HANDLER("level.describe_offline", "level",
    "List a map's actors by reading its .umap file from disk - label, name, class, path, folder, tags, "
    "guid and world transform per actor, in the pinwright.actor-describe.v1 shape asset.dump writes "
    "(storage:\"embedded\"; no properties/components blocks, which need a live object). GUARANTEE: "
    "it never loads the map, never creates its package in memory and never touches the active editor "
    "world, so it is safe to call while another caller holds the world. Actors that a One File Per "
    "Actor / World Partition map stores in external packages are listed under externalActors "
    "(package, name, class) from the asset registry, not resolved. transformExact:false plus "
    "transformCaveats marks a transform that depends on data the file does not hold (a Blueprint "
    "template value, a socket offset, a parent outside the package); tagsExact:false marks unsaved tags of an "
    "actor whose class is a Blueprint or is not loaded in this editor, whose default tags are not readable. label is the saved ActorLabel: "
    "an actor saved without one reads empty here where a loaded read generates one from the class.",
    RPC_PARAMS(
        RPC_PARAM_REQ("levelPath", "path", "Map package path, e.g. /Game/Maps/MyMap (an object path /Game/Maps/MyMap.MyMap is accepted). The map need not be loaded and will not be.")
    ))
{
    using namespace PwLevelDescribeOffline;

    FString RequestedPath;
    if (!Ctx.RequireAssetPath(TEXT("levelPath"), RequestedPath))
    {
        return true;
    }
    const FString PackageName = FPackageName::ObjectPathToPackageName(RequestedPath);

    FString Filename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, FPackageName::GetMapPackageExtension()))
    {
        Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(
            TEXT("'%s' is not under a mounted content root."), *PackageName));
        return true;
    }
    Filename = FPaths::ConvertRelativePathToFull(Filename);
    if (!IFileManager::Get().FileExists(*Filename))
    {
        const FString AssetFile = FPaths::ChangeExtension(Filename, FPackageName::GetAssetPackageExtension());
        if (IFileManager::Get().FileExists(*AssetFile))
        {
            Ctx.SendError(ErrorCodes::ERR_NOT_A_MAP, FString::Printf(
                TEXT("'%s' is a .uasset, not a map (.umap)."), *PackageName));
        }
        else
        {
            Ctx.SendError(ErrorCodes::ERR_ASSET_NOT_FOUND, FString::Printf(
                TEXT("No map file for '%s' (looked for %s)."), *PackageName, *Filename));
        }
        return true;
    }

    TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Filename, FILEREAD_Silent));
    const auto Unreadable = [&Ctx, &PackageName](const FString& Why)
    {
        Ctx.SendError(ErrorCodes::ERR_PARSE_FAILED, FString::Printf(
            TEXT("Could not read '%s' offline: %s"), *PackageName, *Why));
        return true;
    };
    if (!Reader)
    {
        return Unreadable(TEXT("the file could not be opened."));
    }

    FPackageFileSummary Summary;
    *Reader << Summary;
    if (Reader->IsError() || Summary.Tag != PACKAGE_FILE_TAG)
    {
        return Unreadable(TEXT("the file is not a binary package (text-format or truncated)."));
    }
    if (!Summary.IsFileVersionValid() || Summary.IsFileVersionTooOld() || Summary.IsFileVersionTooNew())
    {
        return Unreadable(TEXT("its package version is unversioned, too old or newer than this editor."));
    }
    if (Summary.GetPackageFlags() & (PKG_FilterEditorOnly | PKG_UnversionedProperties))
    {
        return Unreadable(TEXT("it is a cooked package; editor-only data such as actor labels is stripped."));
    }
    if (!(Summary.GetPackageFlags() & PKG_ContainsMap))
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_A_MAP, FString::Printf(
            TEXT("'%s' does not contain a map."), *PackageName));
        return true;
    }

    Reader->SetUEVer(Summary.GetFileVersionUE());
    Reader->SetLicenseeUEVer(Summary.GetFileVersionLicenseeUE());
    Reader->SetEngineVer(Summary.SavedByEngineVersion);
    Reader->SetCustomVersions(Summary.GetCustomVersionContainer());

    FPackageView View;
    View.PackageName = PackageName;
    View.FileSize = Reader->TotalSize();
    View.Ue4Version = Summary.GetFileVersionUE().FileVersionUE4;
    View.Ue5Version = Summary.GetFileVersionUE().FileVersionUE5;
    if (const FCustomVersion* Release = Summary.GetCustomVersionContainer().GetVersion(FReleaseObjectVersion::GUID))
    {
        View.ReleaseObjectVersion = Release->Version;
    }

    // A corrupt summary must not reach Seek: the file reader checkf's on an out-of-range position.
    const auto TableInFile = [&View](int64 Offset, int32 Count)
    {
        return Count >= 0 && (Count == 0 || (Offset >= 0 && Offset <= View.FileSize));
    };
    if (!TableInFile(Summary.NameOffset, Summary.NameCount) || !TableInFile(Summary.ImportOffset, Summary.ImportCount)
        || !TableInFile(Summary.ExportOffset, Summary.ExportCount))
    {
        return Unreadable(TEXT("its name, import or export table lies outside the file."));
    }

    TArray<FName> Names;
    if (Summary.NameCount > 0)
    {
        Reader->Seek(Summary.NameOffset);
    }
    bool bNamesRead = true;
    for (int32 Index = 0; Index < Summary.NameCount && bNamesRead; ++Index)
    {
        bNamesRead = ReadNameEntry(*Reader, Names.AddDefaulted_GetRef());
    }
    if (!bNamesRead)
    {
        return Unreadable(TEXT("its name table is malformed."));
    }

    FNameTableArchive Ar(*Reader, Names);
    if (Summary.ImportCount > 0)
    {
        Ar.Seek(Summary.ImportOffset);
    }
    for (int32 Index = 0; Index < Summary.ImportCount && !Ar.Failed(); ++Index)
    {
        Ar << View.Imports.AddDefaulted_GetRef();
    }
    if (Summary.ExportCount > 0)
    {
        Ar.Seek(Summary.ExportOffset);
    }
    for (int32 Index = 0; Index < Summary.ExportCount && !Ar.Failed(); ++Index)
    {
        Ar << View.Exports.AddDefaulted_GetRef();
    }
    if (Ar.Failed())
    {
        return Unreadable(TEXT("its name, import or export table is malformed."));
    }

    // The persistent level: an export named PersistentLevel of class /Script/Engine.Level whose
    // outer is a top-level /Script/Engine.World export.
    int32 LevelIndex = 0;
    for (int32 Index = 0; Index < View.Exports.Num(); ++Index)
    {
        const FObjectExport& Export = View.Exports[Index];
        const int32 Outer = Export.OuterIndex.ForDebugging();
        if (Export.ObjectName == NAME_PersistentLevel
            && ResolveIndexPath(View, Export.ClassIndex.ForDebugging()) == TEXT("/Script/Engine.Level")
            && Outer > 0 && View.Exports.IsValidIndex(Outer - 1) && View.Exports[Outer - 1].OuterIndex.IsNull()
            && ResolveIndexPath(View, View.Exports[Outer - 1].ClassIndex.ForDebugging()) == TEXT("/Script/Engine.World"))
        {
            LevelIndex = Index + 1;
            break;
        }
    }
    if (LevelIndex == 0)
    {
        Ctx.SendError(ErrorCodes::ERR_NOT_A_MAP, FString::Printf(
            TEXT("'%s' has no World / PersistentLevel export."), *PackageName));
        return true;
    }

    // ULevel::Serialize: tagged properties, the object-guid presence flag (+guid), then Actors.
    TArray<FTag> LevelTags;
    if (!ReadTags(Ar, View, View.Exports[LevelIndex - 1], LevelTags))
    {
        return Unreadable(TEXT("the PersistentLevel's properties could not be parsed."));
    }
    const FTag* ExternalTag = FindTag(LevelTags, TEXT("bUseExternalActors"));
    const bool bUsesExternalActors = ExternalTag && ExternalTag->bBool;
    const FTag* FoldersTag = FindTag(LevelTags, TEXT("bUseActorFolders"));
    const bool bUsesActorFolders = FoldersTag && FoldersTag->bBool;
    bool bHasObjectGuid = false;
    Ar << bHasObjectGuid;
    if (bHasObjectGuid)
    {
        FGuid ObjectGuid;
        Ar << ObjectGuid;
    }
    if (View.ReleaseObjectVersion < FReleaseObjectVersion::LevelTransArrayConvertedToTArray)
    {
        int32 Owner = 0;
        Ar << Owner;
    }
    int32 ActorCount = 0;
    Ar << ActorCount;
    if (Ar.Failed() || ActorCount < 0 || ActorCount > View.Exports.Num())
    {
        return Unreadable(TEXT("the PersistentLevel's actor list could not be parsed."));
    }
    TArray<int32> ActorIndices;
    for (int32 Slot = 0; Slot < ActorCount; ++Slot)
    {
        int32 ActorIndex = 0;
        Ar << ActorIndex;
        if (ActorIndex == 0)
        {
            continue; // the level keeps null slots
        }
        // Self-check of the parse: every entry must be an export of THIS level.
        if (ActorIndex < 0 || !View.Exports.IsValidIndex(ActorIndex - 1)
            || View.Exports[ActorIndex - 1].OuterIndex.ForDebugging() != LevelIndex)
        {
            return Unreadable(TEXT("the PersistentLevel's actor list does not point at its own actors."));
        }
        ActorIndices.Add(ActorIndex);
    }
    if (Ar.Failed())
    {
        // A read past the end leaves every remaining slot 0 - "null" - which must not pass for a
        // short actor list.
        return Unreadable(TEXT("the PersistentLevel's actor list runs past the end of the file."));
    }

    using JsonBuilders::BuildNameArrayJson;
    using JsonBuilders::BuildTransformJson;
    TArray<TSharedPtr<FJsonObject>> Actors;
    for (const int32 ActorIndex : ActorIndices)
    {
        const FObjectExport& Export = View.Exports[ActorIndex - 1];
        TArray<FTag> Tags;
        if (!ReadTags(Ar, View, Export, Tags))
        {
            return Unreadable(FString::Printf(TEXT("the properties of actor '%s' could not be parsed."),
                *Export.ObjectName.ToString()));
        }

        FString Label;
        if (const FTag* Tag = FindTag(Tags, TEXT("ActorLabel")); Tag && !ReadStringBounded(Ar, *Tag, Label))
        {
            return Unreadable(FString::Printf(TEXT("the label of actor '%s' is malformed."),
                *Export.ObjectName.ToString()));
        }
        // Fixed-size values: a name is (index, number), a guid 16 bytes, a TArray<FName> a count plus
        // one name each. Any other size is a malformed file, never a short or zero reading.
        const auto MalformedValue = [&Unreadable, &Export](const TCHAR* Property)
        {
            return Unreadable(FString::Printf(TEXT("the %s of actor '%s' does not match its saved size."),
                Property, *Export.ObjectName.ToString()));
        };
        FName Folder;
        if (const FTag* Tag = FindTag(Tags, TEXT("FolderPath")))
        {
            if (Tag->Size != 2 * sizeof(int32))
            {
                return MalformedValue(TEXT("FolderPath"));
            }
            Ar.Seek(Tag->ValueOffset);
            Ar << Folder;
        }
        FGuid Guid;
        if (const FTag* Tag = FindTag(Tags, TEXT("ActorGuid")))
        {
            if (Tag->Size != sizeof(FGuid))
            {
                return MalformedValue(TEXT("ActorGuid"));
            }
            Ar.Seek(Tag->ValueOffset);
            Ar << Guid;
        }
        const FString ClassPath = ResolveIndexPath(View, Export.ClassIndex.ForDebugging());
        TArray<FName> ActorTags;
        bool bTagsExact = true;
        if (const FTag* Tag = FindTag(Tags, TEXT("Tags")))
        {
            Ar.Seek(Tag->ValueOffset);
            int32 Num = 0;
            Ar << Num;
            if (Ar.Failed() || Num < 0 || 4 + int64(Num) * 8 != Tag->Size)
            {
                return MalformedValue(TEXT("Tags"));
            }
            for (int32 Item = 0; Item < Num; ++Item)
            {
                Ar << ActorTags.AddDefaulted_GetRef();
            }
        }
        else
        {
            // Tags is saved only when it differs from the archetype. A native class's default
            // object, when that class is loaded here, supplies it exactly; a Blueprint's default,
            // or a class not loaded in this editor, is not readable without a load.
            const UClass* ActorClass = FindLoadedClass(View, Export.ClassIndex.ForDebugging());
            const AActor* Defaults = ActorClass && ActorClass->HasAnyClassFlags(CLASS_Native)
                ? Cast<AActor>(ActorClass->GetDefaultObject(false)) : nullptr;
            bTagsExact = Defaults != nullptr;
            if (Defaults)
            {
                ActorTags = Defaults->Tags;
            }
        }
        int32 RootIndex = 0;
        if (!ReadObjectIndex(Ar, View, FindTag(Tags, TEXT("RootComponent")), RootIndex))
        {
            return Unreadable(FString::Printf(TEXT("the RootComponent reference of actor '%s' is malformed."),
                *Export.ObjectName.ToString()));
        }
        if (Ar.Failed())
        {
            return Unreadable(FString::Printf(TEXT("a property value of actor '%s' could not be read."),
                *Export.ObjectName.ToString()));
        }

        TArray<FString> Caveats;
        FTransform Transform = FTransform::Identity;
        if (RootIndex > 0 && View.Exports.IsValidIndex(RootIndex - 1))
        {
            FComponentReader Components{View, Ar, Caveats};
            if (!Components.ComponentToWorld(RootIndex, 0, Transform))
            {
                return Unreadable(FString::Printf(TEXT("the root component transform of actor '%s' could not be "
                    "parsed."), *Export.ObjectName.ToString()));
            }
        }
        else if (RootIndex < 0)
        {
            Caveats.Add(TEXT("RootComponent is an object outside this package; transform not read"));
        }

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("schema"), TEXT("pinwright.actor-describe.v1"));
        Obj->SetStringField(TEXT("storage"), TEXT("embedded"));
        Obj->SetStringField(TEXT("name"), Export.ObjectName.ToString());
        Obj->SetStringField(TEXT("label"), Label);
        Obj->SetStringField(TEXT("path"), ResolveIndexPath(View, ActorIndex));
        Obj->SetStringField(TEXT("class"), ClassPath);
        Obj->SetStringField(TEXT("level"), PackageName);
        Obj->SetStringField(TEXT("folder"), Folder.ToString());
        Obj->SetStringField(TEXT("guid"), Guid.ToString());
        Obj->SetArrayField(TEXT("tags"), BuildNameArrayJson(ActorTags));
        Obj->SetBoolField(TEXT("tagsExact"), bTagsExact);
        Obj->SetObjectField(TEXT("transform"), BuildTransformJson(Transform));
        Obj->SetBoolField(TEXT("transformExact"), Caveats.IsEmpty());
        if (!Caveats.IsEmpty())
        {
            Obj->SetArrayField(TEXT("transformCaveats"), JsonBuilders::BuildStringArrayJson(Caveats, false));
        }
        Actors.Add(Obj);
    }
    Actors.Sort([](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
    {
        return A->GetStringField(TEXT("path")).Compare(B->GetStringField(TEXT("path")), ESearchCase::CaseSensitive) < 0;
    });

    // External actors are separate packages; list what the registry knows about them (a read of
    // registry data gathered from the files, no load).
    TArray<TSharedPtr<FJsonValue>> ExternalValues;
    TArray<FString> Warnings;
    if (bUsesActorFolders)
    {
        Warnings.Add(TEXT("The level stores its folders as actor-folder objects; folder reads None for every actor."));
    }
    // Which bytes this is: the file on disk, which a dirty loaded copy has moved past.
    const TSharedPtr<FJsonObject> Source = AssetDumpBuilder::BuildSourceStampJson(PackageName);
    if (Source->GetBoolField(TEXT("unsavedChanges")))
    {
        Warnings.Add(TEXT("The map is loaded with unsaved changes; this describes the file on disk, not the editor's copy."));
    }
    if (IAssetRegistry* Registry = IAssetRegistry::Get())
    {
        TArray<FAssetData> ExternalAssets;
        Registry->GetAssetsByPath(FName(*ULevel::GetExternalActorsPath(PackageName)), ExternalAssets,
            /*bRecursive=*/true, /*bIncludeOnlyOnDiskAssets=*/true);
        ExternalAssets.Sort([](const FAssetData& A, const FAssetData& B)
        {
            return A.PackageName.LexicalLess(B.PackageName);
        });
        for (const FAssetData& Asset : ExternalAssets)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("storage"), TEXT("external-reference"));
            Entry->SetStringField(TEXT("package"), Asset.PackageName.ToString());
            Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
            Entry->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
            ExternalValues.Add(MakeShared<FJsonValueObject>(Entry));
        }
        if (Registry->IsLoadingAssets())
        {
            Warnings.Add(TEXT("The asset registry is still scanning; externalActors may be incomplete."));
        }
    }
    else
    {
        Warnings.Add(TEXT("No asset registry; externalActors was not read."));
    }

    TArray<TSharedPtr<FJsonValue>> ActorValues;
    for (const TSharedPtr<FJsonObject>& Actor : Actors)
    {
        ActorValues.Add(MakeShared<FJsonValueObject>(Actor));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("levelPath"), PackageName);
    Result->SetStringField(TEXT("packageFile"), Filename);
    Result->SetObjectField(TEXT("source"), Source);
    Result->SetStringField(TEXT("worldPath"), ResolveIndexPath(View, View.Exports[LevelIndex - 1].OuterIndex.ForDebugging()));
    Result->SetBoolField(TEXT("usesExternalActors"), bUsesExternalActors);
    Result->SetNumberField(TEXT("embeddedActorCount"), ActorValues.Num());
    Result->SetNumberField(TEXT("externalActorReferenceCount"), ExternalValues.Num());
    Result->SetArrayField(TEXT("actors"), ActorValues);
    Result->SetArrayField(TEXT("externalActors"), ExternalValues);
    if (!Warnings.IsEmpty())
    {
        Result->SetArrayField(TEXT("warnings"), JsonBuilders::BuildStringArrayJson(Warnings, false));
    }
    Ctx.SendSuccess(Result);
    return true;
}
