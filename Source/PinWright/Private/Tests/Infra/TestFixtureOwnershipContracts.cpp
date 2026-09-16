// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Algo/Count.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IPluginManager.h"

namespace FixtureOwnershipContractHelpers
{
    enum class ELexicalState : uint8
    {
        Code,
        LineComment,
        BlockComment,
        StringLiteral,
        CharacterLiteral,
    };

    struct FFunctionBody
    {
        FString Name;
        FString Text;
        int32 StartLine = 0;
    };

    struct FRequiredGuardedRunTest
    {
        const TCHAR* RelativePath;
        const TCHAR* FunctionName;
    };

    const FRequiredGuardedRunTest IndirectEditorWorldSpawnTests[] = {
        { TEXT("PinWright/Private/Tests/Environment/TestEnvironmentDirtyFlags.cpp"),
          TEXT("FLightingSetupVolumetricFogMarksPackageDirtyTest") },
        { TEXT("PinWright/Private/Tests/Environment/TestEnvironmentDirtyFlags.cpp"),
          TEXT("FPostProcessSetBloomMarksPackageDirtyTest") },
        { TEXT("PinWright/Private/Tests/Environment/TestEnvironmentDirtyFlags.cpp"),
          TEXT("FEnvironmentSpawnReflectionCaptureMarksLevelPackageDirtyTest") },
        { TEXT("PinWright/Private/Tests/World/TestCreateProceduralTerrainLabel.cpp"),
          TEXT("FEnvironmentCreateProceduralTerrainLabelsActorTest") },
        { TEXT("PinWright/Private/Tests/World/TestEnvironmentHandlers.cpp"),
          TEXT("FEnvironmentSpawnSkyAtmosphereValidParamsTest") },
        { TEXT("PinWright/Private/Tests/World/TestEnvironmentHandlers.cpp"),
          TEXT("FEnvironmentSpawnVolumetricCloudValidParamsTest") },
        { TEXT("PinWright/Private/Tests/World/TestEnvironmentHandlers.cpp"),
          TEXT("FEnvironmentSpawnReflectionCaptureSphereTest") },
        { TEXT("PinWright/Private/Tests/World/TestEnvironmentHandlers.cpp"),
          TEXT("FEnvironmentSpawnReflectionCaptureBoxTest") },
        { TEXT("PinWright/Private/Tests/World/TestEnvironmentHandlers.cpp"),
          TEXT("FEnvironmentCreateSkySphereResolvesEngineClassTest") },
        { TEXT("PinWright/Private/Tests/World/TestVolumeHandlers.cpp"),
          TEXT("FVolumeCreateBlockingVolumeBrushGeometryTest") },
#if __has_include("WaterBodyActor.h")
        { TEXT("PinWright/Private/Tests/World/TestWaterHandlers.cpp"),
          TEXT("FWaterSpawnWaterBodyValidParamsTest") },
        { TEXT("PinWright/Private/Tests/World/TestWaterHandlers.cpp"),
          TEXT("FWaterUnderwaterSettingsSilentDropTest") },
#endif
    };

