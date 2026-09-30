#!/usr/bin/env python3
"""Tests for factory_program.py against the fake adapter. Run: python3 -m unittest -v"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import factory_program as fp  # noqa: E402
import fake_adapter  # noqa: E402
import nessumctl  # noqa: E402

BLOCK = "00:50:c2:aa:00:00-00:50:c2:aa:00:02"  # 3 addresses
KEY = bytes.fromhex("0f1e2d3c4b5a69788796a5b4c3d2e1f0")


class BlockTest(unittest.TestCase):
    def test_range(self):
        first, last = fp.parse_block(BLOCK)
        self.assertEqual(fp.int_to_mac(first), "00:50:c2:aa:00:00")
        self.assertEqual(last - first, 2)

    def test_prefix(self):
        first, last = fp.parse_block("00:50:c2:aa:b0:00/36")
        self.assertEqual(fp.int_to_mac(first), "00:50:c2:aa:b0:00")
        self.assertEqual(fp.int_to_mac(last), "00:50:c2:aa:bf:ff")

    def test_rejects(self):
        for bad in ("00:50:c2:aa:00:02-00:50:c2:aa:00:00", "01:00:5e:00:00:00/40",
                    "00:50:c2:aa:00:00/8", "00:50:c2:aa:00:00", "junk"):
            with self.subTest(bad=bad), self.assertRaises((ValueError, nessumctl.MacError)):
                fp.parse_block(bad)


class ProgramTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.log = os.path.join(self.tmp.name, "macs.csv")
        self.first, self.last = fp.parse_block(BLOCK)

    def program(self, adapter=None, **kw):
        adapter, path, stop = fake_adapter.start(adapter)
        self.addCleanup(stop)
        with nessumctl.Console(path, 2) as con:
            rc, msg = fp.program_one(con, self.log, self.first, self.last, KEY, **kw)
        return adapter, rc, msg

    def test_programs_locks_and_logs(self):
        a, rc, msg = self.program()
        self.assertEqual(rc, 0, msg)
        self.assertEqual(a.programmed, "00:50:c2:aa:00:00")
        self.assertEqual(a.nkey, KEY)
        self.assertTrue(a.locked)
        self.assertEqual(a.active, "00:50:c2:aa:00:00")  # rebooted
        rows = fp.read_log(self.log)
        statuses = [(r["status"], r["mac"], r["serial"]) for r in rows]
        self.assertEqual(statuses, [("reserved", "00:50:c2:aa:00:00", "SIM0001"),
                                    ("locked", "00:50:c2:aa:00:00", "SIM0001")])
        self.assertTrue(all(r["key_fp"] == nessumctl.key_fingerprint(KEY) for r in rows))
        with open(self.log) as f:
            self.assertNotIn(KEY.hex(), f.read())  # the key itself is never logged

    def test_sequential_units_get_unique_addresses(self):
        macs = []
        for n in range(3):
            a, rc, msg = self.program(fake_adapter.FakeAdapter(serial=f"U{n}"))
            self.assertEqual(rc, 0, msg)
            macs.append(a.programmed)
        self.assertEqual(len(set(macs)), 3)
        _, rc, msg = self.program(fake_adapter.FakeAdapter(serial="U3"))
        self.assertEqual(rc, 1)
        self.assertIn("exhausted", msg)

    def test_failed_write_retires_address(self):
        bad = fake_adapter.FakeAdapter(serial="BAD")
        bad.fail_storage = True
        _, rc, msg = self.program(bad)
        self.assertEqual(rc, 1)
        self.assertIn("retired", msg)
        a, rc, _ = self.program(fake_adapter.FakeAdapter(serial="GOOD"))
        self.assertEqual(rc, 0)
        self.assertEqual(a.programmed, "00:50:c2:aa:00:01")  # :00 never reused

    def test_refuses_locked_or_programmed_units(self):
        locked = fake_adapter.FakeAdapter()
        locked.programmed, locked.locked = "00:50:c2:aa:99:99", True
        _, rc, msg = self.program(locked, force=True)
        self.assertEqual(rc, 1)
        self.assertIn("already locked", msg)

        prog = fake_adapter.FakeAdapter()
        prog.programmed = "00:50:c2:aa:99:99"
        _, rc, msg = self.program(prog)
        self.assertEqual(rc, 1)
        self.assertIn("--force", msg)
        self.assertEqual(fp.read_log(self.log), [])  # nothing reserved

    def test_no_lock(self):
        a, rc, _ = self.program(lock=False)
        self.assertEqual(rc, 0)
        self.assertFalse(a.locked)
        self.assertEqual(fp.read_log(self.log)[-1]["status"], "programmed")


if __name__ == "__main__":
    unittest.main()
