#!/usr/bin/env bash
# Cortrix end-to-end development launcher
# Starts the backend (8080) + Cortrix Agent (8001) + frontend dev server (5173) together
#
# Usage:
#   ./dev.sh                       # default: CE mode
#   ./dev.sh --config /path/x.yaml # specify a config file
#   ./dev.sh --auth                # enable auth (overrides the setting in config.yaml)
#
# Config file: build/config.yaml (edit this to configure the LLM, data directories, etc.)
# Agent LLM:   cortrix-agent/.env (edit this to configure the LLM provider, API key)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BACKEND="$SCRIPT_DIR/build/cortrix-server"
WEB_DIR="$SCRIPT_DIR/web"
AGENT_DIR="$SCRIPT_DIR/cortrix-agent"
DEFAULT_CONFIG="$SCRIPT_DIR/build/config.yaml"

# Colored output
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYAN}[cortrix]${NC} $*"; }
ok()    { echo -e "${GREEN}[cortrix]${NC} $*"; }
warn()  { echo -e "${YELLOW}[cortrix]${NC} $*"; }
error() { echo -e "${RED}[cortrix]${NC} $*" >&2; exit 1; }

# Cleanup function: kill all services when the script exits
BACKEND_PID=""
AGENT_PID=""
FRONTEND_PID=""
cleanup() {
    echo ""
    info "Stopping services..."
    [[ -n "$BACKEND_PID"  ]] && kill "$BACKEND_PID"  2>/dev/null; info "Backend stopped"
    [[ -n "$AGENT_PID"    ]] && kill "$AGENT_PID"    2>/dev/null; info "Agent stopped"
    [[ -n "$FRONTEND_PID" ]] && kill "$FRONTEND_PID" 2>/dev/null; info "Frontend stopped"
    exit 0
}
trap cleanup INT TERM

# Parse arguments
CONFIG_FILE="$DEFAULT_CONFIG"
AUTH_OVERRIDE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --auth)        AUTH_OVERRIDE="true";  shift ;;
        --no-auth)     AUTH_OVERRIDE="false"; shift ;;
        --config)      CONFIG_FILE="$2"; shift 2 ;;
        *)             error "Unknown argument: $1" ;;
    esac
done

# Check the backend binary
[[ -x "$BACKEND" ]] || error "Backend binary not found: $BACKEND\nRun first: cmake --build build"

# Check the config file
[[ -f "$CONFIG_FILE" ]] || error "Config file does not exist: $CONFIG_FILE"
info "Using config file: $CONFIG_FILE"

# Check frontend dependencies
[[ -d "$WEB_DIR/node_modules" ]] || {
    warn "Frontend dependencies not installed, installing..."
    npm install --prefix "$WEB_DIR"
}

# Clean up old processes and ports
pkill -f "cortrix-server" 2>/dev/null || true
pkill -f "vite"          2>/dev/null || true
pkill -f "uvicorn main:app" 2>/dev/null || true
sleep 0.5
for PORT in 8080 8001 5173; do
    PID=$(lsof -ti :"$PORT" -sTCP:LISTEN 2>/dev/null || true)
    if [[ -n "$PID" ]]; then
        warn "Port $PORT still in use (PID $PID), forcing release..."
        kill -9 "$PID" 2>/dev/null || true
        sleep 0.3
    fi
done

# ── Start backend ──────────────────────────────────────
info "Starting backend (http://localhost:8080)..."
if [[ -n "$AUTH_OVERRIDE" ]]; then
    CORTRIX_AUTH_ENABLED="$AUTH_OVERRIDE" "$BACKEND" --config "$CONFIG_FILE" &
else
    "$BACKEND" --config "$CONFIG_FILE" &
fi
BACKEND_PID=$!

# Wait for the backend to be ready (up to 10 seconds)
for i in {1..33}; do
    if curl -sf http://localhost:8080/api/v1/health &>/dev/null; then
        ok "Backend ready ✓"
        break
    fi
    sleep 0.3
    if [[ $i -eq 33 ]]; then
        error "Backend startup timed out, please check the logs"
    fi
done

# ── Start Cortrix Agent ────────────────────────────────────
AGENT_PYTHON="$AGENT_DIR/venv/bin/python"
if [[ -x "$AGENT_PYTHON" ]]; then
    info "Starting Cortrix Agent (http://localhost:8001)..."
    (cd "$AGENT_DIR" && "$AGENT_PYTHON" -m uvicorn main:app --host 0.0.0.0 --port 8001 --log-level warning) &
    AGENT_PID=$!
    # Wait for the Agent to be ready (up to 8 seconds)
    for i in {1..26}; do
        if curl -sf http://localhost:8001/health &>/dev/null; then
            ok "Cortrix Agent ready ✓  (LLM: $(grep LLM_PROVIDER "$AGENT_DIR/.env" 2>/dev/null | cut -d= -f2 || echo unknown))"
            break
        fi
        sleep 0.3
        if [[ $i -eq 26 ]]; then
            warn "Cortrix Agent startup timed out, the LLM settings feature may be unavailable"
        fi
    done
else
    warn "Cortrix Agent venv not found ($AGENT_PYTHON), skipping the Agent service (LLM settings unavailable)"
fi

# ── Start frontend ──────────────────────────────────────
info "Starting frontend (http://localhost:5173)..."
npm run dev --prefix "$WEB_DIR" &
FRONTEND_PID=$!

sleep 2
ok "====================================="
ok "  Cortrix started"
ok "  Frontend: http://localhost:5173"
ok "  Backend:  http://localhost:8080/api/v1/health"
ok "  Agent:    http://localhost:8001/health"
ok "  Edition:  $EDITION"
ok "  Config:   $CONFIG_FILE"
ok "  LLM:      cortrix-agent/.env"
ok "  Press Ctrl+C to stop all services"
ok "====================================="

# Wait for child processes
WAIT_PIDS=("$FRONTEND_PID" "$BACKEND_PID")
[[ -n "$AGENT_PID" ]] && WAIT_PIDS+=("$AGENT_PID")
wait "${WAIT_PIDS[@]}"
