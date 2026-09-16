// Copyright (c) 2026 Alexander Penkin. MIT License.

// Class resolution utilities for PinWright
#include "Utils/ClassUtils.h"


#include "Engine/Blueprint.h"
#include "UObject/UObjectIterator.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "PinWrightSubsystem.h"
#include "Utils/PathUtils.h"

#if __has_include("EditorAssetLibrary.h")
#include "EditorAssetLibrary.h"
#else
#include "Editor/EditorAssetLibrary.h"
#endif

UClass* ResolveClassByName(const FString& ClassNameOrPath)
{
    // A "//" anywhere in the input can reach CreatePackage's Fatal through any load below, which
    // ends the editor PROCESS rather than returning an error. Nothing in this function loads on the
    // raw input TODAY - UEditorAssetLibrary::LoadAsset collapses "//" via
    // EditorScriptingHelpers.cpp:71 before it resolves anything, and the LoadObject below is
    // reached only for an input containing no '/' at all - but that safety is an engine-internal
    // routing detail of a third-party helper, not a property of this function. Two lines here make
    // it a property of this function; the shapes this refuses were unresolvable anyway.
    if (ClassNameOrPath.IsEmpty() || CanReachCreatePackageFatal(ClassNameOrPath))
    {
        if (!ClassNameOrPath.IsEmpty())
        {
            // Warning, never Error: bElevateLogWarningsToErrors would turn an Error here into a
            // suite failure, and a refused argument is a caller mistake, not a plugin defect.
            UE_LOG(LogPinWrightSubsystem, Warning,
                   TEXT("ResolveClassByName refused '%s': a class reference may not contain '//' "
                        "(it can reach CreatePackage's Fatal and end the editor process)."),
                   *ClassNameOrPath);
        }
        return nullptr;
    }

    // 1) If it's an asset path, prefer loading the asset and deriving the class
    // Skip /Script/ paths as they are native classes, not assets
    if ((ClassNameOrPath.StartsWith(TEXT("/")) ||
         ClassNameOrPath.Contains(TEXT("/"))) &&
        !ClassNameOrPath.StartsWith(TEXT("/Script/")))
    {
        UObject* Loaded = nullptr;
        Loaded = UEditorAssetLibrary::LoadAsset(ClassNameOrPath);
        if (Loaded)
        {
            if (UBlueprint* BP = Cast<UBlueprint>(Loaded))
                return BP->GeneratedClass;
            if (UClass* C = Cast<UClass>(Loaded))
                return C;
        }
    }

    // 2) Try a direct FindObject using nullptr/explicit outer (expects full path)
    if (UClass* Direct = FindObject<UClass>(nullptr, *ClassNameOrPath))
        return Direct;

    // 2.5) Try guessing generic engine locations for common components
    if (!ClassNameOrPath.Contains(TEXT("/")) &&
        !ClassNameOrPath.Contains(TEXT(".")))
    {
        FString EnginePath =
            FString::Printf(TEXT("/Script/Engine.%s"), *ClassNameOrPath);
        if (UClass* EngineClass = FindObject<UClass>(nullptr, *EnginePath))
            return EngineClass;

        if (UClass* EngineClassLoaded = LoadObject<UClass>(nullptr, *EnginePath))
            return EngineClassLoaded;

        FString UMGPath = FString::Printf(TEXT("/Script/UMG.%s"), *ClassNameOrPath);
        if (UClass* UMGClass = FindObject<UClass>(nullptr, *UMGPath))
            return UMGClass;
    }

    // Special handling for common ambiguous types
    if (ClassNameOrPath.Equals(TEXT("NiagaraComponent"),
                               ESearchCase::IgnoreCase))
    {
        if (UClass* NiagaraComp = FindObject<UClass>(
                nullptr, TEXT("/Script/Niagara.NiagaraComponent")))
        {
            return NiagaraComp;
        }
    }

    // 3) Fallback: iterate loaded classes and match by short name or path suffix
    UClass* BestMatch = nullptr;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* C = *It;
        if (!C)
            continue;

        // Exact short name match
        if (C->GetName().Equals(ClassNameOrPath, ESearchCase::IgnoreCase))
        {
            // Prefer /Script/ (native) classes over others if multiple match
            if (C->GetPathName().StartsWith(TEXT("/Script/")))
                return C;
            if (!BestMatch)
                BestMatch = C;
        }
        // Match on ".ClassName" suffix (path-based short form)
        else if (C->GetPathName().EndsWith(
                     FString::Printf(TEXT(".%s"), *ClassNameOrPath),
                     ESearchCase::IgnoreCase))
        {
            if (!BestMatch)
                BestMatch = C;
        }
    }

    return BestMatch;
}

