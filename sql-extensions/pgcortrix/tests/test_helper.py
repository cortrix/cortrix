"""S2/S3 unit tests for pgcortrix_helper — HTTP client logic with urllib mocked.

No PostgreSQL and no network: `plpy` is a small fake (FakePlpy) and urllib is
replaced by a fake urlopen (FakeUrlopen / capture). These cover the three things
the standalone DoD asks for (L1-α briefing §6):
  * HTTP call shape — URL, method, body, headers per endpoint
  * user_id resolution — "pg:<PG_user>:<pid>" synthesis + parse, MEM05 mandatory
  * error-code mapping — CX_ERR_F14_* + transport → plpy.error()

Run: python3 -m unittest discover -s tests  (from the pgcortrix/ dir)
"""

import io
import json
import os
import sys
import unittest
from urllib import error

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "python"))

import pgcortrix_helper as h  # noqa: E402


# ===========================================================================
# Fakes
# ===========================================================================


class PgCanceled(Exception):
    """Sentinel a FakePlpy raises from a SELECT 1 to simulate PG cancel."""


class FakePlpy:
    """Minimal stand-in for plpython3u's `plpy`. Serves GUCs from a dict, answers
    `SELECT session_user` / `pg_backend_pid()`, and records error()/notice().
    Set `cancel_on_ping=True` to make `SELECT 1` raise (cancel test)."""

    def __init__(self, gucs=None, session_user="alice", pid=4242):
        self.gucs = {
            "pgcortrix.endpoint": "http://localhost:8420",
            "pgcortrix.api_key": "",
            "pgcortrix.timeout_ms": "30000",
            "pgcortrix.retry_max": "3",
        }
        if gucs:
            self.gucs.update(gucs)
        self.session_user = session_user
        self.pid = pid
        self.cancel_on_ping = False
        self.errors = []
        self.notices = []

    def execute(self, sql, *args, **kwargs):
        s = sql.strip()
        if s.upper().startswith("SHOW "):
            guc = s[5:].strip()
            return [{guc: self.gucs[guc]}]
        if s == "SELECT 1":
            if self.cancel_on_ping:
                raise PgCanceled("canceled")
            return [{"?column?": 1}]
        if "session_user" in s:
            return [{"session_user": self.session_user}]
        if "pg_backend_pid" in s:
            return [{"pg_backend_pid": self.pid}]
        return [{}]

    def error(self, message):
        # Real plpy.error() raises a PG ERROR; mirror that so callers unwind.
        self.errors.append(message)
        raise RuntimeError(message)

    def notice(self, message):
        self.notices.append(message)


class FakeResponse:
    """Context-manager response object like urlopen returns; .read() → bytes."""

    def __init__(self, payload):
        self._buf = io.BytesIO(
            payload if isinstance(payload, bytes)
            else json.dumps(payload).encode("utf-8"))

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def read(self):
        return self._buf.read()


class Capture:
    """A fake urlopen that records the Request and returns a queued payload."""

    def __init__(self, payloads=None):
        # payloads: list of dict|bytes|Exception, consumed per call.
        self.payloads = list(payloads or [{}])
        self.requests = []
        self.timeouts = []

    def __call__(self, req, timeout=None):
        self.requests.append(req)
        self.timeouts.append(timeout)
        item = self.payloads.pop(0) if self.payloads else {}
        if isinstance(item, Exception):
            raise item
        return FakeResponse(item)

    @property
    def last(self):
        return self.requests[-1]

    def body_of(self, idx=-1):
        return json.loads(self.requests[idx].data.decode("utf-8"))


def make_client(plpy=None, payloads=None, gucs=None):
    plpy = plpy or FakePlpy(gucs=gucs)
    cap = Capture(payloads=payloads)
    # sleep is a no-op so retry-backoff tests run instantly and don't mutate the
    # module's `time` (clean teardown, no cross-test leakage).
    client = h.PgcortrixClient(plpy, urlopen=cap, sleep=lambda s: None)
    return client, plpy, cap


# ===========================================================================
# user_id synthesis / parse  ("pg:<PG_user>:<pid>")
# ===========================================================================


