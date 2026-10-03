// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-python-get-editor-property-returns-live-view.
//
// Engine Python hands back containers and structs from get_editor_property as live
// references (PyUtil.cpp:906, EPyConversionMethod::Reference), but hands back their struct
// elements as detached copies (FPyWrapperArray::GetItem, PyWrapperArray.cpp:426,
// Copy default). So a before/after check around a write compares the value with
// itself, and `for v in arr: v.set_editor_property(...)` writes into temporaries.
// Neither is fixable in plugin code; the python page must say both halves in one
// place, and the save recipe's verification list must point python.execute callers
// at it. This test fails if either page loses that text.
//
// Rendered through WikiHandler::RenderPage, so it exercises the real overlay loader.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonLiveReferenceDocTest,
    "PinWright.infra.wiki_handler.NamespacePage.PythonContainersAreLiveReferences",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPythonLiveReferenceDocTest::RunTest(const FString& Parameters)
{
    FString PythonText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("python"), PythonText))
    {
        return false;
    }

    FString Section;
    if (!TestTrue(TEXT("python page renders the live-reference section above the first H3"),
            WikiDocTestHelpers::ExtractSection(PythonText,
                TEXT("Containers are live references, their struct elements are copies"), Section)))
    {
        return false;
    }

    // Half 1: the container aliases the object, and .copy() is the snapshot.
    TestTrue(TEXT("section names get_editor_property as a live reference"),
        Section.Contains(TEXT("live reference")));
    TestTrue(TEXT("section cites the Reference conversion mode"),
        Section.Contains(TEXT("EPyConversionMethod::Reference")));
    TestTrue(TEXT("section shows the before/after check that cannot fail (new state twice)"),
        Section.Contains(TEXT("`1 -> 1`, the new state twice")));
    TestTrue(TEXT("section prescribes .copy() as the snapshot"),
        Section.Contains(TEXT("get_editor_property('entries').copy()")));

    // Half 2: elements are copies, so the element-write loop silently does nothing.
    TestTrue(TEXT("section names the element-write loop"),
        Section.Contains(TEXT("for v in arr: v.set_editor_property")));
    TestTrue(TEXT("section shows the element written back by index"),
        Section.Contains(TEXT("arr[i] = v")));
    TestTrue(TEXT("section shows the owner write of the whole container"),
        Section.Contains(TEXT("obj.set_editor_property('entries', arr)")));
    TestTrue(TEXT("section says element .copy() changes nothing"),
        Section.Contains(TEXT("already is a copy")));
    TestTrue(TEXT("section scopes the copy trap to struct elements"),
        Section.Contains(TEXT("Writing to a struct element writes nothing")));
    TestTrue(TEXT("section says object elements are the same UObject"),
        Section.Contains(TEXT("Object elements are not copied")));

    // The typed surface is a snapshot by construction.
    TestTrue(TEXT("section points at property.get as a JSON snapshot"),
        Section.Contains(TEXT("property.get")));

    FString RecipeText;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("safe-mutation-save"), RecipeText))
    {
        return false;
    }
    FString Verification;
    if (!TestTrue(TEXT("safe-mutation-save renders its Verification Patterns section"),
            WikiDocTestHelpers::ExtractSection(RecipeText, TEXT("Verification Patterns"), Verification)))
    {
        return false;
    }
    TestTrue(TEXT("Verification Patterns covers python.execute"),
        Verification.Contains(TEXT("After `python.execute`")));
    TestTrue(TEXT("Verification Patterns links the python live-reference section"),
        Verification.Contains(TEXT("Containers are live references, their struct elements are copies")));

    return true;
}
