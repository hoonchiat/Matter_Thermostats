#!/usr/bin/env python3
"""nessumctl - manage the USB <-> Nessum adapter from Linux.

Talks the console protocol in docs/MANAGEMENT.md over the adapter's CDC-ACM
port. Standard library only, so it runs on a minimal embedded rootfs.

Examples:
    nessumctl mac get
    nessumctl mac set 00:50:c2:aa:bb:cc --apply
    nessumctl mac clear --apply
    nessumctl status
"""

import argparse
import os
import re
import select
import sys
import termios
import time
import tty

DEFAULT_DEVICE = "/dev/nessum-mgmt"
TIMEOUT_S = 3.0
EXIT_ERR_DEVICE = 2
EXIT_ERR_PROTOCOL = 3


class MacError(ValueError):
    pass


def parse_mac(text):
    """Normalise a MAC to 'xx:xx:xx:xx:xx:xx' and validate it (MANAGEMENT.md §1.1).

    Accepts ':' or '-' separators or 12 bare hex digits.
    """
    s = text.strip().lower()
    if re.fullmatch(r"[0-9a-f]{2}([:-])[0-9a-f]{2}(\1[0-9a-f]{2}){4}", s):
        octets = re.split(r"[:-]", s)
    elif re.fullmatch(r"[0-9a-f]{12}", s):
        octets = [s[i:i + 2] for i in range(0, 12, 2)]
    else:
        raise MacError(f"not a MAC address: {text!r}")
    first = int(octets[0], 16)
    mac = ":".join(octets)
    if mac == "00:00:00:00:00:00":
        raise MacError("all-zero address is not allowed")
    if mac == "ff:ff:ff:ff:ff:ff":
        raise MacError("broadcast address is not allowed")
    if first & 0x01:
        raise MacError(f"{mac} is a multicast (group) address")
    return mac


def is_locally_administered(mac):
    return bool(int(mac[:2], 16) & 0x02)


class ProtocolError(RuntimeError):
    def __init__(self, code, message):
        super().__init__(f"adapter error {code}: {message}")
        self.code = code


class Console:
    """One request / response exchange at a time over a raw tty."""

    def __init__(self, path, timeout=TIMEOUT_S):
        self.timeout = timeout
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        try:
            tty.setraw(self.fd)
            attrs = termios.tcgetattr(self.fd)
            attrs[3] &= ~termios.ECHO
            termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
            termios.tcflush(self.fd, termios.TCIOFLUSH)
        except termios.error:
            pass  # not a tty (e.g. a pipe in tests)
        self._buf = b""

    def close(self):
        os.close(self.fd)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _readline(self, deadline):
        while b"\n" not in self._buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("no response from adapter")
            ready, _, _ = select.select([self.fd], [], [], remaining)
            if not ready:
                continue
            chunk = os.read(self.fd, 256)
            if not chunk:
                raise ConnectionError("adapter disconnected")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.rstrip(b"\r").decode("ascii", "replace")

    def command(self, cmd):
        """Send one command and return its data lines as a list of dicts."""
        os.write(self.fd, (cmd + "\n").encode("ascii"))
        deadline = time.monotonic() + self.timeout
        data = []
        while True:
            line = self._readline(deadline)
            if line == "OK":
                return data
            if line.startswith("ERR"):
                parts = line.split(" ", 2)
                code = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else -1
                raise ProtocolError(code, parts[2] if len(parts) > 2 else "")
            if line:
                data.append(parse_kv(line))


def parse_kv(line):
    out = {}
    for token in line.split(" "):
        key, sep, value = token.partition("=")
        if sep:
            out[key] = value
    return out


def merged(data):
    out = {}
    for d in data:
        out.update(d)
    return out


def cmd_mac(con, args):
    if args.action == "get":
        info = merged(con.command("MAC GET"))
        if args.quiet:
            print(info.get("active", ""))
        else:
            for key in ("active", "source", "factory", "programmed"):
                print(f"{key:<11}{info.get(key, '?')}")
        return 0

    if args.action == "set":
        mac = parse_mac(args.address)
        if is_locally_administered(mac):
            print(f"warning: {mac} is a locally administered address", file=sys.stderr)
        con.command(f"MAC SET {mac}")
        print(f"programmed {mac}")
    else:  # clear
        con.command("MAC CLEAR")
        print("programmed address cleared; factory address will be used")

    if args.apply:
        con.command("REBOOT")
        print("adapter re-enumerating ...")
        return 0
    print("run 'nessumctl reboot' (or replug) for Linux to pick up the new address")
    return 0


def cmd_simple(con, command):
    for d in con.command(command):
        print(" ".join(f"{k}={v}" for k, v in d.items()))
    return 0


def build_parser():
    p = argparse.ArgumentParser(prog="nessumctl", description=__doc__.split("\n")[0])
    p.add_argument("-d", "--device", default=os.environ.get("NESSUM_DEVICE", DEFAULT_DEVICE),
                   help=f"management tty (default {DEFAULT_DEVICE}, or $NESSUM_DEVICE)")
    p.add_argument("-t", "--timeout", type=float, default=TIMEOUT_S)
    sub = p.add_subparsers(dest="cmd", required=True)

    mac = sub.add_parser("mac", help="show or program the Ethernet MAC address")
    mac_sub = mac.add_subparsers(dest="action", required=True)
    g = mac_sub.add_parser("get", help="show active / factory / programmed addresses")
    g.add_argument("-q", "--quiet", action="store_true", help="print only the active address")
    s = mac_sub.add_parser("set", help="program a persistent MAC address")
    s.add_argument("address")
    s.add_argument("--apply", action="store_true", help="re-enumerate so Linux uses it now")
    c = mac_sub.add_parser("clear", help="revert to the factory MAC address")
    c.add_argument("--apply", action="store_true", help="re-enumerate so Linux uses it now")

    sub.add_parser("version", help="firmware / hardware / Nessum IC versions")
    sub.add_parser("status", help="Nessum link state and rate")
    sub.add_parser("reboot", help="re-enumerate the adapter")
    raw = sub.add_parser("nessum", help="pass a command through to the Nessum IC")
    raw.add_argument("text", nargs=argparse.REMAINDER)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)

    # Validate before touching the device so typos fail fast.
    if args.cmd == "mac" and args.action == "set":
        try:
            parse_mac(args.address)
        except MacError as e:
            print(f"nessumctl: {e}", file=sys.stderr)
            return 1

    try:
        con = Console(args.device, args.timeout)
    except OSError as e:
        print(f"nessumctl: cannot open {args.device}: {e.strerror}", file=sys.stderr)
        return EXIT_ERR_DEVICE

    try:
        with con:
            if args.cmd == "mac":
                return cmd_mac(con, args)
            if args.cmd == "nessum":
                return cmd_simple(con, "NESSUM " + " ".join(args.text))
            return cmd_simple(con, args.cmd.upper())
    except ProtocolError as e:
        print(f"nessumctl: {e}", file=sys.stderr)
        return EXIT_ERR_PROTOCOL
    except (TimeoutError, ConnectionError) as e:
        print(f"nessumctl: {e}", file=sys.stderr)
        return EXIT_ERR_DEVICE


if __name__ == "__main__":
    sys.exit(main())
