# USB 2.0 ↔ Nessum Adapter

A small USB 2.0 High-Speed dongle that gives an embedded Linux host (Arm Cortex-A53)
a **Nessum** (IEEE 1901 wavelet-OFDM, formerly *HD-PLC*) wired link over existing
field wiring: an **RS-485 twisted pair** or a **low-power 24 V AC/DC cable**.

To the host it is an ordinary USB network adapter: it enumerates as a **USB CDC-NCM**
Ethernet device, so the stock Linux `cdc_ncm` driver binds to it and it shows up as a
normal network interface (`usb0` / `enx…`). No out-of-tree kernel driver is needed.

> **Status:** Requirements and architecture. Nessum IC selected (Socionext SC1320A,
> pending a price quote). No schematic or firmware yet. See
> [`docs/SPECIFICATION.md`](docs/SPECIFICATION.md) and the open questions in §9 there.

## At a glance

| | |
|---|---|
| **Host interface** | USB 2.0 High-Speed (480 Mbit/s) device, USB-C or USB-A plug |
| **Host class** | CDC-NCM network (+ optional CDC-ACM management console) |
| **Host** | Arm Cortex-A53, arm64 Linux — in-kernel `cdc_ncm` / `cdc_acm` drivers |
| **Bridge MCU** | NXP i.MX RT1062 (Cortex-M7, on-chip USB HS PHY + 10/100 ENET with RMII) — *proposed* |
| **Nessum IC** | **Socionext SC1320A** (HD-PLC4, single 3.3 V, ~0.2 W, 7×7 QFN). Fallback: MegaChips MLKHN1501AM. See [spec §4.1](docs/SPECIFICATION.md#41-nessum-ic-selection). |
| **MCU ↔ Nessum** | RMII MAC-to-MAC (no PHY), 100 Mbit/s; UART for Nessum configuration |
| **Line side** | Existing RS-485 twisted pair **or** 24 V AC/DC cable. Coupling transformer + DC-blocking caps + TVS, 2-pin terminal block. SELV only, no mains. |
| **Power** | USB bus-powered, ~0.9–1.4 W estimated (budget 2.5 W) |

## Block diagram

```
 ┌───────────── Linux host (Cortex-A53) ───────────────┐
 │  cdc_ncm  ──►  usb0 / enx…  (IP, bridge, DHCP …)    │
 └──────────────────────┬──────────────────────────────┘
                        │ USB 2.0 HS
 ┌──────────────────────▼──────────────────────────────┐
 │  Bridge MCU (i.MX RT1062)                           │
 │   USB HS device ─ CDC-NCM ◄─► frame FIFO ◄─► ENET   │
 │   CDC-ACM (mgmt) ◄─► Nessum config (UART)           │
 └───────────┬───────────────────────────┬─────────────┘
       RMII  │ 50 MHz ref clk        UART │  RESET / GPIO
 ┌───────────▼───────────────────────────▼─────────────┐
 │  Nessum IC: Socionext SC1320A                       │
 └───────────────────────┬─────────────────────────────┘
                         │ coupling xfmr + DC-block caps + TVS
                    ═════╧═════  RS-485 twisted pair  |  24 V AC/DC cable
```

## Layout

| Path | Contents |
|---|---|
| [`docs/SPECIFICATION.md`](docs/SPECIFICATION.md) | Requirements, architecture, interface choices, safety, open questions |
| [`hardware/bom.csv`](hardware/bom.csv) | Preliminary bill of materials (major parts only) |
| [`firmware/README.md`](firmware/README.md) | Bridge-MCU firmware plan (USB stack, NCM ↔ ENET datapath, management) |
| [`host/linux/`](host/linux/) | Host-side kernel config fragment, udev rule, systemd-networkd config, bring-up check |
