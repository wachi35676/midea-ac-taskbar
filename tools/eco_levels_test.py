"""Does this AC accept Eco gear levels (rate select: 50 %, 75 %)? It doesn't advertise the capability,
so try it directly (user approved running the unit). Runs in Cool for about a minute, then restores."""
import asyncio
import json
import pathlib

from msmart.device import AirConditioner as AC
from msmart.device.AC.command import GetPropertiesCommand, PropertyId, Response, SetPropertiesCommand

ROOT = pathlib.Path(__file__).resolve().parent.parent
CREDS = json.loads((ROOT / "ac_creds.json").read_text())


async def raw(dev, cmd):
    out = []
    for f in await dev._send_command(cmd):
        try:
            r = Response.construct(f)
            out.append(f"{type(r).__name__} {r.payload.hex()}")
        except Exception as e:
            out.append(f"unparsed {f.hex()} ({e})")
    return out


async def main():
    dev = AC(ip=CREDS["ip"], port=CREDS["port"], device_id=CREDS["id"])
    await dev.authenticate(CREDS["token"], CREDS["key"])
    await dev.get_capabilities()
    await dev.refresh()
    orig = dict(power=dev.power_state, mode=dev.operational_mode, target=dev.target_temperature)
    print("before:", orig)
    try:
        dev.power_state, dev.operational_mode, dev.target_temperature = True, AC.OperationalMode.COOL, 26
        await dev.apply()
        await asyncio.sleep(3)
        print("read rate select:", await raw(dev, GetPropertiesCommand([PropertyId.RATE_SELECT])))
        for level in (50, 75, 100):
            print(f"set {level}:", await raw(dev, SetPropertiesCommand({PropertyId.RATE_SELECT: level, PropertyId.BUZZER: 0})))
            await asyncio.sleep(2)
            print(f"  read:", await raw(dev, GetPropertiesCommand([PropertyId.RATE_SELECT])))
        # iECO is the other "eco" family some units use
        print("read ieco:", await raw(dev, GetPropertiesCommand([PropertyId.IECO])))
        # Plain eco flag while cooling, for comparison
        await dev.refresh()
        dev.eco = True
        await dev.apply()
        await dev.refresh()
        print("eco flag in cool:", dev.eco)
        dev.eco = False
        await dev.apply()
    finally:
        await dev.refresh()
        dev.power_state, dev.operational_mode, dev.target_temperature = orig["power"], orig["mode"], orig["target"]
        await dev.apply()
        await dev.refresh()
        print("after:", dict(power=dev.power_state, mode=dev.operational_mode, target=dev.target_temperature))


asyncio.run(main())