    // Exact current-tree baselines. Every entry was re-derived from the neutralized main-module
    // test tree: a new file or a changed count requires an ownership review instead of silently
    // expanding the set of raw roots/root-producing helpers.
    const TCHAR* const RawAddToRootBaseline[] = {
        TEXT("PinWright/Private/Tests/Assets/AnimAuthoringTestFixtures.h|2"),
        TEXT("PinWright/Private/Tests/Assets/NaniteRebuildSaveTestUtils.h|1"),
        TEXT("PinWright/Private/Tests/Assets/NiagaraEditTestUtils.h|2"),
        TEXT("PinWright/Private/Tests/Assets/TestAddMetaSoundInputLiteral.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestAnimSequenceDumpBuilder.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestAssetVerificationPathPreservation.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestCascadeDumpBuilder.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestEnvQueryDumpBuilder.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestMaterialHandlers.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestMaterialParameterCollectionHandlers.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestMetaSoundDestructiveOps.cpp|3"),
        TEXT("PinWright/Private/Tests/Assets/TestMetaSoundDumpBuilder.cpp|2"),
        TEXT("PinWright/Private/Tests/Assets/TestMetaSoundInterfaceOps.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestMetaSoundIOMutation.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestMetaSoundLiteralGaps.cpp|11"),
        TEXT("PinWright/Private/Tests/Assets/TestMetaSoundPatchMutatorsAccept.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestMetaSoundPatchPreset.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestMetaSoundVariables.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestMSIRDecompiler.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestNaniteRebuildShapePreservation.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestNiagaraCanonicalHandlers.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestNiagaraDumpBuilder.cpp|3"),
        TEXT("PinWright/Private/Tests/Assets/TestNiagaraDumpCompileDeferred.cpp|2"),
        TEXT("PinWright/Private/Tests/Assets/TestNiagaraHandlers.cpp|2"),
        TEXT("PinWright/Private/Tests/Assets/TestNiagaraModelBuilder.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestPwMusicGraph.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestSkeletalMeshDescribeHandler.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestSkeletonPreviewMesh.cpp|11"),
        TEXT("PinWright/Private/Tests/Assets/TestSoundCueDumpBuilder.cpp|2"),
        TEXT("PinWright/Private/Tests/Assets/TestStateTreeDumpBuilder.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestStaticMeshBakeTransformHandler.cpp|2"),
        TEXT("PinWright/Private/Tests/Assets/TestStaticMeshDescribeHandler.cpp|2"),
        TEXT("PinWright/Private/Tests/Assets/TestStaticMeshDumpSections.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestStaticMeshSetCollisionComplexityHandler.cpp|1"),
        TEXT("PinWright/Private/Tests/Assets/TestUserDefinedStructDumpBuilder.cpp|1"),
        TEXT("PinWright/Private/Tests/Blueprint/TestBlueprintReferencesCaseSensitive.cpp|2"),
        TEXT("PinWright/Private/Tests/Core/TestAssetSaveHonesty.cpp|4"),
        TEXT("PinWright/Private/Tests/Core/TestPackageDirtyUtils.cpp|1"),
        TEXT("PinWright/Private/Tests/DataTable/TestDataTableAuthoring.cpp|1"),
        TEXT("PinWright/Private/Tests/Format/TestPwAnimCompiler.cpp|3"),
        TEXT("PinWright/Private/Tests/Gameplay/TestAimOffsetAddSampleSilentDrop.cpp|4"),
        TEXT("PinWright/Private/Tests/Gameplay/TestAnimationFixtures.h|2"),
        TEXT("PinWright/Private/Tests/Gameplay/TestAnimationHandlers.cpp|6"),
        TEXT("PinWright/Private/Tests/Gameplay/TestBTIRDecompiler.cpp|2"),
        TEXT("PinWright/Private/Tests/Gameplay/TestSkinWeightAudit.cpp|10"),
        TEXT("PinWright/Private/Tests/Gameplay/TestSkinWeightBaseReadback.cpp|6"),
        TEXT("PinWright/Private/Tests/Media/TestAudioAnalysisHandler.cpp|1"),
        TEXT("PinWright/Private/Tests/Media/TestAudioHandlers.cpp|3"),
        TEXT("PinWright/Private/Tests/Media/TestAudioSynthAudition.cpp|1"),
        TEXT("PinWright/Private/Tests/Media/TestPwAudioDecode.cpp|1"),
        TEXT("PinWright/Private/Tests/Media/TestPwFxChainB.cpp|1"),
        TEXT("PinWright/Private/Tests/Media/TestPwGenSampleGranular.cpp|1"),
        TEXT("PinWright/Private/Tests/Media/TestPwMetaSoundRender.cpp|1"),
        TEXT("PinWright/Private/Tests/Media/TestSequencerHandlers.cpp|1"),
        TEXT("PinWright/Private/Tests/Media/TestSoundCueChildAttachment.cpp|1"),
        TEXT("PinWright/Private/Tests/Media/TestSoundWaveDescribeHandler.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraAddEmitterQuiesce.cpp|3"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraCompileQuiesce.cpp|5"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraCompileSave.cpp|3"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraCompileWait.cpp|3"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraDataInterfaceConsistency.cpp|4"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraGraphCreateNodeCreatorGuard.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSystemGraphEmitterWiring.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraValidateScriptCompileError.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNIRDecompiler.cpp|7"),
        TEXT("PinWright/Private/Tests/Render/TestAnimationCaptureHandlers.cpp|1"),
        TEXT("PinWright/Private/Tests/UI/TestUiSetWidgetTextResolvedWorld.cpp|1"),
        TEXT("PinWright/Private/Tests/Utility/TestPropertyListContainerCppType.cpp|1"),
        TEXT("PinWright/Private/Tests/Utility/TestPropertyListNameFilter.cpp|1"),
        TEXT("PinWright/Private/Tests/Widget/TestWidgetAnimationGeneratedPropertyBinding.cpp|1"),
        TEXT("PinWright/Private/Tests/Widget/TestWidgetBindResolution.cpp|1"),
        TEXT("PinWright/Private/Tests/World/TestEnvironmentHandlers.cpp|1"),
    };

    const TCHAR* const RootedProducerBaseline[] = {
        TEXT("PinWright/Private/Tests/Assets/AnimAuthoringTestFixtures.h|2"),
        TEXT("PinWright/Private/Tests/Assets/TestAnimSequenceCreate.cpp|2"),
        TEXT("PinWright/Private/Tests/Assets/TestAnimSequenceDumpBuilder.cpp|5"),
        TEXT("PinWright/Private/Tests/Format/TestPwAnimCompiler.cpp|6"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraCurveHandler.cpp|4"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraEditorOpenGuard.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraGetModuleInputs.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraGraphsLinkedPinDefault.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraModuleInputDefaults.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraModuleInputReachability.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraModuleInputValidation.cpp|2"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraMoveModule.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraRapidIterationSync.cpp|3"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraResetModuleInput.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInput.cpp|7"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInputDynamicInput.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInputLinkedOverride.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInputMatrixQuat.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInputReplacedOverride.cpp|2"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleScript.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNIRDecompiler.cpp|4"),
        TEXT("PinWright/Private/Tests/Niagara/TestNIRFixtures.cpp|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNIRFixtures.h|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNIRGraphDataflow.cpp|12"),
        TEXT("PinWright/Private/Tests/Niagara/TestNIRGraphLinkCoverage.cpp|1"),
    };

    const TCHAR* const LegacyNiagaraProducerCallers[] = {
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraCurveHandler.cpp|FNiagaraSetCurveKeysModuleInputTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraCurveHandler.cpp|FNiagaraSetCurveKeysUnknownInputTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraCurveHandler.cpp|FNiagaraSetCurveKeysStoreMissNamesModuleFormTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraCurveHandler.cpp|FNiagaraSetCurveKeysAddressingExclusivityTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraEditorOpenGuard.cpp|FNiagaraClearModuleOverridesGraphNotifyTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraGraphsLinkedPinDefault.cpp|FNiagaraGraphsLinkedPinDefaultNotEchoedTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraModuleInputValidation.cpp|FNiagaraSetModuleInputLinksParticleAttributeTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraModuleInputValidation.cpp|FNiagaraSetModuleInputRejectsDottedSubInputTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInput.cpp|FNiagaraSetModuleInputInfersOwningEmitterUpdateStackTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInput.cpp|FNiagaraSetStackEnabledInfersOwningEmitterUpdateStackTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInput.cpp|FNiagaraSetModuleInputAcceptsVector2DTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInput.cpp|FNiagaraSetModuleInputLinksParameterTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInput.cpp|FNiagaraSetModuleInputLinkMissingParameterRejectedTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInput.cpp|FNiagaraSetModuleInputEchoesWrittenValueTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInput.cpp|FNiagaraSetModuleInputRefusalLeavesPackageCleanTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInputDynamicInput.cpp|FNiagaraSetModuleInputAssignsDynamicInputChainTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInputLinkedOverride.cpp|FNiagaraSetModuleInputRefusesLiteralOverLinkedOverrideTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInputReplacedOverride.cpp|FNiagaraSetModuleInputLinkReportsReplacedOverrideTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleInputReplacedOverride.cpp|FNiagaraSetModuleInputDynamicInputReportsReplacedOverrideTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNiagaraSetModuleScript.cpp|FNiagaraSetModuleScriptInfersOwningEmitterUpdateStackTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNIRDecompiler.cpp|FNiagaraNirOverrideLiteralFloatTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNIRDecompiler.cpp|FNiagaraNirOverrideLinkedParamTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNIRDecompiler.cpp|FNiagaraNirOverrideDynamicInputTest|1"),
        TEXT("PinWright/Private/Tests/Niagara/TestNIRDecompiler.cpp|FNiagaraNirOverrideRecursionDepthTest|1"),
    };

    FString ResolveSourceRoot()
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PinWright"));
        return Plugin.IsValid() ? (Plugin->GetBaseDir() / TEXT("Source")) : FString();
    }

