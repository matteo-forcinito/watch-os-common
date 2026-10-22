"""Drivable car for testing Motore: an ELM327 adapter on TCP with a Renault
Clio IV 1.2 (5 speed, MAP sensor, ISO 15765-4 CAN 11 bit 500 kbaud) behind it.

    python clio_sim.py [port]          (default 35000, all interfaces)

Keys (in this window):
    W  accelerator (hold or tap)      S  brake        C  clutch (toggle)
    1-5 gear     N neutral             E  add a fault (misfire, check engine light on)
    X  clear the faults                Q  quit

The device connects as to a Wi-Fi adapter (Motore > Collegamento > Wi-Fi,
address of this PC and the port). One client at a time, like real adapters.
Standard library only.
"""
import math
import msvcrt
import socket
import sys
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 35000

# ---------------------------------------------------------------- the car
GEAR_KMH = [0, 7.1, 13.0, 19.0, 25.0, 31.5]   # km/h at 1000 rpm per gear (Clio IV 1.2)
IDLE, REDLINE, LIMITER = 800.0, 6000.0, 6300.0
MASS = 1100.0            # kg with driver
VIN = "VF15R0K0H51234567"


class Car:
    def __init__(self):
        self.lock = threading.Lock()
        self.throttle = 0.0       # 0..1
        self.brake = 0.0
        self.clutch = False       # True = pedal down (disengaged)
        self.gear = 0
        self.speed = 0.0          # km/h
        self.rpm = IDLE
        self.coolant = 22.0
        self.intake = 21.0
        self.runtime = 0.0
        self.dist_since_clear = 1234.0
        self.dtcs = []            # stored codes, e.g. ["P0301"]
        self.pending = []
        self.freeze = None        # values when the first code was stored
        self.last_press = 0.0

    def torque(self, rpm):
        """Nm at full throttle: 1.2 16V, ~107 Nm at 4250 rpm."""
        x = (rpm - 4250) / 3000
        return max(40.0, 107 * (1 - 0.55 * x * x))

    def step(self, dt):
        with self.lock:
            # Pedal decays when not tapped: tap W to keep accelerating.
            if time.time() - self.last_press > 0.25:
                self.throttle = max(0.0, self.throttle - dt * 1.5)
            self.brake = max(0.0, self.brake - dt * 3)
            v = self.speed / 3.6
            engaged = self.gear > 0 and not self.clutch
            if engaged:
                wheel_rpm = self.speed * 1000 / GEAR_KMH[self.gear]
                if wheel_rpm < IDLE * 0.85 and self.throttle < 0.2:
                    wheel_rpm = IDLE   # would stall: the idle controller holds it
                self.rpm = wheel_rpm
                t = self.torque(self.rpm) * (self.throttle if self.rpm < LIMITER else 0)
                # Force at the wheels per Nm of engine torque: overall ratio / tyre radius
                # = 120 pi / (km/h per 1000 rpm), about 52 N/Nm in 1st, 12 in 5th.
                per_nm = 120 * math.pi / GEAR_KMH[self.gear]
                force = (t * 0.9 - 30 * (1 - self.throttle)) * per_nm   # drive minus engine braking
            else:
                target = IDLE + self.throttle * (LIMITER - IDLE)
                self.rpm += (target - self.rpm) * min(1.0, dt * 4)
                force = 0.0
            drag = 0.5 * 1.2 * 0.66 * v * v + 0.012 * MASS * 9.81 * (1 if v > 0.1 else 0)
            force -= drag + self.brake * 9000
            v = max(0.0, v + force / MASS * dt)
            self.speed = v * 3.6
            self.coolant = min(90.0, self.coolant + dt * (0.6 + self.throttle))
            self.runtime += dt
            self.dist_since_clear += self.speed * dt / 3600

    def map_kpa(self):
        return 30 + 70 * self.throttle if self.rpm > 0 else 101

    def snapshot(self):
        with self.lock:
            return dict(rpm=self.rpm, speed=self.speed, coolant=self.coolant, intake=self.intake, map=self.map_kpa(),
                        load=20 + 75 * self.throttle, throttle=self.throttle * 100, runtime=self.runtime,
                        dist=self.dist_since_clear)


