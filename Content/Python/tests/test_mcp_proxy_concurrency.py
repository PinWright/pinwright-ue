# Copyright (c) 2026 Alexander Penkin. MIT License.

"""A streamed call whose job never finishes must not wedge the stdio proxy.

Drives serve_stdio against a real loopback HTTP server that streams progress
forever for a streamed call and answers any other call at once. Pure stdlib.
Run from this directory with:

    python -m unittest test_mcp_proxy_concurrency
"""

import http.server
import json
import os
import sys
import threading
import time
import unittest
from unittest import mock

# mcp_proxy.py lives one directory up (Content/Python/).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mcp_proxy import Proxy, serve_stdio  # noqa: E402
from test_mcp_proxy_editor_start import _CapturedOutput, _ControlledInput  # noqa: E402

TICKET = "j_endless_ticket"


class _EndlessStreamHandler(http.server.BaseHTTPRequestHandler):
    """SSE request -> progress frames every 50 ms until the client hangs up.
    Anything else -> an immediate plain JSON-RPC result."""

    def log_message(self, *_args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if "text/event-stream" not in (self.headers.get("Accept") or ""):
            body = json.dumps({"jsonrpc": "2.0", "id": request["id"],
                               "result": {"content": [], "isError": False}}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        progress = 0
        try:
            while True:  # never terminal, like a job whose queue drained without completing
                progress += 1
                frame = {"jsonrpc": "2.0", "method": "notifications/progress",
                         "params": {"progressToken": "t", "progress": progress,
                                    "ticket_id": TICKET}}
                self.wfile.write(b"event: message\r\ndata: " + json.dumps(frame).encode()
                                 + b"\r\n\r\n")
                self.wfile.flush()
                time.sleep(0.05)
        except OSError:
            pass  # proxy closed the stream


def _streamed_call(msg_id):
    return {"jsonrpc": "2.0", "id": msg_id, "method": "tools/call",
            "params": {"name": "call", "_meta": {"progressToken": "t"},
                       "arguments": {"method": "system.run_tests", "args": {}}}}


def _plain_call(msg_id):
    return {"jsonrpc": "2.0", "id": msg_id, "method": "tools/call",
            "params": {"name": "call",
                       "arguments": {"method": "editor.pie_status", "args": {}}}}


class EndlessStreamTest(unittest.TestCase):
    def setUp(self):
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _EndlessStreamHandler)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)
        self.proxy = Proxy("http://127.0.0.1:%d/mcp" % self.server.server_port,
                           list_timeout=1.0, call_timeout=5.0, probe_timeout=1.0,
                           token_file=None, port_file=None)
        probe = mock.patch.object(self.proxy, "_probe_state", return_value=("alive", None))
        probe.start()
        self.addCleanup(probe.stop)
        self.input = _ControlledInput()
        self.output = _CapturedOutput()
        # Progress frames are relayed to sys.stdout; capture them with the responses.
        stdout = mock.patch("sys.stdout", self.output)
        stdout.start()
        self.addCleanup(stdout.stop)
        threading.Thread(target=serve_stdio, args=(self.proxy, self.input, self.output),
                         daemon=True).start()
        self.addCleanup(time.sleep, 0.2)  # let the relay see EOF before stdout is restored
        self.addCleanup(self.input.close)

    def _wait_for_response(self, msg_id, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in self.output.getvalue().splitlines():
                frame = json.loads(line)
                if frame.get("id") == msg_id:
                    return frame
            self.output.wait_for_lines(self.output.getvalue().count("\n") + 1, 0.1)
        return None

    def test_unrelated_call_is_answered_while_a_stream_never_ends(self):
        self.input.send(_streamed_call(1))
        time.sleep(0.3)  # the stream is open and relaying progress
        self.input.send(_plain_call(2))
        response = self._wait_for_response(2, 3.0)
        self.assertIsNotNone(response, "a later call queued behind the endless stream")
        self.assertFalse(response["result"]["isError"])
        self.assertIn('"progress"', self.output.getvalue())

    def test_endless_stream_returns_its_ticket_at_the_ceiling(self):
        self.proxy.stream_max_seconds = 0.5
        self.input.send(_streamed_call(1))
        response = self._wait_for_response(1, 5.0)
        self.assertIsNotNone(response, "the endless stream never returned")
        self.assertTrue(response["result"]["isError"])
        text = response["result"]["content"][0]["text"]
        self.assertIn(TICKET, text)
        self.assertIn("system.job_status", text)


if __name__ == "__main__":
    unittest.main()
