"""Pin the client surfaces removed because no released server serves them.

``client.sql`` and ``client.sync`` called paths that no Cortrix server has
served, so every call failed with a not-found error. Their result types went
with them.
"""

from __future__ import annotations

import pytest

import cortrix
from cortrix import AsyncCortrix, Cortrix
from cortrix import types as cortrix_types

REMOVED_GROUPS = ("sql", "sync")
REMOVED_TYPES = ("SqlRequest", "SqlResult", "SyncStatus")


@pytest.mark.parametrize("group", REMOVED_GROUPS)
def test_client_has_no_removed_group(client: Cortrix, group: str) -> None:
    assert not hasattr(client, group)


@pytest.mark.parametrize("group", REMOVED_GROUPS)
async def test_async_client_has_no_removed_group(aclient: AsyncCortrix, group: str) -> None:
    assert not hasattr(aclient, group)


@pytest.mark.parametrize("name", REMOVED_TYPES)
def test_removed_types_are_not_exported(name: str) -> None:
    assert name not in cortrix.__all__
    assert not hasattr(cortrix, name)
    assert name not in cortrix_types.__all__
    assert not hasattr(cortrix_types, name)
