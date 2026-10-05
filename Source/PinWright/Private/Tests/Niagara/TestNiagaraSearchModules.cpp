// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression tests for niagara.search_modules and its NiagaraSearch:: helpers.
// Covers: ClassifySource, ScoreModuleMatch, and dispatcher registration.
#include "Misc/AutomationTest.h"
#include "Tests/TestUtils.h"

#include "Handlers/Niagara/NiagaraSearchHandler.h"
#include "Tests/Infra/WikiDocTestHelpers.h"
#include "Internationalization/Regex.h"
#include "Misc/PackageName.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSearchModulesClassifyAndScoreTest,
    "PinWright.niagara.search_modules.ClassifyAndScore",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSearchModulesClassifyAndScoreTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraSearch;

    // ------------------------------------------------------------------
    // Dispatcher registration
    // ------------------------------------------------------------------

    TestTrue(TEXT("niagara.search_modules is registered"),
        IsRegistered(TEXT("niagara.search_modules")));

    // ------------------------------------------------------------------
    // ClassifySource
    // ------------------------------------------------------------------

    TestEqual(TEXT("ClassifySource: /Niagara/ path -> engine"),
        ClassifySource(TEXT("/Niagara/Modules/Forces/CurlNoiseForce")),
        FString(TEXT("engine")));

    TestEqual(TEXT("ClassifySource: /Game/ path -> project"),
        ClassifySource(TEXT("/Game/FX/MyModule")),
        FString(TEXT("project")));

    TestEqual(TEXT("ClassifySource: plugin path -> plugin"),
        ClassifySource(TEXT("/Some_Plugin/Modules/X")),
        FString(TEXT("plugin")));

    // False-positive guard: a project asset whose path contains "/Niagara/" as a
    // subdirectory must NOT be classified as "engine" — catches the Contains() bug.
    TestEqual(TEXT("ClassifySource: /Game/Niagara/Foo -> project (not engine)"),
        ClassifySource(TEXT("/Game/Niagara/Foo")),
        FString(TEXT("project")));

    // ------------------------------------------------------------------
    // ScoreModuleMatch
    // ------------------------------------------------------------------

    // Query matching name prefix → score > 0
    TestTrue(TEXT("ScoreModuleMatch: 'Curl' matches 'CurlNoise' > 0"),
        ScoreModuleMatch(TEXT("Curl"), TEXT("CurlNoise"), TEXT(""), TEXT("")) > 0);

    // Query with no match → score 0
    TestEqual(TEXT("ScoreModuleMatch: 'zzz' vs 'CurlNoise' = 0"),
        ScoreModuleMatch(TEXT("zzz"), TEXT("CurlNoise"), TEXT(""), TEXT("")),
        0);

    // Empty query → score 0 (pre-filter logic handles inclusion; ranking is skipped)
    TestEqual(TEXT("ScoreModuleMatch: empty query = 0"),
        ScoreModuleMatch(TEXT(""), TEXT("AnyName"), TEXT(""), TEXT("")),
        0);


    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSearchModulesStageAliasTest,
    "PinWright.niagara.search_modules.StageAlias",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSearchModulesStageAliasTest::RunTest(const FString& Parameters)
{
    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("usage"), TEXT("Module"));
    Payload->SetStringField(TEXT("stage"), TEXT("ParticleUpdate"));
    Payload->SetStringField(TEXT("sourceFilter"), TEXT("engine"));
    Payload->SetStringField(TEXT("query"), TEXT("__unlikely_no_match__"));
    Payload->SetNumberField(TEXT("limit"), 1);

    FTestResponseCapture Capture;
    const bool bFound = InvokeHandlerWithCapture(TEXT("niagara.search_modules"), Payload, Capture);
    TestTrue(TEXT("niagara.search_modules handler found"), bFound);
    TestTrue(TEXT("ParticleUpdate alias call responded"), Capture.bWasCalled);
    if (!TestTrue(TEXT("ParticleUpdate alias succeeds"), Capture.bSuccess))
    {
        AddError(FString::Printf(TEXT("search_modules error: %s %s"), *Capture.ErrorCode, *Capture.Message));
        return false;
    }

    if (TestTrue(TEXT("ParticleUpdate alias result returned"), Capture.Result.IsValid()))
    {
        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        TestTrue(TEXT("ParticleUpdate alias result has results"),
            Capture.Result->TryGetArrayField(TEXT("results"), Results) && Results != nullptr);

        double TotalMatches = 0.0;
        TestTrue(TEXT("ParticleUpdate alias result has totalMatches"),
            Capture.Result->TryGetNumberField(TEXT("totalMatches"), TotalMatches));
    }

    TSharedPtr<FJsonObject> BadPayload = MakeShared<FJsonObject>();
    BadPayload->SetStringField(TEXT("usage"), TEXT("Module"));
    BadPayload->SetStringField(TEXT("stage"), TEXT("DefinitelyNotAStage"));
    BadPayload->SetStringField(TEXT("sourceFilter"), TEXT("engine"));
    BadPayload->SetStringField(TEXT("query"), TEXT("__unlikely_no_match__"));
    BadPayload->SetNumberField(TEXT("limit"), 1);

    FTestResponseCapture BadCapture;
    const bool bBadFound = InvokeHandlerWithCapture(TEXT("niagara.search_modules"), BadPayload, BadCapture);
    TestTrue(TEXT("niagara.search_modules bad-stage handler found"), bBadFound);
    TestTrue(TEXT("bad stage call responded"), BadCapture.bWasCalled);
    TestFalse(TEXT("bad stage fails"), BadCapture.bSuccess);
    TestEqual(TEXT("bad stage error code"), BadCapture.ErrorCode, FString(TEXT("INVALID_STAGE")));

    return true;
}

