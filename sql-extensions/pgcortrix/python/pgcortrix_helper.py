"""pgcortrix_helper — HTTP client behind the pgcortrix plpython3u functions.

Installed into PG's plpython3u site-packages (see Makefile); the SQL function
bodies do `from pgcortrix_helper import get_client`. SoT: F14-pgcortrix.md
(§2.2.bis SSRF, §2.4 client, §4.2 cancel, §4.3 retry, §5 errors).

Why this shape — the build machine has no PostgreSQL, so the design constraint
(L1-α briefing §6) is that this module be *unit-testable off a live PG by
mocking urllib*. We get there by keeping the PG-specific surface tiny and
injected:

  * The only PG dependency is a `plpy`-like object, passed into the client. In
    PG it is the real `plpy` global; in tests it is a small fake. All access to
    it goes through `_PlpyAdapter`, so a test fake only needs `execute` / a few
    raise helpers.
  * All real logic — endpoint validation, URL + body assembly, user_id
    synthesis, retry/backoff decisions, HTTP error → CortrixError mapping — is
    in module-level pure functions / `CortrixError`, callable with no PG and no
    network.
  * The single network primitive is `urllib.request.urlopen`, patched in tests.

The error model mirrors the C++ Agent-friendly contract (catalog_error.h /
agent_friendly/error.h): a stable CX_ERR_* code + category enum
(auth/quota/transient/permanent/timeout) + retryable + retry_after_ms +
structured_data, table-driven from `_ERROR_REGISTRY`.
"""

import ipaddress
import json
import threading
import time
from urllib import error, request
from urllib.parse import urlencode, urlparse

__all__ = [
    "CortrixError",
    "PgcortrixClient",
    "get_client",
    "ErrorCategory",
    "ALLOWED_FILTER_KEYS",
    "validate_filter",
    "validate_endpoint",
    "synthesize_user_id",
    "parse_synthetic_user_id",
    "build_url",
    "classify_http_error",
]

# ===========================================================================
# Error model (mirrors agent_friendly/error.h + catalog_error.cpp registry)
# ===========================================================================


class ErrorCategory:
    """GEN-Agent error category (CLAUDE.md 7 principles #4). String constants — these
    are exactly the lowercase tokens the API contract serializes."""
    AUTH = "auth"
    QUOTA = "quota"
    TRANSIENT = "transient"
    PERMANENT = "permanent"
    TIMEOUT = "timeout"


# One canonical row per F14 CX_ERR_* code. Mirrors catalog_error.cpp's
# table-driven registry. SoT for these identities: ARCHITECTURE.md §4.1.11
# (CX_ERR_F14_INVALID_FILTER / CX_ERR_F14_ENDPOINT_BLOCKED) + §4.1 user_id
# (CX_ERR_USER_ID_MISSING). retry_after_ms is None unless retryable.
#
# NOTE (design flag raised to Lead): F14 §5/§7.bis acceptance prose also names
# a CX_ERR_PG_* family (SSRF_BLOCKED / HTTP_TIMEOUT / ...). Those are NOT in the
# architecture SoT registry, and §5 explicitly says V1 transport errors
# (timeout/5xx/conn/4xx) are surfaced as plain plpy.error() PG ERRORs, not
# structured codes. So we register only the SoT codes here; transport failures
# go through `plpy.error()` (see PgcortrixClient._post).
_ERROR_REGISTRY = {
    "CX_ERR_F14_INVALID_FILTER": (ErrorCategory.PERMANENT, False, None),
    "CX_ERR_F14_ENDPOINT_BLOCKED": (ErrorCategory.PERMANENT, False, None),
    "CX_ERR_USER_ID_MISSING": (ErrorCategory.AUTH, False, None),
}


