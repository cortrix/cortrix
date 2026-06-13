# Contributing to Cortrix

Thanks for your interest in Cortrix — an Agent-Native Semantic Storage Engine. This guide covers how to set up a dev environment, the project layout, and the workflow for landing a change.

By contributing you agree that your contributions are licensed under the project's [AGPL-3.0](LICENSE) license.

---

## Ways to Contribute

- **Report bugs** — open a [GitHub Issue](https://github.com/cortrix/cortrix/issues) with steps to reproduce, expected vs. actual behaviour, and your environment (OS, compiler, Cortrix version).
- **Propose features** — start a thread in [GitHub Discussions](https://github.com/cortrix/cortrix/discussions) before opening a large PR, so the design can be agreed first.
- **Improve docs** — fixes to the README, API docs, or tutorials are always welcome.
- **Send code** — see the workflow below.

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

# Run it (defaults to http://localhost:8080)
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

1. **Discuss first** for anything non-trivial (open an issue or a Discussion).
2. **Branch** from `main`: `feat/<short-name>` or `fix/<short-name>`.
3. **Keep it focused** — one logical change per PR.
4. **Add tests** for new behaviour and keep existing tests green.
5. **Update docs** — if you change the API, update both `api/openapi.yaml` and the affected docs.
6. **Run the full test suite locally** before pushing.
7. **Open the PR** against `main` with a clear description: what changed, why, and how it was tested.

### PR Checklist

- [ ] Builds cleanly (`cmake` + `make`) with no new warnings.
- [ ] `ctest --output-on-failure` passes; SDK `pytest` passes if the SDK changed.
- [ ] New code has tests.
- [ ] API changes are reflected in `api/openapi.yaml` and the Python SDK.
- [ ] Source is English-only and follows the surrounding style.
- [ ] Commit messages are clear and imperative ("Add cross-namespace filter", not "added stuff").

---

## Reporting Security Issues

Please do **not** open public issues for security vulnerabilities. Email [security@cortrix.ai](mailto:security@cortrix.ai) with details, and we will respond promptly.

---

## License

Cortrix is licensed under **AGPL-3.0**. By submitting a contribution, you agree it is licensed under AGPL-3.0. Questions about licensing or a commercial agreement? Contact [hello@cortrix.ai](mailto:hello@cortrix.ai).
