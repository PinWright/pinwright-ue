// Copyright (c) 2026 Alexander Penkin. MIT License.

// Tests for the capture-subject resolver core: the wire parse, the provider registry's release
// discipline, and the widget-type verification that replaced an unchecked downcast.
//
// WHAT THE WIDGET TESTS ARE ACTUALLY DEFENDING. Four shipped copies of the asset-editor walk
// selected a viewport with
//
//     if (Type == TEXT("SEditorViewport") || Type.EndsWith(TEXT("EditorViewport")))
//         StaticCastSharedRef<SEditorViewport>(Widget);
//
// SEditorViewport carries no SLATE_DECLARE_WIDGET, so the suffix IS the whole test and the cast has
// nothing behind it. Stock UE 5.8 ships five widget types whose names end in "EditorViewport" and
// which derive from SCompoundWidget rather than SEditorViewport - STextureEditorViewport,
// SFontEditorViewport, SCurveEditorViewport, SMediaPlayerEditorViewport, SSimulcamEditorViewport -
// so walking a Texture editor's window casts one of them and reads a viewport client out of
// unrelated memory. Those five names are asserted below as NOT accepted, by name, because they are
// the concrete instances of the defect rather than an imagined one.
//
// The counterfactual that keeps the fixture honest is the precondition assertion in
// UnknownWidgetTypeIsRefusedNotCast: the fake widget's type name is asserted to END IN
// "EditorViewport" first, so the test proves the new predicate refuses something the old one would
// have cast. A fixture named "SNotAViewport" would pass trivially and prove nothing.
//
// None of these tests needs a GPU, a lit preview scene or a live viewport, which is deliberate:
// every capture test that does take a conditional-skip path under -unattended reports success
// without running its assertions (board ticket B-test-skips-assertions-silently). The one test here
// that touches a live viewport says so in its name and reports a loud skip when there is none.

#include "Misc/AutomationTest.h"

#include "Handlers/ErrorCodes.h"
#include "Handlers/Render/CaptureSubject.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "SEditorViewport.h"
#include "Templates/UnrealTypeTraits.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWidget.h"
#include "Tests/TestSkipReporting.h"

#if WITH_DEV_AUTOMATION_TESTS

// File-unique NAMED namespace, never anonymous: Unity merges these translation units and an
// anonymous helper here would collide with a same-named one in a sibling Tests/Render/*.cpp.
namespace PinWrightCaptureSubjectResolverTest
{
    using namespace PinWrightCaptureSubject;

    // ---- the trap fixture ----
    //
    // A widget whose type name ends in "EditorViewport" and which is NOT an SEditorViewport. This
    // is the shape of the five stock engine widgets listed at the top of the file, reproduced here
    // so the refusal can be exercised without opening a Texture editor.
    //
    // The name is prefixed rather than spelled "SFakeEditorViewport" for the same Unity-merge
    // reason as the namespace; what the test depends on is the SUFFIX, which is asserted as a
    // precondition rather than assumed.
    class SPinWrightFakeEditorViewport : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SPinWrightFakeEditorViewport) {}
        SLATE_END_ARGS()

        void Construct(const FArguments& InArgs)
        {
            Canary = ExpectedCanary;
        }

        // A value at the offset an SEditorViewport would keep member state in. Asserted unchanged
        // after the walk: the point of the test is that nothing reinterpreted this object.
        static constexpr uint64 ExpectedCanary = 0x5049'4E57'5249'4748ull;
        uint64 Canary = 0;
    };

    // The compile-time half of "the fixture cannot pass by accident". The plan's *unable to fail
    // if* clause for this test is "the fixture derived from SEditorViewport"; this makes that a
    // build error rather than something a reader has to check.
    static_assert(!TIsDerivedFrom<SPinWrightFakeEditorViewport, SEditorViewport>::Value,
        "The unknown-widget fixture must NOT derive from SEditorViewport, or the test proves nothing.");
    static_assert(TIsDerivedFrom<SPinWrightFakeEditorViewport, SWidget>::Value,
        "The fixture must still be a widget the tree walk will visit.");

    // Widget type names that stock UE 5.8 really does ship, that really do end in
    // "EditorViewport", and that really are NOT SEditorViewports. Each is the engine header that
    // says so. The allow-list must reject every one of them.
    struct FKnownNonViewport
    {
        const TCHAR* TypeName;
        const TCHAR* Evidence;
    };
    inline TArray<FKnownNonViewport> KnownNonViewportTypes()
    {
        return {
            { TEXT("STextureEditorViewport"),
              TEXT("Editor/TextureEditor/Private/Widgets/STextureEditorViewport.h:19 (: public SCompoundWidget)") },
            { TEXT("SFontEditorViewport"),
              TEXT("Editor/FontEditor/Private/SFontEditorViewport.h:23 (: public SCompoundWidget)") },
            { TEXT("SCurveEditorViewport"),
              TEXT("Editor/DistCurveEditor/Private/SCurveEditorViewport.h:23 (: public SCompoundWidget)") },
            { TEXT("SMediaPlayerEditorViewport"),
              TEXT("Plugins/Media/MediaPlayerEditor/.../SMediaPlayerEditorViewport.h:17 (: public SCompoundWidget)") },
            { TEXT("SSimulcamEditorViewport"),
              TEXT("Plugins/VirtualProduction/CameraCalibration/.../SSimulcamEditorViewport.h:15 (: public SCompoundWidget)") }
        };
    }

    // ---- provider fixture ----

    // A non-null FEditorViewportClient* that is never dereferenced, for tests that only ask
    // "is the subject still holding one?". Backed by a real static object so the pointer is a
    // valid address rather than a fabricated one; its type is irrelevant because nothing reads
    // through it, and comparing it against null is the only operation performed on it anywhere.
    inline FEditorViewportClient* SentinelViewportClient()
    {
        static uint8 Storage = 0;
        return reinterpret_cast<FEditorViewportClient*>(&Storage);
    }

    struct FProviderCallCounts
    {
        int32 AcquireCalls = 0;
        int32 ReleaseCalls = 0;
        // Set inside Acquire before it fails, standing in for "it had already opened an editor
        // window". The whole point of running Release on the error path is that this happened.
        bool bAcquireTookSomething = false;
    };

    // A provider whose Acquire fails AFTER taking something, or succeeds, on demand.
    inline FSubjectProvider MakeCountingProvider(ESubjectKind Kind,
        const TSharedRef<FProviderCallCounts>& Counts, bool bAcquireSucceeds,
        bool bSupplyTimeSetter = false)
    {
        FSubjectProvider Provider;
        Provider.Kind = Kind;
        Provider.Acquire = [Counts, bAcquireSucceeds, bSupplyTimeSetter](
            const FSubjectRequest& Request, FResolvedSubject& OutSubject,
            FSubjectTimeSetter& OutTimeSetter, FString& OutErrCode, FString& OutErrMsg)
        {
            ++Counts->AcquireCalls;
            // "Opened an editor" - the state Release exists to undo.
            Counts->bAcquireTookSomething = true;
            OutSubject.bEditorWasAlreadyOpen = false;
            OutSubject.CaptureSource = TEXT("testProviderPreview");
            OutSubject.BoundsSource = TEXT("assetBounds");
            OutSubject.BoundsOrigin = FVector(10.0, 20.0, 30.0);
            OutSubject.BoundsRadius = 42.0;
            if (bSupplyTimeSetter)
            {
                OutSubject.bTimeSupported = true;
                OutSubject.TimeEndSeconds = 2.0;
                OutTimeSetter = [](double, FString&, FString&) { return true; };
            }
            if (!bAcquireSucceeds)
            {
                OutErrCode = ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND;
                OutErrMsg = TEXT("test provider failed after taking something");
                return false;
            }
            return true;
        };
        Provider.Release = [Counts](FResolvedSubject& Subject)
        {
            ++Counts->ReleaseCalls;
            Counts->bAcquireTookSomething = false;
            Subject.bEditorClosed = true;
        };
        return Provider;
    }

    // ---- payload helpers ----

    inline TSharedPtr<FJsonObject> MakeObject()
    {
        return MakeShared<FJsonObject>();
    }

    inline TSharedPtr<FJsonObject> MakeNestedSubjectPayload(const TSharedPtr<FJsonObject>& Subject)
    {
        TSharedPtr<FJsonObject> Payload = MakeObject();
        Payload->SetObjectField(TEXT("subject"), Subject);
        return Payload;
    }
}

