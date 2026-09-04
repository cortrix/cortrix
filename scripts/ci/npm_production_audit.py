#!/usr/bin/env python3
"""Run the production npm audit with bounded retries and explicit classification."""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Sequence


EXIT_VULNERABILITY_OR_TOOL_ERROR = 1
EXIT_EXTERNAL_AUDIT_UNAVAILABLE = 75


def as_text(value: str | bytes | None) -> str:
    """Normalize subprocess output across Python timeout implementations."""
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value


def parse_report(stdout: str) -> tuple[int, int] | None:
    """Return high and critical counts only for a structurally valid audit report."""
    try:
        payload = json.loads(stdout)
    except json.JSONDecodeError:
        return None
    if not isinstance(payload, dict):
        return None
    metadata = payload.get("metadata")
    if not isinstance(metadata, dict):
        return None
    vulnerabilities = metadata.get("vulnerabilities")
    if not isinstance(vulnerabilities, dict):
        return None
    counts = []
    for severity in ("high", "critical"):
        value = vulnerabilities.get(severity)
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            return None
        counts.append(value)
    return counts[0], counts[1]


def write_summary(
    output_dir: Path,
    *,
    classification: str,
    attempts: int,
    exit_code: int,
    high: int | None = None,
    critical: int | None = None,
) -> None:
    """Write a machine-readable terminal result without environment secrets."""
    summary = {
        "classification": classification,
        "attempts": attempts,
        "exit_code": exit_code,
        "high": high,
        "critical": critical,
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def run_audit(
    *,
    npm_command: str,
    attempts: int,
    timeout_seconds: float,
    retry_delay_seconds: float,
    output_dir: Path,
    runner: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
    sleeper: Callable[[float], None] = time.sleep,
) -> int:
    """Run npm audit and distinguish vulnerability findings from service failures."""
    output_dir.mkdir(parents=True, exist_ok=True)
    command: Sequence[str] = (
        npm_command,
        "audit",
        "--omit=dev",
        "--audit-level=high",
        "--json",
    )

    for attempt in range(1, attempts + 1):
        stdout = ""
        stderr = ""
        return_code: int | None = None
        infrastructure_error = ""
        try:
            completed = runner(
                command,
                capture_output=True,
                text=True,
                timeout=timeout_seconds,
                check=False,
            )
            stdout = completed.stdout
            stderr = completed.stderr
            return_code = completed.returncode
        except subprocess.TimeoutExpired as error:
            stdout = as_text(error.stdout)
            stderr = as_text(error.stderr)
            infrastructure_error = f"timeout_after_{timeout_seconds:g}_seconds"
        except OSError as error:
            infrastructure_error = f"command_error:{error.__class__.__name__}:{error}"

        report_path = output_dir / f"attempt-{attempt}.stdout.json"
        error_path = output_dir / f"attempt-{attempt}.stderr.log"
        report_path.write_text(stdout, encoding="utf-8")
        error_path.write_text(stderr, encoding="utf-8")

        counts = parse_report(stdout)
        if counts is not None:
            high, critical = counts
            shutil.copyfile(report_path, output_dir / "npm-audit.json")
            if high > 0 or critical > 0:
                print(
                    "npm_audit_vulnerabilities_detected "
                    f"high={high} critical={critical} attempt={attempt}",
                    file=sys.stderr,
                )
                write_summary(
                    output_dir,
                    classification="vulnerabilities_detected",
                    attempts=attempt,
                    exit_code=EXIT_VULNERABILITY_OR_TOOL_ERROR,
                    high=high,
                    critical=critical,
                )
                return EXIT_VULNERABILITY_OR_TOOL_ERROR
            if return_code != 0:
                print(
                    "npm_audit_valid_report_unexpected_exit "
                    f"status={return_code} attempt={attempt}",
                    file=sys.stderr,
                )
                write_summary(
                    output_dir,
                    classification="valid_report_unexpected_exit",
                    attempts=attempt,
                    exit_code=EXIT_VULNERABILITY_OR_TOOL_ERROR,
                    high=high,
                    critical=critical,
                )
                return EXIT_VULNERABILITY_OR_TOOL_ERROR

            print(f"npm_audit_ok high=0 critical=0 attempt={attempt}")
            write_summary(
                output_dir,
                classification="clean",
                attempts=attempt,
                exit_code=0,
                high=high,
                critical=critical,
            )
            return 0

        detail = infrastructure_error or f"invalid_or_missing_report status={return_code}"
        print(
            f"external_audit_unavailable attempt={attempt}/{attempts} detail={detail}",
            file=sys.stderr,
        )
        if stderr:
            print(stderr.rstrip(), file=sys.stderr)
        if attempt < attempts:
            sleeper(retry_delay_seconds)

    write_summary(
        output_dir,
        classification="external_audit_unavailable",
        attempts=attempts,
        exit_code=EXIT_EXTERNAL_AUDIT_UNAVAILABLE,
    )
    print(
        f"external_audit_unavailable attempts={attempts}",
        file=sys.stderr,
    )
    return EXIT_EXTERNAL_AUDIT_UNAVAILABLE


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--npm-command", default="npm")
    parser.add_argument("--attempts", type=int, default=3)
    parser.add_argument("--timeout-seconds", type=float, default=90)
    parser.add_argument("--retry-delay-seconds", type=float, default=10)
    parser.add_argument("--output-dir", type=Path, default=Path("artifacts/npm-audit"))
    args = parser.parse_args()

    if args.attempts < 1:
        parser.error("--attempts must be at least 1")
    if args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    if args.retry_delay_seconds < 0:
        parser.error("--retry-delay-seconds must not be negative")

    return run_audit(
        npm_command=args.npm_command,
        attempts=args.attempts,
        timeout_seconds=args.timeout_seconds,
        retry_delay_seconds=args.retry_delay_seconds,
        output_dir=args.output_dir,
    )


if __name__ == "__main__":
    raise SystemExit(main())
