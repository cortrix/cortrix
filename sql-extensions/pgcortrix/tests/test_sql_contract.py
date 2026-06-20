"""S1 SQL contract self-check for pgcortrix--1.0.sql.

The build machine has no PostgreSQL, so we cannot run pg_regress / a live server
(that is deferred to D3.5). Instead this test parses the DDL text and asserts the
SQL *contract* defined by F14-pgcortrix.md §2.1: the 4 composite types with their
exact columns, the 5 main + 2 helper function signatures (name, ordered params
with types + defaults, return type, volatility/parallel markers). It catches the
kinds of drift a human review misses — a renamed column, a dropped DEFAULT, a
VOLATILE that should be STABLE, a SETOF that lost its row type.

Run: python3 -m unittest tests.test_sql_contract  (from the pgcortrix/ dir)
"""

import os
import re
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SQL_PATH = os.path.join(_HERE, "..", "sql", "pgcortrix--1.0.sql")


def _load_sql():
    with open(_SQL_PATH, "r", encoding="utf-8") as f:
        return f.read()


def _strip_line_comments(sql):
    """Drop `-- ...` line comments so column/keyword scans aren't fooled by prose
    in comments (e.g. the word VOLATILE inside an explanatory comment)."""
    out = []
    for line in sql.splitlines():
        idx = line.find("--")
        out.append(line if idx < 0 else line[:idx])
    return "\n".join(out)


class TestCompositeTypes(unittest.TestCase):
    """§2.1.1 — 4 composite return types, exact column sets."""

    # name -> ordered [(column, type)]. Types normalized to the spec spelling.
    EXPECTED = {
        "pgcortrix_search_result": [
            ("chunk_id", "TEXT"), ("content", "TEXT"), ("score", "FLOAT"),
            ("rerank_score", "FLOAT"), ("doc_id", "TEXT"),
            ("filename", "TEXT"), ("metadata", "JSONB"),
        ],
        "pgcortrix_doc_info": [
            ("doc_id", "TEXT"), ("filename", "TEXT"), ("status", "TEXT"),
            ("chunks", "INT"), ("created_at", "TIMESTAMPTZ"),
        ],
        "pgcortrix_memory_result": [
            ("memory_id", "TEXT"), ("content", "TEXT"), ("score", "FLOAT"),
            ("created_at", "TIMESTAMPTZ"), ("memory_type", "TEXT"),
            ("status", "TEXT"), ("final_score", "FLOAT"),
        ],
        "pgcortrix_interaction_info": [
            ("interaction_id", "TEXT"), ("user_id", "TEXT"),
            ("session_id", "TEXT"), ("role", "TEXT"), ("content", "TEXT"),
            ("created_at", "TIMESTAMPTZ"), ("metadata", "JSONB"),
        ],
    }

    @classmethod
    def setUpClass(cls):
        cls.sql = _strip_line_comments(_load_sql())

    def _type_body(self, type_name):
        m = re.search(
            r"CREATE\s+TYPE\s+" + re.escape(type_name) +
            r"\s+AS\s*\((?P<body>.*?)\)\s*;",
            self.sql, re.IGNORECASE | re.DOTALL)
        self.assertIsNotNone(m, f"CREATE TYPE {type_name} not found")
        return m.group("body")

    def _parse_columns(self, body):
        cols = []
        for raw in body.split(","):
            raw = raw.strip()
            if not raw:
                continue
            parts = raw.split()
            cols.append((parts[0], parts[1].upper()))
        return cols

    def test_all_four_types_present(self):
        for name in self.EXPECTED:
            self._type_body(name)

    def test_columns_match_spec(self):
        for name, expected in self.EXPECTED.items():
            with self.subTest(type=name):
                got = self._parse_columns(self._type_body(name))
                self.assertEqual(
                    got, expected,
                    f"{name} columns drifted from F14 §2.1.1 SoT")


