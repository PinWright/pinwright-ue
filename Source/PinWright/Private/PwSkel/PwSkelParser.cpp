// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "PwSkel/PwSkelParser.h"

#include "PwSource/PwParseCursor.h"
#include "PwSource/PwSuggest.h"
#include "PwSource/PwToken.h"
#include "PwSource/PwTokenizer.h"
#include "PwSource/PwValueRead.h"

namespace
{
    const TCHAR* PwSkelParser_AcceptedVersions()
    {
        return TEXT("0");
    }

    bool PwSkelParser_IsAcceptedVersion(int32 Version)
    {
        return Version == 0;
    }

    FPwParamSpec PwSkelParser_MakeVector3Param(const TCHAR* Name, const TCHAR* Default,
                                               const TCHAR* Description)
    {
        FPwParamSpec Spec;
        Spec.Name = Name;
        Spec.Type = EPwParamType::Vector3;
        Spec.Default = Default;
        Spec.Description = Description;
        return Spec;
    }

    TArray<FPwParamSpec> PwSkelParser_BuildBoneHeaderParams()
    {
        TArray<FPwParamSpec> Params;
        Params.Add(PwSkelParser_MakeVector3Param(TEXT("at"), TEXT("(0, 0, 0)"),
            TEXT("Bone-local translation in the parent's frame.")));
        Params.Add(PwSkelParser_MakeVector3Param(TEXT("rotate"), TEXT("(0, 0, 0)"),
            TEXT("Bone-local rotation in roll, pitch, yaw degrees.")));
        Params.Add(PwSkelParser_MakeVector3Param(TEXT("scale"), TEXT("(1, 1, 1)"),
            TEXT("Bone-local per-axis scale.")));

        FPwParamSpec Retarget;
        Retarget.Name = TEXT("retarget");
        Retarget.Type = EPwParamType::Enum;
        Retarget.Default = TEXT("animation");
        Retarget.Description = TEXT("Translation retargeting mode stored on this bone.");
        Retarget.AllowedValues = {
            TEXT("animation"), TEXT("skeleton"), TEXT("animation_scaled"),
            TEXT("animation_relative"), TEXT("orient_and_scale")
        };
        Params.Add(MoveTemp(Retarget));
        return Params;
    }

    TArray<FPwParamSpec> PwSkelParser_BuildPreviewMeshParams()
    {
        FPwParamSpec Path;
        Path.Name = TEXT("path");
        Path.Type = EPwParamType::String;
        Path.bRequired = true;
        Path.Description = TEXT("USkeletalMesh asset path used by the skeleton preview viewport.");
        TArray<FPwParamSpec> Params;
        Params.Add(MoveTemp(Path));
        return Params;
    }

    TArray<FPwParamSpec> PwSkelParser_BuildCurveHeaderParams()
    {
        TArray<FPwParamSpec> Params;

        FPwParamSpec Material;
        Material.Name = TEXT("material");
        Material.Type = EPwParamType::Bool;
        Material.Default = TEXT("false");
        Material.Description = TEXT("Treat this curve as a material parameter curve.");
        Params.Add(MoveTemp(Material));

        FPwParamSpec MorphTarget;
        MorphTarget.Name = TEXT("morph_target");
        MorphTarget.Type = EPwParamType::Bool;
        MorphTarget.Default = TEXT("false");
        MorphTarget.Description = TEXT("Treat this curve as a morph-target curve.");
        Params.Add(MoveTemp(MorphTarget));

        FPwParamSpec MaxLod;
        MaxLod.Name = TEXT("max_lod");
        MaxLod.Type = EPwParamType::Integer;
        MaxLod.Default = TEXT("-1");
        MaxLod.Description = TEXT("Highest LOD that evaluates the curve; -1 means every LOD.");
        MaxLod.bHasRange = true;
        MaxLod.MinValue = -1.0;
        MaxLod.MaxValue = 254.0;
        Params.Add(MoveTemp(MaxLod));
        return Params;
    }

    void PwSkelParser_AppendParams(TArray<FPwParamSpec>& Destination,
                                   TArrayView<const FPwParamSpec> Source)
    {
        for (const FPwParamSpec& Spec : Source)
        {
            Destination.Add(Spec);
        }
    }

