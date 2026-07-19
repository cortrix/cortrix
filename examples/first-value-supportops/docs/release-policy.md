# Release Policy

Initial release policy: retry failed Cortrix API calls up to 2 times before escalating to a human reviewer.

Retry is allowed only when the failure is explicitly marked as retryable.

Contract errors should not be retried blindly. If an agent receives a machine-readable contract error, it should repair the request shape and try again only when the repaired request is valid.

Every launch-facing demo must show a source-backed answer, a visible limitation, and a plain-language explanation of what Cortrix replaces compared with hand-written RAG glue code.
