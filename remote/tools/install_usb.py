"""Install a Remote build in the external-app slot of a connected board, then boot it.

    python install_usb.py PORT MAC IMAGE

Reads the app header of both OTA slots and writes only the slot that does NOT
hold Watch OS (the OS may live in ota_0 or ota_1 after OTA updates). Pins the
board by MAC, never writes bootloader, partition table or NVS, verifies the
write, selects the slot. Leaving Remote returns to Watch OS in the other slot.
Run with ESP-IDF's Python (esptool) and IDF_PATH set.
"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

PORT, MAC, IMAGE = sys.argv[1], sys.argv[2].lower(), Path(sys.argv[3])
TABLE = 0x8000
ESPTOOL = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", PORT, "--baud", "460800"]


def esptool(*args, after="no-reset"):
    p = subprocess.run(ESPTOOL + ["--after", after, *args], capture_output=True, text=True, errors="replace")
    if p.returncode:
        sys.exit("esptool failed:\n" + p.stdout[-800:] + p.stderr[-800:])
    return p.stdout


def read(offset, size):
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "r.bin"
        esptool("read-flash", hex(offset), hex(size), str(path))
        return path.read_bytes()


def project(header):
    return header[0x20 + 48:0x20 + 80].split(b"\0")[0].decode("ascii", "replace") if header[0] == 0xE9 else ""


image = IMAGE.read_bytes()
if image[0] != 0xE9 or project(image) != "remote":
    sys.exit("Not a Remote image")
if MAC not in esptool("read-mac").lower():
    sys.exit("Wrong board: MAC is not " + MAC)

slots = {}
table = read(TABLE, 0xC00)
for i in range(0, len(table), 32):
    magic, kind, subtype, offset, size = struct.unpack_from("<HBBII", table, i)
    if magic == 0x50AA and kind == 0 and subtype in (0x10, 0x11):
        slots[subtype - 0x10] = (offset, size)
if len(slots) != 2:
    sys.exit("Partition table without ota_0/ota_1")

names = {n: project(read(offset, 0x100)) for n, (offset, _) in slots.items()}
print("slots:", names)
targets = [n for n, name in names.items() if name in ("", "remote") or not name.endswith(("_watch", "_os", "_panel"))]
os_slots = [n for n in names if n not in targets]
if len(os_slots) != 1:
    sys.exit("Cannot tell which slot holds Watch OS: " + str(names))
target = 1 - os_slots[0]
offset, size = slots[target]
if len(image) > size:
    sys.exit("Image larger than the slot")

print(f"Watch OS in ota_{os_slots[0]}: writing Remote to ota_{target} at {hex(offset)}")
esptool("write-flash", hex(offset), str(IMAGE))
select = Path(__file__).resolve().parents[2] / "solco" / "tools" / "select_slot_usb.py"
subprocess.run([sys.executable, str(select), PORT, "--slot", str(target)], check=True, capture_output=True)
esptool("read-mac", after="watchdog-reset")
print("Remote started; Esci returns to Watch OS.")
