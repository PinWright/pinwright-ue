// Copyright (c) 2026 Alexander Penkin. MIT License.

#include "Handlers/Drive/DriveWebBridge.h"

#include "SWebBrowser.h"
#include "WebBrowser.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Layout/Geometry.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

// Local helpers for the web bridge. A uniquely-named namespace (not anonymous) so the
// Unity build cannot collide these with same-named helpers from other drive TUs that may
// be merged into the same translation unit.
namespace DriveWebBridgeLocal
{
    // ── JSON field readers (raw FJsonObject, no module coupling) ─────────────────────
    double ReadNum(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double Default = 0.0)
    {
        double Value = Default;
        return (Obj.IsValid() && Obj->TryGetNumberField(Key, Value)) ? Value : Default;
    }

    bool ReadBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, bool bDefault)
    {
        bool bValue = bDefault;
        return (Obj.IsValid() && Obj->TryGetBoolField(Key, bValue)) ? bValue : bDefault;
    }

    FString ReadStr(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key)
    {
        FString Value;
        if (Obj.IsValid())
        {
            Obj->TryGetStringField(Key, Value);
        }
        return Value;
    }

    TSharedPtr<FJsonObject> DeserializeObject(const FString& Json)
    {
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return nullptr;
        }
        return Root;
    }

    // How often the source-poll awaiter re-pulls SWebBrowser::GetSource() while waiting for a
    // marked result. GetSource is async + cheap; 20 Hz is responsive without busy-spinning.
    constexpr double GPollIntervalSeconds = 0.05;

    // Standard base64 alphabet (RFC 4648). The injected payload is base64 only, so the marked
    // run ends at the first char that is not one of these (the result node's closing '<').
    bool IsBase64Char(TCHAR C)
    {
        return (C >= TEXT('A') && C <= TEXT('Z'))
            || (C >= TEXT('a') && C <= TEXT('z'))
            || (C >= TEXT('0') && C <= TEXT('9'))
            || C == TEXT('+') || C == TEXT('/') || C == TEXT('=');
    }

    // Decode a standard-base64 string of UTF-8 bytes (what the page's
    // btoa(unescape(encodeURIComponent(...))) produces) back into an FString. Returns false
    // only when the base64 itself is malformed.
    bool DecodeBase64Utf8(const FString& Base64, FString& OutJson)
    {
        OutJson.Reset();
        TArray<uint8> Bytes;
        if (!FBase64::Decode(Base64, Bytes))
        {
            return false;
        }
        if (Bytes.Num() > 0)
        {
            const auto Conv = StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
            OutJson.AppendChars(Conv.Get(), Conv.Length());
        }
        return true;
    }

    // JS that defines __pwwrite(payload): base64-encode JSON.stringify(payload) (UTF-8 safe via
    // unescape/encodeURIComponent) and store EscapedMarker+base64 into a hidden, reused DOM node.
    // base64 has no HTML-special chars, so the value survives DOM serialization with no escaping,
    // and the C++ side pulls it back with SWebBrowser::GetSource().
    FString BuildResultWriterJs(const FString& EscapedMarker)
    {
        return FString::Printf(
            TEXT("var __pwm='%s';")
            TEXT("function __pwwrite(p){")
            TEXT("var b=btoa(unescape(encodeURIComponent(JSON.stringify(p))));")
            TEXT("var __pwn=document.getElementById('__pwdrive_result');")
            TEXT("if(!__pwn){__pwn=document.createElement('div');__pwn.id='__pwdrive_result';")
            TEXT("__pwn.style.display='none';(document.body||document.documentElement).appendChild(__pwn);}")
            TEXT("__pwn.textContent=__pwm+b;}"),
            *EscapedMarker);
    }

    // Whether Browser is a live, drivable CEF browser (see DiscoverBrowsers contract).
    bool IsDrivableBrowser(UWebBrowser* Browser)
    {
        if (!IsValid(Browser))
        {
            return false;
        }
        if (Browser->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
        {
            return false;
        }
        // A cached Slate widget means the browser is built and not slate-released / pooled.
        if (!Browser->GetCachedWidget().IsValid())
        {
            return false;
        }
        // A real navigated document — reject empty and about:* (blank/stale) browsers.
        if (!FDriveWebBridge::IsDrivableUrl(Browser->GetUrl()))
        {
            return false;
        }
        // Non-zero painted geometry means it is actually laid out / visible.
        const FVector2D Size = Browser->GetCachedGeometry().GetLocalSize();
        if (Size.X <= 0.0 || Size.Y <= 0.0)
        {
            return false;
        }
        return true;
    }

    // The owning window's DPI scale, used only as a fallback when the page payload omits
    // devicePixelRatio. Best-effort: 1.0 when the browser is not cached into a window.
    double ResolveDpiScale(UWebBrowser* Browser)
    {
        if (!Browser || !FSlateApplication::IsInitialized())
        {
            return 1.0;
        }

        const TSharedPtr<SWidget> Cached = Browser->GetCachedWidget();
        if (!Cached.IsValid())
        {
            return 1.0;
        }

        const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(Cached.ToSharedRef());
        return Window.IsValid() ? Window->GetDPIScaleFactor() : 1.0;
    }

    // Create the awaiter (which polls GetSource for Marker), then inject FullJs (which must
    // itself write Marker+base64(payload) into the hidden result node).
    void StartAwait(
        UWebBrowser* Browser,
        const FString& FullJs,
        const FString& Marker,
        TFunction<void(bool, const FString&)> OnDone,
        double TimeoutSeconds)
    {
        UDriveWebSourceAwaiter* Awaiter = NewObject<UDriveWebSourceAwaiter>();
        Awaiter->Start(Browser, Marker, MoveTemp(OnDone), TimeoutSeconds);
        Browser->ExecuteJavascript(FullJs);
    }
}

