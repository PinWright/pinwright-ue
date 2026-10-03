// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-add-variable-wiki-class-prefix-rejected.
//
// blueprint.add_variable's page used to document 'class:/Script/X.Y' / 'struct:/Game/...'
// for object and struct refs. The handler's type grammar has no colon-prefix form and
// refuses it with TYPE_NOT_FOUND (pinned by TestMakePinType), while the form that works,
// object<T>, and the other reference wrappers were never mentioned. This test fails if the
// page goes back to advertising the prefix form or stops naming the real wrappers.
//
// Rendered through WikiHandler::RenderPage, so it covers both the blueprint.md overlay
// (### blueprint.add_variable) and the registered variableType param description.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAddVariableTypeGrammarDocTest,
    "PinWright.infra.wiki_handler.MethodPage.BlueprintAddVariableTypeGrammar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FBlueprintAddVariableTypeGrammarDocTest::RunTest(const FString& Parameters)
{
    FString Page;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("blueprint.add_variable"), Page))
    {
        return false;
    }

    // The rejected colon-prefix forms are no longer offered as the way to write a ref.
    TestFalse(TEXT("param no longer offers 'class:/Script/X.Y' for object/class refs"),
        Page.Contains(TEXT("'class:/Script/X.Y' for object/class refs")));
    TestFalse(TEXT("overlay no longer says 'Beyond primitives and class:/struct: refs'"),
        Page.Contains(TEXT("Beyond primitives and `class:`/`struct:` refs")));
    TestTrue(TEXT("overlay says the colon-prefix form is refused"),
        Page.Contains(TEXT("There is no `class:` / `struct:` colon-prefix form")));
    TestTrue(TEXT("param says there is no prefix form"),
        Page.Contains(TEXT("there is no 'class:'/'struct:' prefix form")));

    // The wrappers that do work, with an object<T> and interface<T> example.
    TestTrue(TEXT("param lists the full wrapper set"),
        Page.Contains(TEXT("array<T>, set<T>, map<K,V>, object<T>, struct<T>, enum<T>, class<T>, softobject<T>, softclass<T>, interface<T>")));
    TestTrue(TEXT("param gives an object<T> example"),
        Page.Contains(TEXT("'object<MaterialInstanceDynamic>'")));
    TestTrue(TEXT("overlay says an object reference is object<T>"),
        Page.Contains(TEXT("An object reference is `object<T>`")));
    TestTrue(TEXT("overlay documents interface<T> for Blueprint Interface refs"),
        Page.Contains(TEXT("`interface<T>` (a Blueprint Interface reference")));

    return true;
}
