# Install the Deck agent for the current Windows user: Python packages and a
# Startup shortcut that runs it without a console window at every sign-in.
#   powershell -ExecutionPolicy Bypass -File install_windows.ps1            install and start
#   powershell -ExecutionPolicy Bypass -File install_windows.ps1 -Remove    remove the shortcut
param([switch]$Remove)
$ErrorActionPreference = 'Stop'

$shortcut = Join-Path ([Environment]::GetFolderPath('Startup')) 'Deck agent.lnk'
if ($Remove) {
    Remove-Item -LiteralPath $shortcut -ErrorAction SilentlyContinue
    Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe'" |
        Where-Object { $_.CommandLine -like '*deck_agent.py*' } |
        ForEach-Object { Stop-Process -Id $_.ProcessId }
    Write-Host 'Deck agent removed.'
    return
}

$python = (Get-Command py -ErrorAction SilentlyContinue).Source
if (-not $python) { throw 'Python 3 not found: install it from python.org (with the "py" launcher).' }
& $python -3 -m pip install --quiet --upgrade -r (Join-Path $PSScriptRoot 'requirements.txt')
if ($LASTEXITCODE) { throw 'pip install failed' }

$pythonw = & $python -3 -c "import sys, pathlib; print(pathlib.Path(sys.executable).with_name('pythonw.exe'))"
$agent = Join-Path $PSScriptRoot 'deck_agent.py'
$link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
$link.TargetPath = $pythonw
$link.Arguments = "`"$agent`""
$link.WorkingDirectory = $PSScriptRoot
$link.Description = 'Deck agent for Watch OS Remote'
$link.Save()

Start-Process -FilePath $pythonw -ArgumentList "`"$agent`"" -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
Write-Host "Deck agent installed and running; it starts at every sign-in. Log: $env:LOCALAPPDATA\DeckAgent\agent.log"
