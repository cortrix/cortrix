"""Agent executors (design section 15.2 IAgentExecutor + ChatExecutor).

``IAgentExecutor`` is the V1.5-forward interface; Phase 1 V1.0 ships only
``ChatExecutor`` — a fixed RAG flow with NO autonomous tool selection (the design's
"chat mode"). ``ToolUseExecutor`` (V1.5) and PlanExecuteExecutor (V2) are reserved and
raise NotImplementedError, mirroring the CRAG IClassifier / query routing IRetrievalFallback style.

Naming rule: the product is the "Cortrix Agent"; the executor class
is ``ChatExecutor`` (never "ChatbotExecutor"), preserving the mode distinction with the
future ToolUseExecutor / PlanExecuteExecutor.

ChatExecutor pipeline (one turn):
  1. RAG retrieve via the Python SDK with L1 retry N=3 on transient errors (design section 9.2).
  2. On RAG exhaustion -> L2 fallback: prompt the LLM with no context, mark
     rag_status="degraded".
  3. Build the injection-hardened prompt (design section 6.5) and stream the LLM.
  4. If the LLM itself fails after a degraded RAG (or permanent error) -> L3:
     raise AgentError(CX_ERR_AGENT_RAG_FAILED) so the route emits the 4-field error.

The executor yields ``StreamEvent`` objects; the route layer encodes them as SSE
(``data:{chunk}`` ... ``data:{meta}`` ``data:[DONE]`` — design section 9.1).
"""

from __future__ import annotations

import asyncio
import time
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any, AsyncIterator, Optional

import structlog

from .errors import AgentError
from .explain import (
    RAG_STATUS_DEGRADED,
    RAG_STATUS_SUCCESS,
    build_response_meta,
)
from .prompt import build_chat_prompt
from .sdk_rag import RagResult, SdkRagProvider

logger = structlog.get_logger()

# L1 retry policy (design section 9.2): N=3 attempts, exponential-ish backoff.
_RAG_MAX_ATTEMPTS = 3
_RAG_BACKOFF_MS = [500, 1000]  # waited before attempt 2 and attempt 3


@dataclass
class ChatContext:
    """Per-request context handed to an executor.

    ``tenant_id`` is A-class: always carried, value ``None`` in CE single-tenant
    (design section 1.7). ``explain`` / ``debug`` gate the B / C meta tiers.
    """

    session_id: str
    tenant_id: Optional[str] = None
    namespace: Optional[str] = None
    user_id: Optional[str] = None
    explain: bool = False
    debug: bool = False
    history: list[dict] = field(default_factory=list)


@dataclass
class StreamEvent:
    """An executor output event. Either a text chunk or the terminal meta object."""

    kind: str  # "chunk" | "meta"
    chunk: Optional[str] = None
    meta: Optional[dict[str, Any]] = None


class IAgentExecutor(ABC):
    """Phase 1 V1.0 ships ChatExecutor; V1.5 adds ToolUseExecutor, V2 PlanExecuteExecutor."""

    @abstractmethod
    def execute(self, message: str, context: ChatContext) -> AsyncIterator[StreamEvent]:
        """Stream the agent's response for one turn."""
        raise NotImplementedError


class ToolUseExecutor(IAgentExecutor):
    """V1.5 reserved — LLM autonomous tool selection over the Cortrix tool registry.

    Not implemented in V1.0 (design section 15.2 / non-goal section 1.8).
    """

    def execute(self, message: str, context: ChatContext) -> AsyncIterator[StreamEvent]:
        raise NotImplementedError("ToolUseExecutor is a V1.5 feature (design section 15.2).")


