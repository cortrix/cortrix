"""Shared test fixtures and import-path setup for the Cortrix Agent test suite."""

from __future__ import annotations

import sys
from pathlib import Path

_AGENT_ROOT = Path(__file__).resolve().parent.parent

# Make the agent package importable.
if str(_AGENT_ROOT) not in sys.path:
    sys.path.insert(0, str(_AGENT_ROOT))

# Make the sibling Python SDK importable (cortrix/sdk/python), mirroring main.py —
# the agent dogfoods the SDK rather than pip-installing it (design section 5.2).
_SDK_PATH = _AGENT_ROOT.parent / "sdk" / "python"
if _SDK_PATH.is_dir() and str(_SDK_PATH) not in sys.path:
    sys.path.insert(0, str(_SDK_PATH))