class TestUserIdSynthesis(unittest.TestCase):
    def test_synthesize_format(self):
        self.assertEqual(h.synthesize_user_id("alice", 4242), "pg:alice:4242")

    def test_roundtrip(self):
        self.assertEqual(
            h.parse_synthetic_user_id(h.synthesize_user_id("bob", 7)),
            ("bob", 7))

    def test_pg_user_with_colon_splits_from_right(self):
        # A PG role literally named "ad:min" must still parse pid correctly.
        self.assertEqual(
            h.parse_synthetic_user_id("pg:ad:min:99"), ("ad:min", 99))

    def test_non_synthetic_returns_none(self):
        self.assertIsNone(h.parse_synthetic_user_id("real-user"))
        self.assertIsNone(h.parse_synthetic_user_id("pg:"))
        self.assertIsNone(h.parse_synthetic_user_id("pg:onlyuser"))
        self.assertIsNone(h.parse_synthetic_user_id(None))

    def test_non_numeric_pid_kept_as_string(self):
        self.assertEqual(h.parse_synthetic_user_id("pg:u:abc"), ("u", "abc"))

    def test_caller_identity_synthesized_when_anonymous(self):
        client, plpy, _ = make_client(FakePlpy(session_user="carol", pid=11))
        self.assertEqual(client.caller_user_id(""), "pg:carol:11")
        # WARN emitted once.
        self.assertEqual(len(plpy.notices), 1)
        self.assertIn("pg:carol:11", plpy.notices[0])
        # ...and only once per backend.
        client.caller_user_id("")
        self.assertEqual(len(plpy.notices), 1)

    def test_caller_identity_none_when_api_key_present(self):
        client, _, _ = make_client()
        self.assertIsNone(client.caller_user_id("secret-key"))


# ===========================================================================
# HTTP call shape per endpoint
# ===========================================================================