UClass* ResolveUClass(const FString& Input)
{
    // THE HIGHEST-LEVERAGE GUARD IN THE PLUGIN: ~56 call sites across >=45 verbs funnel their
    // caller-supplied class reference through here. Seven LoadObject calls below (the direct load,
    // the "_C" retry - which PRESERVES a "//" from the input - the UBlueprint retry, and the four
    // short-name/prefix/stripped/registry loads) each reach StaticLoadObjectInternal ->
    // ResolveName2(Create=true) -> CreatePackage, which logs at Fatal for a name containing "//"
    // (UObjectGlobals.cpp:1094-1096). Fatal is not compiled out in any configuration: the editor
    // PROCESS ends and every unsaved package in it is lost, and no `if (!Found)` below is reached.
    //
    // "//" is the ONLY rule this may apply. IsValidLongPackageName is NOT the guard for a class
    // reference: it refuses a leading-slash-less short name and refuses '.', so it would reject
    // "PointLight", "/Script/UMG.UserWidget" and "/Game/BP/BP_X.BP_X_C" - roughly half the shapes
    // this function exists to accept - and break those >=45 verbs.
    //
    // The FindObject at step 1 is safe (ResolveName2 with Create=false, UObjectGlobals.cpp:620),
    // so nothing above this point could have loaded anyway; the guard is still first because a
    // guard placed below a load is not a guard.
    if (Input.IsEmpty() || CanReachCreatePackageFatal(Input))
    {
        if (!Input.IsEmpty())
        {
            // Warning, never Error: bElevateLogWarningsToErrors would turn an Error here into a
            // suite failure. This function returns only nullptr across its 56 call sites and has
            // no error channel, so the log line is the whole diagnosis - name the input and the
            // rule. Adding an out-error parameter to 56 call sites is a separate ticket.
            UE_LOG(LogPinWrightSubsystem, Warning,
                   TEXT("ResolveUClass refused '%s': a class reference may not contain '//' "
                        "(it can reach CreatePackage's Fatal and end the editor process)."),
                   *Input);
        }
        return nullptr;
    }

    // 1. Try finding it directly (full path or already loaded)
    UClass* Found = FindObject<UClass>(nullptr, *Input);
    if (Found)
        return Found;

    // 2. Try loading it directly
    Found = LoadObject<UClass>(nullptr, *Input);
    if (Found)
        return Found;

    // 3. Handle Blueprint Generated Classes explicitly for any content-mount path.
    //    Any path that starts with "/" but is NOT a "/Script/" native class is
    //    potentially a Blueprint asset. This covers /Game/ and any plugin
    //    content mount (e.g. /SomePlugin/, /SomeGameFeature/).
    //    Supports inputs with or without a trailing _C suffix, and falls back to
    //    loading the UBlueprint and returning its GeneratedClass.
    if (Input.StartsWith(TEXT("/")) && !Input.StartsWith(TEXT("/Script/")))
    {
        // Step 2 already tried LoadObject<UClass>(*Input); if that failed for a content-mount
        // path, the asset is either a UBlueprint (cast to UClass fails) or needs the _C suffix.

        if (!Input.EndsWith(TEXT("_C")))
        {
            if (UClass* WithC = LoadObject<UClass>(nullptr, *(Input + TEXT("_C"))))
            {
                return WithC;
            }
        }

        // Package is likely already in memory after step 2's LoadObject attempt, so this
        // degenerates to a FindObject on a UBlueprint that the earlier cast rejected.
        if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Input))
        {
            if (BP->GeneratedClass)
            {
                return BP->GeneratedClass;
            }
        }

        // Qualified path that didn't resolve — don't fall through to short-name iteration.
        return nullptr;
    }

    // 4. Short name resolution
    const TArray<FString> ScriptPackages = {TEXT("/Script/Engine"),
                                            TEXT("/Script/CoreUObject"),
                                            TEXT("/Script/UMG"),
                                            TEXT("/Script/AIModule"),
                                            TEXT("/Script/NavigationSystem"),
                                            TEXT("/Script/Niagara")};

    for (const FString& Pkg : ScriptPackages)
    {
        FString TryPath = FString::Printf(TEXT("%s.%s"), *Pkg, *Input);
        Found = FindObject<UClass>(nullptr, *TryPath);
        if (Found)
            return Found;
        Found = LoadObject<UClass>(nullptr, *TryPath);
        if (Found)
            return Found;
    }

    // 5. Native class search by iteration (slow fallback)
    for (TObjectIterator<UClass> It; It; ++It)
    {
        if (It->GetName() == Input)
        {
            return *It;
        }
    }

    // 5.5. Friendly short names often omit the UObject/AActor prefix
    // (e.g. "GameplayStatics", "Pawn"). Retry with the common prefixes before
    // falling into the prefix-stripping path below.
    if (Input.Len() > 0 && Input[0] != TEXT('U') && Input[0] != TEXT('A'))
    {
        const TArray<FString> PrefixedNames = {
            FString::Printf(TEXT("U%s"), *Input),
            FString::Printf(TEXT("A%s"), *Input)
        };

        for (const FString& PrefixedName : PrefixedNames)
        {
            for (const FString& Pkg : ScriptPackages)
            {
                FString TryPath = FString::Printf(TEXT("%s.%s"), *Pkg, *PrefixedName);
                Found = FindObject<UClass>(nullptr, *TryPath);
                if (Found)
                    return Found;
                Found = LoadObject<UClass>(nullptr, *TryPath);
                if (Found)
                    return Found;
            }

            for (TObjectIterator<UClass> It; It; ++It)
            {
                if (It->GetName() == PrefixedName)
                {
                    return *It;
                }
            }
        }
    }

    // 6. If not found and starts with U/A prefix, retry without it
    if (Input.Len() > 1 && (Input[0] == TEXT('U') || Input[0] == TEXT('A')))
    {
        FString Stripped = Input.Mid(1);
        for (const FString& Pkg : ScriptPackages)
        {
            FString TryPath = FString::Printf(TEXT("%s.%s"), *Pkg, *Stripped);
            Found = FindObject<UClass>(nullptr, *TryPath);
            if (Found)
                return Found;
            Found = LoadObject<UClass>(nullptr, *TryPath);
            if (Found)
                return Found;
        }
        for (TObjectIterator<UClass> It; It; ++It)
        {
            if (It->GetName() == Stripped)
            {
                return *It;
            }
        }
    }

    // 7. Short name with _C suffix: strip _C, search asset registry for a
    //    Blueprint asset with that base name, then load its generated class.
    //    This handles bare inputs like "W_Error_C" for BP assets on any content
    //    mount (/Game/, plugin mounts) that are not yet loaded in memory.
    if (Input.EndsWith(TEXT("_C")))
    {
        const FString BaseName = Input.LeftChop(2); // strip "_C"
        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

        FARFilter Filter;
        Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = false;
        Filter.bRecursivePaths = true;

        TArray<FAssetData> Assets;
        AssetRegistry.GetAssets(Filter, Assets);

        for (const FAssetData& AssetData : Assets)
        {
            if (AssetData.AssetName.ToString() == BaseName)
            {
                // Build the generated-class object path: PackageName.AssetName_C
                FString ClassPath = FString::Printf(TEXT("%s.%s_C"),
                    *AssetData.PackageName.ToString(), *BaseName);
                if (UClass* BPClass = LoadObject<UClass>(nullptr, *ClassPath))
                    return BPClass;

                // Fallback: load the UBlueprint and return GeneratedClass
                if (UBlueprint* BP = Cast<UBlueprint>(AssetData.GetAsset()))
                {
                    if (BP->GeneratedClass)
                        return BP->GeneratedClass;
                }
            }
        }
    }

    return nullptr;
}

