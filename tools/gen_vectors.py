"""Generate golden test vectors for the native C++ port from msmart-ng (the reference implementation).

Writes native/tests/vectors.txt (git-ignored: it holds frames captured from this AC and its discovery reply).
Run:  .venv\\Scripts\\python.exe tools\\gen_vectors.py      (talks to the AC read-only, AC state unchanged)
"""
import asyncio
import json
import pathlib
import random
import socket
from hashlib import sha256

import msmart.lan as lan
from msmart.const import DISCOVERY_MSG
from msmart.device import AirConditioner as AC
from msmart.device.AC import command as C
from msmart.discover import Discover
from msmart.frame import Frame
from msmart import crc8

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "native" / "tests" / "vectors.txt"
CREDS = json.loads((ROOT / "ac_creds.json").read_text())
rnd = random.Random(1234)
lines: list[str] = []


def h(b: bytes) -> str:
    return bytes(b).hex()


def rb(n: int) -> bytes:
    return bytes(rnd.getrandbits(8) for _ in range(n))


def N(v) -> str:
    if v is None:
        return "none"
    return format(float(v), ".6g")


# ---------------- primitives ----------------
for i in range(256):
    lines.append(f"crc8 {i:02x} {crc8.calculate(bytes([i])):02x}")
for n in (1, 7, 16, 33, 100):
    d = rb(n)
    lines.append(f"crc8 {h(d)} {crc8.calculate(d):02x}")
for n in (1, 15, 16, 17, 40, 100):
    d = rb(n)
    lines.append(f"aes_ecb {h(d)} {h(lan.Security.encrypt_aes(d))}")
    lines.append(f"sign {h(d)} {h(lan.Security.sign(d))}")
for _ in range(4):
    d = rb(6)
    lines.append(f"udpid {h(d)} {h(lan.Security.udpid(d))}")

# ---------------- V2 packet ----------------
for _ in range(4):
    ts = rb(8)
    dev_id = rnd.getrandbits(48)
    frame = rb(rnd.randint(10, 60))
    orig = lan._Packet._timestamp
    lan._Packet._timestamp = classmethod(lambda cls, ts=ts: ts)
    pkt = lan._Packet.encode(dev_id, frame)
    lan._Packet._timestamp = orig
    lines.append(f"v2encode {dev_id} {h(frame)} {h(ts)} {h(pkt)}")
    lines.append(f"v2decode {h(pkt)} {h(lan._Packet.decode(pkt))}")

# ---------------- V3 envelope ----------------
for pid in (0, 1, 4095):
    token = rb(64)
    p = lan._LanProtocolV3()
    p._packet_id = pid
    lines.append(f"v3hand {pid} {h(token)} {h(p._encode_handshake_request(pid, token))}")

for n in (14, 30, 46, 77, 100):  # covers pad = 0 and pad > 0
    local_key = rb(32)
    data = rb(n)
    pid = rnd.randint(0, 4095)
    pad_bytes = rb(16)
    p = lan._LanProtocolV3()
    p._local_key = local_key
    orig = lan.get_random_bytes
    lan.get_random_bytes = lambda k, pb=pad_bytes: pb[:k]
    pkt = p._encode_encrypted_request(pid, data)
    lan.get_random_bytes = orig
    lines.append(f"v3enc {pid} {h(local_key)} {h(data)} {h(pad_bytes)} {h(pkt)}")

    # A matching *response* packet (type 3), decoded by msmart
    remainder = (len(data) + 2) % 16
    pad = 16 - remainder if remainder else 0
    header = b"\x83\x70" + (len(data) + pad + 32).to_bytes(2, "big") + b"\x20" + bytes([pad << 4 | 0x3])
    payload = pid.to_bytes(2, "big") + data + pad_bytes[:pad]
    resp = header + lan.Security.encrypt_aes_cbc(local_key, payload) + sha256(header + payload).digest()
    expected = p._decode_encrypted_response(memoryview(resp))
    if pad == 0:
        # Known msmart bug: payload[2:-pad] with pad == 0 slices to b"" and the frame is lost.
        # The C++ port returns the data; assert that instead.
        assert expected == b""
        expected = data
    lines.append(f"v3dec {h(local_key)} {h(resp)} {h(expected)}")

for _ in range(3):
    key, dec = rb(32), rb(32)
    response = lan.Security.encrypt_aes_cbc(key, dec) + sha256(dec).digest()
    p = lan._LanProtocolV3()
    lines.append(f"v3key {h(key)} {h(response)} {h(p._get_local_key(key, memoryview(response)))}")


