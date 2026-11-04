"""Watch Deck: the Deck agent as a Windows tray application (WatchDeck.exe).

    WatchDeck.exe              run in the tray (installs itself on first launch)
    WatchDeck.exe --ui         the Watch Deck window (status, quick launch, own shortcuts)
    WatchDeck.exe --install    copy to %LOCALAPPDATA%\\Programs\\WatchDeck, Start menu, start with Windows
    WatchDeck.exe --uninstall  remove all of it (also from Settings > Apps)

The tray icon is green while a watch is connected (USB or Bluetooth), grey
while searching. Its menu shows the device and the touchbar in use, moves the
watch between USB and Bluetooth, toggles start with Windows, opens the log.
Built by build_exe.ps1 (PyInstaller); runs as plain Python too.
"""

from __future__ import annotations

import asyncio
import ctypes
import os
import shutil
import subprocess
import sys
import threading
import winreg

import pystray
from PIL import Image, ImageDraw

import deck_agent

APP = "WatchDeck"
NAME = "Watch Deck"
VERSION = "1.5.0"
INSTALL_DIR = os.path.join(os.environ["LOCALAPPDATA"], "Programs", APP)
INSTALLED_EXE = os.path.join(INSTALL_DIR, APP + ".exe")
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
UNINSTALL_KEY = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\\" + APP
START_MENU = os.path.join(os.environ["APPDATA"], r"Microsoft\Windows\Start Menu\Programs", NAME + ".lnk")
FROZEN = getattr(sys, "frozen", False)

# ------------------------------------------------------------------ installation


def fresh_env() -> dict:
    """Environment for starting WatchDeck.exe again from inside it. A PyInstaller one-file program
    passes its unpack folder (_MEI...) to its children through these variables: the new process then
    ran from the old one's folder, which the old one deleted on exit ("No module named '_sqlite3'"
    after the automatic restart of 04/11 21:01). Reset: the new process unpacks its own copy."""
    env = {k: v for k, v in os.environ.items() if not k.startswith(("_PYI_", "_MEIPASS"))}
    env["PYINSTALLER_RESET_ENVIRONMENT"] = "1"
    return env


def message(text: str, flags: int = 0x40) -> int:
    return ctypes.windll.user32.MessageBoxW(None, text, NAME, flags)


def shortcut(path: str, target: str) -> None:
    script = (
        f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{path}');"
        f"$s.TargetPath='{target}';$s.WorkingDirectory='{os.path.dirname(target)}';$s.Save()"
    )
    subprocess.run(["powershell", "-NoProfile", "-Command", script], creationflags=0x08000000, check=False)


def autostart(enabled: bool | None = None) -> bool:
    """Read, or set, start with Windows (HKCU Run)."""
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, RUN_KEY, 0, winreg.KEY_READ | winreg.KEY_SET_VALUE) as key:
        if enabled is True:
            winreg.SetValueEx(key, APP, 0, winreg.REG_SZ, f'"{INSTALLED_EXE if FROZEN else sys.executable}"')
        elif enabled is False:
            try:
                winreg.DeleteValue(key, APP)
            except FileNotFoundError:
                pass
        try:
            winreg.QueryValueEx(key, APP)
            return True
        except FileNotFoundError:
            return False


QUIT_EVENT = "Local\\WatchOsDeckAgentQuit"


