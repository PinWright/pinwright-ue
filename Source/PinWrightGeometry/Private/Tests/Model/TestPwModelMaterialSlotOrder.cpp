// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the implicit `Default` material slot's PLACEMENT and PWMODEL_IMPLICIT_DEFAULT_SLOT.
//
// The defect these pin: `Default` used to be allocated in model-wide FIRST-USE order like every
// other slot, so one untagged generator between two tagged ones landed at an index in the MIDDLE
// of the table and pushed every declared slot after it up one - and referencers address sections
// by index, not by name. That happened with `success: true`, a fully green health block and ZERO
// material diagnostics: the only place the fact appeared was `materialSlotList`, which a caller
// had to diff against their own source by eye. The compile that produced it was one `material=`
// away from a two-slot table and byte-identical geometry.
//
// What each test here would let through if it were reverted:
//
//  - THE DECLARED SLOTS DO NOT MOVE. `Default` is rotated to the end of the table, so a slot
//    the author's tags placed has the same index whether or not a sibling generator is tagged.
//    This is the assertion the diagnostic alone did not make: a warning is not a fix for a
//    wrong-index defect whose whole cost is paid by referencers that never read the warning.
//  - A SLOT THE AUTHOR NAMED KEEPS ITS FIRST-USE INDEX. `material="Default"` on any part-level
//    op is a name the author chose, so the rotation must not touch it - otherwise the exemption
//    for the implicit slot silently becomes a rule about the string "Default".
//  - THE UNDECLARED SLOT IS STILL REPORTED, naming the generator that forced it and the index it
//    took. Before that diagnostic this document produced no material diagnostic at all -
//    PwModelParser.cpp ValidateMaterialSlots records untagged generators in a bool rather than in
//    ReferencedSlots, so the implicit slot was structurally unreachable by the unbound check.
//  - THE ORDINARY DOCUMENT STAYS SILENT. A document with no `materials` block, one that binds
//    `Default` itself, and one whose geometry is untagged throughout are all normal and already
//    covered; a check that fired on them would be turned off wholesale and take the real case
//    with it.
#include "Misc/AutomationTest.h"

#include "Model/PwModelCompiler.h"
#include "Model/PwModelDiagnostic.h"

namespace
{
// Prefixed: this module builds with bUseUnity = true, so an anonymous-namespace helper with a
// common name would collide with a sibling test TU once Unity merges them.

FPwModelCompileResult PwModelSlotOrder_Validate(const TCHAR* Source)
{
    FPwModelCompileOptions Options;
    Options.bValidateOnly = true;
    Options.bSave = false;
    return FPwModelCompiler::Compile(Source, Options);
}

TArray<FPwDiagnostic> PwModelSlotOrder_WithCode(const FPwModelCompileResult& Result, const TCHAR* Code)
{
    TArray<FPwDiagnostic> Matching;
    for (const FPwDiagnostic& Diagnostic : Result.Diagnostics)
    {
        if (Diagnostic.Code == Code)
        {
            Matching.Add(Diagnostic);
        }
    }
    return Matching;
}

FString PwModelSlotOrder_Describe(const FPwModelCompileResult& Result)
{
    TArray<FString> Slots;
    for (int32 Index = 0; Index < Result.MaterialSlotList.Num(); ++Index)
    {
        Slots.Add(FString::Printf(TEXT("%d:%s"), Index, *Result.MaterialSlotList[Index].Name));
    }

    TArray<FString> Diagnostics;
    for (const FPwDiagnostic& Diagnostic : Result.Diagnostics)
    {
        Diagnostics.Add(Diagnostic.ToString());
    }

    return FString::Printf(TEXT("slots [%s]; diagnostics [%s]"),
        *FString::Join(Slots, TEXT(", ")), *FString::Join(Diagnostics, TEXT(" | ")));
}

// The ticket's differential repro, one `material=` apart. Line numbers are asserted below, so
// keep the two documents line-aligned when editing them.
const TCHAR* PwModelSlotOrder_MixedDocument()
{
    return
        TEXT("pwmodel 0\n")                                                        // 1
        TEXT("materials {\n")                                                      // 2
        TEXT("    Stone = \"/Game/Materials/M_Stone\"\n")                          // 3
        TEXT("    Algae = \"/Game/Materials/M_Algae\"\n")                          // 4
        TEXT("}\n")                                                                // 5
        TEXT("part a {\n")                                                         // 6
        TEXT("    box size=(100, 100, 100) at=(0, 0, 50) material=\"Stone\"\n")     // 7
        TEXT("    box size=(100, 100, 100) at=(300, 0, 50)\n")                      // 8 untagged
        TEXT("}\n")                                                                // 9
        TEXT("part b {\n")                                                         // 10
        TEXT("    box size=(100, 100, 100) at=(600, 0, 50) material=\"Algae\"\n")   // 11
        TEXT("}\n");                                                               // 12
}

const TCHAR* PwModelSlotOrder_FullyTaggedDocument()
{
    return
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Stone = \"/Game/Materials/M_Stone\"\n")
        TEXT("    Algae = \"/Game/Materials/M_Algae\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(100, 100, 100) at=(0, 0, 50) material=\"Stone\"\n")
        TEXT("    box size=(100, 100, 100) at=(300, 0, 50) material=\"Stone\"\n")
        TEXT("}\n")
        TEXT("part b {\n")
        TEXT("    box size=(100, 100, 100) at=(600, 0, 50) material=\"Algae\"\n")
        TEXT("}\n");
}
}

