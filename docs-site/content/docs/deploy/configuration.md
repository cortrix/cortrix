---
title: Configuration
description: How the configuration file is organized, the environment variables for Docker, and how to configure LLMs by role.
weight: 20
---

Cortrix can be configured in two ways. This page explains both and walks through the settings you're most likely to change: authentication, logging, and LLM roles. For every key, see the [configuration reference](/docs/reference/configuration/).

## Two ways to configure {#two-ways}

| Method | Use it when | Template |
|---|---|---|
| YAML configuration file | You build from source and run `cortrix-server` directly | `config.yaml.example` |
| Environment variables | You deploy with Docker | `deploy/.env.example` |

When a setting appears in both places, the environment variable wins.

### Configuration file {#config-file}

When you run from source, copy the template into the build directory:

```bash
cp config.yaml.example build/config.yaml
```

### Environment variables {#environment-variables}

For a Docker deployment, copy the template to `.env` and edit what you need:

```bash
cp deploy/.env.example deploy/.env
```

Common variables:

| Variable | Value in the template | Purpose |
|---|---|---|
| `CORTRIX_DATA_DIR` | `/data` | The data directory |
| `CORTRIX_HTTP_PORT` | `8420` | The HTTP port |
| `CORTRIX_LOG_LEVEL` | `info` | The log level |
| `CORTRIX_PROFILE` | `quickstart` | `quickstart` is the default and is what the bundled Compose file uses. `full` provisions the embedding model and fails startup if it's missing. `lite` explicitly turns off the embedding model and the parsers, and is for development only |
| `CORTRIX_EMBEDDING_EXECUTION_PROVIDER` | `auto` | The execution provider for embedding |
| `CORTRIX_RERANKER_EXECUTION_PROVIDER` | `auto` | The execution provider for reranking |
| `CORTRIX_WEB_UI_ENABLED` | `true` | Whether the Web UI is on |
| `CORTRIX_AGENT_ENABLED` | `true` | Whether the built-in Agent is on |

> [!NOTE]
> The `deploy/docker-compose.yml` used by the quickstart doesn't need a `.env` file, and it keeps LLMs and the built-in Agent off. It reads only three variables from the environment or from `deploy/.env`: `CORTRIX_PUBLISH_HOST`, `CORTRIX_HTTP_PORT`, and `CORTRIX_SOURCE_REVISION`. Other settings in the file don't reach the container.

## Server {#server}

```yaml {title="config.yaml"}
server:
  host: "127.0.0.1"
  port: 8420
  thread_count: 4
```

`host` defaults to the loopback address. Turn on authentication before you bind to a non-loopback address, or the server refuses to start.

There's one exception, which the bundled Compose file relies on. Inside a container, the server can bind the container interface without authentication when `CORTRIX_SERVER_ALLOW_UNAUTHENTICATED_CONTAINER_BIND` is `true`, as long as the host port is published only to loopback.

## Authentication {#auth}

```yaml {title="config.yaml"}
auth:
  enabled: false
```

Running without authentication is accepted only when the server binds to the loopback address. With authentication on, the configuration looks like this:

```yaml {title="config.yaml"}
auth:
  enabled: true
  api_keys:
    - key_hash: "<SHA-256 of your key>"
      tenant_id: "default"
      permissions: 7           # READ=1, WRITE=2, ADMIN=4; 7 is all of them
      expires_at: 0            # 0 never expires
```

`key_hash` is the SHA-256 of the key. The configuration file never holds the key itself:

```bash
echo -n 'your-key' | shasum -a 256 | cut -d' ' -f1
```

## Logging {#log}

```yaml {title="config.yaml"}
log:
  level: "info"      # trace / debug / info / warn / error / critical / off
  format: "text"     # text for development, json for log collection
  output: "stdout"   # stdout / stderr / a file path
```

## LLM roles {#llm-roles}

Cortrix splits its use of LLMs into five independent roles. You configure each role separately, and a feature stays off until its role is configured.

| Role | Used for |
|---|---|
| `semantic_llm` | Query-side semantic processing such as intent classification |
| `vision_llm` | Refining image recognition after OCR. The model must accept image input |
| `agent_llm` | Chat in the built-in Agent |
| `doc_summary_llm` | Generating document summaries at ingest time |
| `enricher_llm` | Enriching content at ingest time |

None of these roles has anything to do with the local embedding and reranking models the quickstart uses. With no LLM role configured, embedding and reranking work as usual.

Every role takes the same four fields. A role counts as configured only when `provider`, `api_key`, and `model` are all set:

```yaml {title="config.yaml"}
semantic_llm:
  provider: "openai"
  api_key: "your-api-key"
  model: "gpt-4o-mini"
  base_url: "https://api.openai.com/v1"
```

`provider` can be `openai`, `glm`, `claude`, `ollama`, `deepseek`, or `mock`.

### Roles use two different protocols {#wire-protocols}

| Role | Called by | Protocol |
|---|---|---|
| `agent_llm` | The built-in Agent (Python) | Each provider's native protocol |
| The other four | `cortrix-server` (C++) | OpenAI-compatible only. Requests go to `{base_url}/chat/completions` |

That difference matters when you pick a provider. Take Claude as an example:

- For `agent_llm`, connect directly and don't set `base_url`:

  ```yaml {title="config.yaml"}
  agent_llm:
    provider: "claude"
    api_key: "your-api-key"
    model: "claude-haiku-4-5-20251001"
  ```

- For the other four roles, go through an OpenAI-compatible proxy gateway and point `base_url` at it:

  ```yaml {title="config.yaml"}
  enricher_llm:
    provider: "claude"
    api_key: "your-api-key"
    model: "claude-haiku-4-5-20251001"
    base_url: "https://your-openai-compatible-proxy.example.com/v1"
  ```

OpenAI, GLM, DeepSeek, and a local OpenAI-compatible gateway work directly in every role.

> [!CAUTION]
> Use placeholder values in documentation and examples. Keep real keys only in ignored local configuration files, and never commit them.

## Next steps {#next-steps}

- [Configuration reference](/docs/reference/configuration/)
- [Performance tuning](/docs/deploy/performance-tuning/)
- [Security boundaries](/docs/deploy/security/)
