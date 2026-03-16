#!/bin/bash
# server/install.sh — set up venv and install dependencies for development/testing
set -e

cd "$(dirname "$0")"

if ! command -v python3 &>/dev/null; then
    echo "Error: python3 not found"
    exit 1
fi

echo "Creating virtual environment..."
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt

echo ""
echo "Done. Start the server with:"
echo "  PHOTOFRAME_API_KEY=yourkey ./run.sh"
