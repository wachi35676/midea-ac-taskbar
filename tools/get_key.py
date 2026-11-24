"""Fetch the one-time local token/key for a Midea AC using your own app login.

Run:  python tools/get_key.py [--ip 192.168.1.50] [--region US]

Your email and password are sent only to Midea's cloud; nothing is stored except
the device token/key, written to ac_creds.json in the project root (git-ignored).
Without --ip, the AC is found by UDP broadcast discovery on your network.
Needs:  pip install msmart-ng
"""
import argparse
import asyncio
import getpass
import json
import pathlib

from msmart.cloud import NetHomePlusCloud, SmartHomeCloud
from msmart.discover import Discover
from msmart.lan import Security

OUT = pathlib.Path(__file__).resolve().parent.parent / "ac_creds.json"


async def find(ip):
    if ip:
        return await Discover.discover_single(ip, auto_connect=False)
    devices = await Discover.discover(auto_connect=False)
    return devices[0] if devices else None


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", help="the AC's IP address (default: discover it)")
    ap.add_argument("--region", default="US", help="cloud region: US, DE, KR... (default US)")
    args = ap.parse_args()

    account = input("Midea app email: ").strip()
    password = getpass.getpass("Midea app password (hidden): ")

    dev = await find(args.ip)
    if dev is None:
        print("No AC found. Is it powered and on the same Wi-Fi? Try --ip.")
        return
    print(f"Found AC id={dev.id} at {dev.ip}:{dev.port}")

    for cls in (SmartHomeCloud, NetHomePlusCloud):
        name = cls.__name__
        try:
            cloud = cls(args.region, account=account, password=password)
            await cloud.login()
            print(f"{name}: logged in")
        except Exception as e:
            print(f"{name}: login failed: {e}")
            continue

        for endian in ("little", "big"):
            udpid = Security.udpid(dev.id.to_bytes(6, endian)).hex()
            try:
                token, key = await cloud.get_token(udpid, dev.id)
            except Exception as e:
                print(f"{name} ({endian}): token failed: {e}")
                continue
            try:
                await dev.authenticate(token, key)
            except Exception as e:
                print(f"{name} ({endian}): token received but AC rejected it: {e}")
                continue

            OUT.write_text(json.dumps(
                {"ip": dev.ip, "port": dev.port, "id": dev.id, "token": token, "key": key},
                indent=2))
            print(f"\nSUCCESS via {name} ({endian}). Saved to {OUT}")
            return

    print("\nNo working token. Check the account, the region and that the AC is bound to it.")


asyncio.run(main())
