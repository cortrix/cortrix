---
title: Install with an AI agent
description: Hand setup and verification to a terminal-capable AI agent.
weight: 30
---

On this page you'll get a task brief you can hand to a terminal-capable AI agent. The agent installs Cortrix `v1.0.0-rc.2` on your machine, verifies it, and returns a structured report.

This path runs the same loopback-only Docker flow as the [Quickstart](/docs/get-started/quickstart/). It's a local first-value setup, not an internet-facing, cloud, security, or production deployment procedure.

## Prerequisites {#prerequisites}

The AI agent needs to be able to use your local filesystem, Git, Docker, Docker Compose, and `curl`. It can be a coding assistant or a broader agent runtime. Whether it qualifies depends on those capabilities. This page doesn't claim that every version or configuration of any particular product has been validated.

## Contract identity {#contract-identity}

```text
Contract: cortrix-agent-quickstart/v1
Repository: https://github.com/cortrix/cortrix.git
Release: v1.0.0-rc.2
Commit: resolve and record the full commit referenced by the release tag
API bind: 127.0.0.1:8420
```

Both the release tag and the full commit it resolves to must be recorded. A checkout of the mutable `main` branch doesn't satisfy this contract.

## What the agent may do {#allowed}

- Inspect local prerequisites: Git, Docker, Docker Compose, `curl`, disk, network, and ports.
- Create one new installation directory.
- Clone the declared repository and check out the declared release.
- Build and start the documented Docker Compose stack.
- Download the pinned model files that stack needs.
- Call the readiness and query endpoints on the loopback address.
- Leave the verified local service running.
- Report the files, containers, volumes, and commands it used.

## What the agent must not do {#prohibited}

- Modify, reset, delete, or reuse an existing Cortrix checkout.
- Use `sudo` or install system-level dependencies without separate approval from you.
- Request, read, store, or invent keys for LLM, cloud, GitHub, or any other service.
- Change firewall, DNS, proxy, VPN, or system security settings.
- Publish Cortrix on `0.0.0.0` or any non-loopback address.
- Turn off ONNX, embedding, reranking, model checksums, readiness, or source verification to make the run pass.
- Delete existing Docker images, containers, caches, or volumes.
- Run `docker compose down --volumes` unless you explicitly ask for it.
- Describe this contract as a production deployment or as production readiness.

## Copyable agent task {#agent-task}

Give your AI agent this brief exactly as written. It's contract text, so don't reword it:

```text {title="Agent task"}
Install Cortrix locally under the cortrix-agent-quickstart/v1 contract.

Use only this repository and installation target:
- Repository: https://github.com/cortrix/cortrix.git
- Release: v1.0.0-rc.2
- Resolved commit: record the full commit from the exact release tag

Work in a new local directory. Do not modify, reset, delete, or reuse an
existing Cortrix checkout.

Before changing local state, verify that Git, Docker, Docker Compose, and curl
are available. Report available disk space, confirm that the required network
access is available, and check that 127.0.0.1:8420 is not already reserved by
another service. If a prerequisite is missing or the target directory already
exists, stop and report the blocker. Do not use sudo or install system-level
dependencies without asking me.

Clone the exact release, verify the origin URL, verify the exact tag, and record the full commit resolved by that tag. Start only the documented loopback Docker Compose path with CORTRIX_SOURCE_REVISION set to the checked-out commit.

Do not request or use any LLM or cloud provider key. Do not change firewall,
DNS, proxy, VPN, or system security settings. Do not expose a non-loopback
port. Do not disable ONNX, embedding, reranking, readiness, source checks, or
model integrity checks.

Wait until Docker Compose reports the service ready. Call the readiness
endpoint, then run the documented demo query with rerank=true. Verify that the
response contains source-backed quickstart-demo.txt content and numeric
rerank_score values.

Leave the verified service running. Do not delete volumes. Finish with the
exact report schema in this contract and mark the overall result PASS only when
every required assertion passes.
```

## Required execution path {#execution-path}

The agent must create a new `cortrix` directory under a parent directory you approve. If `cortrix` already exists there, it must stop rather than overwrite or reuse it.