# ---------------- commands ----------------
def cmd(msgid: int, kind: str, args: str, c) -> None:
    C.Command._message_id = msgid - 1
    lines.append(f"cmd {msgid} {kind} {args} {h(c.tobytes())}")


cmd(1, "caps0", "-", C.GetCapabilitiesCommand())
cmd(200, "caps1", "-", C.GetCapabilitiesCommand(True))
cmd(255, "state", "-", C.GetStateCommand())
for g in (1, 2, 4, 5, 7, 11):
    cmd(rnd.randint(1, 255), "group", str(g), C.GetGroupDataCommand(g))
for beep in (0, 1):
    t = C.ToggleDisplayCommand()
    t.beep_on = bool(beep)
    cmd(rnd.randint(1, 255), "display", str(beep), t)
P = C.PropertyId
cmd(9, "getprops", "0009,000a,0039,0042", C.GetPropertiesCommand([P.SWING_UD_ANGLE, P.SWING_LR_ANGLE, P.SELF_CLEAN, P.BREEZE_AWAY]))
for props in ({P.SWING_UD_ANGLE: 50, P.BUZZER: 0}, {P.BREEZE_AWAY: True, P.BUZZER: 1}, {P.BREEZE_AWAY: False},
              {P.SELF_CLEAN: True, P.BUZZER: False}, {P.OUT_SILENT: True}, {P.CASCADE: 2}, {P.FRESH_AIR: 60}):
    args = ",".join(f"{int(k):04x}={int(v)}" for k, v in props.items())
    cmd(rnd.randint(1, 255), "setprops", args, C.SetPropertiesCommand(props))
for _ in range(60):
    s = C.SetStateCommand()
    vals = dict(beep=rnd.random() < .5, power=rnd.random() < .5,
                temp=rnd.choice([16, 16.5, 17, 20.5, 24, 29.5, 30, 31, 12, 35.5]),
                mode=rnd.randint(1, 6), fan=rnd.choice([20, 40, 55, 60, 80, 100, 102]),
                swing=rnd.choice([0, 3, 12, 15]), eco=rnd.random() < .5, turbo=rnd.random() < .5,
                fahrenheit=rnd.random() < .5, sleep=rnd.random() < .5, freeze=rnd.random() < .5,
                followme=rnd.random() < .5, purifier=rnd.random() < .5, humidity=rnd.randint(30, 80),
                aux=rnd.random() < .5, indaux=rnd.random() < .5)
    s.beep_on, s.power_on, s.target_temperature = vals["beep"], vals["power"], float(vals["temp"])
    s.operational_mode, s.fan_speed, s.swing_mode = vals["mode"], vals["fan"], vals["swing"]
    s.eco, s.turbo, s.fahrenheit, s.sleep = vals["eco"], vals["turbo"], vals["fahrenheit"], vals["sleep"]
    s.freeze_protection, s.follow_me, s.purifier = vals["freeze"], vals["followme"], vals["purifier"]
    s.target_humidity, s.aux_heat, s.independent_aux_heat = vals["humidity"], vals["aux"], vals["indaux"]
    args = ",".join(f"{k}={N(float(v))}" for k, v in vals.items())
    cmd(rnd.randint(1, 255), "setstate", args, s)