    // Bone scale is the one header value whose *domain* can turn a document the parser
    // accepts into an asset nothing can pose, so it is checked beyond type and arity.
    //
    // Threshold provenance -- deliberately an engine constant, not a tuned one:
    // UE_KINDA_SMALL_NUMBER (1e-4) is the engine's own default "effectively zero" tolerance,
    // the value FMath::IsNearlyZero and FVector::IsNearlyZero use when a caller supplies
    // none. An axis at or under it places the whole subtree inside that tolerance of this
    // bone's origin, so the reference-pose transform is singular at the precision the engine
    // itself compares at; under UE_SMALL_NUMBER (1e-8) FTransform::GetSafeScaleReciprocal
    // substitutes a literal zero and the inverse bind transform is undefined outright.
    //
    // Testing exact zero only would pass scale=(0.00001, 1, 1), which collapses a limb just
    // as completely and reports nothing -- which is the defect this check exists to close.
    constexpr double PwSkelParser_DegenerateScaleTolerance = UE_KINDA_SMALL_NUMBER;

    // Constructs an author reasonably expects to find here and that this format deliberately
    // does not have. A refusal that only says "not valid" leaves them guessing; naming the
    // owning verb, and WHY the split exists, is the difference between one round trip and
    // several. Keyed on the bare keyword, matched before the generic unknown-construct path.
    const TCHAR* PwSkelParser_ElsewhereConstruct(const FString& Keyword)
    {
        if (Keyword == TEXT("socket"))
        {
            return TEXT("Sockets are not authorable in .pwskel. Add them with "
                        "skeleton.create_socket (or skeleton.create_socket on a mesh path for a "
                        "mesh-only socket) after the compile. A later source recompile refuses "
                        "to discard it unless overwrite=true explicitly permits the loss. See "
                        "docs/pwskel-format.md.");
        }
        if (Keyword == TEXT("virtual_bone") || Keyword == TEXT("virtualbone"))
        {
            return TEXT("Virtual bones are not authorable in .pwskel. Add them with "
                        "skeleton.create_virtual_bone after the compile; they span a PAIR of "
                        "bones and the engine mints their name. A later source recompile refuses "
                        "to discard it unless overwrite=true explicitly permits the loss. See "
                        "docs/pwskel-format.md.");
        }
        return nullptr;
    }

    const TCHAR* PwSkelParser_ScaleAxisName(int32 AxisIndex)
    {
        switch (AxisIndex)
        {
        case 0:  return TEXT("x");
        case 1:  return TEXT("y");
        default: return TEXT("z");
        }
    }
}

const FPwParamSpec* FPwSkelOpSpec::FindParam(const FString& ParamName) const
{
    return Params.FindByPredicate(
        [&ParamName](const FPwParamSpec& Candidate) { return Candidate.Name == ParamName; });
}

namespace PwSkelOpTable
{
    TArrayView<const FPwParamSpec> BoneHeaderParams()
    {
        static const TArray<FPwParamSpec> Params = PwSkelParser_BuildBoneHeaderParams();
        return Params;
    }

    TArrayView<const FPwParamSpec> PreviewMeshParams()
    {
        static const TArray<FPwParamSpec> Params = PwSkelParser_BuildPreviewMeshParams();
        return Params;
    }

    TArrayView<const FPwParamSpec> CurveHeaderParams()
    {
        static const TArray<FPwParamSpec> Params = PwSkelParser_BuildCurveHeaderParams();
        return Params;
    }

