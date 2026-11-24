# Native Midea AC library (C++17, Win32)

A C++ port of the parts of [msmart-ng](https://github.com/mill1000/midea-msmart) this AC needs, written so
it can drop into the open-battery taskbar app: plain Win32, no third-party code. It uses Windows CNG
(`bcrypt`) for AES/SHA-256/MD5 and Winsock for networking. Calls are blocking, so run them on the app's
worker thread, never the UI thread.

| File | What it does |
|---|---|
| `src/midea/crypto.*` | AES-ECB/CBC, SHA-256, MD5, random bytes (CNG) |
| `src/midea/lan.*` | V3 "8370" session: handshake with token/key, encrypted packets, V2 "5A5A" packets, retries, 12 h re-auth |
| `src/midea/discover.*` | UDP broadcast discovery, reply decoding |
| `src/midea/ac_protocol.*` | AC frames: every command, CRC-8, checksums, state/capabilities/properties/group parsers |
| `src/midea/ac_device.*` | `Device`: capabilities, state, `Refresh`, `RefreshState`, `Apply`, property writes |
| `src/midea/ac_controller.*` | `Controller`: what the app calls. Connect, poll, validated `Set`, rediscovery by device ID, JSON snapshot |
| `src/midea/creds.*` | reads/writes `ac_creds.json` |
| `tools/acctl.cpp` | CLI: `discover`, `query`, `set k=v ...`, `selftest`, `parse` |

## Build and test

```
native\build.cmd                                   (VS 2022 C++ workload; /W4 /WX clean)
native\build\acctl.exe query
native\build\acctl.exe set power=true target_temperature=24 mode=cool
.venv\Scripts\python.exe tools\gen_vectors.py      golden vectors from msmart-ng + frames from the AC
native\build\acctl.exe selftest native\tests\vectors.txt
.venv\Scripts\python.exe tools\test_native.py      live tests against the AC, cross-checked with Python
```

Results on 2026-11-23:
- **Golden vectors:** **1114 / 1114** pass. They cover:
  - CRC, AES, signatures and V2/V3 packets
  - every command, including 60 random set-state frames
  - all 20 real response frames and 710 mutated ones
  - a real discovery reply
- **Live tests:** all pass. They cover:
  - matching Python field by field
  - 8 writes with read-back and restore
  - 7 rejected inputs
  - power on, Turbo, and power off
  - recovery from a wrong IP (10 s)
  - an unreachable unit (8 s)

- **Long session (`acctl watch 24 2000`):** one `Controller`, 24 polls, 0 failures.
  - A change took 0.28 s.
  - After a dropped TCP session, the next poll reconnected by itself (2.2 s).
  - Pointed at a dead IP, that poll took 7.5 s, then recovered.
- **Side effects (`tools/side_effects.py`):** every field diffed for 11 controls, all restored. A rejected
  request stages nothing, and every reply matches a fresh read.

| Timing (C++) | |
|---|---|
| Connect + handshake + capabilities | 1.3 s (1 s of it is the device's required pause after the handshake) |
| Full refresh (state, 6 data groups, properties) | 1.1 s |
| A change (light read + write) | about 0.3 s, 0.4 s with a property write |
| Discovery | 3 s |

## Differences from msmart-ng

- When an encrypted reply needs no padding, msmart slices `payload[2:-0]` to nothing. The port decodes it
  correctly, and the vector generator asserts this case. It can't happen with this AC: a V2 packet is
  always 56 + 16k bytes, so the V3 pad is always 6. The difference is theoretical.
- A refresh stops at the first command that gets no answer, rather than trying each remaining command. msmart
  carries on, so a dead address costs 30 s or more per poll there.
- Only what this AC uses is ported: no cloud login (the one-time key fetch stays in `tools/get_key.py`), no
  V1/V2-only devices, no commercial (0xCC) units.

## Compilers

Only built and tested with MSVC (VS 2022). The code avoids MSVC-only library calls: it uses `_wfopen`
rather than wide-path fstreams, and includes the C headers it needs. open-battery's MinGW build would also
have to link `-lbcrypt -lws2_32`, because `#pragma comment(lib)` is MSVC-only. It hasn't been compiled with
MinGW yet; this PC has no g++.

## Using it from the taskbar app

The library has no thread or timer of its own; the app provides them.

```cpp
// Worker thread: owns the Controller, never touched by the UI thread.
midea::ac::Controller ac(L"...\\ac_creds.json");
ac.Connect(&err);
loop {
    ac.Poll(&err);                 // app's own cadence: e.g. 10 s, 2 s for a short while after a change
    post a *copy* of ac.state() / ac.capabilities() to the hidden window (WM_RESULTS pattern)
    wait for the interval, or for a queued change from the UI:
        ac.Set({{"power", "true"}, {"target_temperature", "24"}}, &err);
}
```

`state()` returns a reference to data the worker owns: copy it before posting it to the UI. Still to build
in the app: the worker thread, its poll loop and backoff, the change queue, and the mapping to
`WidgetItem` / `flyout::Model`.