class TestHttpShape(unittest.TestCase):
    def test_search_post_query(self):
        client, _, cap = make_client(payloads=[{
            "results": [{"chunk_id": "c1", "content": "hi", "score": 0.9,
                         "doc_id": "d1", "filename": "f.md", "metadata": {}}]}])
        rows = client.search("ns", "hello", 5, {"year": 2026}, True)
        self.assertEqual(cap.last.method, "POST")
        self.assertTrue(cap.last.full_url.endswith("/api/v1/query"))
        body = cap.body_of()
        self.assertEqual(body, {"namespace": "ns", "query": "hello",
                                "top_k": 5, "filter": {"year": 2026},
                                "rerank": True})
        self.assertEqual(rows[0]["chunk_id"], "c1")
        # Content-Type header present.
        self.assertEqual(cap.last.headers.get("Content-type"),
                         "application/json")

    def test_upload_posts_documents_with_hex(self):
        client, _, cap = make_client(payloads=[{"doc_id": "doc-9"}])
        path = os.path.join(os.path.dirname(__file__), "_fixture_upload.bin")
        with open(path, "wb") as f:
            f.write(b"ABC")
        try:
            doc_id = client.upload("ns", path)
        finally:
            os.remove(path)
        self.assertEqual(doc_id, "doc-9")
        self.assertTrue(cap.last.full_url.endswith("/api/v1/documents"))
        body = cap.body_of()
        self.assertEqual(body["namespace"], "ns")
        self.assertEqual(body["filename"], "_fixture_upload.bin")
        self.assertEqual(body["content_base64"], b"ABC".hex())

    def test_list_documents_path_param_ns(self):
        client, _, cap = make_client(payloads=[{"documents": [
            {"doc_id": "d1", "filename": "a", "status": "ready",
             "chunks": 3, "created_at": "2026-05-01T00:00:00Z"}]}])
        docs = client.list_documents("my-ns")
        self.assertEqual(cap.last.method, "GET")
        self.assertTrue(
            cap.last.full_url.endswith("/api/v1/namespaces/my-ns/documents"))
        self.assertEqual(docs[0]["doc_id"], "d1")

    def test_batch_submit_posts_documents_with_options(self):
        resp = {
            "results": [{"doc_id": "d1", "task_id": "t1", "status": "submitted"}],
            "meta": {"succeeded": ["d1"], "failed": [],
                     "coverage_ratio": 1.0, "total_submitted": 1},
        }
        client, _, cap = make_client(payloads=[resp])
        out = client.batch_submit(
            "ns",
            [{"doc_id": "d1", "content": "x", "metadata": {"src": "web"}}],
            on_duplicate="overwrite")
        self.assertEqual(cap.last.method, "POST")
        self.assertTrue(cap.last.full_url.endswith("/api/v1/documents/batch"))
        body = cap.body_of()
        self.assertEqual(body["namespace"], "ns")
        self.assertEqual(body["documents"][0]["doc_id"], "d1")
        self.assertEqual(
            body["options"], {"async": True, "on_duplicate": "overwrite"})
        # Full partial-success envelope returned verbatim (JSONB-out shape).
        self.assertEqual(out["meta"]["coverage_ratio"], 1.0)

    def test_batch_submit_defaults_async_true_skip(self):
        client, _, cap = make_client(payloads=[{"results": [], "meta": {}}])
        client.batch_submit("ns", [{"doc_id": "d1", "content": "x"}])
        body = cap.body_of()
        self.assertEqual(
            body["options"], {"async": True, "on_duplicate": "skip"})

    def test_memory_search_posts_with_user_id(self):
        client, _, cap = make_client(payloads=[{"results": []}])
        client.memory_search("ns", "q", "user-42", 5)
        self.assertTrue(cap.last.full_url.endswith("/api/v1/memory/search"))
        body = cap.body_of()
        self.assertEqual(body["user_id"], "user-42")
        self.assertEqual(body["top_k"], 5)

    def test_list_interactions_get_with_params(self):
        client, _, cap = make_client(payloads=[{"interactions": []}])
        client.list_interactions(
            "chat-ns", "user-42", {"session_id": "abc"}, 20, 0)
        url = cap.last.full_url
        self.assertEqual(cap.last.method, "GET")
        self.assertIn("/api/v1/memory/interactions?", url)
        self.assertIn("namespace=chat-ns", url)
        self.assertIn("user_id=user-42", url)
        self.assertIn("session_id=abc", url)
        self.assertIn("limit=20", url)
        self.assertIn("offset=0", url)

    def test_api_key_becomes_header(self):
        client, _, cap = make_client(
            payloads=[{"results": []}], gucs={"pgcortrix.api_key": "k-1"})
        client.search("ns", "q", 1, None, False)
        self.assertEqual(cap.last.headers.get("X-api-key"), "k-1")
        # With a key, no anonymous caller header.
        self.assertNotIn("X-pgcortrix-caller", cap.last.headers)

    def test_anonymous_sends_caller_header(self):
        client, _, cap = make_client(
            payloads=[{"results": []}])  # api_key empty
        client.search("ns", "q", 1, None, False)
        self.assertEqual(cap.last.headers.get("X-pgcortrix-caller"),
                         "pg:alice:4242")

    def test_configure_overrides_guc_key(self):
        client, _, cap = make_client(payloads=[{"results": []}])
        client.configure("session-key")
        client.search("ns", "q", 1, None, False)
        self.assertEqual(cap.last.headers.get("X-api-key"), "session-key")


# ===========================================================================
# MEM05 user_id enforcement + filter whitelist (error-code mapping)
# ===========================================================================