using namespace DriveWebBridgeLocal;

// ====================================================================================
// UDriveWebSourceAwaiter
// ====================================================================================

void UDriveWebSourceAwaiter::Start(
    UWebBrowser* InBrowser,
    const FString& InMarker,
    TFunction<void(bool, const FString&)> InOnDone,
    double TimeoutSeconds)
{
    Browser = InBrowser;
    Marker = InMarker;
    OnDone = MoveTemp(InOnDone);
    DeadlineSeconds = FPlatformTime::Seconds() + FMath::Max(0.0, TimeoutSeconds);

    // Keep this object alive while the round-trip is in flight.
    AddToRoot();

    PollHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateUObject(this, &UDriveWebSourceAwaiter::HandlePoll),
        static_cast<float>(GPollIntervalSeconds));
}

bool UDriveWebSourceAwaiter::HandlePoll(float DeltaTime)
{
    if (bFinished)
    {
        return false;
    }

    if (FPlatformTime::Seconds() >= DeadlineSeconds)
    {
        Finish(false, FString());
        return false;
    }

    // Only one GetSource request outstanding at a time; wait for its callback.
    if (bRequestInFlight)
    {
        return true;
    }

    UWebBrowser* LiveBrowser = Browser.Get();
    if (!LiveBrowser)
    {
        Finish(false, FString());
        return false;
    }

    const TSharedPtr<SWidget> Cached = LiveBrowser->GetCachedWidget();
    if (!Cached.IsValid())
    {
        Finish(false, FString());
        return false;
    }

    // The UWebBrowser's cached root widget is the SWebBrowser it builds in RebuildWidget()
    // (true for the engine class and game-specific subclasses alike).
    const TSharedRef<SWebBrowser> Web = StaticCastSharedRef<SWebBrowser>(Cached.ToSharedRef());

    bRequestInFlight = true;
    const TWeakObjectPtr<UDriveWebSourceAwaiter> WeakSelf(this);
    Web->GetSource([WeakSelf](const FString& Html)
    {
        if (UDriveWebSourceAwaiter* Self = WeakSelf.Get())
        {
            Self->HandleSource(Html);
        }
    });

    // Keep ticking; HandleSource decides whether to finish.
    return true;
}

void UDriveWebSourceAwaiter::HandleSource(const FString& Html)
{
    bRequestInFlight = false;
    if (bFinished)
    {
        return;
    }

    FString Base64;
    if (!FDriveWebBridge::ExtractMarkedPayload(Html, Marker, Base64) || Base64.IsEmpty())
    {
        // Marker not written yet (or empty): keep polling until the next tick or timeout.
        return;
    }

    FString Json;
    if (!DecodeBase64Utf8(Base64, Json) || Json.IsEmpty())
    {
        // A partially-serialized / undecodable sample: ignore and keep polling.
        return;
    }

    // The payload is in hand, so the hidden result node has done its job. Fire-and-forget a JS
    // snippet that removes it, so PinWright leaves no persistent base64 result blob in the host
    // page (other developers' live HUDs). The next query's writer re-creates the node (it is
    // create-if-missing), and the persistent `data-pw-id` element handles are intentionally left
    // in place for later observe/click. The GetSource callback fired off this browser, so it is
    // live here; no extra liveness work needed beyond the weak-ptr check.
    if (UWebBrowser* LiveBrowser = Browser.Get())
    {
        LiveBrowser->ExecuteJavascript(FDriveWebBridge::BuildCleanupJs());
    }

    Finish(true, Json);
}

void UDriveWebSourceAwaiter::Finish(bool bOk, const FString& Payload)
{
    if (bFinished)
    {
        return;
    }
    bFinished = true;

    if (PollHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
        PollHandle.Reset();
    }

    // Move the callback out and unroot before invoking, so re-entrant calls are safe and
    // the object becomes collectable once the callback returns.
    TFunction<void(bool, const FString&)> Callback = MoveTemp(OnDone);
    OnDone = nullptr;
    RemoveFromRoot();

    if (Callback)
    {
        Callback(bOk, Payload);
    }
}

// ====================================================================================
// FDriveWebBridge — discovery
// ====================================================================================

bool FDriveWebBridge::IsDrivableUrl(const FString& Url)
{
    return !Url.IsEmpty() && !Url.StartsWith(TEXT("about:"), ESearchCase::IgnoreCase);
}

