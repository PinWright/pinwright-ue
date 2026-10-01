// Copyright (c) 2026 Alexander Penkin. MIT License.

// Live tests for the surface=web action verbs against a real CEF page. Each test builds its own
// fixture - a UMG UWebBrowser in a uniquely-titled Slate window, loaded with a self-contained
// data: page - so no host WebUI is needed, and tears it down so later tests see no browser.
//
// One test per verb. Each asserts the page-side effect of REAL input (isTrusted events, CSS
// :hover, text insertion, native wheel scrolling, a pointer drop) and then the refusal of a
// target covered by another page element: TARGET_OCCLUDED naming the cover, and no event reaching
// either element. Both halves fail on the synthetic-DOM implementation these replace: its events
// were untrusted, it never typed text, and it acted on covered targets with success.
//
// A host without CEF (the WebBrowser module unavailable, -nocef, a null RHI) or one whose fixture
// page never becomes drivable skips with PINWRIGHT_ASSERTIONS_SKIPPED.

#include "Misc/AutomationTest.h"

#include "Handlers/Drive/DriveWebBridge.h"
#include "Tests/TestSkipReporting.h"
#include "Tests/TestUtils.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationCommon.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "WebBrowser.h"
#include "WebBrowserModule.h"
#include "Widgets/SWindow.h"

