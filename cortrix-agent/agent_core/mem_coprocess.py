"""F48 <-> MEM co-processing (design section 13 / F48-rev-9/10/11).

After each Cortrix Agent chat turn the conversation is fed back into Cortrix's memory
system: the turn is logged (F13 interaction_log) and that same SDK call triggers MEM02
LLM extraction server-side. It runs fire-and-forget so a logging/extraction failure never
blocks the user's chat response (design F48-rev-9). MEM05 user isolation is honored by
always passing a user_id (design F48-rev-11).

Standalone discipline (D3): the P03 SDK ``client.memory.log`` call is exercised against a
stubbed SDK here; real cortrix-server interaction-log persistence + the live MEM02 worker
are TODO(D3.5).
"""

from __future__ import annotations

from typing import Any, Optional

import structlog

logger = structlog.get_logger()

# CE single-tenant fallback subject when no authenticated user_id is present. The real
# JWT-derived user_id is TODO(D3.5) (design P-5 / section 8.2).
_DEFAULT_USER_ID = "default"


class MemoryCoprocessor:
    """Feeds completed chat turns into Cortrix memory via the P03 SDK (design section 13).

    ``client.memory.log`` writes the F13 interaction_log AND triggers MEM02 auto
    extraction server-side, so a single call covers F48-rev-9 (interaction log) and
    F48-rev-10 (MEM02 trigger).
    """

    def __init__(self, client: Any, namespace: str = "default") -> None:
        self._client = client
        self._namespace = namespace

    async def record_turn(
        self,
        session_id: str,
        user_message: str,
        assistant_text: str,
        *,
        user_id: Optional[str] = None,
        namespace: Optional[str] = None,
    ) -> None:
        """Log a completed turn (F13) + trigger MEM02 extraction; never raises.

        MEM05: a user_id is always sent (CE single-tenant falls back to a default
        subject). On any failure we log a warning carrying CX_ERR_F48_INTERACTION_LOG_FAILED
        and return — the chat turn must not be blocked (design F48-rev-9).
        """
        uid = user_id or _DEFAULT_USER_ID
        ns = namespace or self._namespace
        try:
            await self._client.memory.log(
                ns,
                query=user_message,
                response=assistant_text,
                user_id=uid,
                session_id=session_id,
            )
        except Exception as err:  # noqa: BLE001 - logging must never break the chat turn
            logger.warning(
                "interaction_log_failed",
                code="CX_ERR_F48_INTERACTION_LOG_FAILED",
                session_id=session_id,
                error=str(err),
            )