car = Car()

# ---------------------------------------------------------------- OBD answers
SUPPORTED = {0x01, 0x04, 0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x11, 0x14, 0x1F, 0x21, 0x2F, 0x31, 0x33, 0x42, 0x46}


def bitmap(base):
    value = 0
    for pid in range(base + 1, base + 0x21):
        if pid in SUPPORTED or (pid == base + 0x20 and any(p > base + 0x20 for p in SUPPORTED)):
            value |= 1 << (32 - (pid - base))
    return value.to_bytes(4, "big")


def pid_bytes(pid, s):
    if pid in (0x00, 0x20, 0x40, 0x60):
        return bitmap(pid)
    mil = 0x80 if car.dtcs else 0
    table = {
        0x01: bytes([mil | len(car.dtcs), 0x07, 0x65, 0x00]),
        0x04: bytes([int(s["load"] * 255 / 100)]),
        0x05: bytes([int(s["coolant"] + 40)]),
        0x06: bytes([128 + int(math.sin(time.time()) * 4)]),
        0x07: bytes([128 + 3]),
        0x0B: bytes([int(s["map"])]),
        0x0C: int(s["rpm"] * 4).to_bytes(2, "big"),
        0x0D: bytes([int(min(255, s["speed"]))]),
        0x0E: bytes([int((12 + s["throttle"] / 5 + 64) * 2)]),
        0x0F: bytes([int(s["intake"] + 40)]),
        0x11: bytes([int(s["throttle"] * 255 / 100)]),
        0x14: bytes([int((0.45 + 0.4 * math.sin(time.time() * 6)) * 200), 0xFF]),
        0x1F: int(s["runtime"]).to_bytes(2, "big"),
        0x21: (0).to_bytes(2, "big"),
        0x2F: bytes([int(0.62 * 255)]),
        0x31: int(s["dist"]).to_bytes(2, "big"),
        0x33: bytes([101]),
        0x42: int(14.1 * 1000).to_bytes(2, "big"),
        0x46: bytes([int(14 + 40)]),
    }
    return table.get(pid)


def dtc_bytes(code):
    letter = "PCBU".index(code[0]) << 6
    return bytes([letter | int(code[1]) << 4 | int(code[2], 16), int(code[3:5], 16)])


class Adapter:
    def __init__(self):
        self.echo = True
        self.spaces = True
        self.headers = False

    def fmt(self, data):
        hexes = ["%02X" % b for b in data]
        return (" " if self.spaces else "").join(hexes)

    def frames(self, payload):
        """ISO-TP as the ELM shows it: one line, or a length line + numbered frames."""
        if len(payload) <= 7:
            return [self.fmt(payload)]
        lines = ["%03X" % len(payload)]
        chunks = [payload[:6]] + [payload[i:i + 7] for i in range(6, len(payload), 7)]
        for n, c in enumerate(chunks):
            lines.append("%X:%s%s" % (n % 16, " " if self.spaces else "", self.fmt(c)))
        return lines

    def answer(self, cmd):
        c = cmd.strip().upper().replace(" ", "")
        if not c:
            return []
        if c.startswith("AT"):
            a = c[2:]
            if a in ("Z", "WS"):
                self.__init__()
                return ["", "ELM327 v1.5"]
            if a in ("E0", "E1"):
                self.echo = a == "E1"
            elif a in ("S0", "S1"):
                self.spaces = a == "S1"
            elif a in ("H0", "H1"):
                self.headers = a == "H1"
            elif a == "I":
                return ["ELM327 v1.5"]
            elif a == "RV":
                return ["%.1fV" % (14.1 if car.rpm > 300 else 12.5)]
            elif a == "DP":
                return ["AUTO, ISO 15765-4 (CAN 11/500)"]
            elif a == "DPN":
                return ["A6"]
            elif a == "@1":
                return ["Motore Clio simulator"]
            return ["OK"]
        try:
            mode = int(c[:2], 16)
        except ValueError:
            return ["?"]
        s = car.snapshot()
        if mode == 0x01 and len(c) >= 4:
            data = pid_bytes(int(c[2:4], 16), s)
            return self.frames(bytes([0x41, int(c[2:4], 16)]) + data) if data else ["NO DATA"]
        if mode == 0x02 and len(c) >= 4:
            pid = int(c[2:4], 16)
            if not car.freeze:
                return ["NO DATA"]
            if pid == 0x02:
                return self.frames(bytes([0x42, 0x02, 0x00]) + dtc_bytes(car.dtcs[0]))
            data = pid_bytes(pid, car.freeze)
            return self.frames(bytes([0x42, pid, 0x00]) + data) if data else ["NO DATA"]
        if mode in (0x03, 0x07, 0x0A):
            codes = car.dtcs if mode == 0x03 else car.pending if mode == 0x07 else []
            payload = bytes([mode + 0x40, len(codes)]) + b"".join(dtc_bytes(x) for x in codes)
            return self.frames(payload)
        if mode == 0x04:
            with car.lock:
                car.dtcs, car.pending, car.freeze, car.dist_since_clear = [], [], None, 0.0
            return ["44"]
        if mode == 0x09 and c[2:4] == "02":
            return self.frames(bytes([0x49, 0x02, 0x01]) + VIN.encode())
        return ["NO DATA"]