class TestFunctionSignatures(unittest.TestCase):
    """§2.1.2 / §2.1.3 — 5 main + 2 helper function signatures."""

    @classmethod
    def setUpClass(cls):
        cls.sql = _strip_line_comments(_load_sql())

    def _fn(self, name):
        """Return (params_text, tail_text) for CREATE FUNCTION <name>(...).
        tail_text spans from after the param list up to the body delimiter."""
        m = re.search(
            r"CREATE\s+FUNCTION\s+" + re.escape(name) +
            r"\s*\((?P<params>.*?)\)\s*(?P<tail>.*?)\bAS\s*\$\$",
            self.sql, re.IGNORECASE | re.DOTALL)
        self.assertIsNotNone(m, f"CREATE FUNCTION {name} not found")
        return m.group("params"), m.group("tail")

    def _param_names(self, params_text):
        names = []
        # split top-level commas only (no nested parens in these signatures)
        for raw in params_text.split(","):
            raw = raw.strip()
            if raw:
                names.append(raw.split()[0])
        return names

    def test_all_eight_functions_present(self):
        for name in ("pgcortrix_search", "pgcortrix_upload",
                     "pgcortrix_list_documents", "pgcortrix_batch_submit",
                     "pgcortrix_memory_search", "pgcortrix_list_interactions",
                     "pgcortrix_configure", "pgcortrix_status"):
            self._fn(name)

    def test_search_signature(self):
        params, tail = self._fn("pgcortrix_search")
        self.assertEqual(
            self._param_names(params),
            ["namespace", "query", "top_k", "filter", "rerank"])
        self.assertIn("DEFAULT 10", params)
        self.assertIn("DEFAULT NULL", params)
        self.assertIn("DEFAULT TRUE", params)
        self.assertRegex(tail, r"RETURNS\s+SETOF\s+pgcortrix_search_result")
        # L3.4: STABLE + PARALLEL SAFE for LATERAL JOIN pushdown.
        self.assertRegex(tail, r"\bSTABLE\b")
        self.assertRegex(tail, r"PARALLEL\s+SAFE")

    def test_upload_is_volatile_returns_text(self):
        params, tail = self._fn("pgcortrix_upload")
        self.assertEqual(self._param_names(params), ["namespace", "file_path"])
        self.assertRegex(tail, r"RETURNS\s+TEXT")
        self.assertRegex(tail, r"\bVOLATILE\b")  # write path

    def test_list_documents_signature(self):
        params, tail = self._fn("pgcortrix_list_documents")
        self.assertEqual(self._param_names(params), ["namespace"])
        self.assertRegex(tail, r"RETURNS\s+SETOF\s+pgcortrix_doc_info")
        self.assertRegex(tail, r"\bSTABLE\b")

    def test_memory_search_forces_user_id(self):
        params, tail = self._fn("pgcortrix_memory_search")
        names = self._param_names(params)
        self.assertEqual(names, ["namespace", "query", "user_id", "top_k"])
        # MEM05: user_id must be positional + mandatory (no DEFAULT on it).
        self.assertNotRegex(params, r"user_id\s+TEXT\s+DEFAULT")
        self.assertRegex(tail, r"RETURNS\s+SETOF\s+pgcortrix_memory_result")

    def test_list_interactions_signature(self):
        params, tail = self._fn("pgcortrix_list_interactions")
        names = self._param_names(params)
        # v1.0.2 5-param shape: namespace, user_id, filter, limit_n, offset_n.
        self.assertEqual(
            names, ["namespace", "user_id", "filter", "limit_n", "offset_n"])
        # user_id mandatory (MEM05 D4): no DEFAULT on it; filter is optional.
        self.assertNotRegex(params, r"user_id\s+TEXT\s+DEFAULT")
        self.assertRegex(params, r"filter\s+JSONB\s+DEFAULT\s+NULL")
        self.assertIn("DEFAULT 50", params)
        self.assertIn("DEFAULT 0", params)
        self.assertRegex(tail, r"RETURNS\s+SETOF\s+pgcortrix_interaction_info")

    def test_configure_signature(self):
        params, tail = self._fn("pgcortrix_configure")
        self.assertEqual(self._param_names(params), ["api_key"])
        self.assertRegex(tail, r"RETURNS\s+VOID")
        self.assertRegex(tail, r"\bVOLATILE\b")

    def test_batch_submit_signature(self):
        params, tail = self._fn("pgcortrix_batch_submit")
        # TD-F42-BULK-5-rev-3: namespace + documents JSONB + on_duplicate.
        self.assertEqual(
            self._param_names(params), ["namespace", "documents", "on_duplicate"])
        self.assertRegex(params, r"documents\s+JSONB")
        self.assertRegex(params, r"on_duplicate\s+TEXT\s+DEFAULT\s+'skip'")
        # Partial-success envelope returned as JSONB (like pgcortrix_status).
        self.assertRegex(tail, r"RETURNS\s+JSONB")
        # Write path.
        self.assertRegex(tail, r"\bVOLATILE\b")

    def test_status_returns_jsonb(self):
        params, tail = self._fn("pgcortrix_status")
        self.assertEqual(params.strip(), "")  # no params
        self.assertRegex(tail, r"RETURNS\s+JSONB")
        self.assertRegex(tail, r"\bSTABLE\b")

    def test_all_functions_are_plpython3u(self):
        for name in ("pgcortrix_search", "pgcortrix_upload",
                     "pgcortrix_list_documents", "pgcortrix_batch_submit",
                     "pgcortrix_memory_search", "pgcortrix_list_interactions",
                     "pgcortrix_configure", "pgcortrix_status"):
            _, tail = self._fn(name)
            self.assertRegex(
                tail, r"LANGUAGE\s+plpython3u",
                f"{name} must be LANGUAGE plpython3u (F14 V1 route)")


class TestExtensionFraming(unittest.TestCase):
    """Guards that make the script safe to ship as an extension script."""

    @classmethod
    def setUpClass(cls):
        cls.raw = _load_sql()

    def test_has_create_extension_guard(self):
        # Standard PG idiom: prevent loading the script outside CREATE EXTENSION.
        self.assertIn(r'\quit', self.raw)
        self.assertRegex(self.raw, r'CREATE\s+EXTENSION\s+pgcortrix')

    def test_registers_four_gucs(self):
        sql = _strip_line_comments(self.raw)
        for guc in ("pgcortrix.endpoint", "pgcortrix.api_key",
                    "pgcortrix.timeout_ms", "pgcortrix.retry_max"):
            self.assertIn(guc, sql, f"GUC {guc} not registered (§2.2)")


if __name__ == "__main__":
    unittest.main()
