#!/bin/bash
# server/run.sh — start the photo frame server
cd "$(dirname "$0")"
export PHOTOFRAME_API_KEY="${PHOTOFRAME_API_KEY:-changeme}"
export PHOTOFRAME_PORT="${PHOTOFRAME_PORT:-8080}"
source venv/bin/activate 2>/dev/null || { echo "Run: python3 -m venv venv && pip install -r requirements.txt"; exit 1; }
exec python main.py