namespace DriveWebLiveTest
{
    // Every element the tests act on carries a fixed data-pw-id, so no observe is needed to find
    // handles. #cov sits under #cover; both count every input event they receive.
    const TCHAR* const FixtureHtml = TEXT(R"HTML(<!doctype html><html><head><style>
body{margin:0;font:14px sans-serif;background:#fff}
.b{position:absolute;width:120px;height:40px;margin:0;padding:0;box-sizing:border-box}
#hov{background:rgb(200,200,200)} #hov:hover{background:rgb(0,128,0)}
#list{width:160px;height:80px;overflow:auto} #list div{height:60px}
#cover{position:absolute;left:400px;top:200px;width:200px;height:120px;background:#ddd;z-index:5}
</style></head><body>
<button id="btn" class="b" style="left:10px;top:10px" data-pw-id="t-btn">Btn</button>
<div id="hov" class="b" style="left:150px;top:10px" data-pw-id="t-hov">Hov</div>
<div id="list" class="b" style="left:290px;top:10px" data-pw-id="t-list" tabindex="0"><div>one</div><div>two</div><div>three</div><div>four</div></div>
<input id="inp" class="b" style="left:10px;top:120px" data-pw-id="t-inp">
<div id="src" class="b" style="left:150px;top:120px" data-pw-id="t-src">Src</div>
<div id="dst" class="b" style="left:150px;top:260px" data-pw-id="t-dst">Dst</div>
<input id="cov" class="b" style="left:440px;top:240px" data-pw-id="t-cov">
<div id="cover"></div>
<script>
var S={clicks:0,clickTrusted:false,hover:false,hoverTrusted:false,wheelTrusted:false,drop:'',dropTrusted:false,armed:false,coveredHits:0,coverHits:0};
function g(i){return document.getElementById(i);}
g('btn').addEventListener('click',function(e){S.clicks++;S.clickTrusted=e.isTrusted;});
g('hov').addEventListener('mouseover',function(e){S.hoverTrusted=e.isTrusted;S.hover=g('hov').matches(':hover');});
g('list').addEventListener('wheel',function(e){S.wheelTrusted=e.isTrusted;});
g('src').addEventListener('pointerdown',function(e){S.armed=true;});
document.addEventListener('pointerup',function(e){if(S.armed){S.drop=e.target.id;S.dropTrusted=e.isTrusted;S.armed=false;}});
['pointerdown','pointerover','click','wheel','keydown','focus'].forEach(function(t){
g('cov').addEventListener(t,function(){S.coveredHits++;});g('cover').addEventListener(t,function(){S.coverHits++;});});
window.__pwstate=function(){return {ready:true,clicks:S.clicks,clickTrusted:S.clickTrusted,hover:S.hover,hoverTrusted:S.hoverTrusted,
wheelTrusted:S.wheelTrusted,scrollTop:g('list').scrollTop,value:g('inp').value,covValue:g('cov').value,drop:S.drop,dropTrusted:S.dropTrusted,
coveredHits:S.coveredHits,coverHits:S.coverHits};};
</script></body></html>)HTML");

    constexpr double ReadySeconds = 30.0;
    constexpr double ResponseSeconds = 20.0;
    constexpr double EffectSeconds = 5.0;

    struct FFixture
    {
        FString Title;
        TSharedPtr<SWindow> Window;
        TStrongObjectPtr<UWebBrowser> Browser;
        // Set once a step skipped or failed hard; every later step is then a no-op.
        bool bAborted = false;

        // Per-step scratch.
        bool bStepStarted = false;
        double StepDeadline = 0.0;
        TSharedRef<FTestResponseCapture> Capture = MakeShared<FTestResponseCapture>();
        TSharedPtr<FJsonObject> State;
        bool bStateInFlight = false;

        ~FFixture()
        {
            *AliveToken = false;
            if (Browser.IsValid())
            {
                Browser->ReleaseSlateResources(true);
            }
            if (Window.IsValid() && FSlateApplication::IsInitialized())
            {
                FSlateApplication::Get().RequestDestroyWindow(Window.ToSharedRef());
                // RequestDestroyWindow only queues; tick so later tests do not discover this browser.
                FSlateApplication::Get().Tick(ESlateTickType::All);
            }
        }

        int32 BrowserIndex() const
        {
            return FDriveWebBridge::DiscoverBrowsers().IndexOfByKey(Browser.Get());
        }

        void BeginStep(double Seconds)
        {
            bStepStarted = true;
            StepDeadline = FPlatformTime::Seconds() + Seconds;
        }

        // Kick one page-state read unless one is in flight; State holds the latest answer.
        void RequestState()
        {
            if (bStateInFlight || !Browser.IsValid())
            {
                return;
            }
            bStateInFlight = true;
            FFixture* Self = this;
            const TSharedRef<bool> Alive = AliveToken;
            FDriveWebBridge::ExecuteJsAwaitResult(Browser.Get(),
                TEXT("(window.__pwstate?window.__pwstate():{ready:false})"),
                [Self, Alive](bool bOk, const FString& Json)
                {
                    if (!*Alive)
                    {
                        return;
                    }
                    Self->bStateInFlight = false;
                    TSharedPtr<FJsonObject> Parsed;
                    if (bOk && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Parsed) && Parsed.IsValid())
                    {
                        Self->State = Parsed;
                    }
                },
                5.0);
        }

        bool StateBool(const TCHAR* Field) const
        {
            bool bValue = false;
            return State.IsValid() && State->TryGetBoolField(Field, bValue) && bValue;
        }
        double StateNumber(const TCHAR* Field) const
        {
            double Value = 0.0;
            return (State.IsValid() && State->TryGetNumberField(Field, Value)) ? Value : 0.0;
        }
        FString StateString(const TCHAR* Field) const
        {
            FString Value;
            if (State.IsValid())
            {
                State->TryGetStringField(Field, Value);
            }
            return Value;
        }

        // Cleared by the destructor: a state read still in flight must not write into a freed fixture.
        TSharedRef<bool> AliveToken = MakeShared<bool>(true);
    };

