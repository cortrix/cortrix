"""In-memory session store with an N=10 sliding multi-turn window (design P-2 / section 9.1).

Decision P-2 (design section 9.1): Phase 1 V1.0 keeps session history in an in-process
dict; each session holds at most ``WINDOW_SIZE`` turns (a turn = one user message plus
its assistant reply). When the window overflows, the oldest turn is dropped (FIFO).

V1.5 upgrade (memory extraction integration) is intentionally deferred — see TODO(integration) markers in
the executor where memory co-processing wires to the real cortrix-server.
"""

from __future__ import annotations

import threading
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Optional

# Design section 9.1 / section 12.bis.1: maximum multi-turn context window (N=10).
WINDOW_SIZE = 10


def _now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


@dataclass
class _Turn:
    """One conversation turn: a user message and its assistant reply."""

    user: dict[str, Any]
    assistant: dict[str, Any]


@dataclass
class _Session:
    session_id: str
    tenant_id: Optional[str] = None
    created_at: str = field(default_factory=_now_iso)
    turns: list[_Turn] = field(default_factory=list)


class SessionStore:
    """Thread-safe in-memory session store (P-2). Not durable; process-scoped.

    A turn is appended atomically (user + assistant together), mirroring how the
    real cortrix-server logs an interaction. The window is trimmed FIFO so a session
    never exceeds :data:`WINDOW_SIZE` turns.
    """

    def __init__(self, window_size: int = WINDOW_SIZE) -> None:
        self._window = window_size
        self._sessions: dict[str, _Session] = {}
        self._lock = threading.Lock()

    def get_or_create(self, session_id: str, tenant_id: Optional[str] = None) -> None:
        """Ensure a session exists (idempotent)."""
        with self._lock:
            if session_id not in self._sessions:
                self._sessions[session_id] = _Session(
                    session_id=session_id, tenant_id=tenant_id
                )

    def append_turn(
        self,
        session_id: str,
        user_message: str,
        assistant_message: str,
        *,
        tenant_id: Optional[str] = None,
    ) -> None:
        """Append a user+assistant turn, trimming to the FIFO window (P-2)."""
        with self._lock:
            sess = self._sessions.get(session_id)
            if sess is None:
                sess = _Session(session_id=session_id, tenant_id=tenant_id)
                self._sessions[session_id] = sess
            ts = _now_iso()
            sess.turns.append(
                _Turn(
                    user={"role": "user", "content": user_message, "timestamp": ts},
                    assistant={"role": "assistant", "content": assistant_message, "timestamp": ts},
                )
            )
            # FIFO sliding window: drop oldest turns beyond WINDOW_SIZE.
            if len(sess.turns) > self._window:
                sess.turns = sess.turns[-self._window :]

    def recent_messages(self, session_id: str) -> list[dict[str, Any]]:
        """Flatten the windowed turns into chronological role/content messages.

        Used to inject prior context into the next prompt (oldest first).
        Returns an empty list for an unknown session.
        """
        with self._lock:
            sess = self._sessions.get(session_id)
            if sess is None:
                return []
            out: list[dict[str, Any]] = []
            for turn in sess.turns:
                out.append(dict(turn.user))
                out.append(dict(turn.assistant))
            return out

    def exists(self, session_id: str) -> bool:
        with self._lock:
            return session_id in self._sessions

    def get_history(self, session_id: str) -> Optional[dict[str, Any]]:
        """Return the GET /sessions/:id payload (design section 9.1) or ``None``.

        Shape::

            {session_id, messages:[{role, content, timestamp}], window_size,
             created_at, tenant_id}

        ``tenant_id`` is an A-class field: always present, value ``None`` in the CE
        single-tenant environment (design section 1.7).
        """
        with self._lock:
            sess = self._sessions.get(session_id)
            if sess is None:
                return None
            messages: list[dict[str, Any]] = []
            for turn in sess.turns:
                messages.append(dict(turn.user))
                messages.append(dict(turn.assistant))
            return {
                "session_id": sess.session_id,
                "messages": messages,
                "window_size": self._window,
                "created_at": sess.created_at,
                "tenant_id": sess.tenant_id,
            }

    def clear(self) -> None:
        """Drop all sessions (test helper)."""
        with self._lock:
            self._sessions.clear()
