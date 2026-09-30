# USB 2.0 ↔ Nessum Adapter — Specification

**Document status:** Draft v0.4 (AM62x host, `eth2`, factory-programmed and locked MAC + network key)
**Last updated:** 2026-09-30

---

## 1. Purpose

Give an embedded Linux system built on an Arm Cortex-A processor a Nessum wired-network
port through its USB 2.0 host port, without writing or maintaining a custom kernel driver.

### 1.1 Goals

1. **Driverless on Linux.** Use a standard USB class (CDC-NCM) so the upstream `cdc_ncm`
   driver works on any kernel ≥ 3.x and any CPU architecture.
2. **Saturate the Nessum link.** The USB side must never be the bottleneck. Nessum
   application throughput sits well below 100 Mbit/s, and USB 2.0 HS with NCM
   aggregation delivers far more than that in practice.
3. **Manageable.** Expose Nessum configuration (network key, role, pairing, firmware
   update, link statistics) to Linux userspace.
4. **Robust line interface.** Functional isolation between the USB/host ground and the
   field wiring, with surge protection for long runs of low-voltage cable.
5. **Small and bus-powered** where the power budget allows.

### 1.2 Non-goals (v1)

- Host drivers for Windows or macOS. CDC-NCM works natively on recent Windows and macOS,
  but they are not validated.
- USB 3.x SuperSpeed.
- Being a Nessum *coordinator* for large multi-hop networks. v1 acts as a terminal/end node.
  Coordinator support depends on the chosen IC firmware.

---

## 2. Assumptions & clarifications