// Issue #152: a descriptive multi-word query must find the PascalCase module it names.
// Before the fix the whole query was one literal substring, so "spawn rate" never matched
// "SpawnRate", and "force gravity" (words reversed) never matched GravityForce.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraSearchModulesMultiWordQueryTest,
    "PinWright.niagara.search_modules.MultiWordQueryFindsPascalCaseModule",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraSearchModulesMultiWordQueryTest::RunTest(const FString& Parameters)
{
    using namespace NiagaraSearch;

    TestEqual(TEXT("'spawn rate' is an exact name hit on SpawnRate"),
        ScoreModuleMatch(TEXT("spawn rate"), TEXT("SpawnRate"), TEXT(""), TEXT("")), 1000);
    TestEqual(TEXT("'Initialize Particle' is an exact name hit on InitializeParticle"),
        ScoreModuleMatch(TEXT("Initialize Particle"), TEXT("InitializeParticle"), TEXT(""), TEXT("")), 1000);
    TestEqual(TEXT("'solve forces' is a name prefix of SolveForcesAndVelocity"),
        ScoreModuleMatch(TEXT("solve forces"), TEXT("SolveForcesAndVelocity"), TEXT(""), TEXT("")), 500);
    TestEqual(TEXT("'spawn burst instantaneous' ignores the underscore in SpawnBurst_Instantaneous"),
        ScoreModuleMatch(TEXT("spawn burst instantaneous"), TEXT("SpawnBurst_Instantaneous"), TEXT(""), TEXT("")), 1000);
    TestEqual(TEXT("'force gravity' matches GravityForce in any word order"),
        ScoreModuleMatch(TEXT("force gravity"), TEXT("GravityForce"), TEXT(""), TEXT("")), 75);
    TestTrue(TEXT("all words in the name outrank all words only in the description"),
        ScoreModuleMatch(TEXT("force gravity"), TEXT("GravityForce"), TEXT(""), TEXT(""))
        > ScoreModuleMatch(TEXT("force gravity"), TEXT("CurlNoise"), TEXT("Applies a gravity-like force."), TEXT("")));
    TestEqual(TEXT("query words may come from name and description together"),
        ScoreModuleMatch(TEXT("solve forces position"), TEXT("SolveForcesAndVelocity"),
            TEXT("Integrates velocity and position."), TEXT("")), 50);
    TestEqual(TEXT("one missing word is still no match"),
        ScoreModuleMatch(TEXT("spawn zzz"), TEXT("SpawnRate"), TEXT(""), TEXT("")), 0);
    TestEqual(TEXT("blank query = 0"),
        ScoreModuleMatch(TEXT("   "), TEXT("SpawnRate"), TEXT(""), TEXT("")), 0);

    // Through the verb, against the stock engine modules the issue's callers searched for.
    struct FCase { const TCHAR* Query; const TCHAR* ExpectedName; };
    const FCase Cases[] = {
        { TEXT("spawn rate"),          TEXT("SpawnRate") },
        { TEXT("initialize particle"), TEXT("InitializeParticle") },
        { TEXT("particle state"),      TEXT("ParticleState") },
    };
    for (const FCase& Case : Cases)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("usage"), TEXT("Module"));
        Payload->SetStringField(TEXT("sourceFilter"), TEXT("engine"));
        Payload->SetStringField(TEXT("query"), Case.Query);
        Payload->SetNumberField(TEXT("limit"), 1);

        FTestResponseCapture Capture;
        InvokeHandlerWithCapture(TEXT("niagara.search_modules"), Payload, Capture);
        if (!TestTrue(FString::Printf(TEXT("'%s' succeeds"), Case.Query), Capture.bSuccess && Capture.Result.IsValid()))
        {
            continue;
        }
        const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
        if (!TestTrue(FString::Printf(TEXT("'%s' returns a result"), Case.Query),
                Capture.Result->TryGetArrayField(TEXT("results"), Results) && Results && Results->Num() == 1))
        {
            continue;
        }
        TestEqual(FString::Printf(TEXT("'%s' top result"), Case.Query),
            (*Results)[0]->AsObject()->GetStringField(TEXT("name")), FString(Case.ExpectedName));
    }

    return true;
}

