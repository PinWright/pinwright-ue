// Copyright (c) 2026 Alexander Penkin. MIT License.

// Regression test for E-networking-wiki-stub-no-readback-contract:
// networking.get_networking_info is the namespace's lone structured reader, but
// its RETURN shape was documented on no wiki page — an agent had to read
// NetworkingHandler.cpp to learn which fields round-trip. Because Handlers/
// ParamSpec.h has no RPC_RETURN macro, the auto-content layer cannot emit a
// return shape, so a reader's output contract must be hand-authored in its
// overlay (as actor.md / blueprint.md do for their readers).
//
// The fix adds a `### networking.get_networking_info` H3 section to
// docs/wiki-src/networking.md documenting the CURRENT branch-dependent readback
// contract (verified against HEAD, post the F-networking-info-no-rpc-detail fix
// d96d57b): blueprintPath emits the 8 CDO fields + rpcFunctions[]
// {name,rpcType,reliable,withValidation} + replicatedProperties[]
// {name,replicated,replicatedUsing,replicationCondition}; actorName emits the 8
// CDO fields + role/remoteRole/hasAuthority + owner/ownerName; and the write-only
// setter configure_replicated_movement does NOT round-trip through this reader.
//
// This exercises the live WikiHandler::RenderPage render path (the same entry the
// HTTP gateway uses for doc requests) for the METHOD page, not a copy of the
// overlay text. `### method.name` H3 sections surface on the method page only.
// The auto-generated get_networking_info method page carries only the summary +
// the blueprintPath/actorName param table, so every field/method token asserted
// below is overlay-exclusive: reverting the H3 section makes WikiHandler render
// the auto content alone and these assertions fail. Loads no game content.
#include "Misc/AutomationTest.h"
#include "Tests/Infra/WikiDocTestHelpers.h"

// ============================================================================
// Method page: the `### networking.get_networking_info` H3 section documents the
// branch-dependent readback contract. These markers exist only in that overlay
// section of docs/wiki-src/networking.md.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNetworkingInfoReadbackDocTest,
    "PinWright.infra.wiki_handler.Method.NetworkingInfoReadback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FNetworkingInfoReadbackDocTest::RunTest(const FString& Parameters)
{
    FString Text;
    if (!WikiDocTestHelpers::RenderOrFail(*this, TEXT("networking.get_networking_info"), Text))
    {
        return false;
    }

    // blueprintPath branch: the two per-RPC / per-property arrays the
    // F-networking-info-no-rpc-detail fix landed, documented as the read-back.
    TestTrue(TEXT("get_networking_info page documents the rpcFunctions[]/replicatedProperties[] arrays"),
        Text.Contains(TEXT("rpcFunctions")) && Text.Contains(TEXT("replicatedProperties")));

    // rpcFunctions[] per-entry fields — withValidation is the RPC flag no other
    // reader surfaces (the ticket's core "readback contract" concern).
    TestTrue(TEXT("get_networking_info page names the rpcFunctions[] per-entry withValidation field"),
        Text.Contains(TEXT("withValidation")));

    // replicatedProperties[] per-entry fields (RepNotify name + condition).
    TestTrue(TEXT("get_networking_info page names the replicatedProperties[] replicatedUsing/replicationCondition fields"),
        Text.Contains(TEXT("replicatedUsing")) && Text.Contains(TEXT("replicationCondition")));

    // actorName branch: the role/authority read-back that exists only on the
    // instance branch (remoteRole is a distinctly overlay-exclusive token).
    TestTrue(TEXT("get_networking_info page documents the actorName-branch role/remoteRole/hasAuthority fields"),
        Text.Contains(TEXT("remoteRole")) && Text.Contains(TEXT("hasAuthority")));

    // actorName branch: the owner read-back the F fix landed (owner/ownerName).
    TestTrue(TEXT("get_networking_info page documents the actorName-branch owner/ownerName read-back"),
        Text.Contains(TEXT("ownerName")));

    // The write-only setter that does NOT round-trip through this reader — an
    // agent must learn from docs (not a source dive) that its state is
    // unconfirmable here. It is a method-name token absent from the auto page.
    TestTrue(TEXT("get_networking_info page flags replicate-movement as not read-back-able"),
        Text.Contains(TEXT("configure_replicated_movement")));

    return true;
}