def serve():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", PORT))
    srv.listen(1)
    while True:
        conn, addr = srv.accept()
        status["client"] = addr[0]
        adapter = Adapter()
        buf = b""
        try:
            conn.settimeout(30)
            while True:
                data = conn.recv(256)
                if not data:
                    break
                if data.startswith(b"GET ") or data.startswith(b"POST "):
                    break   # a browser: this is not a web page
                buf += data
                while b"\r" in buf:
                    line, buf = buf.split(b"\r", 1)
                    text = line.decode("ascii", "replace")
                    out = ""
                    if adapter.echo:
                        out += text + "\r"
                    lines = adapter.answer(text)
                    if text.strip().upper().startswith("01") and status["first"]:
                        status["first"] = False
                        lines = ["SEARCHING..."] + lines
                    out += "\r".join(lines) + "\r\r>"
                    conn.sendall(out.encode())
                    status["requests"] += 1
        except OSError:
            pass
        finally:
            conn.close()
            status["client"] = None
            status["first"] = True


status = {"client": None, "requests": 0, "first": True}


def main():
    threading.Thread(target=serve, daemon=True).start()
    print(__doc__.split("\n\n")[1])
    print("In ascolto sulla porta %d.\n" % PORT)
    last = time.time()
    while True:
        while msvcrt.kbhit():
            k = msvcrt.getwch().lower()
            with car.lock:
                if k == "w":
                    car.throttle = min(1.0, car.throttle + 0.35)
                    car.last_press = time.time()
                elif k == "s":
                    car.brake = 1.0
                    car.throttle = 0.0
                elif k == "c":
                    car.clutch = not car.clutch
                elif k in "12345":
                    car.gear = int(k)
                elif k == "n":
                    car.gear = 0
                elif k == "e" and "P0301" not in car.dtcs:
                    car.freeze = None
                    car.dtcs.append("P0301")
                    car.pending.append("P0302")
                elif k == "x":
                    car.dtcs, car.pending, car.freeze = [], [], None
                elif k == "q":
                    return
            if k == "e" and car.freeze is None:
                car.freeze = car.snapshot()
        now = time.time()
        car.step(now - last)
        last = now
        s = car.snapshot()
        gear = "N" if car.gear == 0 else str(car.gear)
        bar = "#" * int(car.throttle * 10)
        print("\r  %-5s %5.0f rpm  %5.1f km/h  marcia %s%s  acc [%-10s] %s %s  %s   " % (
            "ROSSO" if s["rpm"] >= REDLINE else "", s["rpm"], s["speed"], gear, " (frizione)" if car.clutch else "",
            bar, "FRENO" if car.brake > 0.1 else "     ", "SPIA %s" % ",".join(car.dtcs) if car.dtcs else "",
            "collegato " + status["client"] if status["client"] else "in attesa"), end="", flush=True)
        time.sleep(0.05)


if __name__ == "__main__":
    main()