TArray<UWebBrowser*> FDriveWebBridge::DiscoverBrowsers()
{
    TArray<UWebBrowser*> Out;
    for (TObjectIterator<UWebBrowser> It; It; ++It)
    {
        UWebBrowser* Candidate = *It;
        if (IsDrivableBrowser(Candidate))
        {
            Out.Add(Candidate);
        }
    }

    // Most-usable first: the largest on-screen browser (the full-frame live HUD) wins, so
    // SelectBrowser(0) targets it rather than a stale/background browser. Sorting a pointer
    // array passes the predicate dereferenced element references.
    Out.Sort([](const UWebBrowser& A, const UWebBrowser& B)
    {
        const FVector2D SA = A.GetCachedGeometry().GetLocalSize();
        const FVector2D SB = B.GetCachedGeometry().GetLocalSize();
        return (SA.X * SA.Y) > (SB.X * SB.Y);
    });

    return Out;
}

UWebBrowser* FDriveWebBridge::SelectBrowser(int32 Index)
{
    const TArray<UWebBrowser*> All = DiscoverBrowsers();
    return All.IsValidIndex(Index) ? All[Index] : nullptr;
}

// ====================================================================================
// FDriveWebBridge — async injection / await
// ====================================================================================

void FDriveWebBridge::ExecuteJsAwaitResult(
    UWebBrowser* Browser,
    const FString& JsExpr,
    TFunction<void(bool, const FString&)> OnDone,
    double TimeoutSeconds)
{
    if (!Browser)
    {
        if (OnDone)
        {
            OnDone(false, FString());
        }
        return;
    }

    const FString Marker = MakeMarker();
    const FString FullJs = BuildAwaitExprJs(Marker, JsExpr);
    StartAwait(Browser, FullJs, Marker, MoveTemp(OnDone), TimeoutSeconds);
}

void FDriveWebBridge::QueryElements(
    UWebBrowser* Browser,
    TFunction<void(bool, TArray<FDriveElement>)> OnDone,
    double TimeoutSeconds)
{
    if (!Browser)
    {
        if (OnDone)
        {
            OnDone(false, TArray<FDriveElement>());
        }
        return;
    }

    // Read the live geometry synchronously, now, on the game thread.
    FDriveWebViewport Viewport;
    Viewport.BrowserAbsolutePosition = Browser->GetCachedGeometry().GetAbsolutePosition();
    Viewport.FallbackScale = ResolveDpiScale(Browser);

    const FString Marker = MakeMarker();
    const FString FullJs = BuildQueryElementsJs(Marker);

    StartAwait(Browser, FullJs, Marker,
        [Viewport, OnDone = MoveTemp(OnDone)](bool bOk, const FString& Payload)
        {
            TArray<FDriveElement> Elements;
            if (!bOk)
            {
                if (OnDone)
                {
                    OnDone(false, MoveTemp(Elements));
                }
                return;
            }

            FString Error;
            const bool bParsed = FDriveWebBridge::ParseQueryResult(Payload, Viewport, Elements, Error);
            if (OnDone)
            {
                OnDone(bParsed, MoveTemp(Elements));
            }
        },
        TimeoutSeconds);
}

void FDriveWebBridge::ClickElement(
    UWebBrowser* Browser,
    const FString& Handle,
    TFunction<void(bool, FString)> OnDone,
    double TimeoutSeconds)
{
    if (!Browser)
    {
        if (OnDone)
        {
            OnDone(false, TEXT("NO_BROWSER"));
        }
        return;
    }

    const FString Marker = MakeMarker();
    const FString FullJs = BuildClickElementJs(Marker, Handle);

    StartAwait(Browser, FullJs, Marker,
        [OnDone = MoveTemp(OnDone)](bool bOk, const FString& Payload)
        {
            if (!bOk)
            {
                if (OnDone)
                {
                    OnDone(false, TEXT("TIMEOUT"));
                }
                return;
            }

            bool bActionOk = false;
            FString Code;
            FString Detail;
            if (!FDriveWebBridge::ParseActionResult(Payload, bActionOk, Code, Detail))
            {
                if (OnDone)
                {
                    OnDone(false, TEXT("MALFORMED_JSON"));
                }
                return;
            }

            if (OnDone)
            {
                OnDone(bActionOk, Code.IsEmpty() ? Detail : Code);
            }
        },
        TimeoutSeconds);
}

void FDriveWebBridge::TypeIntoElement(
    UWebBrowser* Browser,
    const FString& Handle,
    const FString& Text,
    TFunction<void(bool, FString)> OnDone,
    double TimeoutSeconds)
{
    if (!Browser)
    {
        if (OnDone)
        {
            OnDone(false, TEXT("NO_BROWSER"));
        }
        return;
    }

    const FString Marker = MakeMarker();
    const FString FullJs = BuildTypeIntoElementJs(Marker, Handle, Text);

    StartAwait(Browser, FullJs, Marker,
        [OnDone = MoveTemp(OnDone)](bool bOk, const FString& Payload)
        {
            if (!bOk)
            {
                if (OnDone)
                {
                    OnDone(false, TEXT("TIMEOUT"));
                }
                return;
            }

            bool bActionOk = false;
            FString Code;
            FString Detail;
            if (!FDriveWebBridge::ParseActionResult(Payload, bActionOk, Code, Detail))
            {
                if (OnDone)
                {
                    OnDone(false, TEXT("MALFORMED_JSON"));
                }
                return;
            }

            if (OnDone)
            {
                OnDone(bActionOk, Code.IsEmpty() ? Detail : Code);
            }
        },
        TimeoutSeconds);
}

