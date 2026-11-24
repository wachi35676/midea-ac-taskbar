"""Drive backend/acd.py over stdio like the taskbar app will. AC stays off.

Checks: hello + first state, a write with read-back and restore, rejected commands,
and recovery from a wrong IP (rediscovery), using a throwaway copy of the creds.
"""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import queue

ROOT = pathlib.Path(__file__).resolve().parent.parent
PY = ROOT / ".venv" / "Scripts" / "python.exe"
ACD = ROOT / "backend" / "acd.py"


class Client:
    def __init__(self, creds: pathlib.Path):
        self.p = subprocess.Popen([str(PY), "-I", str(ACD), "--creds", str(creds), "--interval", "5"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, encoding="utf-8", bufsize=1)
        self.q: queue.Queue = queue.Queue()
        self.stderr: list[str] = []
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=lambda: self.stderr.extend(self.p.stderr), daemon=True).start()
        self.n = 0

    def _read(self):
        for line in self.p.stdout:
            self.q.put(json.loads(line))

    def wait(self, pred, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            try:
                m = self.q.get(timeout=end - time.time())
            except queue.Empty:
                break
            if pred(m):
                return m
        raise TimeoutError("no matching message")

    def call(self, cmd, **kw):
        self.n += 1
        mid = self.n
        t = time.perf_counter()
        self.p.stdin.write(json.dumps({"id": mid, "cmd": cmd, **kw}) + "\n")
        self.p.stdin.flush()
        m = self.wait(lambda m: m.get("id") == mid)
        return m, time.perf_counter() - t

    def close(self):
        self.p.stdin.write('{"cmd":"quit"}\n')
        self.p.stdin.flush()
        self.p.wait(timeout=10)


def check(label, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + label + (f"  {extra}" if extra else ""))
    return cond


def main():
    tmp = pathlib.Path(tempfile.mkdtemp())
    creds = tmp / "ac_creds.json"
    shutil.copy(ROOT / "ac_creds.json", creds)
    ok = True

    t = time.perf_counter()
    c = Client(creds)
    hello = c.wait(lambda m: m.get("event") == "hello")
    state = c.wait(lambda m: m.get("event") == "state")["state"]
    ok &= check("hello + first state", state["online"], f"{time.perf_counter() - t:.2f}s incl. Python start")
    ok &= check("capabilities listed", "cool" in hello["capabilities"]["modes"], str(hello["capabilities"]["modes"]))
    print("     state:", {k: state[k] for k in ("power", "mode", "target_temperature", "indoor_temperature",
                                                 "fan_speed", "swing")})
    ok &= check("AC is off (test precondition)", state["power"] is False)
    if not state["power"] is False:
        c.close(); sys.exit(1)

    orig = state["target_temperature"]
    m, dt = c.call("set", changes={"target_temperature": orig - 1})
    ok &= check("set target -1", m["ok"] and m["state"]["target_temperature"] == orig - 1, f"{dt:.2f}s")
    m, dt = c.call("set", changes={"target_temperature": orig})
    ok &= check("restore target", m["ok"] and m["state"]["target_temperature"] == orig, f"{dt:.2f}s")
    ok &= check("still off", m["state"]["power"] is False)

    for bad in ({"target_temperature": 35}, {"target_temperature": 24.3}, {"mode": "turbo"},
                {"fan_speed": 0}, {"vertical_angle": 0}, {"nonsense": 1}, {"power": "yes"}):
        m, _ = c.call("set", changes=bad)
        ok &= check(f"rejects {bad}", not m["ok"], m.get("error", ""))
    m, _ = c.call("bogus")
    ok &= check("rejects unknown cmd", not m["ok"])
    c.close()

    # Wrong IP: the backend should rediscover the unit by ID and update its creds copy.
    data = json.loads(creds.read_text())
    real_ip = data["ip"]
    data["ip"] = "192.0.2.1"
    creds.write_text(json.dumps(data))
    t = time.perf_counter()
    c = Client(creds)
    msg = c.wait(lambda m: m.get("event") in ("state", "offline"), timeout=40)
    ok &= check("recovers from wrong IP", msg.get("event") == "state" and msg["state"]["ip"] == real_ip,
                f"{time.perf_counter() - t:.2f}s")
    ok &= check("creds copy updated", json.loads(creds.read_text())["ip"] == real_ip)
    c.close()

    # Unreachable: no device with this ID anywhere -> offline event, process stays alive.
    data["ip"], data["id"] = "192.0.2.1", 1
    creds.write_text(json.dumps(data))
    t = time.perf_counter()
    c = Client(creds)
    msg = c.wait(lambda m: m.get("event") in ("state", "offline"), timeout=60)
    ok &= check("reports offline", msg.get("event") == "offline", f"{time.perf_counter() - t:.2f}s, "
                f"retry_in={msg.get('retry_in')}")
    m, _ = c.call("set", changes={"power": True})
    ok &= check("write while offline fails cleanly", not m["ok"], m.get("error", ""))
    c.close()

    shutil.rmtree(tmp, ignore_errors=True)
    print("\nALL PASS" if ok else "\nSOME FAILED")


main()
