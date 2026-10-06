---
title: Troubleshooting
description: Common problems, what causes them, and how to fix them.
weight: 40
---

Common problems are listed here by symptom. Most entries come from testing `v1.0.0-rc.2` on one macOS machine while this documentation was written, so some of them aren't described anywhere else in the repository.

## Startup {#startup}

### `docker: 'compose' is not a docker command` {#no-compose-plugin}

The Docker Compose plugin isn't installed. If you have the standalone version, replace `docker compose` with `docker-compose` in each command.

### The first start takes a long time to return {#slow-first-start}

The first start builds the image from source and downloads about 1.17 GB of models, and `--wait` blocks until the service is ready. To watch progress:

```bash
docker compose -f deploy/docker-compose.yml logs -f cortrix
```

The container's health check allows a 30-minute start period. If a model download hits a missing file or a size or checksum mismatch, the service doesn't report ready, and the log says why.

### Port 8420 is in use {#port-in-use}

Start on a different port:

```bash
CORTRIX_HTTP_PORT=18420 docker compose -f deploy/docker-compose.yml up --wait
```

Then replace `8420` with the new port in every command.

## Installation {#install}

### `pip install cortrix-mcp` reports "No matching distribution found" {#pip-not-found}

`cortrix-mcp` requires Python 3.10 or later. With an older interpreter, pip can't find a version it can install. Check the version, and create the virtual environment with a newer Python:

```bash
python3 --version
python3.12 -m venv .venv
```

`cortrix` and `cortrix-skills` need Python 3.9 or later.

## Calling the API {#api}

### Every Python SDK call returns 502 {#sdk-502}

The SDK raises `CortrixError` with status code 502 and no response body, while `curl` to the same address works.

A system proxy is usually intercepting requests to your own machine. Bypass the proxy for local addresses:

```bash
NO_PROXY=127.0.0.1,localhost python your_script.py
```

### A query returns 400 `CX_ERR_DEPRECATED_FIELD` {#deprecated-namespace}

The query request used the singular `namespace`. Use the plural array instead:

```json
{ "namespaces": ["demo"], "query": "..." }
```

### An admin endpoint returns 403 `CX_ERR_ADMIN_LOOPBACK_REQUIRED` {#admin-loopback}

Admin endpoints accept requests only from the loopback address. In a Docker deployment, a request from the host doesn't look like loopback to the container. Send the request from inside the container:

```bash
docker compose -f deploy/docker-compose.yml exec cortrix \
  curl -fsS http://127.0.0.1:8420/api/v1/admin/<endpoint>
```

This error code is in the server code but not in the error list of the OpenAPI spec.

### A query returns HTTP 200 but the results are incomplete {#partial-results}

A cross-namespace query still returns 200 when some namespaces fail. Check `meta.coverage_ratio` and `meta.namespaces_failed` in the response.

A namespace that doesn't exist shows up in `namespaces_failed` with the error code `CX_ERR_INDEX_CORRUPT`. When you see that, first check that the namespace name is spelled correctly:

```bash
curl -fsS http://127.0.0.1:8420/api/v1/namespaces
```

## Ingestion {#ingestion}

### An upload returns 202 but the document never appears {#upload-accepted-but-missing}

An inline upload (`POST /api/v1/documents`) is asynchronous. An accepted request doesn't mean processing succeeded. Check the task progress with the `task_id` you got back:

```bash
curl -fsS http://127.0.0.1:8420/api/v1/documents/tasks/<task_id>/progress
```

A non-null `error_code` means processing failed. A common cause is a namespace that doesn't exist: an inline upload doesn't check for that when you submit.

### A file upload returns 404 `CX_ERR_NS_NOT_FOUND` {#namespace-not-found}

The target namespace doesn't exist. Create it first:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{"name": "your-namespace"}' \
  http://127.0.0.1:8420/api/v1/namespaces
```

### Uploading again returns `skipped` {#upload-skipped}

This is expected. Cortrix recognizes duplicate files by content hash. When the content hasn't changed, it skips reprocessing and returns the original `doc_id`.

### A watcher isn't importing files {#watch-not-importing}

The watched path must be one the Cortrix server process can reach. When Cortrix runs in Docker, that means a path inside the container. Mount a host directory into the container before you watch it.

## Memory {#memory}

### Editing or deleting a memory returns 400 `ns is required` {#memory-ns-required}

- To edit, add `ns` to the request body.
- To delete, add `ns` as a query parameter.

This is tested behavior that differs from the OpenAPI spec, which declares no `ns` parameter for deletion.

See [Work with memory](/docs/guides/memory/#edit).

## MCP {#mcp}

### Claude Code doesn't show the Cortrix tools {#mcp-pending}

Claude Code asks you to approve an MCP server added with `--scope project` before using it. Check its status:

```bash
claude mcp get cortrix
```

If the status is `Pending approval`, start `claude` and approve it.

### The MCP server fails to start because `cortrix-mcp` can't be found {#mcp-command-not-found}

`cortrix-mcp` is installed in a virtual environment, and an MCP client doesn't necessarily start it with that environment's `PATH`. Change `command` in the configuration to an absolute path, for example `/path/to/.venv/bin/cortrix-mcp`.

## GPU {#gpu}

For problems with a CUDA deployment, see [NVIDIA CUDA](/docs/deploy/cuda/#troubleshooting).

## Report a problem {#report}

If this page doesn't cover your problem, report it in [GitHub Issues](https://github.com/cortrix/cortrix/issues). Include the version, your environment, steps to reproduce, and the `request_id` from the error response.
