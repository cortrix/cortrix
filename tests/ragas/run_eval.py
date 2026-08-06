#!/usr/bin/env python3
# =============================================================
# Test suite W1: RAGAS evaluation runner (design: test-suite-tests.md)
# Reads tests/ragas/config.yaml, queries a running Cortrix server,
# and scores retrieval quality with RAGAS.
#
# Skips (exit 0 with a SKIP marker) when:
#   - the judge API key env is unset (no LLM judge available), or
#   - the pinned dataset is missing (lands with #483 real-model pass).
# Gate behavior follows thresholds.mode: record-only | enforce.
# =============================================================
import json
import os
import sys
from pathlib import Path

import requests
import yaml

HERE = Path(__file__).parent


def main() -> int:
    cfg = yaml.safe_load((HERE / "config.yaml").read_text())

    key_env = cfg["judge"]["api_key_env"]
    if not os.environ.get(key_env):
        print(f"SKIP: judge api key env {key_env} unset — RAGAS eval needs an LLM judge")
        return 0

    ds_path = HERE.parent.parent / cfg["dataset"]["path"]
    if not ds_path.exists():
        print(f"SKIP: dataset {ds_path} not present yet (pinned set lands with #483)")
        return 0

    items = [json.loads(l) for l in ds_path.read_text().splitlines() if l.strip()]
    if len(items) < cfg["dataset"]["min_items"]:
        print(f"SKIP: dataset has {len(items)} items < min {cfg['dataset']['min_items']}")
        return 0

    base = cfg["target"]["base_url"]
    # Env overrides let CI / local runs retarget without editing the pinned file.
    ns = os.environ.get("RAGAS_NAMESPACE", cfg["target"]["namespace"])
    top_k = cfg["target"]["top_k"]

    questions, answers, contexts, ground_truths = [], [], [], []
    for it in items:
        r = requests.post(
            f"{base}/api/v1/query",
            json={"query": it["question"], "namespaces": [ns], "top_k": top_k},
            timeout=60,
        )
        r.raise_for_status()
        hits = r.json().get("results", [])
        questions.append(it["question"])
        ground_truths.append(it["ground_truth"])
        contexts.append([h.get("content", "") for h in hits])
        answers.append(hits[0].get("content", "") if hits else "")

    from datasets import Dataset
    from ragas import evaluate
    from ragas.metrics import (
        answer_relevancy,
        context_precision,
        context_recall,
        faithfulness,
    )

    metric_map = {
        "context_precision": context_precision,
        "context_recall": context_recall,
        "faithfulness": faithfulness,
        "answer_relevancy": answer_relevancy,
    }
    metrics = [metric_map[m] for m in cfg["metrics"]]

    # Build the judge explicitly from the config (any OpenAI-compatible
    # endpoint); ragas' implicit default would ignore judge.base_url_env and
    # hardcode an OpenAI model name.
    from langchain_openai import ChatOpenAI, OpenAIEmbeddings
    from ragas.llms import LangchainLLMWrapper
    from ragas.embeddings import LangchainEmbeddingsWrapper

    judge_model = os.environ.get("RAGAS_JUDGE_MODEL", cfg["judge"]["model"])
    judge_key = os.environ[key_env]
    judge_base = os.environ.get(cfg["judge"].get("base_url_env", ""), "") or None
    judge_llm = LangchainLLMWrapper(
        ChatOpenAI(model=judge_model, api_key=judge_key, base_url=judge_base, temperature=0)
    )
    embed_model = os.environ.get("RAGAS_EMBED_MODEL", "text-embedding-3-small")
    judge_embeddings = LangchainEmbeddingsWrapper(
        OpenAIEmbeddings(model=embed_model, api_key=judge_key, base_url=judge_base,
                         check_embedding_ctx_length=False)
    )

    ds = Dataset.from_dict(
        {
            "question": questions,
            "answer": answers,
            "contexts": contexts,
            "ground_truth": ground_truths,
        }
    )
    result = evaluate(ds, metrics=metrics, llm=judge_llm, embeddings=judge_embeddings)
    # ragas >= 0.2 returns EvaluationResult (no .items()); aggregate per-metric
    # means from the per-sample frame, older versions stay dict-like.
    if hasattr(result, "items"):
        scores = {k: float(v) for k, v in result.items()}
    else:
        df = result.to_pandas()
        metric_cols = [m.name for m in metrics if m.name in df.columns]
        scores = {c: float(df[c].mean()) for c in metric_cols}
    print(json.dumps(scores, indent=2))

    th = cfg["thresholds"]
    if th.get("mode") == "enforce":
        failed = [
            f"{name} {scores[name]:.3f} < {th[name]}"
            for name in ("context_precision", "context_recall")
            if name in scores and scores[name] < th[name]
        ]
        if failed:
            print("RAGAS GATE FAIL: " + "; ".join(failed))
            return 1
    print("RAGAS: " + ("PASS" if th.get("mode") == "enforce" else "recorded"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
