#!/usr/bin/env python3
"""Call Cortrix through its MCP server, the way an MCP-capable agent does.

It starts `cortrix-mcp` over stdio, lists the tools, checks health, and runs
one query across the Quick Start's `demo` namespace and one that doesn't exist.
A successful tool result has the same two layers: `data` and `meta`; a failure comes
back with `isError` set instead -- a one-line message followed by the structured error
as JSON.

Usage:
    pip install cortrix-mcp mcp
    python mcp_client.py [--base-url http://127.0.0.1:8420]
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

QUESTION = "What does semantic storage keep close to the agents that need it?"


class ToolError(Exception):
    """A Cortrix tool call that came back with `isError` set."""


def payload(result) -> dict:
    """Return the JSON document a Cortrix tool call carries in its first text block."""
    text = result.content[0].text
    if getattr(result, "is_error", False) or getattr(result, "isError", False):
        # An error result is a one-line message followed by the structured error as JSON.
        raise ToolError(text)
    return json.loads(text)


async def run(base_url: str) -> int:
    bin_dir = os.path.dirname(sys.executable)
    command = shutil.which("cortrix-mcp") or os.path.join(bin_dir, "cortrix-mcp")
    server = StdioServerParameters(
        command=command,
        env={**os.environ, "CORTRIX_URL": base_url, "CORTRIX_NAMESPACE": "demo"},
    )
    async with stdio_client(server) as (read, write), ClientSession(read, write) as session:
        await session.initialize()

        try:
            tools = await session.list_tools()
            print(f"tools: {len(tools.tools)}")

            health = payload(await session.call_tool("cortrix_health", {}))
            print(f"health: {health['data']['status']} (version {health['data']['version']})")

            answer = payload(await session.call_tool(
                "cortrix_cross_ns_query",
                {"query": QUESTION, "namespaces": ["demo", "not-a-real-namespace"], "top_k": 2},
            ))
            details = answer["meta"]["structured_data"]
            print(f"coverage_ratio: {details['coverage_ratio']}")
            print(f"results: {len(answer['data']['results'])}")
            for failed in details["namespaces_failed"]:
                print(f"not searched: {failed['namespace']} "
                      f"(retryable={json.dumps(failed['retryable'])}, "
                      f"category={failed['category']})")
            print(f"session_id: {details['session_id']}")
        except ToolError as error:
            print(f"Cortrix tool error: {error}", file=sys.stderr)
            return 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base-url", default="http://127.0.0.1:8420")
    args = parser.parse_args()
    return asyncio.run(run(args.base_url))


if __name__ == "__main__":
    sys.exit(main())
