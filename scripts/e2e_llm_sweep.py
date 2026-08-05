#!/usr/bin/env python3
# =============================================================
# Real-LLM end-to-end sweep against a running cortrix-server.
#
# Drives the golden path with every LLM role live (semantic /
# vision / doc_summary / enricher) plus real bge-m3 embedding:
#   P0  health + system/features
#   P1  namespace lifecycle
#   P2  corpus upload (multipart, async SPC) → doc status ready
#   P3  document listing
#   P4  query matrix: plain / rerank / explain / route override /
#       missing-NS + error envelope
#   P5  memory: session / interactions / search / inject
#
# The live wire is the design surface: cross-NS query /query
# (namespaces[] + 8-field meta), memory + agent trace surfaces, CE auth
# bootstrap/api-keys, DB import, batch, GC lifecycle, flat /documents
# + /watch. This sweep covers the design-face shapes.
#
# Usage:
#   python3 scripts/e2e_llm_sweep.py [--base http://localhost:8420] \
#       [--corpus tests/integration/test_data] [--report /tmp/e2e_report.json]
#
# Exit code: 0 if no check FAILed (SKIPs allowed), 1 otherwise.
# =============================================================
import argparse
import json
import sys
import time
from pathlib import Path

import requests

CHECKS = []


def check(name, ok, detail=""):
    CHECKS.append({"name": name, "ok": bool(ok), "detail": str(detail)[:400]})
    mark = "PASS" if ok else "FAIL"
    print(f"[{mark}] {name}" + (f" — {detail}" if (detail and not ok) else ""))
    return ok


