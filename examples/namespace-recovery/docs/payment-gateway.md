# Payment gateway design

## Retries and idempotency

When a call to a downstream payment provider fails, the gateway retries at most three times, waiting 1, 2, and 4 seconds. Every payment request must carry an `idempotency_key`. The gateway deduplicates on it for 24 hours: when the same key arrives again, it returns the result of the first attempt and does not charge the customer twice.

## Reconciliation

At 2:00 every morning the gateway pulls the provider's statement and checks it line by line against the local ledger.
