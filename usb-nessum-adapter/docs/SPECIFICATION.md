# USB 2.0 ↔ Nessum Adapter — Specification

**Document status:** Draft v0.1 (project start)
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
4. **Safe line interface.** Galvanic isolation between the USB/host ground and the
   medium, with surge and fault protection suited to the target medium.
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
| **ARM Cortex-A52** | *Some* Arm Cortex-A application core running Linux | Arm has never made a "Cortex-A52". It is most likely a Cortex-**A53**, **A55**, A510 or A520. The design does not depend on which one: the adapter uses a standard USB class and the host driver is architecture-independent. **Please confirm the exact SoC** (§9, Q1) so the kernel config and USB controller quirks can be checked. |
| **USB 2.0** | USB 2.0 **High-Speed** (480 Mbit/s) device | Full-Speed (12 Mbit/s) would bottleneck the link. |
| **Nessum** | Nessum (formerly HD-PLC), IEEE 1901-2020 wavelet-OFDM | Nessum also runs over non-mains media (coax, twisted pair, DC lines). The medium changes the coupling circuit (§6). |

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
- **Management:** a second USB function (CDC-ACM, `/dev/ttyACM0`) is tunnelled by the MCU
  to the Nessum IC's UART command interface. Alternative: a vendor-specific interface
  used from `libusb`. Decide after reading the IC's host-command documentation.

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
| Nessum IC | **Socionext SC1320A** | Has an RMII MAC and a UART/SPI host interface | Datasheet/NDA access, reference AFE, firmware licensing |
| Nessum IC (alt) | **MegaChips MLKHN1500AM** (single-hop) / **MLKHN1501AM** (multi-hop) | Has MII/RMII and UART host interfaces | Same as above |
| Boot flash | QSPI NOR, 8–16 MB | XIP firmware for the RT1062 | — |
| Clocking | 24 MHz crystal (RT1062). 50 MHz RMII ref (from the Nessum IC or an oscillator). | — | Which side sources REF_CLK |
| Line coupling | Nessum-rated coupling transformer + X/Y-rated coupling cap + TVS + fuse | Per the IC vendor reference design | Medium-specific (§6) |
| Power | 5 V USB → buck 3.3 V → LDOs for 1.1/1.8 V as needed | — | Power budget (§5) |

A preliminary BOM is in [`../hardware/bom.csv`](../hardware/bom.csv).

---

## 5. Power budget (preliminary)

| Load | Estimate |
|---|---|
| Nessum IC | ~0.2–0.25 W typical (vendor figures) |
| Line driver / AFE (TX) | **TBD.** This is the load most likely to break the USB budget. |
| i.MX RT1062 + QSPI | ~0.3–0.5 W at full speed |
| Regulator losses | ~15 % |
| **USB 2.0 budget** | **2.5 W** (500 mA @ 5 V after enumeration) |

If the TX line driver pushes the total over ~2 W, choose one of these:

- Power the adapter from the medium through an isolated AC/DC.
- Use USB-C with a 1.5 A current advertisement.

Decide once the AFE reference design is in hand.

---

## 6. Line interface & safety

- **Galvanic isolation:** the coupling transformer is the isolation barrier between the
  host/USB ground and the medium. Creepage and clearance must meet the insulation class
  required for the medium. For AC mains this is reinforced insulation per IEC 62368-1.
- **Mains coupling:** X2/Y-rated coupling capacitor, fuse, MOV/TVS surge protection, and a
  bleeder for the coupling cap. The enclosure must keep mains-connected parts out of reach.
- **Non-mains media** (coax, twisted pair, DC bus): the coupling network is simpler and
  the lower insulation requirements may allow bus power with margin.
- **EMC:** PLC injects conducted RF (roughly 2–28 MHz, or wider depending on band
  plan). Regional conducted-emission limits and notching (e.g. EN 50561-1 for in-home
  mains PLC in the EU) apply. Band plan and notch settings live in Nessum IC firmware and
  must be set per region.

---

## 7. USB device definition

| Item | Value |
|---|---|
| Speed | High-Speed (480 Mbit/s) |
| VID:PID | **TBD.** Use a vendor-owned VID or apply for a free PID (e.g. pid.codes for open hardware). Never ship with a borrowed ID. |
| Configuration | 1 config, bus-powered, `bMaxPower` set from §5 |
| Function 0 | CDC-NCM (Communication + Data interfaces, IAD) |
| Function 1 | CDC-ACM management console (optional, IAD) |
| MAC address | Unique per unit. From MCU OTP/fuses or an EEPROM (e.g. 24AA02E48). Reported in the NCM `iMACAddress` string. |
| NTB sizes | IN/OUT max ≥ 16 KiB (tune during bring-up) |
| Link status | NCM `NETWORK_CONNECTION` notification follows the **Nessum** link state, so `carrier` is up on the host only while the adapter has joined a Nessum network |

---

## 8. Host-side (Linux) integration

- Kernel: `CONFIG_USB_NET_DRIVERS`, `CONFIG_USB_USBNET`, `CONFIG_USB_NET_CDC_NCM`,
  `CONFIG_USB_ACM`. See [`../host/linux/kernel.config`](../host/linux/kernel.config).
- Stable interface name via udev: [`../host/linux/70-nessum.rules`](../host/linux/70-nessum.rules).
- Network config with systemd-networkd: [`../host/linux/20-nessum.network`](../host/linux/20-nessum.network).
- Bring-up check script: [`../host/linux/check-adapter.sh`](../host/linux/check-adapter.sh).
- Userspace management tool (`nessumctl`) over `/dev/ttyACM*`: planned once the IC
  command set is known.

---

## 9. Open questions

| # | Question | Impact |
|---|---|---|
| Q1 | Exact host SoC and kernel version? ("Cortex-A52" is not an Arm part.) | USB host-controller quirks, kernel config, test plan |
| Q2 | Which medium: AC mains, DC bus, coax, or twisted pair? Which region? | Coupling/AFE design, safety class, power source, EMC band plan |
| Q3 | Nessum IC vendor: Socionext SC1320A or MegaChips MLKHN150x? Do we have datasheet/NDA/SDK access? | RMII clocking, management protocol, firmware licensing |
| Q4 | Node role: end node only, or also coordinator / multi-hop relay? | IC variant, firmware |
| Q5 | Throughput and latency targets? | Buffer sizing, IC generation |
| Q6 | Form factor and connector: dongle with USB-A/C plug, or board with cable? Enclosure? | Mechanical, isolation creepage |
| Q7 | USB VID/PID ownership? | Descriptors, certification |
| Q8 | Volume and cost target? | MCU choice (RT1062 vs cheaper HS-PHY part) |

---

## 10. Plan

1. **Phase 0, evaluation (no custom hardware).** Nessum vendor eval board (RMII
   exposed) + i.MX RT1060-EVK wired over RMII. Bring up the CDC-NCM ↔ ENET bridge
   firmware and verify on the target Linux host with `iperf3` across a Nessum link.
2. **Phase 1, schematic/layout rev A.** RT1062 + Nessum IC + AFE per vendor reference.
   Isolation and EMC review.
3. **Phase 2, firmware features.** Management channel, link-state notifications, DFU
   firmware update (USB DFU class), production MAC/serial programming.
4. **Phase 3, validation.** Throughput/latency, hot-plug and suspend/resume on the host,
   conducted emissions, safety pre-compliance.
