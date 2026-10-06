---
title: Configuration reference
linkTitle: Configuration
description: Every key in config.yaml, with its default.
weight: 30
---

Every key in the configuration file is listed here. Defaults come from `config.yaml.example` in the `v1.0.0-rc.2` repository.

For how to configure Cortrix and for common scenarios, see [Configuration](/docs/deploy/configuration/). When a key also has an environment variable, the environment variable wins.

## server {#server}

| Key | Default | Description |
|---|---|---|
| `host` | `"127.0.0.1"` | The address to listen on. Binding to a non-loopback address requires authentication to be on |
| `port` | `8420` | The HTTP port |
| `thread_count` | `4` | Worker threads. The recommended value is your CPU core count. Environment variable: `CORTRIX_SERVER_THREADS` |

## auth {#auth}

| Key | Default | Description |
|---|---|---|
| `enabled` | `false` | Whether API key authentication is on. With it off, only a loopback bind is accepted |
| `api_keys[].key_hash` | — | The SHA-256 of the key |
| `api_keys[].tenant_id` | — | The tenant the key belongs to |
| `api_keys[].permissions` | — | Permission bits: `READ=1`, `WRITE=2`, `ADMIN=4`. `7` is all of them |
| `api_keys[].expires_at` | `0` | Expiry time. `0` never expires |

## log {#log}

| Key | Default | Description |
|---|---|---|
| `level` | `"info"` | `trace`, `debug`, `info`, `warn`, `error`, `critical`, or `off` |
| `format` | `"text"` | `text` or `json` |
| `output` | `"stdout"` | `stdout`, `stderr`, or a file path |

## namespace {#namespace}

| Key | Default | Description |
|---|---|---|
| `data_dir` | `"./build/data"` | The data root, holding databases, the vector index, and files. A relative path is resolved from the directory of the `cortrix-server` binary |
| `max_active` | `10` | The maximum number of namespaces kept loaded at once |
| `idle_timeout_s` | `300` | Seconds of idleness before a namespace is unloaded automatically. `0` never unloads |

## embedding {#embedding}

| Key | Default | Description |
|---|---|---|
| `model_path` | `"./models/bge-m3/model.onnx"` | The embedding model path. Leave it empty to select the stub explicitly |
| `tokenizer_path` | `"./models/bge-m3/tokenizer.json"` | The tokenizer path |
| `dimension` | `1024` | The vector dimension. Fixed at 1024 for bge-m3 and can't be changed |
| `max_seq_length` | `512` | The maximum tokens per encode |
| `execution_provider` | `"auto"` | `auto`, `cpu`, `coreml`, or `cuda`. Environment variable: `CORTRIX_EMBEDDING_EXECUTION_PROVIDER` |

## reranker {#reranker}

| Key | Default | Description |
|---|---|---|
| `model_dir` | `"./models/bge-reranker-v2-m3"` | The reranker model directory. Leave it empty to select the stub explicitly. Environment variable: `CORTRIX_RERANKER_MODEL_DIR` |
| `execution_provider` | `"auto"` | `auto`, `cpu`, `coreml`, or `cuda`. Environment variable: `CORTRIX_RERANKER_EXECUTION_PROVIDER` |

## query_complexity {#query-complexity}

| Key | Default | Description |
|---|---|---|
| `model_dir` | `"./models/query-complexity"` | The directory of the query-complexity classifier. If the model is absent, a heuristic is used instead. Environment variable: `CORTRIX_QUERY_COMPLEXITY_MODEL_DIR` |

## retrieval {#retrieval}

| Key | Default | Description |
|---|---|---|
| `candidate_multiplier` | `3` | How many times `top_k` the candidate pool is, at least 1. Environment variable: `CORTRIX_RETRIEVAL_CANDIDATE_MULTIPLIER` |
| `max_candidates` | `50` | The hard ceiling on the candidate pool, at least 1. Environment variable: `CORTRIX_RETRIEVAL_MAX_CANDIDATES` |

