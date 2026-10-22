"""Shared test fixtures: a fake P03 client recording SDK calls + HTTP fallbacks.

No real network. ``FakeClient`` mimics the parts of ``cortrix.Cortrix`` that
``CortrixToolKit`` touches:

  * Resource verbs (``system.health`` / ``search`` / ``documents.*`` /
    ``namespaces.*`` / ``memory.*`` / ``watchers.*``) — return canned dataclass
    objects so ``_to_dict`` exercises the real dataclass->dict path.
  * ``_request(...)`` — the HTTP fallback hook; records the call and returns a
    canned dict (or raises a programmed ``CortrixError``).

Tests assert which path a method took (SDK verb vs ``_request``) and that the
GEN-Agent 4-field ``CortrixError`` propagates unchanged.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any, List, Optional

# Belt-and-braces sys.path mount (mirrors the repo-root conftest) so this file
# also works if pytest is invoked from the tests/ directory directly.
_TESTS = Path(__file__).resolve().parent
for _p in (_TESTS.parent / "src", _TESTS.parent.parent / "sdk" / "python"):
    if _p.is_dir() and str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

import pytest  # noqa: E402

from cortrix.types import (  # noqa: E402
    Document,
    DocumentTask,
    Memory,
    Namespace,
    QueryMeta,
    QueryResult,
    QueryResultItem,
    Watcher,
)
from cortrix.types.lists import (  # noqa: E402
    DocumentList,
    MemoryList,
    NamespaceList,
    WatcherList,
)


class _Recorder:
    """Records the last call's args for a single resource verb."""

    def __init__(self, name: str, log: list, return_value: Any):
        self._name = name
        self._log = log
        self._return_value = return_value

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        self._log.append((self._name, args, kwargs))
        rv = self._return_value
        return rv(*args, **kwargs) if callable(rv) else rv


class _Resource:
    """A bag of recorder verbs (e.g. ``client.documents``)."""

    def __init__(self, name: str, log: list, verbs: dict):
        self._name = name
        self._log = log
        for verb, rv in verbs.items():
            setattr(self, verb, _Recorder(f"{name}.{verb}", log, rv))


# --- canned dataclass responses (exercise the real dataclass -> dict path) ----

def _doc_task() -> DocumentTask:
    return DocumentTask(task_id="task-1", status="queued", namespace="default")


def _document() -> Document:
    return Document(document_id="doc-1", status="ready", namespace="default", filename="f.txt")


def _namespace() -> Namespace:
    return Namespace(namespace="default", status="active")


def _query_result() -> QueryResult:
    return QueryResult(
        results=[QueryResultItem(child_id="c1", content="hit", score=0.9, namespace="default")],
        meta=QueryMeta(
            namespaces_queried=["default"],
            namespaces_succeeded=["default"],
            coverage_ratio=1.0,
            latency_ms=12,
        ),
    )


def _memory() -> Memory:
    return Memory(memory_id="mem-1", content="likes tea", memory_type="preference", status="valid")


def _memory_invalidated() -> Memory:
    return Memory(memory_id="mem-1", content="likes tea", memory_type="preference", status="invalidated")


class FakeClient:
    """Stand-in for ``cortrix.Cortrix``.

    ``calls`` records ``(name, args, kwargs)`` for resource verbs.
    ``http_calls`` records ``(method, path, json, params, timeout)`` for
    ``_request`` (the HTTP fallback). Set ``http_error`` to a ``CortrixError`` to
    make the next ``_request`` raise it; set ``verb_error`` similarly for the
    next resource verb (via ``raise_on``).
    """

    def __init__(self) -> None:
        self.calls: list = []
        self.http_calls: list = []
        self._http_return: Any = {"ok": True}
        self._http_error: Optional[BaseException] = None
        self.closed = False

        self.system = _Resource("system", self.calls, {"health": {"status": "ok", "version": "1.0.0-rc.2"}})
        self.documents = _Resource(
            "documents",
            self.calls,
            {
                "list": DocumentList(documents=[_document()], total=1),
                "status": _document(),
                "task_progress": _doc_task(),
                "cancel_task": DocumentTask(task_id="task-1", status="cancelled"),
            },
        )
        self.namespaces = _Resource(
            "namespaces",
            self.calls,
            {
                "list": NamespaceList(namespaces=[_namespace()], total=1),
                "create": _namespace(),
            },
        )
        self.watchers = _Resource(
            "watchers",
            self.calls,
            {
                "add": Watcher(id="w-1", path="/data", target_namespaces=["default"], recursive=True),
                "list": WatcherList(watchers=[Watcher(id="w-1", path="/data")], total=1),
            },
        )
        self.memory = _Resource(
            "memory",
            self.calls,
            {
                "list": MemoryList(memories=[_memory()], total=1),
                "create": _memory(),
                "edit": _memory(),
                "update": _memory(),
                "invalidate": _memory_invalidated(),
                "delete": _memory_invalidated(),
            },
        )

    # top-level shortcut (client.search)
    def search(self, *args: Any, **kwargs: Any) -> QueryResult:
        self.calls.append(("search", args, kwargs))
        return _query_result()

    # HTTP fallback hook
    def _request(
        self,
        method: str,
        path: str,
        *,
        json: Optional[dict] = None,
        params: Optional[dict] = None,
        timeout: Optional[float] = None,
        **extra: Any,
    ) -> Any:
        self.http_calls.append((method, path, json, params, timeout))
        if self._http_error is not None:
            err, self._http_error = self._http_error, None
            raise err
        rv = self._http_return
        return rv() if callable(rv) else rv

    def set_http_return(self, value: Any) -> None:
        self._http_return = value

    def set_http_error(self, error: BaseException) -> None:
        self._http_error = error

    def names_called(self) -> List[str]:
        return [c[0] for c in self.calls]

    def http_paths(self) -> List[str]:
        return [c[1] for c in self.http_calls]

    def close(self) -> None:
        self.closed = True


@pytest.fixture
def fake_client() -> FakeClient:
    return FakeClient()


@pytest.fixture
def kit(fake_client: FakeClient):
    from cortrix_skills import CortrixToolKit

    return CortrixToolKit(client=fake_client, default_namespace="default")
