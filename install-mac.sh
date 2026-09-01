#!/bin/bash
# ============================================================================
# ATEM Tally Server - macOS installer
#
#   ./install-mac.sh              install dependencies, ready to run
#   ./install-mac.sh --service    also register as a launchd service
#                                 (starts at login, restarts on crash)
#   ./install-mac.sh --uninstall-service
#   ./install-mac.sh --status     show service state, last log lines, HTTP check
#
# After install:  ./start.command   (or double-click it in Finder)
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$SCRIPT_DIR/server"
PLIST_LABEL="com.noedge.atem-tally"
PLIST_PATH="$HOME/Library/LaunchAgents/$PLIST_LABEL.plist"

info()  { printf '\033[1;32m[install]\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m[install]\033[0m %s\n' "$*"; }
fail()  { printf '\033[1;31m[install]\033[0m %s\n' "$*"; exit 1; }

[ -d "$SERVER_DIR" ] || fail "server/ directory not found next to this script."

case "$SCRIPT_DIR" in
  "$HOME/Desktop"*|"$HOME/Documents"*|"$HOME/Downloads"*)
    warn "This folder is inside Desktop/Documents/Downloads. macOS privacy protection"
    warn "(TCC) often blocks background services from reading files there, which makes"
    warn "the launchd service fail silently. Recommended: move the project to ~/atem-web-tally"
    warn "and re-run this script from there.";;
esac

# ---------------------------------------------------------------------------
# Status, if requested
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--status" ]; then
  echo "--- launchd ---"
  if launchctl list | grep -q "$PLIST_LABEL"; then
    launchctl list | grep "$PLIST_LABEL" | awk '{printf "PID: %s   last exit: %s\n", $1, $2}'
    echo "(PID '-' means not running; a non-zero exit is the crash/launch error code)"
  else
    echo "service not installed (run: ./install-mac.sh --service)"
  fi
  echo "--- plist paths ---"
  [ -f "$PLIST_PATH" ] && grep -A3 ProgramArguments "$PLIST_PATH" | grep string | sed 's/<[^>]*>//g'
  echo "--- last log lines ---"
  tail -n 15 "$SERVER_DIR/logs/err.log" 2>/dev/null || echo "(no err.log yet)"
  echo "--- HTTP check ---"
  PORT="$(node -p "try{JSON.parse(require('fs').readFileSync('$SERVER_DIR/config.json')).httpPort||3000}catch(e){3000}" 2>/dev/null || echo 3000)"
  if curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/api/state" | grep -q 200; then
    echo "server responding on http://localhost:$PORT"
  else
    echo "server NOT responding on port $PORT"
  fi
  exit 0
fi

# ---------------------------------------------------------------------------
# Uninstall service and exit, if requested
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--uninstall-service" ]; then
  if [ -f "$PLIST_PATH" ]; then
    launchctl bootout "gui/$(id -u)/$PLIST_LABEL" 2>/dev/null || launchctl unload "$PLIST_PATH" 2>/dev/null || true
    rm -f "$PLIST_PATH"
    info "launchd service removed."
  else
    info "No service was installed."
  fi
  exit 0
fi

# ---------------------------------------------------------------------------
# 1. Node.js
# ---------------------------------------------------------------------------
if command -v node >/dev/null 2>&1; then
  NODE_MAJOR="$(node -p 'process.versions.node.split(".")[0]')"
  if [ "$NODE_MAJOR" -lt 18 ]; then
    warn "Node $(node -v) found but v18+ is required."
    NEED_NODE=1
  else
    info "Node $(node -v) found."
    NEED_NODE=0
  fi
else
  warn "Node.js not found."
  NEED_NODE=1
fi

if [ "$NEED_NODE" = "1" ]; then
  if command -v brew >/dev/null 2>&1; then
    info "Installing Node.js via Homebrew..."
    brew install node
  else
    fail "Install Node.js first: download the macOS installer from https://nodejs.org (LTS), run it, then re-run this script. (Or install Homebrew from https://brew.sh and re-run.)"
  fi
