"""Install Solco in ota_1 after verifying the saved board identity and backup.

The base OS in ota_0, bootloader, NVS and partition table are never written.
Requires ESP-IDF's Python environment and IDF_PATH.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import re
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("port")
parser.add_argument("image", type=Path)
parser.add_argument("metadata_backup", type=Path)
parser.add_argument("slot_backup", type=Path)
parser.add_argument("--board", choices=("amoled", "knob", "watcher"), required=True)
parser.add_argument("--expected-mac", help="Pin the physical board MAC before reading/writing")
parser.add_argument("--update", action="store_true", help="Replace an already installed Solco in the secondary slot")
args = parser.parse_args()
image = args.image.read_bytes()
metadata = args.metadata_backup.read_bytes()
backup = args.slot_backup.read_bytes()
expected_base, base_offset, app_offset, slot_size, ota_offset = {
    "amoled":("amoled_watch",0x20000,0x620000,6*1024*1024,0xf000),
    "knob":("knob_os",0x20000,0x620000,6*1024*1024,0xf000),
    "watcher":("watcher_os",0x100000,0x800000,7*1024*1024,0x7b000),
}[args.board]
expected_app = "solco_" + args.board
name = lambda data, offset: data[offset:offset + 32].split(b"\0", 1)[0].decode("ascii")
def require(condition, message):
    if not condition:
        raise SystemExit(message)

require(name(metadata, base_offset + 80) == expected_base, "Wrong board backup")
partitions=[]
for at in range(0x8000,0x9000,32):
    if metadata[at:at+2]!=b'\xaaP': break
    magic,kind,subtype,offset,size,label,flags=struct.unpack_from('<HBBII16sI',metadata,at)
    partitions.append((kind,subtype,offset,size))
require((0,0x10,base_offset,slot_size) in partitions and (0,0x11,app_offset,slot_size) in partitions,
        "Saved partition layout differs from this board profile")
require(len(image) >= 256 and image[0] == 0xE9 and name(image, 80) == expected_app, "Wrong application image")
require(len(image) <= len(backup) and len(image) <= slot_size, "Insufficient slot backup")
require(len(backup) % 4096 == 0 and len(backup) <= slot_size,
        "Slot backup must cover complete flash sectors within the app partition")
entries = [struct.unpack_from("<I20sII", metadata, offset) for offset in (ota_offset, ota_offset + 4096)]
valid = [entry for entry in entries if entry[0] != 0xffffffff and entry[2] == 2]
require(valid and (max(valid, key=lambda entry: entry[0])[0] - 1) % 2 == 0, "Base must be in ota_0")
receipt = {
    "port": args.port, "board": expected_base, "app": expected_app,
    "bytes": len(image), "sha256": hashlib.sha256(image).hexdigest(),
    "backup_sha256": hashlib.sha256(backup).hexdigest(),
}
print(json.dumps(receipt, indent=2), flush=True)
# Keep download mode throughout installation: a launcher must not replace ota_1
# between identity checks, image write and boot selection.
# Confirm identity again from the live device before any mutation.
if args.expected_mac:
    require(re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}",args.expected_mac),"Invalid expected MAC")
    probe=subprocess.run([sys.executable,"-m","esptool","--port",args.port,"--after","no-reset","read-mac"],check=True,capture_output=True,text=True)
    found=re.findall(r"MAC:\s*([0-9a-fA-F:]{17})",probe.stdout)
    require(found and all(x.lower()==args.expected_mac.lower() for x in found),"Wrong physical device; no write")
live_header = args.slot_backup.with_name(args.slot_backup.stem + "-live-header.bin")
subprocess.run([sys.executable, "-m", "esptool", "--port", args.port, "--after", "no-reset",
                "read-flash", hex(base_offset), "0x1000", str(live_header)], check=True)
require(name(live_header.read_bytes(), 80) == expected_base, "Live board identity mismatch")
live_table=live_header.with_name(live_header.stem+"-partitions.bin")
subprocess.run([sys.executable,"-m","esptool","--port",args.port,"--after","no-reset",
                "read-flash","0x8000","0x1000",str(live_table)],check=True)
require(live_table.read_bytes()==metadata[0x8000:0x9000],"Live partition table changed; no write")
live_slot = live_header.with_name(live_header.stem + "-slot1.bin")
subprocess.run([sys.executable, "-m", "esptool", "--port", args.port, "--after", "no-reset",
                "read-flash", hex(app_offset), "0x1000", str(live_slot)], check=True)
live_secondary = live_slot.read_bytes()
require(live_secondary == backup[:4096] or
        (args.update and name(live_secondary, 80) == expected_app),
        "Slot backup does not match the live device")
subprocess.run([sys.executable, "-m", "esptool", "--port", args.port, "--no-stub",
                "--after", "no-reset", "--baud", "460800", "write-flash", hex(app_offset), str(args.image)], check=True)
subprocess.run([sys.executable, str(Path(__file__).with_name("select_slot_usb.py")),
                args.port, "--slot", "1"], check=True)
args.image.with_suffix(".usb-install.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
