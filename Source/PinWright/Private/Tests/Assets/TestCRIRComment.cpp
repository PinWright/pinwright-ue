// Copyright (c) 2026 Alexander Penkin. MIT License.

// CRIR comment round-trip regression. A single comment node with non-default
// text, size, and color is built via the controller, decompiled, recompiled,
// and decompiled again — the two CRIR texts must be byte-equal.
//
// Counterfactual: if EmitComment is reverted to a no-op (or if the parser's
// top-level `comment` branch is removed so comments fall through to the
// `%localId` check and parse-error), Result2 either drops the comment line or
// fails to compile — breaking byte equality.
//
// Byte-equality alone cannot see a *symmetric* drop, which is why the checks
// below exist. Decompile #1 and #2 both run the same emitter, so a field the
// emitter stops reading is absent from both texts and the round-trip stays
// byte-equal: gutting `CRIRDecompiler.cpp` `CommentNode->GetCommentText()` /
// `GetSize()` / `GetNodeColor()` (the three reads in the URigVMCommentNode arm)
// to constants silently loses the comment's text, size, and color while this
// test stays green. The independent references are (a) hard-coded expected
// substrings in decompile #1 and (b) the target Blueprint's own comment node,
// neither of which is derived from the emitter's output.
#include "Misc/AutomationTest.h"

