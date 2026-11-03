# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['deck_tray.py'],
    pathex=[],
    binaries=[],
    datas=[],
    hiddenimports=['winrt.windows.devices.bluetooth', 'winrt.windows.devices.bluetooth.genericattributeprofile', 'winrt.windows.devices.enumeration', 'winrt.windows.storage.streams', 'winrt.windows.media.control', 'winrt.windows.foundation', 'winrt.windows.foundation.collections', 'pycaw.pycaw', 'comtypes', 'hid', 'pystray._win32'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='WatchDeck',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=['build/watchdeck.ico'],
)