class TestMem05AndFilter(unittest.TestCase):
    def test_memory_search_requires_user_id(self):
        client, _, cap = make_client()
        with self.assertRaises(h.CortrixError) as ctx:
            client.memory_search("ns", "q", "", 5)
        self.assertEqual(ctx.exception.code, "CX_ERR_USER_ID_MISSING")
        self.assertEqual(ctx.exception.category, h.ErrorCategory.AUTH)
        # No HTTP call made — rejected before transport.
        self.assertEqual(len(cap.requests), 0)

    def test_list_interactions_requires_user_id(self):
        client, _, _ = make_client()
        with self.assertRaises(h.CortrixError) as ctx:
            client.list_interactions("ns", None, None, 50, 0)
        self.assertEqual(ctx.exception.code, "CX_ERR_USER_ID_MISSING")

    def test_list_interactions_rejects_bad_filter_before_http(self):
        client, _, cap = make_client()
        with self.assertRaises(h.CortrixError) as ctx:
            client.list_interactions("ns", "u", {"evil": 1}, 50, 0)
        self.assertEqual(ctx.exception.code, "CX_ERR_F14_INVALID_FILTER")
        self.assertEqual(ctx.exception.structured_data["invalid_field"],
                         ["evil"])
        self.assertEqual(len(cap.requests), 0)

    def test_filter_fields_become_query_params(self):
        client, _, cap = make_client(payloads=[{"interactions": []}])
        client.list_interactions(
            "ns", "u",
            {"from_ts": "2026-05-01T00:00:00Z", "sort_order": "ASC"}, 100, 0)
        url = cap.last.full_url
        self.assertIn("sort_order=ASC", url)
        self.assertIn("from_ts=", url)

    def test_invalid_filter_error_body_is_agent_friendly(self):
        err = h.CortrixError(
            "CX_ERR_F14_INVALID_FILTER", "bad",
            {"invalid_field": ["x"]})
        body = err.to_body()
        self.assertEqual(set(body), {"code", "message", "retryable",
                                     "category", "retry_after_ms",
                                     "structured_data"})
        self.assertFalse(body["retryable"])
        self.assertEqual(body["category"], "permanent")


# ===========================================================================
# SSRF defence (V3-E-02)
# ===========================================================================


class TestSsrf(unittest.TestCase):
    def test_allow_loopback_and_service_name(self):
        for ep in ("http://localhost:8420", "http://127.0.0.1:9091",
                   "http://cortrix-server:8420"):
            self.assertEqual(h.validate_endpoint(ep)[1] in (8420, 9091), True)

    def test_block_cloud_metadata_ip(self):
        with self.assertRaises(h.CortrixError) as ctx:
            h.validate_endpoint("http://169.254.169.254/latest/meta-data/")
        self.assertEqual(ctx.exception.code, "CX_ERR_F14_ENDPOINT_BLOCKED")

    def test_block_private_ranges(self):
        for ep in ("http://10.0.0.5:8420", "http://172.16.0.1:8420",
                   "http://192.168.1.1:8420"):
            with self.assertRaises(h.CortrixError):
                h.validate_endpoint(ep)

    def test_block_non_allowlist_host(self):
        with self.assertRaises(h.CortrixError):
            h.validate_endpoint("http://evil.example.com:8420")

    def test_block_disallowed_port(self):
        with self.assertRaises(h.CortrixError):
            h.validate_endpoint("http://localhost:5432")

    def test_block_ipv6_link_local(self):
        with self.assertRaises(h.CortrixError):
            h.validate_endpoint("http://[fe80::1]:8420")

    def test_request_validates_endpoint(self):
        # A malicious endpoint GUC is caught before any HTTP call.
        client, _, cap = make_client(
            gucs={"pgcortrix.endpoint": "http://169.254.169.254"})
        with self.assertRaises(h.CortrixError) as ctx:
            client.search("ns", "q", 1, None, False)
        self.assertEqual(ctx.exception.code, "CX_ERR_F14_ENDPOINT_BLOCKED")
        self.assertEqual(len(cap.requests), 0)


# ===========================================================================
# Retry / backoff / transport errors (F14 §4.3, §5)
# ===========================================================================