    FString MakeRelative(const FString& AnyPath, const FString& SourceRoot)
    {
        FString Relative = FPaths::ConvertRelativePathToFull(AnyPath);
        const FString FullRoot = FPaths::ConvertRelativePathToFull(SourceRoot) / TEXT("");
        FPaths::MakePathRelativeTo(Relative, *FullRoot);
        return Relative.Replace(TEXT("\\"), TEXT("/"));
    }

    int32 CountPattern(const FString& Text, const TCHAR* Pattern)
    {
        FRegexMatcher Matcher{FRegexPattern(Pattern), Text};
        int32 Count = 0;
        while (Matcher.FindNext())
        {
            ++Count;
        }
        return Count;
    }

    TMap<FString, int32> ParseCountBaseline(
        FAutomationTestBase& Test,
        const TCHAR* const* Entries,
        int32 EntryCount,
        const TCHAR* Label)
    {
        TMap<FString, int32> Result;
        for (int32 Index = 0; Index < EntryCount; ++Index)
        {
            const FString Entry(Entries[Index]);
            FString Key;
            FString CountText;
            if (!Entry.Split(TEXT("|"), &Key, &CountText, ESearchCase::CaseSensitive,
                    ESearchDir::FromEnd) || Key.IsEmpty() || !CountText.IsNumeric())
            {
                Test.AddError(FString::Printf(TEXT("Malformed %s entry: %s"), Label, *Entry));
                continue;
            }
            if (Result.Contains(Key))
            {
                Test.AddError(FString::Printf(TEXT("Duplicate %s entry: %s"), Label, *Key));
                continue;
            }
            Result.Add(Key, FCString::Atoi(*CountText));
        }
        return Result;
    }

    TArray<int32> FindPatternPositions(const FString& Text, const TCHAR* Pattern)
    {
        TArray<int32> Positions;
        FRegexMatcher Matcher{FRegexPattern(Pattern), Text};
        while (Matcher.FindNext())
        {
            Positions.Add(Matcher.GetMatchBeginning());
        }
        return Positions;
    }

    void BlankSourceCharacter(FString& Text, int32 Index)
    {
        if (Text[Index] != TEXT('\r') && Text[Index] != TEXT('\n'))
        {
            Text[Index] = TEXT(' ');
        }
    }

    bool IsNumericLiteralSeparator(const FString& Text, int32 Index)
    {
        if (Index <= 0 || Index + 1 >= Text.Len() || !FChar::IsAlnum(Text[Index + 1]))
        {
            return false;
        }
        int32 TokenStart = Index - 1;
        while (TokenStart >= 0 &&
               (FChar::IsAlnum(Text[TokenStart]) || Text[TokenStart] == TEXT('.')))
        {
            --TokenStart;
        }
        return TokenStart + 1 < Index && FChar::IsDigit(Text[TokenStart + 1]);
    }

    bool NeutralizeOwnershipSourceText(
        FAutomationTestBase& Test,
        const FString& SourceName,
        const FString& Input,
        FString& OutText)
    {
        OutText = Input;
        ELexicalState State = ELexicalState::Code;
        for (int32 Index = 0; Index < Input.Len(); ++Index)
        {
            const TCHAR Current = Input[Index];
            const TCHAR Next = Index + 1 < Input.Len() ? Input[Index + 1] : TEXT('\0');

            if (State == ELexicalState::Code)
            {
                if (Current == TEXT('/') && Next == TEXT('/'))
                {
                    BlankSourceCharacter(OutText, Index);
                    BlankSourceCharacter(OutText, ++Index);
                    State = ELexicalState::LineComment;
                }
                else if (Current == TEXT('/') && Next == TEXT('*'))
                {
                    BlankSourceCharacter(OutText, Index);
                    BlankSourceCharacter(OutText, ++Index);
                    State = ELexicalState::BlockComment;
                }
                else if (Current == TEXT('R') && Next == TEXT('"'))
                {
                    int32 OpenParen = Index + 2;
                    while (OpenParen < Input.Len() && Input[OpenParen] != TEXT('('))
                    {
                        const TCHAR DelimiterChar = Input[OpenParen];
                        if (OpenParen - (Index + 2) >= 16 ||
                            FChar::IsWhitespace(DelimiterChar) ||
                            DelimiterChar == TEXT(')') || DelimiterChar == TEXT('\\'))
                        {
                            Test.AddError(FString::Printf(
                                TEXT("%s:%d has a malformed raw string delimiter."),
                                *SourceName, static_cast<int32>(Algo::Count(Input.Left(Index), TEXT('\n'))) + 1));
                            return false;
                        }
                        ++OpenParen;
                    }
                    if (OpenParen >= Input.Len())
                    {
                        Test.AddError(FString::Printf(
                            TEXT("%s:%d has an unterminated raw string opener."),
                            *SourceName, static_cast<int32>(Algo::Count(Input.Left(Index), TEXT('\n'))) + 1));
                        return false;
                    }

                    const FString Delimiter = Input.Mid(Index + 2, OpenParen - Index - 2);
                    const FString Terminator = TEXT(")") + Delimiter + TEXT("\"");
                    const int32 TerminatorStart = Input.Find(
                        Terminator, ESearchCase::CaseSensitive, ESearchDir::FromStart, OpenParen + 1);
                    if (TerminatorStart == INDEX_NONE)
                    {
                        Test.AddError(FString::Printf(
                            TEXT("%s:%d has an unterminated raw string literal."),
                            *SourceName, static_cast<int32>(Algo::Count(Input.Left(Index), TEXT('\n'))) + 1));
                        return false;
                    }
                    const int32 LiteralEnd = TerminatorStart + Terminator.Len();
                    while (Index < LiteralEnd)
                    {
                        BlankSourceCharacter(OutText, Index++);
                    }
                    --Index;
                }
                else if (Current == TEXT('"'))
                {
                    BlankSourceCharacter(OutText, Index);
                    State = ELexicalState::StringLiteral;
                }
                else if (Current == TEXT('\'') && !IsNumericLiteralSeparator(Input, Index))
                {
                    BlankSourceCharacter(OutText, Index);
                    State = ELexicalState::CharacterLiteral;
                }
            }
            else if (State == ELexicalState::LineComment)
            {
                BlankSourceCharacter(OutText, Index);
                if (Current == TEXT('\n'))
                {
                    State = ELexicalState::Code;
                }
            }
            else if (State == ELexicalState::BlockComment)
            {
                BlankSourceCharacter(OutText, Index);
                if (Current == TEXT('*') && Next == TEXT('/'))
                {
                    BlankSourceCharacter(OutText, ++Index);
                    State = ELexicalState::Code;
                }
            }
            else
            {
                BlankSourceCharacter(OutText, Index);
                if (Current == TEXT('\\') && Index + 1 < Input.Len())
                {
                    BlankSourceCharacter(OutText, ++Index);
                    if (Input[Index] == TEXT('\r') &&
                        Index + 1 < Input.Len() && Input[Index + 1] == TEXT('\n'))
                    {
                        BlankSourceCharacter(OutText, ++Index);
                    }
                }
                else if ((State == ELexicalState::StringLiteral && Current == TEXT('"')) ||
                         (State == ELexicalState::CharacterLiteral && Current == TEXT('\'')))
                {
                    State = ELexicalState::Code;
                }
                else if (Current == TEXT('\r') || Current == TEXT('\n'))
                {
                    Test.AddError(FString::Printf(
                        TEXT("%s:%d has an unterminated ordinary string or character literal."),
                        *SourceName, static_cast<int32>(Algo::Count(Input.Left(Index), TEXT('\n'))) + 1));
                    return false;
                }
            }
        }

        if (State == ELexicalState::BlockComment ||
            State == ELexicalState::StringLiteral ||
            State == ELexicalState::CharacterLiteral)
        {
            Test.AddError(FString::Printf(
                TEXT("%s ends inside a comment, string, or character literal."), *SourceName));
            return false;
        }
        return true;
    }

