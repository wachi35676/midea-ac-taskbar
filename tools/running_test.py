"""Running test (user approved 2026-11-23): power on, try eco/turbo/sleep, read running values, power off.

Restores every setting it touches and always tries to switch the unit off at the end.
"""
import asyncio
import json
import pathlib
import time

from msmart.device import AirConditioner as AC

ROOT = pathlib.Path(__file__).resolve().parent.parent
CREDS = json.loads((ROOT / "ac_creds.json").read_text())
KEYS = ("power", "mode", "target_temperature", "indoor_temperature", "outdoor_temperature", "fan_speed",
        "eco", "turbo", "sleep", "compressor_frequency", "target_compressor_frequency", "compressor_current",
        "compressor_voltage", "indoor_fan_speed", "outdoor_fan_speed", "indoor_coil_temperature",
        "outdoor_coil_temperature", "discharge_pipe_temperature", "outdoor_unit_power", "real_time_power_usage",
        "current_energy_usage", "total_energy_usage", "defrost", "error_code")


async def snap(dev, label):
    await dev.refresh()
    d = dev.to_dict()
    print(f"[{label}]", {k: d.get(k) for k in KEYS})
    print(f"   energy BCD/binary realtime: {dev.get_real_time_power_usage(AC.EnergyDataFormat.BCD)} / "
          f"{dev.get_real_time_power_usage(AC.EnergyDataFormat.BINARY)}")


async def try_flag(dev, attr):
    await dev.refresh()
    setattr(dev, attr, True)
    await dev.apply()
    await dev.refresh()
    got = getattr(dev, attr)
    print(f"{'OK ' if got else 'NO '} {attr} on -> read {got}")
    setattr(dev, attr, False)
    await dev.apply()
    await dev.refresh()
    print(f"     {attr} off -> read {getattr(dev, attr)}")


async def main() -> None:
    dev = AC(ip=CREDS["ip"], port=CREDS["port"], device_id=CREDS["id"])
    await dev.authenticate(CREDS["token"], CREDS["key"])
    await dev.get_capabilities()
    dev.enable_energy_usage_requests = True
    for g in (1, 5, 11):
        setattr(dev, f"enable_group{g}_data_requests", True)

    await snap(dev, "before")
    orig_mode, orig_target = dev.operational_mode, dev.target_temperature
    try:
        dev.power_state = True
        t = time.perf_counter()
        await dev.apply()
        print(f"power on sent ({time.perf_counter() - t:.2f}s)")
        await snap(dev, "on +0s")

        await try_flag(dev, "turbo")
        await try_flag(dev, "sleep")

        # Eco is normally a Cool-mode feature; try it in Cool briefly.
        await dev.refresh()
        dev.operational_mode = AC.OperationalMode.COOL
        dev.target_temperature = 26
        await dev.apply()
        await try_flag(dev, "eco")
        await dev.refresh()
        dev.operational_mode, dev.target_temperature = orig_mode, orig_target
        await dev.apply()

        for s in (20, 45, 75):
            await asyncio.sleep(s - (0 if s == 20 else (20 if s == 45 else 45)))
            await snap(dev, f"on +{s}s")
    finally:
        await dev.refresh()
        dev.power_state = False
        dev.operational_mode, dev.target_temperature = orig_mode, orig_target
        await dev.apply()
        await snap(dev, "after (off)")


asyncio.run(main())
