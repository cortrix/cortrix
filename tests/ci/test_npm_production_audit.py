#!/usr/bin/env python3
"""Unit tests for bounded npm production-audit classification."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "ci" / "npm_production_audit.py"
SPEC = importlib.util.spec_from_file_location("npm_production_audit", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def report(*, high: int = 0, critical: int = 0) -> str:
    return json.dumps(
        {
            "auditReportVersion": 2,
            "metadata": {
                "vulnerabilities": {
                    "info": 0,
                    "low": 0,
                    "moderate": 0,
                    "high": high,
                    "critical": critical,
                    "total": high + critical,
                }
            },
        }
    )


class SequenceRunner:
    def __init__(self, outcomes: list[object]) -> None:
        self.outcomes = outcomes
        self.calls = 0

    def __call__(
        self,
        *_args: object,
        **_kwargs: object,
    ) -> subprocess.CompletedProcess[str]:
        outcome = self.outcomes[self.calls]
        self.calls += 1
        if isinstance(outcome, BaseException):
            raise outcome
        assert isinstance(outcome, subprocess.CompletedProcess)
        return outcome


class NpmProductionAuditTests(unittest.TestCase):
    def run_with(
        self,
        outcomes: list[object],
        *,
        attempts: int = 3,
    ) -> tuple[int, SequenceRunner, dict[str, object]]:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        output_dir = Path(temporary.name)
        runner = SequenceRunner(outcomes)
        result = MODULE.run_audit(
            npm_command="npm",
            attempts=attempts,
            timeout_seconds=1,
            retry_delay_seconds=0,
            output_dir=output_dir,
            runner=runner,
            sleeper=lambda _seconds: None,
        )
        summary = json.loads(
            (output_dir / "summary.json").read_text(encoding="utf-8")
        )
        return result, runner, summary

    def test_clean_report_passes_without_retry(self) -> None:
        result, runner, summary = self.run_with(
            [subprocess.CompletedProcess([], 0, report(), "")]
        )
        self.assertEqual(result, 0)
        self.assertEqual(runner.calls, 1)
        self.assertEqual(summary["classification"], "clean")

    def test_high_vulnerability_fails_without_retry(self) -> None:
        result, runner, summary = self.run_with(
            [subprocess.CompletedProcess([], 1, report(high=1), "")]
        )
        self.assertEqual(result, 1)
        self.assertEqual(runner.calls, 1)
        self.assertEqual(summary["classification"], "vulnerabilities_detected")

    def test_service_errors_retry_then_recover(self) -> None:
        result, runner, summary = self.run_with(
            [
                subprocess.CompletedProcess(
                    [],
                    1,
                    '{"error":"503"}',
                    "service unavailable",
                ),
                subprocess.CompletedProcess([], 1, "", "socket hang up"),
                subprocess.CompletedProcess([], 0, report(), ""),
            ]
        )
        self.assertEqual(result, 0)
        self.assertEqual(runner.calls, 3)
        self.assertEqual(summary["attempts"], 3)

    def test_invalid_reports_exhaust_as_external_unavailable(self) -> None:
        result, runner, summary = self.run_with(
            [subprocess.CompletedProcess([], 1, "not-json", "bad response")] * 3
        )
        self.assertEqual(result, MODULE.EXIT_EXTERNAL_AUDIT_UNAVAILABLE)
        self.assertEqual(runner.calls, 3)
        self.assertEqual(summary["classification"], "external_audit_unavailable")

    def test_timeouts_exhaust_as_external_unavailable(self) -> None:
        timeout = subprocess.TimeoutExpired(
            ["npm", "audit"],
            timeout=1,
            output=b"partial-output",
            stderr=b"timed out",
        )
        result, runner, summary = self.run_with([timeout, timeout, timeout])
        self.assertEqual(result, MODULE.EXIT_EXTERNAL_AUDIT_UNAVAILABLE)
        self.assertEqual(runner.calls, 3)
        self.assertEqual(summary["classification"], "external_audit_unavailable")

    def test_command_errors_exhaust_as_external_unavailable(self) -> None:
        error = OSError("temporary command failure")
        result, runner, summary = self.run_with([error, error, error])
        self.assertEqual(result, MODULE.EXIT_EXTERNAL_AUDIT_UNAVAILABLE)
        self.assertEqual(runner.calls, 3)
        self.assertEqual(summary["classification"], "external_audit_unavailable")

    def test_clean_report_with_nonzero_exit_fails_closed(self) -> None:
        result, runner, summary = self.run_with(
            [subprocess.CompletedProcess([], 2, report(), "unexpected")]
        )
        self.assertEqual(result, 1)
        self.assertEqual(runner.calls, 1)
        self.assertEqual(summary["classification"], "valid_report_unexpected_exit")


if __name__ == "__main__":
    unittest.main()