def skip(name, why):
    CHECKS.append({"name": name, "ok": None, "detail": why})
    print(f"[SKIP] {name} — {why}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="http://localhost:8420")
    ap.add_argument("--corpus", default="tests/integration/test_data")
    ap.add_argument("--ns", default="e2e-llm")
    ap.add_argument("--report", default="/tmp/e2e_llm_report.json")
    ap.add_argument("--task-timeout", type=int, default=900)
    args = ap.parse_args()
    api = args.base.rstrip("/") + "/api/v1"
    s = requests.Session()
    # The server can shed a keep-alive connection under heavy SPC/extraction
    # load; retry transient connection drops instead of aborting the sweep.
    from requests.adapters import HTTPAdapter, Retry
    s.mount("http://", HTTPAdapter(max_retries=Retry(
        total=2, backoff_factor=0.5, allowed_methods=None,
        status_forcelist=())))

    # ---------- P0 health ----------
    r = s.get(f"{api}/health", timeout=10)
    check("P0 /health 200", r.status_code == 200, r.text)
    print("    health:", r.text[:300])
    r = s.get(f"{api}/system/health/live", timeout=10)
    check("P0 liveness 200", r.status_code == 200, r.text[:120])
    r = s.get(f"{api}/system/health/ready", timeout=10)
    check("P0 readiness 200", r.status_code == 200, r.text[:200])

    r = s.get(f"{api}/system/features", timeout=10)
    if check("P0 /system/features 200", r.status_code == 200, r.text):
        feats = r.json()
        check("P0 edition == ce", feats.get("edition") == "ce", feats.get("edition"))

    # ---------- P1 namespace ----------
    r = s.post(f"{api}/namespaces", json={"name": args.ns}, timeout=15)
    check("P1 create namespace", r.status_code in (200, 201, 409), f"{r.status_code} {r.text[:300]}")
    print("    create ns:", r.status_code, r.text[:300])

    r = s.get(f"{api}/namespaces", timeout=10)
    names = r.text if r.status_code == 200 else ""
    check("P1 namespace listed", r.status_code == 200 and args.ns in names, r.text[:300])

    # ---------- P2 corpus upload (multipart) ----------
    corpus = sorted(Path(args.corpus).glob("*.md"))
    check("P2 corpus present", len(corpus) >= 8, f"{len(corpus)} files")
    doc_ids = {}
    for f in corpus:
        r = s.post(
            f"{api}/namespaces/{args.ns}/documents",
            files={"file": (f.name, f.read_text(), "text/markdown")},
            timeout=60,
        )
        ok = r.status_code in (200, 201)
        body = r.json() if ok else {}
        doc_id = body.get("doc_id")
        check(f"P2 upload {f.name}", ok and doc_id, f"{r.status_code} {r.text[:300]}")
        if doc_id:
            doc_ids[f.name] = doc_id

    deadline = time.time() + args.task_timeout
    pending = dict(doc_ids)
    bad = {}
    last = {}
    while pending and time.time() < deadline:
        for fname, did in list(pending.items()):
            r = s.get(f"{api}/namespaces/{args.ns}/documents/{did}/status", timeout=10)
            if r.status_code != 200:
                last[fname] = f"{r.status_code} {r.text[:120]}"
                continue
            st = r.json().get("status", "")
            last[fname] = st
            if st == "ready":
                del pending[fname]
            elif st in ("error", "unknown"):
                bad[fname] = r.text[:300]
                del pending[fname]
        if pending:
            time.sleep(4)
    check("P2 all docs ready", not pending and not bad,
          f"pending={ {k: last.get(k) for k in pending} } failed={bad}")

    # ---------- P3 document listing ----------
    r = s.get(f"{api}/namespaces/{args.ns}/documents", timeout=10)
    docs = []
    if r.status_code == 200:
        body = r.json()
        docs = body.get("documents", body if isinstance(body, list) else [])
    check("P3 documents listed", len(docs) >= len(doc_ids) - len(bad),
          f"{len(docs)} docs, status {r.status_code} {r.text[:200]}")
    if docs:
        print("    doc[0]:", json.dumps(docs[0])[:300])

    # ---------- P4 query matrix ----------
    def query(payload, params=""):
        return s.post(f"{api}/query{params}", json=payload, timeout=120)

    gold = [
        ("Which planet is the hottest in the Solar System?", "venus"),
        ("Who invented the World Wide Web?", "berners-lee"),
        ("What is the most abundant protein on Earth?", "rubisco"),
        ("Which country is the largest coffee producer?", "brazil"),
        ("How deep is the Challenger Deep?", "10,935"),
    ]
    for q, needle in gold:
        r = query({"query": q, "namespaces": [args.ns], "top_k": 5})
        ok = r.status_code == 200
        hit = ok and needle.lower() in r.text.lower()
        check(f"P4 query hit: {q[:40]}", ok and hit,
              f"{r.status_code} needle={needle} resp={r.text[:200]}")

    q = "Why do plants appear green?"
    # Cross-NS query 8-field A-class meta on the array wire.
    r = query({"query": q, "namespaces": [args.ns], "top_k": 5, "rerank": True})
    if check("P4 F04 wire 200", r.status_code == 200, r.text[:300]):
        meta = r.json().get("meta", {})
        need = {"namespaces_queried", "namespaces_succeeded", "coverage_ratio", "latency_ms"}
        check("P4 F04 meta A-fields", need.issubset(meta.keys()), json.dumps(meta)[:300])
        items = r.json().get("results", [])
        check("P4 F04 item shape (child_id+content)",
              bool(items) and "child_id" in items[0] and "content" in items[0],
              json.dumps(items[0])[:250] if items else "no results")

    # Deprecated MVP singular field is rejected loudly (cross-NS query B' ruling).
    r = query({"query": q, "namespace": args.ns, "top_k": 3})
    dep = r.status_code == 400 and "CX_ERR_DEPRECATED_FIELD" in r.text
    check("P4 singular namespace → CX_ERR_DEPRECATED_FIELD", dep, f"{r.status_code} {r.text[:200]}")

    # Multi-NS + wildcard now served in one call (cross-NS query mounted).
    r = query({"query": q, "namespaces": [args.ns, args.ns], "top_k": 3})
    check("P4 multi-NS 200", r.status_code == 200, f"{r.status_code} {r.text[:200]}")
    r = query({"query": q, "namespaces": ["*"], "top_k": 3})
    check("P4 wildcard NS 200", r.status_code == 200, f"{r.status_code} {r.text[:200]}")

    r = query({"query": q, "namespaces": [args.ns], "top_k": 5}, "?explain=true")
    if check("P4 explain=true 200", r.status_code == 200, r.text[:300]):
        body = r.json()
        check("P4 explain section present", "explain" in body, json.dumps(body)[:250])

    for route in ("simple", "complex", "chat"):
        r = query({"query": q, "namespaces": [args.ns], "top_k": 3}, f"?route={route}")
        ok = r.status_code == 200
        if route == "chat" and ok:
            via = (r.json().get("meta", {}) or {}).get("via_path", "")
            check("P4 chat via_path", via == "chat_memory_only", f"via_path={via}")
        check(f"P4 route={route} 200", ok, f"{r.status_code} {r.text[:200]}")

    # CRAG runs only on the Complex route; with explain the verdict is surfaced.
    # Exercises the CRAG explain branch (crag_verdict / crag_score / ambiguous_action_taken).
    r = query({"query": q, "namespaces": [args.ns], "top_k": 5}, "?route=complex&explain=true")
    if check("P4 F37 complex+explain 200", r.status_code == 200, r.text[:250]):
        ex = (r.json() or {}).get("explain", {}) or {}
        flat = json.dumps(ex)
        # crag_verdict appears somewhere in the explain tree when CRAG ran.
        has_crag = "crag_verdict" in flat
        check("P4 F37 CRAG verdict surfaced in explain", has_crag, flat[:300])
        if has_crag:
            # verdict must be one of the known CRAG classes (not an empty/garbage value).
            verdict_ok = any(v in flat for v in ('"correct"', '"ambiguous"', '"incorrect"', '"disabled"'))
            check("P4 F37 CRAG verdict is a known class", verdict_ok, flat[:300])

    r = query({"query": q, "namespaces": ["no-such-ns-xyz"], "top_k": 3})
    if 400 <= r.status_code < 500:
        err = (r.json() or {}).get("error", {})
        check("P4 missing NS handled", bool(err.get("code")), json.dumps(err)[:200])
    else:
        meta = (r.json() or {}).get("meta", {}) if r.status_code == 200 else {}
        failed = meta.get("namespaces_failed", [])
        cov = meta.get("coverage_ratio", 1.0)
        check("P4 missing NS handled",
              r.status_code == 200 and (failed or cov == 0.0),
              f"{r.status_code} meta={json.dumps(meta)[:200]}")

    r = query({"namespaces": [args.ns]})  # missing query
    check("P4 missing query → 400", r.status_code == 400, f"{r.status_code} {r.text[:200]}")

    # ---------- P5 memory ----------
    r = s.post(f"{api}/memory/sessions",
               json={"namespace": args.ns, "user_id": "alex"}, timeout=15)
    sid = (r.json() or {}).get("session_id") if r.status_code in (200, 201) else None
    check("P5 create memory session", bool(sid), f"{r.status_code} {r.text[:250]}")

    if sid:
        convo = [
            ("My name is Alex and I prefer dark roast coffee.", "Nice to meet you, Alex! Dark roast noted."),
            ("I am allergic to peanuts, please remember that.", "Understood — peanut allergy recorded."),
            ("What coffee would you recommend for me?", "Given your dark roast preference, try a French roast."),
        ]
        ok_all = True
        for qt, rt in convo:
            r = s.post(f"{api}/memory/sessions/{sid}/interactions",
                       json={"namespace": args.ns, "query_text": qt, "response_text": rt,
                             "user_id": "alex"},
                       timeout=60)
            if r.status_code not in (200, 201, 202):
                ok_all = False
                check("P5 write interaction", False, f"{r.status_code} {r.text[:250]}")
                break
        if ok_all:
            check("P5 interactions written", True)

        # Memory extraction is wired (integration r2 M1/M2): the queue drains interactions through
        # the LLM extractor. Poll up to 90s for the extracted fact to surface.
        hit = False
        last_resp = ""
        for _ in range(18):
            time.sleep(5)
            try:
                r = s.post(f"{api}/memory/search",
                           json={"namespace": args.ns, "query": "user food allergy", "top_k": 5,
                                 "user_id": "alex"},
                           timeout=60)
            except requests.exceptions.RequestException as exc:
                last_resp = f"transient: {exc}"
                continue
            last_resp = f"{r.status_code} {r.text[:300]}"
            if r.status_code == 200 and "peanut" in r.text.lower():
                hit = True
                break
        check("P5 memory search finds extracted fact (MEM02)", hit, last_resp)
        print("    memory search:", last_resp[:300])

        # Manual extract endpoint (M2) responds with the PartialSuccessById shape.
        r = s.post(f"{api}/memory/extract",
                   json={"ns": args.ns, "namespace": args.ns, "session_id": sid}, timeout=120)
        check("P5 /memory/extract 2xx", r.status_code in (200, 202, 207), f"{r.status_code} {r.text[:250]}")

        r = s.post(f"{api}/memory/inject",
                   json={"namespace": args.ns, "query": "recommend a coffee for the user",
                         "session_id": sid, "user_id": "alex"},
                   timeout=60)
        check("P5 memory inject 200", r.status_code == 200, f"{r.status_code} {r.text[:250]}")
        print("    inject:", r.text[:250])

        # Memory isolation: list is scoped to the requesting user, so the same
        # user_id the session was created under must be passed.
        r = s.get(f"{api}/memory/sessions",
                  params={"namespace": args.ns, "user_id": "alex"}, timeout=15)
        check("P5 sessions listed", r.status_code == 200 and sid in r.text, r.text[:200])

        # Agent trace observability (M6, per-NS): /interactions is live now.
        r = s.get(f"{api}/interactions", params={"namespace_id": args.ns, "session_id": sid}, timeout=15)
        check("P5 F13 /interactions 200", r.status_code == 200, f"{r.status_code} {r.text[:200]}")
        # Agent trace sources view: if any interaction exists, its /sources sub-resource must
        # respond 200 with a source_count field (exercises the interaction-sources branch).
        if r.status_code == 200:
            inters = (r.json() or {}).get("interactions", [])
            iid = inters[0].get("interaction_id") if inters else None
            if iid:
                # NB: the sources sub-resource uses the `namespace` param (per-NS storage),
                # NOT `namespace_id` like the /interactions list above — a documented agent trace
                # contract addendum (observability_routes.cpp §TC4), so pass `namespace`.
                rs = s.get(f"{api}/interactions/{iid}/sources",
                           params={"namespace": args.ns}, timeout=15)
                check("P5 F13 /interactions/{id}/sources 200 + source_count",
                      rs.status_code == 200 and "source_count" in (rs.text or ""),
                      f"{rs.status_code} {rs.text[:200]}")
        r = s.get(f"{api}/traces/{sid}", params={"namespace": args.ns}, timeout=15)
        check("P5 F13 /traces (ns param) responds", r.status_code in (200, 404), f"{r.status_code} {r.text[:160]}")
        # /traces is the GLOBAL agent_trace read (agent trace / TC4): ?namespace is NOT
        # required (a session's calls span namespaces, aggregated by session_id). So
        # omitting it is NOT a 400 — it behaves like the ns-param case above (200 if the
        # session has traces, 404 CX_ERR_TRACE_SESSION_NOT_FOUND if it has none). The old
        # "missing ns → 400" assertion applied the per-NS /interactions contract here.
        r = s.get(f"{api}/traces/{sid}", timeout=15)
        check("P5 F13 /traces no-ns (global, ns not required) responds", r.status_code in (200, 404), f"{r.status_code} {r.text[:160]}")

        # Agent-trace WRITE path (folds in the 2026-06-24 standalone write check so it
        # is permanent sweep coverage): a /query carrying X-Session-Id/X-Agent-Id makes the
        # engine instrumentation Record() a row into the GLOBAL agent_trace table; that row
        # must then surface at GET /traces/{that_sid} as 200 with total_count >= 1. This also
        # negatively confirms the 404 contract above is "no traces yet" and not a dead route.
        trace_sid = f"qa-trace-{int(time.time())}"
        s.post(f"{api}/query",
               json={"query": "Which planet is the hottest in the Solar System?",
                     "namespaces": [args.ns], "top_k": 3},
               headers={"X-Session-Id": trace_sid, "X-Agent-Id": "qa-e2e-agent"}, timeout=120)
        wrote = False
        for _ in range(8):  # trace write is on the query path; allow brief settle
            r = s.get(f"{api}/traces/{trace_sid}", timeout=15)
            if r.status_code == 200:
                b = r.json() or {}
                # §8.1 body: trace_count = rows on this page; meta.total_count = total match.
                n = b.get("trace_count", 0) or (b.get("meta") or {}).get("total_count", 0)
                if n >= 1:
                    wrote = True
                    break
            time.sleep(2)
        check("P5 F13 agent-trace write (X-Session-Id query → /traces trace_count>=1)",
              wrote, f"last={r.status_code} {r.text[:200]}")

        # Memory transparency CRUD on /memory.
        r = s.post(f"{api}/memory",
                   json={"namespace": args.ns, "content": "User Alex speaks fluent French.",
                         "memory_type": "fact", "user_id": "alex"}, timeout=30)
        mem_id = (r.json() or {}).get("id") or (r.json() or {}).get("memory_id") if r.status_code in (200, 201) else None
        check("P5 MEM03 create 2xx", r.status_code in (200, 201), f"{r.status_code} {r.text[:250]}")
        r = s.get(f"{api}/memory", params={"namespace": args.ns, "user_id": "alex"}, timeout=30)
        check("P5 MEM03 list contains created", r.status_code == 200 and "French" in r.text,
              f"{r.status_code} {r.text[:250]}")
        if mem_id:
            r = s.delete(f"{api}/memory/{mem_id}", params={"namespace": args.ns, "user_id": "alex"}, timeout=30)
            check("P5 MEM03 soft-delete 2xx", r.status_code in (200, 204), f"{r.status_code} {r.text[:200]}")

        # Memory opt-out immunity: opt-out is idempotent; revoke is admin (no-auth grants admin).
        r = s.post(f"{api}/memory/session/{sid}/opt-out", json={"namespace": args.ns}, timeout=30)
        check("P5 MEM04 opt-out 2xx", r.status_code in (200, 201), f"{r.status_code} {r.text[:200]}")
        r = s.post(f"{api}/memory/session/{sid}/opt-out/revoke",
                   json={"namespace": args.ns, "reason": "e2e revoke check"}, timeout=30)
        check("P5 MEM04 revoke responds", r.status_code in (200, 201, 403), f"{r.status_code} {r.text[:200]}")
    else:
        skip("P5 memory suite", "no session id")


    # ---------- P6 platform surfaces (D3.5 r2 Wave P) ----------
    r = s.get(f"{api}/system/version", timeout=10)
    if check("P6 /system/version 200", r.status_code == 200, r.text[:160]):
        check("P6 version == 1.0.0-rc.1", (r.json() or {}).get("version") == "1.0.0-rc.1", r.text[:160])

    # Operation_log read surface (/operations). By now uploads + memory CRUD +
    # namespace creates have logged rows. Exercises: list, namespace_id filter branch,
    # resource_type filter branch — and asserts the ns filter actually scopes results.
    r = s.get(f"{api}/operations", timeout=15)
    if check("P6 F18a /operations 200", r.status_code == 200, f"{r.status_code} {r.text[:200]}"):
        body = r.json() or {}
        ops = body.get("operations", [])
        total = (body.get("meta") or {}).get("total_count", body.get("total_count"))
        check("P6 F18a /operations shape (operations[] + total_count)",
              isinstance(ops, list) and total is not None, json.dumps(body)[:250])
        # namespace_id filter branch: every returned row must match the filter.
        rf = s.get(f"{api}/operations", params={"namespace_id": args.ns}, timeout=15)
        if check("P6 F18a /operations?namespace_id 200", rf.status_code == 200, rf.text[:200]):
            fops = (rf.json() or {}).get("operations", [])
            scoped = all((o.get("namespace_id") in (args.ns, None)) for o in fops)
            check("P6 F18a /operations ns filter scopes rows", scoped,
                  json.dumps([o.get("namespace_id") for o in fops[:10]])[:200])
        # resource_type filter branch (memory ops were logged in P5).
        rt = s.get(f"{api}/operations", params={"resource_type": "memory"}, timeout=15)
        check("P6 F18a /operations?resource_type=memory 200", rt.status_code == 200, rt.text[:200])

    # CE auth api-keys CRUD (no-auth mode grants admin for local integration).
    r = s.post(f"{api}/auth/api-keys", json={"name": "e2e-key"}, timeout=15)
    key_id = None
    if check("P6 api-key create 2xx", r.status_code in (200, 201), f"{r.status_code} {r.text[:250]}"):
        body = r.json() or {}
        key_id = body.get("id") or body.get("key_id")
        check("P6 api-key plaintext returned once", bool(body.get("api_key") or body.get("key")),
              json.dumps({k: v for k, v in body.items() if k not in ("api_key", "key")})[:200])
    r = s.get(f"{api}/auth/api-keys", timeout=15)
    check("P6 api-key list 200 (prefix only)", r.status_code == 200 and "e2e-key" in r.text,
          f"{r.status_code} {r.text[:250]}")
    if key_id:
        r = s.delete(f"{api}/auth/api-keys/{key_id}", timeout=15)
        check("P6 api-key revoke 2xx", r.status_code in (200, 204), f"{r.status_code} {r.text[:160]}")

    # Agent section 6.3 agent_llm_config (GET masked / PUT in-handler admin).
    r = s.get(f"{api}/system/agent_llm_config", timeout=15)
    if check("P6 agent_llm_config GET 200", r.status_code == 200, r.text[:250]):
        body = r.json() or {}
        raw = json.dumps(body)
        key_val = str(body.get("api_key") or body.get("api_key_masked") or "")
        masked = key_val == "" or (("..." in key_val or "****" in key_val) and len(key_val) <= 24)
        check("P6 agent_llm api_key masked", masked, raw[:200])

    # DB import admin connections surface responds (empty registry is fine).
    r = s.get(f"{api}/admin/db-connections", timeout=15)
    check("P6 db-connections list responds", r.status_code in (200, 403), f"{r.status_code} {r.text[:200]}")

    # Batch submit (P3/P3b): two inline docs enter the real pipeline.
    r = s.post(f"{api}/documents/batch",
               json={"namespace": args.ns, "documents": [
                   {"doc_id": "batch_a", "filename": "batch_a.md",
                    "content": "Mount Kilimanjaro is the tallest mountain in Africa."},
                   {"doc_id": "batch_b", "filename": "batch_b.md",
                    "content": "The Great Barrier Reef is the world largest coral reef system."},
               ]}, timeout=60)
    if check("P6 batch submit 2xx", r.status_code in (200, 202), f"{r.status_code} {r.text[:300]}"):
        deadline6 = time.time() + 300
        hit = False
        while time.time() < deadline6 and not hit:
            time.sleep(6)
            rq = query({"query": "tallest mountain in Africa", "namespaces": [args.ns], "top_k": 5})
            hit = rq.status_code == 200 and "kilimanjaro" in rq.text.lower()
        check("P6 batch doc searchable", hit, "Kilimanjaro not retrievable within 300s")

    # ---------- P7 GC + delete lifecycle (OPEN-2, soft-delete 30d) ----------
    victim = next(iter(doc_ids.values()), None)
    if victim:
        r = s.delete(f"{api}/namespaces/{args.ns}/documents/{victim}", timeout=30)
        check("P7 DELETE doc → 204 (soft)", r.status_code in (200, 204), f"{r.status_code} {r.text[:160]}")
        r = s.get(f"{api}/namespaces/{args.ns}/documents/{victim}/status", timeout=15)
        check("P7 deleted doc GET → 404", r.status_code == 404, f"{r.status_code} {r.text[:160]}")

        r = s.get(f"{api}/gc/status", timeout=30)
        if check("P7 /gc/status 200", r.status_code == 200, r.text[:250]):
            sd = (r.json() or {}).get("soft_deleted_count", 0)
            check("P7 gc backlog >= 1", sd >= 1, f"soft_deleted_count={sd}")

        r = s.post(f"{api}/gc/restore", json={"namespace": args.ns, "document_ids": [victim]}, timeout=30)
        check("P7 gc restore 2xx", r.status_code in (200, 207), f"{r.status_code} {r.text[:250]}")
        r = s.get(f"{api}/namespaces/{args.ns}/documents/{victim}/status", timeout=15)
        check("P7 restored doc GET 200", r.status_code == 200, f"{r.status_code} {r.text[:200]}")

        r = s.delete(f"{api}/namespaces/{args.ns}/documents/{victim}", timeout=30)
        r = s.post(f"{api}/gc/purge", json={"namespace": args.ns, "document_ids": [victim]},
                   headers={"X-Ops-Confirm": "true"}, timeout=60)
        # purge is accepted-and-async: 202 (queued for the GC sweeper) is a valid
        # success alongside 200/207. The "not restorable" check below confirms effect.
        check("P7 gc purge 2xx", r.status_code in (200, 202, 207), f"{r.status_code} {r.text[:250]}")
        r = s.post(f"{api}/gc/restore", json={"namespace": args.ns, "document_ids": [victim]}, timeout=30)
        check("P7 purged doc not restorable", r.status_code in (200, 207, 404) and
              ("NOT_FOUND" in r.text.upper() or "failed" in r.text.lower() or r.status_code == 404),
              f"{r.status_code} {r.text[:250]}")
    else:
        skip("P7 GC lifecycle", "no doc available")

    # ---------- P8 design-face routes (flat /documents + /watch) ----------
    r = s.post(f"{api}/documents",
               json={"namespace": args.ns, "filename": "flat_probe.md",
                     "content": "The Amazon River discharges more water than any other river."},
               timeout=60)
    if check("P8 flat POST /documents 2xx", r.status_code in (200, 201, 202), f"{r.status_code} {r.text[:250]}"):
        task_id = (r.json() or {}).get("task_id")
        if task_id:
            r = s.get(f"{api}/documents/tasks/{task_id}/progress", timeout=15)
            check("P8 task progress responds", r.status_code == 200, f"{r.status_code} {r.text[:200]}")
    r = s.get(f"{api}/documents", params={"namespace": args.ns, "limit": 50}, timeout=15)
    check("P8 flat GET /documents 200", r.status_code == 200, f"{r.status_code} {r.text[:200]}")
    r = s.get(f"{api}/watch", timeout=15)
    check("P8 GET /watch 200", r.status_code == 200, f"{r.status_code} {r.text[:200]}")

    # ---------- report ----------
    passed = sum(1 for c in CHECKS if c["ok"] is True)
    failed = sum(1 for c in CHECKS if c["ok"] is False)
    skipped = sum(1 for c in CHECKS if c["ok"] is None)
    report = {"base": args.base, "passed": passed, "failed": failed, "skipped": skipped, "checks": CHECKS}
    Path(args.report).write_text(json.dumps(report, indent=2))
    print(f"\n==== E2E SWEEP: {passed} pass / {failed} fail / {skipped} skip → {args.report} ====")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
