"""Resource-style API modules (``client.documents`` / ``client.memory`` / ...).

Method names follow the P03 design's Resource-style surface (the P12/P14
Cross-3 naming SoT); HTTP paths + request/response shapes follow the frozen
OpenAPI spec (``api/openapi.yaml``). Paths are centralised as module-level
constants so a contract change is a one-line edit.
"""
