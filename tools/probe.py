"""Deep read-only probe: raw capabilities, every data group, energy formats, every property.

Writes a full log to probe_output.txt in the project root. Changes nothing on the AC.
"""
import asyncio
import json
import logging
import pathlib

from msmart.device import AirConditioner as AC
from msmart.device.AC.command import (GetGroupDataCommand, GetPropertiesCommand,
                                      PropertyId)

ROOT = pathlib.Path(__file__).resolve().parent.parent
CREDS = json.loads((ROOT / "ac_creds.json").read_text())
OUT = ROOT / "probe_output.txt"

handler = logging.FileHandler(OUT, mode="w", encoding="utf-8")
handler.setFormatter(logging.Formatter("%(name)s %(levelname)s %(message)s"))
for name in ("msmart.device", "msmart.device.AC.device", "msmart.device.AC.command"):
    lg = logging.getLogger(name)
    lg.setLevel(logging.DEBUG)
    lg.addHandler(handler)
log = logging.getLogger("probe")
log.setLevel(logging.DEBUG)
log.addHandler(handler)


def section(title: str) -> None:
    log.info("\n==================== %s ====================", title)
    print(f"--- {title}")


async def main() -> None:
    dev = AC(ip=CREDS["ip"], port=CREDS["port"], device_id=CREDS["id"])
    await dev.authenticate(CREDS["token"], CREDS["key"])

    section("Capabilities (raw + parsed)")
    await dev.get_capabilities()
    for k, v in sorted(dev.capabilities_dict().items()):
        log.info("cap %s = %s", k, v)
    supports = {a: getattr(dev, a) for a in dir(type(dev))
                if a.startswith("supports_") and isinstance(getattr(type(dev), a), property)}
    for k, v in sorted(supports.items()):
        log.info("%s = %s", k, v)

    section("State with every data group + energy enabled")
    dev.enable_energy_usage_requests = True
    for g in (1, 2, 5, 7, 11):
        setattr(dev, f"enable_group{g}_data_requests", True)
    await dev.refresh()
    for k, v in sorted(dev.to_dict().items()):
        log.info("state %s = %s", k, v)

    section("Energy in both formats")
    EnergyDataFormat = AC.EnergyDataFormat
    for fmt in EnergyDataFormat:
        log.info("%s total=%s current=%s realtime=%s", fmt.name,
                 dev.get_total_energy_usage(fmt), dev.get_current_energy_usage(fmt),
                 dev.get_real_time_power_usage(fmt))

    section("Raw group data 0..15 (unknown groups included)")
    for g in range(16):
        try:
            frames = await dev._send_command(GetGroupDataCommand(g))
            log.info("group %d -> %s", g, [f.hex() for f in frames])
        except Exception as e:
            log.info("group %d -> error %s", g, e)

    section("Every known property, one at a time")
    for prop in PropertyId:
        try:
            frames = await dev._send_command(GetPropertiesCommand([prop]))
            log.info("prop %s (0x%04X) -> %s", prop.name, prop.value, [f.hex() for f in frames])
        except Exception as e:
            log.info("prop %s -> error %s", prop.name, e)

    print(f"done, see {OUT}")


asyncio.run(main())
