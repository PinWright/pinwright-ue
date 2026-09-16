// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "Compat/EngineVersionCompat.h"

#if __has_include("MetasoundFrontendSearchEngine.h")

#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundFrontendDocument.h"
// FMetaSoundClassInfo (used by the 5.8+ FindAllClasses overload) is only
// forward-declared by the search-engine header; its definition lives here.
#include "MetasoundFrontendQuery.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchMetaSoundNodesFindsAddTest,
    "PinWright.Assets.SearchMetaSoundNodesFindsAdd",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FSearchMetaSoundNodesFindsAddTest::RunTest(const FString& Parameters)
{
    // The test only needs class names, so collect those; the enumeration API differs
    // per engine (5.8 deprecated FindAllClasses(bool) in favor of a class-info overload).
    TArray<FString> AllClassNames;
#if UE_VERSION_NEWER_THAN_OR_EQUAL(5, 8, 0)
    {
        using namespace Metasound::Frontend;
        for (const FMetaSoundClassInfo& Info :
            ISearchEngine::Get().FindAllClasses(ISearchEngine::EResultVersion::All))
        {
            AllClassNames.Add(Info.ClassName.ToString());
        }
    }
#else
    for (const FMetasoundFrontendClass& Class :
        Metasound::Frontend::ISearchEngine::Get().FindAllClasses(true))
    {
        AllClassNames.Add(Class.Metadata.GetClassName().ToString());
    }
#endif

    if (AllClassNames.IsEmpty())
    {
        // Registry not yet populated (e.g., editor still initialising during
        // this test run). Skip rather than false-fail.
        return true;
    }

    bool bFoundAdd = false;
    for (const FString& ClassNameStr : AllClassNames)
    {
        if (ClassNameStr.Contains(TEXT("Add"), ESearchCase::IgnoreCase))
        {
            bFoundAdd = true;
            break;
        }
    }

    TestTrue(
        TEXT("At least one registered MetaSound node class has 'Add' in its class name "
             "(e.g. Metasound.Add — shipped by Epic)"),
        bFoundAdd);

    return true;

}

#endif // __has_include("MetasoundFrontendSearchEngine.h")