def stop_running(timeout: float = 25) -> bool:
    """Ask a running agent to quit like "Esci" does (it gives the Deck service back to Windows).
    Never taskkill: a killed agent leaves the watches' GATT service busy until bthserv restarts."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    event = kernel32.CreateEventW(None, True, False, QUIT_EVENT)
    kernel32.SetEvent(event)
    deadline = threading.Event()
    waited = 0.0
    while waited < timeout:
        mutex = kernel32.OpenMutexW(0x00100000, False, "Local\\WatchOsDeckAgent")  # SYNCHRONIZE
        if not mutex:
            kernel32.ResetEvent(event)
            kernel32.CloseHandle(event)
            return True
        kernel32.CloseHandle(mutex)
        deadline.wait(0.25)
        waited += 0.25
    kernel32.ResetEvent(event)
    kernel32.CloseHandle(event)
    return False


def install() -> None:
    if not FROZEN:
        raise SystemExit("--install works from WatchDeck.exe (build_exe.ps1); as a script use install_windows.ps1")
    os.makedirs(INSTALL_DIR, exist_ok=True)
    if os.path.normcase(sys.executable) != os.path.normcase(INSTALLED_EXE):
        if not stop_running():
            message(f"{NAME} in esecuzione non si chiude: usa Esci dal menu dell'icona e riprova.", 0x30)
            raise SystemExit(1)
        for attempt in range(120):  # the old exe's launcher process lets go of the file a moment later
            try:
                shutil.copy2(sys.executable, INSTALLED_EXE)
                break
            except PermissionError:
                if attempt == 119:
                    raise
                threading.Event().wait(0.25)
    shortcut(START_MENU, INSTALLED_EXE)
    autostart(True)
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY) as key:
        for name, value in (
            ("DisplayName", NAME),
            ("DisplayVersion", VERSION),
            ("Publisher", "Watch OS"),
            ("DisplayIcon", INSTALLED_EXE),
            ("InstallLocation", INSTALL_DIR),
            ("UninstallString", f'"{INSTALLED_EXE}" --uninstall'),
        ):
            winreg.SetValueEx(key, name, 0, winreg.REG_SZ, value)
        winreg.SetValueEx(key, "NoModify", 0, winreg.REG_DWORD, 1)
        winreg.SetValueEx(key, "NoRepair", 0, winreg.REG_DWORD, 1)


def uninstall() -> None:
    autostart(False)
    for path in (START_MENU,):
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
    try:
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY)
    except FileNotFoundError:
        pass
    stop_running()  # politely: never taskkill (see stop_running)
    # The running exe cannot delete itself: a detached shell removes the folder once it exits.
    subprocess.Popen(f'cmd /c ping -n 3 127.0.0.1 >nul & rmdir /s /q "{INSTALL_DIR}"',
                     creationflags=0x08000000 | 0x00000008, close_fds=True)
    message(f"{NAME} e stato disinstallato.")


# ------------------------------------------------------------------ tray


def icon_image(connected: bool) -> Image.Image:
    size = 64
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    color = (74, 222, 128, 255) if connected else (148, 163, 184, 255)
    draw.rounded_rectangle((4, 4, 60, 60), radius=14, fill=(17, 24, 39, 255), outline=color, width=5)
    for row in range(2):  # a 2x2 key grid: a deck
        for col in range(2):
            x, y = 16 + col * 18, 16 + row * 18
            draw.rounded_rectangle((x, y, x + 13, y + 13), radius=3, fill=color)
    return image


class Tray:
    def __init__(self) -> None:
        self.loop: asyncio.AbstractEventLoop | None = None
        self.connected = False
        self.icon = pystray.Icon(APP, icon_image(False), NAME, self.menu())

    def status(self, _=None) -> str:
        s = deck_agent.STATUS
        return f"Collegato: {s['device']} ({s['via']})" if s["connected"] else "In attesa del watch..."

    def touchbar(self, _=None) -> str:
        app = deck_agent.STATUS["app"]
        return f"Touchbar: {app}" if deck_agent.STATUS["connected"] and app else "Touchbar: -"

    def send(self, command: dict) -> None:
        if self.loop is not None and deck_agent.COMMANDS is not None:
            self.loop.call_soon_threadsafe(deck_agent.COMMANDS.put_nowait, command)

    def menu(self) -> pystray.Menu:  # noqa: C901 - one flat menu
        connected = lambda _: deck_agent.STATUS["connected"]
        return pystray.Menu(
            pystray.MenuItem("Apri Watch Deck", self.open_window, default=True),
            pystray.MenuItem(self.status, None, enabled=False),
            pystray.MenuItem(self.touchbar, None, enabled=False),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Passa il watch a USB", lambda: self.send({"t": "cmd", "c": "usb"}),
                             enabled=lambda _: connected(_) and deck_agent.STATUS["via"] != "USB"),
            pystray.MenuItem("Passa il watch a Bluetooth", lambda: self.send({"t": "cmd", "c": "ble"}),
                             enabled=lambda _: connected(_) and deck_agent.STATUS["via"] == "USB"),
            pystray.Menu.SEPARATOR,
            pystray.MenuItem("Avvia con Windows", lambda: autostart(not autostart()), checked=lambda _: autostart()),
            pystray.MenuItem("Apri il log", lambda: os.startfile(deck_agent.log_to_file())),
            pystray.MenuItem(f"Esci da {NAME}", self.quit),
        )

    def open_window(self) -> None:
        """Settings window in its own process: tkinter and the tray loop never share a thread."""
        command = [sys.executable, "--ui"] if FROZEN else [sys.executable, os.path.abspath(__file__), "--ui"]
        subprocess.Popen(command, close_fds=True, env=fresh_env())

    def watch_status(self) -> None:
        """Repaint the icon when the connection changes (pystray polls nothing by itself)."""
        while True:
            state = deck_agent.STATUS["connected"]
            if state != self.connected:
                self.connected = state
                self.icon.icon = icon_image(state)
                self.icon.title = f"{NAME} - {self.status()}"
                self.icon.update_menu()
            threading.Event().wait(1)

    def agent(self) -> None:
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        while True:  # the agent must never end while the icon is still in the tray
            try:
                self.loop.run_until_complete(deck_agent.run())
            except BaseException as error:  # logged, then the agent starts over
                print(f"Agente fermato ({type(error).__name__}: {error}): riparto")
                import traceback

                traceback.print_exc()
            threading.Event().wait(3)

    def quit_requests(self) -> None:
        """A new WatchDeck.exe being installed asks this one to quit (instead of killing it)."""
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        event = kernel32.CreateEventW(None, True, False, "Local\\WatchOsDeckAgentQuit")
        kernel32.WaitForSingleObject(event, 0xFFFFFFFF)
        print("Chiusura richiesta dal nuovo WatchDeck")
        self.quit()

    def quit(self) -> None:
        self.icon.stop()
        if self.loop is not None:  # give the watches' Deck service back to Windows first
            try:
                asyncio.run_coroutine_threadsafe(deck_agent.shutdown(), self.loop).result(timeout=14)
            except Exception:
                pass
        os._exit(0)  # the agent loop and radio threads end with the process

    def restart(self) -> None:
        """A new agent process (it waits until this one is gone), then this one ends."""
        command = [sys.executable, "--restart"] if FROZEN else [sys.executable, os.path.abspath(__file__), "--restart"]
        subprocess.Popen(command, creationflags=0x00000008 | 0x00000200, close_fds=True,  # detached, new group
                         env=fresh_env())
        try:
            self.icon.stop()
        except Exception:
            pass
        os._exit(3)

    def run(self) -> None:
        deck_agent.NOTIFIER = lambda title, text: self.icon.notify(text, title)
        deck_agent.RESTARTER = self.restart
        threading.Thread(target=self.agent, daemon=True).start()
        threading.Thread(target=self.watch_status, daemon=True).start()
        threading.Thread(target=self.quit_requests, daemon=True).start()
        self.icon.run()


def main() -> None:
    if "--ui" in sys.argv:
        import deck_ui

        deck_ui.main()
        return
    deck_agent.log_to_file()
    if "--uninstall" in sys.argv:
        uninstall()
        return
    if "--install" in sys.argv or (
        FROZEN
        and os.path.normcase(sys.executable) != os.path.normcase(INSTALLED_EXE)
        and message(f"Installare {NAME}?\n\nResta nella barra di Windows, si avvia con il PC e collega "
                    "il watch via USB o Bluetooth.", 0x24) == 6  # Yes
    ):
        install()
        subprocess.Popen([INSTALLED_EXE], creationflags=0x00000008, close_fds=True, env=fresh_env())
        return
    if "--restart" in sys.argv:  # started by a stuck agent: wait until it has gone
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        for _ in range(120):
            mutex = kernel32.OpenMutexW(0x00100000, False, "Local\\WatchOsDeckAgent")
            if not mutex:
                break
            kernel32.CloseHandle(mutex)
            threading.Event().wait(0.25)
        print("Agente riavviato dopo un blocco")
    if not deck_agent.single_instance():
        message(f"{NAME} e gia in esecuzione: cerca l'icona nella barra di Windows.")
        return
    Tray().run()


if __name__ == "__main__":
    main()
