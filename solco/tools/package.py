"""Package external-app binaries and source fingerprints for USB/SD testing."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import zipfile

parser=argparse.ArgumentParser()
parser.add_argument("--allow-compiled-knob",action="store_true")
args=parser.parse_args()
root = Path(__file__).resolve().parents[3]
shared = root / "watch-os-common/solco"
output = root / "release-output/solco/0.1.0-dev.8"
output.mkdir(parents=True, exist_ok=True)
fingerprints = {
    str(path.relative_to(shared)).replace("\\", "/"): hashlib.sha256(path.read_bytes()).hexdigest()
    for path in sorted(list((shared / "main").glob("*")) + list((shared / "assets").glob("*")) + list((shared / "third_party").rglob("*"))) if path.is_file()
}
for folder, board in (("ESP32-S3-Amoled-Touch-1.8", "amoled"), ("ESP32-S3-Knob-Touch-LCD-1.8", "knob"), ("SenseCap-Watcher-W1-A", "watcher")):
    app = root / folder / "external_apps/solco"
    source = app / "build" / f"solco_{board}.bin"
    binary = source.read_bytes()
    expected = f"solco_{board}".encode()
    if binary[80:112].split(b"\0", 1)[0] != expected:
        raise SystemExit(f"Wrong binary identity: {source}")
    target = output / board
    package = target / "Solco"
    package.mkdir(parents=True, exist_ok=True)
    (package / "Solco.bin").write_bytes(binary)
    shutil.copyfile(app / "manifest.json", package / "manifest.json")
    shutil.copyfile(shared / "README.md", target / "README.md")
    shutil.copyfile(shared / "assets/icon.png", package / "icon.png")
    shutil.copyfile(shared / "TEST-REPORT.md", target / "TEST-REPORT.md")
    screenshot = root / "progress-files" / f"solco-{board}-dev8-mixer.png"
    if screenshot.exists():
        shutil.copyfile(screenshot, target / "preview.png")
    shutil.copyfile(shared / "TEST-REPORT-dev1.md", target / "TEST-REPORT-dev1.md")
    if binary[48:80].split(b"\0", 1)[0] != b"0.1.0-dev.8":
        raise SystemExit(f"Wrong binary version: {source}")
    shutil.copyfile(shared / "TEST-REPORT-dev2.md", target / "TEST-REPORT-dev2.md")
    shutil.copyfile(shared / "TEST-REPORT-dev3.md", target / "TEST-REPORT-dev3.md")
    shutil.copyfile(shared / "TEST-REPORT-dev4.md", target / "TEST-REPORT-dev4.md")
    shutil.copyfile(shared / "TEST-REPORT-dev5.md", target / "TEST-REPORT-dev5.md")
    shutil.copyfile(shared / "TEST-REPORT-dev6.md", target / "TEST-REPORT-dev6.md")
    shutil.copyfile(shared / "TEST-REPORT-dev7.md", target / "TEST-REPORT-dev7.md")
    for license_file in (shared / "third_party/minimp3").glob("*"):
        if license_file.name in ("LICENSE", "SOURCE.txt"):
            shutil.copyfile(license_file, target / ("minimp3-" + license_file.name))
    library = root / "progress-files" / f"solco-{board}-dev4-library.png"
    if library.exists():
        shutil.copyfile(library, target / "library.png")
    detail = root / "progress-files" / f"solco-{board}-dev8-vinyl.png"
    if detail.exists():
        shutil.copyfile(detail, target / "vinyl.png")
    for page in ("drumpad","keys","motion","rates"):
        screenshot=root / "progress-files" / f"solco-{board}-dev8-{page}.png"
        if screenshot.exists(): shutil.copyfile(screenshot,target / (page+".png"))
    commands = json.loads((app / "build/compile_commands.json").read_text(encoding="utf-8"))
    audio = next(item["command"] for item in commands if item["file"].replace("\\", "/").endswith("/hardware/audio.c"))
    if "-DWATCH_AUDIO_TX_DMA_FRAMES=128" not in audio:
        raise SystemExit("Solco low-latency audio profile missing")
    volume = root / "progress-files" / f"solco-{board}-dev6-volume.png"
    if volume.exists():
        shutil.copyfile(volume, target / "volume.png")
    installed=json.loads(source.with_suffix(".usb-install.json").read_text(encoding="utf-8"))
    exact_usb=installed["sha256"]==hashlib.sha256(binary).hexdigest()
    if not exact_usb and not (board=="knob" and args.allow_compiled_knob):
        raise SystemExit("Package differs from the USB test image")
    shutil.copyfile(source.with_suffix(".usb-install.json"),target / ("usb-install.json" if exact_usb else "previous-usb-install.json"))
    validation="Exact binary installed and tested over USB" if exact_usb else "Compiled; preceding USB variant passed. Latest changes: generic unsupported-IMU caption and enlarged sixteen-page loop reserve. Knob disconnected before final reinstall."
    print(board,validation)
    if board=="watcher":
        engine=next(item["command"] for item in commands if item["file"].replace("\\","/").endswith("/engine.c"))
        if "-DSOLCO_LOOP_EDGE_PAGES=16" not in engine: raise SystemExit("Watcher loop reserve missing")
    if board=="amoled":
        sensor=next(item["command"] for item in commands if item["file"].replace("\\","/").endswith("/qmi8658.c"))
        if "-DWATCH_QMI8658_ODR=6" not in sensor or "-DWATCH_QMI8658_RECOVER_SAME_PROFILE=1" not in sensor: raise SystemExit("Studio IMU profile missing")
    receipt = {
        "validation":validation, "version": "0.1.0-dev.8", "project": expected.decode(), "board": folder,
        "output_channels": 1 if board in ("amoled","watcher") else 2,
        "bytes": len(binary), "sha256": hashlib.sha256(binary).hexdigest(),
        "sdkconfig_sha256": hashlib.sha256((app / "build/config/sdkconfig.h").read_bytes()).hexdigest(),
        "audio_dma": {"frames": 128, "descriptors": 6, "capacity_ms_at_44100": 128 * 6 * 1000 / 44100},
        "hardware_audio_sha256": hashlib.sha256((root / folder / "components/hardware/audio.c").read_bytes()).hexdigest(),
        "wrapper_cmake_sha256": hashlib.sha256((app / "main/CMakeLists.txt").read_bytes()).hexdigest(),
        "preview": "dev.8 mixer, full-screen vinyl and Studio where present; historical dev.4 library / dev.6 volume",
        "radio": "Local Studio: no BLE, Wi-Fi or ESP-NOW started",
        "studio": {"banks":34,"voices":16,"steps":16,"pattern":"microSD","kits":"A original / B revised kick and tom"},
        "hardware_board_sha256": hashlib.sha256((root / folder / "components/hardware/board.c").read_bytes()).hexdigest(),
        "cache_frames_per_deck": 131072,
        "loop_edge_frames_per_deck":16384,
        "loop_staging_frames_per_deck":16384,
        "transport_rates_q16":[-131072,-98304,-65536,-32768,32768,65536,98304,131072],
        "platter": "full width, cached static grooves, partial marker redraw",
        "usb_tools": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in sorted((shared / "tools").glob("*.py"))},
        "qmi8658_sha256": hashlib.sha256((root / "watch-os-common/components/board_drivers/qmi8658.c").read_bytes()).hexdigest(),
        "sources": fingerprints, "status": "experimental; consult TEST-REPORT.md for actual validation",
    }
    (target / "build.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    with zipfile.ZipFile(output / f"Solco-{board}-0.1.0-dev.8.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for item in sorted(target.rglob("*")):
            if item.is_file(): archive.write(item, item.relative_to(target))
    print(expected.decode(), len(binary), receipt["sha256"])
