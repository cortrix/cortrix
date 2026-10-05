---
title: Upgrade
description: Upgrade from v1.0.0-rc.1 to v1.0.0-rc.2.
weight: 60
---

Before you move from `v1.0.0-rc.1` to `v1.0.0-rc.2`, here's what to check first, what to verify afterward, and how to roll back.

> [!IMPORTANT]
> RC2 is a new pre-release. Back up your persistent data and validate RC2 against a copy before you change an existing environment.

## Before you upgrade {#before-upgrading}

- Read [Compatibility and status](/docs/resources/compatibility/) and confirm the current status of the capabilities you depend on.
- Record your existing source revision, configuration, model files, database path, and rollback procedure.
- In an isolated environment, rerun your API, SDK, MCP, deployment, and persistence smoke tests against the exact RC2 tag.
- Keep treating your source documents as authoritative until you've verified indexing, query behavior, deletion, and rollback.

## Changes to verify {#changes-to-verify}

### Task lifecycle and persistent data {#task-lifecycle}

RC2 changes a lot in the asynchronous task lifecycle. Verify task submission, terminal states, resubmission, cancellation races, cleanup scheduling after a restart, namespace-scoped task identity, and repeated or same-content batch uploads.

There's no new SQL migration file to apply by hand between `v1.0.0-rc.1` and RC2. Startup still creates or adjusts indexes and task state, though, so this is no substitute for a backup or for an upgrade test against a copy.

### Query and reranking {#query-and-rerank}

- When the reranker doesn't suit your task, pass `rerank=false` on each request.
- Don't rely on the namespace-level reranker setting to turn reranking off. The query path doesn't read it yet.

See [Performance tuning](/docs/deploy/performance-tuning/#rerank).

### MCP {#mcp}

- The local MCP server supports both the modern stdio discovery path and the legacy initialization handshake.
- Session and trace identity are passed through to the Cortrix server.
- Verify client and server API compatibility before you replace an existing MCP environment. Local stdio is still the only public transport.

### Deployment {#deployment}

- Docker Compose still listens only on the loopback address by default.
- Publishing to a LAN address is an explicit opt-in, meant only for isolated test networks.
- After upgrading, rerun the readiness check and the reranked query from the [Quickstart](/docs/get-started/quickstart/).

## License change {#license-change}

Cortrix-authored RC2 material is licensed under Apache-2.0. Release objects from the historical `v1.0.0-rc.1` remain under AGPL-3.0-only, and third-party material keeps its own license. Before you redistribute a mixed or modified package, review `LICENSE`, `NOTICE.md`, and `CONTRIBUTING.md` in the repository.

## Reading the benchmarks {#benchmarks}

- The RC2 retrieval evidence is the pinned four-corpus CPU measurement bundle. See [Benchmark evidence](/docs/resources/benchmarks/).
- Don't compare the 2,000-query Quora subset with a result over the full query set.
- Don't read the published CPU latency as production capacity under concurrency.

## Roll back {#rollback}

1. Stop the RC2 environment.
2. Restore the RC1 configuration and data backup you recorded.
3. Check out the immutable `v1.0.0-rc.1` tag.

> [!WARNING]
> Don't reuse a database that RC2 has opened unless you've run a rollback test on that same copy and confirmed it's compatible.

## Next steps {#next-steps}

- [Release notes](/docs/resources/releases/)
- [Compatibility and status](/docs/resources/compatibility/)
