# Adapter management: MAC address & console protocol

**Status:** Draft v0.1. The host tool ([`../host/tools/nessumctl.py`](../host/tools/nessumctl.py))
and a reference simulator ([`../host/tools/fake_adapter.py`](../host/tools/fake_adapter.py))
implement this document. The adapter firmware must match it.

---

## 1. MAC addresses in the adapter

The adapter is a transparent layer-2 bridge between USB and the Nessum line. Linux
creates an Ethernet interface for it, and that interface's MAC address is the one other
nodes on the Nessum network see.

| Address | Where it lives | Used for |
|---|---|---|
| **Factory address** | Read-only EUI-48 in the 24AA02E48 EEPROM's write-protected upper half | Default. Each unit ships with a valid, globally unique address and needs no programming. |
| **Programmed address** | Writable lower half of the same EEPROM: address + CRC-16, two copies | Your address, set with `nessumctl mac set`. Survives power cycles **and firmware updates**, because it is not stored in firmware flash. |
| **Runtime address** | RAM only | Set from Linux with `ip link set … address …` (NCM `SET_NET_ADDRESS`). Lost on unplug. Standard Linux behaviour. |

**Active address** = the runtime address if one is set, otherwise the programmed address if
one is valid, otherwise the factory address.

The adapter reports the active address to Linux in the CDC-NCM `iMACAddress` string. The
`cdc_ncm` driver uses it as the interface's MAC, and that address appears on the wire.

**Nessum IC node address:** the SC1320A has its own MAC for Nessum-level management
(pairing, routing). It is kept **separate** from the Ethernet address above, so the IC's
bridge table never sees the same address on both sides. How it is assigned (IC factory
value or derived) is TBD from the SC1320A datasheet.

### 1.1 Validation rules (enforced by the adapter and by `nessumctl`)

Rejected:
- **Multicast/group addresses**: bit 0 of the first octet is set (e.g. `01:…`, `33:…`).
- **All-zero**: `00:00:00:00:00:00`.
- **Broadcast**: `ff:ff:ff:ff:ff:ff`.

Allowed:
- **Locally administered** addresses (bit 1 of the first octet set, e.g. `02:…`), with a
  warning.

### 1.2 Applying a new address

The Linux driver reads `iMACAddress` only when the device enumerates. So after `MAC SET`
or `MAC CLEAR` the adapter must re-enumerate: `nessumctl mac set … --apply` sends
`REBOOT`, which disconnects and reconnects the adapter from USB. The interface disappears
for about a second, then returns with the new address. udev keeps its name, because the
rules match on USB VID:PID and not on MAC.

To change the address **without** re-enumerating (e.g. in a script), use the standard
`ip link set dev nessum0 address …`. That change is lost at unplug; follow it with
`nessumctl mac set` to make it permanent.

---

## 2. Console protocol

Transport: the adapter's CDC-ACM interface (`/dev/ttyACM*`, symlinked to
`/dev/nessum-mgmt` by the udev rule). The baud rate is ignored. The host opens the tty
in raw mode. The adapter **does not echo**.

- **Request:** one ASCII line, `COMMAND [ARGS…]`, terminated by `\n` (a `\r` before it is
  ignored). Commands are case-insensitive. Arguments are separated by single spaces.
  Maximum 255 bytes.
- **Response:** zero or more data lines of the form `KEY=VALUE [KEY=VALUE…]`, then exactly
  one terminal line: `OK` or `ERR <code> <message>`. Each line ends with `\r\n`.
- One request at a time: the host waits for the terminal line before sending the next.
- MAC addresses are written as lowercase `xx:xx:xx:xx:xx:xx`. The adapter also accepts
  `-` separators and bare 12-hex-digit input.

### 2.1 Commands

| Command | Response data | Notes |
|---|---|---|
| `VERSION` | `fw=<semver> hw=<rev> nessum=<ic fw ver> proto=1` | |
| `STATUS` | `link=up\|down rate=<PHY Mbit/s> peers=<n>` | `rate` is also reported to Linux (§3) |
| `MAC GET` | `active=<mac> source=runtime\|programmed\|factory factory=<mac> programmed=<mac>\|none` | |
| `MAC SET <mac>` | `programmed=<mac> apply=reboot` | Validates (§1.1), then writes both EEPROM copies and verifies them. Does **not** change `active` until reboot. |
| `MAC CLEAR` | `programmed=none apply=reboot` | Erases the programmed address and reverts to the factory address |
| `REBOOT` | — | Sends `OK`, then disconnects from USB within 100 ms and re-enumerates |
| `NESSUM <text>` | `reply=<text>` (one line per IC reply line) | Pass-through to the Nessum IC's UART command interface |

### 2.2 Error codes

| Code | Meaning |
|---|---|
| `1` | Unknown command |
| `2` | Bad argument (syntax) |
| `3` | Invalid MAC (multicast, zero or broadcast) |
| `4` | Storage write or verify failed |
| `5` | Nessum IC not responding |

### 2.3 Example session

```
> MAC GET
< active=00:1e:c0:12:34:56 source=factory factory=00:1e:c0:12:34:56 programmed=none
< OK
> MAC SET 00:50:c2:aa:bb:cc
< programmed=00:50:c2:aa:bb:cc apply=reboot
< OK
> REBOOT
< OK
   (adapter re-enumerates; the Linux interface now uses 00:50:c2:aa:bb:cc)
```

---

## 3. What makes it look like a standard Ethernet port to Linux

Firmware obligations, so that existing software (sockets, DHCP clients, bridges, VLANs,
`ip`, `ethtool -i`, NetworkManager, systemd-networkd) works unchanged:

| Behaviour | How |
|---|---|
| ARPHRD_ETHER netdev with a normal MAC | CDC-NCM class, `cdc_ncm` driver. With a globally administered MAC the kernel names it `eth%d`. udev/systemd then renames it (see `host/linux/`). |
| MTU 1500, VLAN-tagged frames | `wMaxSegmentSize` = **1518** (1514 + 802.1Q tag). Check this against the SC1320A's maximum frame size. |
| `ip link set address` | Implement NCM `SET_NET_ADDRESS` / `GET_NET_ADDRESS`, and set `bmNetworkCapabilities` bit 1 |
| Promiscuous / multicast (bridging, IPv6, mDNS) | Implement `SET_ETHERNET_PACKET_FILTER`. The adapter forwards everything by default. |
| Carrier (`ip link` `LOWER_UP`, `ethtool` link detected) | `NETWORK_CONNECTION` notification follows the Nessum link |
| Link speed in `ethtool` | `CONNECTION_SPEED_CHANGE` notification carries the current Nessum PHY rate |

Known differences from a PHY-based NIC (none of these affect ordinary IP software):
`ethtool -i` reports driver `cdc_ncm`. There is no auto-negotiation, no MDIO/PHY registers,
no Wake-on-LAN, no PTP hardware timestamping, and no checksum/TSO offload.