void FDriveWebBridge::ScrollElement(
    UWebBrowser* Browser,
    const FString& Handle,
    double Delta,
    TFunction<void(bool, const FString&)> OnDone,
    double TimeoutSeconds)
{
    if (!Browser)
    {
        if (OnDone)
        {
            OnDone(false, TEXT("NO_BROWSER"));
        }
        return;
    }

    const FString Marker = MakeMarker();
    const FString FullJs = BuildScrollElementJs(Marker, Handle, Delta);

    StartAwait(Browser, FullJs, Marker,
        [OnDone = MoveTemp(OnDone)](bool bOk, const FString& Payload)
        {
            if (!bOk)
            {
                if (OnDone)
                {
                    OnDone(false, TEXT("TIMEOUT"));
                }
                return;
            }

            bool bActionOk = false;
            FString Code;
            FString Detail;
            if (!FDriveWebBridge::ParseActionResult(Payload, bActionOk, Code, Detail))
            {
                if (OnDone)
                {
                    OnDone(false, TEXT("MALFORMED_JSON"));
                }
                return;
            }

            if (OnDone)
            {
                OnDone(bActionOk, Code.IsEmpty() ? Detail : Code);
            }
        },
        TimeoutSeconds);
}

void FDriveWebBridge::HoverElement(
    UWebBrowser* Browser,
    const FString& Handle,
    TFunction<void(bool, const FString&)> OnDone,
    double TimeoutSeconds)
{
    if (!Browser)
    {
        if (OnDone)
        {
            OnDone(false, TEXT("NO_BROWSER"));
        }
        return;
    }

    const FString Marker = MakeMarker();
    const FString FullJs = BuildHoverElementJs(Marker, Handle);

    StartAwait(Browser, FullJs, Marker,
        [OnDone = MoveTemp(OnDone)](bool bOk, const FString& Payload)
        {
            if (!bOk)
            {
                if (OnDone)
                {
                    OnDone(false, TEXT("TIMEOUT"));
                }
                return;
            }

            bool bActionOk = false;
            FString Code;
            FString Detail;
            if (!FDriveWebBridge::ParseActionResult(Payload, bActionOk, Code, Detail))
            {
                if (OnDone)
                {
                    OnDone(false, TEXT("MALFORMED_JSON"));
                }
                return;
            }

            if (OnDone)
            {
                OnDone(bActionOk, Code.IsEmpty() ? Detail : Code);
            }
        },
        TimeoutSeconds);
}

void FDriveWebBridge::KeyElement(
    UWebBrowser* Browser,
    const FString& Handle,
    const FString& Key,
    const FString& Modifiers,
    const FString& Action,
    TFunction<void(bool, const FString&)> OnDone,
    double TimeoutSeconds)
{
    if (!Browser)
    {
        if (OnDone)
        {
            OnDone(false, TEXT("NO_BROWSER"));
        }
        return;
    }

    const FString Marker = MakeMarker();
    const FString FullJs = BuildKeyElementJs(Marker, Handle, Key, Modifiers, Action);

    StartAwait(Browser, FullJs, Marker,
        [OnDone = MoveTemp(OnDone)](bool bOk, const FString& Payload)
        {
            if (!bOk)
            {
                if (OnDone)
                {
                    OnDone(false, TEXT("TIMEOUT"));
                }
                return;
            }

            bool bActionOk = false;
            FString Code;
            FString Detail;
            if (!FDriveWebBridge::ParseActionResult(Payload, bActionOk, Code, Detail))
            {
                if (OnDone)
                {
                    OnDone(false, TEXT("MALFORMED_JSON"));
                }
                return;
            }

            if (OnDone)
            {
                OnDone(bActionOk, Code.IsEmpty() ? Detail : Code);
            }
        },
        TimeoutSeconds);
}

void FDriveWebBridge::DragElement(
    UWebBrowser* Browser,
    const FString& FromHandle,
    const FString& ToHandle,
    TFunction<void(bool, const FString&)> OnDone,
    double TimeoutSeconds)
{
    if (!Browser)
    {
        if (OnDone)
        {
            OnDone(false, TEXT("NO_BROWSER"));
        }
        return;
    }

    const FString Marker = MakeMarker();
    const FString FullJs = BuildDragElementJs(Marker, FromHandle, ToHandle);

    StartAwait(Browser, FullJs, Marker,
        [OnDone = MoveTemp(OnDone)](bool bOk, const FString& Payload)
        {
            if (!bOk)
            {
                if (OnDone)
                {
                    OnDone(false, TEXT("TIMEOUT"));
                }
                return;
            }

            bool bActionOk = false;
            FString Code;
            FString Detail;
            if (!FDriveWebBridge::ParseActionResult(Payload, bActionOk, Code, Detail))
            {
                if (OnDone)
                {
                    OnDone(false, TEXT("MALFORMED_JSON"));
                }
                return;
            }

            if (OnDone)
            {
                OnDone(bActionOk, Code.IsEmpty() ? Detail : Code);
            }
        },
        TimeoutSeconds);
}

