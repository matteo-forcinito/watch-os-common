"""Deck agent: tells the Remote app on the watch what the PC is doing.

The watch stays a Bluetooth keyboard/mouse (HID); this agent only opens the
Deck GATT service on the same bond (hid_link/hid_deck.c) and exchanges small
JSON messages: focused application, what is playing, CPU/RAM, microphone, and
a short allowlist of actions (lock, display off, microphone, task manager).

    pip install bleak psutil
    optional on Windows: pip install pycaw winrt-Windows.Media.Control winrt-Windows.Foundation
    optional on Linux:   xdotool, playerctl, pactl (PipeWire/PulseAudio)

    python deck_agent.py [--name "AMOLED Remote"] [--selftest]

Pair the watch from the system Bluetooth settings first (Remote > Bluetooth).
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import subprocess
import sys

from bleak import BleakClient, BleakScanner

SERVICE = "7a3e0000-5c1d-4e8b-9f2a-d3c0de0decc0"
RX = "7a3e0001-5c1d-4e8b-9f2a-d3c0de0decc0"  # PC -> watch
TX = "7a3e0002-5c1d-4e8b-9f2a-d3c0de0decc0"  # watch -> PC
MAX_BYTES = 240  # HID_DECK_MAX on the watch
WINDOWS = sys.platform == "win32"

try:
    import psutil
except ImportError:  # CPU/RAM are then simply not shown
    psutil = None


def clean(text: str, limit: int) -> str:
    return "".join(c for c in (text or "") if c >= " ")[:limit]


def encode(message: dict) -> bytes:
    """Compact JSON within MAX_BYTES, trimming the longest text field if needed."""
    while True:
        data = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode()
        if len(data) <= MAX_BYTES:
            return data
        key = max((k for k, v in message.items() if isinstance(v, str) and k != "t"), key=lambda k: len(message[k]))
        # A character is 1..4 UTF-8 bytes: cut at least a quarter of the excess per round.
        message[key] = message[key][: -max(1, (len(data) - MAX_BYTES + 3) // 4)]


# ---------------------------------------------------------------- Windows

if WINDOWS:
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    def foreground() -> tuple[str, str]:
        hwnd = user32.GetForegroundWindow()
        if not hwnd:
            return "", ""
        title = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, title, 256)
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        app = ""
        process = kernel32.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
        if process:
            path = ctypes.create_unicode_buffer(512)
            size = wintypes.DWORD(512)
            if kernel32.QueryFullProcessImageNameW(process, 0, path, ctypes.byref(size)):
                app = os.path.splitext(os.path.basename(path.value))[0].lower()
            kernel32.CloseHandle(process)
        return app, title.value

    async def now_playing() -> dict | None:
        try:
            from winrt.windows.media.control import (
                GlobalSystemMediaTransportControlsSessionManager as Manager,
            )
        except ImportError:
            return None
        session = (await Manager.request_async()).get_current_session()
        if session is None:
            return {"title": "", "artist": "", "playing": 0}
        props = await session.try_get_media_properties_async()
        playing = session.get_playback_info().playback_status == 4  # Playing
        return {"title": props.title or "", "artist": props.artist or "", "playing": int(playing)}

    def _mic():
        try:
            from pycaw.pycaw import AudioUtilities
        except ImportError:
            return None
        device = AudioUtilities.GetMicrophone()
        if device is None:
            return None
        from ctypes import POINTER, cast

        from comtypes import CLSCTX_ALL
        from pycaw.pycaw import IAudioEndpointVolume

        interface = device.Activate(IAudioEndpointVolume._iid_, CLSCTX_ALL, None)
        return cast(interface, POINTER(IAudioEndpointVolume))

    def mic_state() -> int:
        try:
            volume = _mic()
            return -1 if volume is None else int(not volume.GetMute())
        except OSError:
            return -1

    def mic_toggle() -> str:
        volume = _mic()
        if volume is None:
            return "Installa pycaw sul PC"
        volume.SetMute(not volume.GetMute(), None)
        return "Microfono muto" if volume.GetMute() else "Microfono attivo"

    def lock() -> str:
        user32.LockWorkStation()
        return "PC bloccato"

    def display_off() -> str:
        user32.PostMessageW(0xFFFF, 0x0112, 0xF170, 2)  # HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, off
        return "Schermo spento"

    def taskmgr() -> str:
        subprocess.Popen(["taskmgr"])
        return "Gestione attivita"

# ---------------------------------------------------------------- Linux

else:

    def _run(*args: str) -> str:
        if shutil.which(args[0]) is None:
            return ""
        try:
            return subprocess.run(args, capture_output=True, text=True, timeout=2).stdout.strip()
        except (OSError, subprocess.SubprocessError):
            return ""

    def foreground() -> tuple[str, str]:
        pid = _run("xdotool", "getactivewindow", "getwindowpid")
        title = _run("xdotool", "getactivewindow", "getwindowname")
        app = ""
        if pid.isdigit():
            try:
                with open(f"/proc/{pid}/comm", encoding="utf-8") as f:
                    app = f.read().strip().lower()
            except OSError:
                pass
        return app, title

    async def now_playing() -> dict | None:
        out = _run("playerctl", "metadata", "--format", "{{status}}\t{{title}}\t{{artist}}")
        if not out:
            return None
        status, _, rest = out.partition("\t")
        title, _, artist = rest.partition("\t")
        return {"title": title, "artist": artist, "playing": int(status == "Playing")}

    def mic_state() -> int:
        out = _run("pactl", "get-source-mute", "@DEFAULT_SOURCE@")
        return -1 if not out else int(not out.endswith("yes"))

    def mic_toggle() -> str:
        _run("pactl", "set-source-mute", "@DEFAULT_SOURCE@", "toggle")
        return "Microfono muto" if mic_state() == 0 else "Microfono attivo"

    def lock() -> str:
        _run("loginctl", "lock-session")
        return "PC bloccato"

    def display_off() -> str:
        _run("xset", "dpms", "force", "off")
        return "Schermo spento"

    def taskmgr() -> str:
        for tool in ("gnome-system-monitor", "plasma-systemmonitor", "ksysguard"):
            if shutil.which(tool):
                subprocess.Popen([tool])
                return "Monitor di sistema"
        return "Nessun monitor di sistema"


ACTIONS = {"lock": lock, "display_off": display_off, "mic": mic_toggle, "taskmgr": taskmgr}


# ---------------------------------------------------------------- link
#
# Windows keeps the paired watch connected for HID and does not report its
# advertisements, so the agent opens the paired device directly through WinRT
# and shares that connection. Elsewhere bleak connects by name.


class WinLink:
    """Deck service of a paired watch, over the connection Windows already holds."""

    def __init__(self, loop, on_receive, on_lost):
        self.loop, self.on_receive, self.on_lost = loop, on_receive, on_lost
        self.device = self.rx = self.tx = None
        self.tokens = []

    async def open(self, name: str) -> str | None:
        from winrt.windows.devices.bluetooth import BluetoothCacheMode, BluetoothConnectionStatus, BluetoothLEDevice
        from winrt.windows.devices.bluetooth.genericattributeprofile import (
            GattClientCharacteristicConfigurationDescriptorValue as Cccd,
        )
        from winrt.windows.devices.enumeration import DeviceInformation

        selector = BluetoothLEDevice.get_device_selector_from_pairing_state(True)
        for info in await DeviceInformation.find_all_async_aqs_filter(selector):
            if name.lower() not in info.name.lower():
                continue
            device = await BluetoothLEDevice.from_id_async(info.id)
            if device is None or device.connection_status != BluetoothConnectionStatus.CONNECTED:
                continue
            services = await device.get_gatt_services_async(BluetoothCacheMode.UNCACHED)
            service = next((s for s in services.services if str(s.uuid) == SERVICE), None)
            if service is None:
                continue  # an older Remote without the Deck service
            chars = (await service.get_characteristics_async(BluetoothCacheMode.UNCACHED)).characteristics
            by_uuid = {str(c.uuid): c for c in chars}
            self.device, self.rx, self.tx = device, by_uuid[RX], by_uuid[TX]
            self.tokens.append(self.tx.add_value_changed(self._changed))
            self.tokens.append(device.add_connection_status_changed(self._status))
            await self.tx.write_client_characteristic_configuration_descriptor_with_result_async(Cccd.NOTIFY)
            return info.name
        return None

    def _changed(self, _, args) -> None:
        data = bytes(args.characteristic_value)
        self.loop.call_soon_threadsafe(self.on_receive, data)

    def _status(self, device, _) -> None:
        from winrt.windows.devices.bluetooth import BluetoothConnectionStatus

        if device.connection_status != BluetoothConnectionStatus.CONNECTED:
            self.loop.call_soon_threadsafe(self.on_lost)

    async def send(self, data: bytes) -> None:
        from winrt.windows.devices.bluetooth.genericattributeprofile import GattCommunicationStatus, GattWriteOption
        from winrt.windows.storage.streams import DataWriter

        writer = DataWriter()
        writer.write_bytes(data)
        result = await self.rx.write_value_with_result_async(writer.detach_buffer(), GattWriteOption.WRITE_WITH_RESPONSE)
        if result.status != GattCommunicationStatus.SUCCESS:
            raise OSError(f"scrittura GATT: stato {result.status}")

    async def close(self) -> None:
        if self.device is not None:
            self.device.close()
            self.device = None


class BleakLink:
    def __init__(self, loop, on_receive, on_lost):
        self.loop, self.on_receive, self.on_lost = loop, on_receive, on_lost
        self.client = None

    async def open(self, name: str) -> str | None:
        device = await BleakScanner.find_device_by_filter(
            lambda d, adv: name.lower() in (d.name or adv.local_name or "").lower() or SERVICE in (adv.service_uuids or []),
            timeout=10,
        )
        if device is None:
            return None
        self.client = BleakClient(device, disconnected_callback=lambda _: self.loop.call_soon_threadsafe(self.on_lost))
        await self.client.connect()
        await self.client.start_notify(TX, lambda _, data: self.on_receive(bytes(data)))
        return device.name or device.address

    async def send(self, data: bytes) -> None:
        await self.client.write_gatt_char(RX, data, response=True)

    async def close(self) -> None:
        if self.client is not None:
            await self.client.disconnect()
            self.client = None


# Contexts and the touchbar the watch must choose for each (--selftest).
SELFTEST = [
    ("code", "main.c - ESP-IDF - Visual Studio Code", "VS Code"),
    ("chrome", "Gmail - Google Chrome", "Browser"),
    ("msedge", "Lofi beats - YouTube - Microsoft Edge", "YouTube"),
    ("explorer", "Documenti", "File"),
    ("winword", "Relazione.docx - Word", "Office"),
    ("windowsterminal", "PowerShell", "Terminale"),
    ("spotify", "Spotify Premium", "Spotify"),
    ("ms-teams", "Riunione | Microsoft Teams", "Teams"),
    ("zoom", "Zoom Meeting", "Zoom"),
    ("discord", "#generale - Discord", "Discord"),
    ("notepad", "Senza titolo - Blocco note", "Generale"),
]


async def session(name: str, selftest: bool = False) -> bool:
    loop = asyncio.get_running_loop()
    disconnected = asyncio.Event()
    profiles: asyncio.Queue[str] = asyncio.Queue()
    link = None

    def received(data: bytes) -> None:
        try:
            message = json.loads(data.decode())
        except ValueError:
            return
        if message.get("t") == "hello":
            print(f"Watch: {message.get('name', '?')}")
        elif message.get("t") == "prof":
            profiles.put_nowait(message.get("n", ""))
        elif message.get("t") == "act" and message.get("a") in ACTIONS:
            action = ACTIONS[message["a"]]
            print(f"Azione: {message['a']}")

            async def run() -> None:
                try:
                    text = await asyncio.to_thread(action)
                    ok = 1
                except Exception as error:  # report, never crash the agent
                    text, ok = f"Errore: {error}", 0
                await send({"t": "res", "ok": ok, "msg": clean(text, 46)})

            loop.create_task(run())

    async def send(message: dict) -> None:
        await link.send(encode(message))

    link = (WinLink if WINDOWS else BleakLink)(loop, received, disconnected.set)
    found = await link.open(name)
    if found is None:
        return False
    print(f"Collegato a {found}")
    if selftest:
        failed = 0
        for app, title, expected in SELFTEST:
            await send({"t": "ctx", "app": app, "title": title})
            try:
                got = await asyncio.wait_for(profiles.get(), 3)
            except asyncio.TimeoutError:
                got = "(nessuna risposta)"
            failed += got != expected
            print(f"{'OK  ' if got == expected else 'ERR '} {app:16} -> {got} (atteso {expected})")
            await asyncio.sleep(0.8)  # visible on the watch
        await link.close()
        print("Selftest superato" if not failed else f"Selftest: {failed} errori")
        raise SystemExit(1 if failed else 0)
    try:
        last_ctx = last_media = last_sys = None
        tick = 0
        while not disconnected.is_set():
            app, title = foreground()
            ctx = {"t": "ctx", "app": clean(app, 23), "title": clean(title, 63)}
            if ctx != last_ctx and app:
                await send(ctx)
                last_ctx = ctx
                print(f"Contesto: {app} | {title[:50]}")
            if tick % 4 == 0:
                media = await now_playing()
                if media is not None:
                    media = {"t": "media", **media, "title": clean(media["title"], 63), "artist": clean(media["artist"], 47)}
                    if media != last_media:
                        await send(media)
                        last_media = media
                cpu = round(psutil.cpu_percent()) if psutil else -1
                ram = round(psutil.virtual_memory().percent) if psutil else -1
                state = {"t": "sys", "cpu": cpu, "ram": ram, "mic": mic_state()}
                if state != last_sys:
                    await send(state)
                    last_sys = state
            tick += 1
            try:
                await asyncio.wait_for(disconnected.wait(), 0.5)
            except asyncio.TimeoutError:
                pass
    finally:
        await link.close()
    print("Scollegato")
    return True


async def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", default="Remote", help="part of the watch's Bluetooth name")
    parser.add_argument("--selftest", action="store_true", help="check the touchbar chosen for known apps, then exit")
    args = parser.parse_args()
    if sys.stdout is None:  # pythonw (Startup shortcut): log to a file instead of a console
        folder = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "DeckAgent")
        os.makedirs(folder, exist_ok=True)
        sys.stdout = sys.stderr = open(os.path.join(folder, "agent.log"), "a", encoding="utf-8", buffering=1)
    if WINDOWS and not args.selftest:
        kernel32.CreateMutexW(None, False, "Local\\WatchOsDeckAgent")  # held until the process exits
        if ctypes.get_last_error() == 183:  # ERROR_ALREADY_EXISTS
            print("Agente gia in esecuzione.")
            return
    print(f"Cerco \"{args.name}\" tra i dispositivi associati e collegati...")
    waiting = False
    while True:
        try:
            if not await session(args.name, args.selftest) and not waiting:
                print("Non trovato: associa il watch dal Bluetooth del PC e apri Remote (Bluetooth, non USB).")
                waiting = True
        except Exception as error:  # radio errors: retry
            print(f"Collegamento perso: {error}")
            waiting = False
        await asyncio.sleep(3)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