// ============================================================================
// An untagged generator does not move a slot the author's tags placed
// ============================================================================
//
// The behavioural half, and the one a warning cannot stand in for. Asserted as the whole table -
// count plus the name at every index - on BOTH halves of the ticket's differential, because the
// property is that the two agree on where the declared slots are: an author who reads their
// `materials` block, computes an index and hands it to `actor.spawn`'s materialPaths must get the
// same answer whether or not a sibling generator carries `material=`.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelImplicitDefaultSlotGoesLastTest,
    "PinWright.Model.MaterialSlots.UntaggedGeneratorDoesNotRenumberDeclaredSlots",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelImplicitDefaultSlotGoesLastTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Mixed = PwModelSlotOrder_Validate(PwModelSlotOrder_MixedDocument());

    TestTrue(*FString::Printf(TEXT("mixed compile succeeded. %s"), *PwModelSlotOrder_Describe(Mixed)),
        Mixed.bSuccess);

    if (TestEqual(*FString::Printf(TEXT("one untagged sibling adds a third slot. %s"),
            *PwModelSlotOrder_Describe(Mixed)), Mixed.MaterialSlotList.Num(), 3))
    {
        TestEqual(TEXT("the first tagged slot keeps index 0"),
            Mixed.MaterialSlotList[0].Name, FString(TEXT("Stone")));
        TestEqual(TEXT("the second tagged slot keeps index 1, where the author's tags put it"),
            Mixed.MaterialSlotList[1].Name, FString(TEXT("Algae")));
        TestEqual(TEXT("the implicit slot is last, after every slot a tag named"),
            Mixed.MaterialSlotList[2].Name, FString(TEXT("Default")));
    }

    // The other half of the differential: the same document one `material=` later. Two slots,
    // and - the point of the test - the two declared names sit at the same indices they do above.
    const FPwModelCompileResult Tagged =
        PwModelSlotOrder_Validate(PwModelSlotOrder_FullyTaggedDocument());

    TestTrue(*FString::Printf(TEXT("tagged compile succeeded. %s"), *PwModelSlotOrder_Describe(Tagged)),
        Tagged.bSuccess);
    if (TestEqual(*FString::Printf(TEXT("tagging the sibling leaves two slots. %s"),
            *PwModelSlotOrder_Describe(Tagged)), Tagged.MaterialSlotList.Num(), 2))
    {
        TestEqual(TEXT("'Stone' is at index 0 in the fully tagged variant too"),
            Tagged.MaterialSlotList[0].Name, FString(TEXT("Stone")));
        TestEqual(TEXT("'Algae' is at index 1 in the fully tagged variant too"),
            Tagged.MaterialSlotList[1].Name, FString(TEXT("Algae")));
    }

    TestEqual(TEXT("the count and the list it counts cannot disagree"),
        Mixed.MaterialSlots, Mixed.MaterialSlotList.Num());
    TestEqual(TEXT("no implicit-slot diagnostic on the fully tagged document"),
        PwModelSlotOrder_WithCode(Tagged, PwModelDiagnosticCodes::PWMODEL_IMPLICIT_DEFAULT_SLOT).Num(), 0);

    // A slot the author NAMED keeps its first-use index, even when an untagged sibling opened it
    // first. Without this the exemption stops being about the implicit slot and becomes a rule
    // about the string "Default", moving a slot the document tags by hand.
    const FPwModelCompileResult NamedDefault = PwModelSlotOrder_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Default = \"/Game/Materials/M_Filler\"\n")
        TEXT("    Trim    = \"/Game/Materials/M_Trim\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(100, 100, 100) at=(0, 0, 50)\n")
        TEXT("}\n")
        TEXT("part b {\n")
        TEXT("    box size=(100, 100, 100) at=(300, 0, 50) material=\"Trim\"\n")
        TEXT("}\n")
        TEXT("part c {\n")
        TEXT("    box size=(100, 100, 100) at=(600, 0, 50) material=\"Default\"\n")
        TEXT("}\n"));

    TestTrue(*FString::Printf(TEXT("named-Default compile succeeded. %s"),
        *PwModelSlotOrder_Describe(NamedDefault)), NamedDefault.bSuccess);
    if (TestEqual(*FString::Printf(TEXT("two names, two slots. %s"),
            *PwModelSlotOrder_Describe(NamedDefault)), NamedDefault.MaterialSlotList.Num(), 2))
    {
        TestEqual(TEXT("a tagged 'Default' stays at the index its first use gave it"),
            NamedDefault.MaterialSlotList[0].Name, FString(TEXT("Default")));
        TestEqual(TEXT("and the slot after it is not renumbered either"),
            NamedDefault.MaterialSlotList[1].Name, FString(TEXT("Trim")));
    }

    return true;
}

