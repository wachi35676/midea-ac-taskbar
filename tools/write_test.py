"""Write coverage test with the AC OFF: set each control, read it back, restore it.

Never powers the unit on and never starts self-clean.
"""
import asyncio
import json
import pathlib
import time

from msmart.device import AirConditioner as AC

ROOT = pathlib.Path(__file__).resolve().parent.parent
CREDS = json.loads((ROOT / "ac_creds.json").read_text())

M, F, S, A = AC.OperationalMode, AC.FanSpeed, AC.SwingMode, AC.SwingAngle

# (attribute, test value)
TESTS = [
    ("operational_mode", M.COOL),
    ("operational_mode", M.DRY),
    ("operational_mode", M.FAN_ONLY),
    ("operational_mode", M.AUTO),
    ("fan_speed", F.LOW),
    ("fan_speed", F.SILENT),
    ("fan_speed", F.MAX),
    ("fan_speed", 55),                       # custom %
    ("swing_mode", S.OFF),
    ("swing_mode", S.BOTH),
    ("swing_mode", S.HORIZONTAL),
    ("vertical_swing_angle", A.POS_3),
    ("horizontal_swing_angle", A.POS_3),
    ("breeze_away", True),
    ("eco", True),
    ("turbo", True),
    ("purifier", True),
    ("sleep", True),
    ("freeze_protection", True),
    ("beep", False),
    ("fahrenheit", True),
]


async def main() -> None:
    dev = AC(ip=CREDS["ip"], port=CREDS["port"], device_id=CREDS["id"])
    await dev.authenticate(CREDS["token"], CREDS["key"])
    await dev.get_capabilities()

    t = time.perf_counter()
    await dev.refresh()
    print(f"refresh round trip: {time.perf_counter() - t:.2f}s")
    assert dev.power_state is False, "AC is on; this test only runs with it off"

    for attr, value in TESTS:
        await dev.refresh()
        original = getattr(dev, attr)
        try:
            setattr(dev, attr, value)
            t = time.perf_counter()
            await dev.apply()
            dt = time.perf_counter() - t
            await dev.refresh()
            got = getattr(dev, attr)
            ok = "OK " if got == value else "NO "
            print(f"{ok} {attr:24} {original!s:28} -> set {value!s:28} read {got!s:28} ({dt:.2f}s) power={dev.power_state}")
        except Exception as e:
            print(f"ERR {attr:24} set {value!s}: {e!r}")
        finally:
            setattr(dev, attr, original)
            await dev.apply()

    await dev.refresh()
    print("\nfinal:", {k: dev.to_dict()[k] for k in
          ("power", "mode", "target_temperature", "fan_speed", "swing_mode", "eco", "turbo",
           "purifier", "sleep", "breeze_away", "fahrenheit", "vertical_swing_angle",
           "horizontal_swing_angle", "freeze_protection")})


asyncio.run(main())
