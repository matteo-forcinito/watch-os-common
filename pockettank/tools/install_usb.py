"""Install Pocket Tank in ota_1 and its model in the free flash runs, over USB.

Layout (16 MB boards with the launcher's table: ota_0/ota_1 6 MB, coredump 256 KB):
  0x620000  app image (at most 0x260000 bytes)
  0x880000  model, first 0x3A0000 bytes (tail of ota_1)
  0xC60000  model, the rest (after coredump)
  0xFFF000  trailer: magic "PTTM", length, SHA-256 (firmware/main/pt_watch_os.h)

The bootloader, partition table, NVS and ota_0 are never written. 0x0-0x21000 is
backed up first. The board stays in download mode until the slot is selected.
"""
import argparse
import hashlib
import re
import struct
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

MODEL_URL = ("https://raw.githubusercontent.com/mediacutlet/pocket-tank/"
             "3df10498f2814f40f5ca7066535f3d44985e0ec2/model/out/model_q4.bin")
MODEL_SHA256 = "50bed3e16a6b11f3496dde21d8784da9067f40ec9f0a77cf97f60a95acb80f6b"
APP_SLOT, APP_MAX = 0x620000, 0x260000
RUN_A, RUN_A_SIZE = 0x880000, 0xC20000 - 0x880000
RUN_B, TRAILER = 0xC60000, 0xFFF000
MAGIC = 0x4D545450
BOARDS = {"amoled": ("amoled_watch", "pockettank_amoled")}

parser = argparse.ArgumentParser()
parser.add_argument("port")
parser.add_argument("--board", choices=BOARDS, default="amoled")
parser.add_argument("--app", type=Path, required=True)
parser.add_argument("--model", type=Path, help="model_q4.bin (default: download the pinned upstream file)")
parser.add_argument("--skip-model", action="store_true", help="the model is already installed and verified")
parser.add_argument("--expected-mac", required=True)
parser.add_argument("--backup-dir", type=Path, required=True)
args = parser.parse_args()
base_name, app_name = BOARDS[args.board]


def require(condition, message):
    if not condition:
        raise SystemExit(message)


def esptool(*cmd):
    return subprocess.run([sys.executable, "-m", "esptool", "--port", args.port, "--after", "no-reset", *cmd],
                          check=True, capture_output=True, text=True).stdout


name = lambda data, offset: data[offset:offset + 32].split(b"\0", 1)[0].decode("ascii", "replace")

image = args.app.read_bytes()
require(len(image) >= 256 and image[0] == 0xE9 and name(image, 80) == app_name, "Wrong application image")
require(len(image) <= APP_MAX, f"App is {len(image)} bytes, the model starts at {RUN_A:#x}")

model = b""
if not args.skip_model:
    if args.model:
        model = args.model.read_bytes()
    else:
        print("downloading", MODEL_URL, flush=True)
        model = urllib.request.urlopen(MODEL_URL, timeout=120).read()
    require(hashlib.sha256(model).hexdigest() == MODEL_SHA256, "Model SHA-256 differs from the pinned upstream model")
    require(len(model) - RUN_A_SIZE <= TRAILER - RUN_B, "Model does not fit the two runs")

require(re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", args.expected_mac), "Invalid expected MAC")
found = re.findall(r"MAC:\s*([0-9a-fA-F:]{17})", esptool("read-mac"))
require(found and all(x.lower() == args.expected_mac.lower() for x in found), "Wrong physical device; no write")

args.backup_dir.mkdir(parents=True, exist_ok=True)
backup = args.backup_dir / f"{base_name}-0x0-0x21000.bin"
esptool("read-flash", "0x0", "0x21000", str(backup))
meta = backup.read_bytes()
require(name(meta, 0x20000 + 80) == base_name, "The launcher in ota_0 is not " + base_name)
parts = []
for at in range(0x8000, 0x9000, 32):
    if meta[at:at + 2] != b"\xaaP":
        break
    _, kind, subtype, offset, size, label, _ = struct.unpack_from("<HBBII16sI", meta, at)
    parts.append((kind, subtype, offset, size, label.split(b"\0", 1)[0].decode()))
require((0, 0x11, APP_SLOT, 6 * 1024 * 1024, "ota_1") in parts, "ota_1 is not 6 MB at 0x620000")
require(all(p[2] + p[3] <= RUN_B for p in parts), "A partition reaches past 0xC60000: the model would overwrite it")
print("backup:", backup, flush=True)

writes = [hex(APP_SLOT), str(args.app)]
with tempfile.TemporaryDirectory() as tmp:
    if model:
        tmp = Path(tmp)
        (tmp / "a.bin").write_bytes(model[:RUN_A_SIZE])
        (tmp / "b.bin").write_bytes(model[RUN_A_SIZE:])
        trailer = struct.pack("<II", MAGIC, len(model)) + hashlib.sha256(model).digest()
        (tmp / "t.bin").write_bytes(trailer + b"\xff" * (4096 - len(trailer)))
        writes += [hex(RUN_A), str(tmp / "a.bin"), hex(RUN_B), str(tmp / "b.bin"), hex(TRAILER), str(tmp / "t.bin")]
    subprocess.run([sys.executable, "-m", "esptool", "--port", args.port, "--after", "no-reset", "--baud", "921600",
                    "write-flash", *writes], check=True)
select = Path(__file__).resolve().parents[2] / "solco" / "tools" / "select_slot_usb.py"
subprocess.run([sys.executable, str(select), args.port, "--slot", "1"], check=True)
print("installed: app", len(image), "bytes" + (f", model {len(model)} bytes" if model else ""))