    const TArray<FPwSkelOpSpec>& Get()
    {
        static const TArray<FPwSkelOpSpec> Specs = []()
        {
            FPwSkelOpSpec Bone;
            Bone.Name = TEXT("bone");
            Bone.Context = TEXT("skeleton_or_bone");
            Bone.Description = TEXT("A bone-local node in the skeleton hierarchy.");
            Bone.bAcceptsBlock = true;
            Bone.bRequiresBlock = true;
            PwSkelParser_AppendParams(Bone.Params, BoneHeaderParams());

            TArray<FPwSkelOpSpec> Result;
            Result.Add(MoveTemp(Bone));

            FPwSkelOpSpec Preview;
            Preview.Name = TEXT("preview_mesh");
            Preview.Context = TEXT("skeleton");
            Preview.Description = TEXT("The skeletal mesh shown when this skeleton opens in a preview viewport.");
            PwSkelParser_AppendParams(Preview.Params, PreviewMeshParams());
            Result.Add(MoveTemp(Preview));

            FPwSkelOpSpec Curve;
            Curve.Name = TEXT("curve");
            Curve.Context = TEXT("skeleton");
            Curve.Description = TEXT("Authored curve metadata, including flags, LOD limit and linked bones.");
            Curve.bAcceptsBlock = true;
            Curve.bRequiresBlock = true;
            PwSkelParser_AppendParams(Curve.Params, CurveHeaderParams());
            Result.Add(MoveTemp(Curve));

            FPwSkelOpSpec LinkedBone;
            LinkedBone.Name = TEXT("linked_bone");
            LinkedBone.Context = TEXT("curve");
            LinkedBone.Description = TEXT("A bone linked to the containing curve metadata entry.");
            Result.Add(MoveTemp(LinkedBone));
            return Result;
        }();
        return Specs;
    }

    const FPwSkelOpSpec* Find(const FString& OpName)
    {
        return Get().FindByPredicate(
            [&OpName](const FPwSkelOpSpec& Candidate) { return Candidate.Name == OpName; });
    }

    TArray<FString> Names()
    {
        TArray<FString> Result;
        Result.Reserve(Get().Num());
        for (const FPwSkelOpSpec& Spec : Get())
        {
            Result.Add(Spec.Name);
        }
        return Result;
    }
}

namespace
{
    struct FPwSkelParserImpl final : FPwParseCursor
    {
        FPwSkelDocument& Document;
        TMap<FName, int32> BoneDeclarationLines;
        TMap<FName, int32> CurveDeclarationLines;
        FString CurrentBone;

        FPwSkelParserImpl(const TArray<FPwToken>& InTokens, FPwSkelDocument& InDocument,
                          TArray<FPwDiagnostic>& InDiagnostics)
            : FPwParseCursor(InTokens, InDiagnostics)
            , Document(InDocument)
        {
            SyncScope();
        }

        void SyncScope()
        {
            ScopeLabel = CurrentBone.IsEmpty() ? FString() : FString(TEXT("bone"));
            ScopeName = CurrentBone;
        }

        void ErrorScoped(const TCHAR* Code, const FPwToken& At, FString Message,
                         TArray<FString> Suggestions = {})
        {
            SyncScope();
            FPwParseCursor::Error(Code, At, MoveTemp(Message), MoveTemp(Suggestions));
        }

        void WarnScoped(const TCHAR* Code, const FPwToken& At, FString Message)
        {
            SyncScope();
            FPwParseCursor::Emit(EPwSeverity::Warning, Code, At, MoveTemp(Message));
        }

        void RestoreScope(const FString& PreviousBone)
        {
            CurrentBone = PreviousBone;
            SyncScope();
        }

