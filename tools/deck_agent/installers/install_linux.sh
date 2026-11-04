#!/bin/sh
# Watch Deck for Linux: installs the agent for the current user and starts it at every login.
#   sh install.sh            install and start
#   sh install.sh --remove   stop and remove
# Needs python3 (with venv). Optional tools it uses when present: xdotool, wmctrl, playerctl, pactl.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST="$HOME/.local/share/watchdeck"
AUTOSTART="$HOME/.config/autostart/watchdeck.desktop"

if [ "$1" = "--remove" ]; then
    pkill -f "$DEST/agent/deck_agent.py" 2>/dev/null || true
    rm -f "$AUTOSTART"
    rm -rf "$DEST"
    echo "Watch Deck rimosso."
    exit 0
fi

command -v python3 >/dev/null || { echo "Serve python3: installalo e riprova."; exit 1; }
mkdir -p "$DEST/agent"
cp "$HERE"/*.py "$HERE/requirements.txt" "$DEST/agent/"
python3 -m venv "$DEST/venv"
"$DEST/venv/bin/pip" install --quiet --upgrade pip
"$DEST/venv/bin/pip" install --quiet -r "$DEST/agent/requirements.txt"

mkdir -p "$(dirname "$AUTOSTART")"
cat > "$AUTOSTART" <<EOF
[Desktop Entry]
Type=Application
Name=Watch Deck
Exec=$DEST/venv/bin/python $DEST/agent/deck_agent.py
X-GNOME-Autostart-enabled=true
EOF

pkill -f "$DEST/agent/deck_agent.py" 2>/dev/null || true
nohup "$DEST/venv/bin/python" "$DEST/agent/deck_agent.py" >"$DEST/agent.log" 2>&1 &
echo "Watch Deck installato e avviato (log: $DEST/agent.log)."
echo "Si avvia da solo a ogni accesso. Per toglierlo: sh install.sh --remove"
