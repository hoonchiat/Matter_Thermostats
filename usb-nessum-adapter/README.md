# USB 2.0 ↔ Nessum Adapter

A small USB 2.0 High-Speed dongle that gives an embedded Linux host (Arm Cortex-A class)
a **Nessum** (IEEE 1901 wavelet-OFDM, formerly *HD-PLC*) wired link over power lines or
other existing wiring.

To the host it is an ordinary USB network adapter: it enumerates as a **USB CDC-NCM**
Ethernet device, so the stock Linux `cdc_ncm` driver binds to it and it shows up as a
normal network interface (`usb0` / `enx…`). No out-of-tree kernel driver is needed.

> **Status:** Project start — requirements, architecture and bring-up plan only. No
> schematic or firmware yet. See [`docs/SPECIFICATION.md`](docs/SPECIFICATION.md) and the
> open questions in §9 there.

## At a glance

| | |
|---|---|
| **Host interface** | USB 2.0 High-Speed (480 Mbit/s) device, USB-C or USB-A plug |
| **Host class** | CDC-NCM network (+ optional CDC-ACM management console) |
| **Host OS** | Linux, any architecture — in-kernel `cdc_ncm` / `cdc_acm` drivers |
| **Bridge MCU** | NXP i.MX RT1062 (Cortex-M7, on-chip USB HS PHY + 10/100 ENET with RMII) — *proposed* |
| **Nessum IC** | Socionext SC1320A **or** MegaChips MLKHN1500AM — both expose RMII + UART — *to be selected* |
| **MCU ↔ Nessum** | RMII MAC-to-MAC (no PHY), 100 Mbit/s; UART for Nessum configuration |
| **Line side** | Coupling transformer + coupling capacitor + surge protection to the medium (AC mains, DC bus, twisted pair or coax — TBD) |
| **Power** | USB bus-powered target (≤ 500 mA @ 5 V) — budget to be confirmed |

## Block diagram

```
 ┌───────────── Linux host (Arm Cortex-A) ─────────────┐
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
 │  Nessum IC (SC1320A / MLKHN1500AM) + AFE            │
 └───────────────────────┬─────────────────────────────┘
                         │ coupling xfmr + cap + TVS/fuse
                    ═════╧═════  medium (power line …)
```

## Layout

| Path | Contents |
|---|---|
| [`docs/SPECIFICATION.md`](docs/SPECIFICATION.md) | Requirements, architecture, interface choices, safety, open questions |
| [`hardware/bom.csv`](hardware/bom.csv) | Preliminary bill of materials (major parts only) |
| [`firmware/README.md`](firmware/README.md) | Bridge-MCU firmware plan (USB stack, NCM ↔ ENET datapath, management) |
| [`host/linux/`](host/linux/) | Host-side kernel config fragment, udev rule, systemd-networkd config, bring-up check |
