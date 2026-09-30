#!/usr/bin/env python3
"""Tests for nessumctl against the fake adapter. Run: python3 -m unittest -v"""

import contextlib
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fake_adapter  # noqa: E402
import nessumctl  # noqa: E402


class ParseMacTest(unittest.TestCase):
    def test_accepts_common_forms(self):
        for text in ("00:50:C2:AA:BB:CC", "00-50-c2-aa-bb-cc", "0050c2aabbcc", " 00:50:c2:aa:bb:cc\n"):
            self.assertEqual(nessumctl.parse_mac(text), "00:50:c2:aa:bb:cc")

    def test_rejects_invalid(self):
        for text in ("", "00:50:c2:aa:bb", "00:50:c2:aa:bb:cc:dd", "00:50-c2:aa:bb:cc",
                     "zz:50:c2:aa:bb:cc", "00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff",
                     "01:00:5e:00:00:01", "33:33:00:00:00:01"):
            with self.subTest(text=text), self.assertRaises(nessumctl.MacError):
                nessumctl.parse_mac(text)

    def test_locally_administered(self):
        self.assertTrue(nessumctl.is_locally_administered("02:00:00:00:00:01"))
        self.assertFalse(nessumctl.is_locally_administered("00:50:c2:aa:bb:cc"))


class CliTest(unittest.TestCase):
    def setUp(self):
        self.adapter, self.path, stop = fake_adapter.start()
        self.addCleanup(stop)

    def run_cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = nessumctl.main(["-d", self.path, "-t", "2", *argv])
        return rc, out.getvalue(), err.getvalue()

    def test_get_factory(self):
        rc, out, _ = self.run_cli("mac", "get")
        self.assertEqual(rc, 0)
        self.assertIn("active     00:1e:c0:12:34:56", out)
        self.assertIn("source     factory", out)
        self.assertIn("programmed none", out)

    def test_set_then_apply_changes_active(self):
        rc, out, _ = self.run_cli("mac", "set", "00-50-C2-AA-BB-CC")
        self.assertEqual(rc, 0)
        self.assertIn("programmed 00:50:c2:aa:bb:cc", out)
        # Not active until the adapter re-enumerates.
        self.assertEqual(self.run_cli("mac", "get", "-q")[1].strip(), "00:1e:c0:12:34:56")

        self.assertEqual(self.run_cli("reboot")[0], 0)
        rc, out, _ = self.run_cli("mac", "get")
        self.assertIn("active     00:50:c2:aa:bb:cc", out)
        self.assertIn("source     programmed", out)

    def test_set_with_apply_reboots(self):
        rc, _, _ = self.run_cli("mac", "set", "00:50:c2:aa:bb:cc", "--apply")
        self.assertEqual(rc, 0)
        self.assertEqual(self.adapter.reboots, 1)
        self.assertEqual(self.adapter.active, "00:50:c2:aa:bb:cc")

    def test_clear_reverts_to_factory(self):
        self.run_cli("mac", "set", "00:50:c2:aa:bb:cc", "--apply")
        rc, _, _ = self.run_cli("mac", "clear", "--apply")
        self.assertEqual(rc, 0)
        self.assertEqual(self.adapter.active, fake_adapter.FACTORY_MAC)
        self.assertIsNone(self.adapter.programmed)

    def test_invalid_mac_rejected_before_device_io(self):
        rc, _, err = self.run_cli("mac", "set", "01:00:5e:00:00:01")
        self.assertEqual(rc, 1)
        self.assertIn("multicast", err)
        self.assertIsNone(self.adapter.programmed)

    def test_locally_administered_warns(self):
        rc, _, err = self.run_cli("mac", "set", "02:00:00:00:00:01")
        self.assertEqual(rc, 0)
        self.assertIn("locally administered", err)

    def test_storage_failure_reported(self):
        self.adapter.fail_storage = True
        rc, _, err = self.run_cli("mac", "set", "00:50:c2:aa:bb:cc")
        self.assertEqual(rc, nessumctl.EXIT_ERR_PROTOCOL)
        self.assertIn("adapter error 4", err)

    def test_status_and_version(self):
        rc, out, _ = self.run_cli("status")
        self.assertEqual(rc, 0)
        self.assertIn("link=up", out)
        rc, out, _ = self.run_cli("version")
        self.assertIn("proto=1", out)

    def test_missing_device(self):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = nessumctl.main(["-d", "/nonexistent/tty", "status"])
        self.assertEqual(rc, nessumctl.EXIT_ERR_DEVICE)
        self.assertIn("cannot open", err.getvalue())


class ProtocolTest(unittest.TestCase):
    """Firmware-facing rules, checked against the reference simulator."""

    def setUp(self):
        self.a = fake_adapter.FakeAdapter()

    def test_every_response_ends_with_one_terminal_line(self):
        for req in ("VERSION", "STATUS", "MAC GET", "MAC SET 00:50:c2:aa:bb:cc",
                    "MAC SET bogus", "MAC SET", "MAC CLEAR", "REBOOT", "FOO", "mac get"):
            resp = self.a.handle(req)
            with self.subTest(req=req):
                terminal = [r for r in resp if r == "OK" or r.startswith("ERR")]
                self.assertEqual(len(terminal), 1)
                self.assertIs(resp[-1], terminal[0])

    def test_error_codes(self):
        self.assertTrue(self.a.handle("FOO")[-1].startswith("ERR 1 "))
        self.assertTrue(self.a.handle("MAC SET bogus")[-1].startswith("ERR 2 "))
        self.assertTrue(self.a.handle("MAC SET 01:00:5e:00:00:01")[-1].startswith("ERR 3 "))


if __name__ == "__main__":
    unittest.main()