| Request term | Interpreted as | Note |
|---|---|---|
| **Host CPU** | **TI AM62x** (confirmed; Cortex-A53), TI Processor SDK Linux (arm64) | The adapter uses a standard USB class and the host driver is architecture-independent. The only SoC-specific part is the USB host controller: DWC3 with the AM62 glue (§8). |
| **"Standard Ethernet port"** (requirement) | Linux sees an ordinary Ethernet netdev (`ARPHRD_ETHER`, MAC, MTU 1500, carrier, VLAN, multicast), so existing software runs unchanged | Met by CDC-NCM + the in-kernel `cdc_ncm` driver. The firmware obligations and the known differences from a PHY-based NIC are in [MANAGEMENT.md §5](MANAGEMENT.md#5-what-makes-it-look-like-a-standard-ethernet-port-to-linux). |
| **Interface name** (confirmed) | `eth2` | Next after the AM62x CPSW's `eth0`/`eth1`. Alternative name `nessum0`. |
| **MAC address** (confirmed) | Programmed **at the factory** from your own address block, then **locked** | `factory_program.py` assigns addresses without duplicates. After the lock, no change is possible over USB, persistent or runtime. See [MANAGEMENT.md §1, §3](MANAGEMENT.md#1-mac-addresses-in-the-adapter). |
| **Nessum network key** (confirmed) | Programmed **at the factory**, then locked together with the MAC | Write-only over USB (only a fingerprint can be read). Sealed with the RT1062's chip-unique key. See [MANAGEMENT.md §2](MANAGEMENT.md#2-nessum-network-key). |
| **USB 2.0** | USB 2.0 **High-Speed** (480 Mbit/s) device | Full-Speed (12 Mbit/s) would bottleneck the link. |
| **Nessum** | Nessum (formerly HD-PLC), IEEE 1901-2020 wavelet-OFDM | — |
| **Medium** (confirmed) | **(a)** an existing **twisted pair currently carrying RS-485**, or **(b)** an existing **low-power 24 V AC or 24 V DC** cable | Both are SELV / low-voltage media, **not AC mains** (§6). Assumption: Nessum **replaces** RS-485 signalling on the pair. Sharing the pair with live RS-485 traffic is not planned (§6.1). |

---

## 3. System architecture

### 3.1 Options considered

| # | Architecture | Pros | Cons | Verdict |
|---|---|---|---|---|
| A | **USB-HS MCU + RMII MAC-to-MAC to Nessum IC**, MCU runs CDC-NCM | Driverless on Linux. Full control of the USB descriptors. Management channel on the same USB cable. Few extra parts. | Firmware for the bridge MCU has to be written. | **Selected** |
| B | Off-the-shelf USB↔Ethernet bridge (ASIX / Microchip LAN95xx class) + Nessum IC | No firmware to write | Most of these bridges have an integrated PHY and no exposed MII/RMII. Would need PHY-to-PHY back-to-back or magnetics. No path for managing the Nessum IC. Vendor drivers (`asix`, `smsc95xx`) are fine, but the design is locked to that part. | Fallback |
| C | Nessum IC UART/SPI straight to a USB-UART bridge | Simplest hardware | Throughput is limited by UART/SPI. Needs a custom host protocol or driver. | Rejected |
| D | Put the Nessum IC on the Cortex-A board over RMII/SPI | Lowest BOM | Not a USB adapter. Needs board changes and a custom device-tree/driver. | Out of scope |

### 3.2 Selected datapath (option A)

```
Linux ─USB HS─► [RT1062 USB device ctrl + PHY] ─NCM NTB unpack─► frame queue
      ─► [RT1062 ENET MAC] ─RMII─► [Nessum IC MAC] ─► Nessum PHY/AFE ─► medium
```

- **CDC-NCM** batches several Ethernet frames into one USB transfer (NTB). That keeps
  host CPU and interrupt load low on a small Cortex-A.
- **RMII MAC-to-MAC:** the Nessum IC and the MCU ENET talk directly with no Ethernet PHY.
  One side drives the 50 MHz reference clock (TBD per IC datasheet). Link is fixed at
  100 Mbit/s full-duplex. The RT1062 ENET is configured with no MDIO PHY polling.
- **Management:** a second USB function (CDC-ACM, `/dev/ttyACM*`) carries a line-based
  adapter command protocol (MAC address, status, version, reboot). A `NESSUM` command
  passes text through to the Nessum IC's UART. See [MANAGEMENT.md](MANAGEMENT.md).

### 3.3 Throughput budget

| Segment | Raw | Expected usable |
|---|---|---|
| USB 2.0 HS, CDC-NCM | 480 Mbit/s | ~200–300 Mbit/s (host-controller dependent) |
| RMII | 100 Mbit/s | ~95 Mbit/s |
| Nessum link | PHY rate depends on IC/generation | Tens of Mbit/s application throughput, depending on line conditions |

The **line** is the bottleneck by a wide margin, as intended. The MCU needs only modest
buffering (≈ 32–64 KiB of frame queue per direction) to absorb bursts.

---

## 4. Major components (proposed)

| Function | Part | Why | To verify |
|---|---|---|---|
| Bridge MCU | **NXP i.MX RT1062** (Cortex-M7, 600 MHz) | On-chip USB 2.0 HS PHY, so no ULPI PHY is needed. 10/100 ENET with RMII. 1 MB on-chip SRAM. Mature USB device stack (MCUXpresso SDK / TinyUSB). | Package and availability. Alternatives: STM32H7 + external ULPI PHY (USB3300), or other HS-PHY + ETH MCUs. |
| Nessum IC | **Socionext SC1320A** (**selected**, §4.1) | 4th-generation Nessum (HD-PLC4, IEEE 1901-2020). RMII MAC + UART/SPI host interfaces. Single 3.3 V supply. ~200 mW. 7×7 mm QFN. | Price quote, datasheet/NDA, line-driver needs, eval kit |
| Nessum IC (fallback) | **MegaChips MLKHN1500AM** (single-hop) / **MLKHN1501AM** (multi-hop) | Earlier generation (HD-PLC3). MII/RMII + UART. Stocked at distributors with published prices. | Only if the SC1320A quote or support falls through |
| Boot flash | QSPI NOR, 8–16 MB | XIP firmware for the RT1062 | — |
| Clocking | 24 MHz crystal (RT1062). 50 MHz RMII ref (from the Nessum IC or an oscillator). | — | Which side sources REF_CLK |
| Line coupling | Wideband coupling transformer + DC-blocking capacitors + TVS | Per the IC vendor reference design, adapted for twisted pair / 24 V (§6) | — |
| Power | 5 V USB → buck 3.3 V (+ RT1062 internal core regulator) | The SC1320A needs only 3.3 V | Power budget (§5) |

A preliminary BOM is in [`../hardware/bom.csv`](../hardware/bom.csv).

### 4.1 Nessum IC selection

Two Nessum ICs are publicly offered with an RMII host interface:

| | **Socionext SC1320A** | **MegaChips MLKHN1500AM / 1501AM** |
|---|---|---|
| Generation / standard | HD-PLC4, IEEE 1901-2020 (newest) | HD-PLC3 (earlier generation) |
| Host interfaces | RMII MAC, UART/SPI | MII/RMII, UART |
| Supply | **Single 3.3 V** | 3.3 V + 1.2 V |
| Power (typical) | **~0.2 W** | ~0.57 W active, 0.12 W standby |
| Package | **7×7 mm QFN**: 4-layer board, easy assembly | 238-ball LBGA, 18×15 mm: finer PCB rules, X-ray inspection |
| Integrated memory / AFE | AFE integrated. External line driver: TBD from datasheet. | 128 Mb SDRAM + AFE integrated |
| Reach | Up to 10 km with multi-hop (vendor figure) | 1500AM single-hop, 1501AM multi-hop |
| Band | 2–28 MHz; runs over twisted pair and DC lines | 2–28 MHz |
| Price (Sept 2026) | **Not published.** Quote needed from Socionext or its distributor. | **Published:** ~US$8–9 in 1k from the MegaChips store, ~US$14–18 for single units at DigiKey |

**Selected: Socionext SC1320A.** For these media and a USB-powered dongle it wins on
every technical point:

- **Power.** About a third of the MLKHN1500AM's draw, which leaves plenty of margin in
  the 2.5 W USB budget (§5).
- **Board cost.** One supply rail and a QFN instead of a 238-ball BGA mean fewer regulators,
  a cheaper 4-layer PCB, and simpler assembly and inspection. That offsets a chip price
  somewhat above the MegaChips part.
- **Longevity.** It implements the current generation of the standard, where the
  MLKHN150x is the previous one. That matters for a new design.
- **Reach.** Multi-hop and long reach suit long RS-485 runs and 24 V building wiring.

**Condition:** SC1320A pricing is not public. Get a quote at the expected volume, and
confirm Socionext will support low-volume customers, before committing the layout. If the
quote lands well above ~US$10 in volume, or support is not available, switch to the
**MLKHN1501AM** (multi-hop variant). The datapath does not change, because both use RMII
plus UART. What changes is the second supply rail, the BGA footprint and ~0.4 W more in the
power budget.

**Interoperability:** every node on the same cable must speak the same Nessum generation.
Confirm with the vendor whether HD-PLC4 and HD-PLC3 nodes interoperate before mixing
chips, and pick one IC for all nodes.

---

## 5. Power budget (preliminary)

| Load | Estimate |
|---|---|
| SC1320A | ~0.2 W typical (vendor figure) |
| External line driver (if the datasheet requires one) | **TBD**, allow ~0.3–0.5 W |
| i.MX RT1062 + QSPI | ~0.3–0.5 W at full speed |
| Regulator losses | ~15 % |
| **Estimated total** | **~0.9–1.4 W** |
| **USB 2.0 budget** | **2.5 W** (500 mA @ 5 V after enumeration) |

The adapter should be **USB bus-powered**, with margin. It never draws power from the
twisted pair or the 24 V cable, so it adds no load to the existing installation.
(With the MLKHN1500AM fallback, add ~0.4 W. That still fits.)

---

## 6. Line interface & safety

Both target media are low-voltage (SELV), so the mains-grade safety parts (X2/Y caps,
fuses, reinforced creepage) are **not needed**. One adapter design with a single
2-pin line connector covers both media. Only the coupling-capacitor voltage rating and the
TVS are chosen for the worst case (24 V AC peak ≈ 34 V, plus transients).

Common line-side circuit:

- **Wideband coupling transformer** (per the vendor reference design): keeps the host/USB
  ground **functionally isolated** from the field wiring. This breaks the ground loops that
  are common on long RS-485 and 24 V runs.
- **DC-blocking capacitors** in series on both legs, rated ≥ 100 V: they pass 2–28 MHz and
  block 24 V DC and 50/60 Hz AC.
- **Bidirectional TVS** across the line (≈ 36–40 V standoff, so 24 V AC peaks don't
  trigger it) plus common-mode surge protection. Field cables pick up surges.
- **Plug-in 2-pin terminal block** (5.08 mm) for the pair.

### 6.1 Medium (a): existing RS-485 twisted pair

- Nessum **replaces** the RS-485 traffic. Remove or disconnect the RS-485 transceivers
  from the pair. Their input capacitance and the stubs to them load and reflect the MHz
  signal. RS-485 at high baud rates also overlaps the Nessum band.
- **120 Ω end terminations:** leave them in for a first test. Nessum usually copes with a
  resistive load, but check the vendor's guidance on termination for twisted pair.
- If RS-485 must keep running in parallel on the same pair, a coupling filter (high-pass
  to Nessum, low-pass to RS-485) is needed on every node. That is out of scope for v1.
- **Best case of the two media.** A twisted pair has controlled impedance and few
  branches, so expect the highest throughput and longest reach here.

### 6.2 Medium (b): existing 24 V AC / DC power cable

- The adapter only **injects signal** onto the pair and does not take power from it.
- **Impedance problem:** every load on the 24 V line (power supply input capacitors,
  transformers, relays) shorts the high-frequency signal. Plan an **RF choke / ferrite
  filter at each load and at the 24 V source** (a low-pass that passes 24 V and blocks
  2–28 MHz). This is the main field-installation cost of this medium.
- 24 V AC from a transformer is noisy. Switching supplies on 24 V DC are worse. Nessum's
  wavelet-OFDM with error correction is designed for this, but expect lower throughput
  than on the twisted pair.

### 6.3 EMC

Signalling at 2–28 MHz on unshielded field wiring radiates. Use the IC's band-plan and
notch settings to stay out of amateur and broadcast bands for the target region, and
budget for pre-compliance testing (conducted and radiated emissions for the region, e.g.
FCC Part 15 or CISPR 32).

---

## 7. USB device definition

| Item | Value |
|---|---|
| Speed | High-Speed (480 Mbit/s) |
| VID:PID | **TBD.** Use a vendor-owned VID or apply for a free PID (e.g. pid.codes for open hardware). Never ship with a borrowed ID. |
| Configuration | 1 config, bus-powered, `bMaxPower` set from §5 |
| Function 0 | CDC-NCM (Communication + Data interfaces, IAD) |
| Function 1 | CDC-ACM management console (**required**: MAC programming and status, [MANAGEMENT.md §4](MANAGEMENT.md#4-console-protocol)) |
| MAC address | Factory-programmed address (the 24AA02E48's EUI-48 only on not-yet-programmed units) reported in the NCM `iMACAddress` string. `SET_NET_ADDRESS` is refused (STALL) once the unit is factory-locked. See [MANAGEMENT.md §1](MANAGEMENT.md#1-mac-addresses-in-the-adapter). |
| Max segment | `wMaxSegmentSize` = 1518, so the host gets MTU 1500 with 802.1Q VLAN tags |
| Packet filter | `SET_ETHERNET_PACKET_FILTER` implemented (promiscuous/multicast for bridges, IPv6, mDNS) |
| NTB sizes | IN/OUT max ≥ 16 KiB (tune during bring-up) |
| Link status | NCM `NETWORK_CONNECTION` notification follows the **Nessum** link state, so `carrier` is up on the host only while the adapter has joined a Nessum network. `CONNECTION_SPEED_CHANGE` reports the Nessum PHY rate, which `ethtool` displays. |

---

## 8. Host-side (Linux) integration

Target: TI Processor SDK Linux on an **AM62x**. **No custom kernel driver.**

- **Kernel:** `cdc_ncm` + `cdc_acm`, plus the AM62x USB host controller (DWC3 +
  `USB_DWC3_AM62`, xHCI). The USB port's device-tree node (`&usb0`/`&usb1`) must be in
  host or OTG mode. See [`../host/linux/kernel.config`](../host/linux/kernel.config).
- **Interface name `eth2`:** set by [`../host/linux/10-nessum.link`](../host/linux/10-nessum.link),
  with alternative name `nessum0`. It matches on USB VID:PID, and sets
  `MACAddressPolicy=none` so systemd never replaces the adapter's factory MAC. On a
  rootfs without systemd-udevd, use the commented `NAME="eth2"` rule in
  `70-nessum.rules` instead.
- **Management console:** [`../host/linux/70-nessum.rules`](../host/linux/70-nessum.rules)
  creates `/dev/nessum-mgmt`, restricted to root and the `netdev` group.
- **Network config** (systemd-networkd; the TI SDK's Arago rootfs uses systemd):
  [`../host/linux/20-nessum.network`](../host/linux/20-nessum.network).
- **Status and inspection:** [`../host/tools/nessumctl.py`](../host/tools/nessumctl.py)
  (`mac get`, `key status`, `status`, `version`). Python 3, standard library only.
- **Factory programming:** [`../host/tools/factory_program.py`](../host/tools/factory_program.py)
  (MAC from your block + network key + lock, with an append-only CSV log). Both tools are
  tested against the protocol simulator [`fake_adapter.py`](../host/tools/fake_adapter.py).
- **Bring-up check:** [`../host/linux/check-adapter.sh`](../host/linux/check-adapter.sh).

**Alternative if the host board can be changed:** the AM62x CPSW Ethernet switch
supports RMII. Wiring the SC1320A straight to a spare CPSW port (fixed-link, no USB and
no bridge MCU) gives a native `am65-cpsw-nuss` Ethernet port with fewer parts. The MAC then
comes from the SoC's eFuse or device tree. This is only an option for new host board
revisions. The USB adapter remains the plan for existing boards.

---

## 9. Open questions

| # | Question | Impact |
|---|---|---|
| Q1 | ~~Host~~ Resolved: AM62x, interface `eth2`, MAC + network key factory-programmed and locked. Still open: TI SDK version, and which AM62x USB port (`usb0`/`usb1`) the adapter plugs into. | Device-tree `dr_mode` |
| Q1b | Network-key scope: one key per kit/installation (recommended) or one for the whole product? What is your address block (base/size)? | Factory procedure ([MANAGEMENT.md §2](MANAGEMENT.md#2-nessum-network-key)) |
| Q1c | Key size and storage in the SC1320A (AES-128 assumed; does the IC have its own protected key storage?) | `NKEY_LEN`, key sealing design |
| Q2 | ~~Medium~~ Resolved: RS-485 twisted pair or 24 V AC/DC cable. Still open: region, and cable lengths / number of nodes per cable. | EMC band plan, multi-hop need |
| Q3 | ~~IC~~ Resolved: SC1320A selected (§4.1). Still open: Socionext quote at our volume, datasheet/NDA/SDK access, eval kit. | Final go/no-go on SC1320A vs MLKHN1501AM |
| Q3a | Does RS-485 traffic need to keep running on the same pair during migration? | Adds a filter per node (§6.1) |
| Q4 | Node role: end node only, or also coordinator / multi-hop relay? | IC variant, firmware |
| Q5 | Throughput and latency targets? | Buffer sizing, IC generation |
| Q6 | Form factor: dongle with USB-A/C plug, or board with cable? Enclosure? | Mechanical |
| Q7 | USB VID/PID ownership? | Descriptors, certification |
| Q8 | Volume and cost target? | MCU choice (RT1062 vs cheaper HS-PHY part) |

---

## 10. Plan

1. **Phase 0, evaluation (no custom hardware).** Nessum vendor eval board (RMII
   exposed) + i.MX RT1060-EVK wired over RMII. Bring up the CDC-NCM ↔ ENET bridge
   firmware and verify on the target Linux host with `iperf3` across a Nessum link.
2. **Phase 1, schematic/layout rev A.** RT1062 + Nessum IC + AFE per vendor reference.
   Isolation and EMC review.
3. **Phase 2, firmware features.** Management channel, link-state notifications, factory
   MAC/key programming and lock, sealed key storage, and firmware update (USB DFU class).
   **Firmware update must accept only signed images, with RT1062 HAB secure boot enabled
   in production.** Otherwise unsigned firmware loaded over USB could read the network key
   or ignore the lock.
4. **Phase 3, validation.** Throughput/latency, hot-plug and suspend/resume on the host,
   conducted emissions, safety pre-compliance.