        // ValidateParams covers name, type and arity. Scale is the only bone header value
        // with a domain those three cannot express: a zero or near-zero axis makes the
        // reference-pose transform singular, and a negative axis mirrors the frame of every
        // descendant. Both used to validate completely clean.
        void ValidateBoneScale(const FPwBone& Bone)
        {
            const FPwValue* Scale = Bone.Transform.Find(TEXT("scale"));
            if (!Scale || Scale->Type != EPwValueType::Tuple || Scale->Tuple.Num() < 3)
            {
                // Absent means the (1, 1, 1) default. A malformed tuple was already reported
                // by ValidateParams, and re-reporting it here would name the wrong defect.
                return;
            }

            FPwToken At;
            At.Line = Scale->Line;
            At.Column = Scale->Column;

            TArray<FString> Degenerate;
            TArray<FString> Mirrored;
            for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
            {
                const double Axis = Scale->Tuple[AxisIndex];
                const FString Described = FString::Printf(TEXT("%s=%g"),
                    PwSkelParser_ScaleAxisName(AxisIndex), Axis);

                // An axis that is both negative and vanishing is one mistake, not two: the
                // singular reading is the severe one, so it wins and the mirror is not
                // also reported for that axis.
                if (FMath::Abs(Axis) <= PwSkelParser_DegenerateScaleTolerance)
                {
                    Degenerate.Add(Described);
                }
                else if (Axis < 0.0)
                {
                    Mirrored.Add(Described);
                }
            }

            if (Degenerate.Num() > 0)
            {
                ErrorScoped(PwSkelDiagnosticCodes::PWSKEL_DEGENERATE_SCALE, At,
                    FString::Printf(
                        TEXT("Bone '%s' has a scale axis at or inside the zero tolerance %g (%s). ")
                        TEXT("The bone's reference-pose transform is singular, so every descendant ")
                        TEXT("collapses onto this bone's origin and geometry skinned through the ")
                        TEXT("chain has no volume. Use a small positive scale, or omit 'scale' to ")
                        TEXT("keep the (1, 1, 1) default."),
                        *Bone.Name, PwSkelParser_DegenerateScaleTolerance,
                        *FString::Join(Degenerate, TEXT(", "))));
            }

            if (Mirrored.Num() > 0)
            {
                WarnScoped(PwSkelDiagnosticCodes::PWSKEL_MIRRORED_SCALE, At,
                    FString::Printf(
                        TEXT("Bone '%s' has a negative scale axis (%s), which negates the ")
                        TEXT("determinant of the bone's frame: the handedness of this bone and of ")
                        TEXT("every descendant is flipped, and skinned geometry below it reads ")
                        TEXT("inside-out. This is a warning and not an error because a mirrored ")
                        TEXT("chain is a legitimate rig; if the mirror was not intended, negate the ")
                        TEXT("children's 'at' offsets instead."),
                        *Bone.Name, *FString::Join(Mirrored, TEXT(", "))));
            }
        }

        void ParseDocument()
        {
            CurrentBone.Reset();
            SyncScope();
            FPwParseCursor::ParseVersionHeader(TEXT("pwskel"), PwSkelParser_AcceptedVersions(),
                PwSkelParser_IsAcceptedVersion, Document.Header);

            // There is no skeleton/path wrapper. The format header already identifies the
            // document, so top-level declarations are bone statements directly.
            while (true)
            {
                SkipNewlines();
                if (AtEnd())
                {
                    break;
                }

                const int32 Before = Pos;
                ParseTopLevelConstruct();
                if (Pos == Before)
                {
                    Advance();
                }
            }

            ValidateDocument();
        }

        void ParseTopLevelConstruct()
        {
            CurrentBone.Reset();
            SyncScope();

            if (!Check(EPwTokenType::Identifier))
            {
                UnexpectedToken(TEXT("a top-level bone declaration: bone \"name\" { … }"));
                SkipUnknownConstruct();
                return;
            }

            if (CheckIdentifier(TEXT("bone")))
            {
                ParseBone(Document.Roots, /*bTopLevel=*/true);
                return;
            }

            if (CheckIdentifier(TEXT("preview_mesh")))
            {
                ParsePreviewMesh();
                return;
            }
            if (CheckIdentifier(TEXT("curve")))
            {
                ParseCurve();
                return;
            }

            const FPwToken Keyword = Peek();
            const TArray<FString> ValidNames = PwSkelOpTable::Names();
            TArray<FString> Suggestions;
            const FString Guess = PwSuggest::Closest(Keyword.Text, ValidNames);
            if (!Guess.IsEmpty())
            {
                Suggestions.Add(Guess);
            }

            FString Message;
            if (Keyword.Text == TEXT("skeleton"))
            {
                Message = TEXT("A .pwskel file has no 'skeleton' wrapper; declare bones directly as bone \"name\" { … }.");
            }
            else if (const TCHAR* Elsewhere = PwSkelParser_ElsewhereConstruct(Keyword.Text))
            {
                Message = Elsewhere;
                Suggestions.Reset();
            }
            else
            {
                Message = FString::Printf(
                    TEXT("Unknown .pwskel construct '%s'. Valid constructs: %s."),
                    *Keyword.Text, *FString::Join(ValidNames, TEXT(", ")));
            }

            ErrorScoped(PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN, Keyword, MoveTemp(Message),
                MoveTemp(Suggestions));
            SkipUnknownConstruct();
        }

