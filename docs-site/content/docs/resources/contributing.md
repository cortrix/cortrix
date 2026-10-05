---
title: Contributing
description: How to report a problem, set up a development environment, and submit a change.
weight: 60
---

You can take part in Cortrix in several ways. Here's a summary, along with the workflow for landing a change. The full rules are in `CONTRIBUTING.md` in the repository.

## Ways to contribute {#ways-to-contribute}

| What you want to do | Where to go |
|---|---|
| Report a bug | The [bug report form](https://github.com/cortrix/cortrix/issues/new?template=bug.yml). Include steps to reproduce, expected and actual behavior, and your environment |
| Propose a feature or integration | The [feature and integration form](https://github.com/cortrix/cortrix/issues/new?template=feature-integration.yml). Use it to confirm scope before you open a large pull request |
| Improve the docs | The [documentation form](https://github.com/cortrix/cortrix/issues/new?template=documentation.yml), or send a focused documentation pull request |
| Report a security vulnerability | Don't open a public issue. See [Security boundaries](/docs/deploy/security/#reporting) |

## Licensing and certification {#certification}

Cortrix doesn't require a Contributor License Agreement (CLA). Contributions are accepted under the Apache License 2.0, and every new commit must carry a `Signed-off-by` line certifying the [Developer Certificate of Origin 1.1](https://github.com/cortrix/cortrix/blob/release/1.0/DCO) (DCO).

Add `-s` when you commit:

```bash
git commit -s
```

To add it to an existing local commit:

```bash
git commit --amend --signoff --no-edit
```

If a contribution has several commits, sign off every one. Contributors keep the copyright in their contributions.

## Repository layout {#layout}

```filetree
cortrix/
├── src/                  # The C++ server implementation
├── include/cortrix/      # Public C++ headers
├── api/                  # The OpenAPI spec, components, paths, and examples
├── sdk/python/           # The Python SDK
├── sql-extensions/       # pgcortrix, the PostgreSQL extension
├── cortrix-mcp/          # The MCP server
├── cortrix-skills/       # Framework adapters
├── cortrix-agent/        # The built-in Agent
├── web/                  # The Web UI
├── deploy/               # Compose files, Dockerfiles, and configuration
├── tests/                # C++ tests
└── docs/                 # Documentation
```

Code comments and names carry short identifiers from the project's design tracking. The repository's [Feature Index](https://github.com/cortrix/cortrix/blob/release/1.0/docs/feature-index.md) explains them.

## Set up a development environment {#development-setup}

If you only want to run Cortrix, the [Quickstart](/docs/get-started/quickstart/) is all you need. The source build below is for contributors who are changing Cortrix itself.

### Prerequisites {#dev-prerequisites}

| Item | Requirement |
|---|---|
| Operating system | macOS (arm64 or x86_64) or Linux (x86_64) |
| Compiler | C++17 support: Clang 14 or later, or GCC 10 or later |
| CMake | 3.27 or later |
| OpenSSL | Installed through your system package manager |
| Python | 3.9 or later, for the SDK and the MCP server |

CMake fetches every C++ dependency automatically.

### Build the server {#build}

```bash
git clone https://github.com/cortrix/cortrix
cd cortrix
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . --parallel

./cortrix-server --config ../deploy/cortrix.yaml
```

For faster iteration, you can build without ONNX. Embedding then uses a stub that produces random vectors:

```bash
cmake -DCMAKE_BUILD_TYPE=Release -DCORTRIX_USE_ONNX=OFF ..
```

### Python SDK {#sdk}

```bash
cd sdk/python
pip install -e ".[dev]"
pytest
```

## Run the tests {#tests}

Run the C++ tests with `ctest` from the `build/` directory:

```bash
cd build
ctest --output-on-failure
```

You can also run a single suite directly: `./cortrix_unit_tests`, `./cortrix_integration_tests`, `./cortrix_security_tests`, or `./cortrix_stability_tests`.

## Coding standards {#coding-standards}

- Use C++17, in the `cortrix::<module>` namespace, and match the style of the surrounding code.
- Use English only in source: comments, log messages, identifiers, and error strings.
- A new endpoint must return the structured error fields (`code`, `retryable`, `category`, `retry_after_ms`) and carry the relevant `x-cortrix-*` hints in the OpenAPI spec.
- Error codes follow the `CX_ERR_*` naming convention.

## Version number {#version}

The `VERSION` file at the root of the repository is the only place the version is edited by hand. After you change it, synchronize the other locations with the script:

```bash
python3 scripts/sync_version.py
python3 scripts/sync_version.py --check
```

Don't edit a derived version field on its own. CI runs the check command and rejects version drift.

## Pull request workflow {#pull-request-workflow}

1. For anything non-trivial, open an issue first.
2. Branch from the intended base with a short-lived name such as `feat/<topic>`, `fix/<topic>`, or `docs/<topic>`.
3. Keep each pull request to one logical change.
4. Add tests for new behavior and keep existing tests passing.
5. If you change the API, update both `api/openapi.yaml` and the affected docs.
6. Sign off every commit under the DCO.
7. Run the full test suite locally before you push.
8. Open the pull request against `main` and describe what changed, why, and how you tested it.

### Branches {#branching}

| Branch | Purpose |
|---|---|
| `main` | The only integration branch for ongoing development. It doesn't identify any published version |
| `release/1.0` | The stabilization and maintenance line for the 1.0 family |

Published versions are identified by immutable tags and GitHub Releases. A fix normally lands on `main` first and then reaches the release line through a separate `backport/1.0-*` pull request. Delete a pull request's remote branch once it's merged or closed.

## Code of conduct {#conduct}

Participation is governed by the code of conduct in the repository (`CODE_OF_CONDUCT.md`). Send conduct reports privately to [devrel@cortrix.ai](mailto:devrel@cortrix.ai), not through a public issue.

## Next steps {#next-steps}

- [Release notes](/docs/resources/releases/)
