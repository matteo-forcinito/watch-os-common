"""Watch Deck settings: quick launch (Deck section "Avvio") and the user's own
shortcuts per application (section "Le mie"), on Windows, macOS and Linux.

The file is plain JSON, edited by the Watch Deck window (deck_ui.py) and read
by the agent, which notices changes and sends them to the watches:

    {"launch": [{"name": "Netflix", "target": "https://www.netflix.com"}, ...],
     "keys": {"code": [{"label": "Terminale", "keys": "ctrl+`"}], ...}}

A launch target is a URL, an installed app ("shell:AppsFolder\\<id>" on
Windows, an .app bundle on macOS, "desktop:<id>" on Linux) or a program path.
Keys are written like "ctrl+shift+p", "win+e", "alt+f4", "cmd+space".
"""

from __future__ import annotations

import glob
import json
import os
import shutil
import subprocess
import sys
import webbrowser

WINDOWS = sys.platform == "win32"
MAC = sys.platform == "darwin"
LAUNCH_MAX = 16  # DECK_LIST_MAX on the watch
KEYS_MAX = 16

if WINDOWS:
    FOLDER = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "WatchDeck")
elif MAC:
    FOLDER = os.path.expanduser("~/Library/Application Support/WatchDeck")
else:
    FOLDER = os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "watchdeck")
PATH = os.path.join(FOLDER, "config.json")
STATUS_PATH = os.path.join(FOLDER, "status.json")  # written by the agent, shown by the window

# Sites offered even when nothing is installed: streaming first, as the Deck is often a remote.
SITES = [
    ("Netflix", "https://www.netflix.com"),
    ("Mediaset Infinity", "https://mediasetinfinity.mediaset.it"),
    ("RaiPlay", "https://www.raiplay.it"),
    ("YouTube", "https://www.youtube.com"),
    ("Prime Video", "https://www.primevideo.com"),
    ("Disney+", "https://www.disneyplus.com"),
    ("Twitch", "https://www.twitch.tv"),
    ("Spotify Web", "https://open.spotify.com"),
    ("Gmail", "https://mail.google.com"),
    ("Google Calendar", "https://calendar.google.com"),
]

# Installed apps put in the first quick launch list when found (names as the Start menu shows them).
FAVOURITES = [
    "Visual Studio Code", "Netflix", "Mediaset Infinity", "Spotify", "Microsoft Edge", "Google Chrome",
    "Firefox", "Esplora file", "File Explorer", "Finder", "Terminale", "Terminal", "Windows Terminal",
    "Microsoft Teams", "Teams", "Discord", "Outlook", "Word", "Excel", "PowerPoint", "Calcolatrice",
    "Calculator", "Impostazioni", "Settings", "System Settings",
]


# ------------------------------------------------------------------ file


def load() -> dict:
    try:
        with open(PATH, encoding="utf-8") as f:
            config = json.load(f)
    except (OSError, ValueError):
        config = {}
    first = not isinstance(config.get("launch"), list)
    if first:
        config["launch"] = default_launch()
    if not isinstance(config.get("keys"), dict):
        config["keys"] = {}
    if first:
        try:
            save(config)  # the agent and the window then share one list
        except OSError:
            pass
    return config


def save(config: dict) -> None:
    os.makedirs(FOLDER, exist_ok=True)
    temporary = PATH + ".tmp"
    with open(temporary, "w", encoding="utf-8") as f:
        json.dump(config, f, ensure_ascii=False, indent=1)
    os.replace(temporary, PATH)  # the agent never reads half a file


def stamp() -> float:
    """Changes when the window saves: the agent compares it every few seconds."""
    try:
        return os.path.getmtime(PATH)
    except OSError:
        return 0.0


# ------------------------------------------------------------------ installed apps


def installed_apps() -> list[dict]:
    """Apps the user can start, as {"name", "target"}, sorted by name."""
    found: dict[str, str] = {}
    try:
        if WINDOWS:
            out = subprocess.run(
                ["powershell", "-NoProfile", "-Command",
                 "Get-StartApps | Select-Object Name, AppID | ConvertTo-Json -Compress"],
                capture_output=True, text=True, timeout=20, creationflags=0x08000000).stdout
            items = json.loads(out or "[]")
            for item in items if isinstance(items, list) else [items]:
                name, app_id = item.get("Name") or "", item.get("AppID") or ""
                if name and app_id and "uninstall" not in name.lower() and "disinstalla" not in name.lower():
                    found.setdefault(name, "shell:AppsFolder\\" + app_id)
        elif MAC:
            for folder in ("/Applications", "/System/Applications", os.path.expanduser("~/Applications")):
                for bundle in glob.glob(os.path.join(folder, "*.app")):
                    found.setdefault(os.path.splitext(os.path.basename(bundle))[0], bundle)
        else:
            folders = [os.path.expanduser("~/.local/share/applications"), "/usr/share/applications",
                       "/var/lib/flatpak/exports/share/applications"]
            for folder in folders:
                for path in glob.glob(os.path.join(folder, "*.desktop")):
                    name, hidden = "", False
                    with open(path, encoding="utf-8", errors="ignore") as f:
                        for line in f:
                            if line.startswith("Name=") and not name:
                                name = line[5:].strip()
                            hidden |= line.strip() in ("NoDisplay=true", "Hidden=true")
                    if name and not hidden:
                        found.setdefault(name, "desktop:" + os.path.basename(path)[:-8])
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return [{"name": n, "target": t} for n, t in sorted(found.items(), key=lambda x: x[0].lower())]


