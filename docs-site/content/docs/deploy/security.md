---
title: Security boundaries
description: Where Cortrix stands today on authentication, network exposure, and logging, and how to report a vulnerability.
weight: 50
---

Read this before you deploy Cortrix anywhere beyond your own machine. It covers what Cortrix can and can't guarantee about security today.

> [!IMPORTANT]
> Cortrix is in pre-release. Tenant isolation, access control, and log redaction are in development. API key configuration exists, but its behavior hasn't been verified for this documentation. See [Compatibility and status](/docs/resources/compatibility/). Until they're available, deploy Cortrix only on a network you control.

## Default protections {#defaults}

The bundled Compose deployment is configured this way in `v1.0.0-rc.2`:

- **It listens only on the loopback address.** The Compose deployment publishes the API at `127.0.0.1:8420`, so other machines can't reach it.
- **Admin endpoints accept loopback requests only.** An admin request from a non-loopback address gets 403 `CX_ERR_ADMIN_LOOPBACK_REQUIRED`.
- **The metrics port isn't published.** It's reachable only from inside the container.
- **External LLMs are off by default.** The quickstart sends no documents or queries to any external LLM service.

The configuration template also states that running without authentication is accepted only when the server binds to the loopback address, and that authentication must be on before the server binds to a non-loopback address.

## What not to do {#do-not}

- **Don't expose an unauthenticated service to a network you don't control.** Access opened with `CORTRIX_PUBLISH_HOST` has no authentication at all, and is only for an isolated test network you control.
- **Don't use a local run with authentication off to prove that access control works.** With authentication off, denial behavior for RBAC, tenant isolation, ACLs, and quotas can't be demonstrated.
- **Don't assume logs are redacted.** Startup and runtime logs can contain sensitive content. Check them yourself before you collect or share them.
- **Don't put keys in files that get committed.** Keep real API keys only in ignored local configuration.

## Authentication schemes {#auth-schemes}

Cortrix authenticates requests with an API key, sent in the `X-API-Key` header. The configuration file stores an API key as its SHA-256 hash. To set one up, see [Configuration](/docs/deploy/configuration/#auth).

## Before production {#before-production}

At a minimum, verify these in your target environment:

- Authentication mode and credential handling
- Tenant isolation and RBAC behavior
- Namespace ACLs and quotas
- Logging and redaction
- Data persistence and backup strategy
- Deployment topology and resource limits

Treat anything you haven't verified as not ready.

## Report a vulnerability {#reporting}

Report suspected vulnerabilities privately to [security@cortrix.ai](mailto:security@cortrix.ai). Don't open a public issue for an unpatched vulnerability, credentials, customer data, or details of exploitable infrastructure.

Include:

- The affected 40-character commit hash
- The impact
- A minimal reproduction
- Sanitized evidence

Don't include secrets unless the security response team explicitly asks for them and gives you a secure way to send them.

The team acknowledges receipt within 5 business days. An acknowledgment means the report has been received and routed. It isn't a promise that analysis, a fix, or a release will be finished in that time.

Don't disclose the vulnerability publicly before a fix is available or before a disclosure date both sides agree on.

Until the first stable release, security support covers only the latest commit on the default branch.

## Next steps {#next-steps}

- [Compatibility and status](/docs/resources/compatibility/)
- [Configuration](/docs/deploy/configuration/)
- [Deploy with Docker Compose](/docs/deploy/docker-compose/)