    bool ReadNeutralizedSource(
        FAutomationTestBase& Test,
        const FString& AbsolutePath,
        FString& OutContents)
    {
        FString RawContents;
        if (!FFileHelper::LoadFileToString(RawContents, *AbsolutePath))
        {
            Test.AddError(FString::Printf(TEXT("Could not read required source file: %s"),
                *AbsolutePath));
            return false;
        }
        return NeutralizeOwnershipSourceText(Test, AbsolutePath, RawContents, OutContents);
    }

    bool FindMatchingBrace(const FString& Text, int32 OpenBrace, int32& OutCloseBrace)
    {
        int32 Depth = 0;
        for (int32 Index = OpenBrace; Index < Text.Len(); ++Index)
        {
            if (Text[Index] == TEXT('{'))
            {
                ++Depth;
            }
            else if (Text[Index] == TEXT('}'))
            {
                --Depth;
                if (Depth == 0)
                {
                    OutCloseBrace = Index;
                    return true;
                }
            }
        }
        return false;
    }

    bool ExtractRunTestBodies(
        FAutomationTestBase& Test,
        const FString& RelativePath,
        const FString& Contents,
        TArray<FFunctionBody>& OutBodies)
    {
        const FRegexPattern RunTestPattern(
            TEXT("\\bbool\\s+([A-Za-z_][A-Za-z0-9_]*)::RunTest\\s*\\([^)]*\\)\\s*\\{"));
        FRegexMatcher Matcher(RunTestPattern, Contents);
        while (Matcher.FindNext())
        {
            const int32 OpenBrace = Matcher.GetMatchEnding() - 1;
            int32 CloseBrace = INDEX_NONE;
            if (!FindMatchingBrace(Contents, OpenBrace, CloseBrace))
            {
                Test.AddError(FString::Printf(
                    TEXT("%s:%d has an unterminated RunTest body; ownership scan aborted."),
                    *RelativePath, static_cast<int32>(Algo::Count(Contents.Left(OpenBrace), TEXT('\n'))) + 1));
                return false;
            }

            FFunctionBody& Body = OutBodies.AddDefaulted_GetRef();
            Body.Name = Matcher.GetCaptureGroup(1);
            Body.Text = Contents.Mid(OpenBrace + 1, CloseBrace - OpenBrace - 1);
            Body.StartLine = static_cast<int32>(Algo::Count(Contents.Left(OpenBrace), TEXT('\n'))) + 1;
        }
        return true;
    }

    TArray<int32> ScopePathAt(const FString& Text, int32 Position)
    {
        TArray<int32> ScopePath;
        ScopePath.Add(INDEX_NONE);
        for (int32 Index = 0; Index < Position; ++Index)
        {
            if (Text[Index] == TEXT('{'))
            {
                ScopePath.Add(Index);
            }
            else if (Text[Index] == TEXT('}') && ScopePath.Num() > 1)
            {
                ScopePath.Pop();
            }
        }
        return ScopePath;
    }

    bool IsScopePrefix(const TArray<int32>& Candidate, const TArray<int32>& Current)
    {
        if (Candidate.Num() > Current.Num())
        {
            return false;
        }
        for (int32 Index = 0; Index < Candidate.Num(); ++Index)
        {
            if (Candidate[Index] != Current[Index])
            {
                return false;
            }
        }
        return true;
    }

    bool HasActiveGuardBefore(const FString& Body, int32 TriggerPosition)
    {
        const TArray<int32> TriggerScope = ScopePathAt(Body, TriggerPosition);
        const TArray<int32> GuardPositions = FindPatternPositions(
            Body,
            TEXT("\\bFScopedEditorWorldActorGuard\\s+[A-Za-z_][A-Za-z0-9_]*\\s*(?:[;{(])"));
        for (const int32 GuardPosition : GuardPositions)
        {
            if (GuardPosition < TriggerPosition &&
                IsScopePrefix(ScopePathAt(Body, GuardPosition), TriggerScope))
            {
                return true;
            }
        }
        return false;
    }

    const FFunctionBody* FindBodyByName(
        const TArray<FFunctionBody>& Bodies,
        const FString& Name)
    {
        return Bodies.FindByPredicate(
            [&Name](const FFunctionBody& Body) { return Body.Name == Name; });
    }

    int32 CheckLiveWorldBody(
        FAutomationTestBase& Test,
        const FString& RelativePath,
        const FFunctionBody& Body)
    {
        TArray<int32> TriggerPositions = FindPatternPositions(
            Body.Text, TEXT("\\bSpawnActorInActiveWorld\\s*<"));
        TriggerPositions.Append(FindPatternPositions(
            Body.Text,
            TEXT("\\b(?:SpawnActorWithRootScene|SpawnCompBindProbeActor|MakeActorFixture|"
                 "PrepareUnsupportedFixture)\\s*\\(")));
        TriggerPositions.Sort();

        for (const int32 TriggerPosition : TriggerPositions)
        {
            if (!HasActiveGuardBefore(Body.Text, TriggerPosition))
            {
                Test.AddError(FString::Printf(
                    TEXT("%s:%d %s::RunTest reaches a live editor-world spawn before an active "
                         "FScopedEditorWorldActorGuard declaration in the same lexical scope."),
                    *RelativePath,
                    Body.StartLine + static_cast<int32>(Algo::Count(Body.Text.Left(TriggerPosition), TEXT('\n'))),
                    *Body.Name));
            }
        }
        return TriggerPositions.Num();
    }