fi

# ---------------------------------------------------------------------------
# 2. Dependencies
# ---------------------------------------------------------------------------
info "Installing server dependencies (npm install)..."
cd "$SERVER_DIR"
npm install --no-audit --no-fund

# ---------------------------------------------------------------------------
# 3. First-run config
# ---------------------------------------------------------------------------
if [ ! -f "$SERVER_DIR/config.json" ]; then
  info "Creating default config.json (first run of the server also does this)..."
  node -e "require('fs').writeFileSync('config.json', JSON.stringify({
    atemIp: '192.168.1.240', cameraCount: 4, httpPort: 3000,
    broadcastPort: 7411, statusPort: 7412, broadcastAddress: '255.255.255.255',
    heartbeatMs: 500, lowBatteryPct: 20, inputNameOverrides: {}, devices: {}
  }, null, 2))"
  warn "Edit server/config.json and set atemIp to your ATEM's IP address."
fi

# ---------------------------------------------------------------------------
# 4. start.command (double-clickable in Finder)
# ---------------------------------------------------------------------------
cat > "$SCRIPT_DIR/start.command" <<EOF
#!/bin/bash
cd "\$(dirname "\$0")/server"
exec node server.js
EOF
chmod +x "$SCRIPT_DIR/start.command"
info "Created start.command (first double-click: right-click > Open to pass Gatekeeper)."

# ---------------------------------------------------------------------------
# 5. Optional launchd service
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--service" ]; then
  NODE_BIN="$(command -v node)"
  [ -x "$NODE_BIN" ] || fail "node binary not found/executable at '$NODE_BIN'"
  [ -f "$SERVER_DIR/server.js" ] || fail "server.js not found at $SERVER_DIR"
  [ -d "$SERVER_DIR/node_modules" ] || fail "node_modules missing — npm install did not complete"
  NODE_DIR="$(dirname "$NODE_BIN")"
  mkdir -p "$HOME/Library/LaunchAgents" "$SERVER_DIR/logs"
  cat > "$PLIST_PATH" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$PLIST_LABEL</string>
  <key>ProgramArguments</key>
  <array><string>$NODE_BIN</string><string>$SERVER_DIR/server.js</string></array>
  <key>WorkingDirectory</key><string>$SERVER_DIR</string>
  <key>EnvironmentVariables</key>
  <dict>
    <key>PATH</key><string>$NODE_DIR:/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin</string>
    <key>HOME</key><string>$HOME</string>
  </dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$SERVER_DIR/logs/out.log</string>
  <key>StandardErrorPath</key><string>$SERVER_DIR/logs/err.log</string>
</dict>
</plist>
EOF
  launchctl bootout "gui/$(id -u)/$PLIST_LABEL" 2>/dev/null || true
  if ! launchctl bootstrap "gui/$(id -u)" "$PLIST_PATH"; then
    warn "bootstrap failed, falling back to legacy load"
    launchctl load "$PLIST_PATH"
  fi
  sleep 2
  if launchctl list | grep "$PLIST_LABEL" | awk '{exit ($1=="-")}'; then
    info "launchd service installed and running (logs in server/logs/)."
  else
    warn "service registered but not running — run ./install-mac.sh --status for details"
  fi
fi

# ---------------------------------------------------------------------------
# 6. Report URLs on this Mac's LAN address
# ---------------------------------------------------------------------------
LAN_IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo '<this-mac-ip>')"
PORT="$(node -p "try{JSON.parse(require('fs').readFileSync('$SERVER_DIR/config.json')).httpPort||3000}catch(e){3000}")"

echo ""
info "Done. To start (if not installed as a service): ./start.command"
echo ""
echo "    Admin:       http://$LAN_IP:$PORT/admin.html"
echo "    Web tally:   http://$LAN_IP:$PORT/tally.html"
echo ""
warn "Reminder: set atemIp in server/config.json, and keep this Mac and the"
warn "tallies on the same network/VLAN (UDP broadcast does not cross subnets)."
