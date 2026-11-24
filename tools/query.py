"""Read the AC's capabilities and current state over the LAN (no cloud)."""
import asyncio
import json
import pathlib

from msmart.device import AirConditioner as AC

CREDS = json.loads((pathlib.Path(__file__).resolve().parent.parent / "ac_creds.json").read_text())


async def main() -> None:
    dev = AC(ip=CREDS["ip"], port=CREDS["port"], device_id=CREDS["id"])
    await dev.authenticate(CREDS["token"], CREDS["key"])

    await dev.get_capabilities()
    print("=== Capabilities")
    for k, v in sorted(dev.capabilities_dict().items()):
        print(f"  {k}: {v}")

    await dev.refresh()
    print("\n=== Current state")
    for k, v in sorted(dev.to_dict().items()):
        print(f"  {k}: {v}")


asyncio.run(main())