class CortrixError(Exception):
    """Agent-friendly structured error. Identity = `code` (CX_ERR_*); category /
    retryable / retry_after_ms come from `_ERROR_REGISTRY` so call sites never
    restate them (same discipline as MakeCatalogError)."""

    def __init__(self, code, message="", structured_data=None):
        if code not in _ERROR_REGISTRY:
            # Defensive: an unregistered code is a programming error, not a
            # silently-mis-categorized one. Treat as permanent/internal.
            category, retryable, retry_after_ms = (
                ErrorCategory.PERMANENT, False, None)
        else:
            category, retryable, retry_after_ms = _ERROR_REGISTRY[code]
        self.code = code
        self.message = message or code
        self.category = category
        self.retryable = retryable
        self.retry_after_ms = retry_after_ms
        self.structured_data = structured_data
        super().__init__("%s: %s" % (self.code, self.message))

    def to_body(self):
        """The AGENT_FRIENDLY §3.1 error body (4+ fields). retry_after_ms /
        structured_data become None (JSON null) when unset."""
        return {
            "code": self.code,
            "message": self.message,
            "retryable": self.retryable,
            "category": self.category,
            "retry_after_ms": self.retry_after_ms,
            "structured_data": self.structured_data,
        }


# ===========================================================================
# §2.1.5 — filter JSONB whitelist (anti SQL-injection / over-posting)
# ===========================================================================

ALLOWED_FILTER_KEYS = frozenset(
    ["session_id", "namespace_id", "from_ts", "to_ts", "sort_order"])


def validate_filter(filter_jsonb):
    """Whitelist-validate the list_interactions filter. Rejects any key not in
    ALLOWED_FILTER_KEYS → CX_ERR_F14_INVALID_FILTER (F14 §2.1.5). Accepts a dict
    (plpython3u hands JSONB in as a dict) or a JSON string; None → {}."""
    if filter_jsonb is None:
        return {}
    if isinstance(filter_jsonb, dict):
        f = filter_jsonb
    else:
        try:
            f = json.loads(filter_jsonb)
        except Exception:
            raise CortrixError(
                "CX_ERR_F14_INVALID_FILTER",
                "filter must be valid JSONB object",
                {"reason": "json_parse_failed"})
        if not isinstance(f, dict):
            raise CortrixError(
                "CX_ERR_F14_INVALID_FILTER",
                "filter must be a JSONB object",
                {"reason": "not_an_object"})
    extra = set(f.keys()) - ALLOWED_FILTER_KEYS
    if extra:
        raise CortrixError(
            "CX_ERR_F14_INVALID_FILTER",
            "unknown filter field(s): %s" % sorted(extra),
            {"invalid_field": sorted(extra),
             "allowed_fields": sorted(ALLOWED_FILTER_KEYS)})
    if "sort_order" in f and f["sort_order"] not in ("ASC", "DESC"):
        raise CortrixError(
            "CX_ERR_F14_INVALID_FILTER",
            "sort_order must be 'ASC' or 'DESC'",
            {"invalid_value": f["sort_order"]})
    return f


# ===========================================================================
# §2.2.bis — endpoint SSRF defence (V3-E-02)
# ===========================================================================

ALLOWED_HOSTS = frozenset(["localhost", "127.0.0.1", "cortrix-server"])
ALLOWED_PORTS = frozenset([8420, 9091])  # main API + OpenMetrics

# Private / metadata networks that must never be reachable via the endpoint GUC.
# loopback (127/8) is allowed explicitly (cortrix-server runs on loopback), so
# it is excluded from the block decision below.
_BLOCKED_NETWORKS = [
    ipaddress.ip_network("169.254.0.0/16"),  # link-local (cloud metadata)
    ipaddress.ip_network("10.0.0.0/8"),      # RFC1918
    ipaddress.ip_network("172.16.0.0/12"),   # RFC1918
    ipaddress.ip_network("192.168.0.0/16"),  # RFC1918
    ipaddress.ip_network("fd00::/8"),        # IPv6 unique-local
    ipaddress.ip_network("fe80::/10"),       # IPv6 link-local
]


