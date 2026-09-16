// Copyright (c) 2026 Alexander Penkin. MIT License.

#pragma once

#include "CoreMinimal.h"

// Keeping <Project>/Saved/PinWright/gateway-port truthful when THIS editor is not serving.
//
// THE DEFECT THIS EXISTS FOR. The port file was written only on a successful bind, on a
// stated "last-known-good survives failures" policy, and never retracted. An editor whose
// bind lost therefore left the file naming a port from some earlier session while serving
// nothing itself - and the stdio proxy re-resolves the endpoint from that file before every
// call, so the caller was confidently aimed at an endpoint no editor of this project was
// answering. The bounded rebind (Transport/BindRetryPolicy.h) fixed the editor half; this is
// the advertisement half, and neither is sufficient alone.
//
// WHY THE VERDICT IS NOT SIMPLY "DELETE IT". A second editor of the same project losing the
// bind is the NORMAL case, and in it the file is CORRECT: the first editor holds that port
// and is serving on it. Deleting the advertisement there would break the editor that is
// actually working, on behalf of the one that is not. The claim may only be retracted when
// it is provably false, and the only evidence available in-process that settles it is
// whether anything at all is listening on the advertised port.
//
// The judgement is therefore split: a pure verdict that can be tested without a socket, and
// one impure probe that supplies its single input.
namespace PortAdvertisement
{

// What to do with the advertisement currently on disk.
enum class EVerdict : uint8
{
    // Nothing is advertised; there is no claim to be wrong about.
    NoClaim,
    // Something is listening on the advertised port. It may be another editor of this
    // project serving normally, or an unrelated process - this side of the wire cannot
    // tell, and both readings argue for leaving the file alone rather than deleting a
    // working editor's endpoint on a guess.
    Retain,
    // Nothing is listening on the advertised port and this editor is not serving either,
    // so the claim is false no matter who wrote it. Retract it: a proxy that finds no file
    // reports the editor as not running, which is the truth, instead of dialling a dead
    // endpoint and reporting whatever the OS says about it.
    Retract
};

// The pure decision. bHasClaim is "the port file names a port"; bSomethingListening is the
// probe's answer for that port. Deliberately has no notion of WHICH port we lost or who
// wrote the file: ownership is unknowable across a crash, liveness is not.
inline EVerdict Judge(bool bHasClaim, bool bSomethingListening)
{
    if (!bHasClaim)
    {
        return EVerdict::NoClaim;
    }
    return bSomethingListening ? EVerdict::Retain : EVerdict::Retract;
}

// True when the exact loopback endpoint cannot be claimed by this probe. A successful bind is
// treated as definite "nothing there"; every other outcome (including a bind failure or no socket
// subsystem) is reported as listening, because the verdict above only retracts on proof and an
// inconclusive probe is not proof. TimeoutSeconds remains in the interface for callers that use
// the prior bounded-probe contract.
bool IsSomethingListening(int32 Port, double TimeoutSeconds = 0.25);

// Reads the advertisement, probes it, applies Judge, and logs the outcome. Call on the game
// thread from the not-serving paths only - never while this editor holds the port, or it
// would probe and reason about its own listener.
EVerdict ReconcileWhileNotServing();

} // namespace PortAdvertisement
