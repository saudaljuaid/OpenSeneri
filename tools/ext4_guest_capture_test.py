#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Exercise guest output capture, terminal failures and bounded timeout cleanup."""
from pathlib import Path
import sys
import tempfile
import threading
import unittest

from ext4_powercut_test import PowerCutError, _capture_guest


class GuestCaptureTests(unittest.TestCase):
    def test_success_preserves_exit_and_complete_serial(self):
        with tempfile.TemporaryDirectory() as raw:
            log = Path(raw) / "serial.log"
            status, trace = _capture_guest([sys.executable, "-c",
                "import sys; print('ST BEGIN ext4-recovery'); print('ST PASS ext4-recovery'); sys.exit(13)"], log, 5)
            self.assertEqual(status, 13)
            self.assertEqual(trace, "ST BEGIN ext4-recovery\nST PASS ext4-recovery\n")
            self.assertEqual(log.read_text(), trace)
            self.assertFalse(any(thread.name == "ext4-guest-serial" for thread in threading.enumerate()))

    def test_terminal_guest_failure_does_not_wait_for_process_exit(self):
        for marker in ("Phipia PANIC: Rust panicked", "ST FAIL ext4 test failed"):
            with self.subTest(marker=marker), tempfile.TemporaryDirectory() as raw:
                log = Path(raw) / "serial.log"
                command = [sys.executable, "-c",
                    f"import threading; print({marker!r}, flush=True); threading.Event().wait()"]
                with self.assertRaisesRegex(PowerCutError, "guest reported a terminal failure") as caught:
                    _capture_guest(command, log, 5)
                self.assertIn(marker, str(caught.exception))
                self.assertEqual(log.read_text(), marker + "\n")
                self.assertFalse(any(thread.name == "ext4-guest-serial" for thread in threading.enumerate()))

    def test_timeout_preserves_partial_output_and_releases_reader(self):
        with tempfile.TemporaryDirectory() as raw:
            log = Path(raw) / "serial.log"
            command = [sys.executable, "-c", "import threading; print('guest still running', flush=True); threading.Event().wait()"]
            with self.assertRaisesRegex(PowerCutError, "QEMU timed out"):
                _capture_guest(command, log, 2)
            self.assertEqual(log.read_text(), "guest still running\n")
            self.assertFalse(any(thread.name == "ext4-guest-serial" for thread in threading.enumerate()))


if __name__ == "__main__":
    unittest.main()
