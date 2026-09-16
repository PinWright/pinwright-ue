// Copyright (c) 2026 Alexander Penkin. MIT License.

// Pure helpers for editor.play's `networkEmulation` param: parse/validate the
// requested PIE network-emulation state ({enabled, target, profile}) into a
// canonical FEmulationSpec, with the available profile list passed in by the
// caller so validation stays engine-global-free and unit-testable (same pure-
// helper pattern as PieWorldSelector.h). The engine-touching apply/read side
// (transient ULevelEditorPlaySettings duplicate, active-session read-back)
// lives in PieControlUtils.h. Named-namespace inline functions, Unity-merge safe.
#pragma once

#include "CoreMinimal.h"

namespace PieNetworkEmulation
{

// Wire-level emulation target, mirroring the engine's NetworkEmulationTarget
// (Server / Client / Any) without referencing the UnrealEd header so this file
// stays pure. Mapping to the engine enum happens in PieControlUtils.
enum class EEmulationTarget : uint8
{
    ServerOnly,   // "serverOnly"  -> NetworkEmulationTarget::Server
    ClientsOnly,  // "clientsOnly" -> NetworkEmulationTarget::Client
    Everyone      // "everyone"    -> NetworkEmulationTarget::Any
};

// Canonical, validated emulation request for one PIE session. Default state is
// the editor.play default: emulation FORCE-DISABLED regardless of the user's
// saved play settings (deterministic wire unless explicitly asked for).
struct FEmulationSpec
{
    bool bEnabled = false;
    EEmulationTarget Target = EEmulationTarget::ServerOnly;
    FString Profile;  // canonical config profile name; only meaningful when bEnabled
};

inline const TCHAR* TargetToString(EEmulationTarget Target)
{
    switch (Target)
    {
    case EEmulationTarget::ClientsOnly:
        return TEXT("clientsOnly");
    case EEmulationTarget::Everyone:
        return TEXT("everyone");
    default:
        return TEXT("serverOnly");
    }
}

// Case-insensitive parse of the wire target names. Returns false on anything
// that is not exactly serverOnly / clientsOnly / everyone.
inline bool ParseTarget(const FString& Text, EEmulationTarget& OutTarget)
{
    if (Text.Equals(TEXT("serverOnly"), ESearchCase::IgnoreCase))
    {
        OutTarget = EEmulationTarget::ServerOnly;
        return true;
    }
    if (Text.Equals(TEXT("clientsOnly"), ESearchCase::IgnoreCase))
    {
        OutTarget = EEmulationTarget::ClientsOnly;
        return true;
    }
    if (Text.Equals(TEXT("everyone"), ESearchCase::IgnoreCase))
    {
        OutTarget = EEmulationTarget::Everyone;
        return true;
    }
    return false;
}

// Validates and canonicalizes the raw `networkEmulation` fields against the
// configured profile list (pass PieControlUtils::GetAvailableEmulationProfiles()).
//
// - TargetStr / ProfileStr are validated whenever non-empty, even with
//   bEnabled == false, so a typo never silently degrades to the default.
// - When enabled: Target defaults to serverOnly, Profile defaults to "Average".
//   Profile names match the config list case-insensitively and are canonicalized
//   to the config casing (LoadEmulationProfile builds an ini section name from
//   the string, so the canonical spelling is the honest one to echo back).
// - The details-panel-only "Custom" pseudo-profile is NOT in the config list and
//   is rejected like any unknown name: accepting it would replay whatever packet
//   values sit in the user's saved settings — the nondeterminism this param
//   exists to eliminate.
// Returns false with a caller-facing OutError (listing the valid values) on any
// validation failure.
inline bool BuildEmulationSpec(bool bEnabled, const FString& TargetStr, const FString& ProfileStr,
    const TArray<FString>& AvailableProfiles, FEmulationSpec& OutSpec, FString& OutError)
{
    FEmulationSpec Spec;
    Spec.bEnabled = bEnabled;

    if (!TargetStr.IsEmpty() && !ParseTarget(TargetStr, Spec.Target))
    {
        OutError = FString::Printf(
            TEXT("Unknown networkEmulation.target '%s'. Valid values: serverOnly, clientsOnly, everyone."),
            *TargetStr);
        return false;
    }

    const FString RequestedProfile = ProfileStr.IsEmpty() ? FString(TEXT("Average")) : ProfileStr;
    const FString* CanonicalProfile = AvailableProfiles.FindByPredicate(
        [&RequestedProfile](const FString& Name)
        {
            return Name.Equals(RequestedProfile, ESearchCase::IgnoreCase);
        });
    if (CanonicalProfile)
    {
        Spec.Profile = *CanonicalProfile;
    }
    else if (!ProfileStr.IsEmpty() || bEnabled)
    {
        // An explicit unknown name is always an error; a missing default
        // ("Average" stripped from the project config) only matters when the
        // caller actually asked for emulation.
        OutError = FString::Printf(
            TEXT("Unknown networkEmulation.profile '%s'. Available profiles (from [/Script/Engine.NetworkSettings] NetworkEmulationProfiles): %s."),
            *RequestedProfile,
            AvailableProfiles.Num() > 0 ? *FString::Join(AvailableProfiles, TEXT(", ")) : TEXT("none configured"));
        return false;
    }

    if (!Spec.bEnabled)
    {
        // Disabled sessions carry no profile — nothing is in effect.
        Spec.Profile.Reset();
    }

    OutSpec = Spec;
    OutError.Reset();
    return true;
}

} // namespace PieNetworkEmulation
