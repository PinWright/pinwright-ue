# Copyright (c) 2026 Alexander Penkin. MIT License.

"""Unit tests for Proxy._probe_state's failure classification.

THE DEFECT THESE PIN. `_probe_state` ended in a catch-all that mapped ANY unexpected
exception to `not_running`. `not_running` is the only state that lets a caller start an
editor (`_editor_process_guard` blocks the spawn for every other state), so a live editor
whose ping merely failed to parse was reported as absent - and the second editor started
beside it cannot bind the port the first one holds. It boots into a lost bind and serves
nothing, which is the port-contention failure the C++ bind retry exists for. The catch-all
is the upstream half of that bug.

Pure stdlib; no socket is opened and no editor is started - `_post` is replaced by a stub
that raises. Run from the parent directory (Content/Python/) with:

    python -m unittest discover tests

or explicitly:

    python -m unittest tests.test_mcp_proxy_probe_state
"""

import errno
import json
import os
import sys
import unittest
from unittest import mock

# mcp_proxy.py lives one directory up (Content/Python/).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from mcp_proxy import Proxy  # noqa: E402

_URL = "http://127.0.0.1:27145/mcp"


class ProbeStateFailureClassificationTest(unittest.TestCase):
    def _proxy(self):
        return Proxy(_URL, 0.1, 0.1, 0.1, None, None)

    def _probe_raising(self, exc):
        proxy = self._proxy()
        with mock.patch.object(proxy, "_post", side_effect=exc):
            return proxy._probe_state(_URL)

    # ---- The only two things that may be reported as an absent editor.

    def test_connection_refused_is_not_running(self):
        """A refused connection is positive evidence that nothing is listening."""
        state, _detail = self._probe_raising(ConnectionRefusedError(errno.ECONNREFUSED, "refused"))
        self.assertEqual(state, "not_running")

    def test_urlerror_wrapping_connection_refused_is_not_running(self):
        """urllib wraps the OSError in URLError.reason; the unwrap must survive."""
        import urllib.error

        wrapped = urllib.error.URLError(ConnectionRefusedError(errno.ECONNREFUSED, "refused"))
        state, _detail = self._probe_raising(wrapped)
        self.assertEqual(state, "not_running")

    def test_bare_oserror_with_econnrefused_is_not_running(self):
        """Not every refusal arrives as the ConnectionRefusedError subclass."""
        state, _detail = self._probe_raising(OSError(errno.ECONNREFUSED, "refused"))
        self.assertEqual(state, "not_running")

    # ---- Everything else answered on the port, so none of it may say "not running".

    def test_unparseable_reply_is_not_reported_as_absent(self):
        """The measured shape: something IS listening, the body just is not JSON."""
        state, detail = self._probe_raising(json.JSONDecodeError("Expecting value", "", 0))
        self.assertNotEqual(state, "not_running")
        self.assertEqual(state, "unresponsive")
        self.assertIn("do not start a second one", detail)

    def test_connection_reset_is_not_reported_as_absent(self):
        state, _detail = self._probe_raising(ConnectionResetError(errno.ECONNRESET, "reset"))
        self.assertNotEqual(state, "not_running")

    def test_connection_aborted_is_not_reported_as_absent(self):
        state, _detail = self._probe_raising(ConnectionAbortedError(errno.ECONNABORTED, "aborted"))
        self.assertNotEqual(state, "not_running")

    def test_truncated_response_is_not_reported_as_absent(self):
        import http.client

        state, _detail = self._probe_raising(http.client.RemoteDisconnected("closed early"))
        self.assertNotEqual(state, "not_running")

    def test_unmapped_urlerror_is_not_reported_as_absent(self):
        import urllib.error

        state, _detail = self._probe_raising(urllib.error.URLError("some transport oddity"))
        self.assertNotEqual(state, "not_running")

    def test_unexpected_exception_type_is_not_reported_as_absent(self):
        """The catch-all's own case: an exception nobody anticipated."""
        state, _detail = self._probe_raising(ValueError("something nobody predicted"))
        self.assertNotEqual(state, "not_running")

    # ---- Unchanged behaviour that the narrowing must not disturb.

    def test_timeout_remains_unresponsive(self):
        state, detail = self._probe_raising(TimeoutError())
        self.assertEqual(state, "unresponsive")
        self.assertIn("do not start", detail)


class ProbeStateSpawnGateTest(unittest.TestCase):
    """The consequence the classification exists for: only `not_running` lets a spawn through."""

    def _proxy(self):
        return Proxy(_URL, 0.1, 0.1, 0.1, None, None)

    def test_unparseable_reply_blocks_the_editor_spawn(self):
        proxy = self._proxy()
        with mock.patch.object(proxy, "_post", side_effect=json.JSONDecodeError("x", "", 0)):
            guard = proxy._editor_process_guard()
        self.assertIsNotNone(
            guard,
            "an editor that answered on the port must not be spawned over: "
            "the second editor cannot bind the port the first one holds",
        )

    def test_connection_refused_allows_the_editor_spawn(self):
        proxy = self._proxy()
        with mock.patch.object(
                proxy, "_post", side_effect=ConnectionRefusedError(errno.ECONNREFUSED, "refused")):
            guard = proxy._editor_process_guard()
        self.assertIsNone(guard, "a genuinely absent editor must still be startable")


if __name__ == "__main__":
    unittest.main()
