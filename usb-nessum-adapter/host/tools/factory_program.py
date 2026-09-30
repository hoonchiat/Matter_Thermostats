#!/usr/bin/env python3
"""Factory programming: MAC from your block + Nessum network key, verify, lock.

Each station keeps an append-only CSV log. An address is recorded as 'reserved'
*before* it is written to the adapter, so a crash or unplug mid-way can never lead
to the same address being handed out twice. Reserved-but-unfinished addresses
are skipped, never reused.

    factory_program.py --block 00:50:c2:aa:00:00/36 --key-file kit42.key --log macs.csv
    factory_program.py --block 00:50:c2:aa:00:00-00:50:c2:aa:0f:ff --key-file eng.key \
        --log eng.csv --no-lock

Use one log file (or disjoint sub-blocks) per programming station. The key file
holds the Nessum network key as hex and must be mode 600. The log records only the
key's fingerprint, never the key.
"""

import argparse
import csv
import datetime
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nessumctl  # noqa: E402

LOG_FIELDS = ["timestamp", "status", "mac", "serial", "key_fp", "detail"]


def mac_to_int(mac):
    return int(nessumctl.parse_mac(mac).replace(":", ""), 16)


def int_to_mac(value):
    h = f"{value:012x}"
    return ":".join(h[i:i + 2] for i in range(0, 12, 2))


def parse_block(text):
    """'first-last' or 'base/prefixlen' -> (first_int, last_int)."""
    if "/" in text:
        base, plen = text.split("/", 1)
        plen = int(plen)
        if not 24 <= plen <= 47:
            raise ValueError("prefix length must be 24..47")
        size = 1 << (48 - plen)
        first = mac_to_int(base) & ~(size - 1)
        last = first + size - 1
    elif len(text) == 35 and text[17] == "-":  # xx:xx:xx:xx:xx:xx-xx:xx:xx:xx:xx:xx
        first, last = mac_to_int(text[:17]), mac_to_int(text[18:])
    else:
        raise ValueError("block must be 'first-last' (':' separators) or 'base/prefixlen'")
    if last < first:
        raise ValueError("block end is before block start")
    if (first >> 40) & 0x01:
        raise ValueError("block is in multicast address space")
    return first, last


def read_log(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def append_log(path, **row):
    new = not os.path.exists(path) or os.path.getsize(path) == 0
    row.setdefault("timestamp", datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"))
    with open(path, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=LOG_FIELDS)
        if new:
            w.writeheader()
        w.writerow({k: row.get(k, "") for k in LOG_FIELDS})
        f.flush()
        os.fsync(f.fileno())


def next_address(rows, first, last):
    """Lowest address in the block above every address ever reserved in the log."""
    used = [mac_to_int(r["mac"]) for r in rows if r.get("mac")]
    in_block = [u for u in used if first <= u <= last]
    candidate = max(in_block) + 1 if in_block else first
    # Never hand out the all-ones host part of a block edge case like ff:ff:ff:ff:ff:ff.
    while candidate <= last:
        mac = int_to_mac(candidate)
        try:
            nessumctl.parse_mac(mac)
            return mac
        except nessumctl.MacError:
            candidate += 1
    return None


def program_one(con, log, first, last, key, lock=True, force=False):
    """Program one adapter's MAC and network key. Returns (exit_code, message)."""
    ver = nessumctl.merged(con.command("VERSION"))
    serial = ver.get("serial", "?")
    info = nessumctl.merged(con.command("MAC GET"))

    if info.get("locked") == "yes":
        return 1, f"{serial}: already locked with {info.get('programmed')} - nothing done"
    if info.get("programmed", "none") != "none" and not force:
        return 1, f"{serial}: already programmed with {info['programmed']} (use --force to replace)"

    rows = read_log(log)
    mac = next_address(rows, first, last)
    if mac is None:
        return 1, "address block exhausted"
    remaining = last - mac_to_int(mac)
    key_fp = nessumctl.key_fingerprint(key)

    append_log(log, status="reserved", mac=mac, serial=serial, key_fp=key_fp)
    try:
        con.command(f"MAC SET {mac}")
        check = nessumctl.merged(con.command("MAC GET"))
        if check.get("programmed") != mac:
            raise RuntimeError(f"read-back mismatch: {check.get('programmed')}")
        con.command(f"NKEY SET {key.hex()}")
        if nessumctl.merged(con.command("NKEY GET")).get("fp") != key_fp:
            raise RuntimeError("network key fingerprint mismatch after write")
        if lock:
            con.command("LOCK")
            if nessumctl.merged(con.command("MAC GET")).get("locked") != "yes":
                raise RuntimeError("lock did not take effect")
        con.command("REBOOT")
    except (nessumctl.ProtocolError, RuntimeError, TimeoutError, ConnectionError) as e:
        append_log(log, status="failed", mac=mac, serial=serial, key_fp=key_fp, detail=str(e))
        return 1, f"{serial}: FAILED programming {mac}: {e} (address retired, not reused)"

    append_log(log, status="locked" if lock else "programmed", mac=mac, serial=serial, key_fp=key_fp)
    done = "programmed + locked" if lock else "programmed"
    return 0, f"{serial}: {mac}, key {key_fp}, {done} ({remaining} left in block)"


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("-d", "--device", default=os.environ.get("NESSUM_DEVICE", nessumctl.DEFAULT_DEVICE))
    p.add_argument("--block", required=True,
                   help="your address block: 'first-last' or 'base/prefixlen' (e.g. 00:50:c2:aa:00:00/36)")
    p.add_argument("--key-file", required=True,
                   help="Nessum network key as hex (mode 600). Create one per kit/installation: "
                        "(umask 077; python3 -c 'import secrets;print(secrets.token_hex(16))' > kit42.key)")
    p.add_argument("--log", required=True, help="append-only CSV log for this station")
    p.add_argument("--no-lock", action="store_true", help="program without locking (e.g. engineering units)")
    p.add_argument("--force", action="store_true", help="replace an existing unlocked programmed address")
    args = p.parse_args(argv)

    try:
        first, last = parse_block(args.block)
    except (ValueError, nessumctl.MacError) as e:
        print(f"factory_program: bad --block: {e}", file=sys.stderr)
        return 2

    try:
        key = nessumctl.read_key_file(args.key_file)
    except (nessumctl.KeyError_, OSError) as e:
        print(f"factory_program: bad --key-file: {e}", file=sys.stderr)
        return 2

    try:
        con = nessumctl.Console(args.device)
    except OSError as e:
        print(f"factory_program: cannot open {args.device}: {e.strerror}", file=sys.stderr)
        return 2

    with con:
        try:
            rc, msg = program_one(con, args.log, first, last, key,
                                  lock=not args.no_lock, force=args.force)
        except (nessumctl.ProtocolError, TimeoutError, ConnectionError) as e:
            rc, msg = 2, f"adapter not responding correctly: {e}"
    print(msg, file=sys.stdout if rc == 0 else sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
