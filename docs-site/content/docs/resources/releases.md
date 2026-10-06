---
title: Release notes
description: The scope, main changes, and known issues of v1.0.0-rc.2.
weight: 50
---

Here's a summary of `v1.0.0-rc.2`. For every change with its issue and pull request, see the [change-by-change release notes](https://github.com/cortrix/cortrix/blob/v1.0.0-rc.2/docs/releases/v1.0.0-rc.2.md).

Cortrix `v1.0.0-rc.2` is a pre-release for local evaluation and integration testing. It isn't a production-readiness claim. Before you upgrade from RC1, read [Upgrade](/docs/deploy/upgrade/) and [Compatibility and status](/docs/resources/compatibility/).

## Highlights {#highlights}

- **License change**: Cortrix-authored RC2 material is available under the Apache License 2.0. The historical `v1.0.0-rc.1` release remains under AGPL-3.0-only.
- **More reliable asynchronous ingestion**: task identity is scoped to the namespace, and terminal-state transitions, cancellation races, cleanup scheduling, and retry de-duplication are all stronger.
- **Reranking can be turned off per request**: for duplicate detection and similar-item lookups, set `rerank=false` on the query.
- **MCP works with modern and legacy clients**: the local MCP server supports the modern stdio discovery path while keeping the legacy initialization handshake, and it passes session and trace identity to the Cortrix server.
- **Hardening in several places**: parser output handling, sparse-vector decoding, configuration validation, memory-scope validation, and operation-log error reporting.
- **Deployment defaults are unchanged**: Docker Compose still listens only on the loopback address by default, with a new explicit publish-address option for isolated test networks.

## FiQA retrieval accuracy {#fiqa}

With the non-LLM cross-encoder reranking configuration, the RC2 benchmark results on FiQA are higher than the July baseline:

| Metric | July result | August result | Relative change |
|---|---:|---:|---:|
| nDCG@10 | 0.2526 | 0.3125 | +23.7% |
| Recall@10 | 0.3635 | 0.4523 | +24.4% |

These are retrieval-accuracy measurements, not server performance measurements. The two runs differ in build, configuration, and execution environment, so the comparison doesn't isolate the improvement to any single code change.

For the scope and limits of the measurements, see [Benchmark evidence](/docs/resources/benchmarks/).

## Compatibility notes {#compatibility-notes}

- Setting `rerank=false` on an individual query is the verified way to turn reranking off. The namespace-level `reranker_config.enabled` isn't wired into the query path yet.
- Local stdio is the current public MCP transport.

## Known issues {#known-issues}

| Issue | Tracking |
|---|---|
| Enrichment backfill still has open follow-ups in scheduling and transport retries | [#62](https://github.com/cortrix/cortrix/issues/62), [#78](https://github.com/cortrix/cortrix/issues/78) |
| HyPE LLM-call metrics aren't connected to the production enrichment path yet | [#69](https://github.com/cortrix/cortrix/issues/69) |
| The LAN publish-address option doesn't yet give you a complete authenticated setup without editing configuration by hand | [#71](https://github.com/cortrix/cortrix/issues/71) |
| Ingestion into a single namespace is constrained by serialized durability and index updates | [#73](https://github.com/cortrix/cortrix/issues/73) |
| Cross-encoder reranking is on by default, even for tasks where turning it off is the better choice | [#75](https://github.com/cortrix/cortrix/issues/75) |

## Release artifacts {#artifacts}

GitHub provides source archives for the tag automatically. RC2 doesn't promise a separately published container image or prebuilt binary unless the GitHub pre-release page explicitly attaches one. To use Cortrix, build it from source. See the [Quickstart](/docs/get-started/quickstart/).

## Full changelog {#full-changelog}

- [Change-by-change notes](https://github.com/cortrix/cortrix/blob/v1.0.0-rc.2/docs/releases/v1.0.0-rc.2.md)
- Commit comparison: [`v1.0.0-rc.1...v1.0.0-rc.2`](https://github.com/cortrix/cortrix/compare/v1.0.0-rc.1...v1.0.0-rc.2)

## Next steps {#next-steps}

- [Upgrade](/docs/deploy/upgrade/)
- [Compatibility and status](/docs/resources/compatibility/)