// =================================================================================================
// 1. Ambiguity
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAmbiguousPayloadIsRefusedTest,
    "PinWright.render.capture_subject.AmbiguousPayloadIsRefused",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAmbiguousPayloadIsRefusedTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubject;
    using namespace PinWrightCaptureSubjectResolverTest;

    // ---- legacy top-level spellings ----
    {
        TSharedPtr<FJsonObject> Payload = MakeObject();
        Payload->SetStringField(TEXT("assetPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        Payload->SetStringField(TEXT("actorName"), TEXT("SomeActor"));

        FSubjectRequest Request;
        FString ErrCode;
        FString ErrMsg;
        const bool bParsed = ParseSubject(Payload, Request, ErrCode, ErrMsg);

        TestFalse(TEXT("{assetPath, actorName} is refused"), bParsed);
        TestEqual(TEXT("the refusal is INVALID_ARGUMENT"), ErrCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        // BOTH substrings, not just "parsing failed": a typo in a key name also fails to parse, and
        // asserting only the failure would let this test pass for the wrong reason.
        TestTrue(TEXT("the message names assetPath"), ErrMsg.Contains(TEXT("assetPath")));
        TestTrue(TEXT("the message names actorName"), ErrMsg.Contains(TEXT("actorName")));
        // The message promises `kind` as the way out, so it has to say so.
        TestTrue(TEXT("the message offers `kind` as the tie-break"), ErrMsg.Contains(TEXT("kind")));
    }

    // ---- the same clash inside the nested object ----
    {
        TSharedPtr<FJsonObject> Subject = MakeObject();
        Subject->SetStringField(TEXT("assetPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        Subject->SetStringField(TEXT("actorName"), TEXT("SomeActor"));

        FSubjectRequest Request;
        FString ErrCode;
        FString ErrMsg;
        const bool bParsed = ParseSubject(MakeNestedSubjectPayload(Subject), Request, ErrCode, ErrMsg);

        TestFalse(TEXT("subject:{assetPath, actorName} is refused"), bParsed);
        TestEqual(TEXT("nested refusal is INVALID_ARGUMENT"), ErrCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        TestTrue(TEXT("nested message names assetPath"), ErrMsg.Contains(TEXT("assetPath")));
        TestTrue(TEXT("nested message names actorName"), ErrMsg.Contains(TEXT("actorName")));
    }

    // ---- THE SHAPE THAT ACTUALLY HAPPENS: half the payload moved into `subject` ----
    //
    // A caller who adopts the new object and leaves the old key behind gets a payload with two
    // targets. Reading only the nested half would return a successful capture of something they
    // did not ask for, which is worse than any error. Every legacy target spelling is covered,
    // because the rule lives here rather than being written once per verb.
    {
        const TCHAR* const LegacyTargetKeys[] =
            { TEXT("assetPath"), TEXT("actorName"), TEXT("actor_name"), TEXT("actorPath"), TEXT("objectPath") };
        for (const TCHAR* LegacyKey : LegacyTargetKeys)
        {
            TSharedPtr<FJsonObject> Subject = MakeObject();
            Subject->SetStringField(TEXT("path"), TEXT("/Engine/BasicShapes/Cube.Cube"));
            TSharedPtr<FJsonObject> Payload = MakeNestedSubjectPayload(Subject);
            Payload->SetStringField(LegacyKey, TEXT("SomeActor"));

            FSubjectRequest Request;
            FString ErrCode;
            FString ErrMsg;
            TestFalse(*FString::Printf(TEXT("subject + top-level '%s' is refused"), LegacyKey),
                ParseSubject(Payload, Request, ErrCode, ErrMsg));
            TestEqual(*FString::Printf(TEXT("subject + '%s' is INVALID_ARGUMENT"), LegacyKey),
                ErrCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
            // Both names, not just "it failed": a typo in the nested key would also fail to parse.
            TestTrue(*FString::Printf(TEXT("the message names 'subject' (%s)"), LegacyKey),
                ErrMsg.Contains(TEXT("subject")));
            TestTrue(*FString::Printf(TEXT("the message names '%s'"), LegacyKey),
                ErrMsg.Contains(LegacyKey));
        }
    }
    {
        // A top-level `point` is a target too.
        TSharedPtr<FJsonObject> Subject = MakeObject();
        Subject->SetStringField(TEXT("name"), TEXT("SomeActor"));
        TSharedPtr<FJsonObject> Payload = MakeNestedSubjectPayload(Subject);
        TSharedPtr<FJsonObject> TopLevelPoint = MakeObject();
        TopLevelPoint->SetNumberField(TEXT("x"), 0.0);
        TopLevelPoint->SetNumberField(TEXT("y"), 0.0);
        TopLevelPoint->SetNumberField(TEXT("z"), 0.0);
        Payload->SetObjectField(TEXT("point"), TopLevelPoint);

        FSubjectRequest Request;
        FString ErrCode;
        FString ErrMsg;
        TestFalse(TEXT("subject + top-level point is refused"),
            ParseSubject(Payload, Request, ErrCode, ErrMsg));
        TestTrue(TEXT("the point clash names 'subject'"), ErrMsg.Contains(TEXT("subject")));
        TestTrue(TEXT("the point clash names 'point'"), ErrMsg.Contains(TEXT("point")));
    }
    {
        // FAILURE DIRECTION, and it is load-bearing: `radius` is a MODIFIER, not a target.
        // camera.orbit_shots has taken a top-level `radius` since it shipped, so refusing it beside
        // a `subject` would break a legitimate call shape to catch nothing. Without this assertion
        // a ParseSubject that refused every top-level key would pass the loop above.
        TSharedPtr<FJsonObject> Subject = MakeObject();
        Subject->SetStringField(TEXT("name"), TEXT("SomeActor"));
        TSharedPtr<FJsonObject> Payload = MakeNestedSubjectPayload(Subject);
        Payload->SetNumberField(TEXT("radius"), 4000.0);
        Payload->SetNumberField(TEXT("width"), 1024.0);
        Payload->SetStringField(TEXT("viewMode"), TEXT("unlit"));

        FSubjectRequest Request;
        FString ErrCode;
        FString ErrMsg;
        TestTrue(TEXT("subject + top-level radius/width/viewMode still parses"),
            ParseSubject(Payload, Request, ErrCode, ErrMsg));
        TestEqual(TEXT("the nested actor subject survives"),
            static_cast<int32>(Request.Kind), static_cast<int32>(ESubjectKind::Actor));
    }

    // ---- a point and an actor are also two different subjects ----
    {
        TSharedPtr<FJsonObject> Subject = MakeObject();
        TSharedPtr<FJsonObject> Point = MakeObject();
        Point->SetNumberField(TEXT("x"), 100.0);
        Point->SetNumberField(TEXT("y"), 200.0);
        Point->SetNumberField(TEXT("z"), 300.0);
        Subject->SetObjectField(TEXT("point"), Point);
        Subject->SetStringField(TEXT("name"), TEXT("SomeActor"));

        FSubjectRequest Request;
        FString ErrCode;
        FString ErrMsg;
        TestFalse(TEXT("subject:{point, name} is refused"),
            ParseSubject(MakeNestedSubjectPayload(Subject), Request, ErrCode, ErrMsg));
        TestTrue(TEXT("point/name message names point"), ErrMsg.Contains(TEXT("point")));
        TestTrue(TEXT("point/name message names name"), ErrMsg.Contains(TEXT("name")));
    }

    // ---- FAILURE DIRECTION: one key alone must NOT be refused ----
    //
    // Without these, a ParseSubject that refused everything would pass the assertions above.
    {
        TSharedPtr<FJsonObject> Payload = MakeObject();
        Payload->SetStringField(TEXT("actorName"), TEXT("SomeActor"));

        FSubjectRequest Request;
        FString ErrCode;
        FString ErrMsg;
        TestTrue(TEXT("{actorName} alone parses"), ParseSubject(Payload, Request, ErrCode, ErrMsg));
        TestEqual(TEXT("{actorName} infers the actor kind"),
            static_cast<int32>(Request.Kind), static_cast<int32>(ESubjectKind::Actor));
        TestEqual(TEXT("the actor name survives"), Request.ActorName, FString(TEXT("SomeActor")));
        TestTrue(TEXT("a subject was seen on the wire"), Request.bProvided);
        TestFalse(TEXT("the kind was inferred, not spelled"), Request.bKindProvided);
    }
    {
        TSharedPtr<FJsonObject> Subject = MakeObject();
        TSharedPtr<FJsonObject> Point = MakeObject();
        Point->SetNumberField(TEXT("x"), 1.0);
        Point->SetNumberField(TEXT("y"), 2.0);
        Point->SetNumberField(TEXT("z"), 3.0);
        Subject->SetObjectField(TEXT("point"), Point);
        Subject->SetNumberField(TEXT("radius"), 4000.0);

        FSubjectRequest Request;
        FString ErrCode;
        FString ErrMsg;
        TestTrue(TEXT("subject:{point, radius} parses"),
            ParseSubject(MakeNestedSubjectPayload(Subject), Request, ErrCode, ErrMsg));
        TestEqual(TEXT("a bare point is the world kind"),
            static_cast<int32>(Request.Kind), static_cast<int32>(ESubjectKind::World));
        // bPointProvided is what makes the point-without-radius refusal reachable at all: Point
        // defaults to the origin, so the value alone cannot say whether one was given.
        TestTrue(TEXT("the point is flagged as explicitly provided"), Request.bPointProvided);
        TestTrue(TEXT("the radius is flagged as explicitly provided"), Request.bRadiusProvided);
        TestEqual(TEXT("the radius survives"), Request.Radius, 4000.0f);
        TestEqual(TEXT("the point survives"), Request.Point, FVector(1.0, 2.0, 3.0));
    }

    // ---- an explicit kind really is the way out the message promises ----
    {
        TSharedPtr<FJsonObject> Subject = MakeObject();
        Subject->SetStringField(TEXT("kind"), TEXT("actor"));
        Subject->SetStringField(TEXT("assetPath"), TEXT("/Engine/BasicShapes/Cube.Cube"));
        Subject->SetStringField(TEXT("actorName"), TEXT("SomeActor"));

        FSubjectRequest Request;
        FString ErrCode;
        FString ErrMsg;
        TestTrue(TEXT("an explicit kind resolves the clash the message pointed at"),
            ParseSubject(MakeNestedSubjectPayload(Subject), Request, ErrCode, ErrMsg));
        TestEqual(TEXT("the explicit kind wins"),
            static_cast<int32>(Request.Kind), static_cast<int32>(ESubjectKind::Actor));
        TestTrue(TEXT("the kind is recorded as explicit"), Request.bKindProvided);
    }

    // ---- an unknown kind names what IS accepted ----
    {
        TSharedPtr<FJsonObject> Subject = MakeObject();
        Subject->SetStringField(TEXT("kind"), TEXT("landscape"));

        FSubjectRequest Request;
        FString ErrCode;
        FString ErrMsg;
        TestFalse(TEXT("an unknown kind is refused"),
            ParseSubject(MakeNestedSubjectPayload(Subject), Request, ErrCode, ErrMsg));
        TestEqual(TEXT("unknown kind is INVALID_ARGUMENT"), ErrCode, FString(ErrorCodes::ERR_INVALID_ARGUMENT));
        for (int32 Index = 0; Index < SubjectKindCount; ++Index)
        {
            const FString WireName = ToWireName(static_cast<ESubjectKind>(Index));
            TestTrue(*FString::Printf(TEXT("the refusal lists '%s'"), *WireName),
                ErrMsg.Contains(WireName));
        }
    }

    // ---- an empty payload is the level, not an error ----
    {
        FSubjectRequest Request;
        FString ErrCode;
        FString ErrMsg;
        TestTrue(TEXT("an empty payload parses"), ParseSubject(MakeObject(), Request, ErrCode, ErrMsg));
        TestEqual(TEXT("nothing named means the world kind"),
            static_cast<int32>(Request.Kind), static_cast<int32>(ESubjectKind::World));
        TestFalse(TEXT("...and bProvided says no subject was on the wire"), Request.bProvided);
    }

    // ---- closeAfterCapture is three-state, and the third state has to be visible ----
    {
        TSharedPtr<FJsonObject> Subject = MakeObject();
        Subject->SetStringField(TEXT("name"), TEXT("SomeActor"));

        FSubjectRequest Absent;
        FString ErrCode;
        FString ErrMsg;
        TestTrue(TEXT("subject without closeAfterCapture parses"),
            ParseSubject(MakeNestedSubjectPayload(Subject), Absent, ErrCode, ErrMsg));
        TestTrue(TEXT("closeAfterCapture defaults to true"), Absent.bCloseAfterCapture);
        TestFalse(TEXT("...but is not marked as explicitly provided"), Absent.bCloseAfterCaptureProvided);

        Subject->SetBoolField(TEXT("closeAfterCapture"), true);
        FSubjectRequest Explicit;
        TestTrue(TEXT("subject with an explicit closeAfterCapture parses"),
            ParseSubject(MakeNestedSubjectPayload(Subject), Explicit, ErrCode, ErrMsg));
        TestTrue(TEXT("explicit true reads true"), Explicit.bCloseAfterCapture);
        // Without this flag "close only what I opened" and "close it either way" are the same
        // payload, which is how the second capture of an asset silently stopped cleaning up.
        TestTrue(TEXT("explicit true is distinguishable from the default"),
            Explicit.bCloseAfterCaptureProvided);
    }

    return true;
}

// =================================================================================================
// 2. The unchecked downcast
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectUnknownWidgetTypeIsRefusedNotCastTest,
    "PinWright.render.capture_subject.UnknownWidgetTypeIsRefusedNotCast",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectUnknownWidgetTypeIsRefusedNotCastTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubject;
    using namespace PinWrightCaptureSubjectResolverTest;

    TSharedRef<SPinWrightFakeEditorViewport> Fake = SNew(SPinWrightFakeEditorViewport);
    const FString FakeTypeName = Fake->GetTypeAsString();

    // ---- preconditions: the fixture is the trap, not a straw man ----
    //
    // If the type name did not end in "EditorViewport" the old predicate would have refused it too
    // and this test would prove nothing about the change.
    TestTrue(*FString::Printf(TEXT("the fixture's type name ('%s') ends in EditorViewport, so the "
                                   "old suffix predicate WOULD have cast it"), *FakeTypeName),
        FakeTypeName.EndsWith(TEXT("EditorViewport")));
    TestEqual(TEXT("the canary starts intact"), Fake->Canary,
        SPinWrightFakeEditorViewport::ExpectedCanary);

    // ---- the new predicate refuses it by name ----
    TestFalse(TEXT("the fixture's type is not allow-listed"),
        IsAllowListedEditorViewportType(FakeTypeName));

    // Nested one level down, so the recursion is exercised rather than only the root test.
    TSharedRef<SWidget> Root = SNew(SBox)[Fake];

    const FEditorViewportSearch Search = FindEditorViewportInWidgetTree(Root);
    TestFalse(TEXT("no viewport is produced"), Search.Viewport.IsValid());
    TestFalse(TEXT("nothing claims the client registry verified it"), Search.bVerifiedByClientRegistry);
    TestFalse(TEXT("nothing claims the allow-list verified it"), Search.bVerifiedByAllowList);
    TestTrue(TEXT("the type is reported as unverified rather than skipped silently"),
        Search.UnverifiedTypeNames.Contains(FakeTypeName));

    // The object was never reinterpreted as an SEditorViewport. A cast followed by
    // GetViewportClient() would have read a TSharedPtr out of these bytes.
    TestEqual(TEXT("the fixture's memory was not reinterpreted"), Fake->Canary,
        SPinWrightFakeEditorViewport::ExpectedCanary);

    // ---- the typed refusal names the type ----
    TSharedPtr<SEditorViewport> Unused;
    FString ErrCode;
    FString ErrMsg;
    TestFalse(TEXT("the error overload also refuses"),
        FindEditorViewportInWidgetTree(Root, Unused, ErrCode, ErrMsg));
    TestEqual(TEXT("the refusal is PREVIEW_VIEWPORT_NOT_FOUND"),
        ErrCode, FString(ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND));
    TestTrue(TEXT("the refusal names the widget type it would not cast"),
        ErrMsg.Contains(FakeTypeName));
    TestFalse(TEXT("no viewport is handed back on the refusal path"), Unused.IsValid());

    return true;
}

// =================================================================================================
// 3. The allow-list
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectAllowListEntriesAreRealEditorViewportsTest,
    "PinWright.render.capture_subject.AllowListEntriesAreRealEditorViewports",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectAllowListEntriesAreRealEditorViewportsTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubject;
    using namespace PinWrightCaptureSubjectResolverTest;

    const TArrayView<const FEditorViewportTypeEntry> AllowList = GetEditorViewportTypeAllowList();
    TestTrue(TEXT("the allow-list is not empty"), AllowList.Num() > 0);

    // The static_assert(TIsDerivedFrom<T, SEditorViewport>::Value) for every entry whose type is
    // reachable from a public engine header lives beside the table in CaptureSubject.cpp, so THAT
    // half of this criterion is enforced at build time and cannot rot silently. What cannot be
    // checked that way is an entry whose widget class lives in a PRIVATE engine header - a plugin
    // cannot include SStaticMeshEditorViewport.h at all - so those entries carry a cited engine
    // header:line instead, and this test enforces that the citation exists and that the two kinds
    // of entry are labelled differently rather than blurred together.
    int32 CompileTimeVerifiedCount = 0;
    TSet<FString> SeenTypeNames;
    for (const FEditorViewportTypeEntry& Entry : AllowList)
    {
        const FString TypeName = Entry.TypeName ? FString(Entry.TypeName) : FString();
        TestFalse(TEXT("every entry has a type name"), TypeName.IsEmpty());
        TestFalse(*FString::Printf(TEXT("'%s' appears once"), *TypeName), SeenTypeNames.Contains(TypeName));
        SeenTypeNames.Add(TypeName);

        const FString Evidence = Entry.EngineEvidence ? FString(Entry.EngineEvidence) : FString();
        TestFalse(*FString::Printf(TEXT("'%s' cites engine evidence"), *TypeName), Evidence.IsEmpty());
        // A citation with no line number is not a citation anyone can re-check.
        TestTrue(*FString::Printf(TEXT("'%s' cites a header path"), *TypeName),
            Evidence.Contains(TEXT(".h:")));

        TestTrue(*FString::Printf(TEXT("'%s' is accepted by IsAllowListedEditorViewportType"), *TypeName),
            IsAllowListedEditorViewportType(TypeName));

        if (Entry.bCompileTimeVerified)
        {
            ++CompileTimeVerifiedCount;
        }
    }
    TestTrue(TEXT("at least one entry is compile-time verified, so the static_assert path is live"),
        CompileTimeVerifiedCount > 0);

    // The three types reachable from public UnrealEd / LevelEditor headers must be the
    // compile-verified ones; marking one of them human-verified would quietly weaken the strongest
    // check the list has.
    for (const TCHAR* PublicType : { TEXT("SEditorViewport"), TEXT("SAssetEditorViewport"), TEXT("SLevelViewport") })
    {
        const FEditorViewportTypeEntry* Found = AllowList.FindByPredicate(
            [PublicType](const FEditorViewportTypeEntry& Entry)
            {
                return Entry.TypeName && FCString::Strcmp(Entry.TypeName, PublicType) == 0;
            });
        TestNotNull(*FString::Printf(TEXT("'%s' is on the allow-list"), PublicType), Found);
        if (Found)
        {
            TestTrue(*FString::Printf(TEXT("'%s' is compile-time verified"), PublicType),
                Found->bCompileTimeVerified);
        }
    }

    // The three day-one leaf entries the plan names. SNiagaraSystemViewport is the one the SUFFIX
    // rule could never match - it does not end in "EditorViewport" - so its presence is what makes
    // a Niagara preview reachable at all, not a narrowing.
    for (const TCHAR* LeafType : { TEXT("SStaticMeshEditorViewport"), TEXT("SAnimationEditorViewport"),
                                   TEXT("SNiagaraSystemViewport") })
    {
        TestTrue(*FString::Printf(TEXT("'%s' is on the allow-list"), LeafType),
            IsAllowListedEditorViewportType(FString(LeafType)));
    }
    TestFalse(TEXT("SNiagaraSystemViewport does not end in 'EditorViewport', which is why the old "
                   "suffix rule refused it"),
        FString(TEXT("SNiagaraSystemViewport")).EndsWith(TEXT("EditorViewport")));

    // ---- FAILURE DIRECTION: the five real engine widgets the suffix rule would have cast ----
    for (const FKnownNonViewport& Known : KnownNonViewportTypes())
    {
        TestTrue(*FString::Printf(TEXT("'%s' ends in EditorViewport, so the old predicate matched it"),
                Known.TypeName),
            FString(Known.TypeName).EndsWith(TEXT("EditorViewport")));
        TestFalse(*FString::Printf(TEXT("'%s' is REFUSED by the allow-list (%s)"),
                Known.TypeName, Known.Evidence),
            IsAllowListedEditorViewportType(FString(Known.TypeName)));
    }

    // The match is exact, not a prefix or a case-folded compare.
    TestFalse(TEXT("a case variant is not accepted"),
        IsAllowListedEditorViewportType(TEXT("sniagarasystemviewport")));
    TestFalse(TEXT("a suffixed variant is not accepted"),
        IsAllowListedEditorViewportType(TEXT("SNiagaraSystemViewportToolbar")));
    TestFalse(TEXT("a prefixed variant is not accepted"),
        IsAllowListedEditorViewportType(TEXT("SMySEditorViewport")));

    return true;
}

// =================================================================================================
// 4. Release discipline
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectReleaseRunsOnTheErrorPathTest,
    "PinWright.render.capture_subject.ReleaseRunsOnTheErrorPath",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectReleaseRunsOnTheErrorPathTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubject;
    using namespace PinWrightCaptureSubjectResolverTest;

    FSubjectRequest Request;
    Request.Kind = ESubjectKind::StaticMesh;
    Request.AssetPath = TEXT("/Game/Test/Fixture.Fixture");

    // ---- the error path ----
    {
        TSharedRef<FProviderCallCounts> Counts = MakeShared<FProviderCallCounts>();
        const FSubjectProvider Provider =
            MakeCountingProvider(ESubjectKind::StaticMesh, Counts, /*bAcquireSucceeds=*/false);

        FString ErrCode;
        FString ErrMsg;
        {
            FResolvedSubject Resolved;
            FSubjectTimeSetter TimeSetter;
            const bool bResolved =
                ResolveWithProvider(Provider, Request, Resolved, TimeSetter, ErrCode, ErrMsg);

            TestFalse(TEXT("a failing Acquire fails the resolve"), bResolved);
            TestEqual(TEXT("Acquire ran once"), Counts->AcquireCalls, 1);
            // The point of the criterion: the provider had already taken something when it failed.
            TestEqual(TEXT("Release ran exactly once on the error path"), Counts->ReleaseCalls, 1);
            TestFalse(TEXT("...and it really put back what Acquire took"), Counts->bAcquireTookSomething);
            TestTrue(TEXT("the failure keeps the provider's own error code"),
                ErrCode == ErrorCodes::ERR_PREVIEW_VIEWPORT_NOT_FOUND);

            // A caller that keeps the failed subject and releases it again must not double-release.
            ReleaseSubject(Resolved);
            TestEqual(TEXT("an explicit release after a failed resolve is a no-op"),
                Counts->ReleaseCalls, 1);
        }
        // ...and neither must the destructor.
        TestEqual(TEXT("the destructor does not release a second time"), Counts->ReleaseCalls, 1);
    }

    // ---- FAILURE DIRECTION: a successful Acquire must NOT be released by Resolve ----
    //
    // Without this, a Resolve that released unconditionally would pass every assertion above while
    // tearing down the viewport before the capture ever ran.
    {
        TSharedRef<FProviderCallCounts> Counts = MakeShared<FProviderCallCounts>();
        const FSubjectProvider Provider =
            MakeCountingProvider(ESubjectKind::StaticMesh, Counts, /*bAcquireSucceeds=*/true);

        FString ErrCode;
        FString ErrMsg;
        {
            FResolvedSubject Resolved;
            FSubjectTimeSetter TimeSetter;
            TestTrue(TEXT("a succeeding Acquire resolves"),
                ResolveWithProvider(Provider, Request, Resolved, TimeSetter, ErrCode, ErrMsg));
            TestEqual(TEXT("Release has NOT run yet"), Counts->ReleaseCalls, 0);
            TestTrue(TEXT("the subject is still holding what Acquire took"),
                Counts->bAcquireTookSomething);
            TestEqual(TEXT("the resolve stamped the requested kind onto the subject"),
                static_cast<int32>(Resolved.Kind), static_cast<int32>(ESubjectKind::StaticMesh));
            TestEqual(TEXT("the resolve echoed the asset path"), Resolved.AssetPath, Request.AssetPath);

            // An explicit early release is what a verb uses when its response must report a
            // MEASURED assetEditorClosed rather than a predicted one.
            ReleaseSubject(Resolved);
            TestEqual(TEXT("the explicit release runs once"), Counts->ReleaseCalls, 1);
            TestTrue(TEXT("the release's measured outcome is readable afterwards"),
                Resolved.bEditorClosed);
            ReleaseSubject(Resolved);
            TestEqual(TEXT("a second explicit release is a no-op"), Counts->ReleaseCalls, 1);
        }
        TestEqual(TEXT("the destructor adds nothing after an explicit release"), Counts->ReleaseCalls, 1);
    }

    // ---- scope exit alone is enough ----
    //
    // A verb that forgets to release cannot leak an asset editor window, which is the shutdown
    // crash precondition in ~FStaticMeshEditor (docs/lessons.md:166).
    {
        TSharedRef<FProviderCallCounts> Counts = MakeShared<FProviderCallCounts>();
        const FSubjectProvider Provider =
            MakeCountingProvider(ESubjectKind::StaticMesh, Counts, /*bAcquireSucceeds=*/true);

        FString ErrCode;
        FString ErrMsg;
        {
            FResolvedSubject Resolved;
            FSubjectTimeSetter TimeSetter;
            TestTrue(TEXT("resolve succeeds"),
                ResolveWithProvider(Provider, Request, Resolved, TimeSetter, ErrCode, ErrMsg));
            TestEqual(TEXT("nothing released it inside the scope"), Counts->ReleaseCalls, 0);
        }
        TestEqual(TEXT("leaving the scope releases exactly once"), Counts->ReleaseCalls, 1);
    }

    // ---- a provider that is not registered is a typed refusal, not a crash ----
    {
        FSubjectRequest Unregistered;
        // Every kind that has a provider is registered from its own file; a kind with none must
        // still answer. Resolve names the kind and lists what IS registered.
        Unregistered.Kind = ESubjectKind::Niagara;
        FResolvedSubject Resolved;
        FSubjectTimeSetter TimeSetter;
        FString ErrCode;
        FString ErrMsg;
        if (FindProvider(ESubjectKind::Niagara) == nullptr)
        {
            TestFalse(TEXT("an unregistered kind refuses"),
                Resolve(Unregistered, Resolved, TimeSetter, ErrCode, ErrMsg));
            TestEqual(TEXT("the refusal is UNSUPPORTED_ASSET_EDITOR"),
                ErrCode, FString(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR));
            TestTrue(TEXT("the refusal names the kind"), ErrMsg.Contains(TEXT("niagara")));
        }
        else
        {
            // The Niagara provider landed, which is the intended end state; the branch above is
            // only reachable before it does. Said out loud rather than skipped silently.
            AddInfo(TEXT("A niagara provider is registered, so the unregistered-kind branch was not "
                         "exercised. This is the expected steady state once C5 lands."));
        }
    }

    return true;
}

// =================================================================================================
// 4b. The subject holds NO viewport reference by the time its provider closes the asset editor
//
// WHAT THIS CATCHES, AND WHY IT IS NOT A STYLE RULE. A provider's Release ends in CloseAssetEditor,
// and closing an asset editor destroys its preview SEditorViewport - whose destructor does
// check(SceneViewport.IsUnique()) after resetting its client (UE 5.8
// Editor/UnrealEd/Private/SEditorViewport.cpp:65). check(), not ensure(): one surviving
// TSharedPtr<FSceneViewport> aborts the editor. It did, mid-queue, from camera.orbit_shots over a
// staticMesh subject, taking the rest of the suite with it.
//
// Three providers cleared FResolvedSubject's viewport pair by hand at the top of their Release and
// the Niagara one did not, so the invariant was per-provider discipline - invisible at review time
// and one new provider away from returning. ReleaseSubject now clears the pair centrally, BEFORE
// the provider's Release runs, and this test is the assertion that it does.
//
// WHAT IT FAILS ON: a ReleaseSubject that leaves the clearing to providers. The fake provider below
// deliberately does NOT clear the pair - it is shaped exactly like the Niagara one was - and
// records what it was handed. On the pre-fix code it is handed a live pointer and this test fails
// by NAME with a readable message, which is the whole point: the pre-fix signal was a dead process
// and a log that blamed the engine.
//
// UNABLE TO FAIL IF the fake provider cleared the pair itself, or if the assertions ran on the
// subject after ReleaseSubject returned rather than on what Release SAW. Both would pass against a
// ReleaseSubject that never touched the fields, because the destructor and the provider would have
// tidied up by the time anyone looked. The recording therefore happens INSIDE Release, at the only
// instant that matters - the one where CloseAssetEditor is about to run.
//
// NO REAL VIEWPORT IS INVOLVED and none is needed: the assertion is about who is still holding a
// reference, not about what it points at. ViewportClient carries the signal here as a never-
// dereferenced sentinel (this file already relies on null-comparison being the only defined
// operation on it), which keeps the test hostless and deterministic. The live-viewport half - that
// a real preview FSceneViewport really does drop to zero extra holders - is measured against an
// actual Static Mesh editor in Tests/Render/TestCaptureSubjectMesh.cpp.
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectReleaseDropsViewportRefsBeforeProviderReleaseTest,
    "PinWright.render.capture_subject.ReleaseDropsViewportRefsBeforeProviderRelease",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectReleaseDropsViewportRefsBeforeProviderReleaseTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubject;
    using namespace PinWrightCaptureSubjectResolverTest;

    FSubjectRequest Request;
    Request.Kind = ESubjectKind::StaticMesh;
    Request.AssetPath = TEXT("/Game/Test/Fixture.Fixture");

    // What the provider's Release was handed, recorded at its first statement.
    struct FSeenAtRelease
    {
        bool bRan = false;
        bool bViewportClientStillSet = false;
    };
    TSharedRef<FSeenAtRelease> Seen = MakeShared<FSeenAtRelease>();

    FSubjectProvider Provider;
    Provider.Kind = ESubjectKind::StaticMesh;
    Provider.Acquire = [](const FSubjectRequest&, FResolvedSubject& OutSubject,
        FSubjectTimeSetter&, FString&, FString&)
    {
        // A stand-in for "this provider opened an asset editor and took its preview viewport".
        // NEVER dereferenced - only compared against null, here and in Release - so no viewport,
        // no Slate and no RHI are needed to measure the invariant.
        OutSubject.ViewportClient = SentinelViewportClient();
        OutSubject.CaptureSource = TEXT("testProviderPreview");
        return true;
    };
    Provider.Release = [Seen](FResolvedSubject& Subject)
    {
        // FIRST statement, before anything else this Release might do: CloseAssetEditor runs from
        // here, so this is the instant the engine's check() would be evaluated against.
        Seen->bRan = true;
        Seen->bViewportClientStillSet = Subject.ViewportClient != nullptr;
        // Deliberately does NOT clear the pair. Shaped like the Niagara provider was, so the test
        // measures ReleaseSubject rather than the provider's own good manners.
        Subject.bEditorClosed = true;
    };

    FResolvedSubject Resolved;
    FSubjectTimeSetter TimeSetter;
    FString ErrCode;
    FString ErrMsg;
    if (!TestTrue(TEXT("the fixture provider resolves"),
            ResolveWithProvider(Provider, Request, Resolved, TimeSetter, ErrCode, ErrMsg)))
    {
        return true;
    }
    TestNotNull(TEXT("the resolved subject is holding a viewport client before the release"),
        Resolved.ViewportClient);

    ReleaseSubject(Resolved);

    if (!TestTrue(TEXT("the provider's Release actually ran"), Seen->bRan))
    {
        return true;
    }
    // THE ASSERTION. A live viewport client here means a live SceneViewport beside it, and
    // CloseAllEditorsForAsset one line later means check(SceneViewport.IsUnique()) aborting the
    // editor.
    TestFalse(TEXT("the subject's viewport client is already dropped when the provider's Release "
                   "(and so CloseAssetEditor) runs"),
        Seen->bViewportClientStillSet);

    // And it stays dropped for a caller that reads the subject afterwards for its measured fields.
    TestNull(TEXT("the released subject exposes no viewport client"), Resolved.ViewportClient);
    TestFalse(TEXT("the released subject exposes no scene viewport"), Resolved.SceneViewport.IsValid());
    TestTrue(TEXT("the measured close outcome survives the clearing"), Resolved.bEditorClosed);

    return true;
}

// =================================================================================================
// 5. A kind with no time axis refuses by name
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectMissingTimeAxisIsATypedRefusalTest,
    "PinWright.render.capture_subject.MissingTimeAxisIsATypedRefusal",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectMissingTimeAxisIsATypedRefusalTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubject;
    using namespace PinWrightCaptureSubjectResolverTest;

    FSubjectRequest Request;
    Request.Kind = ESubjectKind::StaticMesh;

    // A provider that supplies NO time setter. Decision 6: the missing capability must answer with
    // a typed refusal naming what is missing, not with silence and not with UNKNOWN_PARAMS from the
    // dispatcher's argument gate.
    TSharedRef<FProviderCallCounts> Counts = MakeShared<FProviderCallCounts>();
    const FSubjectProvider Provider = MakeCountingProvider(ESubjectKind::StaticMesh, Counts,
        /*bAcquireSucceeds=*/true, /*bSupplyTimeSetter=*/false);

    FResolvedSubject Resolved;
    FSubjectTimeSetter TimeSetter;
    FString ErrCode;
    FString ErrMsg;
    TestTrue(TEXT("resolve succeeds even though the kind has no time axis"),
        ResolveWithProvider(Provider, Request, Resolved, TimeSetter, ErrCode, ErrMsg));
    TestFalse(TEXT("the subject reports no time support"), Resolved.bTimeSupported);

    // ALWAYS filled, per the contract: a caller must never invoke an empty TFunction.
    TestTrue(TEXT("a time setter is always installed"), static_cast<bool>(TimeSetter));

    FString SetterErrCode;
    FString SetterErrMsg;
    TestFalse(TEXT("asking a static mesh for an instant is refused"),
        TimeSetter(1.5, SetterErrCode, SetterErrMsg));
    TestEqual(TEXT("the refusal is UNSUPPORTED_ASSET_EDITOR, not UNKNOWN_PARAMS"),
        SetterErrCode, FString(ErrorCodes::ERR_UNSUPPORTED_ASSET_EDITOR));
    TestTrue(TEXT("the refusal names the missing time axis"),
        SetterErrMsg.Contains(TEXT("time")));
    TestTrue(TEXT("the refusal names the kind that has none"),
        SetterErrMsg.Contains(TEXT("staticMesh")));

    // FAILURE DIRECTION: a provider that DOES supply a setter keeps its own, and it is not
    // overwritten by the refusing default.
    {
        TSharedRef<FProviderCallCounts> TimedCounts = MakeShared<FProviderCallCounts>();
        const FSubjectProvider TimedProvider = MakeCountingProvider(ESubjectKind::Animation,
            TimedCounts, /*bAcquireSucceeds=*/true, /*bSupplyTimeSetter=*/true);

        FSubjectRequest TimedRequest;
        TimedRequest.Kind = ESubjectKind::Animation;
        FResolvedSubject TimedResolved;
        FSubjectTimeSetter RealSetter;
        TestTrue(TEXT("a timed subject resolves"),
            ResolveWithProvider(TimedProvider, TimedRequest, TimedResolved, RealSetter, ErrCode, ErrMsg));
        TestTrue(TEXT("it reports time support"), TimedResolved.bTimeSupported);
        FString UnusedCode;
        FString UnusedMsg;
        TestTrue(TEXT("its own setter is kept, not replaced by the refusing default"),
            RealSetter(0.5, UnusedCode, UnusedMsg));
    }

    return true;
}

// =================================================================================================
// 6. The `subject` response block
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectInfoBlockOmitsWhatItCannotSayTest,
    "PinWright.render.capture_subject.SubjectBlockOmitsWhatItCannotSay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectInfoBlockOmitsWhatItCannotSayTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubject;
    using namespace PinWrightCaptureSubjectResolverTest;

    // ---- a bare world subject says almost nothing, and says it by omission ----
    {
        FResolvedSubject Subject;
        Subject.Kind = ESubjectKind::World;

        const TSharedPtr<FJsonObject> Info = MakeSubjectInfoObject(Subject);
        TestTrue(TEXT("the block exists"), Info.IsValid());
        TestEqual(TEXT("kind is unconditional"), Info->GetStringField(TEXT("kind")), FString(TEXT("world")));
        // A zeroed origin with a zero radius emitted anyway would read as "a point at the world
        // origin", which is a different claim from "nothing measured its size".
        TestFalse(TEXT("unmeasured bounds are omitted, not zeroed"), Info->HasField(TEXT("boundsRadius")));
        TestFalse(TEXT("unmeasured bounds origin is omitted"), Info->HasField(TEXT("boundsOrigin")));
        TestFalse(TEXT("an empty path is omitted"), Info->HasField(TEXT("path")));
        TestFalse(TEXT("an empty name is omitted"), Info->HasField(TEXT("name")));
        TestFalse(TEXT("an empty captureSource is omitted"), Info->HasField(TEXT("captureSource")));
        // No asset editor exists for a world subject, and `false` for both would invite the reader
        // to conclude one was left open.
        TestFalse(TEXT("window state is omitted on a world subject"),
            Info->HasField(TEXT("assetEditorWasAlreadyOpen")));
        TestFalse(TEXT("...both halves of it"), Info->HasField(TEXT("assetEditorClosed")));
        TestFalse(TEXT("time bounds are omitted when there is no time axis"),
            Info->HasField(TEXT("timeStartSeconds")));
        TestFalse(TEXT("no boundsWarning when there is nothing to warn about"),
            Info->HasField(TEXT("boundsWarning")));
        TestFalse(TEXT("no reproducibilityWarning when there is nothing to warn about"),
            Info->HasField(TEXT("reproducibilityWarning")));
    }

    // ---- a measured asset subject says everything it measured ----
    {
        FResolvedSubject Subject;
        Subject.Kind = ESubjectKind::Niagara;
        Subject.AssetPath = TEXT("/Game/FX/NS_Sparks.NS_Sparks");
        Subject.CaptureSource = TEXT("niagaraSystemEditorPreview");
        Subject.BoundsSource = TEXT("pinnedFixedBounds");
        Subject.BoundsOrigin = FVector(1.0, 2.0, 3.0);
        Subject.BoundsRadius = 250.0;
        Subject.bTimeSupported = true;
        Subject.TimeStartSeconds = 0.0;
        Subject.TimeEndSeconds = 3.0;
        Subject.bEditorWasAlreadyOpen = true;
        Subject.bEditorClosed = false;
        Subject.bTimeReproducible = false;
        Subject.BoundsWarning = TEXT("Emitter 'Spray' is a GPU emitter with no authored fixed bounds.");
        Subject.ReproducibilityWarning = TEXT("UNiagaraSystem::bDeterminism is false.");

        const TSharedPtr<FJsonObject> Info = MakeSubjectInfoObject(Subject);
        TestEqual(TEXT("kind"), Info->GetStringField(TEXT("kind")), FString(TEXT("niagara")));
        TestEqual(TEXT("path"), Info->GetStringField(TEXT("path")), Subject.AssetPath);
        TestEqual(TEXT("captureSource"), Info->GetStringField(TEXT("captureSource")), Subject.CaptureSource);
        TestEqual(TEXT("boundsSource"), Info->GetStringField(TEXT("boundsSource")), Subject.BoundsSource);
        TestEqual(TEXT("boundsRadius"), Info->GetNumberField(TEXT("boundsRadius")), 250.0);
        TestTrue(TEXT("boundsOrigin is an object"), Info->HasTypedField<EJson::Object>(TEXT("boundsOrigin")));
        TestTrue(TEXT("timeSupported"), Info->GetBoolField(TEXT("timeSupported")));
        TestEqual(TEXT("timeEndSeconds"), Info->GetNumberField(TEXT("timeEndSeconds")), 3.0);
        TestTrue(TEXT("window state is present on an asset subject"),
            Info->GetBoolField(TEXT("assetEditorWasAlreadyOpen")));
        TestFalse(TEXT("...and reports the measured close"), Info->GetBoolField(TEXT("assetEditorClosed")));
        TestEqual(TEXT("boundsWarning"), Info->GetStringField(TEXT("boundsWarning")), Subject.BoundsWarning);
        TestEqual(TEXT("reproducibilityWarning"),
            Info->GetStringField(TEXT("reproducibilityWarning")), Subject.ReproducibilityWarning);

        // §4.2: NO reproducibility claim of any spelling, in either direction. bTimeReproducible is
        // deliberately not mirrored into the JSON, because a boolean would be read as a promise
        // Niagara cannot keep - determinism is off by default at all three scopes and void under a
        // variable tick delta.
        for (const TPair<FString, TSharedPtr<FJsonValue>> Field : Info->Values)
        {
            TestFalse(*FString::Printf(TEXT("no field claims reproducibility ('%s')"), *Field.Key),
                Field.Key.Contains(TEXT("reproducible"), ESearchCase::IgnoreCase));
        }
    }

    // ---- the other direction: a reproducible subject still carries no boolean ----
    {
        FResolvedSubject Subject;
        Subject.Kind = ESubjectKind::Animation;
        Subject.bTimeSupported = true;
        Subject.bTimeReproducible = true;
        const TSharedPtr<FJsonObject> Info = MakeSubjectInfoObject(Subject);
        for (const TPair<FString, TSharedPtr<FJsonValue>> Field : Info->Values)
        {
            TestFalse(*FString::Printf(TEXT("still no reproducibility field ('%s')"), *Field.Key),
                Field.Key.Contains(TEXT("reproducible"), ESearchCase::IgnoreCase));
        }
        TestFalse(TEXT("a reproducible subject emits no warning either"),
            Info->HasField(TEXT("reproducibilityWarning")));
    }

    return true;
}

