// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-geometry-array-radial-merges-in-place:
// geometry.array_radial / array_linear bake count-1 copies INTO the source mesh in
// place (no actors spawned), but their registered summary + `count` doc used to read
// as instancing ("Create a radial array...", "Number of copies including original").
// This guards the disambiguated wording so the docs/wiki keep disclosing the in-place
// merge. It exercises the live registration records (the same data WikiHandler renders),
// not a copy, so reverting the handler description strings fails these assertions.
#include "Misc/AutomationTest.h"
#include "Handlers/HandlerRegistration.h"
#include "Handlers/ParamSpec.h"

namespace
{
    const FHandlerRegistration* FindReg(const FString& MethodName)
    {
        const TArray<FHandlerRegistration>& Regs = FAutoRegisterHandler::GetPendingRegistrations();
        for (const FHandlerRegistration& Reg : Regs)
        {
            if (Reg.MethodName == MethodName)
            {
                return &Reg;
            }
        }
        return nullptr;
    }

    const FParamSpec* FindParam(const FHandlerRegistration& Reg, const FString& ParamName)
    {
        for (const FParamSpec& Param : Reg.Params)
        {
            if (Param.Name == ParamName)
            {
                return &Param;
            }
        }
        return nullptr;
    }

    // The summary and `count` doc must disclose the in-place merge: that copies are
    // merged into the source mesh and that no actors are spawned. The old wording
    // ("Create a radial array of a dynamic mesh", "Number of copies including
    // original") said neither, which is the trap this ticket fixes.
    void CheckArrayVerbDisambiguated(FAutomationTestBase& Test, const FString& MethodName)
    {
        const FHandlerRegistration* Reg = FindReg(MethodName);
        Test.TestNotNull(*FString::Printf(TEXT("%s registered"), *MethodName), Reg);
        if (!Reg)
        {
            return;
        }

        const FString Summary = Reg->Summary.ToLower();
        Test.TestTrue(
            *FString::Printf(TEXT("%s summary mentions merging into the source mesh"), *MethodName),
            Summary.Contains(TEXT("merge")) || Summary.Contains(TEXT("merged")) || Summary.Contains(TEXT("into the source mesh")));
        Test.TestTrue(
            *FString::Printf(TEXT("%s summary states no actors are spawned"), *MethodName),
            Summary.Contains(TEXT("does not spawn")) || Summary.Contains(TEXT("no actors")) || Summary.Contains(TEXT("not spawn")));

        const FParamSpec* CountParam = FindParam(*Reg, TEXT("count"));
        Test.TestNotNull(*FString::Printf(TEXT("%s has a count param"), *MethodName), CountParam);
        if (CountParam)
        {
            const FString CountDoc = CountParam->Description.ToLower();
            Test.TestTrue(
                *FString::Printf(TEXT("%s count doc discloses in-place merge (not bare 'copies including original')"), *MethodName),
                CountDoc.Contains(TEXT("in place")) || CountDoc.Contains(TEXT("merged")) || CountDoc.Contains(TEXT("no new actors")) || CountDoc.Contains(TEXT("no actors")));
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGeometryArrayDocDisambiguationTest,
    "PinWright.infra.geometry.ArrayVerbDocDisclosesInPlaceMerge",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FGeometryArrayDocDisambiguationTest::RunTest(const FString& Parameters)
{
    CheckArrayVerbDisambiguated(*this, TEXT("geometry.array_radial"));
    CheckArrayVerbDisambiguated(*this, TEXT("geometry.array_linear"));
    return true;
}