See [Performance tuning](/docs/deploy/performance-tuning/#candidate-pool).

## spc {#spc}

The document processing pipeline. It supports PDF, DOCX, TXT, MD, CSV, JSON, and images (through OCR).

| Key | Default | Description |
|---|---|---|
| `worker_count` | `2` | Threads processing in parallel. 2 to 4 is recommended |
| `chunk_size` | `512` | The target number of tokens per text block |
| `chunk_overlap` | `50` | Tokens shared between neighboring blocks |
| `embedding_batch_size` | `1` | Blocks embedded per batch |
| `onnx_intra_threads` | `4` | Intra-op threads for ONNX inference |
| `onnx_inter_threads` | `1` | Parallel ONNX inferences |
| `python_bin` | `"./scripts/ocr_venv/bin/python3.12"` | The Python used by the parsing scripts |
| `parse_pdf_script` | `"./scripts/parse_pdf.py"` | The PDF parsing script |
| `parse_word_script` | `"./scripts/parse_word.py"` | The Word parsing script |
| `ocr_script` | `"./scripts/run_ocr.py"` | The OCR script |
| `parser_timeout_s` | `120` | Document parsing timeout, in seconds |
| `ocr_timeout_s` | `3600` | OCR timeout, in seconds |

## LLM roles {#llm-roles}

The five roles `semantic_llm`, `vision_llm`, `agent_llm`, `doc_summary_llm`, and `enricher_llm` each have their own section with the same fields:

| Key | Description |
|---|---|
| `provider` | `openai`, `glm`, `claude`, `ollama`, `deepseek`, or `mock` |
| `api_key` | The provider's API key |
| `model` | The provider's model identifier |
| `base_url` | The API address |

A role counts as configured only when `provider`, `api_key`, and `model` are all set. See [Configuration](/docs/deploy/configuration/#llm-roles).

## watch_dir {#watch-dir}

| Key | Default | Description |
|---|---|---|
| `data_dir` | `""` | The absolute path of a local directory to watch. Empty turns it off |
| `namespace_name` | `"local"` | The namespace files are imported into. It's created if it doesn't exist |
| `watch_enabled` | `true` | When `false`, existing files are imported but later changes aren't watched |

To manage watchers through the API, see [Watch a directory](/docs/guides/watch/).

## memory {#memory}

| Key | Default | Description |
|---|---|---|
| `default_ttl_seconds` | `0` | How long memory is kept. `0` keeps it forever |
| `inject_recent_turns` | `5` | How many recent turns to include when injecting context |
| `inject_max_tokens` | `2000` | The token ceiling for injected context |

## rag_fusion {#rag-fusion}

| Key | Default | Description |
|---|---|---|
| `default_enabled` | `false` | The global default for whether it's on |
| `default_variant_count` | `3` | How many query variants to generate |
| `default_rrf_k` | `60` | The rank-fusion constant |
| `default_timeout_ms` | `5000` | The timeout for the LLM to generate variants |
| `llm_prompt_template_zh` | `"default-zh"` | The Chinese prompt template |
| `llm_prompt_template_en` | `"default-en"` | The English prompt template |

RAG-Fusion is off by default and runs only when an LLM role is configured. The configuration template notes that the server doesn't read the keys in this section yet, so the built-in defaults are what's actually used.

## gc {#gc}

Garbage collection runs in three stages: soft delete, hard delete, and file cleanup.

| Key | Default | Description |
|---|---|---|
| `enabled` | `true` | Whether the background garbage collection thread runs |
| `soft_delete_retention_days` | `30` | Days a soft-deleted item is kept, during which it can be restored |
| `blob_gc_retention_days` | `90` | Days a file is kept after hard delete before it's physically removed |
| `scan_interval_hours` | `24` | The interval between background scans |
| `max_purge_per_run` | `10000` | The most files cleaned up in one run |
| `max_run_duration_minutes` | `5` | The longest a single run can take |
| `dry_run` | `false` | Scan without deleting, for testing and inspection |
| `immediate_purge_enabled` | `false` | Turns on the immediate-purge API. Leave it `false` |

## Environment variables for Docker {#environment-variables}

See [Configuration](/docs/deploy/configuration/#environment-variables).
