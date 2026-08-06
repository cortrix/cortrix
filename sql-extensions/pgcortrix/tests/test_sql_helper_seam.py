"""Seam check: the SQL function bodies call the helper with the exact arguments
the helper methods accept.

The SQL DDL (sql/pgcortrix--1.0.sql) and the Python helper (python/
pgcortrix_helper.py) live in separate files but are coupled: each plpython3u
body does `client.<method>(<args>)`. Off a live PG nothing forces them to agree,
so a rename on one side would only surface at integration. This test parses both and
asserts every `client.<m>(...)` call in the SQL matches a PgcortrixClient method
with the same positional arity.

Run: python3 -m unittest discover -s tests
"""

import inspect
import os
import re
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "python"))

import pgcortrix_helper as h  # noqa: E402

_SQL_PATH = os.path.join(_HERE, "..", "sql", "pgcortrix--1.0.sql")


def _sql_client_calls():
    """Yield (method, arg_count) for each client.<method>(...) call in the SQL."""
    with open(_SQL_PATH, encoding="utf-8") as f:
        sql = f.read()
    for m in re.finditer(r"client\.([a-z_]+)\(([^)]*)\)", sql):
        method = m.group(1)
        args = [a.strip() for a in m.group(2).split(",") if a.strip()]
        yield method, len(args)


class TestSqlHelperSeam(unittest.TestCase):
    def test_every_sql_call_matches_a_helper_method(self):
        calls = list(_sql_client_calls())
        # Sanity: each exposed SQL function should make exactly one client call.
        self.assertEqual(len(calls), 8,
                         "expected 8 client.* calls")
        for method, argc in calls:
            with self.subTest(method=method):
                self.assertTrue(
                    hasattr(h.PgcortrixClient, method),
                    f"helper has no method {method!r} (SQL/Python drift)")
                fn = getattr(h.PgcortrixClient, method)
                sig = inspect.signature(fn)
                # positional params excluding `self`
                positional = [
                    p for p in sig.parameters.values()
                    if p.name != "self"
                    and p.kind in (p.POSITIONAL_OR_KEYWORD, p.POSITIONAL_ONLY)]
                self.assertEqual(
                    argc, len(positional),
                    f"{method}: SQL passes {argc} args but helper takes "
                    f"{len(positional)}")

    def test_all_eight_methods_called(self):
        called = {m for m, _ in _sql_client_calls()}
        self.assertEqual(
            called,
            {"search", "upload", "list_documents", "batch_submit",
             "memory_search",
             "list_interactions", "configure", "status"})


if __name__ == "__main__":
    unittest.main()
