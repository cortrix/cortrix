#!/usr/bin/env python3
"""Unit tests for the public source isolation scanner."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "ci" / "check_open_core_isolation.py"
SPEC = importlib.util.spec_from_file_location("check_open_core_isolation", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class OpenCoreIsolationTests(unittest.TestCase):
    def make_tree(self, *, violation: str = "") -> Path:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / "src").mkdir()
        (root / "include").mkdir()
        for index in range(101):
            content = "namespace cortrix {}\n" if index == 0 else "int value = 0;\n"
            if index == 100 and violation:
                content = violation + "\n"
            (root / "src" / f"source_{index}.cpp").write_text(
                content,
                encoding="utf-8",
            )
        return root

    def test_clean_tree_passes(self) -> None:
        sources, violations = MODULE.scan(self.make_tree())
        self.assertEqual(len(sources), 101)
        self.assertEqual(violations, [])

    def test_enterprise_reference_fails(self) -> None:
        _, violations = MODULE.scan(
            self.make_tree(violation="namespace enterprise {}")
        )
        self.assertEqual(len(violations), 1)
        self.assertIn("enterprise namespace", violations[0])

    def test_truncated_source_universe_fails_closed(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        (root / "src").mkdir()
        (root / "include").mkdir()
        (root / "src" / "only.cpp").write_text(
            "namespace cortrix {}\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(RuntimeError, "expected more than 100"):
            MODULE.scan(root)


if __name__ == "__main__":
    unittest.main()
