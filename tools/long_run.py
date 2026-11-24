"""6-minute running log (user approved): power on in the current mode, log state and raw data groups every
20 s, then power off. Settles compressor readings and gives energy counters time to move."""
import asyncio
import json
import pathlib
import time

from msmart.device import AirConditioner as AC
from msmart.device.AC.command import GetGroupDataCommand

ROOT = pathlib.Path(__file__).resolve().parent.parent
CREDS = json.loads((ROOT / "ac_creds.json").read_text())
LOG = ROOT / "docs" / "long_run_log.txt"
DURATION, STEP = 360, 20


async def main() -> None:
    dev = AC(ip=CREDS["ip"], port=CREDS["port"], device_id=CREDS["id"])
    await dev.authenticate(CREDS["token"], CREDS["key"])
    await dev.get_capabilities()
    dev.enable_energy_usage_requests = True
    for g in (1, 2, 5, 7, 11):
        setattr(dev, f"enable_group{g}_data_requests", True)
    out = LOG.open("w", encoding="utf-8")

    def log(s: str) -> None:
        print(s, flush=True)
        out.write(s + "\n")
        out.flush()

    await dev.refresh()
    was_on = dev.power_state
    log(f"start: power={dev.power_state} mode={dev.operational_mode!r} target={dev.target_temperature} "
        f"room={dev.indoor_temperature}")
    t0 = time.monotonic()
    try:
        if not was_on:
            dev.power_state = True
            await dev.apply()
        while (t := time.monotonic() - t0) < DURATION:
            await dev.refresh()
            raw = {}
            for g in (3, 4, 6, 12):
                frames = await dev._send_command(GetGroupDataCommand(g))
                raw[g] = frames[0][10:-2].hex() if frames else None
            log(f"t={t:5.0f}s power={dev.power_state} room={dev.indoor_temperature} out={dev.outdoor_temperature} "
                f"comp={dev.compressor_frequency}/{dev.target_compressor_frequency}Hz cur={dev.compressor_current} "
                f"V={dev.compressor_voltage} coil_in={dev.indoor_coil_temperature} coil_out={dev.outdoor_coil_temperature} "
                f"disch={dev.discharge_pipe_temperature} fan_in={dev.indoor_fan_speed}/{dev.target_indoor_fan_speed} "
                f"fan_out={dev.outdoor_fan_speed} pwr7={dev.outdoor_unit_power} defrost={dev.defrost_active} "
                f"E_cur={dev.current_energy_usage} E_rt={dev.real_time_power_usage} E_tot={dev.total_energy_usage}")
            log(f"        raw g3={raw[3]} g4={raw[4]} g6={raw[6]} g12={raw[12]}")
            await asyncio.sleep(STEP)
    finally:
        await dev.refresh()
        dev.power_state = was_on
        await dev.apply()
        await dev.refresh()
        log(f"end: power={dev.power_state} mode={dev.operational_mode!r} target={dev.target_temperature}")
        out.close()


asyncio.run(main())
