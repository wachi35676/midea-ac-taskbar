"""Live tests of native/build/acctl.exe against the AC, cross-checked with msmart-ng.

Covers: discovery, query vs Python, writes with read-back and restore, rejected input, wrong-IP recovery,
unreachable device, and a short power on/off cycle (user approved). Leaves the AC as it found it.
"""
import asyncio
import json
import pathlib
import shutil
import subprocess
import tempfile
import time

from msmart.device import AirConditioner as AC

ROOT = pathlib.Path(__file__).resolve().parent.parent
ACCTL = ROOT / "native" / "build" / "acctl.exe"
CREDS = ROOT / "ac_creds.json"
ok_all = True


def run(*args, creds=CREDS):
    t = time.perf_counter()
    p = subprocess.run([str(ACCTL), *args, "--creds", str(creds)], capture_output=True, text=True, timeout=120)
    dt = time.perf_counter() - t
    out = json.loads(p.stdout) if p.stdout.strip().startswith(("{", "[")) else None
    return p.returncode, out, p.stderr.strip().replace("\n", " | "), dt


def check(label, cond, extra=""):
    global ok_all
    ok_all &= bool(cond)
    print(("PASS " if cond else "FAIL ") + label + (f"  ({extra})" if extra else ""))


async def python_state():
    c = json.loads(CREDS.read_text())
    d = AC(ip=c["ip"], port=c["port"], device_id=c["id"])
    await d.authenticate(c["token"], c["key"])
    await d.get_capabilities()
    await d.refresh()
    return d


# --- discovery
rc, out, err, dt = run("discover")
creds = json.loads(CREDS.read_text())
check("discover finds the AC", rc == 0 and any(d["id"] == creds["id"] for d in out), f"{dt:.1f}s")

# --- query vs Python
rc, q, err, dt = run("query")
check("query", rc == 0 and q["state"]["online"], f"{dt:.1f}s; {err}")
py = asyncio.run(python_state())
s = q["state"]
pairs = {
    "power": (s["power"], py.power_state),
    "mode": (s["mode"], {1: "auto", 2: "cool", 3: "dry", 4: "heat", 5: "fan"}[int(py.operational_mode)]),
    "target_temperature": (s["target_temperature"], py.target_temperature),
    "indoor_temperature": (s["indoor_temperature"], py.indoor_temperature),
    "fan_percent": (s["fan_percent"], int(py.fan_speed)),
    "swing": (s["swing"], {0: "off", 3: "horizontal", 12: "vertical", 15: "both"}[int(py.swing_mode)]),
    "eco/turbo/sleep": ((s["eco"], s["turbo"], s["sleep"]), (py.eco, py.turbo, py.sleep)),
    "purifier/freeze": ((s["purifier"], s["freeze_protection"]), (py.purifier, py.freeze_protection)),
    "angles": ((s["vertical_angle"], s["horizontal_angle"]), (int(py.vertical_swing_angle), int(py.horizontal_swing_angle))),
    "breeze_away": (s["breeze_away"], py.breeze_away),
}
for k, (a, b) in pairs.items():
    check(f"matches Python: {k}", a == b, f"C++ {a} / Python {b}")
caps = q["capabilities"]
check("capabilities match Python", sorted(caps["modes"]) == sorted(["fan", "dry", "cool", "heat", "auto"])
      and caps["min_temperature"] == py.min_target_temperature and caps["max_temperature"] == py.max_target_temperature
      and caps["custom_fan_speed"] == py.supports_custom_fan_speed, json.dumps(caps)[:120])
print("     diagnostics:", s["diagnostics"])

orig = s
# --- writes (AC off), read back, restore
for change, field, want in [("target_temperature=29", "target_temperature", 29),
                            ("mode=cool", "mode", "cool"), ("fan_speed=low", "fan_speed", "low"),
                            ("fan_speed=55", "fan_percent", 55), ("swing=both", "swing", "both"),
                            ("vertical_angle=50", "vertical_angle", 50), ("breeze_away=true", "breeze_away", True),
                            ("purifier=true", "purifier", True)]:
    rc, out, err, dt = run("set", change)
    got = out["state"][field] if out and out.get("state") else None
    check(f"set {change}", rc == 0 and got == want, f"read {got}, {err.split('|')[-1].strip()}")
rc, out, err, dt = run("set", f"target_temperature={orig['target_temperature']}", f"mode={orig['mode']}",
                       "fan_speed=auto", f"swing={orig['swing']}", "breeze_away=false", "purifier=false")
check("restore", rc == 0, err)
rc, q2, err, dt = run("query")
s2 = q2["state"]
check("state restored", all(s2[k] == orig[k] for k in ("power", "mode", "target_temperature", "fan_percent", "swing",
                                                        "breeze_away", "purifier", "vertical_angle")),
      {k: s2[k] for k in ("mode", "target_temperature", "fan_percent", "swing", "vertical_angle")})

# --- rejected input
for bad in ("target_temperature=35", "target_temperature=24.3", "mode=turbo", "fan_speed=0", "vertical_angle=0",
            "nonsense=1", "power=yes"):
    rc, out, err, dt = run("set", bad)
    check(f"rejects {bad}", rc != 0 and out and not out["ok"], out and out.get("error"))

# --- power cycle (approved)
rc, out, err, dt = run("set", "power=true")
check("power on", rc == 0 and out["state"]["power"] is True, err.split("|")[-1].strip())
time.sleep(3)
rc, out, err, dt = run("set", "turbo=true")
check("turbo while running", rc == 0 and out["state"]["turbo"] is True)
rc, out, err, dt = run("set", "turbo=false", "power=false")
check("turbo off + power off", rc == 0 and out["state"]["power"] is False and out["state"]["turbo"] is False)

# --- wrong IP -> rediscovery, on a throwaway creds copy
tmp = pathlib.Path(tempfile.mkdtemp())
c2 = tmp / "ac_creds.json"
data = json.loads(CREDS.read_text())
real_ip = data["ip"]
data["ip"] = "192.0.2.1"
c2.write_text(json.dumps(data))
rc, out, err, dt = run("query", creds=c2)
check("recovers from wrong IP", rc == 0 and out["state"]["ip"] == real_ip, f"{dt:.1f}s")
check("creds copy updated", json.loads(c2.read_text())["ip"] == real_ip)

# --- unreachable
data["ip"], data["id"] = "192.0.2.1", 1
c2.write_text(json.dumps(data))
rc, out, err, dt = run("query", creds=c2)
check("reports unreachable cleanly", rc != 0 and out and out["online"] is False, f"{dt:.1f}s: {err}")
shutil.rmtree(tmp, ignore_errors=True)

print("\nALL PASS" if ok_all else "\nSOME FAILED")
