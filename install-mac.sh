#!/bin/bash
# ============================================================================
# ATEM Tally Server - macOS installer
#
#   ./install-mac.sh              install dependencies, ready to run
#   ./install-mac.sh --service    also register as a launchd service
#                                 (starts at login, restarts on crash)
#   ./install-mac.sh --uninstall-service
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

# ---------------------------------------------------------------------------
# Uninstall service and exit, if requested
# ---------------------------------------------------------------------------
if [ "${1:-}" = "--uninstall-service" ]; then
  if [ -f "$PLIST_PATH" ]; then
    launchctl unload "$PLIST_PATH" 2>/dev/null || true
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
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$SERVER_DIR/logs/out.log</string>
  <key>StandardErrorPath</key><string>$SERVER_DIR/logs/err.log</string>
</dict>
</plist>
EOF
  launchctl unload "$PLIST_PATH" 2>/dev/null || true
  launchctl load "$PLIST_PATH"
  info "launchd service installed and started (logs in server/logs/)."
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