// Issue #152: niagara.authoring's "Standard particle stack" recipe names engine module paths for
// callers to pass straight to niagara.add_module. Each must be a package on disk, and
// the recipe must keep listing the six stack modules.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FNiagaraStandardStackRecipePathsResolveTest,
    "PinWright.infra.wiki_handler.Topic.NiagaraStandardStackRecipePathsResolve",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNiagaraStandardStackRecipePathsResolveTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("niagara.authoring"), Text))
    {
        return false;
    }
    FString Section;
    if (!TestTrue(TEXT("niagara.authoring has a Standard particle stack section"),
            WikiDocTestHelpers::ExtractSection(Text, TEXT("Standard particle stack"), Section)))
    {
        return false;
    }

    const FRegexPattern Pattern(TEXT("/Niagara/Modules/[A-Za-z0-9_/]+\\.[A-Za-z0-9_]+"));
    FRegexMatcher Matcher(Pattern, Section);
    TSet<FString> Paths;
    while (Matcher.FindNext())
    {
        Paths.Add(Matcher.GetCaptureGroup(0));
    }
    for (const TCHAR* Required : { TEXT("/Niagara/Modules/Emitter/EmitterState.EmitterState"),
                                   TEXT("/Niagara/Modules/Emitter/SpawnRate.SpawnRate"),
                                   TEXT("/Niagara/Modules/Spawn/Initialization/V2/InitializeParticle.InitializeParticle"),
                                   TEXT("/Niagara/Modules/Spawn/Velocity/AddVelocity.AddVelocity"),
                                   TEXT("/Niagara/Modules/Update/Lifetime/ParticleState.ParticleState"),
                                   TEXT("/Niagara/Modules/Solvers/SolveForcesAndVelocity.SolveForcesAndVelocity") })
    {
        TestTrue(FString::Printf(TEXT("recipe lists %s"), Required), Paths.Contains(Required));
    }
    for (const FString& Path : Paths)
    {
        TestTrue(FString::Printf(TEXT("%s is a package on disk"), *Path),
            FPackageName::DoesPackageExist(FSoftObjectPath(Path).GetLongPackageName()));
    }
    return true;
}
