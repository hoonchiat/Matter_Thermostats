#!/usr/bin/env python3
"""Reference simulator of the adapter's management console (docs/MANAGEMENT.md §2).

Serves the protocol on a pseudo-terminal so nessumctl can be tested without
hardware. It is also the executable reference for the firmware's mgmt/ module.

    python3 fake_adapter.py            # prints the pty path, serves until Ctrl-C
    nessumctl -d <pty path> mac get
"""

import hashlib
import os
import pty
import re
import sys
import threading
import tty

DEFAULT_MAC = "00:1e:c0:12:34:56"  # the 24AA02E48 EUI-48
NKEY_LEN = 16  # bytes; AES-128 assumed - confirm against the SC1320A security spec
# Nessum IC commands still allowed through NESSUM after the factory lock (read-only).
# Placeholder list until the SC1320A command set is known.
NESSUM_READONLY = {"STATUS", "STATS", "VERSION", "PEERS"}
MAC_RE = re.compile(r"^[0-9a-f]{2}([:-]?)[0-9a-f]{2}(\1[0-9a-f]{2}){4}$")


def key_fingerprint(key):
    """First 16 hex digits of SHA-256(key): identifies a key without revealing it."""
    return hashlib.sha256(key).hexdigest()[:16]


def normalise(text):
    s = text.lower()
    if not MAC_RE.match(s):
        return None
    return ":".join(re.findall(r"[0-9a-f]{2}", s))


class FakeAdapter:
    def __init__(self, default=DEFAULT_MAC, serial="SIM0001"):
        self.default = default
        self.serial = serial
        self.programmed = None       # "EEPROM"
        self.nkey = None             # Nessum network key (sealed blob on real hardware)
        self.locked = False          # factory lock flag; cleared only via SWD
        self.active = default        # what Linux sees since the last enumeration
        self.source = "default"
        self.reboots = 0
        self.fail_storage = False    # test hook: simulate EEPROM write failure

    def _enumerate(self):
        self.reboots += 1
        if self.programmed:
            self.active, self.source = self.programmed, "programmed"
        else:
            self.active, self.source = self.default, "default"

    def handle(self, line):
        """Return the response lines (without line endings) for one request line."""
        parts = line.strip().split(" ")
        two_word = parts[0].upper() in ("MAC", "NKEY")
        cmd = " ".join(parts[:2]).upper() if two_word else parts[0].upper()
        args = parts[2:] if two_word else parts[1:]

        if cmd == "VERSION":
            return [f"fw=0.0.0-sim hw=SIM serial={self.serial} nessum=sim proto=1", "OK"]
        if cmd == "STATUS":
            return ["link=up rate=240 peers=1", "OK"]
        if cmd == "MAC GET":
            return [f"active={self.active} source={self.source} default={self.default} "
                    f"programmed={self.programmed or 'none'} "
                    f"locked={'yes' if self.locked else 'no'}", "OK"]
        if cmd == "MAC SET":
            if len(args) != 1:
                return ["ERR 2 usage: MAC SET <mac>"]
            if self.locked:
                return ["ERR 6 adapter is factory-locked"]
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
            if self.locked:
                return ["ERR 6 adapter is factory-locked"]
            self.programmed = None
            return ["programmed=none apply=reboot", "OK"]
        if cmd == "NKEY SET":
            if self.locked:
                return ["ERR 6 adapter is factory-locked"]
            if len(args) != 1 or not re.fullmatch(r"[0-9a-fA-F]+", args[0]) or len(args[0]) % 2:
                return ["ERR 2 usage: NKEY SET <hex>"]
            key = bytes.fromhex(args[0])
            if len(key) != NKEY_LEN:
                return [f"ERR 8 network key must be {NKEY_LEN} bytes"]
            if key == bytes(NKEY_LEN):
                return ["ERR 8 all-zero network key rejected"]
            if self.fail_storage:
                return ["ERR 4 key store verify failed"]
            self.nkey = key
            return [f"fp={key_fingerprint(key)}", "OK"]
        if cmd == "NKEY GET":
            # Never returns the key itself.
            fp = key_fingerprint(self.nkey) if self.nkey else "none"
            return [f"set={'yes' if self.nkey else 'no'} fp={fp} len={NKEY_LEN}", "OK"]
        if cmd == "LOCK":
            if not (self.programmed and self.nkey):
                return ["ERR 7 MAC and network key must both be programmed before locking"]
            self.locked = True
            return ["locked=yes", "OK"]
        if cmd == "REBOOT":
            self._enumerate()
            return ["OK"]
        if cmd == "NESSUM":
            if self.locked and (not args or args[0].upper() not in NESSUM_READONLY):
                return ["ERR 6 only read-only Nessum commands are allowed after the factory lock"]
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
