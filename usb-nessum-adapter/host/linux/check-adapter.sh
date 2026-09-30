#!/bin/sh
# Bring-up sanity check for the USB <-> Nessum adapter on the Linux host.
# Usage: check-adapter.sh [iface]   (default iface: nessum0)
set -u
IFACE="${1:-nessum0}"
rc=0

say()  { printf '%s\n' "$*"; }
fail() { say "FAIL: $*"; rc=1; }
ok()   { say "ok:   $*"; }

say "== Host"
say "$(uname -srm)"
grep -m1 -E 'CPU part|model name' /proc/cpuinfo 2>/dev/null || true

say "== Kernel driver"
if [ -d /sys/module/cdc_ncm ] || modprobe -n cdc_ncm 2>/dev/null; then
  ok "cdc_ncm available"
else
  fail "cdc_ncm missing - enable CONFIG_USB_NET_CDC_NCM (see kernel.config)"
fi

say "== USB enumeration"
found=0
for d in /sys/bus/usb/devices/*; do
  [ -f "$d/speed" ] || continue
  for i in "$d":*; do
    [ -e "$i/driver" ] || continue
    if [ "$(basename "$(readlink "$i/driver")")" = "cdc_ncm" ]; then
      found=1
      spd=$(cat "$d/speed")
      say "device $(cat "$d/idVendor"):$(cat "$d/idProduct") '$(cat "$d/product" 2>/dev/null)' speed=${spd}Mb/s"
      [ "$spd" = "480" ] && ok "High-Speed link" || fail "not High-Speed (got ${spd}Mb/s) - check cable/hub"
    fi
  done
done
[ "$found" = 1 ] || fail "no cdc_ncm device bound"

say "== Management console"
if [ -e /dev/nessum-mgmt ]; then
  ok "/dev/nessum-mgmt present"
  if command -v nessumctl >/dev/null 2>&1; then
    say "adapter MAC (active): $(nessumctl mac get -q 2>/dev/null || echo '?')"
  fi
else
  fail "/dev/nessum-mgmt missing - cdc_acm loaded and 70-nessum.rules installed?"
fi

say "== Network interface $IFACE"
if [ -d "/sys/class/net/$IFACE" ]; then
  ok "present, mac $(cat "/sys/class/net/$IFACE/address")"
  say "type $(cat "/sys/class/net/$IFACE/type") (1 = Ethernet), mtu $(cat "/sys/class/net/$IFACE/mtu")"
  if [ "$(cat "/sys/class/net/$IFACE/carrier" 2>/dev/null)" = "1" ]; then
    ok "carrier up (joined a Nessum network)"
  else
    say "note: no carrier - adapter has not joined a Nessum network yet"
  fi
else
  fail "interface $IFACE not found (10-nessum.link installed?)"
fi

exit $rc