// ============================================================================
// The undeclared slot is reported, with the generator and the index
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelImplicitDefaultSlotIsReportedTest,
    "PinWright.Model.MaterialSlots.UntaggedGeneratorAllocatingAnUndeclaredSlotIsReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelImplicitDefaultSlotIsReportedTest::RunTest(const FString& Parameters)
{
    const FPwModelCompileResult Mixed = PwModelSlotOrder_Validate(PwModelSlotOrder_MixedDocument());

    TestTrue(*FString::Printf(TEXT("mixed compile succeeded. %s"), *PwModelSlotOrder_Describe(Mixed)),
        Mixed.bSuccess);

    const TArray<FPwDiagnostic> Reported =
        PwModelSlotOrder_WithCode(Mixed, PwModelDiagnosticCodes::PWMODEL_IMPLICIT_DEFAULT_SLOT);
    if (!TestEqual(*FString::Printf(TEXT("exactly one implicit-slot diagnostic. %s"),
            *PwModelSlotOrder_Describe(Mixed)), Reported.Num(), 1))
    {
        return false;
    }

    const FPwDiagnostic& Diagnostic = Reported[0];
    TestEqual(TEXT("reported as a warning, not an error"), Diagnostic.Severity, EPwSeverity::Warning);
    TestEqual(TEXT("anchored on the untagged generator, not on the part or the materials block"),
        Diagnostic.Line, 8);
    TestEqual(TEXT("names the part the untagged generator sits in"),
        Diagnostic.ScopeName, FString(TEXT("a")));

    // The three facts a caller has to act on: which op, which index the slot occupies, and that
    // the geometry there is on no material of theirs. The index is named even though nothing is
    // renumbered any more - it is what a referencer binding by index has to stay off.
    TestTrue(*FString::Printf(TEXT("names the generator. %s"), *Diagnostic.Message),
        Diagnostic.Message.Contains(TEXT("'box'"), ESearchCase::CaseSensitive));
    TestTrue(*FString::Printf(TEXT("names the index the implicit slot took. %s"), *Diagnostic.Message),
        Diagnostic.Message.Contains(TEXT("at index 2"), ESearchCase::CaseSensitive));
    TestTrue(*FString::Printf(TEXT("says the geometry ships unbound. %s"), *Diagnostic.Message),
        Diagnostic.Message.Contains(TEXT("engine default material"), ESearchCase::CaseSensitive));
    TestTrue(*FString::Printf(TEXT("names both remedies. %s"), *Diagnostic.Message),
        Diagnostic.Message.Contains(TEXT("material=\"<Slot>\""), ESearchCase::CaseSensitive)
        && Diagnostic.Message.Contains(TEXT("bind 'Default'"), ESearchCase::CaseSensitive));

    return true;
}

