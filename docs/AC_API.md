# AC API surface (measured on this unit)

Midea V3 split AC, device type `0xAC`, LAN protocol on TCP 6444, via `msmart-ng` 2026.11.0.
Device identity (IP, ID, token, key) lives in `ac_creds.json` (git-ignored), not in docs.

Measured on 2026-11-23, first with the unit off, then running (see the running test sections below).

## Transport and timing

| What | Measured |
|---|---|
| Discovery | UDP broadcast to 6445/20086; works without the cloud once the token/key are saved |
| Auth | Token/key from SmartHomeCloud, **big-endian** udpid; saved once, reused locally |
| Full refresh (state + properties) | **0.26 s** |
| Write (`apply`) | 0.14 s for state fields, 0.26 s when it also sends properties |
| Write semantics | `apply()` sends the **whole** state frame: always `refresh()` right before a write, or a change made from the remote/phone is overwritten |

## Readings

| Field | Source | Value at test | Notes |
|---|---|---|---|
| power | state | off | |
| mode | state | Heat (4) | Auto 1, Cool 2, Dry 3, Heat 4, Fan 5 |
| target_temperature | state | 30.0 | 16–30 °C, 0.5 steps |
| indoor_temperature | state | 21.7 | |
| outdoor_temperature | state | None off, 19.3 running | only reported while the outdoor unit runs |
| fan_speed | state | Auto (102) | Silent 20, Low 40, Med 60, High 80, Max 100, Auto 102, or custom 1–100 |
| swing_mode | state | Vertical (12) | Off 0, Horizontal 3, Vertical 12, Both 15 |
| vertical/horizontal_swing_angle | property 0x09/0x0A | 0 | 0 = none fixed (swinging or default); positions 1, 25, 50, 75, 100 |
| breeze_away | property 0x42 | false | |
| eco, turbo, sleep, purifier, freeze_protection, follow_me | state | false | |
| fahrenheit (display unit) | state | false | |
| display_on | state | true | read only; see controls |
| error_code | state | 0 | |
| self_clean_active | property 0x39 | false | |
| indoor_coil_temperature | group 1 | 22.0 °C | |
| outdoor_coil_temperature | group 1 | 22.5 °C | |
| compressor_frequency / target | group 1 | 0 / 0 | see the 6-minute run below |
| compressor_current / voltage | group 1 | 1 / 0 off, 1 / 230-232 running | the "voltage" is mains voltage |
| indoor / outdoor fan speed | groups 2 / 5 | 0 / 0 off, outdoor 808 running | |
| discharge_pipe_temperature | group 1 | 0 off, 20 running | °C |
| outdoor_unit_power | group 5 | 2 | unit unknown |
| defrost, water_pump_running | groups | false | |
| horizontal/vertical_louvers_angle | group 11 | 240 / 240 | 240 looks like "parked" |
| total_energy_usage | group 4 | BCD 65.18 → 5465.18 → 6466.18 | **unusable**: the top byte jumps between reads |
| current_energy_usage | group 4 | BCD 92.0, binary 3584 | unverified |
| real_time_power_usage | group 4 | 0.0, even at 2 kW | not usable: read outdoor_unit_power (group 7) instead |
| indoor_humidity | — | None | no humidity sensor |
| filter_alert | — | false | **not supported** (`supports_filter_reminder` false) |

Energy: the ENERGY capability reports `00`, yet group 4 answers with data. Treat the energy readings as
experimental until checked against the app.

Undocumented groups that answered with non-zero data: **3** (`…0103…00aa…`), **6** (`…014649a34101…`),
**12** (`…014c02000f…`). Groups 0, 2, 8, 9, 10, 13, 14, 15 answered all zeros.

## Controls (write, then read back)

