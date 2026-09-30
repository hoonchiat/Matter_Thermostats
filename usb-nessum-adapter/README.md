# USB 2.0 ↔ Nessum Adapter

A small USB 2.0 High-Speed dongle that gives an embedded Linux host (TI Sitara Cortex-A53)
a **Nessum** (IEEE 1901 wavelet-OFDM, formerly *HD-PLC*) wired link over existing
field wiring: an **RS-485 twisted pair** or a **low-power 24 V AC/DC cable**.

To Linux it is a **standard Ethernet port**. It enumerates as a **USB CDC-NCM** device,
the stock `cdc_ncm` driver binds to it, and it appears as an ordinary Ethernet interface
(named `nessum0`, or an `ethN` of your choice) with a MAC, MTU 1500, carrier and VLAN
support. Existing networking software works unchanged, and no custom kernel driver is
needed.

The **Ethernet MAC address is stored in the adapter** and can be programmed from Linux:

```sh
nessumctl mac get                          # active / factory / programmed
nessumctl mac set 00:50:c2:aa:bb:cc --apply  # persistent; adapter re-enumerates
nessumctl mac clear --apply                # back to the factory EUI-48
```

Install the tool on the target with `install -m 0755 host/tools/nessumctl.py /usr/local/bin/nessumctl`
(it needs only Python 3). See [`docs/MANAGEMENT.md`](docs/MANAGEMENT.md).

> **Status:** Requirements and architecture. Nessum IC selected (Socionext SC1320A,
> pending a price quote). No schematic or firmware yet. See
> [`docs/SPECIFICATION.md`](docs/SPECIFICATION.md) and the open questions in §9 there.

## At a glance

| | |
|---|---|
| **Host interface** | USB 2.0 High-Speed (480 Mbit/s) device, USB-C or USB-A plug |
| **Host class** | CDC-NCM network + CDC-ACM management console |
| **Host** | TI AM6x (Cortex-A53), TI Processor SDK Linux — in-kernel `cdc_ncm` / `cdc_acm` drivers |
| **MAC address** | Factory EUI-48 by default. Programmable and persistent (EEPROM) via `nessumctl`. `ip link set address` also works. |
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
| [`docs/MANAGEMENT.md`](docs/MANAGEMENT.md) | MAC address storage and programming, console protocol, "standard Ethernet" obligations |
| [`host/linux/`](host/linux/) | TI kernel config fragment, `.link` naming, udev rule, systemd-networkd config, bring-up check |
| [`host/tools/`](host/tools/) | `nessumctl.py` (MAC programming and status), `fake_adapter.py` protocol simulator, tests (`python3 -m unittest`) |