UEnum* ResolveUEnum(const FString& EnumName)
{
    // Same shape as ResolveUClass, verified rather than assumed: tier 1's FindObject is safe
    // (Create=false) and its LoadObject on the RAW input is not. The `Contains(".")` gate above
    // that load does not help - "/Script/Foo//Bar.EBaz" satisfies it and still carries the "//"
    // into CreatePackage's Fatal. Tiers 2-3 only walk already-loaded objects and never load.
    // 12 call sites, nullptr-only return, so the log line is the whole diagnosis.
    if (EnumName.IsEmpty() || CanReachCreatePackageFatal(EnumName))
    {
        if (!EnumName.IsEmpty())
        {
            // Warning, never Error - see ResolveUClass above.
            UE_LOG(LogPinWrightSubsystem, Warning,
                   TEXT("ResolveUEnum refused '%s': an enum reference may not contain '//' "
                        "(it can reach CreatePackage's Fatal and end the editor process)."),
                   *EnumName);
        }
        return nullptr;
    }

    // Tier 1: full-path form (contains '.') — try direct find then load.
    if (EnumName.Contains(TEXT(".")))
    {
        if (UEnum* Found = FindObject<UEnum>(nullptr, *EnumName))
            return Found;
        if (UEnum* Found = LoadObject<UEnum>(nullptr, *EnumName))
            return Found;
    }

    // Tier 2: short-name fast lookup.
    if (UEnum* Found = FindFirstObjectSafe<UEnum>(*EnumName, EFindFirstObjectOptions::None))
        return Found;

    // Tier 3: iterate all loaded UEnum objects — covers project-module enums
    // (e.g. EReplaySaveState from /Script/App) whose UObject short name or
    // CppType matches the requested name.
    for (TObjectIterator<UEnum> It; It; ++It)
    {
        if (It->GetName() == EnumName || It->CppType == EnumName)
            return *It;
    }

    return nullptr;
}

