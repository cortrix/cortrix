"""Resource-style API modules (``client.documents`` / ``client.memory`` / ...).

Method names follow the Python SDK design's Resource-style surface (the MCP server/Skill SDK
Cross-3 naming SoT); HTTP paths + request/response shapes follow the frozen
OpenAPI spec (``api/openapi.yaml``). Paths are centralized as module-level
constants so a contract change is a one-line edit.
"""
