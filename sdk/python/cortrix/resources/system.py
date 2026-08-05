"""System resource. ``/system/*`` (ARCH § 4.1.11).

This resource follows the implemented HTTP architecture. Endpoints include
``health/live`` / ``version`` /
``namespaces/{ns}/stats`` / ``agent_llm_config``. ``features()``
(``GET /system/features``) is **§2.12-only** -> § 2.12 wire (API spec to be
added -> D3.5).
"""

from __future__ import annotations

from typing import Any

from ..types import AgentLlmConfig, NsStats
from ._base import AsyncResource, SyncResource

PATH_HEALTH = "/system/health/live"
PATH_VERSION = "/system/version"
PATH_NS_STATS = "/system/namespaces/{ns}/stats"
PATH_AGENT_LLM_CONFIG = "/system/agent_llm_config"
PATH_FEATURES = "/system/features"  # §2.12-only -> D3.5 spec


class System(SyncResource):
    """System info. ``/system/*``."""

    def health(self) -> Any:
        """Health check. ``GET /system/health/live``."""
        return self._client._request("GET", PATH_HEALTH)

    def version(self) -> Any:
        """Version info. ``GET /system/version``."""
        return self._client._request("GET", PATH_VERSION)

    def namespace_stats(self, namespace: str) -> NsStats:
        """Per-NS metrics. ``GET /system/namespaces/{ns}/stats`` -> NsStats."""
        return self._client._request(
            "GET", PATH_NS_STATS.format(ns=namespace), response_model=NsStats
        )

    def agent_llm_config(self) -> AgentLlmConfig:
        """Get agent LLM config (redacted). ``GET /system/agent_llm_config``."""
        return self._client._request(
            "GET", PATH_AGENT_LLM_CONFIG, response_model=AgentLlmConfig
        )

    def features(self) -> Any:
        """Feature flags for this deployment. ``GET /system/features``
        (§2.12-only -> D3.5 spec)."""
        return self._client._request("GET", PATH_FEATURES)


class AsyncSystem(AsyncResource):
    """Async System (symmetric to :class:`System`)."""

    async def health(self) -> Any:
        return await self._client._request("GET", PATH_HEALTH)

    async def version(self) -> Any:
        return await self._client._request("GET", PATH_VERSION)

    async def namespace_stats(self, namespace: str) -> NsStats:
        return await self._client._request(
            "GET", PATH_NS_STATS.format(ns=namespace), response_model=NsStats
        )

    async def agent_llm_config(self) -> AgentLlmConfig:
        return await self._client._request(
            "GET", PATH_AGENT_LLM_CONFIG, response_model=AgentLlmConfig
        )

    async def features(self) -> Any:
        return await self._client._request("GET", PATH_FEATURES)