```bash
git clone --branch v1.0.0-rc.2 --depth 1 \
  https://github.com/cortrix/cortrix.git cortrix
cd cortrix
test "$(git remote get-url origin)" = "https://github.com/cortrix/cortrix.git"
test "$(git describe --tags --exact-match)" = "v1.0.0-rc.2"
CORTRIX_SOURCE_REVISION="$(git rev-parse HEAD)"
test "$(printf '%s' "$CORTRIX_SOURCE_REVISION" | wc -c | tr -d ' ')" = "40"
CORTRIX_SOURCE_REVISION="$CORTRIX_SOURCE_REVISION" \
  docker compose -f deploy/docker-compose.yml up --build --wait
```

The first start downloads about 1.17 GB of pinned model files and can take several minutes. No `.env` file, provider key, host-side model tooling, manual model download or conversion, or separate bootstrap command is required.

## Required verification {#verification}

Readiness:

```bash
curl -fsS http://127.0.0.1:8420/api/v1/system/health/ready
```

A source-backed reranked query:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{
    "namespaces": ["demo"],
    "query": "What does semantic storage keep close to the agents that need it?",
    "top_k": 5,
    "rerank": true
  }' \
  http://127.0.0.1:8420/api/v1/query
```

The result must contain content from `quickstart-demo.txt` and a numeric `rerank_score`. A process that exits successfully without those two assertions is still a failed contract.

## Required final report {#report}

```text {title="Report format"}
CORTRIX_AGENT_QUICKSTART=PASS|FAIL
contract=cortrix-agent-quickstart/v1
repository=https://github.com/cortrix/cortrix.git
release=v1.0.0-rc.2
commit=<observed full commit resolved by the exact release tag>
installation_directory=<absolute path>
platform=<os and architecture>
docker_version=<observed version>
docker_compose_version=<observed version>
available_disk=<observed value>
bind_address=127.0.0.1:8420
readiness=PASS|FAIL
source_content=PASS|FAIL
numeric_rerank_score=PASS|FAIL
external_llm_enabled=false
service_state=RUNNING|NOT_RUNNING
created_resources=<summary>
stop_command=docker compose -f deploy/docker-compose.yml down
destructive_cleanup_command=docker compose -f deploy/docker-compose.yml down --volumes
warnings=<none or exact warnings>
blocker=<none or exact blocker>
```

`PASS` requires all of these: the exact repository and release, the 40-character commit that the tag resolves to, a loopback-only bind, readiness, source content, numeric rerank scores, external LLM roles left off, and a running service.

## Failure behavior {#failure}

The agent must stop and return `FAIL` when:

- A prerequisite is missing.
- The target directory already exists.
- The tag or commit doesn't match what was expected.
- The API port is in use.
- The Docker build, a download, an integrity check, startup, or readiness fails.
- The query fails or lacks the required source content.
- `rerank_score` is absent or isn't numeric.
- The service binds beyond the loopback address.
- Finishing the task would require a prohibited action.

The report must preserve the original error and name the next decision you need to make. The agent must not quietly switch to a different build profile or to weaker verification.

## Stop or reset {#stop-or-reset}

After you've reviewed the report, you can ask the agent to stop the service:

```bash
docker compose -f deploy/docker-compose.yml down
```

Removing the cached models and local data is a separate, destructive decision that's yours to make:

```bash
docker compose -f deploy/docker-compose.yml down --volumes
```

> [!WARNING]
> The agent must not run the cleanup command with `--volumes` unless you explicitly ask for it.

## What this path doesn't verify {#not-verified}

An agent-assisted install doesn't verify every HTTP API, parser, the built-in Agent, MCP, the SDK, authentication, tenants, security, benchmarks, or production behavior. Before you expand the deployment, read [Compatibility and status](/docs/resources/compatibility/) and [Choose an access path](/docs/integrations/overview/).

## Next steps {#next-steps}

- [Quickstart](/docs/get-started/quickstart/): run the same flow yourself.
- [Choose an access path](/docs/integrations/overview/)
- [Compatibility and status](/docs/resources/compatibility/)