        void ParsePreviewMesh()
        {
            const FPwToken Keyword = Advance();
            TMap<FString, FPwValue> Params;
            ParseParams(Params, TEXT("preview_mesh"));
            FPwParseCursor::ValidateParams(
                Keyword, TEXT("preview_mesh"), PwSkelOpTable::PreviewMeshParams(), Params);

            if (Document.PreviewMesh.IsSet())
            {
                ErrorScoped(PwSkelDiagnosticCodes::PWSKEL_DUPLICATE_PREVIEW_MESH, Keyword,
                    FString::Printf(TEXT("preview_mesh is declared more than once; the earlier declaration is on line %d."),
                        Document.PreviewMesh->Line));
                return;
            }

            FPwSkelPreviewMesh Preview;
            Preview.AssetPath = PwValueRead::GetString(Params, TEXT("path"));
            Preview.Line = Keyword.Line;
            Preview.Column = Keyword.Column;
            Document.PreviewMesh = MoveTemp(Preview);
        }

        void ParseCurve()
        {
            const FPwToken Keyword = Advance();
            FPwSkelCurveMetaData Curve;
            Curve.Line = Keyword.Line;
            Curve.Column = Keyword.Column;

            if (!Check(EPwTokenType::String))
            {
                UnexpectedToken(TEXT("a quoted curve metadata name"));
                SkipUnknownConstruct();
                return;
            }
            const FPwToken NameToken = Advance();
            Curve.Name = NameToken.Text;
            if (Curve.Name.IsEmpty())
            {
                ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, NameToken,
                    TEXT("A curve metadata name must not be empty."));
            }

            if (const int32* EarlierLine = CurveDeclarationLines.Find(FName(*Curve.Name)))
            {
                ErrorScoped(PwSkelDiagnosticCodes::PWSKEL_DUPLICATE_CURVE, Keyword,
                    FString::Printf(TEXT("Curve metadata '%s' is declared more than once; the earlier declaration is on line %d."),
                        *Curve.Name, *EarlierLine));
            }
            else if (!Curve.Name.IsEmpty())
            {
                CurveDeclarationLines.Add(FName(*Curve.Name), Curve.Line);
            }

            TMap<FString, FPwValue> Params;
            const FString Owner = FString::Printf(TEXT("curve \"%s\""), *Curve.Name);
            const int32 HeaderStart = Pos;
            FPwToken BraceToken;
            const bool bParamsParsed = ParseParams(Params, Owner);
            if (bParamsParsed && Check(EPwTokenType::OpenBrace))
            {
                BraceToken = Advance();
            }
            else if (bParamsParsed)
            {
                FPwParseCursor::ValidateParams(
                    Keyword, Owner, PwSkelOpTable::CurveHeaderParams(), Params);
                ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK, Keyword,
                    MissingBlockMessage(TEXT("curve")));
                SkipToNextLine();
                return;
            }
            else
            {
                int32 Scan = HeaderStart;
                while (Scan < Pos && Tokens[Scan].Type != EPwTokenType::OpenBrace)
                {
                    ++Scan;
                }
                if (Scan >= Pos)
                {
                    return;
                }
                BraceToken = Tokens[Scan];
                Pos = Scan + 1;
            }

            FPwParseCursor::ValidateParams(
                Keyword, Owner, PwSkelOpTable::CurveHeaderParams(), Params);
            Curve.bMaterial = PwValueRead::GetBool(Params, TEXT("material"), false);
            Curve.bMorphTarget = PwValueRead::GetBool(Params, TEXT("morph_target"), false);
            Curve.MaxLod = PwValueRead::GetInt(Params, TEXT("max_lod"), -1);