    TSharedPtr<FFixture> BuildFixture(FAutomationTestBase& Test)
    {
        if (PinWrightTestSkip::SkipIfRenderingUnavailable(Test))
        {
            return nullptr;
        }
        if (!FSlateApplication::IsInitialized())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("slate_not_initialized"),
                TEXT("FSlateApplication is not initialized; the web fixture window cannot be built."));
            return nullptr;
        }
        if (FParse::Param(FCommandLine::Get(), TEXT("nocef")) || !IWebBrowserModule::Get().IsWebModuleAvailable())
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("cef-unavailable"),
                TEXT("The WebBrowser module reports no CEF runtime on this host (or -nocef); no web fixture can load."));
            return nullptr;
        }

        TSharedPtr<FFixture> Fixture = MakeShared<FFixture>();
        Fixture->Title = FString::Printf(TEXT("PW_WebLive_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        const FTCHARToUTF8 Utf8(FixtureHtml);
        const TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        const FString Url = TEXT("data:text/html;base64,") + FBase64::Encode(Bytes);

        UWebBrowser* Browser = NewObject<UWebBrowser>(GetTransientPackage());
        Fixture->Browser.Reset(Browser);
        // InitialURL is the widget's construction-time page (protected, so set by reflection):
        // loading it at construction avoids racing a LoadURL against the async CEF browser creation.
        if (FStrProperty* InitialUrl = FindFProperty<FStrProperty>(UWebBrowser::StaticClass(), TEXT("InitialURL")))
        {
            InitialUrl->SetPropertyValue_InContainer(Browser, Url);
        }

        // Place the window clear of every visible window this editor already shows, so no other
        // window holds the fixture's points in Slate's window order and the test measures the verbs
        // rather than the host's window layout.
        TArray<TSharedRef<SWindow>> Visible;
        FSlateApplication::Get().GetAllVisibleWindowsOrdered(Visible);
        double ClearX = 160.0;
        for (const TSharedRef<SWindow>& Other : Visible)
        {
            const FVector2D Position(Other->GetPositionInScreen());
            const FVector2D Size(Other->GetSizeInScreen());
            ClearX = FMath::Max(ClearX, Position.X + Size.X + 40.0);
        }

        Fixture->Window = SNew(SWindow)
            .Title(FText::FromString(Fixture->Title))
            .ScreenPosition(FVector2D(ClearX, 140.0))
            .ClientSize(FVector2D(640.0f, 400.0f))
            .FocusWhenFirstShown(false)
            // Also topmost, in case the platform ignores the requested position: Slate orders a new
            // topmost window above every other window, notification windows included.
            .IsTopmostWindow(true)
            .SupportsMaximize(false)
            .SupportsMinimize(false);
        Fixture->Window->SetContent(Browser->TakeWidget());
        FSlateApplication::Get().AddWindow(Fixture->Window.ToSharedRef(), /*bShowImmediately=*/true);
        return Fixture;
    }
}

DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FDriveWebLiveStep, TFunction<bool()>, Step);

bool FDriveWebLiveStep::Update()
{
    return Step();
}

namespace DriveWebLiveTest
{
    // Wait until the fixture browser is discoverable and its page script answers.
    void QueueReady(FAutomationTestBase& Test, const TSharedPtr<FFixture>& F)
    {
        ADD_LATENT_AUTOMATION_COMMAND(FDriveWebLiveStep([&Test, F]() -> bool
        {
            if (!F->bStepStarted)
            {
                F->BeginStep(ReadySeconds);
            }
            if (F->BrowserIndex() != INDEX_NONE)
            {
                if (F->StateBool(TEXT("ready")))
                {
                    F->bStepStarted = false;
                    return true;
                }
                F->RequestState();
            }
            if (FPlatformTime::Seconds() > F->StepDeadline)
            {
                PinWrightTestSkip::SkipAssertions(Test, TEXT("web-fixture-not-ready"),
                    FString::Printf(TEXT("The CEF fixture page did not become drivable within %.0f s (discovered=%s, page answered=%s)."),
                        ReadySeconds, F->BrowserIndex() != INDEX_NONE ? TEXT("yes") : TEXT("no"),
                        F->State.IsValid() ? TEXT("yes") : TEXT("no")));
                F->bAborted = true;
                F->bStepStarted = false;
                return true;
            }
            return false;
        }));
    }

    // Run Method with Args (surface=web and the fixture's browser_index are added) and wait for its
    // response in F->Capture.
    void QueueAction(FAutomationTestBase& Test, const TSharedPtr<FFixture>& F, const FString& Method,
        const TSharedPtr<FJsonObject>& Args)
    {
        ADD_LATENT_AUTOMATION_COMMAND(FDriveWebLiveStep([&Test, F, Method, Args]() -> bool
        {
            if (F->bAborted)
            {
                return true;
            }
            if (!F->bStepStarted)
            {
                F->BeginStep(ResponseSeconds);
                Args->SetStringField(TEXT("surface"), TEXT("web"));
                Args->SetNumberField(TEXT("browser_index"), F->BrowserIndex());
                Test.TestTrue(FString::Printf(TEXT("%s handler found"), *Method),
                    InvokeHandlerWithSharedCapture(Method, Args, F->Capture));
            }
            if (F->Capture->bWasCalled)
            {
                F->bStepStarted = false;
                return true;
            }
            if (FPlatformTime::Seconds() > F->StepDeadline)
            {
                Test.AddError(FString::Printf(TEXT("%s never answered within %.0f s."), *Method, ResponseSeconds));
                F->bAborted = true;
                F->bStepStarted = false;
                return true;
            }
            return false;
        }));
    }

