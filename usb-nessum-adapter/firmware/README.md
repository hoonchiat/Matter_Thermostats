# Bridge-MCU firmware (plan)

Target: NXP i.MX RT1062 (Cortex-M7), bare-metal or FreeRTOS, MCUXpresso SDK drivers.

## Components

| Component | Responsibility |
|---|---|
| `usb/` | USB HS device stack (TinyUSB or the MCUXpresso USB stack). Composite descriptors: CDC-NCM + CDC-ACM (+ DFU later). |
| `ncm/` | NTB16 parse (host→device) and build (device→host). Aggregates frames up to the negotiated NTB size, with a short aggregation timeout (~100 µs) to bound latency. |
| `enet/` | ENET MAC in RMII mode, fixed 100 Mbit/s full-duplex, no MDIO/PHY polling. Zero-copy DMA descriptor rings shared with the NCM layer where alignment allows. |
| `nessum/` | Nessum IC control over UART: reset/boot sequencing, configuration, link-state polling or IRQ. Link state drives the NCM `NETWORK_CONNECTION` notification. |
| `mgmt/` | Tunnels the CDC-ACM console to the Nessum UART (transparent mode). An optional line-based command set covers adapter-level settings (MAC, stats, version). |
| `board/` | Clocks, pin mux, RMII REF_CLK direction, MAC address from EEPROM/OTP. |

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
