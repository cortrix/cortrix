"""Python version compatibility helpers (3.9+).

``typing.Literal`` and PEP 604 ``X | None`` syntax via ``from __future__ import
annotations`` cover most needs; this module centralises the few imports that
differ across 3.9–3.12 so the rest of the package imports from one place.
"""

from __future__ import annotations

import sys
from typing import Literal, Protocol, runtime_checkable

PY_39 = sys.version_info[:2] == (3, 9)

__all__ = ["Literal", "Protocol", "runtime_checkable", "PY_39"]
