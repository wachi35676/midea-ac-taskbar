# Midea AC control from the PC: research notes

Date: 2026-11-23

## How others do it

Midea ACs (and rebadged ones: Comfee, Toshiba-Midea, Inventor, Pioneer, Carrier some models) speak a local LAN protocol on **TCP 6444**, plus a UDP discovery broadcast on 6445/20086. People reverse-engineered it, and almost every project builds on the same Python code:

| Project | What it is |
|---|---|
| [mill1000/midea-msmart](https://github.com/mill1000/midea-msmart) (PyPI `msmart-ng`) | Maintained core library + CLI (`discover`, `query`, `control`). Async, few dependencies. Best base for a tray app. |
| [mill1000/midea-ac-py](https://github.com/mill1000/midea-ac-py) | Home Assistant integration on top of msmart-ng. |
| [georgezhao2010/midea_ac_lan](https://github.com/georgezhao2010/midea_ac_lan) / [midea-lan/midea-local](https://github.com/midea-lan/midea-local) | Separate implementation that covers many Midea appliance types, with its own cloud login code. |
| [ashevchuk/midea-ac](https://github.com/ashevchuk/midea-ac), `coolth` (PyPI) | Smaller alternatives. |

I found no ready-made Windows tray app, so we'd build one on top of msmart-ng.

### How the protocol works

1. **Discovery:** the PC broadcasts and the AC replies with its ID, serial, SSID and protocol version.
2. **V3 devices (ours) need a token and key.** These are fetched **once** from Midea's cloud, using the device ID. After that, control is fully local and the cloud isn't needed.
3. **Commands:** encrypted frames over TCP 6444: power, mode, target temperature, fan speed, swing, eco/turbo, display, and reading indoor/outdoor temperature.

## What I found on this network

- **AC found on the LAN, TCP port 6444** (IP, device ID and serial are in `ac_creds.json`, kept out of the docs)
- Type `0xAC` (air conditioner), **protocol V3**, so it needs a token and key

## The blocker: getting the token and key

| Cloud | Result |
|---|---|
| NetHome Plus, shared US account | Locked: "too many failed login attempts" (everyone using the library shares this account) |
| NetHome Plus, shared DE / KR accounts | Logged in, but token lookup gave `9999 system error` |
| SmartHome (MSmartHome), shared accounts | `3201 You have no permissions`, meaning the device is known there but bound to *your* account. Community reports say Midea shut down this token service, so this route may be dead anyway. |

**Ways forward (in order):**

1. **Use your own app account.** Run the key script with your Midea app login (whichever app you use: MSmartHome, NetHome Plus or Midea Air). You type the password yourself; it stays on your PC.
2. Retry the shared US account at a quiet time (it's locked intermittently).
3. Pull the token from the phone app's logs (Android, or an emulator), as described in the midea-ac-py README.

## Result: local control works

**What worked:** some Midea app accounts are created with Google sign-in and have no password. Give the account one ("Forgot password" on the app's login screen), then run `tools/get_key.py` with that email and password. For the unit tested here it got the token and key from **SmartHomeCloud, big-endian udpid** (little-endian was rejected by the AC). They're saved in `ac_creds.json` (git-ignored). Everything after this is local, with no cloud.

**Write test:** with the AC off, set the target 30 → 29 → 30 °C, read back correctly each time, power stayed off.

### What the AC supports

The full measured list of readings and controls is in **[docs/AC_API.md](docs/AC_API.md)**. This replaces an
earlier summary here that was wrong on three points:
- display on/off control is *not* advertised;
- there's no filter reminder;
- energy data *does* come back from data group 4, though its units are unverified.

## Files

- `tools/get_key.py`: one-time key fetch (asks for login, saves only token/key)
- `tools/query.py`: print capabilities and current state
- `tools/control_test.py`: harmless write test (temperature nudge with the AC off)
- `tools/probe.py`: deep read-only probe (raw capabilities, all data groups, every property) to `probe_output.txt`
- `tools/write_test.py`: write/read-back/restore test of every control, AC off only
- `tools/restore.py`: puts back the pre-test settings

