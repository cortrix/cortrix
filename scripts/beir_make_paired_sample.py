#!/usr/bin/env python3
"""Build a reproducible paired BEIR sample for Cortrix retrieval diagnostics.

The output format intentionally matches scripts/rag_fusion_rerank_loop_diag.py:

  sampled_corpus.jsonl
  sampled_queries.jsonl
  sampled_qrels.tsv
  manifest.json

It is not a full BEIR benchmark runner. It is the deterministic sampling gate
that keeps paired profile runs honest: every profile must run over the exact same
corpus/query/qrels slice, with all positive qrels for selected queries included
in the sampled corpus.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import random
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Sequence, Tuple


JsonRow = Mapping[str, object]
Qrel = Tuple[str, str, float]


def load_jsonl(path: Path) -> List[JsonRow]:
    rows: List[JsonRow] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def load_qrels(path: Path) -> List[Qrel]:
    out: List[Qrel] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, start=1):
            line = line.rstrip("\n")
            if not line:
                continue
            if line_no == 1 and line.startswith("query-id\t"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                raise SystemExit(f"invalid qrels row at {path}:{line_no}: {line!r}")
            out.append((parts[0], parts[1], float(parts[2])))
    return out


def stable_query_key(qid: str) -> Tuple[int, object]:
    try:
        return (0, int(qid))
    except ValueError:
        return (1, qid)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def write_jsonl(path: Path, rows: Iterable[JsonRow]) -> None:
    with path.open("w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")


def choose_queries(
    qrels: Sequence[Qrel],
    query_ids: set[str],
    corpus_ids: set[str],
    query_count: int,
    seed: int,
    shuffle: bool,
) -> List[str]:
    by_query: Dict[str, set[str]] = {}
    for qid, docid, score in qrels:
        if score <= 0:
            continue
        if qid not in query_ids or docid not in corpus_ids:
            continue
        by_query.setdefault(qid, set()).add(docid)
    candidates = sorted(by_query, key=stable_query_key)
    if shuffle:
        rng = random.Random(seed)
        rng.shuffle(candidates)
    if query_count > 0:
        candidates = candidates[:query_count]
    if not candidates:
        raise SystemExit("no qrels-backed queries could be selected")
    return candidates


def main() -> int:
    ap = argparse.ArgumentParser(description="Create a deterministic paired BEIR sample.")
    ap.add_argument("--dataset-dir", type=Path, required=True, help="Directory containing corpus.jsonl, queries.jsonl, and qrels/*.tsv")
    ap.add_argument("--output-dir", type=Path, required=True)
    ap.add_argument("--dataset", default="", help="Dataset id for manifest only; defaults to dataset-dir name")
    ap.add_argument("--split", default="test")
    ap.add_argument("--query-count", type=int, default=20)
    ap.add_argument("--negative-docs", type=int, default=200)
    ap.add_argument("--seed", type=int, default=44)
    ap.add_argument("--shuffle-queries", action="store_true")
    args = ap.parse_args()

    corpus_path = args.dataset_dir / "corpus.jsonl"
    queries_path = args.dataset_dir / "queries.jsonl"
    qrels_path = args.dataset_dir / "qrels" / f"{args.split}.tsv"
    for path in (corpus_path, queries_path, qrels_path):
        if not path.exists():
            raise SystemExit(f"missing required BEIR file: {path}")

    corpus_rows = load_jsonl(corpus_path)
    query_rows = load_jsonl(queries_path)
    qrels = load_qrels(qrels_path)

    corpus_by_id = {str(row["_id"]): row for row in corpus_rows if "_id" in row}
    query_by_id = {str(row["_id"]): row for row in query_rows if "_id" in row}
    selected_query_ids = choose_queries(
        qrels,
        set(query_by_id),
        set(corpus_by_id),
        args.query_count,
        args.seed,
        args.shuffle_queries,
    )
    selected_query_set = set(selected_query_ids)

    selected_qrels = [
        (qid, docid, score)
        for qid, docid, score in qrels
        if qid in selected_query_set and score > 0 and docid in corpus_by_id
    ]
    relevant_doc_ids = {docid for _, docid, _ in selected_qrels}
    negative_pool = [docid for docid in sorted(corpus_by_id, key=stable_query_key) if docid not in relevant_doc_ids]
    rng = random.Random(args.seed)
    rng.shuffle(negative_pool)
    negative_doc_ids = set(negative_pool[: max(0, args.negative_docs)])
    sampled_doc_ids = relevant_doc_ids | negative_doc_ids

    sampled_corpus = [corpus_by_id[docid] for docid in sorted(sampled_doc_ids, key=stable_query_key)]
    sampled_queries = [query_by_id[qid] for qid in selected_query_ids]
    sampled_qrels = [row for row in selected_qrels if row[1] in sampled_doc_ids]

    if len(sampled_qrels) != len(selected_qrels):
        raise SystemExit("internal error: selected qrels not fully covered by sampled corpus")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    out_corpus = args.output_dir / "sampled_corpus.jsonl"
    out_queries = args.output_dir / "sampled_queries.jsonl"
    out_qrels = args.output_dir / "sampled_qrels.tsv"
    write_jsonl(out_corpus, sampled_corpus)
    write_jsonl(out_queries, sampled_queries)
    with out_qrels.open("w", encoding="utf-8") as handle:
        handle.write("query-id\tcorpus-id\tscore\n")
        for qid, docid, score in sampled_qrels:
            score_text = str(int(score)) if float(score).is_integer() else str(score)
            handle.write(f"{qid}\t{docid}\t{score_text}\n")

    manifest = {
        "dataset": args.dataset or args.dataset_dir.name,
        "split": args.split,
        "source": {
            "dataset_dir": str(args.dataset_dir),
            "corpus": str(corpus_path),
            "queries": str(queries_path),
            "qrels": str(qrels_path),
            "source_corpus_rows": len(corpus_rows),
            "source_queries": len(query_rows),
            "source_qrels": len(qrels),
        },
        "sampling": {
            "query_count_requested": args.query_count,
            "negative_docs_requested": args.negative_docs,
            "seed": args.seed,
            "shuffle_queries": args.shuffle_queries,
        },
        "sample": {
            "selected_queries": len(sampled_queries),
            "selected_query_ids": selected_query_ids,
            "sampled_corpus_rows": len(sampled_corpus),
            "relevant_doc_count": len(relevant_doc_ids),
            "negative_doc_count": len(negative_doc_ids),
            "sampled_qrels": len(sampled_qrels),
            "all_positive_qrels_covered": True,
        },
        "artifacts": {
            "sampled_corpus": str(out_corpus),
            "sampled_queries": str(out_queries),
            "sampled_qrels": str(out_qrels),
            "sampled_corpus_sha256": sha256_file(out_corpus),
            "sampled_queries_sha256": sha256_file(out_queries),
            "sampled_qrels_sha256": sha256_file(out_qrels),
        },
        "claim_boundary": "paired sample only; not a full BEIR published benchmark by itself",
    }
    out_manifest = args.output_dir / "manifest.json"
    out_manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
