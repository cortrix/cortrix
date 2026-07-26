# Install Cortrix with an AI Agent

This contract is for terminal-capable AI Agents that can use a local
filesystem, Git, Docker, Docker Compose, and `curl`. It installs the current
Cortrix release candidate through the same loopback-only Docker path documented
in the [Quick Start](QUICKSTART.md).

The AI Agent can be a coding assistant or a broader agent runtime, such as
OpenClaw or Hermes. Eligibility is capability-based: it must be able to use the
local filesystem and terminal tools required by this contract. Naming an agent
runtime here does not claim that every version or configuration has been
validated.

This is a local first-value setup. It is not an internet-facing, cloud, security,
or production deployment procedure.

## Contract identity

```text
Contract: cortrix-agent-quickstart/v1
Repository: https://github.com/cortrix/cortrix.git
Release: v1.0.0-rc.1
Commit: e159974f936bd987d5718195d54cd70ff3acc2e5
API bind: 127.0.0.1:8420
```

The release tag and commit are both required. A mutable `main` checkout does not
satisfy this contract.

## What the agent may do

The agent may:

- inspect the local Git, Docker, Docker Compose, `curl`, disk, network, and port
  prerequisites;
- create one new installation directory;
- clone the declared repository and check out the declared release;
- build and start the documented Docker Compose stack;
- download the pinned model assets required by that stack;
- call the loopback readiness and query endpoints;
- leave the verified local service running;
- report the exact files, containers, volumes, and commands it used.

## What the agent must not do

The agent must not:

- modify, reset, delete, or reuse an existing Cortrix checkout;
- use `sudo` or install system-level prerequisites without separate human
  approval;
- request, read, store, or invent LLM, cloud, GitHub, or other provider keys;
- change firewall, DNS, proxy, VPN, or system security settings;
- publish Cortrix on `0.0.0.0` or any non-loopback interface;
- disable ONNX, embedding, reranking, model checksums, readiness, or source
  verification to make the run pass;
- delete existing Docker images, containers, caches, or volumes;
- run `docker compose down --volumes` unless the human explicitly asks for
  destructive cleanup;
- describe this contract as production deployment or production readiness.

## Copyable agent task

Give the following task to a terminal-capable AI Agent:

```text
Install Cortrix locally under the cortrix-agent-quickstart/v1 contract.

Use only this repository and installation target:
- Repository: https://github.com/cortrix/cortrix.git
- Release: v1.0.0-rc.1
- Expected commit: e159974f936bd987d5718195d54cd70ff3acc2e5

Work in a new local directory. Do not modify, reset, delete, or reuse an
existing Cortrix checkout.

Before changing local state, verify that Git, Docker, Docker Compose, and curl
are available. Report available disk space, confirm that the required network
access is available, and check that 127.0.0.1:8420 is not already reserved by
another service. If a prerequisite is missing or the target directory already
exists, stop and report the blocker. Do not use sudo or install system-level
dependencies without asking me.

Clone the exact release, verify the origin URL, verify the exact tag, and verify
that HEAD equals the expected commit. Start only the documented loopback Docker
Compose path with CORTRIX_SOURCE_REVISION set to the checked-out commit.

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

## Required execution path

The agent must create a new `cortrix` directory beneath a human-approved parent
directory. If `cortrix` already exists there, it must stop instead of overwriting
or reusing it.

```bash
git clone --branch v1.0.0-rc.1 --depth 1 \
  https://github.com/cortrix/cortrix.git cortrix
cd cortrix
test "$(git remote get-url origin)" = "https://github.com/cortrix/cortrix.git"
test "$(git describe --tags --exact-match)" = "v1.0.0-rc.1"
test "$(git rev-parse HEAD)" = "e159974f936bd987d5718195d54cd70ff3acc2e5"
CORTRIX_SOURCE_REVISION="$(git rev-parse HEAD)" \
  docker compose -f deploy/docker-compose.yml up --build --wait
```

The first start downloads about 1.17 GB of pinned model assets and can take
several minutes. No `.env` file, provider key, host-side model tooling, manual
model download, model conversion, or separate bootstrap command is required.

## Required verification

Readiness:

```bash
curl -fsS http://127.0.0.1:8420/api/v1/system/health/ready
```

Source-backed reranked query:

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

The result must contain content from `quickstart-demo.txt` and numeric
`rerank_score` values. A successful process exit without those assertions is a
failed contract.

## Required final report

The agent must finish with this schema:

```text
CORTRIX_AGENT_QUICKSTART=PASS|FAIL
contract=cortrix-agent-quickstart/v1
repository=https://github.com/cortrix/cortrix.git
release=v1.0.0-rc.1
commit=e159974f936bd987d5718195d54cd70ff3acc2e5
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

`PASS` requires the exact repository, release and commit; a loopback-only bind;
readiness; source content; numeric reranking scores; disabled external LLM
roles; and a running service.

## Failure behavior

The agent must stop and return `FAIL` when:

- a prerequisite is missing;
- the target directory already exists;
- the expected tag or commit does not match;
- the API port is occupied;
- Docker build, download, integrity, startup, or readiness fails;
- the query fails or lacks the required source content;
- `rerank_score` is absent or non-numeric;
- the service binds beyond loopback;
- completing the task would require a prohibited action.

The report must preserve the original error and name the next human decision.
It must not silently switch to a different build profile or weaker verification.

## Stop or reset

After reviewing the report, the human may ask the agent to stop the service:

```bash
docker compose -f deploy/docker-compose.yml down
```

Removing the cached models and local data is a separate destructive decision:

```bash
docker compose -f deploy/docker-compose.yml down --volumes
```

The agent must not run the destructive cleanup command unless explicitly asked.

## Related documentation

- [Docker Quick Start](QUICKSTART.md)
- [Compatibility and known status](compatibility.md)
- [Agent access](agent-access.md)
- [Model provenance and integrity](../deploy/MODELS.md)

Agent-assisted installation does not verify every HTTP API, parser, built-in
Agent, MCP, SDK, authentication, tenant, security, benchmark, or production
surface. Use the linked compatibility and access documents before expanding the
deployment.
