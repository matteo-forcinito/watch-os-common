#!/bin/sh
# Watch Deck for macOS: double-click in Finder (or run in Terminal). Installs the agent for the
# current user and starts it at every login (LaunchAgent). Run with --remove to uninstall.
# The first time macOS asks to allow Bluetooth and Accessibility for Python: allow both.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST="$HOME/Library/Application Support/WatchDeck"
PLIST="$HOME/Library/LaunchAgents/it.watchos.watchdeck.plist"

if [ "$1" = "--remove" ]; then
    launchctl unload "$PLIST" 2>/dev/null || true
    rm -f "$PLIST"
    rm -rf "$DEST"
    echo "Watch Deck rimosso."
    exit 0
fi

command -v python3 >/dev/null || { echo "Serve python3 (xcode-select --install oppure python.org)."; exit 1; }
mkdir -p "$DEST/agent"
cp "$HERE"/*.py "$HERE/requirements.txt" "$DEST/agent/"
python3 -m venv "$DEST/venv"
"$DEST/venv/bin/pip" install --quiet --upgrade pip
"$DEST/venv/bin/pip" install --quiet -r "$DEST/agent/requirements.txt"

mkdir -p "$(dirname "$PLIST")"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>it.watchos.watchdeck</string>
  <key>ProgramArguments</key><array>
    <string>$DEST/venv/bin/python</string><string>$DEST/agent/deck_agent.py</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$DEST/agent.log</string>
  <key>StandardErrorPath</key><string>$DEST/agent.log</string>
</dict></plist>
EOF
launchctl unload "$PLIST" 2>/dev/null || true
launchctl load "$PLIST"
echo "Watch Deck installato e avviato (log: $DEST/agent.log)."
echo "Si avvia da solo a ogni accesso. Per toglierlo: sh install_macos.command --remove"
