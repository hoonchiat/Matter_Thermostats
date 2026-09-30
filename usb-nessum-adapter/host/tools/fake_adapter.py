#!/usr/bin/env python3
"""Reference simulator of the adapter's management console (docs/MANAGEMENT.md §2).

Serves the protocol on a pseudo-terminal so nessumctl can be tested without
hardware. It is also the executable reference for the firmware's mgmt/ module.

    python3 fake_adapter.py            # prints the pty path, serves until Ctrl-C
    nessumctl -d <pty path> mac get
"""

import os
import pty
import re
import sys
import threading
import tty

FACTORY_MAC = "00:1e:c0:12:34:56"
MAC_RE = re.compile(r"^[0-9a-f]{2}([:-]?)[0-9a-f]{2}(\1[0-9a-f]{2}){4}$")


def normalise(text):
    s = text.lower()
    if not MAC_RE.match(s):
        return None
    return ":".join(re.findall(r"[0-9a-f]{2}", s))


class FakeAdapter:
    def __init__(self, factory=FACTORY_MAC):
        self.factory = factory
        self.programmed = None       # "EEPROM"
        self.active = factory        # what Linux sees since the last enumeration
        self.source = "factory"
        self.reboots = 0
        self.fail_storage = False    # test hook: simulate EEPROM write failure

    def _enumerate(self):
        self.reboots += 1
        if self.programmed:
            self.active, self.source = self.programmed, "programmed"
        else:
            self.active, self.source = self.factory, "factory"

    def handle(self, line):
        """Return the response lines (without line endings) for one request line."""
        parts = line.strip().split(" ")
        cmd = " ".join(parts[:2]).upper() if parts[0].upper() == "MAC" else parts[0].upper()
        args = parts[2:] if parts[0].upper() == "MAC" else parts[1:]

        if cmd == "VERSION":
            return ["fw=0.0.0-sim hw=SIM nessum=sim proto=1", "OK"]
        if cmd == "STATUS":
            return ["link=up rate=240 peers=1", "OK"]
        if cmd == "MAC GET":
            return [f"active={self.active} source={self.source} factory={self.factory} "
                    f"programmed={self.programmed or 'none'}", "OK"]
        if cmd == "MAC SET":
            if len(args) != 1:
                return ["ERR 2 usage: MAC SET <mac>"]
            mac = normalise(args[0])
            if mac is None:
                return ["ERR 2 bad MAC syntax"]
            first = int(mac[:2], 16)
            if mac in ("00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff") or first & 1:
                return ["ERR 3 invalid MAC (multicast, zero or broadcast)"]
            if self.fail_storage:
                return ["ERR 4 EEPROM verify failed"]
            self.programmed = mac
            return [f"programmed={mac} apply=reboot", "OK"]
        if cmd == "MAC CLEAR":
            self.programmed = None
            return ["programmed=none apply=reboot", "OK"]
        if cmd == "REBOOT":
            self._enumerate()
            return ["OK"]
        if cmd == "NESSUM":
            return [f"reply={'_'.join(args) or 'empty'}", "OK"]
        return [f"ERR 1 unknown command {parts[0]}"]


def serve(adapter, fd, stop):
    buf = b""
    while not stop.is_set():
        try:
            chunk = os.read(fd, 256)
        except OSError:
            return
        if not chunk:
            return
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.rstrip(b"\r").decode("ascii", "replace")
            if not line:
                continue
            out = "".join(r + "\r\n" for r in adapter.handle(line))
            os.write(fd, out.encode("ascii"))


def start(adapter=None):
    """Start a simulator thread. Returns (adapter, pty_path, stop_fn)."""
    adapter = adapter or FakeAdapter()
    master, slave = pty.openpty()
    tty.setraw(slave)
    path = os.ttyname(slave)
    stop = threading.Event()
    t = threading.Thread(target=serve, args=(adapter, master, stop), daemon=True)
    t.start()

    def stop_fn():
        stop.set()
        os.close(slave)
        os.close(master)

    return adapter, path, stop_fn


if __name__ == "__main__":
    _, path, stop_fn = start()
    print(path, flush=True)
    try:
        threading.Event().wait()
    except KeyboardInterrupt:
        stop_fn()
        sys.exit(0)