# ---------------- responses ----------------
def describe(r) -> str:
    f: list[tuple[str, str]] = []
    if isinstance(r, C.StateResponse):
        f.append(("kind", "state"))
        for k in ("power_on", "target_temperature", "operational_mode", "fan_speed", "swing_mode", "turbo", "eco",
                  "sleep", "fahrenheit", "indoor_temperature", "outdoor_temperature", "filter_alert", "display_on",
                  "follow_me", "purifier", "aux_heat", "independent_aux_heat", "target_humidity",
                  "freeze_protection", "error_code"):
            name = {"power_on": "power", "operational_mode": "mode"}.get(k, k)
            f.append((name, N(getattr(r, k))))
    elif isinstance(r, C.CapabilitiesResponse):
        f.append(("kind", "capabilities"))
        for k, v in sorted(r.raw_capabilities.items()):
            f.append((k, N(v)))
        f.append(("additional", N(r.additional_capabilities)))
    elif isinstance(r, C.PropertiesResponse):
        f.append(("kind", "properties"))
        for k, v in sorted(r._properties.items(), key=lambda kv: int(kv[0])):
            f.append((f"{int(k):04x}", N(v)))
    elif isinstance(r, C.Group1Response):
        f.append(("kind", "group1"))
        for k in ("compressor_frequency", "target_compressor_frequency", "compressor_current", "compressor_voltage",
                  "indoor_temperature", "indoor_coil_temperature", "outdoor_coil_temperature", "outdoor_temperature",
                  "discharge_pipe_temperature"):
            f.append((k, N(getattr(r, k))))
    elif isinstance(r, C.Group2Response):
        f.append(("kind", "group2"))
        for k in ("target_indoor_fan_speed", "indoor_fan_speed", "water_pump_running"):
            f.append((k, N(getattr(r, k))))
    elif isinstance(r, C.Group4Response):
        f.append(("kind", "group4"))
        for k in ("total_energy", "current_energy", "real_time_power", "total_energy_binary", "current_energy_binary",
                  "real_time_power_binary"):
            f.append((k, N(getattr(r, k))))
    elif isinstance(r, C.Group5Response):
        f.append(("kind", "group5"))
        for k in ("humidity", "outdoor_fan_speed", "defrost"):
            f.append((k, N(getattr(r, k))))
    elif isinstance(r, C.Group7Response):
        f.append(("kind", "group7"))
        f.append(("outdoor_unit_power", N(r.outdoor_unit_power)))
    elif isinstance(r, C.Group11Response):
        f.append(("kind", "group11"))
        f.append(("horizontal_louvers_angle", N(r.horizontal_louvers_angle)))
        f.append(("vertical_louvers_angle", N(r.vertical_louvers_angle)))
    else:
        f.append(("kind", "unknown"))
    return ";".join(f"{k}={v}" for k, v in f)


def add_resp(frame: bytes) -> bool:
    try:
        r = C.Response.construct(frame)
        lines.append(f"resp {h(frame)} {describe(r)}")
        return True
    except Exception:
        return False


def refit(frame: bytearray) -> bytes:
    """Recompute payload CRC and frame checksum after mutating a payload."""
    frame[-2] = crc8.calculate(bytes(frame[10:-2]))
    frame[-1] = Frame.checksum(bytes(frame[1:-1]))
    return bytes(frame)


async def capture() -> list[bytes]:
    dev = AC(ip=CREDS["ip"], port=CREDS["port"], device_id=CREDS["id"])
    await dev.authenticate(CREDS["token"], CREDS["key"])
    frames: list[bytes] = []
    for c in [C.GetCapabilitiesCommand(), C.GetCapabilitiesCommand(True), C.GetStateCommand(),
              *[C.GetGroupDataCommand(g) for g in range(16)],
              C.GetPropertiesCommand([P.SWING_UD_ANGLE, P.SWING_LR_ANGLE, P.SELF_CLEAN, P.BREEZE_AWAY, P.BUZZER]),
              C.GetPropertiesCommand(list(P))]:
        frames += await dev._send_command(c)
    return frames


captured = asyncio.run(capture())
real = sum(add_resp(f) for f in captured)
print(f"captured {len(captured)} frames, {real} parse in msmart")

# Mutations of real frames: random payload bytes, valid CRC/checksum, so both parsers see unusual values.
mutated = 0
for f in captured:
    kind_id = f[10]
    for _ in range(40 if kind_id in (0xC0, 0xC1) else 10):
        g = bytearray(f)
        lo = 11 if kind_id != 0xC1 else 14  # keep group number intact
        for _ in range(rnd.randint(1, 6)):
            i = rnd.randrange(lo, len(g) - 2)
            g[i] = rnd.getrandbits(8)
        mutated += add_resp(refit(g))
print(f"mutated frames: {mutated}")
# Short / corrupt frames must be rejected by both
for f in captured[:3]:
    bad = bytearray(f)
    bad[-1] ^= 0xFF
    lines.append(f"resp {h(bad)} error")


# ---------------- discovery ----------------
async def discovery_vectors() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(2)
    s.sendto(DISCOVERY_MSG, ("255.255.255.255", 6445))
    n = 0
    try:
        while True:
            data, (ip, _) = s.recvfrom(1024)
            try:
                version = Discover._get_device_version(data)
                info = await Discover._get_device_info(ip, version, data)
            except Exception:
                continue
            lines.append(f"disc {ip} {h(data)} {info['device_id']},{info['port']},{info['sn']},{info['name']},"
                         f"{info['version']},{info['device_type']}")
            n += 1
    except socket.timeout:
        pass
    return n


print(f"discovery replies: {asyncio.run(discovery_vectors())}")

OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text("# generated by tools/gen_vectors.py from msmart-ng\n" + "\n".join(lines) + "\n")
print(f"wrote {len(lines)} vectors to {OUT}")
