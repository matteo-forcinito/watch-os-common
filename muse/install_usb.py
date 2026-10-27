"""Install a Watch OS build of Muse into ota_1 of a connected board, then boot it.

Usage: install_usb.py PORT MAC IMAGE PARTITION_TABLE_BIN

Pins the board by MAC, saves bootloader/partition table/NVS/otadata
(0x0-0x21000) before writing, requires the live partition table to match the
image's build, and never writes anything but ota_1 and the boot selection.
Back in Watch OS: Muse > Settings > Watch OS.
"""
import hashlib, json, os, re, subprocess, sys, time
from pathlib import Path

PY = sys.executable
port, mac, image, table = sys.argv[1], sys.argv[2].lower(), Path(sys.argv[3]), Path(sys.argv[4])
out = Path.home() / ".aetherion" / "diagnostics" / ("muse-install-" + str(time.time_ns()))
out.mkdir(parents=True)
base = [PY, "-m", "esptool", "--chip", "esp32s3", "--port", port, "--no-stub", "--baud", "460800", "--after", "no-reset"]


def run(*args):
    p = subprocess.run(base + list(args), capture_output=True, text=True, encoding="utf-8", errors="replace")
    (out / "esptool.log").open("a", encoding="utf-8").write(p.stdout + p.stderr)
    if p.returncode:
        raise SystemExit("esptool failed, see " + str(out / "esptool.log"))
    return p.stdout


data = image.read_bytes()
name = lambda blob, at: blob[at + 80:at + 112].split(b"\0")[0].decode("ascii", "replace")
assert data[0] == 0xE9 and name(data, 0) == "muse-gadget", "Not a Muse image"
found = re.findall(r"MAC:\s*([0-9a-fA-F:]{17})", run("read-mac"))
assert found and all(x.lower() == mac for x in found), "Wrong physical board; nothing written"
run("read-flash", "--no-progress", "0x0", "0x21000", str(out / "metadata.bin"))
meta = (out / "metadata.bin").read_bytes()
table_bin = table.read_bytes()
assert meta[0x8000:0x8000 + len(table_bin)] == table_bin, "Live partition table differs; nothing written"
slot = None
for at in range(0x8000, 0x9000, 32):
    if meta[at:at + 2] != b"\xaaP":
        break
    kind, subtype, offset, size = meta[at + 2], meta[at + 3], int.from_bytes(meta[at + 4:at + 8], "little"), int.from_bytes(meta[at + 8:at + 12], "little")
    if kind == 0 and subtype == 0x11:
        slot = (offset, size)
assert slot and len(data) <= slot[1], "No ota_1 large enough"
assert name(meta, 0x20000) != "muse-gadget", "ota_0 must keep Watch OS"
run("read-flash", "--no-progress", hex(slot[0]), "0x1000", str(out / "slot1-header.bin"))
print("Watch OS in ota_0:", name(meta, 0x20000), "| previous ota_1:", name((out / "slot1-header.bin").read_bytes(), 0), flush=True)
run("write-flash", hex(slot[0]), str(image))
run("verify-flash", hex(slot[0]), str(image))
env = dict(os.environ, IDF_PATH=os.environ.get("IDF_PATH", "C:/esp/v6.1/esp-idf"))
selector = Path(__file__).resolve().parents[1] / "solco" / "tools" / "select_slot_usb.py"
subprocess.run([PY, str(selector), port, "--slot", "1"], check=True, env=env, capture_output=True)
receipt = dict(port=port, mac=mac, slot=hex(slot[0]), image_sha256=hashlib.sha256(data).hexdigest(), backup=str(out / "metadata.bin"))
(out / "receipt.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
print(json.dumps(receipt, indent=2))
