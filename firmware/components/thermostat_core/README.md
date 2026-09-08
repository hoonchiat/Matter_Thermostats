# thermostat_core

The HVAC control law as a **pure state machine** — no I/O, no RTOS, no ESP dependency. It
takes a snapshot of inputs plus the previous state and returns the desired W/Y/G/O·B
output state. This isolates the safety-relevant decisions and makes them unit-testable on
a host PC.

## Guarantees

- **Fail-safe:** `fault` or `MODE_OFF` → all outputs off immediately.
- **Never both:** heat and cool are mutually exclusive.
- **Hysteresis:** per-mode deadband prevents chatter around the setpoint.
- **Cycle protection:** `min_off` (anti short-cycle), `min_on`, and `startup_lockout` gate
  compressor calls. `min_off` applies only *after* an observed off-transition — the
  post-boot window is governed by `startup_lockout`, not a phantom off event.
- **Auto dead-zone:** enforces a minimum gap between heat and cool setpoints.

## Test

```bash
cd firmware/test/host && make
```

See [../../../docs/FIRMWARE.md](../../../docs/FIRMWARE.md#control-algorithm) for the full
algorithm and state table.