    // Poll the page state until Until holds or EffectSeconds pass (the assertions that follow
    // report a miss); an effect such as smooth scrolling can land a few frames after the response.
    void QueueState(const TSharedPtr<FFixture>& F, TFunction<bool(const FFixture&)> Until)
    {
        ADD_LATENT_AUTOMATION_COMMAND(FDriveWebLiveStep([F, Until]() -> bool
        {
            if (F->bAborted)
            {
                return true;
            }
            if (!F->bStepStarted)
            {
                F->BeginStep(EffectSeconds);
                F->State.Reset();
            }
            if (F->State.IsValid() && Until(*F))
            {
                F->bStepStarted = false;
                return true;
            }
            if (FPlatformTime::Seconds() > F->StepDeadline)
            {
                F->bStepStarted = false;
                return true;
            }
            F->RequestState();
            return false;
        }));
    }

    void QueueCheck(const TSharedPtr<FFixture>& F, TFunction<void(const FFixture&)> Check)
    {
        ADD_LATENT_AUTOMATION_COMMAND(FDriveWebLiveStep([F, Check]() -> bool
        {
            if (!F->bAborted)
            {
                Check(*F);
            }
            return true;
        }));
    }

    // The action succeeded through real input. The fixture is placed clear of every window that
    // existed when it was built; a window the host opens over it afterwards makes the Slate refusal
    // correct and the run measure nothing, so that alone is a skip.
    bool CheckDelivered(FAutomationTestBase& Test, FFixture& F, const TCHAR* Verb)
    {
        const FTestResponseCapture& Capture = *F.Capture;
        FString Window;
        if (!Capture.bSuccess && Capture.ErrorCode == TEXT("TARGET_OCCLUDED") && Capture.Result.IsValid()
            && Capture.Result->TryGetStringField(TEXT("occluding_window"), Window) && Window != F.Title)
        {
            PinWrightTestSkip::SkipAssertions(Test, TEXT("fixture-window-stacked-under-host-window"),
                FString::Printf(TEXT("The web fixture window sits under host window '%s'."), *Window));
            F.bAborted = true;
            return false;
        }
        Test.TestTrue(FString::Printf(TEXT("web %s succeeds (error: %s %s)"), Verb, *Capture.ErrorCode, *Capture.Message),
            Capture.bSuccess);
        FString InputPath;
        Test.TestTrue(TEXT("the response reports Slate input"),
            Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("input_path"), InputPath) && InputPath == TEXT("slate"));
        FString Outcome;
        Test.TestTrue(TEXT("the response carries a settle outcome"),
            Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("outcome"), Outcome) && !Outcome.IsEmpty());
        return Capture.bSuccess;
    }

    // Queue the occluded-target half: Method on the covered #cov is refused with TARGET_OCCLUDED
    // naming #cover, and neither element received any event from it. The counts are taken against
    // a sample read just before the action: on Windows the OS cursor (wherever earlier tests left
    // it) can deliver a real pointer event when the fixture window appears under it, and that event
    // is not the verb's (seen once on UE 5.7 in a full suite, coverHits=1 on the first fixture).
    void QueueOccludedRefusal(FAutomationTestBase& Test, const TSharedPtr<FFixture>& F, const FString& Method,
        const TSharedPtr<FJsonObject>& Args)
    {
        const TSharedRef<TPair<double, double>> Before = MakeShared<TPair<double, double>>(0.0, 0.0);
        QueueState(F, [](const FFixture&) { return true; });
        QueueCheck(F, [Before](const FFixture& Fx)
        {
            *Before = TPair<double, double>(Fx.StateNumber(TEXT("coveredHits")), Fx.StateNumber(TEXT("coverHits")));
        });
        QueueAction(Test, F, Method, Args);
        QueueState(F, [](const FFixture&) { return true; });
        QueueCheck(F, [&Test, Method, Before](const FFixture& Fx)
        {
            const FTestResponseCapture& Capture = *Fx.Capture;
            Test.TestFalse(FString::Printf(TEXT("%s on a covered element is not a success"), *Method), Capture.bSuccess);
            Test.TestEqual(FString::Printf(TEXT("%s on a covered element is TARGET_OCCLUDED"), *Method),
                Capture.ErrorCode, FString(TEXT("TARGET_OCCLUDED")));
            FString Occluder;
            Test.TestTrue(TEXT("the refusal names the covering element"),
                Capture.Result.IsValid() && Capture.Result->TryGetStringField(TEXT("occluding_element"), Occluder)
                && Occluder == TEXT("div#cover"));
            Test.TestEqual(TEXT("the covered element received no event"), Fx.StateNumber(TEXT("coveredHits")), Before->Key);
            Test.TestEqual(TEXT("the cover received no event"), Fx.StateNumber(TEXT("coverHits")), Before->Value);
        });
    }

    TSharedPtr<FJsonObject> HandleArgs(const TCHAR* Handle)
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("handle"), Handle);
        return Args;
    }
}

// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebLiveClickTest,
    "PinWright.drive.weblive.ClickIsTrustedAndRefusesOccluded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebLiveClickTest::RunTest(const FString& Parameters)
{
    using namespace DriveWebLiveTest;
    TSharedPtr<FFixture> F = BuildFixture(*this);
    if (!F.IsValid())
    {
        return true;
    }
    QueueReady(*this, F);
    QueueAction(*this, F, TEXT("drive.click"), HandleArgs(TEXT("t-btn")));
    QueueState(F, [](const FFixture& Fx) { return Fx.StateNumber(TEXT("clicks")) >= 1.0; });
    QueueCheck(F, [this, F](const FFixture& Fx)
    {
        if (CheckDelivered(*this, *F, TEXT("click")))
        {
            TestEqual(TEXT("the button was clicked exactly once"), Fx.StateNumber(TEXT("clicks")), 1.0);
            TestTrue(TEXT("the click is a trusted (real) event"), Fx.StateBool(TEXT("clickTrusted")));
        }
    });
    QueueOccludedRefusal(*this, F, TEXT("drive.click"), HandleArgs(TEXT("t-cov")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebLiveHoverTest,
    "PinWright.drive.weblive.HoverAppliesCssHoverAndRefusesOccluded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebLiveHoverTest::RunTest(const FString& Parameters)
{
    using namespace DriveWebLiveTest;
    TSharedPtr<FFixture> F = BuildFixture(*this);
    if (!F.IsValid())
    {
        return true;
    }
    QueueReady(*this, F);
    QueueAction(*this, F, TEXT("drive.hover"), HandleArgs(TEXT("t-hov")));
    QueueState(F, [](const FFixture& Fx) { return Fx.StateBool(TEXT("hoverTrusted")); });
    QueueCheck(F, [this, F](const FFixture& Fx)
    {
        if (CheckDelivered(*this, *F, TEXT("hover")))
        {
            TestTrue(TEXT("the hover is a trusted mouseover"), Fx.StateBool(TEXT("hoverTrusted")));
            TestTrue(TEXT("CSS :hover applied to the hovered element"), Fx.StateBool(TEXT("hover")));
        }
    });
    QueueOccludedRefusal(*this, F, TEXT("drive.hover"), HandleArgs(TEXT("t-cov")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebLiveScrollTest,
    "PinWright.drive.weblive.ScrollWheelsListAndRefusesOccluded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebLiveScrollTest::RunTest(const FString& Parameters)
{
    using namespace DriveWebLiveTest;
    TSharedPtr<FFixture> F = BuildFixture(*this);
    if (!F.IsValid())
    {
        return true;
    }
    QueueReady(*this, F);
    TSharedPtr<FJsonObject> Args = HandleArgs(TEXT("t-list"));
    // Negative scrolls down, as on the game surface.
    Args->SetNumberField(TEXT("delta"), -2.0);
    QueueAction(*this, F, TEXT("drive.scroll"), Args);
    QueueState(F, [](const FFixture& Fx) { return Fx.StateNumber(TEXT("scrollTop")) > 0.0; });
    QueueCheck(F, [this, F](const FFixture& Fx)
    {
        if (CheckDelivered(*this, *F, TEXT("scroll")))
        {
            TestTrue(TEXT("the wheel event is trusted"), Fx.StateBool(TEXT("wheelTrusted")));
            TestTrue(FString::Printf(TEXT("the list scrolled down natively (scrollTop=%.0f)"), Fx.StateNumber(TEXT("scrollTop"))),
                Fx.StateNumber(TEXT("scrollTop")) > 0.0);
        }
    });
    TSharedPtr<FJsonObject> Covered = HandleArgs(TEXT("t-cov"));
    Covered->SetNumberField(TEXT("delta"), -2.0);
    QueueOccludedRefusal(*this, F, TEXT("drive.scroll"), Covered);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebLiveKeyTest,
    "PinWright.drive.weblive.KeyTypesIntoInputAndRefusesOccluded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebLiveKeyTest::RunTest(const FString& Parameters)
{
    using namespace DriveWebLiveTest;
    TSharedPtr<FFixture> F = BuildFixture(*this);
    if (!F.IsValid())
    {
        return true;
    }
    QueueReady(*this, F);
    TSharedPtr<FJsonObject> Args = HandleArgs(TEXT("t-inp"));
    Args->SetStringField(TEXT("key"), TEXT("x"));
    QueueAction(*this, F, TEXT("drive.key"), Args);
    QueueState(F, [](const FFixture& Fx) { return Fx.StateString(TEXT("value")) == TEXT("x"); });
    QueueCheck(F, [this, F](const FFixture& Fx)
    {
        if (CheckDelivered(*this, *F, TEXT("key")))
        {
            // Only a real key event inserts text; a dispatched KeyboardEvent never does.
            TestEqual(TEXT("the key's default action typed into the focused input"),
                Fx.StateString(TEXT("value")), FString(TEXT("x")));
        }
    });
    TSharedPtr<FJsonObject> Covered = HandleArgs(TEXT("t-cov"));
    Covered->SetStringField(TEXT("key"), TEXT("x"));
    QueueOccludedRefusal(*this, F, TEXT("drive.key"), Covered);
    QueueCheck(F, [this](const FFixture& Fx)
    {
        TestEqual(TEXT("nothing was typed into the covered input"), Fx.StateString(TEXT("covValue")), FString());
    });
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDriveWebLiveDragTest,
    "PinWright.drive.weblive.DragDropsOnTargetAndRefusesOccluded",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FDriveWebLiveDragTest::RunTest(const FString& Parameters)
{
    using namespace DriveWebLiveTest;
    TSharedPtr<FFixture> F = BuildFixture(*this);
    if (!F.IsValid())
    {
        return true;
    }
    QueueReady(*this, F);
    TSharedPtr<FJsonObject> Args = HandleArgs(TEXT("t-src"));
    Args->SetStringField(TEXT("to_handle"), TEXT("t-dst"));
    QueueAction(*this, F, TEXT("drive.drag"), Args);
    QueueState(F, [](const FFixture& Fx) { return !Fx.StateString(TEXT("drop")).IsEmpty(); });
    QueueCheck(F, [this, F](const FFixture& Fx)
    {
        if (CheckDelivered(*this, *F, TEXT("drag")))
        {
            TestEqual(TEXT("the pointer was released over the drop target"), Fx.StateString(TEXT("drop")), FString(TEXT("dst")));
            TestTrue(TEXT("the release is a trusted pointerup"), Fx.StateBool(TEXT("dropTrusted")));
        }
    });
    TSharedPtr<FJsonObject> Covered = HandleArgs(TEXT("t-cov"));
    Covered->SetStringField(TEXT("to_handle"), TEXT("t-dst"));
    QueueOccludedRefusal(*this, F, TEXT("drive.drag"), Covered);
    return true;
}
