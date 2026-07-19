# Contributing to Cortrix

Thanks for your interest in Cortrix — an Agent-Native Semantic Storage Engine. This guide covers how to set up a dev environment, the project layout, and the workflow for landing a change.

Cortrix does not require a Contributor License Agreement. Contributions to Cortrix-authored material are accepted under [AGPL-3.0-only](LICENSE), the same license under which that material is made available, and must be certified under the [Developer Certificate of Origin 1.1](DCO). Third-party material remains under the license identified in its file or directory and must not be relicensed by a contribution.

---

## Ways to Contribute

- **Report bugs** — use the [Bug report form](https://github.com/cortrix/cortrix/issues/new?template=bug.yml) with steps to reproduce, expected vs. actual behaviour, and your environment.
- **Propose features or integrations** — use the [Feature or integration form](https://github.com/cortrix/cortrix/issues/new?template=feature-integration.yml) before opening a large PR so maintainers can confirm scope and evidence requirements.
- **Improve docs** — use the [Documentation form](https://github.com/cortrix/cortrix/issues/new?template=documentation.yml), or send a focused documentation PR.
- **Send code** — see the workflow below.

---

## Contribution Certification

Every new commit must include a `Signed-off-by` trailer certifying the [Developer Certificate of Origin 1.1](DCO). Create it with:

```bash
git commit -s
```

For an existing local commit, add the trailer and update the branch before review:

```bash
git commit --amend --signoff --no-edit
```

For a multi-commit contribution, every commit must be signed off. Existing repository history before the policy effective date of 2026-07-20 is not retroactively re-signed. Maintainers verify sign-offs before merge; no GitHub App, required check, or branch-protection enforcement is claimed by this repository policy.

Contributors retain copyright in their contributions. No separate CLA grant is required. A different license for a contribution requires separate permission from the applicable copyright holder or holders.

---

## Repository Layout

Cortrix is a C++ server with a Python SDK, a PostgreSQL extension, and an MCP server.

```
cortrix/
├── src/                  # C++ server implementation
├── include/cortrix/      # Public C++ headers (auth, common, config, memory, query, server, spc, store)
├── api/                  # OpenAPI 3.0 spec (openapi.yaml), components, paths, examples
├── sdk/python/           # Python SDK (the `cortrix` PyPI package)
├── sql-extensions/       # pgcortrix — PostgreSQL extension (plpython3u + HTTP)
├── mcp-server/           # MCP Server for Claude Code / Cline / Cursor
├── web/                  # Web UI (static)
├── cortrix-agent/        # Cortrix built-in Agent (F48, FastAPI + P03 SDK)
├── deploy/               # docker-compose.yml, Dockerfile, configs
├── tests/                # C++ unit / integration / security / stability / benchmark suites
└── docs/                 # Documentation
```

---

## Development Setup

### Prerequisites

- **OS**: macOS (arm64/x86_64) or Linux (x86_64)
- **Compiler**: C++17 (Clang 14+ or GCC 10+)
- **CMake**: 3.20+
- **OpenSSL**: via your system package manager
- **Python**: 3.9+ (for the SDK and MCP server)
- **Disk**: ~3 GB for the bge-m3 model files (optional — a stub embedder is available without the model)

All C++ dependencies (cpp-httplib, yaml-cpp, nlohmann/json, spdlog, hnswlib, SQLite+FTS5, ONNX Runtime, GoogleTest, Google Benchmark) are fetched automatically via CMake `FetchContent`.

### Build the Server

```bash
git clone https://github.com/cortrix/cortrix
cd cortrix

mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run it (defaults to http://localhost:8420)
./cortrix-server --config ../deploy/config.yaml
```

To build without ONNX (uses a stub embedder with random vectors — handy for fast iteration):

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DCORTRIX_USE_ONNX=OFF ..
```

### Python SDK

```bash
cd sdk/python
pip install -e ".[dev]"
pytest
```

### pgcortrix (PostgreSQL extension)

Built independently with PGXS — it is deliberately **not** part of the main CMake build:

```bash
cd sql-extensions/pgcortrix
make && make install      # requires pg_config on PATH (PostgreSQL 13–17)
```

---

## Running Tests

C++ tests run via `ctest` from the `build/` directory:

```bash
cd build
ctest --output-on-failure

# Or run a suite directly:
./cortrix_unit_tests
./cortrix_integration_tests
./cortrix_security_tests
./cortrix_stability_tests

# Benchmarks
./cortrix_benchmarks --benchmark_format=json
```

Python SDK tests:

```bash
cd sdk/python
pytest --cov=cortrix
```

---

## Coding Standards

- **C++17**, namespaced under `cortrix::<module>`. Match the style of the surrounding code.
- **English only** in all source — comments, log messages, identifiers, and error strings.
- **Agent-friendly by design**: new endpoints must return the 4-field error schema (`code` / `retryable` / `category` / `retry_after_ms`) and carry the relevant `x-cortrix-*` OpenAPI hints. The OpenAPI spec ([`api/openapi.yaml`](api/openapi.yaml)) is the source of truth for the wire contract — keep it and the SDK in sync.
- **Errors** use the project's `CX_ERR_*` code convention (see [`api/openapi.yaml`](api/openapi.yaml)).

---

## Pull Request Workflow

1. **Open an issue first** for anything non-trivial.
2. **Branch** from `main`: `feat/<short-name>` or `fix/<short-name>`.
3. **Keep it focused** — one logical change per PR.
4. **Add tests** for new behaviour and keep existing tests green.
5. **Update docs** — if you change the API, update both `api/openapi.yaml` and the affected docs.
6. **Sign off every commit** under DCO 1.1.
7. **Run the full test suite locally** before pushing.
8. **Open the PR** against `main` with a clear description: what changed, why, and how it was tested.

### PR Checklist

- [ ] Builds cleanly (`cmake` + `make`) with no new warnings.
- [ ] `ctest --output-on-failure` passes; SDK `pytest` passes if the SDK changed.
- [ ] New code has tests.
- [ ] API changes are reflected in `api/openapi.yaml` and the Python SDK.
- [ ] Source is English-only and follows the surrounding style.
- [ ] Commit messages are clear and imperative ("Add cross-namespace filter", not "added stuff").
- [ ] Every new commit contains a valid `Signed-off-by` trailer.

---

## Reporting Security Issues

Please do **not** open public issues for security vulnerabilities. Follow [SECURITY.md](SECURITY.md) and email [security@cortrix.ai](mailto:security@cortrix.ai). The acknowledgement target is within 5 business days; it is not a remediation deadline.

## Conduct

Participation is governed by the [Contributor Covenant Code of Conduct](CODE_OF_CONDUCT.md). Send conduct reports privately to [devrel@cortrix.ai](mailto:devrel@cortrix.ai), not through a public issue. Ownership and recusal rules are documented in [MAINTAINERS.md](MAINTAINERS.md).

---

## License

Cortrix-authored material is licensed under [AGPL-3.0-only](LICENSE). Contributions use the applicable license, are certified under [DCO 1.1](DCO), and do not require a CLA. Third-party material retains its own license. See [NOTICE.md](NOTICE.md) for copyright and exception boundaries.
