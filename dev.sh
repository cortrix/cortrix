#!/usr/bin/env bash
# Cortrix end-to-end development launcher
# Starts the backend (8420) + Cortrix Agent (8001) + frontend dev server (5173) together
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
AGENT_PYTHON="$AGENT_DIR/venv/bin/python"
AGENT_PORT="${CORTRIX_AGENT_PORT:-8001}"
AGENT_BASE_URL="${CORTRIX_AGENT_BASE_URL:-http://127.0.0.1:${AGENT_PORT}}"
DEFAULT_CONFIG="$SCRIPT_DIR/build/config.yaml"
EDITION="CE"

# Colored output
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYAN}[cortrix]${NC} $*"; }
ok()    { echo -e "${GREEN}[cortrix]${NC} $*"; }
warn()  { echo -e "${YELLOW}[cortrix]${NC} $*"; }
error() { echo -e "${RED}[cortrix]${NC} $*" >&2; exit 1; }

# Detect whether npm resolves to a Windows binary (WSL interop path starts with /mnt/ or /c/)
NPM_IS_WINDOWS=false
NPM_PATH="$(which npm 2>/dev/null || true)"
if [[ "$NPM_PATH" == /c/* || "$NPM_PATH" == /mnt/c/* ]]; then
    NPM_IS_WINDOWS=true
    warn "Windows npm detected ($NPM_PATH). Install Linux Node.js for best compatibility."
fi

# Kill Windows node/npm processes on a port range via taskkill (WSL interop)
kill_windows_vite_ports() {
    for PORT in 5173 5174 5175 5176 5177 5178 5179 5180; do
        PIDS=$(cmd.exe /c "netstat -ano 2>nul | findstr :${PORT}" 2>/dev/null \
               | awk '{print $NF}' | grep -E '^[0-9]+$' | sort -u)
        for PID in $PIDS; do
            [[ "$PID" == "0" ]] && continue
            cmd.exe /c "taskkill /F /PID $PID" 2>/dev/null || true
        done
    done
}

# Cleanup function: kill all services when the script exits
BACKEND_PID=""
AGENT_PID=""
FRONTEND_PID=""
cleanup() {
    echo ""
    info "Stopping services..."
    [[ -n "$BACKEND_PID" ]] && kill "$BACKEND_PID" 2>/dev/null || true
    info "Backend stopped"
    [[ -n "$AGENT_PID"   ]] && kill "$AGENT_PID"   2>/dev/null || true
    info "Agent stopped"
    [[ -n "$FRONTEND_PID" ]] && kill "$FRONTEND_PID" 2>/dev/null || true
    pkill -f "vite" 2>/dev/null || true
    pkill -TERM -f "node.*vite" 2>/dev/null || true
    if [[ "$NPM_IS_WINDOWS" == true ]]; then
        kill_windows_vite_ports
    fi
    info "Frontend stopped"
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
export CORTRIX_OPENAPI_ROOT="${CORTRIX_OPENAPI_ROOT:-$SCRIPT_DIR}"

# Check the local models are provisioned (fail fast — the server would otherwise
# silently fall back to stub embedding / stub reranker / heuristic complexity and
# skip PDF/image parsing, which is rarely what a one-command launch wants).
# Set CORTRIX_ALLOW_MISSING_MODELS=1 to start anyway in degraded/stub mode.
MISSING_MODELS=()
[[ -f "$SCRIPT_DIR/models/bge-m3/model.onnx" ]]            || MISSING_MODELS+=("bge-m3 (embedding)")
[[ -f "$SCRIPT_DIR/models/bge-reranker-v2-m3/model.onnx" ]] || MISSING_MODELS+=("bge-reranker-v2-m3 (rerank)")
[[ -f "$SCRIPT_DIR/models/query-complexity/model.onnx" ]]  || MISSING_MODELS+=("query-complexity (F39 routing)")
[[ -x "$SCRIPT_DIR/scripts/ocr_venv/bin/python3" || -x "$SCRIPT_DIR/scripts/ocr_venv/bin/python3.12" ]] \
    || MISSING_MODELS+=("docling/paddleocr parser venv (PDF/image parsing)")
if [[ ${#MISSING_MODELS[@]} -gt 0 ]]; then
    if [[ "${CORTRIX_ALLOW_MISSING_MODELS:-}" == "1" ]]; then
        warn "Missing models (degraded mode, CORTRIX_ALLOW_MISSING_MODELS=1):"
        for m in "${MISSING_MODELS[@]}"; do warn "  - $m"; done
    else
        echo "" >&2
        for m in "${MISSING_MODELS[@]}"; do echo -e "${RED}[cortrix]${NC}   missing: $m" >&2; done
        error "Local models are not provisioned.\nRun the one-click setup first:  ./scripts/setup_models.sh\n(see deploy/MODELS.md for download/hosting options)\nOr start anyway in degraded/stub mode:  CORTRIX_ALLOW_MISSING_MODELS=1 ./dev.sh"
    fi
fi

# Check frontend dependencies
[[ -d "$WEB_DIR/node_modules" ]] || {
    warn "Frontend dependencies not installed, installing..."
    (cd "$WEB_DIR" && npm install)
}

# Clean up old processes and ports
pkill -f "cortrix-server"   2>/dev/null || true
pkill -f "vite"             2>/dev/null || true
pkill -f "node.*vite"       2>/dev/null || true
pkill -f "uvicorn main:app" 2>/dev/null || true
if [[ "$NPM_IS_WINDOWS" == true ]]; then
    kill_windows_vite_ports
fi
sleep 0.5

# ── Start backend ──────────────────────────────────────
info "Starting backend (http://localhost:8420)..."
ENABLE_AGENT_PROXY=false
if [[ -x "$AGENT_PYTHON" || -n "${CORTRIX_AGENT_BASE_URL:-}" ]]; then
    ENABLE_AGENT_PROXY=true
fi

if [[ "$ENABLE_AGENT_PROXY" == true && -n "$AUTH_OVERRIDE" ]]; then
    CORTRIX_AUTH_ENABLED="$AUTH_OVERRIDE" CORTRIX_AGENT_BASE_URL="$AGENT_BASE_URL" "$BACKEND" --config "$CONFIG_FILE" &
elif [[ "$ENABLE_AGENT_PROXY" == true ]]; then
    CORTRIX_AGENT_BASE_URL="$AGENT_BASE_URL" "$BACKEND" --config "$CONFIG_FILE" &
elif [[ -n "$AUTH_OVERRIDE" ]]; then
    CORTRIX_AUTH_ENABLED="$AUTH_OVERRIDE" "$BACKEND" --config "$CONFIG_FILE" &
else
    "$BACKEND" --config "$CONFIG_FILE" &
fi
BACKEND_PID=$!

# Wait for the backend to be ready (up to 60 seconds).
# Real-model cold start (bge-m3 2.1GB + reranker + complexity, with CoreML
# graph compilation on first run) blocks before the HTTP server begins
# listening and can take ~20s; the old 10s window timed out prematurely.
for i in {1..200}; do
    if curl -sf http://localhost:8420/api/v1/health &>/dev/null; then
        ok "Backend ready ✓"
        break
    fi
    sleep 0.3
    if [[ $i -eq 200 ]]; then
        error "Backend startup timed out, please check the logs"
    fi
done

# ── Start Cortrix Agent ────────────────────────────────────
if [[ -x "$AGENT_PYTHON" ]]; then
    info "Starting Cortrix Agent ($AGENT_BASE_URL)..."
    (cd "$AGENT_DIR" && "$AGENT_PYTHON" -m uvicorn main:app --host 0.0.0.0 --port "$AGENT_PORT" --log-level warning) &
    AGENT_PID=$!
    # Wait for the Agent to be ready (up to 8 seconds)
    for i in {1..26}; do
        if curl -sf "$AGENT_BASE_URL/health" &>/dev/null; then
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
(cd "$WEB_DIR" && npm run dev) &
FRONTEND_PID=$!

sleep 2
ok "====================================="
ok "  Cortrix started"
ok "  Frontend: http://localhost:5173"
ok "  Backend:  http://localhost:8420/api/v1/health"
ok "  Agent:    $AGENT_BASE_URL/health"
ok "  Edition:  $EDITION"
ok "  Config:   $CONFIG_FILE"
ok "  LLM:      cortrix-agent/.env"
ok "  Press Ctrl+C to stop all services"
ok "====================================="

# Wait for child processes
WAIT_PIDS=("$FRONTEND_PID" "$BACKEND_PID")
[[ -n "$AGENT_PID" ]] && WAIT_PIDS+=("$AGENT_PID")
wait "${WAIT_PIDS[@]}"
