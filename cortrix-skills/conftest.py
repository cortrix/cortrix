"""Test bootstrap: make ``cortrix_skills`` and ``cortrix`` importable on sys.path.

This session's APFS/editable-install instability occasionally drops the
editable ``.pth`` import hook for the P03 SDK (``cortrix``), so we mount both the
package ``src/`` and the P03 SDK source directory directly. This keeps the test
run independent of whether ``pip install -e`` is currently honoured. (If the
package is properly installed, these inserts are harmless duplicates.)
"""

from __future__ import annotations

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.join(_HERE, "src")
# P03 SDK source from the repository layout adjacent to cortrix-skills/.
_P03_SDK = os.path.abspath(os.path.join(_HERE, "..", "sdk", "python"))

for path in (_SRC, _P03_SDK):
    if os.path.isdir(path) and path not in sys.path:
        sys.path.insert(0, path)