UScriptStruct* ResolveUScriptStruct(const FString& StructName)
{
    // Same shape as ResolveUEnum, verified rather than assumed: tier 1's LoadObject on the RAW
    // input is the only lethal call; tiers 2-4 walk already-loaded objects. 9 call sites,
    // nullptr-only return.
    if (StructName.IsEmpty() || CanReachCreatePackageFatal(StructName))
    {
        if (!StructName.IsEmpty())
        {
            // Warning, never Error - see ResolveUClass above.
            UE_LOG(LogPinWrightSubsystem, Warning,
                   TEXT("ResolveUScriptStruct refused '%s': a struct reference may not contain "
                        "'//' (it can reach CreatePackage's Fatal and end the editor process)."),
                   *StructName);
        }
        return nullptr;
    }

    // Tier 1: full-path form (contains '.') — try direct find then load.
    if (StructName.Contains(TEXT(".")))
    {
        if (UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *StructName))
            return Found;
        if (UScriptStruct* Found = LoadObject<UScriptStruct>(nullptr, *StructName))
            return Found;
    }

    // Tier 2: short-name fast lookup.
    if (UScriptStruct* Found = FindFirstObjectSafe<UScriptStruct>(*StructName, EFindFirstObjectOptions::None))
        return Found;

    // Tier 3: strip leading 'F' prefix (C++ convention) and retry.
    // e.g. "FEditorReplay" → retry as "EditorReplay".
    if (StructName.Len() > 1 && StructName[0] == TEXT('F'))
    {
        const FString Stripped = StructName.Mid(1);
        if (UScriptStruct* Found = FindFirstObjectSafe<UScriptStruct>(*Stripped, EFindFirstObjectOptions::None))
            return Found;

        // Also try iterating with the stripped name.
        for (TObjectIterator<UScriptStruct> It; It; ++It)
        {
            if (It->GetName().Equals(Stripped, ESearchCase::IgnoreCase)
                || It->GetName().Equals(StructName, ESearchCase::IgnoreCase))
                return *It;
        }
    }

    // Tier 4: iterate all loaded UScriptStruct objects — covers project-module structs
    // (e.g. FEditorReplay from /Script/App) whose UObject short name or CppType matches.
    for (TObjectIterator<UScriptStruct> It; It; ++It)
    {
        if (It->GetName().Equals(StructName, ESearchCase::IgnoreCase))
            return *It;
    }

    return nullptr;
}
