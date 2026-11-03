# Build WatchDeck.exe (tray agent, single file, no console) with PyInstaller.
#   powershell -ExecutionPolicy Bypass -File build_exe.ps1
# Output: dist\WatchDeck.exe. Running it offers to install itself (Start menu,
# start with Windows, Settings > Apps entry for removal).
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

py -3 -m pip install --quiet --upgrade -r requirements.txt pyinstaller
if ($LASTEXITCODE) { throw 'pip install failed' }

# Icon: the same 2x2 deck drawn by the tray, as a multi-size .ico
New-Item -ItemType Directory -Force build | Out-Null
py -3 -c "import deck_tray; deck_tray.icon_image(True).save('build/watchdeck.ico', sizes=[(16,16),(32,32),(48,48),(64,64)])"
if ($LASTEXITCODE) { throw 'icon failed' }

$hidden = @(
    'winrt.windows.devices.bluetooth', 'winrt.windows.devices.bluetooth.genericattributeprofile',
    'winrt.windows.devices.enumeration', 'winrt.windows.storage.streams', 'winrt.windows.media.control',
    'winrt.windows.foundation', 'winrt.windows.foundation.collections', 'pycaw.pycaw', 'comtypes', 'hid',
    'pystray._win32'
) | ForEach-Object { '--hidden-import'; $_ }

py -3 -m PyInstaller --noconfirm --clean --onefile --windowed --name WatchDeck `
    --icon build/watchdeck.ico @hidden deck_tray.py
if ($LASTEXITCODE) { throw 'PyInstaller failed' }
Write-Host "Built $PSScriptRoot\dist\WatchDeck.exe"