    bool CheckNiagaraCallerBody(
        FAutomationTestBase& Test,
        const FString& RelativePath,
        const FFunctionBody& Body,
        const TMap<FString, int32>& LegacyBaseline,
        TMap<FString, int32>& ObservedLegacy,
        int32& OutProducerCalls)
    {
        const TArray<int32> ProducerCalls = FindPatternPositions(
            Body.Text,
            TEXT("\\b(?:BuildScriptHostingGraph|BuildEmptySystemWithEmitter)\\s*\\("));
        OutProducerCalls += ProducerCalls.Num();
        if (ProducerCalls.IsEmpty())
        {
            return true;
        }

        const TArray<int32> GuardDeclarations = FindPatternPositions(
            Body.Text,
            TEXT("\\b(?:NiagaraEditTestUtils::)?FAuthorableSystemRoots\\s+"
                 "[A-Za-z_][A-Za-z0-9_]*"));
        bool bUsesScopedGuard = false;
        for (const int32 ProducerCall : ProducerCalls)
        {
            const int32* GuardAfterCall = GuardDeclarations.FindByPredicate(
                [ProducerCall](const int32 Position) { return Position > ProducerCall; });
            if (!GuardAfterCall ||
                ScopePathAt(Body.Text, ProducerCall) != ScopePathAt(Body.Text, *GuardAfterCall))
            {
                const FString LegacyKey = RelativePath + TEXT("|") + Body.Name;
                if (!LegacyBaseline.Contains(LegacyKey))
                {
                    Test.AddError(FString::Printf(
                        TEXT("%s:%d %s::RunTest receives a rooted Niagara fixture without "
                             "FAuthorableSystemRoots; add scoped ownership or explicitly review "
                             "the caller for the legacy allow-list."),
                        *RelativePath,
                        Body.StartLine + static_cast<int32>(Algo::Count(Body.Text.Left(ProducerCall), TEXT('\n'))),
                        *Body.Name));
                }
                else
                {
                    ObservedLegacy.FindOrAdd(LegacyKey)++;
                }
            }
            else
            {
                bUsesScopedGuard = true;
            }
        }

        const bool bCapturesOwnership = !bUsesScopedGuard ||
            CountPattern(Body.Text, TEXT("\\bCaptureFixtureRoots\\s*\\(")) > 0 ||
            (CountPattern(Body.Text, TEXT("\\bRoots\\s*\\.\\s*System\\s*=")) > 0 &&
             CountPattern(Body.Text, TEXT("\\bRoots\\s*\\.\\s*Emitter\\s*=")) > 0);
        if (!bCapturesOwnership)
        {
            Test.AddError(FString::Printf(
                TEXT("%s:%d %s::RunTest declares a root guard but does not transfer both the "
                     "system and emitter roots into it."),
                *RelativePath, Body.StartLine, *Body.Name));
        }
        return bCapturesOwnership;
    }