def validate_endpoint(endpoint):
    """SSRF defence (V3-E-02): allowlist host + port, block private / metadata
    IPs. Raises CX_ERR_F14_ENDPOINT_BLOCKED on violation. Returns the parsed
    (host, port) on success."""
    parsed = urlparse(endpoint)
    host = parsed.hostname
    if host is None:
        raise CortrixError(
            "CX_ERR_F14_ENDPOINT_BLOCKED",
            "pgcortrix.endpoint has no host: %r" % endpoint,
            {"endpoint": endpoint, "reason": "no_host"})
    port = parsed.port or (443 if parsed.scheme == "https" else 80)

    if host not in ALLOWED_HOSTS:
        # Not an allowlisted name → must be a non-private, non-metadata IP.
        try:
            ip = ipaddress.ip_address(host)
        except ValueError:
            raise CortrixError(
                "CX_ERR_F14_ENDPOINT_BLOCKED",
                "pgcortrix.endpoint host %s not in allowlist" % host,
                {"host": host, "allowed_hosts": sorted(ALLOWED_HOSTS)})
        if not ip.is_loopback:
            for blocked in _BLOCKED_NETWORKS:
                if ip in blocked:
                    raise CortrixError(
                        "CX_ERR_F14_ENDPOINT_BLOCKED",
                        "pgcortrix.endpoint host %s in blocked network %s"
                        % (host, blocked),
                        {"host": host, "blocked_network": str(blocked)})

    if port not in ALLOWED_PORTS:
        raise CortrixError(
            "CX_ERR_F14_ENDPOINT_BLOCKED",
            "pgcortrix.endpoint port %s not in allowlist" % port,
            {"port": port, "allowed_ports": sorted(ALLOWED_PORTS)})
    return host, port


# ===========================================================================
# user_id synthesis (F18a §2.3 issue 8 Option D: "pg:<PG_user>:<pid>")
# ===========================================================================

_SYNTHETIC_USER_PREFIX = "pg:"


def synthesize_user_id(pg_user, pid):
    """Caller-identity for audit logging when pgcortrix has no configure()'d API
    key (F18a Option D). NOT the MEM05 data-isolation user_id (that is an explicit
    function argument). Format: "pg:<PG_user>:<pid>"."""
    return "%s%s:%s" % (_SYNTHETIC_USER_PREFIX, pg_user, pid)


def parse_synthetic_user_id(user_id):
    """Inverse of synthesize_user_id → (pg_user, pid) or None if not synthetic.
    pid is returned as int when numeric, else as the raw string. Tolerates a
    PG_user that itself contains ':' by splitting from the right."""
    if not isinstance(user_id, str) or not user_id.startswith(
            _SYNTHETIC_USER_PREFIX):
        return None
    rest = user_id[len(_SYNTHETIC_USER_PREFIX):]
    if ":" not in rest:
        return None
    pg_user, _, pid_str = rest.rpartition(":")
    if pg_user == "":
        return None
    try:
        pid = int(pid_str)
    except ValueError:
        pid = pid_str
    return pg_user, pid


# ===========================================================================
# HTTP helpers (pure)
# ===========================================================================


def build_url(endpoint, path, params=None):
    """Join endpoint + path (+ optional query string). Drops params whose value
    is None so optional filters don't appear as empty query keys."""
    url = endpoint.rstrip("/") + path
    if params:
        clean = {k: v for k, v in params.items() if v is not None}
        if clean:
            url = url + "?" + urlencode(clean)
    return url


def classify_http_error(status_code):
    """Map an HTTP status to (retryable, short_reason) for retry decisions
    (F14 §4.3): 5xx retryable (transient), 4xx not. The reason string feeds the
    PG error message / metric label."""
    if 500 <= status_code <= 599:
        return True, "http_5xx"
    if status_code in (401, 403):
        return False, "http_auth"
    if status_code == 404:
        return False, "http_not_found"
    if 400 <= status_code <= 499:
        return False, "http_4xx"
    return False, "http_other"


def backoff_delays_ms(retry_max):
    """Exponential backoff schedule (F14 §4.3): 100 / 200 / 400 ... for the
    given number of retries."""
    return [100 * (2 ** i) for i in range(max(0, retry_max))]


# ===========================================================================
# plpy adapter — the entire PG-specific surface, kept tiny + injectable
# ===========================================================================


class _PlpyAdapter:
    """Thin wrapper over the plpython3u `plpy` object. Centralizes the few calls
    we make so a unit-test fake only has to implement `execute` (+ optionally
    raise on a sentinel for cancel tests). In PG, `plpy.error()` raises; our fake
    raises RuntimeError so tests can assert on it."""

    def __init__(self, plpy):
        self._plpy = plpy

    def setting(self, guc, default=""):
        # current_setting(..., true) is allowed inside STABLE functions, unlike
        # SHOW. The GUC name is internal and never user supplied.
        rows = self._plpy.execute(
            "SELECT current_setting('%s', true) AS value" % guc)
        if not rows:
            return default
        value = rows[0].get("value")
        return default if value is None else value

    def scalar(self, sql):
        rows = self._plpy.execute(sql)
        if not rows:
            return None
        # return the single column value of the single row
        return list(rows[0].values())[0]

    def ping_for_interrupt(self):
        """A no-op query whose execution makes PG run CHECK_FOR_INTERRUPTS, so a
        pg_cancel_backend / statement_timeout is observed (F14 §4.2). Raises
        whatever plpy raises on interrupt; caller treats any raise as cancel."""
        self._plpy.execute("SELECT 1")

    def error(self, message):
        """Raise a PG ERROR. plpy.error() never returns; we still `raise` after
        in case a fake returns, so control flow is unambiguous."""
        self._plpy.error(message)
        raise RuntimeError(message)  # unreachable under real plpy


