// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/Optional.h"
#include "Misc/Parse.h"

#include "Handlers/ErrorCodes.h"

// Second guard for the two raw-console verbs (system.console_command, editor.console_command),
// beside ScalabilityConsoleGuard.h. It refuses the console lines that reach an effect a typed
// PinWright verb deliberately gates, or that exist only to kill or wedge the process
// (board B-console-verbs-bypass-typed-verb-guards):
//
//   - QUIT_EDITOR / CLOSE_SLATE_MAINFRAME (EditorServer.cpp:5993-6001 on 5.8) close the editor
//     without editor.quit's EDITOR_IN_USE / UNSAVED_CHANGES refusals, asset-editor close and job
//     termination. EXIT / QUIT are NOT aliases here: in the editor only ULocalPlayer::Exec_Editor
//     handles them (ends PIE, LocalPlayer.cpp:1620), and GEditor->Exec never routes there;
//   - PY (PythonScriptPlugin.cpp:1060) shares python.execute's SafePoint deferral but skips the
//     rest of that handler: private scope with sys.modules restore, captured log output, the
//     PIE-active warning and the leaked tick/shutdown callback report;
//   - EXECFILE (EditorServer.cpp:329-338) runs every line of a file through Exec, and only the
//     outer line reaches either guard;
//   - DEBUG <crash|hang|memory subcommand> (UEngine::PerformError / PerformBlockingError,
//     compiled into every non-Shipping build, editor included).
//
// Not refused: MACRO / EXEC only open a "deprecated command" dialog (EditorServer.cpp:325-328),
// which ScopedUnattendedRpc suppresses during dispatch; DEBUG HITCH / RENDERHITCH (bounded
// profiling sleeps), RESETLOADERS and LONGLOG.
//
// Words are matched with FParse::Command itself, so the guard sees exactly what the engine's
// exec handlers see: case-insensitive, and a word ends at the first non-alphanumeric character
// (Parse.cpp:787-791), which means `PY.foo` and `DEBUG CRASH_now` are refused because the
// engine runs them. The line is TrimStart()ed first; FParse::Command skips only space and tab,
// so that trim can only over-refuse, never let a line through.
namespace ConsoleCommandGuard
{
    struct FRefusal
    {
        const TCHAR* Code = nullptr;
        // The matched command words, e.g. "QUIT_EDITOR" or "DEBUG CRASH".
        FString MatchedCommand;
        // The typed verb to use instead; empty when there is none.
        FString UseVerb;
        FString Message;

        TSharedRef<FJsonObject> ToJson() const
        {
            TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
            Data->SetStringField(TEXT("refusedCommand"), MatchedCommand);
            if (!UseVerb.IsEmpty())
            {
                Data->SetStringField(TEXT("useVerb"), UseVerb);
            }
            Data->SetBoolField(TEXT("forceOverrides"), true);
            return Data;
        }
    };

    // DEBUG subcommands by effect: the union of UEngine::PerformError and PerformBlockingError
    // on UE 5.3 through 5.8 (RHICRASH and USEAFTERFREEMEMSTACK are 5.8-only; the USEAFTERFREE
    // pair exists only under USING_ADDRESS_SANITISER). ENSURE variants do not exit, but capture a
    // callstack and write a crash report, so they sit with the crashes.
    inline const TCHAR* const DebugCrashWords[] = {
        TEXT("CRASH"), TEXT("GPF"), TEXT("CHECK"), TEXT("FATAL"), TEXT("ENSURE"),
        TEXT("ENSUREALWAYS"), TEXT("BUFFEROVERRUN"), TEXT("CRTINVALID"), TEXT("RECURSE"),
        TEXT("STACKOVERFLOW"), TEXT("RENDERCRASH"), TEXT("RENDERCHECK"), TEXT("RENDERGPF"),
        TEXT("RENDERFATAL"), TEXT("RENDERENSURE"), TEXT("RHICRASH"), TEXT("GPUCRASH"),
        TEXT("THREADCRASH"), TEXT("THREADCHECK"), TEXT("THREADGPF"), TEXT("THREADEDCHECKS"),
        TEXT("TWOTHREADSCRASH"), TEXT("TWOTHREADSGPF"), TEXT("THREADENSURE"),
        TEXT("THREADFATAL"), TEXT("THREADRECURSE"), TEXT("THREADSTACKOVERFLOW"),
        TEXT("AUDIOGPF"), TEXT("AUDIOCHECK"), TEXT("CRASHCONTEXTWRITER"), TEXT("TERMINATE"),
        TEXT("ABORT"), TEXT("USEAFTERFREE"), TEXT("USEAFTERFREEMEMSTACK"),
    };
    // STALL defaults to 10 s, SLEEP to an hour, SOFTLOCK / INFINITELOOP never return. SPIN and
    // RENDERSPIN are listed by the board ticket; HITCH / RENDERHITCH are left to the caller.
    inline const TCHAR* const DebugHangWords[] = {
        TEXT("STALL"), TEXT("SPIN"), TEXT("RENDERSPIN"), TEXT("SLEEP"), TEXT("SOFTLOCK"),
        TEXT("INFINITELOOP"),
    };
    inline const TCHAR* const DebugMemoryWords[] = {
        TEXT("EATMEM"), TEXT("OOM"), TEXT("ALLOC"), TEXT("GPUALLOC"),
    };