def default_launch() -> list[dict]:
    """First list: favourite apps that are installed, then streaming sites not installed as apps."""
    apps = {a["name"].lower(): a for a in installed_apps()}
    chosen = []
    for name in FAVOURITES:
        app = apps.get(name.lower())
        if app is not None and all(c["name"] != app["name"] for c in chosen):
            chosen.append(app)
    for name, url in SITES[:6]:
        if not any(c["name"].lower() == name.lower() for c in chosen):
            chosen.append({"name": name, "target": url})
    return chosen[:12]


# ------------------------------------------------------------------ actions


def launch(target: str) -> None:
    if target.startswith(("http://", "https://")):
        webbrowser.open(target)
    elif WINDOWS:
        if target.startswith("shell:"):
            subprocess.Popen(["explorer.exe", target], creationflags=0x08000000)
        else:
            os.startfile(target)
    elif MAC:
        subprocess.Popen(["open", target])
    elif target.startswith("desktop:"):
        subprocess.Popen(["gtk-launch", target[8:]])
    else:
        subprocess.Popen([target] if shutil.which(target) else ["xdg-open", target])


_VK = {
    "ctrl": 0x11, "control": 0x11, "shift": 0x10, "alt": 0x12, "win": 0x5B, "cmd": 0x5B, "meta": 0x5B,
    "tab": 0x09, "enter": 0x0D, "invio": 0x0D, "esc": 0x1B, "space": 0x20, "spazio": 0x20,
    "backspace": 0x08, "delete": 0x2E, "del": 0x2E, "canc": 0x2E, "insert": 0x2D, "ins": 0x2D,
    "left": 0x25, "up": 0x26, "right": 0x27, "down": 0x28, "home": 0x24, "end": 0x23,
    "pageup": 0x21, "pgup": 0x21, "pagedown": 0x22, "pgdn": 0x22, "printscreen": 0x2C,
    "+": 0xBB, "plus": 0xBB, "-": 0xBD, "minus": 0xBD, ",": 0xBC, ".": 0xBE, "`": 0xC0,
    "volumeup": 0xAF, "volumedown": 0xAE, "mute": 0xAD, "playpause": 0xB3, "next": 0xB0, "prev": 0xB1,
}
_EXTENDED = {0x25, 0x26, 0x27, 0x28, 0x24, 0x23, 0x21, 0x22, 0x2E, 0x2D, 0x5B}
_MODIFIERS = {"ctrl", "control", "shift", "alt", "win", "cmd", "meta"}


def parse_keys(keys: str) -> list[str]:
    parts = [p.strip().lower() for p in keys.replace(" ", "").split("+") if p.strip()]
    if keys.strip().endswith("++"):
        parts.append("+")
    return parts


def valid_keys(keys: str) -> bool:
    parts = parse_keys(keys)
    if not parts:
        return False
    return all(p in _VK or (len(p) == 1 and p.isalnum()) or (p[:1] == "f" and p[1:].isdigit()) for p in parts)


def press(keys: str) -> None:
    """Press a combination on this PC as if typed: modifiers down, key, all up."""
    parts = parse_keys(keys)
    if not parts:
        raise ValueError("scorciatoia vuota")
    if WINDOWS:
        import ctypes

        user32 = ctypes.WinDLL("user32")

        def code(part: str) -> int:
            if part in _VK:
                return _VK[part]
            if part[:1] == "f" and part[1:].isdigit():
                return 0x6F + int(part[1:])
            if len(part) == 1 and part.isalnum():
                return ord(part.upper())
            raise ValueError(f"tasto sconosciuto: {part}")

        codes = [code(p) for p in parts]
        for vk in codes:
            user32.keybd_event(vk, 0, 1 if vk in _EXTENDED else 0, 0)
        for vk in reversed(codes):
            user32.keybd_event(vk, 0, (1 if vk in _EXTENDED else 0) | 2, 0)
    elif MAC:
        names = {"ctrl": "control down", "control": "control down", "shift": "shift down",
                 "alt": "option down", "cmd": "command down", "win": "command down", "meta": "command down"}
        mods = [names[p] for p in parts if p in _MODIFIERS]
        key = next((p for p in parts if p not in _MODIFIERS), "")
        codes = {"enter": 36, "tab": 48, "space": 49, "esc": 53, "left": 123, "right": 124, "down": 125, "up": 126,
                 "delete": 117, "backspace": 51, "home": 115, "end": 119, "pageup": 116, "pagedown": 121}
        action = f"key code {codes[key]}" if key in codes else f'keystroke "{key}"'
        using = f" using {{{', '.join(mods)}}}" if mods else ""
        subprocess.run(["osascript", "-e", f'tell application "System Events" to {action}{using}'], timeout=5)
    else:
        names = {"ctrl": "ctrl", "control": "ctrl", "shift": "shift", "alt": "alt", "win": "super", "cmd": "super",
                 "meta": "super", "esc": "Escape", "enter": "Return", "invio": "Return", "space": "space",
                 "tab": "Tab", "left": "Left", "right": "Right", "up": "Up", "down": "Down", "delete": "Delete",
                 "backspace": "BackSpace", "home": "Home", "end": "End", "pageup": "Prior", "pagedown": "Next"}
        combo = "+".join(names.get(p, p.upper() if p[:1] == "f" and p[1:].isdigit() else p) for p in parts)
        subprocess.run(["xdotool", "key", combo], timeout=5)
