#!/usr/bin/env python3
"""Minimal F36/F02 rerank-loop diagnostic over an existing sampled BEIR slice.

This is intentionally a local diagnostic harness, not the F44 published benchmark
runner. It runs several query profiles against one Cortrix namespace and writes
machine-readable artifacts so we can see whether LLM expansion + final rerank can
move Recall@K / NDCG@K toward an 0.80 target on a small controlled sample.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, MutableMapping, Optional


def post_json(base: str, path: str, body: Mapping[str, object], timeout: float = 120.0) -> Mapping[str, object]:
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        base + path,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def get_json(base: str, path: str, timeout: float = 30.0) -> Mapping[str, object]:
    with urllib.request.urlopen(base + path, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def load_jsonl(path: Path) -> List[Mapping[str, object]]:
    rows: List[Mapping[str, object]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def load_queries(path: Path) -> Dict[str, str]:
    out: Dict[str, str] = {}
    for row in load_jsonl(path):
        out[str(row["_id"])] = str(row.get("text", ""))
    return out


def load_qrels(path: Path) -> Dict[str, Dict[str, float]]:
    qrels: Dict[str, Dict[str, float]] = {}
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, start=1):
            if line_no == 1 and line.startswith("query-id\t"):
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 3:
                qrels.setdefault(parts[0], {})[parts[1]] = float(parts[2])
    return qrels


def safe_name(value: str) -> str:
    return "".join(ch if ch.isalnum() else "-" for ch in value.lower())[:63].strip("-")


def corpus_to_docs(dataset: str, rows: Iterable[Mapping[str, object]]) -> List[Mapping[str, object]]:
    docs: List[Mapping[str, object]] = []
    for row in rows:
        doc_id = str(row.get("_id", ""))
        title = str(row.get("title", "") or "")
        text = str(row.get("text", "") or "")
        content = f"{title}\n\n{text}" if title else text
        docs.append(
            {
                "doc_id": doc_id,
                "filename": f"{dataset}-{safe_name(doc_id)}.txt",
                "content": content,
                "metadata": {
                    "beir_dataset": dataset,
                    "beir_corpus_id": doc_id,
                    "beir_title": title,
                },
            }
        )
    return docs


def extract_doc_ids(response: Mapping[str, object]) -> List[str]:
    out: List[str] = []
    seen: set[str] = set()
    for item in response.get("results", []):
        if not isinstance(item, dict):
            continue
        candidates: List[str] = []
        meta = item.get("metadata")
        if isinstance(meta, dict):
            for key in ("beir_corpus_id", "source_doc_id", "hybrid_doc_id", "doc_id"):
                value = meta.get(key)
                if isinstance(value, str):
                    candidates.append(value)
            source_path = meta.get("source_path")
            if isinstance(source_path, str):
                match = re.search(r"(?:^|[-_/])([A-Za-z0-9_.-]+)\.txt$", source_path)
                if match:
                    stem = match.group(1)
                    candidates.append(stem)
                    if "-" in stem:
                        candidates.append(stem.split("-", 1)[1])
        for key in ("doc_id", "child_id"):
            value = item.get(key)
            if isinstance(value, str):
                candidates.append(value)
        for value in candidates:
            if value and value not in seen:
                seen.add(value)
                out.append(value)
                break
    return out


def extract_rag_state(response: Mapping[str, object]) -> Mapping[str, object]:
    explain = response.get("explain")
    if not isinstance(explain, dict):
        return {}
    features = explain.get("llm_dependent_features")
    if not isinstance(features, dict):
        return {}
    rag = features.get("rag_fusion")
    return rag if isinstance(rag, dict) else {}


def extract_llm_rerank_state(response: Mapping[str, object]) -> Mapping[str, object]:
    explain = response.get("explain")
    if not isinstance(explain, dict):
        return {}
    features = explain.get("llm_dependent_features")
    if not isinstance(features, dict):
        return {}
    lr = features.get("llm_rerank")
    return lr if isinstance(lr, dict) else {}


def extract_query_failure(response: Mapping[str, object]) -> Mapping[str, object]:
    meta = response.get("meta")
    if not isinstance(meta, dict):
        return {}
    failed = meta.get("namespaces_failed")
    if not isinstance(failed, list) or not failed:
        return {}
    failures = []
    for item in failed:
        if not isinstance(item, dict):
            continue
        failures.append(
            {
                "namespace": item.get("namespace"),
                "error_code": item.get("error_code"),
                "message": item.get("message"),
                "category": item.get("category"),
                "retryable": item.get("retryable"),
                "structured_data": item.get("structured_data"),
            }
        )
    return {"namespaces_failed": failures}


def dcg(gains: List[float]) -> float:
    return sum(g / math.log2(i + 2) for i, g in enumerate(gains))


def recall_for(qrels: Mapping[str, float], docs: List[str], k: int) -> float:
    relevant = {doc for doc, score in qrels.items() if score > 0}
    if not relevant:
        return 0.0
    return len(relevant.intersection(docs[:k])) / len(relevant)


def ndcg_for(qrels: Mapping[str, float], docs: List[str], k: int) -> float:
    gains = [float(qrels.get(doc, 0.0)) for doc in docs[:k]]
    ideal = sorted((float(v) for v in qrels.values()), reverse=True)[:k]
    denom = dcg(ideal)
    return 0.0 if denom <= 0 else dcg(gains) / denom


def mean(values: Iterable[float]) -> float:
    vals = list(values)
    return statistics.mean(vals) if vals else 0.0


def poll_tasks(base: str, task_ids: List[str], timeout_s: float, interval_s: float) -> Dict[str, Mapping[str, object]]:
    deadline = time.time() + timeout_s
    final: Dict[str, Mapping[str, object]] = {}
    pending = set(task_ids)
    while pending and time.time() < deadline:
        for task_id in list(pending):
            try:
                item = get_json(base, f"/api/v1/documents/tasks/{task_id}/progress", timeout=10)
            except urllib.error.HTTPError as exc:
                item = {"status": f"http_{exc.code}"}
            status = str(item.get("status", "")).lower()
            if status in {"completed", "failed", "error", "cancelled"}:
                final[task_id] = item
                pending.remove(task_id)
        if pending:
            time.sleep(interval_s)
    if pending:
        raise SystemExit(f"task polling timed out; pending={len(pending)}")
    return final


def create_namespace(base_url: str, namespace: str) -> None:
    try:
        post_json(base_url, "/api/v1/namespaces", {"name": namespace}, timeout=30)
    except urllib.error.HTTPError as exc:
        if exc.code != 409:
            raise


def ingest_sample(
    base_url: str,
    namespace: str,
    docs: List[Mapping[str, object]],
    batch_size: int,
    timeout_s: float,
    interval_s: float,
) -> Dict[str, int]:
    task_ids: List[str] = []
    for start in range(0, len(docs), batch_size):
        batch = docs[start : start + batch_size]
        resp = post_json(base_url, "/api/v1/documents/batch", {"namespace": namespace, "documents": batch})
        for item in resp.get("results", []):
            if isinstance(item, dict) and item.get("task_id"):
                task_ids.append(str(item["task_id"]))
        print(f"[import] docs={min(start + len(batch), len(docs))} tasks={len(task_ids)}", flush=True)

    final = poll_tasks(base_url, task_ids, timeout_s, interval_s)
    counts: Dict[str, int] = {}
    for item in final.values():
        status = str(item.get("status", "")).lower()
        counts[status] = counts.get(status, 0) + 1
    if any(status != "completed" for status in counts):
        raise SystemExit(f"ingest did not fully complete: {counts}")
    return counts


def profile_matrix(
    max_candidates: int,
    rag_fusion_timeout_ms: int,
    activation_score_margin: float,
    activation_min_results: int,
    llm_rerank_top_n: int = 20,
    llm_rerank_model: str = "",
    llm_rerank_timeout_ms: int = 60000,
    llm_rerank_consensus_runs: int = 1,
    rag_fusion_model: str = "",
) -> Dict[str, Mapping[str, object]]:
    dense_search_config = {
        "enable_vector": True,
        "enable_bm25": False,
        "enable_sparse": False,
    }
    llm_rerank_config = {
        "enabled": True,
        "top_n": llm_rerank_top_n,
        "timeout_ms": llm_rerank_timeout_ms,
        "locale": "en",
        "consensus_runs": llm_rerank_consensus_runs,
    }
    if llm_rerank_model:
        llm_rerank_config["model"] = llm_rerank_model
    # v2 window: widened listwise (§3.5.2 sliding window) + doc-dedup slots via
    # top_k=20 (metric layer dedups doc ids and keeps @10).
    llm_rerank_config_v2 = dict(llm_rerank_config)
    llm_rerank_config_v2["top_n"] = 30
    rag_fusion_config_variants = {
        "enabled": True,
        "locale": "en",
        "timeout_ms": rag_fusion_timeout_ms,
        "candidate_multiplier": 3,
        "max_candidates": max_candidates,
        "final_rerank": True,
    }
    if rag_fusion_model:
        rag_fusion_config_variants["model"] = rag_fusion_model
    return {
        "baseline_rrf": {
            "rerank": False,
            "rag_fusion": False,
        },
        "dense_only": {
            "rerank": False,
            "rag_fusion": False,
            "search_config": dense_search_config,
        },
        "dense_rerank": {
            "rerank": True,
            "rag_fusion": False,
            "search_config": dense_search_config,
        },
        "rerank_only": {
            "rerank": True,
            "rag_fusion": False,
        },
        "llm_guarded_rrf": {
            "rerank": False,
            "rag_fusion": True,
            "rag_fusion_config": {
                "enabled": True,
                "locale": "en",
                "timeout_ms": rag_fusion_timeout_ms,
                "candidate_multiplier": 1,
                "max_candidates": max_candidates,
                "final_rerank": False,
            },
        },
        "llm_m2_final_rerank": {
            "rerank": True,
            "rag_fusion": True,
            "rag_fusion_config": {
                "enabled": True,
                "locale": "en",
                "timeout_ms": rag_fusion_timeout_ms,
                "candidate_multiplier": 2,
                "max_candidates": max_candidates,
                "final_rerank": True,
            },
        },
        "llm_m3_final_rerank": {
            "rerank": True,
            "rag_fusion": True,
            "rag_fusion_config": {
                "enabled": True,
                "locale": "en",
                "timeout_ms": rag_fusion_timeout_ms,
                "candidate_multiplier": 3,
                "max_candidates": max_candidates,
                "final_rerank": True,
            },
        },
        "dense_llm_m3_final_rerank": {
            "rerank": True,
            "rag_fusion": True,
            "search_config": dense_search_config,
            "rag_fusion_config": {
                "enabled": True,
                "locale": "en",
                "timeout_ms": rag_fusion_timeout_ms,
                "candidate_multiplier": 3,
                "max_candidates": max_candidates,
                "final_rerank": True,
            },
        },
        # Attribution control for F36-LR: same widened candidate window as the
        # listwise profiles (top_k = llm_rerank top_n) but NO LLM — the metric
        # trim to args.top_k keeps the CE-ordered head. D vs this isolates the
        # LLM's pure ORDERING contribution; this vs dense_rerank isolates the
        # window effect.
        "dense_rerank_w20": {
            "rerank": True,
            "rag_fusion": False,
            "search_config": dense_search_config,
            "top_k": llm_rerank_top_n,
        },
        # F36-LR §3 profile D: strict dense candidates + F02 CE + LLM listwise
        # rerank — isolates the LLM's direct ORDERING contribution vs dense_rerank.
        "dense_rerank_llm_listwise": {
            "rerank": True,
            "rag_fusion": False,
            "search_config": dense_search_config,
            "llm_rerank": True,
            "llm_rerank_config": llm_rerank_config,
        },
        # F36-LR §3 profile E: full LLM path — F36 variants (candidate side) +
        # CE + LLM listwise (ordering side).
        "dense_llm_full_listwise": {
            "rerank": True,
            "rag_fusion": True,
            "search_config": dense_search_config,
            "rag_fusion_config": rag_fusion_config_variants,
            "llm_rerank": True,
            "llm_rerank_config": llm_rerank_config,
        },
        # §3.5.5 attribution control: CE-only over the same widened pool as v2
        # (top_k=30). Metric layer trims to @10; artifacts keep the full list so
        # Recall@20/@30 ceilings are measurable offline.
        "dense_rerank_w30": {
            "rerank": True,
            "rag_fusion": False,
            "search_config": dense_search_config,
            "top_k": 30,
        },
        # §3.5.5 D-v2: sliding-window listwise over top 30 + presentation-order
        # consensus + doc-dedup slots (top_k=20 response, metric dedups to @10).
        "dense_rerank_llm_listwise_v2": {
            "rerank": True,
            "rag_fusion": False,
            "search_config": dense_search_config,
            "top_k": 20,
            "llm_rerank": True,
            "llm_rerank_config": llm_rerank_config_v2,
        },
        # §3.5.5 E-v2: F36 variants widen candidates + v2 listwise ordering.
        "dense_llm_full_listwise_v2": {
            "rerank": True,
            "rag_fusion": True,
            "search_config": dense_search_config,
            "rag_fusion_config": rag_fusion_config_variants,
            "top_k": 20,
            "llm_rerank": True,
            "llm_rerank_config": llm_rerank_config_v2,
        },
        # §3.5.5 H-v2: hybrid candidate stream (dense + BM25 [+ sparse]) + CE +
        # v2 listwise ordering. Stage-A2 found the dense route's recall ceiling
        # (Recall@20 == Recall@30); BM25 adds lexical-match candidates the dense
        # route misses, and the LLM final ordering absorbs the score-scale mix
        # that used to make hybrid WORSE under pure RRF/CE ordering.
        "hybrid_rerank_llm_listwise_v2": {
            "rerank": True,
            "rag_fusion": False,
            "top_k": 20,
            "llm_rerank": True,
            "llm_rerank_config": llm_rerank_config_v2,
        },
        "llm_m3_selective_final_rerank": {
            "rerank": True,
            "rag_fusion": True,
            "rag_fusion_config": {
                "enabled": True,
                "locale": "en",
                "timeout_ms": rag_fusion_timeout_ms,
                "candidate_multiplier": 3,
                "max_candidates": max_candidates,
                "final_rerank": True,
                "activation_policy": "selective_margin",
                "activation_score_margin": activation_score_margin,
                "activation_min_results": activation_min_results,
            },
        },
        "llm_m5_final_rerank": {
            "rerank": True,
            "rag_fusion": True,
            "rag_fusion_config": {
                "enabled": True,
                "locale": "en",
                "timeout_ms": rag_fusion_timeout_ms,
                "candidate_multiplier": 5,
                "max_candidates": max_candidates,
                "final_rerank": True,
            },
        },
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="Run a local F36/F02 rerank-loop diagnostic.")
    ap.add_argument("--base-url", default="http://127.0.0.1:18485")
    ap.add_argument("--sample-dir", type=Path, required=True)
    ap.add_argument("--work-dir", type=Path, default=Path("/tmp/cortrix-min-llm-bench"))
    ap.add_argument("--run-id", default=time.strftime("f36-rerank-loop-%Y%m%dT%H%M%SZ", time.gmtime()))
    ap.add_argument("--dataset", default="scifact")
    ap.add_argument("--top-k", type=int, default=10)
    ap.add_argument(
        "--granularity",
        choices=("auto", "chunk", "doc", "both"),
        default="both",
        help="Query granularity for all profiles; use chunk for strict retrieval/rerank/LLM ablations.",
    )
    ap.add_argument(
        "--crag",
        choices=("true", "false"),
        default="true",
        help="Enable F37 CRAG post-processing; use false for pure retrieval/rerank/LLM ablations.",
    )
    ap.add_argument("--batch-size", type=int, default=30)
    ap.add_argument("--max-candidates", type=int, default=50)
    ap.add_argument("--rag-fusion-timeout-ms", type=int, default=15000)
    ap.add_argument("--activation-score-margin", type=float, default=0.0)
    ap.add_argument("--activation-min-results", type=int, default=10)
    ap.add_argument("--llm-rerank-top-n", type=int, default=20)
    ap.add_argument("--llm-rerank-model", default="",
                    help="Per-call model override for the F36-LR listwise rerank profiles.")
    ap.add_argument("--llm-rerank-timeout-ms", type=int, default=60000)
    ap.add_argument("--llm-rerank-consensus-runs", type=int, default=1,
                    help="Presentation-order consensus votes per listwise window (1-3).")
    ap.add_argument("--rag-fusion-model", default="",
                    help="Per-call model override for F36 variant generation profiles.")
    ap.add_argument("--target-score", type=float, default=0.80)
    ap.add_argument("--namespace", default="")
    ap.add_argument("--max-queries", type=int, default=0)
    ap.add_argument(
        "--profiles",
        default="",
        help="Comma-separated profile names to run; baseline_rrf is auto-added when omitted.",
    )
    ap.add_argument("--query-timeout-seconds", type=float, default=180.0)
    ap.add_argument("--skip-ingest", action="store_true")
    ap.add_argument("--allow-query-failures", action="store_true")
    ap.add_argument("--poll-timeout-seconds", type=float, default=900.0)
    ap.add_argument("--poll-interval-seconds", type=float, default=1.0)
    args = ap.parse_args()

    run_dir = args.work_dir / "runs" / args.run_id
    run_dir.mkdir(parents=True, exist_ok=True)

    corpus_path = args.sample_dir / "sampled_corpus.jsonl"
    queries_path = args.sample_dir / "sampled_queries.jsonl"
    qrels_path = args.sample_dir / "sampled_qrels.tsv"

    health = get_json(args.base_url, "/api/v1/health")
    readiness = get_json(args.base_url, "/api/v1/system/health/ready")

    namespace = args.namespace or safe_name(f"f36-loop-{args.dataset}-{args.run_id}")
    create_namespace(args.base_url, namespace)

    docs = corpus_to_docs(args.dataset, load_jsonl(corpus_path))
    task_status_counts: Dict[str, int] = {}
    if not args.skip_ingest:
        task_status_counts = ingest_sample(
            args.base_url,
            namespace,
            docs,
            args.batch_size,
            args.poll_timeout_seconds,
            args.poll_interval_seconds,
        )

    queries = load_queries(queries_path)
    if args.max_queries > 0:
        queries = dict(list(queries.items())[: args.max_queries])
    qrels = load_qrels(qrels_path)
    profiles = profile_matrix(
        args.max_candidates,
        args.rag_fusion_timeout_ms,
        args.activation_score_margin,
        args.activation_min_results,
        args.llm_rerank_top_n,
        args.llm_rerank_model,
        args.llm_rerank_timeout_ms,
        args.llm_rerank_consensus_runs,
        args.rag_fusion_model,
    )
    if args.profiles:
        requested_profiles = [p.strip() for p in args.profiles.split(",") if p.strip()]
        if "baseline_rrf" not in requested_profiles:
            requested_profiles.insert(0, "baseline_rrf")
        unknown_profiles = sorted(set(requested_profiles).difference(profiles))
        if unknown_profiles:
            raise SystemExit(f"unknown profile(s): {', '.join(unknown_profiles)}")
        profiles = {name: profiles[name] for name in requested_profiles}
    if "baseline_rrf" not in profiles:
        raise SystemExit("profiles must include baseline_rrf so deltas can be computed")
    runs: Dict[str, Dict[str, List[str]]] = {name: {} for name in profiles}
    latencies: Dict[str, List[float]] = {name: [] for name in profiles}
    rag_states: Dict[str, List[Mapping[str, object]]] = {name: [] for name in profiles}
    lr_states: Dict[str, List[Mapping[str, object]]] = {name: [] for name in profiles}
    query_rows: List[Mapping[str, object]] = []

    for qid, qtext in queries.items():
        for name, switches in profiles.items():
            print(f"[query-profile] {qid} {name} start", flush=True)
            body: MutableMapping[str, object] = {
                "query": qtext,
                "namespaces": [namespace],
                "top_k": args.top_k,
                "route": "complex",
                "granularity": args.granularity,
                "crag": args.crag == "true",
                "locale": "en",
                "explain": True,
            }
            body.update(switches)
            started = time.perf_counter()
            response = post_json(
                args.base_url,
                f"/api/v1/query?granularity={args.granularity}&crag={args.crag}&explain=true",
                body,
                timeout=args.query_timeout_seconds,
            )
            latency_ms = (time.perf_counter() - started) * 1000.0
            query_failure = extract_query_failure(response)
            if query_failure and not args.allow_query_failures:
                detail = json.dumps(
                    {
                        "query_id": qid,
                        "profile": name,
                        "query_failure": query_failure,
                    },
                    ensure_ascii=False,
                )
                raise SystemExit(f"query failed; aborting invalid benchmark run: {detail}")
            # Keep the FULL deduped doc list in artifacts (§3.5.5 — offline
            # Recall@20/@30 ceilings); metric helpers slice to @k themselves.
            docs_out = extract_doc_ids(response)
            rag = extract_rag_state(response)
            lr = extract_llm_rerank_state(response)
            rag_reason = None
            if rag:
                rag_reason = rag.get("reason") or rag.get("degrade_reason")
            runs[name][qid] = docs_out
            latencies[name].append(latency_ms)
            rag_states[name].append(rag)
            lr_states[name].append(lr)
            query_rows.append(
                {
                    "query_id": qid,
                    "profile": name,
                    "latency_ms": latency_ms,
                    "retrieved_doc_ids": docs_out,
                    "rag_fusion": rag,
                    "rag_reason": rag_reason,
                    "rag_degrade_reason": rag.get("degrade_reason") if rag else None,
                    "rag_degrade_detail": rag.get("degrade_detail") if rag else None,
                    "rag_variant_count": rag.get("variant_count") if rag else None,
                    "llm_rerank": lr,
                    "query_failure": query_failure,
                    "body_switches": switches,
                }
            )
            print(
                f"[query-profile] {qid} {name} done docs={len(docs_out)} "
                f"rag_reason={rag_reason or ''} "
                f"rag_detail={rag.get('degrade_detail') if rag else ''} "
                f"rag_variants={rag.get('variant_count') if rag else ''} "
                f"lr_active={lr.get('active') if lr else ''} "
                f"lr_changed={lr.get('order_changed') if lr else ''} "
                f"lr_degrade={lr.get('degrade_reason') if lr else ''} "
                f"latency_ms={latency_ms:.1f}",
                flush=True,
            )
        print(f"[query] {qid}", flush=True)

    recall: Dict[str, Dict[str, float]] = {name: {} for name in profiles}
    ndcg: Dict[str, Dict[str, float]] = {name: {} for name in profiles}
    for name in profiles:
        for qid in queries:
            recall[name][qid] = recall_for(qrels.get(qid, {}), runs[name].get(qid, []), args.top_k)
            ndcg[name][qid] = ndcg_for(qrels.get(qid, {}), runs[name].get(qid, []), args.top_k)

    baseline = "baseline_rrf"
    per_query = []
    for qid in queries:
        row: Dict[str, object] = {"query_id": qid}
        base_docs = runs[baseline].get(qid, [])
        for name in profiles:
            docs_out = runs[name].get(qid, [])
            row[name] = {
                "recall": recall[name][qid],
                "ndcg": ndcg[name][qid],
                "docs": docs_out,
                "order_changed_vs_baseline": docs_out != base_docs,
            }
        per_query.append(row)

    def summarize(name: str) -> Mapping[str, object]:
        states = rag_states[name]
        lrs = lr_states[name]
        mean_recall = mean(recall[name].values())
        mean_ndcg = mean(ndcg[name].values())
        degrade_reasons: Dict[str, int] = {}
        degrade_details: Dict[str, int] = {}
        for state in states:
            reason = state.get("degrade_reason") if state else None
            if isinstance(reason, str) and reason:
                degrade_reasons[reason] = degrade_reasons.get(reason, 0) + 1
            detail = state.get("degrade_detail") if state else None
            if isinstance(detail, str) and detail:
                degrade_details[detail] = degrade_details.get(detail, 0) + 1
        lr_degrade_reasons: Dict[str, int] = {}
        for state in lrs:
            reason = state.get("degrade_reason") if state else None
            if isinstance(reason, str) and reason:
                lr_degrade_reasons[reason] = lr_degrade_reasons.get(reason, 0) + 1
        return {
            f"recall@{args.top_k}": mean_recall,
            f"ndcg@{args.top_k}": mean_ndcg,
            "target_80_hit": mean_recall >= args.target_score or mean_ndcg >= args.target_score,
            "latency_ms_mean": mean(latencies[name]),
            "latency_ms_p50": statistics.median(latencies[name]) if latencies[name] else 0.0,
            "queries": len(queries),
            "queries_with_recall_hit": sum(1 for value in recall[name].values() if value > 0),
            "order_changed_vs_baseline": sum(1 for qid in queries if runs[name].get(qid, []) != runs[baseline].get(qid, [])),
            "rag_active_count": sum(1 for s in states if bool(s.get("active"))),
            "rag_degraded_count": sum(1 for s in states if bool(s.get("degraded"))),
            "rag_degrade_reasons": degrade_reasons,
            "rag_degrade_details": degrade_details,
            "rag_variant_counts": sorted({int(s.get("variant_count", 0)) for s in states if s}),
            "llm_rerank_active_count": sum(1 for s in lrs if bool(s.get("active"))),
            "llm_rerank_degraded_count": sum(1 for s in lrs if bool(s.get("degraded"))),
            "llm_rerank_order_changed_count": sum(1 for s in lrs if bool(s.get("order_changed"))),
            "llm_rerank_degrade_reasons": lr_degrade_reasons,
        }

    profile_summary = {name: summarize(name) for name in profiles}
    best_recall_profile = max(profile_summary, key=lambda name: float(profile_summary[name][f"recall@{args.top_k}"]))
    best_ndcg_profile = max(profile_summary, key=lambda name: float(profile_summary[name][f"ndcg@{args.top_k}"]))

    summary = {
        "run_id": args.run_id,
        "dataset": args.dataset,
        "namespace": namespace,
        "granularity": args.granularity,
        "crag_enabled": args.crag == "true",
        "server": {"health": health, "readiness": readiness},
        "sample": {
            "sampled_corpus_path": str(corpus_path),
            "sampled_queries_path": str(queries_path),
            "sampled_qrels_path": str(qrels_path),
            "actual_corpus_rows": len(docs),
            "selected_queries": len(queries),
        },
        "imported_docs": 0 if args.skip_ingest else len(docs),
        "task_status_counts": task_status_counts,
        "profiles": profile_summary,
        "best": {
            "by_recall": best_recall_profile,
            "by_ndcg": best_ndcg_profile,
            f"best_recall@{args.top_k}": profile_summary[best_recall_profile][f"recall@{args.top_k}"],
            f"best_ndcg@{args.top_k}": profile_summary[best_ndcg_profile][f"ndcg@{args.top_k}"],
            "target_score": args.target_score,
        },
        "artifacts": {
            "query_rows": str(run_dir / "query_rows.jsonl"),
            "per_query": str(run_dir / "per_query.json"),
            "summary": str(run_dir / "summary.json"),
        },
        "claim_boundary": "local diagnostic only; not a published BEIR benchmark result",
    }

    with (run_dir / "query_rows.jsonl").open("w", encoding="utf-8") as handle:
        for row in query_rows:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")
    (run_dir / "per_query.json").write_text(json.dumps(per_query, ensure_ascii=False, indent=2), encoding="utf-8")
    (run_dir / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
