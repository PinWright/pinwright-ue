# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Unit tests for mcp_proxy's SSE units: iter_sse_events (stream parsing) and
_wants_stream (the streaming opt-in gate).

Pure stdlib; drives the parser with canned byte streams via a readline
callable, no sockets. Run from this directory with:

    python -m unittest

or explicitly:

    python -m unittest test_mcp_proxy_sse
"""

import json
import os
import sys
import unittest

# mcp_proxy.py lives one directory up (Content/Python/).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mcp_proxy import _wants_stream, iter_sse_events


def make_readline(payload):
    """Returns a readline() callable over the canned byte payload, mimicking a
    socket file object: each call returns one line INCLUDING its terminator;
    b"" signals EOF."""
    lines = payload.splitlines(keepends=True)
    queue = list(lines)

    def readline():
        return queue.pop(0) if queue else b""

    return readline


def frame(obj, eol=b"\r\n"):
    """One server-shaped SSE frame: event: message / data: <json> / blank."""
    data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    return b"event: message" + eol + b"data: " + data + eol + eol


PROGRESS_1 = {
    "jsonrpc": "2.0",
    "method": "notifications/progress",
    "params": {"progressToken": "tok-1", "progress": 1, "ticket_id": "job_abc"},
}
PROGRESS_2 = {
    "jsonrpc": "2.0",
    "method": "notifications/progress",
    "params": {"progressToken": "tok-1", "progress": 2},
}
TERMINAL = {
    "jsonrpc": "2.0",
    "id": 1,
    "result": {"content": [{"type": "text", "text": "done"}], "isError": False},
}


class IterSseEventsTest(unittest.TestCase):
    def test_normal_sequence_yields_three_events_in_order(self):
        payload = frame(PROGRESS_1) + frame(PROGRESS_2) + frame(TERMINAL)
        events = list(iter_sse_events(make_readline(payload)))
        self.assertEqual(len(events), 3)
        self.assertEqual(events[0], PROGRESS_1)
        self.assertEqual(events[1], PROGRESS_2)
        self.assertEqual(events[2], TERMINAL)

    def test_heartbeat_comment_lines_are_skipped(self):
        payload = (
            b": ping\n\n"
            + frame(PROGRESS_1)
            + b": ping\n\n"
            + b": ping\n\n"
            + frame(TERMINAL)
        )
        events = list(iter_sse_events(make_readline(payload)))
        self.assertEqual(events, [PROGRESS_1, TERMINAL])

    def test_malformed_data_line_is_skipped_without_raising(self):
        payload = (
            frame(PROGRESS_1)
            + b"event: message\r\ndata: {not valid json\r\n\r\n"
            + frame(TERMINAL)
        )
        events = list(iter_sse_events(make_readline(payload)))
        self.assertEqual(events, [PROGRESS_1, TERMINAL])

    def test_crlf_and_lf_line_endings_both_tolerated(self):
        crlf_events = list(iter_sse_events(make_readline(
            frame(PROGRESS_1, eol=b"\r\n") + frame(TERMINAL, eol=b"\r\n"))))
        lf_events = list(iter_sse_events(make_readline(
            frame(PROGRESS_1, eol=b"\n") + frame(TERMINAL, eol=b"\n"))))
        self.assertEqual(crlf_events, [PROGRESS_1, TERMINAL])
        self.assertEqual(lf_events, [PROGRESS_1, TERMINAL])

    def test_utf8_multibyte_payload_intact(self):
        tricky = {
            "jsonrpc": "2.0",
            "method": "notifications/progress",
            "params": {"progressToken": "tok-1", "message": "Дрон летит \U0001F681"},
        }
        events = list(iter_sse_events(make_readline(frame(tricky) + frame(TERMINAL))))
        self.assertEqual(len(events), 2)
        self.assertEqual(events[0]["params"]["message"], "Дрон летит \U0001F681")

    def test_multi_data_lines_join_as_one_event(self):
        # Per the SSE spec, consecutive data: lines join with "\n" before
        # dispatch - the joined text must parse as ONE JSON payload.
        payload = b'data: {"a":\ndata: 1}\n\n'
        events = list(iter_sse_events(make_readline(payload)))
        self.assertEqual(events, [{"a": 1}])

    def test_event_field_lines_and_eof_without_trailing_blank(self):
        # `event:` lines carry no payload; EOF with no trailing blank line
        # must terminate cleanly (undispatched partial data is dropped).
        payload = frame(PROGRESS_1) + b"event: message\r\ndata: {\"half\": true"
        events = list(iter_sse_events(make_readline(payload)))
        self.assertEqual(events, [PROGRESS_1])

    def test_str_lines_accepted_as_well_as_bytes(self):
        # readline may return str (text-mode file); both must parse.
        lines = ["event: message\n", 'data: {"ok": 1}\n', "\n"]
        queue = list(lines)
        events = list(iter_sse_events(lambda: queue.pop(0) if queue else ""))
        self.assertEqual(events, [{"ok": 1}])


def call_msg(meta=None, args="OMIT"):
    """A tools/call envelope with optional _meta and args, shaped like the
    client sends it. args="OMIT" leaves the args key out entirely."""
    arguments = {"method": "asset.dump_folder"}
    if args != "OMIT":
        arguments["args"] = args
    params = {"name": "call", "arguments": arguments}
    if meta is not None:
        params["_meta"] = meta
    return {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": params}


TOKEN = {"progressToken": "tok-1"}


class WantsStreamTest(unittest.TestCase):
    """Block-by-default gate: progressToken present AND args.wait is not an
    explicit JSON false -> stream; args.wait == false -> buffered ticket."""

    def test_token_and_wait_absent_streams(self):
        self.assertTrue(_wants_stream(call_msg(meta=TOKEN)))
        self.assertTrue(_wants_stream(call_msg(meta=TOKEN, args={})))

    def test_token_and_wait_true_streams(self):
        self.assertTrue(_wants_stream(call_msg(meta=TOKEN, args={"wait": True})))

    def test_token_and_non_boolean_wait_streams(self):
        self.assertTrue(_wants_stream(call_msg(meta=TOKEN, args={"wait": "no"})))
        self.assertTrue(_wants_stream(call_msg(meta=TOKEN, args={"wait": 0})))
        self.assertTrue(_wants_stream(call_msg(meta=TOKEN, args={"wait": None})))

    def test_token_and_wait_false_does_not_stream(self):
        self.assertFalse(_wants_stream(call_msg(meta=TOKEN, args={"wait": False})))

    def test_no_token_never_streams(self):
        self.assertFalse(_wants_stream(call_msg()))
        self.assertFalse(_wants_stream(call_msg(args={"wait": True})))
        self.assertFalse(_wants_stream(call_msg(meta={}, args={"wait": True})))
        self.assertFalse(_wants_stream(
            call_msg(meta={"progressToken": None}, args={"wait": True})))

    def test_malformed_envelope_does_not_stream(self):
        self.assertFalse(_wants_stream({}))
        self.assertFalse(_wants_stream({"params": "not-a-dict"}))
        self.assertFalse(_wants_stream({"params": {"_meta": "not-a-dict"}}))


if __name__ == "__main__":
    unittest.main()
