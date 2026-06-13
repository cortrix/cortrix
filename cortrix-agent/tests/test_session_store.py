"""Unit tests for the in-memory N=10 sliding-window session store (design P-2)."""

from __future__ import annotations

from agent_core.session_store import WINDOW_SIZE, SessionStore


def test_window_size_default_is_ten():
    assert WINDOW_SIZE == 10
    assert SessionStore()._window == 10


def test_append_turn_creates_session_and_two_messages():
    store = SessionStore()
    store.append_turn("s1", "hi", "hello", tenant_id=None)
    msgs = store.recent_messages("s1")
    assert len(msgs) == 2  # one turn = user + assistant
    assert msgs[0]["role"] == "user" and msgs[0]["content"] == "hi"
    assert msgs[1]["role"] == "assistant" and msgs[1]["content"] == "hello"
    assert "timestamp" in msgs[0]


def test_fifo_window_drops_oldest_turn_beyond_ten():
    store = SessionStore()
    for i in range(12):  # 12 turns, window keeps last 10
        store.append_turn("s1", f"u{i}", f"a{i}")
    msgs = store.recent_messages("s1")
    assert len(msgs) == WINDOW_SIZE * 2  # 10 turns * (user+assistant)
    # Oldest two turns (u0/u1) dropped; window starts at u2.
    assert msgs[0]["content"] == "u2"
    assert msgs[-1]["content"] == "a11"


def test_get_history_shape_matches_design_section_9_1():
    store = SessionStore()
    store.get_or_create("s1", tenant_id=None)
    store.append_turn("s1", "q", "r", tenant_id=None)
    payload = store.get_history("s1")
    assert set(payload.keys()) == {
        "session_id",
        "messages",
        "window_size",
        "created_at",
        "tenant_id",
    }
    assert payload["session_id"] == "s1"
    assert payload["window_size"] == 10
    assert payload["tenant_id"] is None  # A-class null in CE
    assert len(payload["messages"]) == 2


def test_get_history_unknown_session_returns_none():
    assert SessionStore().get_history("nope") is None


def test_recent_messages_unknown_session_returns_empty():
    assert SessionStore().recent_messages("nope") == []


def test_tenant_id_persisted_on_first_turn():
    store = SessionStore()
    store.append_turn("s1", "q", "r", tenant_id="tenant-7")
    assert store.get_history("s1")["tenant_id"] == "tenant-7"


def test_get_or_create_is_idempotent():
    store = SessionStore()
    store.get_or_create("s1")
    store.get_or_create("s1")
    assert store.exists("s1")
    assert store.recent_messages("s1") == []  # no turns yet


def test_clear_drops_all_sessions():
    store = SessionStore()
    store.append_turn("s1", "q", "r")
    store.clear()
    assert not store.exists("s1")


def test_custom_window_size():
    store = SessionStore(window_size=2)
    for i in range(5):
        store.append_turn("s1", f"u{i}", f"a{i}")
    msgs = store.recent_messages("s1")
    assert len(msgs) == 4  # 2 turns retained
    assert msgs[0]["content"] == "u3"