// ====================================================================================
// FDriveWebBridge — pure helpers
// ====================================================================================

FString FDriveWebBridge::MakeMarker()
{
    return FString::Printf(TEXT("__PWDRIVE_%s__"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString FDriveWebBridge::EscapeJsString(const FString& In)
{
    FString Out;
    Out.Reserve(In.Len() + 8);
    for (const TCHAR C : In)
    {
        switch (C)
        {
        case TEXT('\\'): Out += TEXT("\\\\"); break;
        case TEXT('\''): Out += TEXT("\\'"); break;
        case TEXT('\"'): Out += TEXT("\\\""); break;
        case TEXT('\n'): Out += TEXT("\\n"); break;
        case TEXT('\r'): Out += TEXT("\\r"); break;
        case TEXT('\t'): Out += TEXT("\\t"); break;
        default: Out.AppendChar(C); break;
        }
    }
    return Out;
}

FString FDriveWebBridge::BuildAwaitExprJs(const FString& Marker, const FString& JsExpr)
{
    const FString M = EscapeJsString(Marker);
    return FString::Printf(
        TEXT("(function(){%s try{__pwwrite((%s));}")
        TEXT("catch(e){__pwwrite({pwError:String((e&&e.message)||e)});}})();"),
        *BuildResultWriterJs(M), *JsExpr);
}

FString FDriveWebBridge::BuildQueryElementsJs(const FString& Marker)
{
    const FString M = EscapeJsString(Marker);
    return FString::Printf(
        TEXT("(function(){%s try{")
        // Interactable selector. Native controls plus custom controls: any focusable element
        // ([tabindex], excluding tabindex="-1" which is programmatic-only, not user-tab-navigable)
        // and the interactive ARIA widget roles. Without these a custom control built from a
        // focusable div (e.g. <div class="pdd" tabindex="0" role="listbox">) matches neither IS
        // nor TS, so add() is never called on it and it is emitted with no clickable handle.
        TEXT("var IS='a,button,input,select,textarea,[role=button],[role=link],[role=checkbox],[role=radio],[role=switch],[role=tab],[role=menuitem],[role=menuitemcheckbox],[role=menuitemradio],[role=option],[role=combobox],[role=listbox],[role=slider],[role=spinbutton],[role=textbox],[role=searchbox],[role=treeitem],[onclick],[tabindex]:not([tabindex=\"-1\"])';")
        TEXT("var TS='h1,h2,h3,h4,h5,h6,p,span,label,li,td,th';")
        TEXT("var seen=new Set();var n=0;var out=[];")
        // Seed the mint counter past the highest existing pw-N stamp BEFORE minting any new handle.
        // data-pw-id stamps are intentionally persisted across observes (see the cleanup comment near
        // the top of this file / BuildCleanupJs), but n resets to 0 each call -- so without this seed
        // the first DOM-added interactable in a later observe re-mints pw-0, colliding with the reused
        // pw-0 stamp already in the same response; the first-match resolver (querySelector) then
        // silently routes the action to the wrong element. Scanning the existing stamps and starting n
        // at max+1 keeps every minted handle disjoint from every persistent stamp on the page.
        TEXT("var RE=/^pw-(\\d+)$/;document.querySelectorAll('[data-pw-id]').forEach(function(el){var m=RE.exec(el.getAttribute('data-pw-id'));if(m)n=Math.max(n,parseInt(m[1],10)+1);});")
        TEXT("function hid(el){var h=el.getAttribute('data-pw-id');if(!h){h='pw-'+(n++);el.setAttribute('data-pw-id',h);}return h;}")
        TEXT("function txt(el){var t=((el.innerText||el.value||el.getAttribute('aria-label'))||'').trim();return t.length>200?t.slice(0,200):t;}")
        TEXT("function vis(el){var s=window.getComputedStyle(el);return !(s.visibility==='hidden'||s.display==='none');}")
        TEXT("function add(el,ix){if(seen.has(el))return;var r=el.getBoundingClientRect();if(r.width<=0&&r.height<=0)return;var t=txt(el);if(!ix&&t.length===0)return;seen.add(el);")
        TEXT("out.push({handle:hid(el),tag:el.tagName.toLowerCase(),text:t,interactable:ix,enabled:!el.disabled,visible:vis(el),x:r.left,y:r.top,w:r.width,h:r.height});}")
        TEXT("document.querySelectorAll(IS).forEach(function(el){add(el,true);});")
        TEXT("document.querySelectorAll(TS).forEach(function(el){add(el,false);});")
        TEXT("__pwwrite({devicePixelRatio:window.devicePixelRatio||1,innerWidth:window.innerWidth,innerHeight:window.innerHeight,elements:out});")
        TEXT("}catch(e){__pwwrite({pwError:String((e&&e.message)||e),elements:[]});}})();"),
        *BuildResultWriterJs(M));
}

FString FDriveWebBridge::BuildClickElementJs(const FString& Marker, const FString& Handle)
{
    const FString M = EscapeJsString(Marker);
    const FString H = EscapeJsString(Handle);
    return FString::Printf(
        TEXT("(function(){%s try{")
        TEXT("var h='%s';")
        TEXT("var el=document.querySelector('[data-pw-id=\"'+h+'\"]');")
        TEXT("if(!el){__pwwrite({ok:false,code:'TARGET_NOT_FOUND'});return;}")
        TEXT("var s=window.getComputedStyle(el);var r=el.getBoundingClientRect();")
        TEXT("var visible=s.visibility!=='hidden'&&s.display!=='none'&&(r.width>0||r.height>0);")
        TEXT("if(!visible){__pwwrite({ok:false,code:'TARGET_CHANGED',detail:'element not visible'});return;}")
        TEXT("el.click();")
        TEXT("__pwwrite({ok:true,code:'OK'});")
        TEXT("}catch(e){__pwwrite({ok:false,code:'ACTION_FAILED',detail:String((e&&e.message)||e)});}})();"),
        *BuildResultWriterJs(M), *H);
}

FString FDriveWebBridge::BuildTypeIntoElementJs(const FString& Marker, const FString& Handle, const FString& Text)
{
    const FString M = EscapeJsString(Marker);
    const FString H = EscapeJsString(Handle);
    const FString T = EscapeJsString(Text);
    return FString::Printf(
        TEXT("(function(){%s try{")
        TEXT("var h='%s';var v='%s';")
        TEXT("var el=document.querySelector('[data-pw-id=\"'+h+'\"]');")
        TEXT("if(!el){__pwwrite({ok:false,code:'TARGET_NOT_FOUND'});return;}")
        TEXT("el.focus();")
        TEXT("if('value' in el){el.value=v;}else{el.textContent=v;}")
        TEXT("el.dispatchEvent(new Event('input',{bubbles:true}));")
        TEXT("el.dispatchEvent(new Event('change',{bubbles:true}));")
        TEXT("__pwwrite({ok:true,code:'OK'});")
        TEXT("}catch(e){__pwwrite({ok:false,code:'ACTION_FAILED',detail:String((e&&e.message)||e)});}})();"),
        *BuildResultWriterJs(M), *H, *T);
}

FString FDriveWebBridge::BuildScrollElementJs(const FString& Marker, const FString& Handle, double Delta)
{
    const FString M = EscapeJsString(Marker);
    const FString H = EscapeJsString(Handle);
    // SanitizeFloat is locale-independent (always a '.') and yields a valid JS numeric literal.
    const FString D = FString::SanitizeFloat(Delta);
    return FString::Printf(
        TEXT("(function(){%s try{")
        TEXT("var h='%s';var d=%s;")
        TEXT("var el=document.querySelector('[data-pw-id=\"'+h+'\"]');")
        TEXT("if(!el){__pwwrite({ok:false,code:'TARGET_NOT_FOUND'});return;}")
        TEXT("var s=window.getComputedStyle(el);var r=el.getBoundingClientRect();")
        TEXT("var visible=s.visibility!=='hidden'&&s.display!=='none'&&(r.width>0||r.height>0);")
        TEXT("if(!visible){__pwwrite({ok:false,code:'TARGET_CHANGED',detail:'element not visible'});return;}")
        TEXT("var cx=r.left+r.width/2;var cy=r.top+r.height/2;")
        TEXT("el.dispatchEvent(new WheelEvent('wheel',{deltaY:d,clientX:cx,clientY:cy,bubbles:true,cancelable:true}));")
        // Fallback so the scroll lands even with no wheel handler: scroll the element, then walk up
        // to the nearest scrollable ancestor and scroll that too.
        TEXT("if(el.scrollBy){el.scrollBy({top:d});}")
        TEXT("var sc=el.parentElement;while(sc&&sc.scrollHeight<=sc.clientHeight){sc=sc.parentElement;}")
        TEXT("if(sc&&sc.scrollBy){sc.scrollBy({top:d});}")
        TEXT("__pwwrite({ok:true,code:'OK'});")
        TEXT("}catch(e){__pwwrite({ok:false,code:'ACTION_FAILED',detail:String((e&&e.message)||e)});}})();"),
        *BuildResultWriterJs(M), *H, *D);
}

FString FDriveWebBridge::BuildHoverElementJs(const FString& Marker, const FString& Handle)
{
    const FString M = EscapeJsString(Marker);
    const FString H = EscapeJsString(Handle);
    return FString::Printf(
        TEXT("(function(){%s try{")
        TEXT("var h='%s';")
        TEXT("var el=document.querySelector('[data-pw-id=\"'+h+'\"]');")
        TEXT("if(!el){__pwwrite({ok:false,code:'TARGET_NOT_FOUND'});return;}")
        TEXT("var s=window.getComputedStyle(el);var r=el.getBoundingClientRect();")
        TEXT("var visible=s.visibility!=='hidden'&&s.display!=='none'&&(r.width>0||r.height>0);")
        TEXT("if(!visible){__pwwrite({ok:false,code:'TARGET_CHANGED',detail:'element not visible'});return;}")
        TEXT("var cx=r.left+r.width/2;var cy=r.top+r.height/2;")
        TEXT("var o={clientX:cx,clientY:cy,bubbles:true,cancelable:true};")
        TEXT("el.dispatchEvent(new PointerEvent('pointerover',o));")
        TEXT("el.dispatchEvent(new MouseEvent('mouseover',o));")
        TEXT("el.dispatchEvent(new MouseEvent('mouseenter',o));")
        TEXT("el.dispatchEvent(new MouseEvent('mousemove',o));")
        TEXT("__pwwrite({ok:true,code:'OK'});")
        TEXT("}catch(e){__pwwrite({ok:false,code:'ACTION_FAILED',detail:String((e&&e.message)||e)});}})();"),
        *BuildResultWriterJs(M), *H);
}

FString FDriveWebBridge::BuildKeyElementJs(
    const FString& Marker, const FString& Handle, const FString& Key,
    const FString& Modifiers, const FString& Action)
{
    const FString M = EscapeJsString(Marker);
    const FString H = EscapeJsString(Handle);
    const FString K = EscapeJsString(Key);
    const FString Mods = EscapeJsString(Modifiers);
    const FString A = EscapeJsString(Action);
    return FString::Printf(
        TEXT("(function(){%s try{")
        TEXT("var h='%s';var k='%s';var mods='%s';var act='%s';")
        TEXT("var el;")
        // Handle given: resolve + focus (missing handle is TARGET_NOT_FOUND). Else target the page's
        // current focus / body so a bare key still lands somewhere sensible.
        TEXT("if(h){el=document.querySelector('[data-pw-id=\"'+h+'\"]');")
        TEXT("if(!el){__pwwrite({ok:false,code:'TARGET_NOT_FOUND'});return;}el.focus();}")
        TEXT("else{el=document.activeElement||document.body;}")
        TEXT("var ml=mods.toLowerCase().split('+');")
        TEXT("var mo={ctrlKey:ml.indexOf('ctrl')>=0,shiftKey:ml.indexOf('shift')>=0,")
        TEXT("altKey:ml.indexOf('alt')>=0,metaKey:ml.indexOf('meta')>=0};")
        TEXT("function ev(t){return new KeyboardEvent(t,{key:k,bubbles:true,cancelable:true,")
        TEXT("ctrlKey:mo.ctrlKey,shiftKey:mo.shiftKey,altKey:mo.altKey,metaKey:mo.metaKey});}")
        TEXT("if(act==='down'){el.dispatchEvent(ev('keydown'));}")
        TEXT("else if(act==='up'){el.dispatchEvent(ev('keyup'));}")
        TEXT("else{el.dispatchEvent(ev('keydown'));el.dispatchEvent(ev('keypress'));el.dispatchEvent(ev('keyup'));}")
        TEXT("__pwwrite({ok:true,code:'OK'});")
        TEXT("}catch(e){__pwwrite({ok:false,code:'ACTION_FAILED',detail:String((e&&e.message)||e)});}})();"),
        *BuildResultWriterJs(M), *H, *K, *Mods, *A);
}

FString FDriveWebBridge::BuildDragElementJs(const FString& Marker, const FString& FromHandle, const FString& ToHandle)
{
    const FString M = EscapeJsString(Marker);
    const FString F = EscapeJsString(FromHandle);
    const FString T = EscapeJsString(ToHandle);
    return FString::Printf(
        TEXT("(function(){%s try{")
        TEXT("var fh='%s';var th='%s';")
        TEXT("var fe=document.querySelector('[data-pw-id=\"'+fh+'\"]');")
        TEXT("var te=document.querySelector('[data-pw-id=\"'+th+'\"]');")
        TEXT("if(!fe||!te){__pwwrite({ok:false,code:'TARGET_NOT_FOUND'});return;}")
        TEXT("var fr=fe.getBoundingClientRect();var tr=te.getBoundingClientRect();")
        TEXT("var fx=fr.left+fr.width/2;var fy=fr.top+fr.height/2;")
        TEXT("var tx=tr.left+tr.width/2;var ty=tr.top+tr.height/2;")
        TEXT("function mev(el,t,x,y){el.dispatchEvent(new MouseEvent(t,{clientX:x,clientY:y,bubbles:true,cancelable:true}));}")
        TEXT("function pev(el,t,x,y){el.dispatchEvent(new PointerEvent(t,{clientX:x,clientY:y,bubbles:true,cancelable:true}));}")
        TEXT("pev(fe,'pointerdown',fx,fy);mev(fe,'mousedown',fx,fy);")
        TEXT("pev(fe,'pointermove',fx,fy);mev(fe,'mousemove',fx,fy);")
        TEXT("pev(te,'pointermove',tx,ty);mev(te,'mousemove',tx,ty);")
        TEXT("pev(te,'pointerup',tx,ty);mev(te,'mouseup',tx,ty);")
        TEXT("__pwwrite({ok:true,code:'OK'});")
        TEXT("}catch(e){__pwwrite({ok:false,code:'ACTION_FAILED',detail:String((e&&e.message)||e)});}})();"),
        *BuildResultWriterJs(M), *F, *T);
}

FString FDriveWebBridge::BuildCleanupJs()
{
    // Remove the hidden result node by its fixed id. The writer (BuildResultWriterJs) is
    // create-if-missing, so dropping the node here does not break the next query — it re-creates
    // it. The `data-pw-id` handles stamped on elements are deliberately left alone (see header).
    return TEXT(
        "(function(){var n=document.getElementById('__pwdrive_result');"
        "if(n&&n.parentNode)n.parentNode.removeChild(n);})();");
}

bool FDriveWebBridge::ExtractMarkedPayload(const FString& Html, const FString& Marker, FString& OutPayload)
{
    OutPayload.Reset();
    if (Marker.IsEmpty())
    {
        return false;
    }

    const int32 Index = Html.Find(Marker, ESearchCase::CaseSensitive, ESearchDir::FromStart);
    if (Index == INDEX_NONE)
    {
        return false;
    }

    // The payload is the base64 run right after the marker; it ends at the first non-base64
    // char, i.e. the result node's closing tag in the surrounding serialized HTML.
    const int32 Start = Index + Marker.Len();
    int32 End = Start;
    while (End < Html.Len() && IsBase64Char(Html[End]))
    {
        ++End;
    }

    OutPayload = Html.Mid(Start, End - Start);
    return true;
}

bool FDriveWebBridge::ParseQueryResult(
    const FString& Json,
    const FDriveWebViewport& Viewport,
    TArray<FDriveElement>& OutElements,
    FString& OutError)
{
    OutElements.Reset();
    OutError.Reset();

    const TSharedPtr<FJsonObject> Root = DeserializeObject(Json);
    if (!Root.IsValid())
    {
        OutError = TEXT("MALFORMED_JSON");
        return false;
    }

    FString JsError;
    if (Root->TryGetStringField(TEXT("pwError"), JsError))
    {
        OutError = FString::Printf(TEXT("JS_ERROR: %s"), *JsError);
        return false;
    }

    // The page's devicePixelRatio is the authoritative CSS-px -> screen-px scale; the
    // Slate window DPI is only a fallback when the payload omits it.
    double Scale = Viewport.FallbackScale;
    double DevicePixelRatio = 0.0;
    if (Root->TryGetNumberField(TEXT("devicePixelRatio"), DevicePixelRatio) && DevicePixelRatio > 0.0)
    {
        Scale = DevicePixelRatio;
    }

    const TArray<TSharedPtr<FJsonValue>>* Elems = nullptr;
    if (Root->TryGetArrayField(TEXT("elements"), Elems) && Elems)
    {
        OutElements.Reserve(Elems->Num());
        for (const TSharedPtr<FJsonValue>& Value : *Elems)
        {
            const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
            if (!Value.IsValid() || !Value->TryGetObject(ObjPtr) || !ObjPtr)
            {
                continue;
            }
            const TSharedPtr<FJsonObject>& E = *ObjPtr;

            FDriveElement Element;
            Element.Handle = ReadStr(E, TEXT("handle"));
            Element.Type = ReadStr(E, TEXT("tag"));
            Element.Label = ReadStr(E, TEXT("text"));
            Element.bInteractable = ReadBool(E, TEXT("interactable"), false);
            Element.bEnabled = ReadBool(E, TEXT("enabled"), true);
            Element.bVisible = ReadBool(E, TEXT("visible"), true);
            Element.bFocused = false;
            Element.Surface = EDriveSurface::Web;

            // DOM/CSS rect -> Slate absolute screen space.
            const double X = ReadNum(E, TEXT("x"));
            const double Y = ReadNum(E, TEXT("y"));
            const double W = ReadNum(E, TEXT("w"));
            const double H = ReadNum(E, TEXT("h"));
            Element.AbsolutePosition = FVector2D(
                Viewport.BrowserAbsolutePosition.X + X * Scale,
                Viewport.BrowserAbsolutePosition.Y + Y * Scale);
            Element.AbsoluteSize = FVector2D(W * Scale, H * Scale);

            OutElements.Add(MoveTemp(Element));
        }
    }

    return true;
}

bool FDriveWebBridge::ParseActionResult(const FString& Json, bool& bOutOk, FString& OutCode, FString& OutDetail)
{
    bOutOk = false;
    OutCode.Reset();
    OutDetail.Reset();

    const TSharedPtr<FJsonObject> Root = DeserializeObject(Json);
    if (!Root.IsValid())
    {
        OutCode = TEXT("MALFORMED_JSON");
        return false;
    }

    bOutOk = ReadBool(Root, TEXT("ok"), false);
    OutCode = ReadStr(Root, TEXT("code"));
    OutDetail = ReadStr(Root, TEXT("detail"));
    return true;
}
