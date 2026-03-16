#!/bin/bash
# server/install-service.sh — install as a systemd service for production
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_NAME="photoframe-server"

# Check venv exists
if [ ! -d "$SCRIPT_DIR/venv" ]; then
    echo "Error: run ./install.sh first to set up the virtual environment"
    exit 1
fi

# Prompt for config
read -p "API key for frame authentication [changeme]: " API_KEY
API_KEY="${API_KEY:-changeme}"

read -p "Server port [8080]: " PORT
PORT="${PORT:-8080}"

# Write env file
cat > "$SCRIPT_DIR/server.env" <<EOF
PHOTOFRAME_API_KEY=$API_KEY
PHOTOFRAME_PORT=$PORT
PHOTOFRAME_HOST=0.0.0.0
EOF

echo "Config written to $SCRIPT_DIR/server.env"

# Install systemd service
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

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable ${SERVICE_NAME}
sudo systemctl start ${SERVICE_NAME}

echo ""
echo "=== Service installed ==="
echo "  Status:  sudo systemctl status $SERVICE_NAME"
echo "  Logs:    sudo journalctl -u $SERVICE_NAME -f"
echo "  Stop:    sudo systemctl stop $SERVICE_NAME"
echo "  Restart: sudo systemctl restart $SERVICE_NAME"
