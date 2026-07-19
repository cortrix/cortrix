#!/usr/bin/env python3
"""Run the fail-closed Cortrix SupportOps first-value contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


RUNNER_VERSION = "1.1.0"
TERMINAL_SUCCESS = {"completed", "ready"}
TERMINAL_FAILURE = {"failed", "cancelled"}


class DemoFailure(RuntimeError):
    """A contract failure that must make the whole run fail."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def git_value(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def core_identity(repo: Path) -> dict[str, str]:
    if not repo.is_dir():
        raise DemoFailure(f"Core repository does not exist: {repo}")
    try:
        commit = git_value(repo, "rev-parse", "HEAD")
        tree = git_value(repo, "rev-parse", "HEAD^{tree}")
        dirty = git_value(repo, "status", "--porcelain=v1", "--untracked-files=all")
    except (OSError, subprocess.CalledProcessError) as exc:
        raise DemoFailure(f"Cannot resolve Core Git identity: {exc}") from exc
    if dirty:
        raise DemoFailure("Core repository is dirty; refusing a release-grade run")
    return {"commit": commit, "tree": tree, "dirty": "false"}


def cmake_cache(build_dir: Path, core_repo: Path) -> dict[str, str]:
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        raise DemoFailure(f"CMake cache does not exist: {cache_path}")
    values: dict[str, str] = {}
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("//") or line.startswith("#") or "=" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        key = key_and_type.split(":", 1)[0]
        values[key] = value
    source = values.get("CMAKE_HOME_DIRECTORY")
    if not source or Path(source).expanduser().resolve() != core_repo:
        raise DemoFailure("CMake build directory is not configured from the declared Core repo")
    if values.get("CORTRIX_USE_ONNX") != "OFF":
        raise DemoFailure("CMake build must set CORTRIX_USE_ONNX=OFF for this contract")
    if values.get("CMAKE_BUILD_TYPE") != "Release":
        raise DemoFailure("CMake build must set CMAKE_BUILD_TYPE=Release for this contract")
    return {
        "cache_path": str(cache_path.resolve()),
        "cache_sha256": sha256_file(cache_path),
        "cmake_home_directory": source,
        "cortrix_use_onnx": values["CORTRIX_USE_ONNX"],
        "cmake_build_type": values.get("CMAKE_BUILD_TYPE", ""),
    }


def server_attestation(core_repo: Path, build_dir: Path) -> dict[str, str]:
    build_dir = build_dir.expanduser().resolve()
    cache = cmake_cache(build_dir, core_repo)
    binary = build_dir / "cortrix-server"
    if not binary.is_file():
        raise DemoFailure(f"Cortrix server binary does not exist: {binary}")
    return {
        **cache,
        "binary_path": str(binary),
        "binary_sha256": sha256_file(binary),
        "source_binding": "cmake-source-and-runner-launched-binary",
    }


def ensure_port_available(host: str, port: int) -> None:
    if host not in {"127.0.0.1", "localhost"}:
        raise DemoFailure("The first-value runner only starts a loopback server")
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.settimeout(0.5)
        if probe.connect_ex((host, port)) == 0:
            raise DemoFailure(f"TCP port is already in use: {host}:{port}")


def write_runtime_config(path: Path, data_dir: Path, host: str, port: int) -> None:
    value = f'''server:
  host: "{host}"
  port: {port}
  thread_count: 4

auth:
  enabled: false

log:
  level: "info"
  format: "text"
  output: "stdout"

namespace:
  data_dir: "{data_dir}"
  max_active: 10
  idle_timeout_s: 300

embedding:
  model_path: ""
  tokenizer_path: ""
  dimension: 1024
  max_seq_length: 512
  execution_provider: "cpu"

reranker:
  model_dir: ""
  execution_provider: "cpu"

query_complexity:
  model_dir: ""

spc:
  worker_count: 2
  chunk_size: 512
  chunk_overlap: 50
  embedding_batch_size: 1
  onnx_intra_threads: 4
  onnx_inter_threads: 1
  parser_timeout_s: 120
  ocr_timeout_s: 3600

watch_dir:
  data_dir: ""
  namespace_name: "local"
  watch_enabled: false

memory:
  default_ttl_seconds: 0
  inject_recent_turns: 5
  inject_max_tokens: 2000

rag_fusion:
  default_enabled: false
  default_variant_count: 3
  default_rrf_k: 60
  default_timeout_ms: 5000
'''
    path.write_text(value, encoding="utf-8")


