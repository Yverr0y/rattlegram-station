#!/usr/bin/env bash
# Serve the web app on http://localhost:8787 (localhost = secure context, so mic works).
cd "$(dirname "$0")/web"
exec python3 -m http.server "${1:-8787}"
