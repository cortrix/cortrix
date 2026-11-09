"""Decide what to do after a Cortrix error, from the structured fields alone.

decide() reads code, retryable, category, retry_after_ms and structured_data.
It never reads message, which is for people and may change between releases.
Only the standard library is used, so you can copy this file as it is.
"""

MAX_RETRIES = 3
BACKOFF_START_MS = 1000


def decide(error, attempt=0):
    """Map one error object to an action dict.

    Actions: fix_and_retry, retry, check_then_escalate, ask_human, stop, give_up.
    `error` is the "error" object of an error response, or one entry of "failed".
    `attempt` counts the retries already made.
    """
    code = error.get("code") or error.get("error_code")
    category = error.get("category")
    data = error.get("structured_data") or {}
    base = {"code": code, "category": category}

    # A destructive operation that needs an explicit confirmation is for a human.
    if code == "CX_ERR_OPS_CONFIRM_REQUIRED":
        return {**base, "action": "ask_human"}
    # Transient and timeout errors are worth retrying, within a budget.
    if error.get("retryable") or category == "timeout":
        if attempt >= MAX_RETRIES:
            return {**base, "action": "give_up", "why": "retry budget used up"}
        wait_ms = error.get("retry_after_ms") or BACKOFF_START_MS * 2**attempt
        return {**base, "action": "retry", "wait_ms": wait_ms}
    # Auth: do not swap keys blindly. Check the request first, then escalate.
    if category == "auth":
        return {**base, "action": "check_then_escalate"}
    # Quota will not clear by retrying. Stop and report.
    if category == "quota":
        return {**base, "action": "stop"}
    # Permanent: act only if structured_data names the next step.
    if category == "permanent":
        if isinstance(data.get("missing_param"), str):
            return {**base, "action": "fix_and_retry", "add_param": data["missing_param"]}
        if isinstance(data.get("live_doc_id"), str):
            return {**base, "action": "fix_and_retry", "delete_first": data["live_doc_id"]}
        return {**base, "action": "give_up", "why": "no next step in structured_data"}
    return {**base, "action": "give_up", "why": "unknown category"}


def decide_items(body):
    """Handle an HTTP 200 response that lists per-item failures in `failed`.

    The status is 200 even when some items failed, so read coverage_ratio and
    apply the same rules to every failed item.
    """
    failed = body.get("failed") or []
    return {
        "coverage_ratio": body.get("coverage_ratio"),
        "items": [{"id": f.get("id"), **decide(f)} for f in failed],
    }