def wait_for_server(process: subprocess.Popen[bytes], base_url: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    health_url = base_url.rstrip("/") + "/api/v1/health"
    while time.monotonic() < deadline:
        code = process.poll()
        if code is not None:
            raise DemoFailure(f"Cortrix server exited during startup with code {code}")
        try:
            with urllib.request.urlopen(health_url, timeout=1.0) as response:
                if response.status == 200:
                    return
        except (urllib.error.URLError, TimeoutError, OSError):
            time.sleep(0.25)
    raise DemoFailure(f"Cortrix server did not become healthy within {timeout}s")


class Client:
    def __init__(self, base_url: str, evidence_dir: Path, timeout: float) -> None:
        self.api = base_url.rstrip("/") + "/api/v1"
        self.evidence_dir = evidence_dir
        self.timeout = timeout
        self.sequence = 0

    def request(
        self,
        method: str,
        path: str,
        *,
        payload: Any | None = None,
        headers: dict[str, str] | None = None,
        expected_status: set[int],
        label: str,
    ) -> tuple[int, Any]:
        self.sequence += 1
        body = None
        request_headers = dict(headers or {})
        if payload is not None:
            body = json.dumps(payload).encode("utf-8")
            request_headers["Content-Type"] = "application/json"
        request = urllib.request.Request(
            self.api + path,
            data=body,
            headers=request_headers,
            method=method,
        )
        status = 0
        raw = b""
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                status = response.status
                raw = response.read()
        except urllib.error.HTTPError as exc:
            status = exc.code
            raw = exc.read()
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            raise DemoFailure(f"{label}: request failed: {exc}") from exc

        parsed: Any = None
        if raw:
            try:
                parsed = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                raise DemoFailure(f"{label}: response is not valid JSON") from exc
        write_json(
            self.evidence_dir / "responses" / f"{self.sequence:02d}-{label}.json",
            {"method": method, "path": path, "status": status, "body": parsed},
        )
        if status not in expected_status:
            raise DemoFailure(f"{label}: HTTP {status}, expected {sorted(expected_status)}")
        return status, parsed


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise DemoFailure(f"Cannot load versioned JSON file {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise DemoFailure(f"Expected a JSON object in {path}")
    return value


def run_contract(args: argparse.Namespace) -> tuple[dict[str, Any], Path]:
    package = Path(__file__).resolve().parent
    manifest_path = package / "manifest.json"
    manifest = load_json(manifest_path)
    if manifest.get("runner_contract_version") != RUNNER_VERSION:
        raise DemoFailure(
            "Manifest runner_contract_version does not match this runner"
        )
    query_path = package / str(manifest["query"])
    expected_path = package / str(manifest["expected"])
    query_spec = load_json(query_path)
    expected = load_json(expected_path)
    documents = [package / str(item) for item in manifest["documents"]]
    for path in [manifest_path, query_path, expected_path, *documents]:
        if not path.is_file():
            raise DemoFailure(f"Required fixture file is missing: {path}")

    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + secrets.token_hex(3)
    namespace = "fv_supportops_" + run_id.lower().replace("-", "_")
    evidence_dir = Path(args.output_root).expanduser().resolve() / run_id
    evidence_dir.mkdir(parents=True, exist_ok=False)
    core_repo = Path(args.core_repo).expanduser().resolve()
    identity = core_identity(core_repo)
    if args.expected_core_commit and identity["commit"] != args.expected_core_commit:
        raise DemoFailure(
            f"Core commit mismatch: {identity['commit']} != {args.expected_core_commit}"
        )

    build = server_attestation(core_repo, Path(args.build_dir))
    base_url = f"http://{args.host}:{args.port}"
    runtime_config = evidence_dir / "runtime-config.yaml"
    runtime_data = evidence_dir / "runtime-data"
    runtime_data.mkdir()
    write_runtime_config(runtime_config, runtime_data, args.host, args.port)
    file_hashes = {
        str(path.relative_to(package)): sha256_file(path)
        for path in [manifest_path, query_path, expected_path, *documents]
    }
    client = Client(base_url, evidence_dir, args.http_timeout)
    summary: dict[str, Any] = {
        "contract": "FAIL",
        "runner_version": RUNNER_VERSION,
        "run_id": run_id,
        "namespace": namespace,
        "base_url": base_url,
        "core": identity,
        "runtime": {
            **build,
            "config_path": str(runtime_config),
            "config_sha256": sha256_file(runtime_config),
            "profile_id": "runner-owned-onnx-off-no-llm-rerank-false",
            "server_working_directory": str(core_repo),
            "server_pid": None,
            "server_stopped": False,
        },
        "fixture_version": manifest.get("version"),
        "file_hashes": file_hashes,
        "expected_file_loaded": True,
        "expected_assertions": {},
        "tasks": [],
        "trace_assertion": "NOT_RUN",
        "cleanup": {"attempted": False, "remaining_matching_namespaces": None},
        "errors": [],
    }
    created = False
    server_process: subprocess.Popen[bytes] | None = None

    try:
        ensure_port_available(args.host, args.port)
        server_process = subprocess.Popen(
            [build["binary_path"], "--config", str(runtime_config)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            cwd=str(core_repo),
            start_new_session=True,
        )
        summary["runtime"]["server_pid"] = server_process.pid
        wait_for_server(server_process, base_url, args.server_start_timeout)
        _, health = client.request(
            "GET", "/health", expected_status={200}, label="health"
        )
        if not isinstance(health, dict) or health.get("status") != "healthy":
            raise DemoFailure("Health response is not healthy")
        if health.get("llm_enabled") is not False:
            raise DemoFailure("Health response does not attest llm_enabled=false")
        summary["health"] = health

        client.request(
            "GET",
            "/namespaces/" + urllib.parse.quote(namespace),
            expected_status={404},
            label="namespace-before",
        )
        summary["clean_before_matching_namespaces"] = 0

        created = True
        client.request(
            "POST",
            "/namespaces",
            payload={"name": namespace},
            expected_status={201},
            label="create-namespace",
        )
        for document in documents:
            relative = str(document.relative_to(package))
            _, accepted = client.request(
                "POST",
                "/documents",
                payload={
                    "namespace": namespace,
                    "filename": relative,
                    "content": document.read_text(encoding="utf-8"),
                    "metadata": {
                        "demo_id": manifest["demo_id"],
                        "fixture_version": manifest["version"],
                        "fixture_sha256": file_hashes[relative],
                    },
                },
                expected_status={202},
                label="upload-" + document.stem,
            )
            if not isinstance(accepted, dict) or not isinstance(accepted.get("task_id"), str):
                raise DemoFailure(f"Upload did not return a task_id for {relative}")
            task_id = accepted["task_id"]
            deadline = time.monotonic() + args.task_timeout
            final_task: dict[str, Any] | None = None
            while time.monotonic() < deadline:
                _, progress = client.request(
                    "GET",
                    f"/documents/tasks/{urllib.parse.quote(task_id)}/progress",
                    expected_status={200},
                    label="task-" + document.stem,
                )
                if not isinstance(progress, dict) or not isinstance(progress.get("status"), str):
                    raise DemoFailure(f"Task response has an unexpected shape for {relative}")
                status = progress["status"].lower()
                if status in TERMINAL_SUCCESS:
                    if progress.get("error_code") or progress.get("error_msg"):
                        raise DemoFailure(f"Task completed with an error for {relative}")
                    final_task = progress
                    break
                if status in TERMINAL_FAILURE:
                    raise DemoFailure(f"Task {task_id} ended with status {status}")
                time.sleep(args.poll_interval)
            if final_task is None:
                raise DemoFailure(f"Task {task_id} timed out after {args.task_timeout}s")
            summary["tasks"].append(
                {
                    "source": relative,
                    "task_id": task_id,
                    "status": final_task["status"],
                    "document_id": final_task.get("document_id") or final_task.get("doc_id"),
                }
            )

        request_body = dict(query_spec.get("request") or {})
        request_body["namespaces"] = [namespace]
        request_body["top_k"] = 5
        request_body["rerank"] = False
        request_body["include_sources"] = True
        trace_id = "fv-trace-" + run_id
        session_id = "fv-session-" + run_id
        _, query = client.request(
            "POST",
            "/query?explain=true",
            payload=request_body,
            headers={
                "X-Session-Id": session_id,
                "X-Trace-Id": trace_id,
                "X-Agent-Id": "cortrix-first-value-demo",
            },
            expected_status={200},
            label="query",
        )
        if not isinstance(query, dict) or not isinstance(query.get("results"), list):
            raise DemoFailure("Query response has an unexpected shape")
        results = [item for item in query["results"] if isinstance(item, dict)]
        if not results:
            raise DemoFailure("Query returned no results")

        normalized_results: list[dict[str, Any]] = []
        for position, item in enumerate(results):
            metadata = item.get("metadata") if isinstance(item.get("metadata"), dict) else {}
            source = item.get("source_path") or metadata.get("source_path")
            parent = (
                item.get("parent_id")
                or item.get("doc_id")
                or metadata.get("doc_id")
                or metadata.get("source_doc_id")
            )
            normalized_results.append(
                {
                    "position": position,
                    "source": source if isinstance(source, str) else "",
                    "content": str(item.get("content") or item.get("chunk_text") or ""),
                    "has_provenance": bool(item.get("child_id") and parent),
                }
            )
        evidence_groups = expected.get("required_evidence_groups")
        if not isinstance(evidence_groups, list) or not evidence_groups:
            raise DemoFailure("Expected file has no required_evidence_groups")
        group_matches: list[dict[str, Any]] = []
        for group in evidence_groups:
            if not isinstance(group, dict) or not isinstance(group.get("id"), str):
                raise DemoFailure("Evidence group has an unexpected shape")
            sources_any = [str(value) for value in group.get("sources_any", [])]
            snippets_any = [str(value) for value in group.get("snippets_any", [])]
            matches = [
                item
                for item in normalized_results
                if item["has_provenance"]
                and item["source"] in sources_any
                and any(
                    snippet.casefold() in item["content"].casefold()
                    for snippet in snippets_any
                )
            ]
            if not matches:
                raise DemoFailure(
                    f"Required evidence group {group['id']} did not match one result "
                    "with source, snippet, and provenance"
                )
            group_matches.append(
                {
                    "id": group["id"],
                    "matched_positions": [item["position"] for item in matches],
                    "matched_sources": sorted({item["source"] for item in matches}),
                }
            )
        forbidden = [str(value) for value in expected.get("must_not_claim", [])]
        content_folded = "\n".join(item["content"] for item in normalized_results).casefold()
        forbidden_match = [value for value in forbidden if value.casefold() in content_folded]
        if forbidden_match:
            raise DemoFailure(f"Forbidden claims appeared in query results: {forbidden_match}")
        summary["expected_assertions"] = {
            "required_evidence_groups": evidence_groups,
            "group_matches": group_matches,
            "must_not_claim": forbidden,
            "forbidden_matches": forbidden_match,
        }

        trace_query = urllib.parse.urlencode({"namespace": namespace, "limit": 20})
        _, trace = client.request(
            "GET",
            f"/traces/{urllib.parse.quote(session_id)}?{trace_query}",
            expected_status={200},
            label="trace",
        )
        traces = trace.get("traces") if isinstance(trace, dict) else None
        trace_matches = [
            item
            for item in (traces or [])
            if isinstance(item, dict)
            and item.get("trace_id") == trace_id
            and item.get("status") == "success"
        ]
        if not trace_matches:
            raise DemoFailure("Trace timeline did not contain the successful query trace_id")
        summary["trace_assertion"] = "PASS"
        summary["trace_id"] = trace_id
        summary["query_result_count"] = len(results)
        summary["contract"] = "PASS"
    except Exception as exc:  # The summary must survive every fail-closed path.
        summary["errors"].append(str(exc))
    finally:
        summary["cleanup"]["attempted"] = True
        cleanup_errors: list[str] = []
        if created:
            try:
                client.request(
                    "DELETE",
                    "/namespaces/" + urllib.parse.quote(namespace),
                    expected_status={204},
                    label="delete-namespace",
                )
            except Exception as exc:
                cleanup_errors.append(str(exc))
        try:
            after_status, after_namespace = client.request(
                "GET",
                "/namespaces/" + urllib.parse.quote(namespace),
                expected_status={200, 404},
                label="namespace-after",
            )
            cleanup_state = "not-found"
            if after_status == 200:
                if not isinstance(after_namespace, dict):
                    raise DemoFailure("Deleted namespace response has an unexpected shape")
                if (
                    after_namespace.get("status") != "deleted"
                    or after_namespace.get("doc_count") != 0
                    or after_namespace.get("block_count") != 0
                ):
                    raise DemoFailure(
                        "Deleted namespace still has an active state, documents, or blocks"
                    )
                cleanup_state = "tombstoned-empty"
            summary["cleanup"]["remaining_matching_namespaces"] = 0
            summary["cleanup"]["remaining"] = []
            summary["cleanup"]["verified_state"] = cleanup_state
        except Exception as exc:
            cleanup_errors.append(str(exc))
        if server_process is not None:
            try:
                if server_process.poll() is None:
                    server_process.send_signal(signal.SIGINT)
                server_process.wait(timeout=10)
                summary["runtime"]["server_exit_code"] = server_process.returncode
                summary["runtime"]["server_stopped"] = True
            except Exception as exc:
                server_process.kill()
                server_process.wait(timeout=5)
                cleanup_errors.append(f"Server shutdown required kill: {exc}")
        if cleanup_errors:
            summary["errors"].extend(cleanup_errors)
            summary["contract"] = "FAIL"

    write_json(evidence_dir / "machine-summary.json", summary)
    return summary, evidence_dir


def parse_args() -> argparse.Namespace:
    default_output = Path(tempfile.gettempdir()) / "cortrix-first-value-demo"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--core-repo", default=".")
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--expected-core-commit")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18420)
    parser.add_argument("--output-root", default=str(default_output))
    parser.add_argument("--http-timeout", type=float, default=20.0)
    parser.add_argument("--task-timeout", type=float, default=120.0)
    parser.add_argument("--server-start-timeout", type=float, default=30.0)
    parser.add_argument("--poll-interval", type=float, default=0.5)
    return parser.parse_args()


def main() -> int:
    try:
        summary, evidence_dir = run_contract(parse_args())
    except Exception as exc:
        print("FIRST_VALUE_DEMO_CONTRACT=FAIL")
        print(f"bootstrap_error={exc}")
        return 1
    print("FIRST_VALUE_DEMO_CONTRACT=" + summary["contract"])
    print("expected_file_loaded=" + str(summary["expected_file_loaded"]).lower())
    print("trace_assertion=" + summary["trace_assertion"])
    print(
        "remaining_matching_namespaces="
        + str(summary["cleanup"]["remaining_matching_namespaces"])
    )
    print("core_commit=" + summary["core"]["commit"])
    print("core_tree=" + summary["core"]["tree"])
    print("evidence_dir=" + str(evidence_dir))
    if summary["errors"]:
        for error in summary["errors"]:
            print("error=" + error)
    return 0 if summary["contract"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
