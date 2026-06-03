#!/usr/bin/env bash
# start_demo.sh — launch Runtime Truth UI + Cloudflare Tunnel for trusted remote demo
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT=5001
TOKEN="${RT_DEMO_TOKEN:-}"

# ── generate token if not supplied ──────────────────────────────────────────
if [[ -z "$TOKEN" ]]; then
    TOKEN=$(python3 -c "import secrets; print(secrets.token_urlsafe(16))")
fi
export RT_DEMO_TOKEN="$TOKEN"

# ── ensure cloudflared is installed ─────────────────────────────────────────
if ! command -v cloudflared &>/dev/null; then
    echo "cloudflared not found — installing via Homebrew..."
    brew install cloudflared
fi

# ── kill any existing instance on the port ──────────────────────────────────
OLD_PID=$(lsof -i ":$PORT" -t 2>/dev/null || true)
if [[ -n "$OLD_PID" ]]; then
    echo "Stopping existing process on port $PORT (PID $OLD_PID)..."
    kill "$OLD_PID" 2>/dev/null || true
    sleep 1
fi

# ── start Flask ──────────────────────────────────────────────────────────────
mkdir -p /tmp/rt_ui_runs
echo "Starting Runtime Truth UI on port $PORT..."
cd "$SCRIPT_DIR"
python3 app.py "$PORT" > /tmp/rt_flask.log 2>&1 &
FLASK_PID=$!
echo "Flask PID: $FLASK_PID"

# wait for Flask to be ready
for i in $(seq 1 15); do
    if curl -s -o /dev/null -w "%{http_code}" "http://localhost:$PORT/" \
           -H "Authorization: Basic $(echo -n "demo:$TOKEN" | base64)" | grep -qE "^[23]"; then
        break
    fi
    sleep 0.5
done

echo ""
echo "Flask is up on http://localhost:$PORT"

# ── start cloudflared quick tunnel ──────────────────────────────────────────
# --url  : local service to expose
# Output lands on stderr; grep the https URL from it
echo ""
echo "Starting Cloudflare Tunnel..."
CF_LOG=/tmp/rt_cloudflare.log
cloudflared tunnel --url "http://localhost:$PORT" > "$CF_LOG" 2>&1 &
CF_PID=$!
echo "cloudflared PID: $CF_PID"

# wait up to 20 s for the public URL to appear
PUBLIC_URL=""
for i in $(seq 1 40); do
    PUBLIC_URL=$(grep -oE 'https://[a-z0-9-]+\.trycloudflare\.com' "$CF_LOG" 2>/dev/null | head -1 || true)
    [[ -n "$PUBLIC_URL" ]] && break
    sleep 0.5
done

# ── summary ──────────────────────────────────────────────────────────────────
echo ""
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║           Runtime Truth UI — Demo Ready                     ║"
echo "╠══════════════════════════════════════════════════════════════╣"
if [[ -n "$PUBLIC_URL" ]]; then
    echo "║  Public URL : $PUBLIC_URL"
else
    echo "║  Public URL : (tunnel starting — check /tmp/rt_cloudflare.log)"
fi
echo "║  Username   : demo"
echo "║  Password   : $TOKEN"
echo "║"
echo "║  Local URL  : http://localhost:$PORT"
echo "║"
echo "║  Flask log  : /tmp/rt_flask.log"
echo "║  Tunnel log : /tmp/rt_cloudflare.log"
echo "║"
echo "║  Flask PID  : $FLASK_PID"
echo "║  Tunnel PID : $CF_PID"
echo "╠══════════════════════════════════════════════════════════════╣"
echo "║  To stop everything:"
echo "║    kill $FLASK_PID $CF_PID"
echo "║  Or just close this terminal."
echo "╚══════════════════════════════════════════════════════════════╝"
echo ""
echo "Remote access credentials:"
if [[ -n "$PUBLIC_URL" ]]; then
    echo "  URL      : $PUBLIC_URL"
fi
echo "  Username : demo"
echo "  Password : $TOKEN"
echo ""

# keep the script alive so both background jobs stay alive
wait
