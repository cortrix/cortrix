#!/usr/bin/env python3
"""Static contracts for early release failure and isolation job topology."""

from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "release-gate.yml"


def job_block(workflow: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(name)}:\n(.*?)(?=^  [A-Za-z0-9_]+:\n|\Z)",
        workflow,
    )
    if match is None:
        raise AssertionError(f"missing workflow job: {name}")
    return match.group(1)


class ReleaseGateTopologyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = WORKFLOW.read_text(encoding="utf-8")

    def test_preflight_blocks_stage2(self) -> None:
        preflight = job_block(self.workflow, "release_preflight")
        stage2 = job_block(self.workflow, "stage2_full")
        self.assertIn("needs: identity", preflight)
        self.assertIn("needs: release_preflight", stage2)
        self.assertIn("npm_production_audit.py", preflight)
        self.assertNotIn("continue-on-error", preflight)

    def test_isolation_runs_without_waiting_for_stage2(self) -> None:
        isolation = job_block(self.workflow, "isolation_and_image")
        self.assertIn("needs: [identity, release_preflight]", isolation)
        self.assertNotIn("needs.stage2_full", isolation)
        self.assertIn("needs.release_preflight.result == 'success'", isolation)
        self.assertNotIn("continue-on-error", isolation)

    def test_audit_has_one_authoritative_workflow_entry(self) -> None:
        self.assertEqual(
            self.workflow.count("python3 ../scripts/ci/npm_production_audit.py"),
            1,
        )
        self.assertNotIn("npm audit --omit=dev", self.workflow)
        npm_ci_lines = [
            line.strip()
            for line in self.workflow.splitlines()
            if "npm ci " in line
        ]
        self.assertTrue(npm_ci_lines)
        self.assertTrue(all("--no-audit" in line for line in npm_ci_lines))
        isolation = job_block(self.workflow, "isolation_and_image")
        self.assertIn("npm ci --ignore-scripts --no-audit", isolation)

    def test_release_evidence_hard_gates_remain(self) -> None:
        isolation = job_block(self.workflow, "isolation_and_image")
        for contract in (
            "Build release image",
            "Generate SPDX and CycloneDX SBOMs",
            "Runtime QuickStart and network-none proof",
            "External-address negative controls",
            "Trivy image scan (hard gate)",
            "Validate release evidence completeness",
            "if-no-files-found: error",
        ):
            self.assertIn(contract, isolation)

    def test_both_validation_scopes_remain_available(self) -> None:
        self.assertIn("- full", self.workflow)
        self.assertIn("- isolation_and_image", self.workflow)


if __name__ == "__main__":
    unittest.main()