# ===========================================================================
# Client
# ===========================================================================


class PgcortrixClient:
    """HTTP client used by all pgcortrix functions. One instance per backend,
    cached in GD (see get_client). Reads GUCs on every call so runtime `SET`s
    take effect (F14 §2.4)."""

    def __init__(self, plpy, urlopen=None, sleep=None):
        self._pg = _PlpyAdapter(plpy)
        # Injectable for tests; defaults to the stdlib. urllib is the ONLY
        # network primitive (mocked in unit tests).
        self._urlopen = urlopen or request.urlopen
        # Injectable so retry-backoff tests don't actually sleep (and don't have
        # to monkeypatch the module's `time`).
        self._sleep = sleep or time.sleep
        # Per-session API key override set via pgcortrix_configure(); takes
        # precedence over the api_key GUC when non-empty.
        self._session_api_key = None
        # Synthetic user_id is logged WARN once per backend (F18a Option D).
        self._warned_synthetic_user = False

    # ---- config ----------------------------------------------------------

    def _get_config(self):
        cfg = {
            "endpoint": self._pg.setting(
                "pgcortrix.endpoint", "http://localhost:8420"),
            "api_key": self._pg.setting("pgcortrix.api_key", ""),
            "timeout_ms": int(self._pg.setting("pgcortrix.timeout_ms", "30000")),
            "retry_max": int(self._pg.setting("pgcortrix.retry_max", "3")),
        }
        if self._session_api_key:
            cfg["api_key"] = self._session_api_key
        return cfg

    def configure(self, api_key):
        """pgcortrix_configure(api_key): set a session-level API key."""
        self._session_api_key = api_key

    def caller_user_id(self, configured_api_key):
        """Resolve the audit caller-identity (F18a Option D). With a configured
        API key the server derives the real user_id, so we send nothing here
        (return None). Without one, synthesize "pg:<PG_user>:<pid>" and WARN
        once. This is the audit identity, distinct from the MEM05 data user_id
        passed as a function argument."""
        if configured_api_key:
            return None
        pg_user = self._pg.scalar("SELECT session_user")
        pid = self._pg.scalar("SELECT pg_backend_pid()")
        uid = synthesize_user_id(pg_user, pid)
        if not self._warned_synthetic_user:
            try:
                self._pg._plpy.notice(  # best-effort; absent on minimal fakes
                    "pgcortrix: no API key configured; logging caller as %s "
                    "(call pgcortrix_configure to attribute operations to a "
                    "real user)" % uid)
            except Exception:
                pass
            self._warned_synthetic_user = True
        return uid

    # ---- HTTP core -------------------------------------------------------

    def _headers(self, cfg):
        headers = {"Content-Type": "application/json"}
        if cfg["api_key"]:
            headers["X-API-Key"] = cfg["api_key"]
        caller = self.caller_user_id(cfg["api_key"])
        if caller:
            # F18a audit attribution when anonymous (no API key).
            headers["X-Pgcortrix-Caller"] = caller
        return headers

    def _request(self, method, path, cfg, body=None, params=None):
        """Single HTTP attempt. Validates the endpoint first (SSRF), builds the
        Request, runs it in a daemon thread while the main thread polls PG for
        cancel (§4.2). Returns parsed JSON. Raises urllib errors to the retry
        loop; raises CortrixError for SSRF; calls plpy.error() for cancel /
        timeout."""
        validate_endpoint(cfg["endpoint"])  # V3-E-02, every call
        url = build_url(cfg["endpoint"], path, params)
        data = json.dumps(body).encode("utf-8") if body is not None else None
        req = request.Request(
            url, data=data, headers=self._headers(cfg), method=method)

        result = {"response": None, "error": None}

        def _do():
            try:
                with self._urlopen(req, timeout=cfg["timeout_ms"] / 1000.0) as resp:
                    raw = resp.read()
                    result["response"] = json.loads(raw) if raw else {}
            except Exception as e:  # propagate to main thread
                result["error"] = e

        t = threading.Thread(target=_do, daemon=True)
        t.start()

        deadline = time.time() + cfg["timeout_ms"] / 1000.0
        while t.is_alive() and time.time() < deadline:
            t.join(timeout=0.1)
            # Touch PG so CHECK_FOR_INTERRUPTS runs; any raise = cancel (§4.2).
            try:
                self._pg.ping_for_interrupt()
            except Exception:
                self._pg.error("pgcortrix request canceled by PG")

        if t.is_alive():
            self._pg.error(
                "pgcortrix request timeout (%dms)" % cfg["timeout_ms"])

        if result["error"] is not None:
            raise result["error"]
        return result["response"]

    def _post(self, path, body, params=None):
        return self._do_with_retry("POST", path, body=body, params=params)

    def _get(self, path, params=None):
        return self._do_with_retry("GET", path, body=None, params=params)

    def _do_with_retry(self, method, path, body=None, params=None):
        """Retry 5xx / connection errors per §4.3 (exponential backoff); 4xx and
        SSRF are not retried. Terminal transport failures are surfaced as PG
        ERRORs via plpy.error() (F14 §5 — V1 does not map to structured codes)."""
        cfg = self._get_config()
        delays = backoff_delays_ms(cfg["retry_max"])
        attempt = 0
        while True:
            try:
                return self._request(
                    method, path, cfg, body=body, params=params)
            except error.HTTPError as e:
                retryable, _reason = classify_http_error(e.code)
                if retryable and attempt < len(delays):
                    e.close()  # discard this attempt's body before retrying
                    self._sleep(delays[attempt] / 1000.0)
                    attempt += 1
                    continue
                detail = ""
                try:
                    detail = e.read().decode("utf-8", "replace")[:500]
                except Exception:
                    pass
                e.close()
                self._pg.error(
                    "pgcortrix HTTP %s: %s%s"
                    % (e.code, getattr(e, "reason", ""),
                       (" — " + detail) if detail else ""))
            except error.URLError as e:
                # Connection-level failure: treat like 5xx (§4.3).
                if attempt < len(delays):
                    self._sleep(delays[attempt] / 1000.0)
                    attempt += 1
                    continue
                self._pg.error(
                    "pgcortrix endpoint unreachable: %s (%s)"
                    % (cfg["endpoint"], getattr(e, "reason", e)))

    # ---- typed endpoints (F14 §2.4 + v1.0.3 endpoint map §3.3) ----------

    def search(self, namespace, query, top_k, filter, rerank):
        resp = self._post("/api/v1/query", {
            "namespaces": [namespace],
            "query": query,
            "top_k": top_k,
            "filter": filter,
            "rerank": rerank,
        })
        results = resp.get("results", [])
        normalized = []
        for row in results:
            if not isinstance(row, dict):
                normalized.append(row)
                continue
            item = dict(row)
            metadata = item.get("metadata")
            if isinstance(metadata, dict):
                if "chunk_id" not in item and "child_id" in item:
                    item["chunk_id"] = item.get("child_id")
                if not item.get("filename"):
                    item["filename"] = (
                        metadata.get("source_path") or metadata.get("filename"))
            normalized.append(item)
        return normalized

    def upload(self, namespace, file_path):
        with open(file_path, "rb") as f:
            content = f.read()
        resp = self._post("/api/v1/documents", {
            "namespace": namespace,
            "filename": file_path.split("/")[-1],
            "content_base64": content.hex(),  # V1 simplification (§2.4)
        })
        return resp.get("doc_id")

    def list_documents(self, namespace):
        resp = self._get("/api/v1/namespaces/%s/documents" % namespace)
        documents = resp.get("documents", resp.get("results", []))
        normalized = []
        for doc in documents:
            if not isinstance(doc, dict):
                normalized.append(doc)
                continue
            item = dict(doc)
            if "filename" not in item and "source_path" in item:
                item["filename"] = item.get("source_path")
            if "chunks" not in item and "block_count" in item:
                item["chunks"] = item.get("block_count")
            normalized.append(item)
        return normalized

    def batch_submit(self, namespace, documents, async_=True,
                     on_duplicate="skip"):
        """TD-F42-BULK: batch-submit up to 100 documents in one call.

        ``documents`` is the already-decoded list of per-doc dicts (each with a
        client-supplied doc_id + content; optional filename / metadata). Returns
        the full partial-success envelope (results + meta) verbatim so the SQL
        layer can hand it back as JSONB — per-doc failures live in
        meta.failed[] (GEN-Agent 5 fields), not as PG ERRORs; only batch-level
        faults (size/payload/empty/duplicate-doc_id) surface as HTTP 4xx ->
        plpy.error via _do_with_retry. async_ must stay True in V1."""
        resp = self._post("/api/v1/documents/batch", {
            "namespace": namespace,
            "documents": documents,
            "options": {"async": async_, "on_duplicate": on_duplicate},
        })
        return resp

    def memory_search(self, namespace, query, user_id, top_k):
        if not user_id:
            raise CortrixError(
                "CX_ERR_USER_ID_MISSING",
                "pgcortrix_memory_search requires user_id (MEM05 isolation)",
                {"reason": "user_id_required_for_mem05_isolation"})
        resp = self._post("/api/v1/memory/search", {
            "namespace": namespace,
            "query": query,
            "user_id": user_id,
            "top_k": top_k,
        })
        return resp.get("results", [])

    def list_interactions(self, namespace, user_id, filter, limit_n, offset_n):
        """v1.0.2: MEM05 D4 user_id mandatory + whitelist filter (§2.1.5).
        Endpoint GET /api/v1/memory/interactions (§3.3 v1.0.3)."""
        if not user_id:
            raise CortrixError(
                "CX_ERR_USER_ID_MISSING",
                "pgcortrix_list_interactions requires user_id (MEM05 D4)",
                {"reason": "user_id_required_for_mem05_isolation"})
        validated = validate_filter(filter)  # raises CX_ERR_F14_INVALID_FILTER
        params = {
            "namespace": namespace,
            "user_id": user_id,            # MEM05 D4: never NULL
            "limit": limit_n,
            "offset": offset_n,
        }
        # Whitelisted filter fields become query params.
        for key in ("session_id", "namespace_id", "from_ts", "to_ts",
                    "sort_order"):
            if key in validated:
                params[key] = validated[key]
        resp = self._get("/api/v1/memory/interactions", params=params)
        return resp.get("interactions", resp.get("results", []))

    def status(self):
        """V23 D6: JSONB-friendly dict. Probes /api/v1/health for connectivity
        with a /health fallback. Never raises — a disconnected server still yields a status
        blob with http_connected=false (diagnostics must always answer)."""
        cfg = self._get_config()
        result = {
            "version": "1.0.0-rc.1",
            "endpoint": cfg["endpoint"],
            "timeout_ms": cfg["timeout_ms"],
            "retry_max": cfg["retry_max"],
        }
        try:
            t0 = time.time()
            validate_endpoint(cfg["endpoint"])
            for path in ("/api/v1/health", "/health"):
                try:
                    url = build_url(cfg["endpoint"], path)
                    req = request.Request(
                        url, headers=self._headers(cfg), method="GET")
                    with self._urlopen(
                            req, timeout=cfg["timeout_ms"] / 1000.0) as resp:
                        resp.read()
                    break
                except error.HTTPError as e:
                    if path == "/health":
                        raise
                    e.close()
                    continue
            result["http_connected"] = True
            result["latency_ms"] = int((time.time() - t0) * 1000)
        except Exception as e:
            result["http_connected"] = False
            result["error"] = str(e)[:200]
        return result


# ===========================================================================
# GD-cached factory (F14 §2.3) — import the module + build the client once per
# backend, not per call.
# ===========================================================================

_GD_KEY = "pgcortrix_client"


def get_client(plpy, gd):
    """Return the per-backend PgcortrixClient, creating + caching it in GD on
    first use. `gd` is plpython3u's per-session global dict."""
    client = gd.get(_GD_KEY)
    if client is None:
        client = PgcortrixClient(plpy)
        gd[_GD_KEY] = client
    return client
