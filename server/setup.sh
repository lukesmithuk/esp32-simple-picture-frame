#!/bin/bash
# setup.sh — single-command installer for the Photo Frame Server
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_NAME="photoframe-server"
MIN_PYTHON_VERSION="3.10"

# ── Checks ───────────────────────────────────────────────────────────────

if [ "$(id -u)" = 0 ]; then
    echo "Error: do not run as root. Run as your normal user — sudo is used only where needed."
    exit 1
fi

echo "=== Photo Frame Server Setup ==="

# Check Python version
if ! command -v python3 &>/dev/null; then
    echo "Error: python3 not found. Install it with:"
    echo "  sudo apt install python3 python3-venv"
    exit 1
fi

PYTHON_VERSION=$(python3 -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}")')
if python3 -c "import sys; exit(0 if sys.version_info >= (3, 10) else 1)" 2>/dev/null; then
    echo "Checking Python... OK ($PYTHON_VERSION)"
else
    echo "Error: Python >= $MIN_PYTHON_VERSION required (found $PYTHON_VERSION)"
    echo "  sudo apt install python3 python3-venv"
    exit 1
fi

# Check python3-venv
if ! python3 -m venv --help &>/dev/null; then
    echo "Error: python3-venv not found. Install it with:"
    echo "  sudo apt install python3-venv"
    exit 1
fi

# Check libjpeg-dev (needed to build Pillow)
if ! dpkg -s libjpeg-dev &>/dev/null 2>&1; then
    echo "Installing libjpeg-dev (required by Pillow)..."
    sudo apt install -y libjpeg-dev
fi

# ── Virtual environment ──────────────────────────────────────────────────

cd "$SCRIPT_DIR"

echo "Creating virtual environment..."
python3 -m venv venv
source venv/bin/activate
echo "Installing dependencies..."
pip install -r requirements.txt
echo "Dependencies installed."

# ── Configuration ────────────────────────────────────────────────────────

# Read existing config if present (idempotent re-run)
EXISTING_KEY=""
EXISTING_PORT=""
if [ -f "$SCRIPT_DIR/server.env" ]; then
    EXISTING_KEY=$(grep -oP 'PHOTOFRAME_API_KEY=\K.*' "$SCRIPT_DIR/server.env" 2>/dev/null || true)
    EXISTING_PORT=$(grep -oP 'PHOTOFRAME_PORT=\K.*' "$SCRIPT_DIR/server.env" 2>/dev/null || true)
fi

# Generate default API key if no existing one
if [ -z "$EXISTING_KEY" ]; then
    DEFAULT_KEY=$(python3 -c "import secrets; print(secrets.token_hex(16))")
else
    DEFAULT_KEY="$EXISTING_KEY"
fi
DEFAULT_PORT="${EXISTING_PORT:-8080}"

read -p "API key [$DEFAULT_KEY]: " API_KEY
API_KEY="${API_KEY:-$DEFAULT_KEY}"

read -p "Server port [$DEFAULT_PORT]: " PORT
PORT="${PORT:-$DEFAULT_PORT}"

cat > "$SCRIPT_DIR/server.env" <<EOF
PHOTOFRAME_API_KEY=$API_KEY
PHOTOFRAME_PORT=$PORT
PHOTOFRAME_HOST=0.0.0.0
EOF

# ── Systemd service ─────────────────────────────────────────────────────

echo "Installing systemd service..."
sudo tee /etc/systemd/system/${SERVICE_NAME}.service > /dev/null <<EOF
[Unit]
Description=Photo Frame Server
After=network.target

[Service]
Type=simple
User=$USER
WorkingDirectory=$SCRIPT_DIR
EnvironmentFile=$SCRIPT_DIR/server.env
ExecStart=$SCRIPT_DIR/venv/bin/python $SCRIPT_DIR/main.py
Restart=on-failure
RestartSec=5
TimeoutStopSec=5
KillMode=mixed
KillSignal=SIGINT

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable ${SERVICE_NAME}
sudo systemctl restart ${SERVICE_NAME}

# ── Done ─────────────────────────────────────────────────────────────────

SERVER_IP=$(hostname -I | awk '{print $1}')

echo ""
echo "=== Setup Complete ==="
echo "Server running at http://${SERVER_IP}:${PORT}"
echo "API key: ${API_KEY}"
echo ""
echo "Add to your frame's /sdcard/config.txt:"
echo "  server_url=http://${SERVER_IP}:${PORT}"
echo "  server_api_key=${API_KEY}"
echo ""
echo "Commands:"
echo "  sudo systemctl status $SERVICE_NAME"
echo "  sudo systemctl restart $SERVICE_NAME"
echo "  sudo journalctl -u $SERVICE_NAME -f"