| Control | Result |
|---|---|
| power | ✅ on and off (running test) |
| target_temperature | ✅ |
| mode (all 5) | ✅ |
| fan_speed presets + custom % | ✅ (restore with the `FanSpeed.AUTO` enum, not the int) |
| swing_mode (all 4) | ✅ |
| vertical / horizontal fixed angle | ✅ for positions; **writing 0 is rejected** (`0x11`). Setting a fixed angle stops swing on that axis; choosing a swing mode that includes the axis clears it |
| breeze_away | ✅ |
| purifier (ionizer, ANION) | ✅ |
| freeze_protection (8 °C heat) | ✅ |
| fahrenheit display | ✅ |
| beep on commands | ✅ (sent with each command) |
| eco | ✅ while running in Cool; doesn't stick while off |
| turbo | ✅ while running; doesn't stick while off |
| sleep | ✅ while running; doesn't stick while off |
| display toggle | `toggle_display()` exists, but `supports_display_control` is false: untested |
| self_clean | `start_self_clean()` exists; **not triggered** (starts a long cycle) |

Not supported by this unit: humidity / target humidity, breezeless, breeze mild, cascade, fresh air, iECO,
flash/jet cool, outdoor silent, aux heat, rate select, sound property, filter reminder.

## Running test (2026-11-23, user approved, `tools/running_test.py`)

- **Power on/off:** ✅ (0.15 s to send).
- **Turbo, Sleep:** ✅ on and off while running, in Heat.
- **Eco:** ✅ in Cool.
- **Outdoor temperature** now reports (19.3 °C). It reads `None` while the unit is off.
- **`compressor_voltage`** reads 230–232 while running: this is mains voltage.
- **Outdoor fan** reached 808 shortly after start. The compressor stayed at 0 Hz for the 75 s test, which is
  the normal start-up delay for these units.
- **Energy:**
  - `total_energy` jumped 65.18 → 5465.18 → 6466.18 between reads, so its top byte isn't usable. Don't show it.
  - `current_energy` stayed at 92.0 and real-time power at 0.0.
  - The units are still unknown; compare with the MSmartHome app before showing either.

Everything is back as before: off, Heat, 30 °C, fan Auto, vertical swing.

## 6-minute heating run (2026-11-23, `tools/long_run.py`, full log in `docs/long_run_log.txt`)

The run used Heat at 30 °C. The room went 21.4 → 29.1 °C. The compressor stopped at about 4 min, which
fits the unit nearing its target.

| Reading | Behaviour |
|---|---|
| **group 7 `outdoor_unit_power`** | **Live power draw in watts.** 2 W idle → 100 → 425 → 856 → 1095 → 1411 → 1652 → **1964 W peak**, back to 2–3 W when the compressor stopped. This is the power reading to show; add it up over time for kWh. |
| compressor_frequency / target | 17 → 62 Hz actual, 97 Hz target, falling to 56 then 0 as the room warmed |
| compressor_current | 1 idle → 8 at peak (likely amps: 8 A × 233 V ≈ 1.9 kW, matching group 7) |
| compressor_voltage | 229–237, mains voltage |
| indoor coil / outdoor coil | 21 → 60 °C / 18.5 → 5.5 °C while heating |
| discharge pipe | 20 → 65 °C |
| indoor fan (group 2) | 744 → 1208 (internal units) |
| outdoor fan | 696–808 while running, 0 after the compressor stops |
| group 4 `current_energy` | Stepped 92 → 192 → 292 → 392 while running, then **reset to 92** when the compressor stopped. Not a usable counter. |
| group 4 `total_energy` | Top byte jumps. Not usable. |
| group 4 real-time power | Always 0. Use group 7 instead. |
| raw groups 3, 6 | Bytes change with the compressor (meaning unknown); logged raw. Group 12 stays all zeros. |

## Side effects of each control (`tools/side_effects.py`, unit off, every field diffed)

| Change | Also changes |
|---|---|
| mode, fan_speed (preset or %), swing, purifier, freeze_protection, fahrenheit, beep | nothing |
| vertical_angle = N | swing → **off** |
| horizontal_angle = N | nothing (vertical swing keeps going) |
| **breeze_away = true** | **fan → custom 1 %, swing → off** (the fan-1 seen in the first write test came from this) |
| `vertical_angle=release` / `horizontal_angle=release` (native controller) | clears the fixed angle by swinging through a mode that covers that axis, then back to the swing you chose |

The panel has to show these side effects. The native controller re-reads everything after a property
write, so its reply already includes them.