class TestRetryAndErrors(unittest.TestCase):
    def _http_error(self, code):
        return error.HTTPError(
            "http://localhost:8420/x", code, "err", {}, io.BytesIO(b"body"))

    def test_5xx_retries_then_succeeds(self):
        # First two 503s, third returns OK (F14 §7.2 case 6).
        client, _, cap = make_client(payloads=[
            self._http_error(503), self._http_error(503), {"results": [{}]}])
        rows = client.search("ns", "q", 1, None, False)
        self.assertEqual(len(cap.requests), 3)
        self.assertEqual(rows, [{}])

    def test_5xx_exhausts_retries_then_pg_error(self):
        client, plpy, cap = make_client(payloads=[self._http_error(500)] * 10)
        with self.assertRaises(RuntimeError):
            client.search("ns", "q", 1, None, False)
        # retry_max=3 → 1 initial + 3 retries = 4 attempts.
        self.assertEqual(len(cap.requests), 4)
        self.assertTrue(plpy.errors)
        self.assertIn("HTTP 500", plpy.errors[-1])

    def test_4xx_not_retried(self):
        client, plpy, cap = make_client(payloads=[self._http_error(404)] * 5)
        with self.assertRaises(RuntimeError):
            client.search("ns", "q", 1, None, False)
        self.assertEqual(len(cap.requests), 1)  # no retry on 4xx
        self.assertIn("HTTP 404", plpy.errors[-1])

    def test_connection_error_retried_then_unreachable(self):
        client, plpy, cap = make_client(
            payloads=[error.URLError("refused")] * 10)
        with self.assertRaises(RuntimeError):
            client.search("ns", "q", 1, None, False)
        self.assertEqual(len(cap.requests), 4)
        self.assertIn("unreachable", plpy.errors[-1])

    def test_backoff_schedule(self):
        self.assertEqual(h.backoff_delays_ms(3), [100, 200, 400])
        self.assertEqual(h.backoff_delays_ms(0), [])

    def test_classify_http_error(self):
        self.assertEqual(h.classify_http_error(503), (True, "http_5xx"))
        self.assertEqual(h.classify_http_error(500), (True, "http_5xx"))
        self.assertEqual(h.classify_http_error(404)[0], False)
        self.assertEqual(h.classify_http_error(401), (False, "http_auth"))
        self.assertEqual(h.classify_http_error(400), (False, "http_4xx"))


# ===========================================================================
# PG cancel (F14 §4.2)
# ===========================================================================


class TestCancel(unittest.TestCase):
    def test_cancel_during_request_raises_pg_error(self):
        # A request that "hangs": urlopen blocks until we observe cancel. We
        # simulate by making the worker slow and the ping raise.
        import time as _t

        plpy = FakePlpy()
        plpy.cancel_on_ping = True

        def slow_urlopen(req, timeout=None):
            _t.sleep(0.25)  # still running when the 100ms ping fires
            return FakeResponse({"results": []})

        client = h.PgcortrixClient(plpy, urlopen=slow_urlopen)
        with self.assertRaises(RuntimeError):
            client.search("ns", "q", 1, None, False)
        self.assertTrue(any("canceled" in e for e in plpy.errors))


# ===========================================================================
# status() (V23 D6)
# ===========================================================================


class TestStatus(unittest.TestCase):
    def test_status_connected(self):
        client, _, _ = make_client(payloads=[{"status": "ok"}])
        s = client.status()
        self.assertTrue(s["http_connected"])
        self.assertEqual(s["version"], "1.0.0")
        self.assertEqual(s["endpoint"], "http://localhost:8420")
        self.assertIn("latency_ms", s)

    def test_status_disconnected_never_raises(self):
        client, _, _ = make_client(payloads=[error.URLError("refused")])
        s = client.status()
        self.assertFalse(s["http_connected"])
        self.assertIn("error", s)

    def test_status_blocked_endpoint_reports_not_connected(self):
        client, _, _ = make_client(
            gucs={"pgcortrix.endpoint": "http://169.254.169.254"})
        s = client.status()
        self.assertFalse(s["http_connected"])


# ===========================================================================
# GD caching (F14 §2.3)
# ===========================================================================


class TestGdCache(unittest.TestCase):
    def test_client_cached_in_gd(self):
        plpy = FakePlpy()
        gd = {}
        c1 = h.get_client(plpy, gd)
        c2 = h.get_client(plpy, gd)
        self.assertIs(c1, c2)
        self.assertIn("pgcortrix_client", gd)


if __name__ == "__main__":
    unittest.main()
