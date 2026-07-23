# Agent Setup Guide

The SupportOps team uses Cortrix as the shared context layer for support and release agents.

Agents should query Cortrix with a `namespaces` array. Use the unique namespace created for the current run, for example:

```json
{
  "namespaces": ["fv_supportops_<run_id>"]
}
```

The agent should keep source metadata visible in every reviewable answer. A teammate should be able to inspect which document, ticket, or policy shaped the result.

The first version of the workflow is intentionally small:

- load a few support and release documents;
- load a small support ticket export;
- ask a source-backed question;
- inspect the answer, source, trace, and limitation;
- hand the evidence to another reviewer.

The setup goal is not to hide all complexity. The goal is to keep context, source, and review artifacts together instead of spreading them across local scripts and prompt snapshots.
