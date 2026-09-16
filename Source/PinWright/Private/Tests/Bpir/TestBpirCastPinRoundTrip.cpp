// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Misc/AutomationTest.h"
#include "CompilerTestUtils.h"

#include "Compiler/BpirCompiler.h"
#include "Compiler/CompilerTypes.h"
#include "Decompiler/BpirDecompiler.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"
#include "Internationalization/Regex.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

using namespace CompilerTestUtils;

namespace
{
    FString StripAuthoredPositions(FString BpirText)
    {
        const FRegexPattern PositionPattern(TEXT("\\s*@\\(\\s*-?\\d+\\s*,\\s*-?\\d+\\s*\\)"));
        FRegexMatcher Matcher(PositionPattern, BpirText);
        FString Result;
        int32 Cursor = 0;
        while (Matcher.FindNext())
        {
            const int32 Begin = Matcher.GetMatchBeginning();
            const int32 End = Matcher.GetMatchEnding();
            Result.Append(BpirText.Mid(Cursor, Begin - Cursor));
            Cursor = End;
        }
        Result.Append(BpirText.Mid(Cursor));
        return Result;
    }

    FString MakeCanonicalCastAccessor(const FString& GeneratedClassName)
    {
        FString ClassName = GeneratedClassName;
        ClassName.RemoveFromEnd(TEXT("_C"));
        ClassName.ReplaceInline(TEXT("_"), TEXT(""));
        return FString::Printf(TEXT("As%s"), *ClassName);
    }

    FString MakeSpacedCastAccessor(const FString& GeneratedClassName)
    {
        FString ClassName = GeneratedClassName;
        ClassName.RemoveFromEnd(TEXT("_C"));
        ClassName.ReplaceInline(TEXT("_"), TEXT(" "));
        return FString::Printf(TEXT("As %s"), *ClassName);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBpirCastPinCanonicalAccessorRoundTripTest,
    "PinWright.bpir.round_trip.CastPinCanonicalAccessor",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBpirCastPinCanonicalAccessorRoundTripTest::RunTest(const FString& Parameters)
{
    UBlueprint* TargetBP = CreateTransientTestBP(TEXT("B_Test_Foo_Bar"));
    TestNotNull(TEXT("Target Blueprint created"), TargetBP);
    if (!TargetBP) return false;

    FEdGraphPinType BoolType;
    BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    FBlueprintEditorUtils::AddMemberVariable(TargetBP, TEXT("bCastRoundTripFlag"), BoolType);
    FKismetEditorUtilities::CompileBlueprint(TargetBP);

    UClass* TargetClass = TargetBP->GeneratedClass;
    TestNotNull(TEXT("Target generated class exists"), TargetClass);
    if (!TargetClass) return false;

    const FString CastTypeName = TargetClass->GetName();
    const FString CanonicalAccessor = MakeCanonicalCastAccessor(CastTypeName);
    const FString SpacedAccessor = MakeSpacedCastAccessor(CastTypeName);

    UBlueprint* HostBP = CreateTransientTestBP(TEXT("CastPinCanonicalAccessorHostBP"));
    TestNotNull(TEXT("Host Blueprint created"), HostBP);
    if (!HostBP) return false;

    FEdGraphPinType BaseType;
    BaseType.PinCategory = UEdGraphSchema_K2::PC_Object;
    BaseType.PinSubCategoryObject = AActor::StaticClass();
    FBlueprintEditorUtils::AddMemberVariable(HostBP, TEXT("Base"), BaseType);
    FKismetEditorUtilities::CompileBlueprint(HostBP);

    const FString InputBpir = FString::Printf(
        TEXT("entry event BeginPlay() {\n")
        TEXT("    %%typed = cast<%s>($Base) [success -> @ok, fail -> @done]\n")
        TEXT("@ok:\n")
        TEXT("    set %%typed.%s.bCastRoundTripFlag = true\n")
        TEXT("@done:\n")
        TEXT("}"),
        *CastTypeName,
        *CanonicalAccessor);

    FBpirCompiler Compiler(HostBP);
    FCompileResult CompileResult = Compiler.Compile(InputBpir);
    if (!CompileResult.bSuccess)
    {
        for (const FCompileError& Err : CompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Initial compile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Initial compile with canonical cast accessor succeeded"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) return false;

    FBpirDecompiler Decompiler(HostBP);
    FBpirDecompileResult DecompileResult = Decompiler.Decompile();
    TestTrue(TEXT("Decompile succeeded"), DecompileResult.bSuccess);
    if (!DecompileResult.bSuccess)
    {
        for (const FBpirWarning& Warning : DecompileResult.Warnings)
        {
            AddError(FString::Printf(TEXT("Decompile warning: %s"), *Warning.Text));
        }
        return false;
    }

    const FString CanonicalNeedle = FString::Printf(TEXT(".%s.bCastRoundTripFlag"), *CanonicalAccessor);
    const FString SpacedNeedle = FString::Printf(TEXT(".`%s`.bCastRoundTripFlag"), *SpacedAccessor);
    TestTrue(TEXT("Decompiled BPIR emits canonical no-space cast accessor"),
        DecompileResult.BpirText.Contains(CanonicalNeedle));
    TestFalse(TEXT("Decompiled BPIR does not emit backtick-quoted spaced cast accessor"),
        DecompileResult.BpirText.Contains(SpacedNeedle));

    UBlueprint* RecompileBP = CreateTransientTestBP(TEXT("CastPinCanonicalAccessorRecompileBP"));
    TestNotNull(TEXT("Recompile Blueprint created"), RecompileBP);
    if (!RecompileBP) return false;

    FBlueprintEditorUtils::AddMemberVariable(RecompileBP, TEXT("Base"), BaseType);
    FKismetEditorUtilities::CompileBlueprint(RecompileBP);

    FBpirCompiler RecompileCompiler(RecompileBP);
    FCompileResult RecompileResult = RecompileCompiler.Compile(StripAuthoredPositions(DecompileResult.BpirText));
    if (!RecompileResult.bSuccess)
    {
        for (const FCompileError& Err : RecompileResult.Errors)
        {
            AddError(FString::Printf(TEXT("Recompile L%d: %s"), Err.Line, *Err.Message));
        }
    }
    TestTrue(TEXT("Recompile of decompiled canonical accessor succeeded"), RecompileResult.bSuccess);

    return true;
}