// ============================================================================
// The ordinary untagged document stays silent
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPwModelImplicitDefaultSlotOrdinaryDocumentsStaySilentTest,
    "PinWright.Model.MaterialSlots.OrdinaryUntaggedDocumentsAreNotReported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPwModelImplicitDefaultSlotOrdinaryDocumentsStaySilentTest::RunTest(const FString& Parameters)
{
    // No `materials` block: the author stated no slot table, so there is nothing for the implicit
    // slot to displace. This is the normal shape of most documents and the population the parser
    // keeps `Default` out of ReferencedSlots for.
    const FPwModelCompileResult NoBlock = PwModelSlotOrder_Validate(
        TEXT("pwmodel 0\n")
        TEXT("part a {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("}\n"));

    TestEqual(*FString::Printf(TEXT("a document with no materials block is silent. %s"),
        *PwModelSlotOrder_Describe(NoBlock)),
        PwModelSlotOrder_WithCode(NoBlock, PwModelDiagnosticCodes::PWMODEL_IMPLICIT_DEFAULT_SLOT).Num(), 0);

    // `Default` bound explicitly: the slot IS in the author's table, and the binding reaches the
    // asset. Warning here would report the fix as the defect.
    const FPwModelCompileResult BindsDefault = PwModelSlotOrder_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Stone   = \"/Game/Materials/M_Stone\"\n")
        TEXT("    Default = \"/Game/Materials/M_Filler\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(100, 100, 100) at=(0, 0, 50) material=\"Stone\"\n")
        TEXT("    box size=(100, 100, 100) at=(300, 0, 50)\n")
        TEXT("}\n"));

    TestEqual(*FString::Printf(TEXT("a document that binds Default is silent. %s"),
        *PwModelSlotOrder_Describe(BindsDefault)),
        PwModelSlotOrder_WithCode(BindsDefault, PwModelDiagnosticCodes::PWMODEL_IMPLICIT_DEFAULT_SLOT).Num(), 0);

    // A materials block whose slots no geometry tags: `Default` is the only slot, nothing is
    // renumbered, and PWMODEL_UNUSED_MATERIAL already reports each dropped binding with the same
    // remedy. A second diagnostic would be noise on a case that is fully covered.
    const FPwModelCompileResult NothingTagged = PwModelSlotOrder_Validate(
        TEXT("pwmodel 0\n")
        TEXT("materials {\n")
        TEXT("    Stone = \"/Game/Materials/M_Stone\"\n")
        TEXT("}\n")
        TEXT("part a {\n")
        TEXT("    box size=(100, 100, 100)\n")
        TEXT("}\n"));

    TestEqual(*FString::Printf(TEXT("a document whose geometry is untagged throughout is silent. %s"),
        *PwModelSlotOrder_Describe(NothingTagged)),
        PwModelSlotOrder_WithCode(NothingTagged, PwModelDiagnosticCodes::PWMODEL_IMPLICIT_DEFAULT_SLOT).Num(), 0);
    TestEqual(TEXT("and the binding nothing tags is still reported as dropped"),
        PwModelSlotOrder_WithCode(NothingTagged, PwModelDiagnosticCodes::PWMODEL_UNUSED_MATERIAL).Num(), 1);

    return true;
}