#include "CRIR/CRIRCompiler.h"
#include "CRIR/CRIRDecompiler.h"
#include "CRIR/CRIRParser.h"
#include "Utils/ControlRigBlueprintCompat.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "RigVMModel/RigVMClient.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/Nodes/RigVMCommentNode.h"
#include "Tests/Assets/CRIRTestHelpers.h"
#include "Tests/TestUtils.h"
#include "Tests/TestSkipReporting.h"
#include "Utils/AssetUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRComment_RoundTrip,
    "PinWright.CRIR.RoundTrip.Comment",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCRIRComment_RoundTrip::RunTest(const FString& Parameters)
{
    const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString SourcePath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRComment_Src_%s"), *Guid);
    const FString TargetPath = FString::Printf(TEXT("/Game/PinWrightTests/CR_CRIRComment_Tgt_%s"), *Guid);
    const FString TargetObject = FString::Printf(TEXT("%s.CR_CRIRComment_Tgt_%s"), *TargetPath, *Guid);

    ON_SCOPE_EXIT
    {
        CleanupTestAsset(SourcePath);
        CleanupTestAsset(TargetPath);
    };

    FString CreateError;
    UBlueprint* SourceBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRComment_Src_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* SourceBP = Cast<UControlRigBlueprint>(SourceBP_Raw);
    if (!SourceBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR comment round-trip: source unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    UBlueprint* TargetBP_Raw = McpCreateControlRigBlueprint(
        FString::Printf(TEXT("CR_CRIRComment_Tgt_%s"), *Guid),
        TEXT("/Game/PinWrightTests"),
        nullptr, CreateError);
    UControlRigBlueprint* TargetBP = Cast<UControlRigBlueprint>(TargetBP_Raw);
    if (!TargetBP)
    {
        PinWrightTestSkip::SkipAssertions(*this, TEXT("fixture-unavailable"),
            FString::Printf(TEXT("CRIR comment round-trip: target unavailable (%s) - skipped."), *CreateError));
        return true;
    }

    URigVMGraph* SourceModel = nullptr;
    URigVMController* SourceController = GetFirstModelController(SourceBP, SourceModel);
    TestNotNull(TEXT("source controller"), SourceController);
    if (!SourceController) { return false; }

    URigVMNode* CommentNode = SourceController->AddCommentNode(
        TEXT("Hello"),
        FVector2D(-100, -100),
        FVector2D(300, 200),
        FLinearColor(0.2f, 0.2f, 0.2f, 0.6f),
        /*InNodeName*/ FString(TEXT("HelloComment")),
        /*bSetupUndoRedo*/ false,
        /*bPrintPythonCommand*/ false);
    TestNotNull(TEXT("comment created"), CommentNode);
    if (!CommentNode) { return false; }

    FCRIRDecompileResult Result1 = FCRIRDecompiler(SourceBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #1 (errorCode='%s')"), *Result1.ErrorCode), Result1.bSuccess);
    if (!Result1.bSuccess) { return false; }

    // Independent reference #1: the expected text is written out here, not
    // read back from the emitter. `EmitComment` formats size/color with `%g`
    // via `FString::Printf(TEXT("(%g,%g)"), ...)`, so the authored
    // FVector2D(300, 200) / FLinearColor(0.2, 0.2, 0.2, 0.6) render exactly
    // as below.
    TestTrue(TEXT("decompile #1 carries the authored comment text"),
        Result1.CRIRText.Contains(TEXT("comment \"Hello\"")));
    TestTrue(TEXT("decompile #1 carries the authored comment size"),
        Result1.CRIRText.Contains(TEXT("size=(300,200)")));
    TestTrue(TEXT("decompile #1 carries the authored comment color"),
        Result1.CRIRText.Contains(TEXT("color=(0.2,0.2,0.2,0.6)")));

    FCRIRCompileOptions Options;
    Options.TargetAssetPath = TargetObject;
    Options.Mode = ECRIRCompileMode::Replace;
    Options.bRunLayout = false;
    Options.bSave = false;

    FCRIRCompileResult CompileResult = FCRIRCompiler::Compile(Result1.CRIRText, Options);
    TestTrue(FString::Printf(TEXT("compile comment (errorCode='%s', message='%s')"),
        *CompileResult.ErrorCode, *CompileResult.ErrorMessage), CompileResult.bSuccess);
    if (!CompileResult.bSuccess) { return false; }

    FCRIRDecompileResult Result2 = FCRIRDecompiler(TargetBP).Decompile();
    TestTrue(FString::Printf(TEXT("decompile #2 (errorCode='%s')"), *Result2.ErrorCode), Result2.bSuccess);
    if (!Result2.bSuccess) { return false; }

    // Independent reference #2: the target Blueprint's own graph. Byte-equal
    // text proves the two decompiles agree; only reading the target proves the
    // comment actually landed there with its authored attributes.
    URigVMGraph* TargetModel = nullptr;
    GetFirstModelController(TargetBP, TargetModel);
    TestNotNull(TEXT("target model available"), TargetModel);
    if (TargetModel)
    {
        URigVMCommentNode* TargetComment = nullptr;
        for (URigVMNode* Node : TargetModel->GetNodes())
        {
            if (URigVMCommentNode* AsComment = Cast<URigVMCommentNode>(Node))
            {
                TargetComment = AsComment;
                break;
            }
        }
        TestNotNull(TEXT("target graph contains a comment node"), TargetComment);
        if (TargetComment)
        {
            TestEqual(TEXT("target comment text round-trips"),
                TargetComment->GetCommentText(), FString(TEXT("Hello")));
            // No FVector2D overload of TestEqual exists; compare components.
            const FVector2D TargetSize = TargetComment->GetSize();
            TestEqual(TEXT("target comment size X round-trips"), TargetSize.X, 300.0);
            TestEqual(TEXT("target comment size Y round-trips"), TargetSize.Y, 200.0);
            TestEqual(TEXT("target comment color round-trips"),
                TargetComment->GetNodeColor(), FLinearColor(0.2f, 0.2f, 0.2f, 0.6f));
        }
    }

    const FString Normalized1 = NormalizeCRIRLineEndings(Result1.CRIRText);
    const FString Normalized2 = NormalizeCRIRLineEndings(Result2.CRIRText);
    TestEqual(TEXT("comment round-trip text equality"), Normalized2, Normalized1);
    return true;
}

// B-crir-comment-attrs-silently-default: the comment attribute schema is
// closed. A present-but-invalid `size=` / `color=`, a bare flag, an unknown key
// (`colour=`) or a duplicate used to be dropped by the parser, after which the
// compiler substituted its 400x300 / black defaults and reported success.
// Counterfactual: revert ParseCommentInstruction's validation and every refusal
// case below parses OK (bOk true, no CRIR_BAD_COMMENT_ATTR), failing the test.
// The accepted cases pin that valid forms (incl. %g exponents the decompiler
// emits, and omission) still parse and reach the instruction unchanged.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCRIRCommentAttrsRefuseInvalidTest,
    "PinWright.CRIR.Parse.CommentAttrsRefuseInvalid",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCRIRCommentAttrsRefuseInvalidTest::RunTest(const FString& Parameters)
{
    const auto ParseCommentLine = [](const FString& CommentLine, TArray<FCRIREntryBlock>& Blocks, TArray<FCRIRParseError>& Errors)
    {
        const FString Text = FString::Printf(TEXT("rig_graph \"RigVMModel\" {\n    %s\n}\n"), *CommentLine);
        return FCRIRParser::Parse(Text, Blocks, Errors, /*bSkipReferenceValidation*/ false);
    };

    const TCHAR* Refused[] = {
        TEXT("comment \"Note\" size=(wide,tall) color=(1,0,0,1)"),
        TEXT("comment \"Note\" size=(300,200) color=(1,0,0)"),
        TEXT("comment \"Note\" colour=(1,0,0,1)"),
        TEXT("comment \"Note\" pinned"),
        TEXT("comment \"Note\" size="),
        TEXT("comment \"Note\" size= (1,2)"),
        TEXT("comment \"Note\" size=400"),
        TEXT("comment \"Note\" size=(300,200,)"),
        TEXT("comment \"Note\" size=(300,,200)"),
        TEXT("comment \"Note\" size=(300px,200)"),
        TEXT("comment \"Note\" size=(inf,200)"),
        TEXT("comment \"Note\" size=(1e999,200)"),
        TEXT("comment \"Note\" size=(300,200) size=(10,10)"),
    };
    for (const TCHAR* Line : Refused)
    {
        TArray<FCRIREntryBlock> Blocks;
        TArray<FCRIRParseError> Errors;
        const bool bOk = ParseCommentLine(Line, Blocks, Errors);
        TestFalse(FString::Printf(TEXT("'%s' is refused"), Line), bOk);
        const bool bTyped = Errors.Num() > 0 && Errors[0].Code == TEXT("CRIR_BAD_COMMENT_ATTR") && Errors[0].Line == 2;
        TestTrue(FString::Printf(TEXT("'%s' reports CRIR_BAD_COMMENT_ATTR on line 2 (errors='%s')"),
            Line, *JoinCRIRParseErrors(Errors)), bTyped);
    }

    struct FAccepted { const TCHAR* Line; const TCHAR* Size; const TCHAR* Color; };
    const FAccepted Accepted[] = {
        { TEXT("comment \"Note\""), TEXT(""), TEXT("") },
        { TEXT("comment \"Note\" size=(300, 200) color=(0.2, 0.2, 0.2, 0.6) @(-100, -100)"), TEXT("(300, 200)"), TEXT("(0.2, 0.2, 0.2, 0.6)") },
        { TEXT("comment \"Note\" SIZE=(1e+06,-2.5) color=(1e-05,0,1,1)"), TEXT("(1e+06,-2.5)"), TEXT("(1e-05,0,1,1)") },
    };
    for (const FAccepted& Case : Accepted)
    {
        TArray<FCRIREntryBlock> Blocks;
        TArray<FCRIRParseError> Errors;
        const bool bOk = ParseCommentLine(Case.Line, Blocks, Errors);
        TestTrue(FString::Printf(TEXT("'%s' parses (errors='%s')"), Case.Line, *JoinCRIRParseErrors(Errors)), bOk);
        if (!bOk || Blocks.Num() != 1 || Blocks[0].Instructions.Num() != 1)
        {
            AddError(FString::Printf(TEXT("'%s' did not produce exactly one instruction"), Case.Line));
            continue;
        }
        const FCRIRInstruction& Inst = Blocks[0].Instructions[0];
        TestEqual(FString::Printf(TEXT("'%s' opcode"), Case.Line), (int32)Inst.Opcode, (int32)ECRIROpcode::Comment);
        TestEqual(FString::Printf(TEXT("'%s' size slot"), Case.Line), Inst.VarType, FString(Case.Size));
        TestEqual(FString::Printf(TEXT("'%s' color slot"), Case.Line), Inst.VarDefault, FString(Case.Color));
    }
    return true;
}
