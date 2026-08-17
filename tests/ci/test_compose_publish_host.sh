#!/usr/bin/env bash
# Contract test for the Compose publish address (deploy/docker-compose.yml).
#
# Two rendered shapes are pinned:
#   1. default (no override): host_ip 127.0.0.1, published 8420 — the loopback-only
#      Quick Start contract, byte-for-byte unchanged when the variable is unset;
#   2. explicit opt-in: CORTRIX_PUBLISH_HOST=<addr> CORTRIX_HTTP_PORT=<port> renders
#      exactly that address/port and nothing else changes on the service.
# A third check pins that setting only the port does NOT widen the bind — the
# publish host must be an explicit, separate decision.
set -euo pipefail
cd "$(dirname "$0")/../.."
COMPOSE=deploy/docker-compose.yml
fail() { echo "FAIL: $*" >&2; exit 1; }
render() { env -u CORTRIX_PUBLISH_HOST -u CORTRIX_HTTP_PORT "$@" docker compose -f "$COMPOSE" config; }
field() { grep -E "^\s+$1:" | head -1 | sed -E 's/.*: *"?([^"]*)"?/\1/'; }

# 1. default: loopback only.
out="$(render)"
[ "$(printf '%s\n' "$out" | field host_ip)"   = "127.0.0.1" ] || fail "default host_ip must be 127.0.0.1"
[ "$(printf '%s\n' "$out" | field published)" = "8420" ]      || fail "default published port must be 8420"
printf '%s\n' "$out" | grep -q 'CORTRIX_SERVER_ALLOW_UNAUTHENTICATED_CONTAINER_BIND: "true"' \
  || fail "container-bind exception must remain part of the rendered stack (documented boundary)"

# 2. explicit LAN opt-in.
out="$(render CORTRIX_PUBLISH_HOST=10.0.0.5 CORTRIX_HTTP_PORT=18420)"
[ "$(printf '%s\n' "$out" | field host_ip)"   = "10.0.0.5" ] || fail "override host_ip must be 10.0.0.5"
[ "$(printf '%s\n' "$out" | field published)" = "18420" ]    || fail "override published port must be 18420"

# 3. port alone must not widen the bind.
out="$(render CORTRIX_HTTP_PORT=18420)"
[ "$(printf '%s\n' "$out" | field host_ip)"   = "127.0.0.1" ] || fail "port-only override must keep host_ip 127.0.0.1"
[ "$(printf '%s\n' "$out" | field published)" = "18420" ]     || fail "port-only override must publish 18420"

echo "compose publish contract: 3/3 shapes OK"
