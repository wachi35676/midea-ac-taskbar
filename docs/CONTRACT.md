# Backend ↔ taskbar app contract

**Update 2026-11-23: the protocol is now native C++** (`native/`, see `native/README.md`). The taskbar app
links it in directly: a worker thread owns a `midea::ac::Controller`, calls `Poll()` and `Set()`, and posts
results to the hidden window (open-battery's `WM_RESULTS` pattern). The UI thread never waits on the AC.
No second process and no network port.

The key names, values, snapshot and capabilities below are unchanged. `Controller::Set` takes the same
`key=value` changes, and `SnapshotJson()` / `CapabilitiesJson()` produce the JSON shown here. The
Python stdio backend (`backend/acd.py`) stays as the reference implementation and test oracle.

Snapshot changes in the native version:
- New top-level `power_watts` (data group 7): live power draw. This is the figure to show.
- `vertical_angle` / `horizontal_angle` accept `"release"` to clear a fixed louver angle.
- `diagnostics.compressor_voltage` is renamed `mains_voltage` (it reads 230–232 V while running).
- `energy.total` is renamed `total_raw` and must not be shown, because its top byte jumps between reads.
- `diagnostics` gains the louver angles.

## Measured behaviour (`tools/test_backend.py`, all pass)

| Case | Time |
|---|---|
| Start to first `state` (includes Python start-up) | 2.5 s |
| `set` (light read, write, use the AC's reply) | 0.4 s, up to 1.2 s if it waits behind a poll |
| Wrong IP: rediscovered by device ID, creds updated | 10.5 s |
| AC unreachable: `offline` event, process stays up, writes fail cleanly | 13.4 s, then backoff up to 60 s |

Polling: every 10 s by default, every 2 s for 10 s after a write. The UI should show a change straight away
(optimistically) and correct itself from the reply.

## Messages from the app (stdin)

```json
{"id": 1, "cmd": "get"}                       // last snapshot, no AC traffic
{"id": 2, "cmd": "refresh"}                   // read the AC now
{"id": 3, "cmd": "set", "changes": {"power": true, "target_temperature": 24}}
{"id": 4, "cmd": "set_interval", "seconds": 15}
{"cmd": "quit"}
```

`set` accepts any subset of these keys. They are applied together in one write, after a fresh read, so
changes made from the remote or phone aren't overwritten.

| Key | Type / values |
|---|---|
| `power` | bool |
| `mode` | `"auto" "cool" "dry" "heat" "fan"` |
| `target_temperature` | number, `min_temperature`–`max_temperature`, 0.5 steps |
| `fan_speed` | `"silent" "low" "medium" "high" "max" "auto"` or int 1–100 |
| `swing` | `"off" "vertical" "horizontal" "both"` |
| `vertical_angle`, `horizontal_angle` | `1 25 50 75 100` (fixes the louver; pick a swing mode to release it) |
| `eco`, `turbo`, `sleep` | bool (only take effect while running) |
| `purifier`, `freeze_protection`, `breeze_away`, `fahrenheit`, `beep` | bool |
| `display` | `"toggle"` (unit doesn't advertise this; may do nothing) |
| `self_clean` | `"start"` (long cycle; the UI should confirm first) |

## Messages from the backend (stdout)

```json
{"id": 3, "ok": true, "state": { ...snapshot... }}
{"id": 3, "ok": false, "error": "target_temperature 35 is outside 16–30"}
{"event": "hello", "version": 1, "capabilities": { ... }}
{"event": "state", "state": { ...snapshot... }}          // after every poll and write
{"event": "offline", "error": "...", "retry_in": 10}     // AC unreachable; the backend keeps retrying
```

### Snapshot

```json
{
  "online": true, "updated_at": "2026-11-23T14:05:00Z", "ip": "192.168.1.50",
  "power": false, "mode": "heat", "target_temperature": 30.0, "indoor_temperature": 21.7,
  "outdoor_temperature": null, "fan_speed": "auto", "fan_percent": 102, "swing": "vertical",
  "vertical_angle": 0, "horizontal_angle": 0,
  "eco": false, "turbo": false, "sleep": false, "purifier": false, "freeze_protection": false,
  "breeze_away": false, "fahrenheit": false, "display_on": true, "self_clean_active": false,
  "error_code": 0, "defrost": false,
  "diagnostics": {"indoor_coil": 22.0, "outdoor_coil": 22.5, "compressor_hz": 0, "compressor_target_hz": 0,
                  "compressor_current": 1, "compressor_voltage": 0, "indoor_fan": 0, "outdoor_fan": 0,
                  "discharge_pipe": 0, "outdoor_unit_power": 2},
  "energy": {"total": 65.18, "current": 92.0, "realtime": 0.0, "verified_units": false}
}
```

`capabilities`: `modes`, `fan_speeds`, `swing_modes`, `min_temperature`, `max_temperature`,
`custom_fan_speed`, `vertical_angle`, `horizontal_angle`, and the `eco/turbo/purifier/breeze_away/
freeze_protection/self_clean/display_control/humidity/energy` flags.

## What the strip and the panel show

**Taskbar strip** (open-battery `WidgetItem`): one item.

| Situation | Icon | Text |
|---|---|---|
| Running | mode glyph (snowflake / sun / drop / fan / auto) | `24°` (target) |
| Off | power glyph, `dim = true` | room temperature, `21.7°` |
| Offline | dim, warning glyph | `--` |
| `error_code` ≠ 0 | `low = true` (red) | `E{code}` |

Tooltip: `Cooling to 24°, room 26.3°, fan Auto`.

**Click panel** (open-battery `flyout::Model`), from top to bottom:
1. **Header:** power button, mode, big target temperature with − / +, and room temperature.
2. **Mode row:** Auto, Cool, Dry, Heat, Fan (only the modes in `capabilities.modes`).
3. **Fan row:** presets plus a custom slider when `custom_fan_speed`.
4. **Swing:** Off / Vertical / Horizontal / Both, and the fixed angle positions.
5. **Toggles** (two per row, the existing `flyout::Toggle`): Eco, Turbo, Sleep, Purifier, Breeze away, Freeze protection, Beep.
6. **Details** (collapsed): coil temperatures, compressor, fans, energy (marked unverified), IP and last update.
7. **Actions:** Refresh, Self-clean (with confirm), Settings, Exit.

Every control is disabled with a short reason while the AC is offline. Eco, Turbo and Sleep are disabled
while it's off.
