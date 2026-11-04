"""Build the "deck" folder that goes on the watches' microSD: the Watch Deck installer for each OS.

    python make_sd_bundle.py [OUTPUT]     default: dist/sd/deck

    WatchDeck.exe          Windows (build first with build_exe.ps1)
    WatchDeck-macos.zip    agent + install_macos.command
    WatchDeck-linux.zip    agent + install.sh
    LEGGIMI.txt, version.txt

Remote (Agente PC > Installa sul PC) opens the microSD on the PC as a USB drive; the user runs
the installer. Copy the folder to the root of the card as "deck".
"""
from __future__ import annotations

import shutil
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
AGENT = ["deck_agent.py", "deck_config.py", "deck_dev.py", "deck_ui.py", "requirements.txt"]


def version() -> str:
    for line in (HERE / "deck_agent.py").read_text(encoding="utf-8").splitlines():
        if line.startswith("VERSION = "):
            return line.split('"')[1]
    raise SystemExit("VERSION not found in deck_agent.py")


def add(z: zipfile.ZipFile, source: Path, name: str, executable: bool = False) -> None:
    info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (0o100755 if executable else 0o100644) << 16  # Finder/unzip keep the x bit
    z.writestr(info, source.read_bytes())


def bundle(out: Path) -> None:
    exe = HERE / "dist" / "WatchDeck.exe"
    if not exe.is_file():
        raise SystemExit("Missing dist/WatchDeck.exe: run build_exe.ps1 first")
    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(exe, out / "WatchDeck.exe")
    readme = HERE / "installers" / "LEGGIMI.txt"
    for system, script, name in (("macos", "install_macos.command", "install_macos.command"),
                                 ("linux", "install_linux.sh", "install.sh")):
        with zipfile.ZipFile(out / f"WatchDeck-{system}.zip", "w") as z:
            for f in AGENT:
                add(z, HERE / f, f)
            add(z, HERE / "installers" / script, name, executable=True)
            add(z, readme, "LEGGIMI.txt")
    shutil.copy2(readme, out / "LEGGIMI.txt")
    (out / "version.txt").write_text(version() + "\n", encoding="utf-8")
    for f in sorted(out.iterdir()):
        print(f"{f.name:22} {f.stat().st_size:>10} byte")


if __name__ == "__main__":
    bundle(Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "dist" / "sd" / "deck")
