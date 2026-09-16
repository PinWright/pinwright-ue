// Copyright (c) 2026 Alexander Penkin. MIT License.

// Shared derivation of the per-file source-control status flags returned by
// source_control.status. Kept in a header so the handler and its regression
// test drive the SAME derivation (not a copy).
#pragma once

#include "CoreMinimal.h"
#include "ISourceControlState.h"
#include "Dom/JsonObject.h"

namespace PinWright::SourceControl
{
    // Write the per-file boolean status flags (isCheckedOut / isAdded /
    // isDeleted / isModified / isUnchanged / canCheckOut / isUnknown) for one
    // asset into Out, from its engine ISourceControlState.
    //
    // isUnchanged ("pristine" / clean-from-the-depot) DELIBERATELY does NOT fold
    // in IsCheckedOut(). The Git provider is lockless, so
    // FGitSourceControlState::IsCheckedOut() returns IsSourceControlled() and
    // EVERY tracked file — clean or dirty — reports checked-out. An earlier
    // derivation "!IsModified() && !IsAdded() && !IsDeleted() && !IsCheckedOut()"
    // therefore pinned isUnchanged=false for every clean git file, so the one
    // field whose name promises "unchanged from the depot" could never say true
    // under Git. A file is unchanged iff it is under source control and carries
    // no pending add / delete / modify. Folding in IsSourceControlled() also
    // flips a not-under-control file (no pending change, but not "unchanged from
    // the depot") to isUnchanged=false, where the old formula wrongly said true.
    inline void WriteStateFlags(const ISourceControlState& State, FJsonObject& Out)
    {
        Out.SetBoolField(TEXT("isCheckedOut"), State.IsCheckedOut());
        Out.SetBoolField(TEXT("isAdded"), State.IsAdded());
        Out.SetBoolField(TEXT("isDeleted"), State.IsDeleted());
        Out.SetBoolField(TEXT("isModified"), State.IsModified());
        Out.SetBoolField(TEXT("isUnchanged"),
            State.IsSourceControlled() && !State.IsModified() && !State.IsAdded() && !State.IsDeleted());
        Out.SetBoolField(TEXT("canCheckOut"), State.CanCheckout());
        Out.SetBoolField(TEXT("isUnknown"), State.IsUnknown());
    }
}