// =================================================================================================
// 7. The engine's own client registry accepts a real viewport with no cast
// =================================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCaptureSubjectRegisteredViewportNeedsNoCastTest,
    "PinWright.render.capture_subject.RegisteredViewportWidgetNeedsNoCast",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCaptureSubjectRegisteredViewportNeedsNoCastTest::RunTest(const FString& Parameters)
{
    using namespace PinWrightCaptureSubject;

    // Every FEditorViewportClient registers itself with GEditor in its constructor
    // (EditorViewportClient.cpp:601) and holds a TYPED TWeakPtr<SEditorViewport> back to its widget
    // (EditorViewportClient.h:1299). That pair is what lets the walk identify a viewport on the
    // engine's authority instead of on a name, so this test uses whatever real viewport the running
    // editor already has.
    TSharedPtr<SEditorViewport> RealViewport;
    if (GEditor)
    {
        for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
        {
            if (!Client)
            {
                continue;
            }
            TSharedPtr<SEditorViewport> Widget = Client->GetEditorViewportWidget();
            if (Widget.IsValid() && Widget->GetViewportClient().IsValid() &&
                Widget->GetSceneViewport().IsValid())
            {
                RealViewport = Widget;
                break;
            }
        }
    }

    if (!RealViewport.IsValid())
    {
        // LOUD, not silent. A test that takes a conditional-skip path and still reports success is
        // the failure mode board ticket B-test-skips-assertions-silently exists for, so the marker
        // is greppable and the reason is named.
        PinWrightTestSkip::SkipAssertions(*this, TEXT("no-realized-editor-viewport"),
            TEXT("this host has no FEditorViewportClient with a realized widget, so the "
                 "client-registry verification path was NOT exercised. The allow-list path is "
                 "covered by AllowListEntriesAreRealEditorViewports and the refusal path by "
                 "UnknownWidgetTypeIsRefusedNotCast"));
        return true;
    }

    const FString RealTypeName = RealViewport->GetTypeAsString();
    const FEditorViewportSearch Search =
        FindEditorViewportInWidgetTree(RealViewport.ToSharedRef());

    TestTrue(*FString::Printf(TEXT("a real '%s' is found"), *RealTypeName), Search.Viewport.IsValid());
    TestEqual(TEXT("...and it is the same widget"), Search.Viewport.Get(), RealViewport.Get());
    // The registry path is what accepted it. If this ever flips to the allow-list, the registry
    // lookup silently stopped working and every off-list viewport quietly became unreachable again.
    TestTrue(TEXT("the engine's client registry verified it, with no cast"),
        Search.bVerifiedByClientRegistry);
    TestFalse(TEXT("the allow-list was not needed"), Search.bVerifiedByAllowList);
    TestEqual(TEXT("nothing was reported unverified"), Search.UnverifiedTypeNames.Num(), 0);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