    // Matches Word at the head of Stream without consuming it on a miss. On a hit Stream is
    // advanced past the word and its trailing blanks, as FParse::Command does.
    inline bool MatchWord(const TCHAR*& Stream, const TCHAR* Word)
    {
        const TCHAR* Probe = Stream;
        // bParseMightTriggerExecution=false: the guard never executes anything, and `true` makes
        // FParse::Command answer false while the console command library dump is active.
        if (FParse::Command(&Probe, Word, /*bParseMightTriggerExecution=*/false))
        {
            Stream = Probe;
            return true;
        }
        return false;
    }

    template <int32 N>
    inline const TCHAR* FindWord(const TCHAR* Stream, const TCHAR* const (&Words)[N])
    {
        for (const TCHAR* Word : Words)
        {
            if (MatchWord(Stream, Word))
            {
                return Word;
            }
        }
        return nullptr;
    }

    inline FString ForceSentence()
    {
        return TEXT(" Pass force:true to run this line anyway.");
    }

    // Unset when the line is allowed.
    inline TOptional<FRefusal> FindRefusal(const FString& CommandLine)
    {
        const FString Trimmed = CommandLine.TrimStart();
        const TCHAR* Stream = *Trimmed;
        FRefusal Refusal;

        for (const TCHAR* Word : { TEXT("QUIT_EDITOR"), TEXT("CLOSE_SLATE_MAINFRAME") })
        {
            if (MatchWord(Stream, Word))
            {
                Refusal.Code = ErrorCodes::ERR_EDITOR_QUIT_USE_TYPED_VERB;
                Refusal.MatchedCommand = Word;
                Refusal.UseVerb = TEXT("editor.quit");
                Refusal.Message = FString::Printf(
                    TEXT("Refusing '%s': %s closes the editor without the checks editor.quit makes. "
                         "editor.quit refuses while another client drove the editor in the last "
                         "5 minutes (EDITOR_IN_USE) or while user packages are unsaved "
                         "(UNSAVED_CHANGES), ends PIE and closes open asset editors before shutdown, "
                         "and terminates running jobs so streaming clients get a terminal event. "
                         "Use editor.quit.%s"),
                    *CommandLine, Word, *ForceSentence());
                return Refusal;
            }
        }

        if (MatchWord(Stream, TEXT("PY")))
        {
            Refusal.Code = ErrorCodes::ERR_PYTHON_USE_TYPED_VERB;
            Refusal.MatchedCommand = TEXT("PY");
            Refusal.UseVerb = TEXT("python.execute");
            Refusal.Message = FString::Printf(
                TEXT("Refusing '%s': console PY runs Python without python.execute's protections: "
                     "the private scope with sys.modules restore, log output captured into the "
                     "response, the PIE-active warning, and the report of tick or shutdown "
                     "callbacks the script leaked. Use python.execute.%s"),
                *CommandLine, *ForceSentence());
            return Refusal;
        }

        if (MatchWord(Stream, TEXT("EXECFILE")))
        {
            Refusal.Code = ErrorCodes::ERR_EXECFILE_SEND_LINES_INDIVIDUALLY;
            Refusal.MatchedCommand = TEXT("EXECFILE");
            Refusal.Message = FString::Printf(
                TEXT("Refusing '%s': EXECFILE runs every line of the file through Exec, and only "
                     "this outer line is checked, so the file's lines bypass this guard and the "
                     "scalability-CVar guard. Send the lines individually through this verb.%s"),
                *CommandLine, *ForceSentence());
            return Refusal;
        }

        if (MatchWord(Stream, TEXT("DEBUG")))
        {
            const TCHAR* Effect = nullptr;
            const TCHAR* Sub = nullptr;
            if ((Sub = FindWord(Stream, DebugCrashWords)) != nullptr)
            {
                Refusal.Code = ErrorCodes::ERR_DEBUG_COMMAND_CRASHES_PROCESS;
                Effect = TEXT("crashes the editor process (ENSURE variants capture a callstack and "
                              "write a crash report instead of exiting)");
            }
            else if ((Sub = FindWord(Stream, DebugHangWords)) != nullptr)
            {
                Refusal.Code = ErrorCodes::ERR_DEBUG_COMMAND_HANGS_PROCESS;
                Effect = TEXT("blocks the game or render thread for seconds to forever");
            }
            else if ((Sub = FindWord(Stream, DebugMemoryWords)) != nullptr)
            {
                Refusal.Code = ErrorCodes::ERR_DEBUG_COMMAND_EXHAUSTS_MEMORY;
                Effect = TEXT("allocates memory until the process runs out or leaks it deliberately");
            }
            if (Sub)
            {
                Refusal.MatchedCommand = FString::Printf(TEXT("DEBUG %s"), Sub);
                Refusal.Message = FString::Printf(
                    TEXT("Refusing '%s': DEBUG %s is an engine fault-injection command that %s. "
                         "In a shared editor that is every connected client's session. DEBUG HITCH "
                         "and DEBUG RENDERHITCH are not refused.%s"),
                    *CommandLine, Sub, Effect, *ForceSentence());
                return Refusal;
            }
        }

        return {};
    }
}
