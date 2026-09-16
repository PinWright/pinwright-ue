// Copyright (c) 2026 Alexander Penkin. MIT License.

// Unit tests for PathUtils: SanitizeProjectRelativePath, IsValidAssetPath,
// SanitizeAssetName, ValidateAssetCreationPath
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Handlers/PackagePathCompose.h"
#include "PinWrightHelpers.h"

// ============================================================================
// SanitizeProjectRelativePath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSanitizePathNormalizeSlashesTest,
    "PinWright.core.path.sanitize_project_relative_path.NormalizeSlashes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSanitizePathNormalizeSlashesTest::RunTest(const FString& Parameters)
{
    // Backslashes should be converted to forward slashes, double slashes collapsed
    FString Result = SanitizeProjectRelativePath(TEXT("/Game\\Folder//SubFolder"));
    if (!Result.IsEmpty())
    {
        TestFalse(TEXT("No backslashes in result"), Result.Contains(TEXT("\\")));
        TestFalse(TEXT("No double slashes in result"), Result.Contains(TEXT("//")));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSanitizePathBlocksTraversalTest,
    "PinWright.core.path.sanitize_project_relative_path.BlocksTraversal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSanitizePathBlocksTraversalTest::RunTest(const FString& Parameters)
{
    // Directory traversal must be rejected
    FString Result = SanitizeProjectRelativePath(TEXT("/Game/../../../etc/passwd"));
    TestTrue(TEXT("Traversal path returns empty"), Result.IsEmpty());

    FString Result2 = SanitizeProjectRelativePath(TEXT("/Game/Folder/.."));
    TestTrue(TEXT("Trailing traversal returns empty"), Result2.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSanitizePathStripsWindowsAbsoluteTest,
    "PinWright.core.path.sanitize_project_relative_path.RejectsWindowsAbsolute",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSanitizePathStripsWindowsAbsoluteTest::RunTest(const FString& Parameters)
{
    // Windows absolute paths (C:\...) must be rejected
    FString Result = SanitizeProjectRelativePath(TEXT("C:\\Users\\Admin\\Desktop\\asset"));
    TestTrue(TEXT("Windows absolute path returns empty"), Result.IsEmpty());

    FString Result2 = SanitizeProjectRelativePath(TEXT("D:/SomeFolder/file"));
    TestTrue(TEXT("Windows drive path returns empty"), Result2.IsEmpty());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSanitizePathEmptyTest,
    "PinWright.core.path.sanitize_project_relative_path.EmptyInput",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSanitizePathEmptyTest::RunTest(const FString& Parameters)
{
    FString Result = SanitizeProjectRelativePath(TEXT(""));
    TestTrue(TEXT("Empty input returns empty"), Result.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSanitizePathValidGameTest,
    "PinWright.core.path.sanitize_project_relative_path.ValidGamePath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSanitizePathValidGameTest::RunTest(const FString& Parameters)
{
    FString Result = SanitizeProjectRelativePath(TEXT("/Game/MyFolder/MyAsset"));
    TestEqual(TEXT("Valid /Game path passes through"), Result, TEXT("/Game/MyFolder/MyAsset"));
    return true;
}

// ============================================================================
// IsValidAssetPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIsValidAssetPathTest,
    "PinWright.core.path.IsValidAssetPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIsValidAssetPathTest::RunTest(const FString& Parameters)
{
    // Valid paths
    TestTrue(TEXT("/Game/Folder/Asset is valid"),
        IsValidAssetPath(TEXT("/Game/Folder/Asset")));
    TestTrue(TEXT("/Engine/Materials/Default is valid"),
        IsValidAssetPath(TEXT("/Engine/Materials/Default")));

    // Invalid paths
    TestFalse(TEXT("Empty is invalid"), IsValidAssetPath(TEXT("")));
    TestFalse(TEXT("No leading slash is invalid"), IsValidAssetPath(TEXT("Game/Folder")));
    TestFalse(TEXT("Traversal is invalid"), IsValidAssetPath(TEXT("/Game/../etc")));
    TestFalse(TEXT("Double slash is invalid"), IsValidAssetPath(TEXT("/Game//Folder")));
    TestFalse(TEXT("Colon is invalid"), IsValidAssetPath(TEXT("C:/Game/Folder")));

    return true;
}

// ============================================================================
// SanitizeAssetName
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSanitizeAssetNameValidTest,
    "PinWright.core.path.sanitize_asset_name.PreservesValid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSanitizeAssetNameValidTest::RunTest(const FString& Parameters)
{
    // Valid characters are preserved
    TestEqual(TEXT("Normal name preserved"),
        SanitizeAssetName(TEXT("MyBlueprint_01")), TEXT("MyBlueprint_01"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSanitizeAssetNameInvalidTest,
    "PinWright.core.path.sanitize_asset_name.RemovesInvalid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSanitizeAssetNameInvalidTest::RunTest(const FString& Parameters)
{
    // Invalid characters are replaced with underscores and cleaned up
    FString Result = SanitizeAssetName(TEXT("My@Asset#Name"));
    TestFalse(TEXT("No @ in result"), Result.Contains(TEXT("@")));
    TestFalse(TEXT("No # in result"), Result.Contains(TEXT("#")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSanitizeAssetNameEmptyTest,
    "PinWright.core.path.sanitize_asset_name.EmptyDefault",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSanitizeAssetNameEmptyTest::RunTest(const FString& Parameters)
{
    // Empty input returns default "Asset"
    TestEqual(TEXT("Empty returns Asset"), SanitizeAssetName(TEXT("")), TEXT("Asset"));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSanitizeAssetNameSQLInjectionTest,
    "PinWright.core.path.sanitize_asset_name.BlocksSQLInjection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSanitizeAssetNameSQLInjectionTest::RunTest(const FString& Parameters)
{
    // SQL injection patterns are stripped
    FString Result = SanitizeAssetName(TEXT("asset;DROP TABLE--"));
    TestFalse(TEXT("No semicolons"), Result.Contains(TEXT(";")));
    TestFalse(TEXT("No double-dashes"), Result.Contains(TEXT("--")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSanitizeAssetNameSlashAndDotTest,
    "PinWright.core.path.sanitize_asset_name.RemovesSlashAndDot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSanitizeAssetNameSlashAndDotTest::RunTest(const FString& Parameters)
{
    // THE TWO CHARACTERS THE OTHER FOUR CASES NEVER USED, which is exactly why the defect
    // survived: '/' and '.' were absent from InvalidChars, so SanitizeAssetName("Foo/Bar")
    // returned "Foo/Bar" unchanged and the composed package path carried a folder separator
    // inside its leaf. Both are in the engine's INVALID_OBJECTNAME_CHARACTERS
    // (Core/Public/UObject/NameTypes.h:191), so no UObject could ever have been named either.
    const FString Slashed = SanitizeAssetName(TEXT("Foo/Bar"));
    TestFalse(TEXT("a '/' does not survive an asset NAME"), Slashed.Contains(TEXT("/")));
    TestEqual(TEXT("and is replaced by a single underscore"), Slashed, FString(TEXT("Foo_Bar")));

    const FString Dotted = SanitizeAssetName(TEXT("My.Asset"));
    TestFalse(TEXT("a '.' does not survive an asset NAME"), Dotted.Contains(TEXT(".")));
    TestEqual(TEXT("and is replaced by a single underscore"), Dotted, FString(TEXT("My_Asset")));

    // A path handed to the NAME slot is caller confusion, not a name. It must not come back
    // looking like a path, and the leading separators must not survive as leading underscores.
    const FString PathShaped = SanitizeAssetName(TEXT("/Game/Folder/Asset"));
    TestFalse(TEXT("a whole path loses every separator"), PathShaped.Contains(TEXT("/")));
    TestEqual(TEXT("and collapses to one underscore-joined name"), PathShaped,
        FString(TEXT("Game_Folder_Asset")));

    // The object-path form, whose '/' and '.' must BOTH go.
    const FString ObjectShaped = SanitizeAssetName(TEXT("/Game/A/BP_X.BP_X"));
    TestFalse(TEXT("no '/' survives"), ObjectShaped.Contains(TEXT("/")));
    TestFalse(TEXT("no '.' survives"), ObjectShaped.Contains(TEXT(".")));

    // CONTROL. The rest of the character set is unchanged - a valid name still passes through
    // untouched, so this is not "the sanitizer now eats everything".
    TestEqual(TEXT("a valid name is still returned verbatim"),
        SanitizeAssetName(TEXT("MyBlueprint_01")), FString(TEXT("MyBlueprint_01")));
    return true;
}

// ============================================================================
// ValidateAssetCreationPath
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidateAssetCreationPathValidTest,
    "PinWright.core.path.validate_asset_creation_path.ValidPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FValidateAssetCreationPathValidTest::RunTest(const FString& Parameters)
{
    FString OutFullPath, OutError;
    bool bValid = ValidateAssetCreationPath(
        TEXT("/Game/MyFolder"), TEXT("MyAsset"), OutFullPath, OutError);
    TestTrue(TEXT("Valid creation path succeeds"), bValid);
    TestTrue(TEXT("Full path contains /Game"), OutFullPath.Contains(TEXT("/Game")));
    TestTrue(TEXT("Full path contains asset name"), OutFullPath.Contains(TEXT("MyAsset")));
    TestTrue(TEXT("Error message is empty"), OutError.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidateAssetCreationPathTraversalTest,
    "PinWright.core.path.validate_asset_creation_path.RejectsTraversal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FValidateAssetCreationPathTraversalTest::RunTest(const FString& Parameters)
{
    FString OutFullPath, OutError;
    bool bValid = ValidateAssetCreationPath(
        TEXT("/Game/../../etc"), TEXT("passwd"), OutFullPath, OutError);
    TestFalse(TEXT("Traversal path fails validation"), bValid);
    TestFalse(TEXT("Error message is non-empty"), OutError.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FValidateAssetCreationPathInvalidNameTest,
    "PinWright.core.path.validate_asset_creation_path.SanitizesName",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FValidateAssetCreationPathInvalidNameTest::RunTest(const FString& Parameters)
{
    // Even with invalid characters in the name, it should be sanitized
    FString OutFullPath, OutError;
    bool bValid = ValidateAssetCreationPath(
        TEXT("/Game/Folder"), TEXT("My@Bad#Name"), OutFullPath, OutError);
    // Should succeed (name is sanitized, not rejected)
    TestTrue(TEXT("Sanitizable name succeeds"), bValid);
    if (bValid)
    {
        TestFalse(TEXT("Sanitized name has no @"), OutFullPath.Contains(TEXT("@")));
        TestFalse(TEXT("Sanitized name has no #"), OutFullPath.Contains(TEXT("#")));
    }
    return true;
}

// ============================================================================
// NormalizeToObjectPath
//
// The helper behind B-mesh-audit-package-path-reads-as-broken-asset. Its return value is what
// keeps three facts apart that used to be one: a malformed string is a CALLER error, a
// well-formed path with nothing at it is ASSET_NOT_FOUND, and a package path is neither - it is
// simply the shorter spelling of an object path.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNormalizeToObjectPathPackageFormTest,
    "PinWright.core.path.normalize_to_object_path.PackageFormBecomesObjectPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNormalizeToObjectPathPackageFormTest::RunTest(const FString& Parameters)
{
    FString Out;
    FString Error;

    // The exact form the ticket was filed on.
    TestTrue(TEXT("a package path is well formed"),
        NormalizeToObjectPath(TEXT("/Game/Props/Meshes/Trees/SM_X"), Out, Error));
    TestEqual(TEXT("and normalizes to its object path"), Out,
        FString(TEXT("/Game/Props/Meshes/Trees/SM_X.SM_X")));
    TestTrue(TEXT("with no error text"), Error.IsEmpty());

    // An object path is already correct and must survive untouched - normalizing twice cannot
    // produce /Game/A/SM_X.SM_X.SM_X.
    TestTrue(TEXT("an object path is well formed"),
        NormalizeToObjectPath(TEXT("/Game/Props/Meshes/Trees/SM_X.SM_X"), Out, Error));
    TestEqual(TEXT("and is returned unchanged"), Out,
        FString(TEXT("/Game/Props/Meshes/Trees/SM_X.SM_X")));

    FString Twice;
    TestTrue(TEXT("normalization is idempotent"), NormalizeToObjectPath(Out, Twice, Error));
    TestEqual(TEXT("second pass changes nothing"), Twice, Out);

    // A subobject path's ':' suffix is the registry's business, not this helper's.
    TestTrue(TEXT("a subobject path is well formed"),
        NormalizeToObjectPath(TEXT("/Game/A/BP_X.BP_X:Component"), Out, Error));
    TestEqual(TEXT("and is left alone"), Out, FString(TEXT("/Game/A/BP_X.BP_X:Component")));

    // Surrounding whitespace is the caller's typing, not a malformed path.
    TestTrue(TEXT("whitespace is trimmed"), NormalizeToObjectPath(TEXT("  /Engine/A/B  "), Out, Error));
    TestEqual(TEXT("and the path still normalizes"), Out, FString(TEXT("/Engine/A/B.B")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNormalizeToObjectPathRejectsMalformedTest,
    "PinWright.core.path.normalize_to_object_path.MalformedInputIsRejectedWithAReason",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNormalizeToObjectPathRejectsMalformedTest::RunTest(const FString& Parameters)
{
    // Each of these is a caller error, and the point of returning false is that the caller can
    // report it as one instead of filing it as a fact about an asset.
    const TArray<FString> Malformed = {
        FString(),                        // empty
        TEXT("   "),                      // whitespace only
        TEXT("SM_X"),                     // a bare name with no mount point
        TEXT("Game/A/SM_X"),              // no leading slash
        TEXT("/Game/A/"),                 // a folder, not an asset
        TEXT("/Game/A.B/SM_X"),           // a dot in a folder component
        TEXT("/Game/A/SM_X."),            // nothing after the dot
        TEXT("/Game/A/.SM_X"),            // nothing before it
        TEXT("/Game/../Secret/SM_X"),     // traversal
        TEXT("C:/Game/A/SM_X"),           // a Windows absolute path
    };

    for (const FString& Path : Malformed)
    {
        FString Out = TEXT("<untouched>");
        FString Error;
        TestFalse(*FString::Printf(TEXT("'%s' is rejected"), *Path),
            NormalizeToObjectPath(Path, Out, Error));
        TestTrue(*FString::Printf(TEXT("'%s' is rejected WITH a reason"), *Path), !Error.IsEmpty());
        // The out-param must be cleared rather than left holding a stale value, so a caller
        // that ignores the return value cannot resolve last iteration's path.
        TestTrue(*FString::Printf(TEXT("'%s' leaves no object path behind"), *Path), Out.IsEmpty());
    }
    return true;
}

// ============================================================================
// CanReachCreatePackageFatal
//
// The predicate the resolvers consult before a load or a create. TRUE means the string can reach
// CreatePackage's Fatal (UObjectGlobals.cpp:1094-1096), which ends the PROCESS rather than
// failing the call.
//
// THESE ARE PURE-FUNCTION TESTS ON PURPOSE. No verb, no dispatcher, nothing loaded - so nothing
// here can reach the Fatal on either a guarded or an unguarded build. That is the same limitation
// Tests/Sequencer/TestSequencerExportAnimSequencePathSafety.cpp states: a payload that WOULD
// discriminate the two builds ends the process on one of them.
//
// The accept half is the half that matters. A guard that refused too much would break ~45 verbs
// (a short class name, an object path and a _C path are all legitimate resolver input), and no
// test of the reject half would notice.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCanReachCreatePackageFatalAcceptsEveryShapeTest,
    "PinWright.core.path.can_reach_create_package_fatal.AcceptsEveryLegitimateShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCanReachCreatePackageFatalAcceptsEveryShapeTest::RunTest(const FString& Parameters)
{
    // Every shape a resolver in this plugin documents as valid input. One guard sits above
    // resolvers that accept several of these at once, so it must pass all of them without
    // knowing which it was handed.
    const TArray<FString> MustPass = {
        TEXT("PointLight"),                     // a bare short class name, no slash, no dot
        TEXT("StaticMeshActor"),                // ditto
        TEXT("/Game/A/B"),                      // a package path
        TEXT("/Game/A/B.B"),                    // an object path
        TEXT("/Game/A/BP_X.BP_X:Component"),    // a subobject path
        TEXT("/Game/A/BP_X.BP_X_C"),            // a generated-class path
        TEXT("/Script/Engine.StaticMeshActor"), // a script class reference
        TEXT("/Script/UMG.UserWidget"),         // ditto
        TEXT("/Engine/BasicShapes/Cube"),       // a read-only root
        TEXT("/MyPlugin/Content/A/B"),          // a plugin mount
        TEXT("/Game/A/B_C.B_C"),                // underscores are not separators
        FString(),                              // empty: not lethal, just not resolvable
        TEXT("/"),                              // a lone slash is not a doubled one
        TEXT("/Game/"),                         // a trailing separator is not a doubled one
        TEXT("C:/Game/A/B"),                    // malformed, but not a process kill
        TEXT("\\Game\\A\\B"),                   // backslashes cannot reach the Fatal (see header)
        TEXT("/Game/../A/B"),                   // traversal is a different rule's job
    };

    for (const FString& Path : MustPass)
    {
        TestFalse(*FString::Printf(
            TEXT("'%s' must NOT be treated as lethal - refusing it would narrow the resolvers"),
            *Path), CanReachCreatePackageFatal(Path));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCanReachCreatePackageFatalRefusesDoubleSlashTest,
    "PinWright.core.path.can_reach_create_package_fatal.RefusesEveryDoubleSlashShape",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCanReachCreatePackageFatalRefusesDoubleSlashTest::RunTest(const FString& Parameters)
{
    // Each of these ends the editor process today if it reaches a load or a create unguarded.
    // The dot-free entries are the non-obvious ones: ResolveName2 returns at :1241 with no
    // delimiter, then StaticLoadObjectInternal re-enters itself with the short name appended
    // after a '.' (:1474-1482) and the second pass walks into CreatePackage (:1310).
    const TArray<FString> MustRefuse = {
        TEXT("a//b"),                           // no leading slash, no dot - still a kill
        TEXT("A//B"),
        TEXT("//"),                             // the substring alone
        TEXT("/Game//X"),                       // the shape that killed a live editor
        TEXT("//Game/X"),                       // doubled at the root
        TEXT("/Game/A//B/C"),                   // mid-path
        TEXT("/Game/A/B//"),                    // trailing
        TEXT("/Game/A/B.B//C"),                 // after the object name
        TEXT("/Game/A/BP_X.BP_X:Comp//Sub"),    // inside a subobject suffix
        TEXT("/Script/Engine//StaticMeshActor"),
        TEXT("/Game///X"),                      // a run of three still contains "//"
        TEXT("Foo//Bar.Foo//Bar"),
    };

    for (const FString& Path : MustRefuse)
    {
        TestTrue(*FString::Printf(
            TEXT("'%s' can reach the CreatePackage Fatal and must be refused"), *Path),
            CanReachCreatePackageFatal(Path));
    }
    return true;
}

// ============================================================================
// IsValidMountPoint
//
// CHARACTERIZATION FIRST, AND THE ORDER IS THE POINT. Every row below was measured against
// the previous three-`StartsWith` body BEFORE that body was replaced, and each row that the
// replacement moves carries the old verdict inline. A reviewer can revert IsValidMountPoint's
// body in Utils/PathUtils.cpp on its own and read straight off this table which behaviour
// comes back.
//
// The old body answered TRUE for anything starting with the literal text "/Game", "/Engine" or
// "/Script" and only consulted the engine for everything else, so it was a PREFIX test wearing
// a mount point's name: "/GameFoo/Bar", "/Enginexyz/A" and "/Scriptable/Junk" all passed while
// naming roots that are not mounted. Those three rows are the whole behavioural delta.
//
// WHAT DELIBERATELY DID NOT MOVE, and a reader must not "fix" it here: "/Game//X" still answers
// TRUE. FPackageName::GetPackageMountPoint confirms with FPathViews::IsParentPathOf, and that
// helper explicitly STRIPS duplicate separators that follow the parent
// (Core/Private/Misc/PathViews.cpp:468-473, "If there were invalid duplicate slashes after the
// parent path, remove them"), so "/Game//X" is a child of "/Game/" by its rules. The engine's
// own "//" rule lives in IsValidTextForLongPackageName (PackageName.cpp:1702-1706), which
// GetPackageMountPoint does not call - and NOT calling it is exactly why an OBJECT path still
// answers TRUE here. This predicate is a mount-point question, not the "//" guard; the "//"
// guard is CanReachCreatePackageFatal and the dispatch-boundary type gate.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIsValidMountPointVerdictsTest,
    "PinWright.core.path.is_valid_mount_point.MountPointVerdicts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIsValidMountPointVerdictsTest::RunTest(const FString& Parameters)
{
    // --- Accepted, and unchanged by the rewrite -----------------------------------------
    //
    // A BARE ROOT WITH NO CHILD AND NO TRAILING SLASH WAS THE ONE GENUINELY UNKNOWN CASE, and
    // it is measured rather than assumed: mount roots are stored WITH a trailing slash
    // ("/Game/", PackageName.cpp:804-807), TDirectoryTree::FTreeNode::TryFindClosestPath
    // compares with FPathViews::TryMakeChildPathRelativeTo, and that helper chops a redundant
    // terminating separator off the PARENT before comparing and then defines an exactly-equal
    // child as a child (PathViews.cpp:406-437). So the tree finds the node, IsParentPathOf
    // agrees, and GetPackageMountPoint("/Game") returns the FName "Game" - not NAME_None. No
    // MountPointExists fallback is needed.
    TestTrue(TEXT("a bare mounted root resolves"), IsValidMountPoint(TEXT("/Game")));
    TestTrue(TEXT("a bare mounted root with a trailing slash resolves"),
        IsValidMountPoint(TEXT("/Game/")));
    TestTrue(TEXT("/Engine is a mounted root"), IsValidMountPoint(TEXT("/Engine")));
    TestTrue(TEXT("/Script is a mounted root"), IsValidMountPoint(TEXT("/Script")));
    TestTrue(TEXT("/Temp is a mounted (read-only) root"), IsValidMountPoint(TEXT("/Temp/X")));
    TestTrue(TEXT("a package path under a mount resolves"),
        IsValidMountPoint(TEXT("/Script/Engine")));

    // OBJECT AND SUBOBJECT PATHS MUST KEEP ANSWERING TRUE. This is why the rewrite is
    // GetPackageMountPoint and not FPackageName::IsValidLongPackageName: '.' and ':' are in
    // INVALID_LONGPACKAGE_CHARACTERS, so the obvious IsValidLongPackageName spelling would
    // start REFUSING most of this predicate's real callers - every class reference and every
    // object path they hand it.
    TestTrue(TEXT("a class object path resolves"),
        IsValidMountPoint(TEXT("/Script/Engine.CameraActor")));
    TestTrue(TEXT("an asset object path resolves"), IsValidMountPoint(TEXT("/Game/A/B.B")));
    TestTrue(TEXT("a subobject path resolves"), IsValidMountPoint(TEXT("/Game/A/B.B:C")));

    // AN OBJECT PATH UNDER A NON-CORE MOUNT: the row that the old body got WRONG, and the reason
    // this rewrite is a bug fix and not only a tightening. The three StartsWith short-circuits
    // covered "/Game", "/Engine" and "/Script" ONLY, so an object path under any other mounted
    // root - a plugin, DLC, /Temp - fell through to IsValidLongPackageName, which rejects '.' and
    // answered FALSE. SanitizeProjectRelativePath returns EMPTY on that verdict
    // (Utils/PathUtils.cpp), so "/MyPlugin/Anims/AS_X.AS_X" normalized to nothing and every verb
    // downstream reported NOT_FOUND for an asset that loads fine.
    //
    // "/Temp/" is used as the deterministic stand-in because it is a real mounted root that is NOT
    // one of the three short-circuited names and is registered in every editor
    // (PackageName.cpp:808, :904), so this row needs no particular plugin to be installed. The
    // asset need not exist: this predicate answers a mount question and never touches the registry.
    TestTrue(TEXT("an object path under a non-core mount resolves (was FALSE - the regression)"),
        IsValidMountPoint(TEXT("/Temp/Anims/AS_X.AS_X")));
    TestTrue(TEXT("and so does its subobject form"),
        IsValidMountPoint(TEXT("/Temp/Anims/AS_X.AS_X:Track")));

    // The same row against a REAL plugin mount when this editor has one, so the stand-in above
    // cannot drift away from the case actually reported. Enumerated rather than hardcoded: which
    // plugins are mounted is a property of the host, not of this test.
    TArray<FString> RootContentPaths;
    FPackageName::QueryRootContentPaths(RootContentPaths, /*bIncludeReadOnlyRoots=*/false);
    const FString* PluginRoot = RootContentPaths.FindByPredicate(
        [](const FString& Root)
        {
            return Root != TEXT("/Game/") && Root != TEXT("/Engine/") &&
                   Root != TEXT("/Script/") && Root != TEXT("/Temp/") && Root != TEXT("/Memory/");
        });
    if (PluginRoot)
    {
        const FString PluginPackagePath = *PluginRoot + TEXT("Anims/AS_X");
        TestTrue(*FString::Printf(TEXT("'%s' is a mounted package path"), *PluginPackagePath),
            IsValidMountPoint(PluginPackagePath));
        const FString PluginObjectPath = PluginPackagePath + TEXT(".AS_X");
        TestTrue(*FString::Printf(TEXT("'%s' is a mounted OBJECT path"), *PluginObjectPath),
            IsValidMountPoint(PluginObjectPath));
    }
    else
    {
        AddInfo(TEXT("No non-core content root is mounted in this editor; the plugin-mount rows "
                     "were skipped and the /Temp/ stand-in above carries the case."));
    }

    // --- Refused, and unchanged --------------------------------------------------------
    TestFalse(TEXT("the empty string names no mount point"), IsValidMountPoint(TEXT("")));

    // --- THE DELTA: these three answered TRUE before and answer FALSE now ---------------
    //
    // Each names a root that is NOT mounted and merely shares leading text with one that is.
    // GetPackageMountPoint is segment-aware, so "GameFoo" is not "Game".
    TestFalse(TEXT("/GameFoo is not /Game (was TRUE under the StartsWith body)"),
        IsValidMountPoint(TEXT("/GameFoo/Bar")));
    TestFalse(TEXT("/Enginexyz is not /Engine (was TRUE under the StartsWith body)"),
        IsValidMountPoint(TEXT("/Enginexyz/A")));
    TestFalse(TEXT("/Scriptable is not /Script (was TRUE under the StartsWith body)"),
        IsValidMountPoint(TEXT("/Scriptable/Junk")));

    // --- Pinned BECAUSE it did not move, so a later "tidy-up" cannot move it silently ----
    //
    // See the block comment above: IsParentPathOf strips duplicate separators after the
    // parent, so this is a mounted path by the engine's own definition. Refusing it here
    // would make every prepend caller below manufacture a WORSE string ("/Game//X" would
    // become "/Game/Game//X"), which is the opposite of closing the defect.
    TestTrue(TEXT("a duplicate separator does not change WHICH mount point a path names"),
        IsValidMountPoint(TEXT("/Game//X")));
    return true;
}

// The interaction that makes tightening this predicate risky, and the reason it is tested
// separately from the verdict table.
//
// Callers spell the fallback `if (!IsValidMountPoint(P)) P = TEXT("/Game/") + P;`. While the
// predicate answered TRUE for anything merely STARTING with "/Game", that branch could not fire
// on a path that already began with a slash - so the concatenation's defect was latent. Making
// the predicate segment-aware makes it fire on exactly those inputs ("/GameFoo/Bar" and
// "/Scriptable/Junk" above), and `TEXT("/Game/") + TEXT("/GameFoo/Bar")` is "/Game//GameFoo/Bar":
// the tightening would MANUFACTURE the byte sequence CreatePackage logs Fatal on
// (UObjectGlobals.cpp:1094-1096), which ends the editor PROCESS.
//
// The composition every caller must use instead is FString::operator/, which routes through
// PathAppend: when the right side already starts with a separator it pops the left side's
// terminator instead of adding one (Core/Private/Containers/String.cpp.inl:868-879), so both
// spellings of the right operand compose to a single separator.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIsValidMountPointPrependCompositionTest,
    "PinWright.core.path.is_valid_mount_point.PrependIdiomNeverManufacturesDoubleSlash",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FIsValidMountPointPrependCompositionTest::RunTest(const FString& Parameters)
{
    // Rooted-but-unmounted inputs: each starts with '/' AND fails the mount check, which is the
    // exact pair of conditions the old concatenation could not survive.
    const TArray<FString> RootedButUnmounted = {
        TEXT("/GameFoo/Bar"),
        TEXT("/Scriptable/Junk"),
        TEXT("/Enginexyz/A"),
    };

    for (const FString& Path : RootedButUnmounted)
    {
        TestTrue(*FString::Printf(TEXT("'%s' already starts with '/'"), *Path),
            Path.StartsWith(TEXT("/")));
        TestFalse(*FString::Printf(TEXT("'%s' fails the mount check, so the prepend fires"), *Path),
            IsValidMountPoint(Path));

        const FString Composed = FString(TEXT("/Game")) / Path;
        TestFalse(*FString::Printf(TEXT("composing '%s' produces no '//'"), *Path),
            Composed.Contains(TEXT("//")));
        TestTrue(*FString::Printf(TEXT("composing '%s' lands under /Game/"), *Path),
            Composed.StartsWith(TEXT("/Game/")));
    }

    // The composed result must be the path itself under /Game, not a mangled one.
    TestEqual(TEXT("a rooted input keeps its own segments"),
        FString(TEXT("/Game")) / FString(TEXT("/GameFoo/Bar")), FString(TEXT("/Game/GameFoo/Bar")));

    // The slash-less spelling that already worked must keep working - operator/ inserts the
    // separator when the right side lacks one, so this is not a regression trade.
    TestEqual(TEXT("a slash-less input still gains exactly one separator"),
        FString(TEXT("/Game")) / FString(TEXT("Maps/L_X")), FString(TEXT("/Game/Maps/L_X")));

    // An empty right side composes to the bare root with a trailing slash, which is what the
    // callers that pop a trailing separator afterwards already expect.
    TestEqual(TEXT("an empty input composes to the bare root"),
        FString(TEXT("/Game")) / FString(), FString(TEXT("/Game/")));
    return true;
}

// ============================================================================
// NormalizeContentAssetPath
//
// The shared normalizer the audio, anim, texture, MetaSound and sound-cue clusters were folded
// onto. Its contract is narrow on purpose - empty means REFUSED - so every widening of what it
// refuses silently converts a working verb into one that answers NOT_FOUND for a healthy asset.
//
// THIS TEST EXISTS BECAUSE THAT ALREADY HAPPENED ONCE, IN THIS WAVE. The anim cluster's local
// normalizer did not call SanitizeProjectRelativePath; the fold to this function put a mount check
// underneath ~93 sites that had none. While IsValidMountPoint still ran
// FPackageName::IsValidLongPackageName for non-/Game roots, that check rejected '.' (it is in
// INVALID_LONGPACKAGE_CHARACTERS, NameTypes.h:197), so a plugin-mounted OBJECT path
// "/MyPlugin/Anims/AS_X.AS_X" normalized to EMPTY and every loader below it reported NOT_FOUND.
// "/Game/..." object paths were unaffected, which is exactly why it took a second cluster to
// surface. IsValidMountPoint now asks FPackageName::GetPackageMountPoint, which answers the mount
// question WITHOUT the package-name text pass, so the dot survives.
//
// A '.' IS LEGITIMATE IN AN OBJECT PATH AND MUST NEVER BE JUDGED BY A PACKAGE-NAME PREDICATE.
// That is the invariant these rows defend, and they are written against paths rather than against
// IsValidMountPoint so the test still fails if the narrowing returns by some other route.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNormalizeContentAssetPathObjectPathsSurviveTest,
    "PinWright.core.path.normalize_content_asset_path.ObjectPathsUnderEveryMountSurvive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNormalizeContentAssetPathObjectPathsSurviveTest::RunTest(const FString& Parameters)
{
    // Roots that are mounted on every host: the engine registers /Game/, /Engine/ and /Script/
    // unconditionally (PackageName.cpp:803-808). "/PinWright/" is mounted wherever this test can
    // run at all - it is this plugin's own mount, and the descriptor declares
    // "CanContainContent": true - which is what makes the plugin-mount row host-independent
    // instead of depending on a plugin the suite host may not have installed.
    const TArray<FString> MustSurvive = {
        TEXT("/Game/Anims/AS_X"),                    // package path, the baseline
        TEXT("/Game/Anims/AS_X.AS_X"),               // object path on the root that always worked
        TEXT("/Engine/BasicShapes/Cube.Cube"),       // object path on a read-only root
        TEXT("/Script/Engine.StaticMeshActor"),      // a script object path
        TEXT("/PinWright/Anims/AS_X"),               // plugin mount, package form
        TEXT("/PinWright/Anims/AS_X.AS_X"),          // THE REGRESSION: plugin mount, object form
        TEXT("/PinWright/A/BP_X.BP_X_C"),            // generated-class path under a plugin mount
        TEXT("/PinWright/A/BP_X.BP_X:Component"),    // subobject path under a plugin mount
    };

    for (const FString& Path : MustSurvive)
    {
        const FString Normalized = NormalizeContentAssetPath(Path);
        // Empty is this function's refusal, and a refusal here is the defect - not a near miss.
        if (!TestFalse(*FString::Printf(
                TEXT("'%s' is a mounted content path and must NOT normalize to empty"), *Path),
                Normalized.IsEmpty()))
        {
            continue;
        }
        TestEqual(*FString::Printf(TEXT("'%s' is returned unchanged"), *Path), Normalized, Path);
    }

    // The refusals the contract still owes, so the rows above cannot be satisfied by a function
    // that returns its input. Each is a genuinely unusable string rather than a legal shape.
    TestTrue(TEXT("traversal is still refused"),
        NormalizeContentAssetPath(TEXT("/Game/../Secret/AS_X")).IsEmpty());
    TestTrue(TEXT("an unmounted root is still refused"),
        NormalizeContentAssetPath(TEXT("/NotAMountPoint/A/B")).IsEmpty());
    TestTrue(TEXT("a Windows absolute path is still refused"),
        NormalizeContentAssetPath(TEXT("C:/Game/A/B")).IsEmpty());
    TestTrue(TEXT("empty in, empty out"), NormalizeContentAssetPath(FString()).IsEmpty());

    // The trailing separator is dropped rather than refused - 15 audio call sites compose
    // "<folder>/<name>" onto this result and a kept separator would be a doubled one there.
    TestEqual(TEXT("a trailing separator is trimmed, not refused"),
        NormalizeContentAssetPath(TEXT("/Game/Audio/Cues/")), FString(TEXT("/Game/Audio/Cues")));

    // "/Content" is NOT rewritten to "/Game". The rewrite the folded copies carried sat below the
    // mount check and could never fire, because "/Content" is not a mount point; this row pins
    // that the input is REFUSED rather than silently re-rooted, so a future re-add is a red test
    // rather than a quiet behaviour change. See PathUtils.h.
    TestTrue(TEXT("'/Content/...' is refused, not re-rooted onto /Game"),
        NormalizeContentAssetPath(TEXT("/Content/Audio/SC_X")).IsEmpty());
    return true;
}

// ============================================================================
// PinWrightComposeAssetPackagePath
//
// WHY THIS TEST EXISTS AT ALL, GIVEN THE DISPATCH GATE ABOVE IT. The composed-path half of this
// helper used to be pinned behaviourally by ExpectInstanceFolderRefused in
// Tests/Material/TestMaterialCreateNamePathSafety.cpp, which drove
// `path: "/Game//Materials"` through material.authoring.create_material_instance. Once the
// destination folder slot was declared `path` (MaterialCreatePathParamUtils.h), the dispatcher's
// doubled-slash pass answers that payload BEFORE the handler runs - so that case still passes but
// would now pass even against a composer that checked only the bare leaf name. The property moved
// out of reach of a wire-driven test, so it is asserted directly here instead of being quietly
// lost. The layering is deliberate (gate first, composer as defence in depth for the
// internally-composed and default-folder strings the gate never sees); the coverage should not
// thin just because the outer layer fires first.
//
// Pure-function, no dispatcher, no verb, nothing loaded - so no shape here can reach the
// CreatePackage Fatal on either a guarded or an unguarded build.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FComposeAssetPackagePathChecksComposedPathTest,
    "PinWright.core.path.compose_asset_package_path.ComposedPathIsCheckedNotJustTheLeaf",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FComposeAssetPackagePathChecksComposedPathTest::RunTest(const FString& Parameters)
{
    FString Composed;
    FString Error;

    // THE CASE THE MATERIAL TEST NO LONGER DISCRIMINATES: a perfectly legal bare leaf with the
    // "//" in the FOLDER. A name-only character filter passes this straight through; only a check
    // on the composed string refuses it.
    Composed = TEXT("/Game/Stale/Leftover");
    TestFalse(TEXT("a doubled slash in the FOLDER is refused"),
        PinWrightComposeAssetPackagePath(TEXT("/Game//Materials"), TEXT("MI_Probe"), Composed, Error));
    TestTrue(TEXT("and the refusal carries the engine's reason"), !Error.IsEmpty());
    TestTrue(TEXT("and the composed path is cleared, not left stale"), Composed.IsEmpty());

    // The leaf half, refused a step earlier by FName::IsValidXName.
    TestFalse(TEXT("a doubled slash in the NAME is refused"),
        PinWrightComposeAssetPackagePath(TEXT("/Game/Materials"), TEXT("a//b"), Composed, Error));
    TestFalse(TEXT("a folder separator in the NAME is refused"),
        PinWrightComposeAssetPackagePath(TEXT("/Game/Materials"), TEXT("Sub/Leaf"), Composed, Error));
    // A backslash is not in INVALID_OBJECTNAME_CHARACTERS but IS in
    // INVALID_LONGPACKAGE_CHARACTERS, so this one can only be caught on the composed path.
    TestFalse(TEXT("a backslash in the NAME is refused by the package rules"),
        PinWrightComposeAssetPackagePath(TEXT("/Game/Materials"), TEXT("Sub\\Leaf"), Composed, Error));
    TestFalse(TEXT("an unmounted root is refused"),
        PinWrightComposeAssetPackagePath(TEXT("/NotAMountPoint/X"), TEXT("MI_Probe"), Composed, Error));

    // CONTROL. Without these the refusals above would be satisfied by a composer that refused
    // everything.
    TestTrue(TEXT("a bare folder and a bare name compose"),
        PinWrightComposeAssetPackagePath(TEXT("/Game/Materials"), TEXT("MI_Probe"), Composed, Error));
    TestEqual(TEXT("and produce folder + '/' + name"), Composed,
        FString(TEXT("/Game/Materials/MI_Probe")));

    // THE WIDENING THIS WAVE LANDED, pinned so it cannot silently revert to Printf("%s/%s"):
    // FString::operator/ absorbs ONE trailing separator (PathAppend,
    // Core/Private/Containers/String.cpp.inl:855-885), so a trailing-slash folder is accepted and
    // composes identically. This is what let the three per-cluster trailing-slash trims be deleted.
    TestTrue(TEXT("a trailing-slash folder is accepted, not refused"),
        PinWrightComposeAssetPackagePath(TEXT("/Game/Materials/"), TEXT("MI_Probe"), Composed, Error));
    TestEqual(TEXT("and composes identically to the bare folder"), Composed,
        FString(TEXT("/Game/Materials/MI_Probe")));

    // It absorbs exactly ONE, which is why the guard is unchanged by that widening: a folder that
    // genuinely ends in "//" still composes a "//" and is still refused.
    TestFalse(TEXT("a folder ending in '//' is still refused"),
        PinWrightComposeAssetPackagePath(TEXT("/Game/Materials//"), TEXT("MI_Probe"), Composed, Error));
    return true;
}