            TSet<FString> SeenLinks;
            FPwParseCursor::ForEachBlockEntry(BraceToken, [this, &Curve, &SeenLinks]()
            {
                if (!CheckIdentifier(TEXT("linked_bone")))
                {
                    UnexpectedToken(TEXT("linked_bone \"name\""));
                    SkipUnknownConstruct();
                    return;
                }

                const FPwToken LinkKeyword = Advance();
                if (!Check(EPwTokenType::String))
                {
                    UnexpectedToken(TEXT("a quoted linked bone name"));
                    SkipToNextLine();
                    return;
                }
                const FPwToken LinkName = Advance();
                if (LinkName.Text.IsEmpty() || SeenLinks.Contains(LinkName.Text))
                {
                    ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, LinkKeyword,
                        LinkName.Text.IsEmpty()
                            ? TEXT("A linked bone name must not be empty.")
                            : FString::Printf(TEXT("Curve metadata '%s' links bone '%s' more than once."),
                                *Curve.Name, *LinkName.Text));
                }
                else
                {
                    SeenLinks.Add(LinkName.Text);
                    Curve.LinkedBones.Add(LinkName.Text);
                }
                SkipToNextLine();
            });

            Document.Curves.Add(MoveTemp(Curve));
        }

        bool ParseBone(TArray<FPwBone>& Destination, bool bTopLevel)
        {
            const FPwToken Keyword = Advance(); // 'bone'
            FPwBone Bone;
            Bone.Line = Keyword.Line;
            Bone.Column = Keyword.Column;

            // Bone names are strings deliberately. Identifiers are ASCII-only and cannot
            // represent every imported rig name; the three source formats must agree here.
            if (!Check(EPwTokenType::String))
            {
                UnexpectedToken(TEXT("a quoted bone name"));
                SkipUnknownConstruct();
                return false;
            }

            const FPwToken NameToken = Advance();
            Bone.Name = NameToken.Text;
            if (Bone.Name.IsEmpty())
            {
                ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_VALUE, NameToken,
                    TEXT("A bone name must not be an empty string."));
            }

            if (bTopLevel && Document.Roots.Num() > 0)
            {
                const FPwBone& FirstRoot = Document.Roots[0];
                ErrorScoped(PwSkelDiagnosticCodes::PWSKEL_MULTIPLE_ROOTS, Keyword,
                    FString::Printf(
                        TEXT("A .pwskel file has more than one root bone: '%s' (line %d) and '%s' (line %d). "
                             "Nest the second bone under the first."),
                        *FirstRoot.Name, FirstRoot.Line, *Bone.Name, Bone.Line));
            }

            const FString PreviousBone = CurrentBone;
            CurrentBone = Bone.Name;
            SyncScope();

            if (!Bone.Name.IsEmpty() && !BoneDeclarationLines.Contains(FName(*Bone.Name)))
            {
                BoneDeclarationLines.Add(FName(*Bone.Name), Bone.Line);
            }
            else if (!Bone.Name.IsEmpty())
            {
                const int32 ExistingLine = BoneDeclarationLines.FindChecked(FName(*Bone.Name));
                ErrorScoped(PwSkelDiagnosticCodes::PWSKEL_DUPLICATE_BONE, Keyword,
                    FString::Printf(
                        TEXT("Bone '%s' is declared more than once; the earlier declaration is on line %d."),
                        *Bone.Name, ExistingLine));
            }

            const int32 HeaderStart = Pos;
            FPwToken BraceToken;
            const FString Owner = FString::Printf(TEXT("bone \"%s\""), *Bone.Name);
            const bool bParamsParsed = ParseParams(Bone.Transform, Owner);
            if (bParamsParsed)
            {
                if (!Check(EPwTokenType::OpenBrace))
                {
                    // Validate the header before bailing out. This early return used to skip the
                    // ValidateParams call below, so a bad 'at=' on a bone whose block was also
                    // wrong was reported only after the block was fixed -- two round trips for
                    // two errors the parser had already seen.
                    FPwParseCursor::ValidateParams(Keyword, Owner,
                        PwSkelOpTable::BoneHeaderParams(), Bone.Transform);
                    ValidateBoneScale(Bone);
                    ErrorScoped(PwSourceDiagnosticCodes::PWSRC_BAD_BLOCK, Keyword,
                        MissingBlockMessage(TEXT("bone")));
                    SkipToNextLine();
                    RestoreScope(PreviousBone);
                    return false;
                }
                BraceToken = Advance();
            }
            else
            {
                // ParseParams recovers by consuming the malformed header line. If that line
                // contained the body brace, rewind to it and parse the body once, preventing a
                // single bad transform value from cascading through the rest of the hierarchy.
                int32 Scan = HeaderStart;
                while (Scan < Pos && Tokens[Scan].Type != EPwTokenType::OpenBrace)
                {
                    ++Scan;
                }
                if (Scan >= Pos)
                {
                    RestoreScope(PreviousBone);
                    return false;
                }

                BraceToken = Tokens[Scan];
                Pos = Scan + 1;
            }

            FPwParseCursor::ValidateParams(Keyword, Owner, PwSkelOpTable::BoneHeaderParams(),
                Bone.Transform);
            ValidateBoneScale(Bone);

            FPwParseCursor::ForEachBlockEntry(BraceToken, [this, &Bone]()
            {
                if (!CheckIdentifier(TEXT("bone")))
                {
                    if (Check(EPwTokenType::Identifier))
                    {
                        const FPwToken Keyword = Peek();
                        const TCHAR* Elsewhere =
                            PwSkelParser_ElsewhereConstruct(Keyword.Text);
                        ErrorScoped(PwSourceDiagnosticCodes::PWSRC_UNEXPECTED_TOKEN, Keyword,
                            Elsewhere
                                ? FString(Elsewhere)
                                : FString(TEXT("Only nested 'bone \"name\" { … }' declarations are valid inside a bone.")));
                    }
                    else
                    {
                        UnexpectedToken(TEXT("a nested bone declaration"));
                    }
                    SkipUnknownConstruct();
                    return;
                }

                ParseBone(Bone.Children, /*bTopLevel=*/false);
            });

            Destination.Add(MoveTemp(Bone));
            RestoreScope(PreviousBone);
            return true;
        }

        void ValidateDocument()
        {
            if (Document.Roots.Num() == 0)
            {
                FPwToken At;
                At.Line = Document.Header.VersionLine > 0 ? Document.Header.VersionLine : 1;
                At.Column = Document.Header.VersionColumn > 0 ? Document.Header.VersionColumn : 1;
                ErrorScoped(PwSkelDiagnosticCodes::PWSKEL_NO_BONES, At,
                    TEXT("A .pwskel document declares at least one top-level bone; there is no skeleton hierarchy to build."));
            }

            for (const FPwSkelCurveMetaData& Curve : Document.Curves)
            {
                for (const FString& LinkedBone : Curve.LinkedBones)
                {
                    if (!BoneDeclarationLines.Contains(FName(*LinkedBone)))
                    {
                        FPwToken At;
                        At.Line = Curve.Line;
                        At.Column = Curve.Column;
                        ErrorScoped(PwSkelDiagnosticCodes::PWSKEL_UNKNOWN_LINKED_BONE, At,
                            FString::Printf(TEXT("Curve metadata '%s' links unknown bone '%s'. Declare the bone in this source or remove the link."),
                                *Curve.Name, *LinkedBone));
                    }
                }
            }
        }
    };
}

bool FPwSkelParser::Parse(FStringView Source, FPwSkelDocument& OutDocument,
                          TArray<FPwDiagnostic>& OutDiagnostics)
{
    OutDocument = FPwSkelDocument();
    OutDiagnostics.Reset();

    TArray<FPwToken> Tokens;
    TArray<FPwDiagnostic> LexDiagnostics;
    FPwTokenizer::Tokenize(Source, Tokens, LexDiagnostics);
    OutDiagnostics.Append(MoveTemp(LexDiagnostics));

    FPwSkelParserImpl Impl(Tokens, OutDocument, OutDiagnostics);
    Impl.ParseDocument();

    return !PwDiagnosticsHaveError(OutDiagnostics);
}