class ChatExecutor(IAgentExecutor):
    """V1.0 default executor — fixed RAG chat flow (design section 15.2)."""

    def __init__(self, rag: SdkRagProvider, llm: Any, model_name: str = "") -> None:
        self._rag = rag
        self._llm = llm
        self._model_name = model_name

    async def _retrieve_with_retry(
        self, message: str, namespace: Optional[str]
    ) -> tuple[Optional[RagResult], Optional[Exception]]:
        """L1: retry RAG up to N=3 on transient errors (design section 9.2).

        Returns ``(result, None)`` on success or ``(None, last_error)`` after exhaustion.
        """
        last_err: Optional[Exception] = None
        for attempt in range(_RAG_MAX_ATTEMPTS):
            try:
                result = await self._rag.retrieve(message, namespace=namespace)
                return result, None
            except Exception as err:  # noqa: BLE001 - mapped to degradation policy
                last_err = err
                logger.warning("rag_attempt_failed", attempt=attempt + 1, error=str(err))
                if attempt < len(_RAG_BACKOFF_MS):
                    await asyncio.sleep(_RAG_BACKOFF_MS[attempt] / 1000.0)
        return None, last_err

    async def execute(  # type: ignore[override]
        self, message: str, context: ChatContext
    ) -> AsyncIterator[StreamEvent]:
        t0 = time.monotonic()

        # --- 1. RAG retrieve (L1 retry) ---
        rag_result, rag_err = await self._retrieve_with_retry(message, context.namespace)
        rag_latency = int((time.monotonic() - t0) * 1000)

        rag_failed_detail: Optional[dict[str, Any]] = None
        if rag_result is not None:
            rag_status = RAG_STATUS_SUCCESS
            chunks = rag_result.chunks
        else:
            # L2: fall back to LLM-only, mark degraded (design section 9.2).
            rag_status = RAG_STATUS_DEGRADED
            chunks = []
            rag_failed_detail = {"cortrix_server_error": str(rag_err) if rag_err else "unknown"}
            logger.warning("rag_degraded_llm_only", error=str(rag_err) if rag_err else None)

        # Prefix each chunk with its source_path so the prompt's "Cite sources using
        # [source_path]" instruction is actionable — without it the LLM only has chunk
        # text and (correctly) reports it cannot name/list the source files.
        rag_texts = [f"[source_path: {c.source_path}]\n{c.content}" for c in chunks]
        chunk_ids = [c.chunk_id for c in chunks]

        # Best-effort namespace document inventory so the chat can answer "what files /
        # documents are here" AND "where did this come from / how do I find the source"
        # — RAG retrieval alone only sees query-relevant chunks, not the full catalog or
        # the origin. Each line carries the filename + a source locator (full path for
        # watch_dir, S3/connector URI, or "stored in Cortrix" for uploads). Never blocks
        # the chat (getattr guards stub rag; the lister is itself fail-soft, returns []).
        doc_inventory: list[str] = []
        lister = getattr(self._rag, "list_document_summaries", None)
        if lister is not None:
            try:
                summaries = await lister(namespace=context.namespace)
                doc_inventory = [
                    f"{s.get('name')} — location: {s.get('origin')}" for s in summaries
                ]
            except Exception:  # noqa: BLE001 — inventory is best-effort
                doc_inventory = []

        # --- 2. Build injection-hardened prompt (design section 6.5) ---
        prompt = build_chat_prompt(
            message, rag_texts, history=context.history, doc_inventory=doc_inventory
        )

        # --- 3. Stream the LLM ---
        t_llm = time.monotonic()
        assistant_text = ""
        llm_failed_detail: Optional[dict[str, Any]] = None
        try:
            async for piece in self._llm.stream_chat(prompt, message):
                if piece:
                    assistant_text += piece
                    yield StreamEvent(kind="chunk", chunk=piece)
        except Exception as llm_err:  # noqa: BLE001
            logger.warning("llm_stream_failed", error=str(llm_err))
            llm_failed_detail = {"provider_error": str(llm_err)}
            if rag_status == RAG_STATUS_DEGRADED:
                # L3: both RAG and LLM unavailable -> hard error (design section 9.2).
                raise AgentError(
                    "CX_ERR_AGENT_RAG_FAILED",
                    retryable=False,
                    structured_data={
                        "cortrix_server_error": rag_failed_detail.get("cortrix_server_error")
                        if rag_failed_detail
                        else None,
                        "fallback_attempted": True,
                        "llm_error": str(llm_err),
                    },
                ) from llm_err
            # RAG succeeded but LLM failed: degrade to returning raw retrieved chunks.
            rag_status = RAG_STATUS_DEGRADED
            fallback = "LLM unavailable. Returning top retrieved results:\n\n"
            for i, c in enumerate(chunks, 1):
                fallback += "[" + str(i) + "] (" + c.source_path + "): " + c.content + "\n\n"
            assistant_text = fallback if chunks else "LLM unavailable and no documents were retrieved."
            yield StreamEvent(kind="chunk", chunk=assistant_text)

        llm_latency = int((time.monotonic() - t_llm) * 1000)
        total_latency = int((time.monotonic() - t0) * 1000)

        # --- 4. Terminal meta (A always; B if explain; C if debug+failure) ---
        meta = build_response_meta(
            session_id=context.session_id,
            tenant_id=context.tenant_id,
            chunk_ids=chunk_ids,
            latency_ms={"rag": rag_latency, "llm": llm_latency, "total": total_latency},
            rag_status=rag_status,
            explain=context.explain,
            debug=context.debug,
            prompt_text=prompt,
            rag_chunks_full=[c.to_full() for c in chunks],
            model_used=self._model_name or None,
            llm_token_count={},  # TODO(integration): real token accounting from provider usage.
            rag_call_failed_detail=rag_failed_detail,
            llm_call_failed_detail=llm_failed_detail,
        )
        # Expose the final assistant text to the route layer (for session logging).
        meta["_assistant_text"] = assistant_text
        yield StreamEvent(kind="meta", meta=meta)
