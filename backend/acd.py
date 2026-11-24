"""AC backend daemon: JSON lines over stdin/stdout. See docs/CONTRACT.md.

Usage:  python acd.py [--creds PATH] [--interval SECONDS]
stdout carries protocol messages only; logs go to stderr.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import pathlib
import sys
import threading
import time
from datetime import datetime, timezone
from typing import Any, Optional

from msmart.device import AirConditioner as AC
from msmart.discover import Discover

PROTOCOL_VERSION = 1
FAST_POLL_SECONDS = 2.0      # right after a write, so the panel shows the unit settling
FAST_POLL_WINDOW = 10.0
MAX_BACKOFF = 60.0

log = logging.getLogger("acd")

Mode, Fan, Swing, Angle = AC.OperationalMode, AC.FanSpeed, AC.SwingMode, AC.SwingAngle
MODES = {"auto": Mode.AUTO, "cool": Mode.COOL, "dry": Mode.DRY, "heat": Mode.HEAT, "fan": Mode.FAN_ONLY}
FANS = {"silent": Fan.SILENT, "low": Fan.LOW, "medium": Fan.MEDIUM, "high": Fan.HIGH, "max": Fan.MAX,
        "auto": Fan.AUTO}
SWINGS = {"off": Swing.OFF, "vertical": Swing.VERTICAL, "horizontal": Swing.HORIZONTAL, "both": Swing.BOTH}
ANGLES = {1: Angle.POS_1, 25: Angle.POS_2, 50: Angle.POS_3, 75: Angle.POS_4, 100: Angle.POS_5}
BOOL_KEYS = {"power": "power_state", "eco": "eco", "turbo": "turbo", "sleep": "sleep", "purifier": "purifier",
             "freeze_protection": "freeze_protection", "breeze_away": "breeze_away",
             "fahrenheit": "fahrenheit", "beep": "beep"}


def _name(table: dict, value: Any) -> Any:
    for k, v in table.items():
        if v == value:
            return k
    return value if value is None else int(value)


class CommandError(Exception):
    pass


class Backend:
    def __init__(self, creds_path: pathlib.Path, interval: float) -> None:
        self.creds_path = creds_path
        self.creds = json.loads(creds_path.read_text())
        self.interval = interval
        self.dev: Optional[AC] = None
        self.snapshot: dict[str, Any] = {"online": False}
        self.lock = asyncio.Lock()          # one AC conversation at a time
        self.wake = asyncio.Event()
        self.fast_until = 0.0
        self.failures = 0
        self.out_lock = threading.Lock()

    # ---------- output ----------
    def send(self, msg: dict) -> None:
        line = json.dumps(msg, separators=(",", ":"), default=str)
        with self.out_lock:
            sys.stdout.write(line + "\n")
            sys.stdout.flush()

    # ---------- connection ----------
    async def connect(self) -> None:
        dev = AC(ip=self.creds["ip"], port=self.creds["port"], device_id=self.creds["id"])
        await dev.authenticate(self.creds["token"], self.creds["key"])
        dev.enable_energy_usage_requests = True
        for g in (1, 5, 11):                 # groups that carried data on this unit
            setattr(dev, f"enable_group{g}_data_requests", True)
        await dev.get_capabilities()
        self.dev = dev
        log.info("connected to %s", self.creds["ip"])

    async def rediscover(self) -> bool:
        """The IP is DHCP-assigned: find the unit again by its ID and save the new address."""
        log.info("rediscovering device %s", self.creds["id"])
        try:
            found = await Discover.discover(auto_connect=False, timeout=3)
        except Exception as e:
            log.warning("discovery failed: %r", e)
            return False
        for d in found or []:
            if d.id == self.creds["id"]:
                if d.ip != self.creds["ip"]:
                    log.info("device moved %s -> %s", self.creds["ip"], d.ip)
                    self.creds["ip"], self.creds["port"] = d.ip, d.port
                    self.creds_path.write_text(json.dumps(self.creds, indent=2))
                return True
        return False

    def _set_groups(self, on: bool) -> None:
        assert self.dev
        self.dev.enable_energy_usage_requests = on
        for g in (1, 5, 11):
            setattr(self.dev, f"enable_group{g}_data_requests", on)

    async def read(self, light: bool = False) -> None:
        """Refresh state; reconnects or rediscovers when the unit doesn't answer.

        light=True skips the diagnostic/energy groups (used right before a write)."""
        for attempt in range(2):
            try:
                if self.dev is None:
                    await self.connect()
                assert self.dev
                if light:
                    self._set_groups(False)
                try:
                    await self.dev.refresh()
                finally:
                    if light and self.dev:
                        self._set_groups(True)
                if not self.dev.online:
                    raise ConnectionError("no response from the AC")
                self.failures = 0
                self.snapshot = self.build_snapshot()
                return
            except Exception as e:
                log.warning("read failed (attempt %d): %r", attempt + 1, e)
                self.dev = None
                if attempt == 0:
                    await self.rediscover()
        self.failures += 1
        self.snapshot = {**self.snapshot, "online": False}
        raise ConnectionError("AC unreachable")

    # ---------- snapshot ----------
    def capabilities(self) -> dict[str, Any]:
        d = self.dev
        if d is None:
            return {}
        return {
            "modes": [_name(MODES, m) for m in d.supported_operation_modes],
            "fan_speeds": [_name(FANS, f) for f in d.supported_fan_speeds],
            "swing_modes": [_name(SWINGS, s) for s in d.supported_swing_modes],
            "min_temperature": d.min_target_temperature, "max_temperature": d.max_target_temperature,
            "custom_fan_speed": d.supports_custom_fan_speed,
            "vertical_angle": d.supports_vertical_swing_angle,
            "horizontal_angle": d.supports_horizontal_swing_angle,
            "eco": d.supports_eco, "turbo": d.supports_turbo, "purifier": d.supports_purifier,
            "breeze_away": d.supports_breeze_away, "freeze_protection": d.supports_freeze_protection,
            "self_clean": d.supports_self_clean, "display_control": d.supports_display_control,
            "humidity": d.supports_humidity, "energy": True,
        }

    def build_snapshot(self) -> dict[str, Any]:
        d = self.dev
        assert d
        fan = d.fan_speed
        return {
            "online": True,
            "updated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "ip": d.ip,
            "power": d.power_state, "mode": _name(MODES, d.operational_mode),
            "target_temperature": d.target_temperature, "indoor_temperature": d.indoor_temperature,
            "outdoor_temperature": d.outdoor_temperature,
            "fan_speed": _name(FANS, fan) if fan in FANS.values() else "custom", "fan_percent": int(fan),
            "swing": _name(SWINGS, d.swing_mode),
            "vertical_angle": int(d.vertical_swing_angle), "horizontal_angle": int(d.horizontal_swing_angle),
            "eco": d.eco, "turbo": d.turbo, "sleep": d.sleep, "purifier": d.purifier,
            "freeze_protection": d.freeze_protection, "breeze_away": d.breeze_away,
            "fahrenheit": d.fahrenheit, "display_on": d.display_on, "self_clean_active": d.self_clean_active,
            "error_code": d.error_code, "defrost": d.defrost_active,
            "diagnostics": {
                "indoor_coil": d.indoor_coil_temperature, "outdoor_coil": d.outdoor_coil_temperature,
                "compressor_hz": d.compressor_frequency, "compressor_target_hz": d.target_compressor_frequency,
                "compressor_current": d.compressor_current, "compressor_voltage": d.compressor_voltage,
                "indoor_fan": d.indoor_fan_speed, "outdoor_fan": d.outdoor_fan_speed,
                "discharge_pipe": d.discharge_pipe_temperature, "outdoor_unit_power": d.outdoor_unit_power,
            },
            "energy": {"total": d.total_energy_usage, "current": d.current_energy_usage,
                       "realtime": d.real_time_power_usage, "verified_units": False},
        }

    # ---------- writes ----------
    def apply_changes(self, changes: dict[str, Any]) -> list[str]:
        """Validate and stage changes on self.dev. Returns the special actions to run after apply()."""
        d = self.dev
        assert d
        actions: list[str] = []
        for key, value in changes.items():
            if key in BOOL_KEYS:
                if not isinstance(value, bool):
                    raise CommandError(f"{key} must be true or false")
                setattr(d, BOOL_KEYS[key], value)
            elif key == "mode":
                if value not in MODES or MODES[value] not in d.supported_operation_modes:
                    raise CommandError(f"mode {value!r} is not supported")
                d.operational_mode = MODES[value]
            elif key == "target_temperature":
                t = float(value)
                if not d.min_target_temperature <= t <= d.max_target_temperature or (t * 2) % 1:
                    raise CommandError(f"target_temperature {value} must be "
                                       f"{d.min_target_temperature:g}-{d.max_target_temperature:g} in 0.5 steps")
                d.target_temperature = t
            elif key == "fan_speed":
                if isinstance(value, str) and value in FANS:
                    d.fan_speed = FANS[value]
                elif isinstance(value, int) and not isinstance(value, bool) and 1 <= value <= 100 \
                        and d.supports_custom_fan_speed:
                    d.fan_speed = value
                else:
                    raise CommandError(f"fan_speed {value!r} is not supported")
            elif key == "swing":
                if value not in SWINGS or SWINGS[value] not in d.supported_swing_modes:
                    raise CommandError(f"swing {value!r} is not supported")
                d.swing_mode = SWINGS[value]
            elif key in ("vertical_angle", "horizontal_angle"):
                if value not in ANGLES:
                    raise CommandError(f"{key} must be one of {sorted(ANGLES)}; choose a swing mode to release it")
                setattr(d, key.replace("_angle", "_swing_angle"), ANGLES[value])
            elif key == "display" and value == "toggle":
                actions.append("display")
            elif key == "self_clean" and value == "start":
                actions.append("self_clean")
            else:
                raise CommandError(f"unknown setting {key!r}")
        return actions

    async def set(self, changes: dict[str, Any]) -> None:
        if not isinstance(changes, dict) or not changes:
            raise CommandError("changes must be a non-empty object")
        async with self.lock:
            await self.read(light=True)     # fresh state first: apply() sends the whole frame
            actions = self.apply_changes(changes)
            assert self.dev
            await self.dev.apply()          # the AC answers with its new state, which apply() parses
            if "display" in actions:
                await self.dev.toggle_display()
            if "self_clean" in actions:
                await self.dev.start_self_clean()
            self.snapshot = {**self.build_snapshot(), "diagnostics": self.snapshot.get("diagnostics"),
                             "energy": self.snapshot.get("energy")}
        self.fast_until = time.monotonic() + FAST_POLL_WINDOW
        self.wake.set()

    # ---------- loops ----------
    async def poll_loop(self) -> None:
        first = True
        while True:
            try:
                async with self.lock:
                    await self.read()
                if first:
                    self.send({"event": "hello", "version": PROTOCOL_VERSION, "capabilities": self.capabilities()})
                    first = False
                self.send({"event": "state", "state": self.snapshot})
                delay = FAST_POLL_SECONDS if time.monotonic() < self.fast_until else self.interval
            except Exception as e:
                delay = min(self.interval * (2 ** (self.failures - 1)), MAX_BACKOFF)
                if first:
                    self.send({"event": "hello", "version": PROTOCOL_VERSION, "capabilities": {}})
                    first = False
                self.send({"event": "offline", "error": str(e), "retry_in": delay})
            self.wake.clear()
            try:
                await asyncio.wait_for(self.wake.wait(), timeout=delay)
            except asyncio.TimeoutError:
                pass

    async def handle(self, msg: dict) -> bool:
        mid, cmd = msg.get("id"), msg.get("cmd")
        try:
            if cmd == "quit":
                return False
            if cmd == "get":
                pass
            elif cmd == "refresh":
                async with self.lock:
                    await self.read()
                self.send({"event": "state", "state": self.snapshot})
            elif cmd == "set":
                await self.set(msg.get("changes"))
            elif cmd == "set_interval":
                self.interval = max(2.0, float(msg["seconds"]))
                self.wake.set()
            else:
                raise CommandError(f"unknown cmd {cmd!r}")
            self.send({"id": mid, "ok": True, "state": self.snapshot})
        except Exception as e:
            log.warning("cmd %s failed: %r", cmd, e)
            self.send({"id": mid, "ok": False, "error": str(e), "state": self.snapshot})
        return True


def stdin_reader(loop: asyncio.AbstractEventLoop, queue: asyncio.Queue) -> None:
    """Blocking stdin reads on a thread (asyncio can't read pipes on Windows' default loop)."""
    for line in sys.stdin:
        loop.call_soon_threadsafe(queue.put_nowait, line)
    loop.call_soon_threadsafe(queue.put_nowait, None)   # EOF: the app went away


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--creds", type=pathlib.Path,
                    default=pathlib.Path(__file__).resolve().parent.parent / "ac_creds.json")
    ap.add_argument("--interval", type=float, default=10.0)
    args = ap.parse_args()

    backend = Backend(args.creds, args.interval)
    queue: asyncio.Queue = asyncio.Queue()
    threading.Thread(target=stdin_reader, args=(asyncio.get_running_loop(), queue), daemon=True).start()
    poller = asyncio.create_task(backend.poll_loop())

    while True:
        line = await queue.get()
        if line is None:
            break
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
            if not isinstance(msg, dict):
                raise ValueError("not an object")
        except ValueError as e:
            backend.send({"ok": False, "error": f"bad JSON: {e}"})
            continue
        if not await backend.handle(msg):
            break
    poller.cancel()


if __name__ == "__main__":
    logging.basicConfig(stream=sys.stderr, level=logging.INFO, format="%(asctime)s %(name)s %(message)s")
    for noisy in ("msmart", "httpx"):
        logging.getLogger(noisy).setLevel(logging.ERROR)
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    asyncio.run(main())
