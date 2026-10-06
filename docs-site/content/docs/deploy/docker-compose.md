---
title: Deploy with Docker Compose
description: Run Cortrix with the Compose file that ships in the repository, and manage its data.
weight: 10
---

The repository ships a Compose deployment. Here's what it includes, how to check on it, where your data lives, and how to let other machines reach it.

If you haven't started Cortrix yet, begin with the [Quickstart](/docs/get-started/quickstart/).

> [!IMPORTANT]
> The bundled Compose deployment is built for local evaluation. It has no authentication and listens only on the loopback address by default. It isn't an internet-facing production setup.

## What's included {#what-is-included}

| Item | Details |
|---|---|
| Compose file | `deploy/docker-compose.yml` |
| Image | Built from source with `deploy/Dockerfile` and tagged `cortrix:quickstart` |
| Service | One `cortrix` container running `cortrix-server` and the Web UI |
| Port | `127.0.0.1:8420`, shared by the API and the Web UI |
| Volume | `cortrix-data`, mounted at `/data` in the container |
| Execution provider | CPU for both embedding and reranking |
| Off by default | External LLM roles and the built-in Agent |

The metrics port and the built-in Agent port aren't published to the host.

## Start and stop {#start-and-stop}

```bash
CORTRIX_SOURCE_REVISION="$(git rev-parse HEAD)" \
  docker compose -f deploy/docker-compose.yml up --build --wait
```

```bash
docker compose -f deploy/docker-compose.yml down
```

`down` keeps the volume. On later starts you can drop `--build` to skip rebuilding the image.

## Check status {#status}

```bash
docker compose -f deploy/docker-compose.yml ps
docker compose -f deploy/docker-compose.yml logs -f cortrix
```

Cortrix has three health endpoints:

| Endpoint | What it tells you |
|---|---|
| `/api/v1/system/health/live` | Whether the process is alive |
| `/api/v1/system/health/ready` | Whether the API, the models, and the demo data are all ready |
| `/api/v1/health` | A summary of each component |

```bash
curl -fsS http://127.0.0.1:8420/api/v1/health
```

```json
{
  "components": {
    "config": "ok",
    "logging": "ok",
    "namespace_manager": "ok",
    "parser": "disabled"
  },
  "llm_enabled": false,
  "status": "healthy",
  "version": "1.0.0-rc.2"
}
```

The container's health check allows a 30-minute start period so the first start has time to download the models.

## Web UI {#web-ui}

The Web UI shares a port with the API. Once the service is up, open `http://127.0.0.1:8420/` in your browser.

## Where your data lives {#data}

All persistent data is in the `cortrix-data` volume: model files, databases, the vector index, and uploaded files. Docker prefixes the volume name with the Compose project name, so by default it's `deploy_cortrix-data`.

```bash
docker volume inspect deploy_cortrix-data
```

> [!CAUTION]
> `docker compose down --volumes` deletes this volume, along with all of your data and the downloaded models.

## Let other machines connect {#publish}

By default, nothing outside the host can reach the service. To open it up on an isolated test network you control, set `CORTRIX_PUBLISH_HOST` when you start it, and preferably pick an uncommon port:

```bash
CORTRIX_PUBLISH_HOST=10.0.0.5 CORTRIX_HTTP_PORT=18420 \
  docker compose -f deploy/docker-compose.yml up -d
```

Any machine that can route to that address can then reach the Web UI and API at `http://10.0.0.5:18420/`.

> [!WARNING]
> This path has no authentication. Anyone who can reach the address gets full API access. Use it only on an isolated test network you control, never on a shared corporate network, a cloud network with other tenants, or anything internet-facing.
>
> Setting only the port, without `CORTRIX_PUBLISH_HOST`, doesn't widen what the service listens on.

## Admin endpoints {#admin-endpoints}

Admin endpoints accept requests only from the loopback address. In a Docker deployment, a request from the host doesn't look like loopback to the container, so it gets 403 `CX_ERR_ADMIN_LOOPBACK_REQUIRED`. When you need an admin endpoint, send the request from inside the container:

```bash
docker compose -f deploy/docker-compose.yml exec cortrix \
  curl -fsS http://127.0.0.1:8420/api/v1/admin/db-connections
```

## Next steps {#next-steps}

- [Configuration](/docs/deploy/configuration/)
- [Performance tuning](/docs/deploy/performance-tuning/)
- [Security boundaries](/docs/deploy/security/)
