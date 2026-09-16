// Copyright (c) 2026 Alexander Penkin. MIT License.

// PinWrightProgressLibrary.h - the Python-callable progress-reporting surface.
//
// The problem it solves: python.execute runs a user script synchronously on the game
// thread. While that script runs, the editor services no RPCs, so nothing outside the
// script can observe it - a per-actor sweep over this project's level wedged an editor
// for 168 minutes with no way to tell a slow run from a hung one.
//
// The script itself, however, is executing ON the game thread, not waiting for it. So a
// call it makes back into C++ runs immediately, and FSocketHttpServer::WriteStreamFrame
// only ENQUEUES bytes onto a connection's outbound queue (SocketHttpServer.cpp:1063) that
// the transport's own I/O thread drains. A progress frame reported from inside the loop
// therefore reaches the client's socket while the loop is still running, without the game
// thread returning to the tick loop at all.
//
// The limit is exact and worth stating: this reports progress BETWEEN units of work, not
// inside one. A single blocking engine call - EditorAssetLibrary.rename_directory loading
// 237 assets, one mesh build, one package save - has no point at which the script gets
// control back, so nothing can be emitted until it returns. Chunkable loops are
// observable; irreducible engine calls are not.
//
// Following PinWrightPackageLibrary: every function is static and BlueprintCallable on a
// UBlueprintFunctionLibrary in an Editor module with LoadingPhase "Default", which is the
// ordinary route to `unreal.PinWrightProgressLibrary.<snake_case_name>()`.
//
// No out-parameters, deliberately, for the reason recorded on PinWrightPackageLibrary:
// PyGenUtil folds a return value plus out-params into a Python TUPLE, and a non-empty
// tuple is always truthy - so `if lib.report_progress(...)` would silently pass.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PinWrightProgressLibrary.generated.h"

UCLASS()
class PINWRIGHT_API UPinWrightProgressLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /**
     * Report one unit of progress against the RPC currently executing this code, which
     * is delivered to the caller as an MCP notifications/progress frame.
     *
     * Message is the human-readable line the client displays ("grounding actor 412 of
     * 1660"). Progress is the caller's own numerator; Total is the denominator, or 0
     * when it is not known - an unknown total is omitted from the frame rather than sent
     * as zero, because a zero total renders as a completed bar.
     *
     * **Safe to call every iteration of a tight loop.** Reports are rate-limited to one
     * per "Min progress event interval (ms)" (Editor Preferences, default 1000), so a
     * script does not have to remember to report only every Nth item - the mechanism
     * handles it. Without that limit a loop over this project's 3283 actors, which takes
     * 0.1 s, would put ~3283 notifications on the client in a tenth of a second.
     *
     * Returns true only when the event was accepted and put on the wire. False is NOT an
     * error, must not abort the caller's work, and must not stop it reporting. The
     * legitimate causes are (a) the report was rate-limited - the common case in a fast
     * loop; (b) the request was not a streaming request, because the caller sent no
     * params._meta.progressToken or sent args.wait=false, so there is no open stream;
     * (c) the RPC has already reached a terminal state; (d) called off the game thread.
     * Progress reporting is strictly additive - a script that ignores the return value
     * behaves identically whether or not anyone is watching.
     *
     * MCP requires the progress value to increase with every notification. A value that
     * would go backwards is clamped forward and the raw value the caller passed is kept
     * in the Saved/PinWright/jobs.jsonl audit line, so the wire stays conformant without
     * the record losing what was actually reported.
     *
     * Reporting between iterations of a loop is the intended use. It cannot interrupt a
     * single blocking engine call - see the file header.
     */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Progress",
              meta = (AdvancedDisplay = "Total"))
    static bool ReportProgress(const FString& Message, float Progress, float Total);

    /**
     * True when a client is currently streaming this RPC and would receive a
     * ReportProgress call. Use it to skip building an expensive progress message, not to
     * decide whether to do the work: the work must be identical either way.
     */
    UFUNCTION(BlueprintCallable, Category = "PinWright|Progress")
    static bool IsProgressObserved();
};
