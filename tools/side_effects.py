"""Side-effect test (AC off): for each control, diff EVERY state field before/after, restore, confirm.

Also checks that a rejected request stages nothing, and that a set's reply already shows property-write
side effects (e.g. swing after a fixed angle).
"""
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
ACCTL = str(ROOT / "native" / "build" / "acctl.exe")
SKIP = {"updated_at", "indoor_temperature", "outdoor_temperature", "diagnostics", "energy", "ip", "online",
        "power_watts"}  # live readings; a one-shot acctl set never polls the data groups
ok_all = True


def acctl(*args):
    p = subprocess.run([ACCTL, *args], capture_output=True, text=True, timeout=120)
    return json.loads(p.stdout) if p.stdout.strip() else None


def state():
    return acctl("query")["state"]


def diff(a, b):
    return {k: (a[k], b[k]) for k in a if k not in SKIP and a.get(k) != b.get(k)}


def check(label, cond, extra=""):
    global ok_all
    ok_all &= bool(cond)
    print(("PASS " if cond else "FAIL ") + label + (f"  {extra}" if extra else ""))


base = state()
assert base["power"] is False, "run with the AC off"
restore = [f"mode={base['mode']}", f"target_temperature={base['target_temperature']}",
           f"fan_speed={base['fan_speed'] if base['fan_speed'] != 'custom' else base['fan_percent']}",
           f"swing={base['swing']}", f"purifier={str(base['purifier']).lower()}",
           f"breeze_away={str(base['breeze_away']).lower()}",
           f"freeze_protection={str(base['freeze_protection']).lower()}",
           f"fahrenheit={str(base['fahrenheit']).lower()}", "vertical_angle=release", "horizontal_angle=release"]
print("base:", {k: v for k, v in base.items() if k not in SKIP})

for change in ["mode=cool", "fan_speed=low", "fan_speed=55", "swing=both", "vertical_angle=50",
               "horizontal_angle=25", "breeze_away=true", "purifier=true", "freeze_protection=true",
               "fahrenheit=true", "beep=true"]:
    reply = acctl("set", change)
    after = state()
    d = diff(base, after)
    stale = diff(reply["state"], after) if reply and reply.get("state") else {"no reply": (None, None)}
    print(f"\n{change}: changed {d}")
    check(f"  reply matches a fresh read", not stale, f"stale: {stale}" if stale else "")
    acctl("set", *restore)
    back = diff(base, state())
    check(f"  restored", not back, f"left: {back}" if back else "")

# Rejected request must not stage anything
acctl("set", "vertical_angle=50", "mode=turbo")
acctl("set", f"target_temperature={base['target_temperature']}")
after = state()
check("\nrejected request staged nothing", not diff(base, after), str(diff(base, after)))

print("\nALL PASS" if ok_all else "\nSOME FAILED")