    int32 CheckAnimationFactoryCallers(
        FAutomationTestBase& Test,
        const FString& RelativePath,
        const FFunctionBody& Body)
    {
        const FRegexPattern CallPattern(
            TEXT("\\b([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*"
                 "(?:AnimAuthoringTestFixtures\\s*::\\s*)?"
                 "(?:NewTransientAnimSequence|NewTransientSkeletonWithBones)\\s*\\("));
        FRegexMatcher Matcher(CallPattern, Body.Text);
        int32 Calls = 0;
        while (Matcher.FindNext())
        {
            ++Calls;
            const FString Receiver = Matcher.GetCaptureGroup(1);
            const int32 ProducerStatementEnd = Body.Text.Find(
                TEXT(";"), ESearchCase::CaseSensitive, ESearchDir::FromStart,
                Matcher.GetMatchEnding());
            const FString GuardPattern = FString::Printf(
                TEXT("\\b(?:AnimAuthoringTestFixtures\\s*::\\s*)?FScopedAnimAssetRoot\\s+"
                     "[A-Za-z_][A-Za-z0-9_]*\\s*\\(\\s*%s\\s*\\)\\s*;"), *Receiver);
            FRegexMatcher GuardMatcher(FRegexPattern(GuardPattern), Body.Text);
            GuardMatcher.SetLimits(
                ProducerStatementEnd == INDEX_NONE ? Matcher.GetMatchEnding() : ProducerStatementEnd + 1,
                Body.Text.Len());
            const bool bHasGuard = GuardMatcher.FindNext();
            const bool bGuardIsNextStatement = bHasGuard && ProducerStatementEnd != INDEX_NONE &&
                Body.Text.Mid(ProducerStatementEnd + 1,
                    GuardMatcher.GetMatchBeginning() - ProducerStatementEnd - 1)
                    .TrimStartAndEnd().IsEmpty();
            if (!bGuardIsNextStatement ||
                ScopePathAt(Body.Text, Matcher.GetMatchBeginning()) !=
                    ScopePathAt(Body.Text, GuardMatcher.GetMatchBeginning()))
            {
                Test.AddError(FString::Printf(
                    TEXT("%s:%d %s::RunTest receives rooted '%s' from an animation fixture "
                         "factory without constructing FScopedAnimAssetRoot from it as the next "
                         "statement in the same lexical scope."),
                    *RelativePath,
                    Body.StartLine + static_cast<int32>(Algo::Count(Body.Text.Left(Matcher.GetMatchBeginning()), TEXT('\n'))),
                    *Body.Name, *Receiver));
            }
        }
        return Calls;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditorWorldSpawnGuardsTest,
    "PinWright.infra.contract.EditorWorldSpawn.GuardedTestSpawns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FEditorWorldSpawnGuardsTest::RunTest(const FString& Parameters)
{
    using namespace FixtureOwnershipContractHelpers;

    const FString LexicalSample =
        TEXT("bool FLexicalSample::RunTest(const FString&) {\n")
        TEXT("    const TCHAR* Ordinary = TEXT(\"{\");\n")
        TEXT("    const TCHAR Character = '}';\n")
        TEXT("    const TCHAR* Raw = R\"brace({)brace\";\n")
        TEXT("    const uint64 Canary = 0x5049'4E57'5249'4748ull;\n")
        TEXT("    // }\n")
        TEXT("    /* { */\n")
        TEXT("    FScopedEditorWorldActorGuard Guard;\n")
        TEXT("    SpawnActorInActiveWorld<AActor>();\n")
        TEXT("}\n");
    FString NeutralizedSample;
    if (!TestTrue(TEXT("Lexical neutralizer accepts quoted and commented unmatched braces"),
            NeutralizeOwnershipSourceText(
                *this, TEXT("FixtureOwnershipContract.LexicalSelfCheck"),
                LexicalSample, NeutralizedSample)))
    {
        return false;
    }
    TestEqual(TEXT("Lexical neutralizer preserves source offsets"),
        NeutralizedSample.Len(), LexicalSample.Len());
    TestEqual(TEXT("Lexical neutralizer preserves source line structure"),
        static_cast<int32>(Algo::Count(NeutralizedSample, TEXT('\n'))),
        static_cast<int32>(Algo::Count(LexicalSample, TEXT('\n'))));
    TArray<FFunctionBody> LexicalBodies;
    if (!TestTrue(TEXT("Quoted braces do not break RunTest body extraction"),
            ExtractRunTestBodies(
                *this, TEXT("FixtureOwnershipContract.LexicalSelfCheck"),
                NeutralizedSample, LexicalBodies)))
    {
        return false;
    }
    TestEqual(TEXT("Lexical self-check contains one RunTest body"), LexicalBodies.Num(), 1);
    if (LexicalBodies.Num() == 1)
    {
        TestEqual(TEXT("Lexical self-check preserves one guarded spawn trigger"),
            CheckLiveWorldBody(
                *this, TEXT("FixtureOwnershipContract.LexicalSelfCheck"), LexicalBodies[0]), 1);
    }

    const FString SourceRoot = ResolveSourceRoot();
    if (!TestFalse(TEXT("Resolved the plugin Source root"), SourceRoot.IsEmpty()))
    {
        return false;
    }

    const FString TestsRoot = SourceRoot / TEXT("PinWright/Private/Tests");
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *TestsRoot, TEXT("*.cpp"), true, false, false);
    if (!TestTrue(TEXT("Found PinWright C++ test sources"), Files.Num() > 0))
    {
        return false;
    }

    TMap<FString, TArray<FFunctionBody>> BodiesByFile;
    int32 FilesRead = 0;
    int32 SpawnTriggers = 0;
    for (const FString& File : Files)
    {
        FString Contents;
        if (!ReadNeutralizedSource(*this, File, Contents))
        {
            continue;
        }
        ++FilesRead;

        const FString Relative = MakeRelative(File, SourceRoot);
        TArray<FFunctionBody>& Bodies = BodiesByFile.FindOrAdd(Relative);
        if (!ExtractRunTestBodies(*this, Relative, Contents, Bodies))
        {
            continue;
        }
        for (const FFunctionBody& Body : Bodies)
        {
            SpawnTriggers += CheckLiveWorldBody(*this, Relative, Body);
        }
    }

    TestEqual(TEXT("Read every discovered PinWright C++ test source"), FilesRead, Files.Num());
    TestTrue(TEXT("Observed direct or known-helper live-world spawn calls"), SpawnTriggers > 0);

    int32 IndirectContractsFound = 0;
    for (const FRequiredGuardedRunTest& Contract : IndirectEditorWorldSpawnTests)
    {
        const TArray<FFunctionBody>* Bodies = BodiesByFile.Find(Contract.RelativePath);
        const FFunctionBody* Body = Bodies ? FindBodyByName(*Bodies, Contract.FunctionName) : nullptr;
        if (!Body)
        {
            AddError(FString::Printf(TEXT("Missing indirect-spawn contract body %s::%s::RunTest"),
                Contract.RelativePath, Contract.FunctionName));
            continue;
        }
        ++IndirectContractsFound;
        TArray<int32> HandlerCalls = FindPatternPositions(
            Body->Text, TEXT("\\bInvokeHandler(?:With(?:Shared)?Capture)?\\s*\\("));
        HandlerCalls.Append(FindPatternPositions(
            Body->Text,
            TEXT("\\bInvokeEnvironmentSpawnAndValidateResponse\\s*<[^>]+>\\s*\\(")));
        HandlerCalls.Sort();
        if (HandlerCalls.IsEmpty())
        {
            AddError(FString::Printf(
                TEXT("%s:%d %s::RunTest no longer contains its known actor-spawning handler "
                     "invocation; update the ownership contract."),
                Contract.RelativePath, Body->StartLine, Contract.FunctionName));
            continue;
        }
        for (const int32 HandlerCall : HandlerCalls)
        {
            if (!HasActiveGuardBefore(Body->Text, HandlerCall))
            {
                AddError(FString::Printf(
                    TEXT("%s:%d %s::RunTest invokes a known actor-spawning handler before an "
                         "active FScopedEditorWorldActorGuard declaration."),
                    Contract.RelativePath,
                    Body->StartLine + static_cast<int32>(Algo::Count(Body->Text.Left(HandlerCall), TEXT('\n'))),
                    Contract.FunctionName));
            }
        }
    }
    TestEqual(TEXT("Found every known indirect live-world spawn contract"),
        IndirectContractsFound, static_cast<int32>(UE_ARRAY_COUNT(IndirectEditorWorldSpawnTests)));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNoLabelBasedPreDeleteTest,
    "PinWright.infra.contract.EditorWorldSpawn.NoLabelBasedPreDelete",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNoLabelBasedPreDeleteTest::RunTest(const FString& Parameters)
{
    using namespace FixtureOwnershipContractHelpers;

    const FString SourceRoot = ResolveSourceRoot();
    if (!TestFalse(TEXT("Resolved the plugin Source root"), SourceRoot.IsEmpty()))
    {
        return false;
    }

    const FString TestsRoot = SourceRoot / TEXT("PinWright/Private/Tests");
    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *TestsRoot, TEXT("*.cpp"), true, false, false);
    IFileManager::Get().FindFilesRecursive(Files, *TestsRoot, TEXT("*.h"), true, false, false);
    if (!TestTrue(TEXT("Found PinWright C++ test sources"), Files.Num() > 0))
    {
        return false;
    }

    int32 FilesRead = 0;
    int32 HandlerBodiesScanned = 0;
    for (const FString& File : Files)
    {
        FString Contents;
        if (!ReadNeutralizedSource(*this, File, Contents))
        {
            continue;
        }
        ++FilesRead;

        const FString Relative = MakeRelative(File, SourceRoot);
        if (Contents.Contains(TEXT("DestroyEnvironmentSpawnTestActors")))
        {
            AddError(FString::Printf(
                TEXT("%s reintroduces the label-wide environment actor destroy helper; "
                     "use FScopedEditorWorldActorGuard and a GUID-suffixed fixture label."),
                *Relative));
        }

        TArray<FFunctionBody> Bodies;
        if (!ExtractRunTestBodies(*this, Relative, Contents, Bodies))
        {
            continue;
        }
        for (const FFunctionBody& Body : Bodies)
        {
            TArray<int32> HandlerCalls = FindPatternPositions(
                Body.Text, TEXT("\\bInvokeHandler(?:With(?:Shared)?Capture)?\\s*\\("));
            HandlerCalls.Append(FindPatternPositions(
                Body.Text,
                TEXT("\\bInvokeEnvironmentSpawnAndValidateResponse\\s*<[^>]+>\\s*\\(")));
            HandlerCalls.Sort();
            if (HandlerCalls.IsEmpty())
            {
                continue;
            }
            ++HandlerBodiesScanned;

            const TArray<int32> LabelDestroyCalls = FindPatternPositions(
                Body.Text,
                TEXT("\\b(?:Destroy|Delete|Remove)[A-Za-z0-9_]*(?:Label|Name)"
                     "[A-Za-z0-9_]*\\s*\\("));
            const TArray<int32> LabelReads = FindPatternPositions(
                Body.Text, TEXT("\\bGetActorLabel\\s*\\("));
            const TArray<int32> DirectDestroyCalls = FindPatternPositions(
                Body.Text, TEXT("(?:->\\s*Destroy\\s*\\(|\\bEditorDestroyActor\\s*\\()"));
            const bool bIsNewWorldFixture =
                (Relative == TEXT("PinWright/Private/Tests/World/TestVolumeHandlers.cpp") &&
                    Body.Name == TEXT("FVolumeCreateBlockingVolumeBrushGeometryTest")) ||
                (Relative == TEXT("PinWright/Private/Tests/World/TestWaterHandlers.cpp") &&
                    (Body.Name == TEXT("FWaterSpawnWaterBodyValidParamsTest") ||
                        Body.Name == TEXT("FWaterUnderwaterSettingsSilentDropTest")));

            bool bReportedDirectPreDelete = false;
            for (const int32 HandlerCall : HandlerCalls)
            {
                for (const int32 LabelDestroyCall : LabelDestroyCalls)
                {
                    if (LabelDestroyCall < HandlerCall)
                    {
                        AddError(FString::Printf(
                            TEXT("%s:%d %s::RunTest performs label-based actor deletion before "
                                 "a handler call; use scoped fixture ownership."),
                            *Relative,
                            Body.StartLine + static_cast<int32>(
                                Algo::Count(Body.Text.Left(LabelDestroyCall), TEXT('\n'))),
                            *Body.Name));
                    }
                }

                for (const int32 DestroyCall : DirectDestroyCalls)
                {
                    if (DestroyCall >= HandlerCall)
                    {
                        continue;
                    }
                    const bool bLabelSelectedBeforeDelete = LabelReads.ContainsByPredicate(
                        [DestroyCall](const int32 LabelRead) { return LabelRead < DestroyCall; });
                    if (bLabelSelectedBeforeDelete)
                    {
                        AddError(FString::Printf(
                            TEXT("%s:%d %s::RunTest selects an actor by display label and "
                                 "deletes it before a handler call; use scoped fixture ownership."),
                            *Relative,
                            Body.StartLine + static_cast<int32>(
                                Algo::Count(Body.Text.Left(DestroyCall), TEXT('\n'))),
                            *Body.Name));
                        bReportedDirectPreDelete = true;
                        break;
                    }
                }
                if (!bReportedDirectPreDelete && bIsNewWorldFixture)
                {
                    for (const int32 DestroyCall : DirectDestroyCalls)
                    {
                        if (DestroyCall <= HandlerCall)
                        {
                            continue;
                        }
                        const bool bLabelReadBetweenHandlerAndDelete = LabelReads.ContainsByPredicate(
                            [HandlerCall, DestroyCall](const int32 LabelRead)
                            {
                                return LabelRead > HandlerCall && LabelRead < DestroyCall;
                            });
                        if (bLabelReadBetweenHandlerAndDelete)
                        {
                            AddError(FString::Printf(
                                TEXT("%s:%d %s::RunTest selects an actor by display label and "
                                     "deletes it after a handler call; use the handler's canonical "
                                     "actor path and scoped fixture ownership."),
                                *Relative,
                                Body.StartLine + static_cast<int32>(
                                    Algo::Count(Body.Text.Left(DestroyCall), TEXT('\n'))),
                                *Body.Name));
                            bReportedDirectPreDelete = true;
                            break;
                        }
                    }
                }
                if (bReportedDirectPreDelete)
                {
                    break;
                }
            }
        }
    }

    TestEqual(TEXT("Read every discovered PinWright C++ test source"), FilesRead, Files.Num());
    TestTrue(TEXT("Scanned handler-driving test bodies for label-based actor deletion"),
        HandlerBodiesScanned > 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRootedFixtureOwnershipContractTest,
    "PinWright.infra.contract.AddToRoot.ScopedFixtureOwnership",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FRootedFixtureOwnershipContractTest::RunTest(const FString& Parameters)
{
    using namespace FixtureOwnershipContractHelpers;

    const FString SourceRoot = ResolveSourceRoot();
    if (!TestFalse(TEXT("Resolved the plugin Source root"), SourceRoot.IsEmpty()))
    {
        return false;
    }

    const FString TestsRoot = SourceRoot / TEXT("PinWright/Private/Tests");
    if (!TestTrue(TEXT("Main-module Tests tree exists"),
            IFileManager::Get().DirectoryExists(*TestsRoot)))
    {
        return false;
    }

    TArray<FString> AllTestSources;
    IFileManager::Get().FindFilesRecursive(
        AllTestSources, *TestsRoot, TEXT("*.cpp"), true, false, false);
    IFileManager::Get().FindFilesRecursive(
        AllTestSources, *TestsRoot, TEXT("*.h"), true, false, false);
    if (!TestTrue(TEXT("Recursive ownership walk found C++ test sources"),
            AllTestSources.Num() > 0))
    {
        return false;
    }

    TMap<FString, FString> ContractSource;
    int32 FilesRead = 0;
    for (const FString& File : AllTestSources)
    {
        FString Contents;
        if (!ReadNeutralizedSource(*this, File, Contents))
        {
            continue;
        }
        ContractSource.Add(MakeRelative(File, SourceRoot), MoveTemp(Contents));
        ++FilesRead;
    }
    TestEqual(TEXT("Read every discovered C++ test source"), FilesRead, AllTestSources.Num());

    const auto CheckExactBaseline = [this, &ContractSource](
        const TCHAR* const* Entries,
        int32 EntryCount,
        const TCHAR* Pattern,
        const TCHAR* Label)
    {
        const TMap<FString, int32> Expected = ParseCountBaseline(
            *this, Entries, EntryCount, Label);
        TMap<FString, int32> Observed;
        for (const TPair<FString, FString>& Source : ContractSource)
        {
            const int32 Count = CountPattern(Source.Value, Pattern);
            if (Count > 0)
            {
                Observed.Add(Source.Key, Count);
                if (!Expected.Contains(Source.Key))
                {
                    AddError(FString::Printf(
                        TEXT("%s has %d unreviewed %s occurrence(s); add scoped ownership or "
                             "an evidence-backed allow-list entry."),
                        *Source.Key, Count, Label));
                }
            }
        }
        for (const TPair<FString, int32>& Entry : Expected)
        {
            if (!ContractSource.Contains(Entry.Key))
            {
                AddError(FString::Printf(
                    TEXT("Expected %s source is missing or unreadable: %s"), Label, *Entry.Key));
                continue;
            }
            TestEqual(FString::Printf(TEXT("%s %s count"), *Entry.Key, Label),
                Observed.FindRef(Entry.Key), Entry.Value);
        }
    };

    CheckExactBaseline(RawAddToRootBaseline, UE_ARRAY_COUNT(RawAddToRootBaseline),
        TEXT("\\bAddToRoot\\s*\\("), TEXT("raw AddToRoot"));
    CheckExactBaseline(RootedProducerBaseline, UE_ARRAY_COUNT(RootedProducerBaseline),
        TEXT("\\b(?:BuildScriptHostingGraph|BuildEmptySystemWithEmitter|"
             "NewTransientAnimSequence|NewTransientSkeletonWithBones)\\s*\\("),
        TEXT("rooted producer"));

    const TMap<FString, int32> LegacyNiagaraBaseline = ParseCountBaseline(
        *this, LegacyNiagaraProducerCallers, UE_ARRAY_COUNT(LegacyNiagaraProducerCallers),
        TEXT("legacy Niagara producer caller"));
    TMap<FString, int32> ObservedLegacyNiagara;
    int32 NiagaraProducerCalls = 0;
    int32 AnimationFactoryCalls = 0;
    for (const TPair<FString, FString>& Source : ContractSource)
    {
        TArray<FFunctionBody> Bodies;
        if (!ExtractRunTestBodies(*this, Source.Key, Source.Value, Bodies))
        {
            continue;
        }
        for (const FFunctionBody& Body : Bodies)
        {
            CheckNiagaraCallerBody(*this, Source.Key, Body, LegacyNiagaraBaseline,
                ObservedLegacyNiagara, NiagaraProducerCalls);
            AnimationFactoryCalls += CheckAnimationFactoryCallers(*this, Source.Key, Body);
        }
    }
    TestTrue(TEXT("Observed rooted Niagara fixture producer calls"), NiagaraProducerCalls > 0);
    TestTrue(TEXT("Observed animation fixture factory calls"), AnimationFactoryCalls > 0);
    for (const TPair<FString, int32>& Entry : LegacyNiagaraBaseline)
    {
        TestEqual(FString::Printf(TEXT("Legacy Niagara ownership caller still matches: %s"),
            *Entry.Key), ObservedLegacyNiagara.FindRef(Entry.Key), Entry.Value);
    }

    const FString* NiagaraUtils = ContractSource.Find(
        TEXT("PinWright/Private/Tests/Assets/NiagaraEditTestUtils.h"));
    if (NiagaraUtils)
    {
        TestEqual(TEXT("Niagara root transfer is limited to its two named factories"),
            CountPattern(*NiagaraUtils, TEXT("\\bAddToRoot\\s*\\(")), 2);
        TestTrue(TEXT("FAuthorableSystemRoots is non-copyable"),
            NiagaraUtils->Contains(TEXT("FAuthorableSystemRoots(const FAuthorableSystemRoots&) = delete")) &&
            NiagaraUtils->Contains(TEXT("operator=(const FAuthorableSystemRoots&) = delete")));
        TestTrue(TEXT("FAuthorableSystemRoots releases system and emitter roots"),
            CountPattern(*NiagaraUtils, TEXT("\\bRemoveFromRoot\\s*\\(")) >= 2);
    }

    const FString* NirFixtures = ContractSource.Find(
        TEXT("PinWright/Private/Tests/Niagara/TestNIRFixtures.cpp"));
    if (NirFixtures)
    {
        TestTrue(TEXT("BuildEmptySystemWithEmitter cleans the rooted system if emitter creation fails"),
            CountPattern(*NirFixtures,
                TEXT("if\\s*\\(\\s*!Emitter\\s*\\)\\s*\\{\\s*"
                     "DestroyFixture\\s*\\(\\s*System\\s*\\)\\s*;\\s*"
                     "return\\s+nullptr\\s*;")) == 1);
    }

    const FString* AnimationFactories = ContractSource.Find(
        TEXT("PinWright/Private/Tests/Assets/AnimAuthoringTestFixtures.h"));
    if (AnimationFactories)
    {
        TestEqual(TEXT("Animation root transfer is limited to its two named factories"),
            CountPattern(*AnimationFactories, TEXT("\\bAddToRoot\\s*\\(")), 2);
        TestTrue(TEXT("Animation root owner is non-copyable and movable"),
            AnimationFactories->Contains(TEXT("FScopedAnimAssetRoot(const FScopedAnimAssetRoot&) = delete")) &&
            AnimationFactories->Contains(TEXT("operator=(const FScopedAnimAssetRoot&) = delete")) &&
            AnimationFactories->Contains(TEXT("FScopedAnimAssetRoot(FScopedAnimAssetRoot&& Other) noexcept")));
        TestTrue(TEXT("Animation root owner releases its asset"),
            CountPattern(*AnimationFactories, TEXT("\\bRemoveFromRoot\\s*\\(")) >= 1);
    }
    return true;
}
