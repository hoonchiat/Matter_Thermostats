# Bridge-MCU firmware (plan)

Target: NXP i.MX RT1062 (Cortex-M7), bare-metal or FreeRTOS, MCUXpresso SDK drivers.

## Components

| Component | Responsibility |
|---|---|
| `usb/` | USB HS device stack (TinyUSB or the MCUXpresso USB stack). Composite descriptors: CDC-NCM + CDC-ACM (+ DFU later). |
| `ncm/` | NTB16 parse (host→device) and build (device→host). Aggregates frames up to the negotiated NTB size, with a short aggregation timeout (~100 µs) to bound latency. |
| `enet/` | ENET MAC in RMII mode, fixed 100 Mbit/s full-duplex, no MDIO/PHY polling. Zero-copy DMA descriptor rings shared with the NCM layer where alignment allows. |
| `nessum/` | Nessum IC control over UART: reset/boot sequencing, configuration, link-state polling or IRQ. Link state drives the NCM `NETWORK_CONNECTION` notification. |
| `mgmt/` | CDC-ACM console protocol per [`../docs/MANAGEMENT.md`](../docs/MANAGEMENT.md): `VERSION`, `STATUS`, `MAC GET/SET/CLEAR`, `REBOOT`, `NESSUM` pass-through. [`../host/tools/fake_adapter.py`](../host/tools/fake_adapter.py) is the executable reference for its behaviour. |
| `macaddr/` | MAC selection (runtime → programmed → factory). Reads the factory EUI-48 from the 24AA02E48 (0xFA–0xFF). The programmed address is stored as two CRC-16-protected copies in the EEPROM's writable lower half, and each write is read back to verify. Rejects multicast, zero and broadcast. Handles NCM `SET/GET_NET_ADDRESS` (runtime only, never persisted). |
| `board/` | Clocks, pin mux, RMII REF_CLK direction, EEPROM I²C. |

## "Standard Ethernet port" obligations

These are what the Linux `cdc_ncm` driver needs to present an ordinary Ethernet
interface (see [`../docs/MANAGEMENT.md §3`](../docs/MANAGEMENT.md#3-what-makes-it-look-like-a-standard-ethernet-port-to-linux)):

- `iMACAddress` string = the active MAC.
- `bmNetworkCapabilities` advertises the net-address and packet-filter requests.
- `wMaxSegmentSize` = 1518 (MTU 1500 plus a VLAN tag).
- `NETWORK_CONNECTION` and `CONNECTION_SPEED_CHANGE` notifications follow the Nessum link.
- `REBOOT` performs a USB soft-disconnect and re-enumeration, so Linux re-reads
  `iMACAddress` after a MAC change.

## Datapath rules

- Never drop frames silently. Count drops per direction and expose the counters over `mgmt`.
- Apply back-pressure: stop taking OUT transfers while the ENET TX ring is full rather
  than queueing without bound.
- Keep frame buffers in the OCRAM/DTCM region that the ENET and USB DMA can reach. Handle
  cache maintenance explicitly (the Cortex-M7 D-cache is on).

## Bring-up milestones

1. USB enumerates as CDC-NCM on the Linux host. A loopback mode (OUT frames echoed to IN)
   passes `ping` to a static neighbour entry.
2. ENET RMII link to the Nessum eval board. Frames go across the power line to a second
   Nessum node. Measure with `iperf3`.
3. Management console reaches the Nessum IC command interface.
4. Link-state notifications, counters, USB DFU update.
