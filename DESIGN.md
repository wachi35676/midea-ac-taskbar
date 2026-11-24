# AC Taskbar: design

A Windows 11 utility, not a remote: show what the AC is doing at a glance, and make every control one
click away without a wall of buttons.

## Surfaces

**Taskbar strip** (on every monitor's taskbar; right end by default, left of the notification area or clock, next to the notification area; the right-click menu moves it to
the left end. It steps past other apps' strips, such as open-battery's, on either side.)
- A small Fluent-3D style AC glyph (44x20, from the art module) plus two lines of text, in a soft pill.
- Running: the glyph's display and air wisps glow in the mode colour. The room temperature is big (`26.3°`), with `Heat → 30°` under it.
- Off: the glyph is still, its display dark. `24.6°` / `AC off`.
- Offline: the glyph is grey. `AC` / `Can't reach it`.
- The tooltip carries the long form (`Heating to 30°`, outside temperature, watts).

**Click panel**: fixed size, **392 × 776 DIPs** at 100 % (scaled down to fit a short work area). It never resizes or scrolls. From top to bottom:
1. Scene card (184 tall):
   - Fable's illustration: the unit is fixed at the top of a soft wall, and its air flows into the room below.
   - Readings as small chips along the bottom of the wall: Room, Outside, power use, and `Off in 1:24` while a sleep timer runs.
   - No status chip and no buttons on the scene.
2. Control bar:
   - A round power button (accent when on).
   - One wide pill: − / big target temperature with a "target temperature" caption / +.
3. Mode row: five icon tiles with labels.
4. Tabs: **Controls** · **Schedule** · **Details**, with fixed-height content (368).
   - Controls, one persistent page:
     - Fan speed: one rising five-bar ramp. A click picks a preset (20/40/60/80/100 %), a drag sets an exact speed. The value and an Auto button sit beside it.
     - Air direction: for each axis one track with Swing and five fixed positions drawn as louver diagrams, the chosen one lit.
     - Features: up to 4 × 2 tiles (Eco, Turbo, Sleep, Purifier, Breeze away, Freeze guard, Beep, °F). Unsupported ones aren't drawn; tiles that need the unit running are disabled with one caption.
   - Schedule:
     - A "Next: turns off at 23:00" line and a 24-hour bar of today's timers (filled dot = on, hollow = off, now as a hairline).
     - Sleep timer: Off, 30 m, 1 h, 2 h or 4 h. The label turns into the countdown and "Off" becomes "Cancel".
     - Any number of schedules, each **Turn on** or **Turn off** at a time, either **Once** on a date or
       **Repeat** on chosen weekdays, each with its own switch. Four two-line rows at a time (the wheel scrolls),
       in time order. Click a row to edit, × to delete, **+ Add** for a new one.
     - The editor replaces the list: action, time (−1 h, −15, +15, +1 h), Once with a date stepper or Repeat
       with weekday toggles, a plain-language summary, Delete, Cancel and Save. Save is disabled for a time that has already passed.
     - One-time schedules switch themselves off after running ("done"). One whose time passed while the PC
       was off shows "missed" and doesn't fire late.
     - Saved under `HKCU\Software\ACTaskbar` (value `Schedules`). The timers run in the app, so they need the PC on.
   - Details:
     - Power card: live watts, peak, and a 10-minute chart.
     - Three ring gauges: compressor Hz (with a dot at the target), current A, mains V.
     - Five temperature bars on one shared -10…80° axis (room, outside, coils, discharge pipe).
     - Chips for defrost, fan speeds and the last unit code; Refresh and Self-clean (inline confirm).
   - Unit codes are shown as errors only after they've stayed for 20 s. A code (25) appeared briefly while the AC was switched on and off, then cleared.

**Desktop widget**: the same panel as a persistent window on the desktop (default: the second screen's bottom right). Draggable by any empty area, position remembered, never takes focus. Settings: size 75–200 % (capped to the screen), background Clear glass / Frosted / Solid, keep on top, lock position, reset.

**Settings window** (right-click → Settings):
- Windows 11 Settings style: Mica, a "Settings" title, sections of 68-tall cards (icon, title, description, control at the right), segmented choices and switches in the accent.
- Panel: background, **Mica / Acrylic / Clear glass** (default Clear glass); desktop widget options.
- Taskbar: position (Left / Right); monitors (All / Main only).
- General: Start with Windows; refresh every 5 / 10 / 30 / 60 s.
- Air conditioner: address and status, with Refresh now; Exit app.
- Everything applies at once and is saved under `HKCU\Software\ACTaskbar`.

**Backdrop:** with a backdrop, the panel paints on a transparent target and uses see-through Windows fills (card 5 % white in dark, 62 % white in light; controls 6.5 % / 55 %). Before Windows 11 22H2 it falls back to the opaque colours below.

## Colour

Follows the Windows light/dark setting. Neutrals:

| Role | Dark | Light |
|---|---|---|
| Panel | `#202020` | `#F3F3F3` |
| Card | `#2B2B2B` | `#FFFFFF` |
| Control | `#363636` | `#E9E9E9` |
| Control hover | `#424242` | `#DDDDDD` |
| Text | `#FFFFFF` | `#1A1A1A` |
| Secondary text | `#B8B8B8` | `#616161` |

**Colour rule:**
- **Windows theme and accent:** everything you operate, meaning selected tiles (accent tint with an accent outline), active icons, fan bars, the slider, switches, the power button, chart and tabs.
- **AC mode colour** (`art::ModeColor`: cool blue, heat orange, dry teal, fan grey, auto green): only the AC itself, meaning the scene's air and display, the strip glyph and the status-chip dot.

## Type

Segoe UI Variable Text. Big set point 40 semibold, header 15 semibold, body 12.5, captions 11.5.

## Shape and spacing

- 16 padding.
- Radius: 8 for cards, 6 for controls, full for round buttons.
- 4 / 8 / 12 spacing steps.
- No shadows inside the panel; Windows draws the window's own rounded corner and shadow.

## Motion

- The scene is the one continuous animation: airflow, particles and fan spin, a pure function of time.
- State changes ease (exponential smoothing, τ ≈ 0.25 s; the power fade is 0.4 s; the mode crossfade 0.35 s).
  The louver moves like a real flap; swing is a slow sine (6 s period up/down, 7 s left/right).
- UI feedback: hover fades in 120 ms with ease-out, and tab content cross-fades in 150 ms. Temperature
  − / + and toggles change instantly (used often).
- Icons are still at rest. Only the hovered control's icon moves (fan spins, sun pulses, drop bobs).
- `prefers-reduced-motion` (Windows' "Animation effects" off): no ambient motion and no tweens; values snap.
- The strip animates at 30 fps only while the AC runs, and is fully still when off.
