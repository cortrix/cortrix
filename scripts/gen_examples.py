#!/usr/bin/env python3
"""gen_examples.py — generate three-language example skeletons (curl / Python SDK / JS fetch) from the bundled spec.

Background
----------
P04 § 8.1: each endpoint needs 3 languages × 3 scenarios = 9 examples. ~71 endpoints =
~639 files, which is unmaintainable to hand-write (P04 R6 reserved an "example
auto-generation tool"). This script reads build/openapi.bundled.yaml and, per operation,
generates example skeletons from its method / path / x-cortrix metadata / requestBody schema:

  examples/<domain>/<opid>/success/{curl.sh,python.py,javascript.js}
  examples/<domain>/<opid>/error_category_<cat>/{curl.sh,python.py,javascript.js,response.json}

Error scenarios are derived by mapping the 4xx/5xx in each operation's responses → category
(auth/quota/transient/permanent/timeout), guaranteeing ≥1 success + ≥2 errors per endpoint
(including 1 retryable=true + 1 false) to satisfy the § 8.4 CI gate.

The hand-refined canonical directories (query/ documents/ memory/) are not overwritten by
default (only with --force).

Usage:
    python3 scripts/gen_examples.py [--spec build/openapi.bundled.yaml] [--out api/examples] [--force] [--dry-run]

Note: this generates **skeletons** (placeholder payloads); owners refine the payloads before D5.
The 3 canonical domains are the refinement reference.
"""
from __future__ import annotations

import argparse
import os
import sys

try:
    import yaml
except ImportError:
    sys.stderr.write("PyYAML is required\n")
    sys.exit(2)

HTTP_TO_CATEGORY = {
    "400": ("permanent", False, "CX_ERR_INVALID_REQUEST"),
    "401": ("auth", False, "CX_ERR_AUTH_INVALID_API_KEY"),
    "403": ("auth", False, "CX_ERR_NS_UNAUTHORIZED"),
    "404": ("permanent", False, "CX_ERR_NOT_FOUND"),
    "409": ("permanent", False, "CX_ERR_CONFLICT"),
    "413": ("permanent", False, "CX_ERR_PAYLOAD_TOO_LARGE"),
    "429": ("quota", True, "CX_ERR_RATE_LIMIT"),
    "500": ("transient", True, "CX_ERR_INTERNAL"),
    "503": ("transient", True, "CX_ERR_SERVICE_UNAVAILABLE"),
    "504": ("timeout", True, "CX_ERR_TIMEOUT"),
}

# These domains have hand-refined canonical examples; skipped by default (unless --force).
CANONICAL_DOMAINS = {"query", "documents", "memory"}

METHODS = ("get", "post", "put", "delete", "patch")


def domain_of(path: str) -> str:
    # /api/v1 already stripped in spec paths (servers carry the prefix); take the first segment.
    # Special cases (aligned with the P04 § 2.1 examples/ directory naming):
    #   /namespaces/{ns}/acl*  → acl
    #   /gc/* + /maintenance/* → ops (P03 GC + maintenance both belong to the ops domain)
    if "/acl" in path:
        return "acl"
    seg = [s for s in path.split("/") if s and not s.startswith("{")]
    first = seg[0] if seg else "root"
    if first in ("gc", "maintenance"):
        return "ops"
    return first


def curl_for(method: str, path: str, has_body: bool, ok: bool, status: str = "") -> str:
    url = f'"https://api.cortrix.io/api/v1{path}"'
    lines = [f"#!/usr/bin/env bash", f"# {method.upper()} /api/v1{path} — " + ("success" if ok else f"error (HTTP {status})")]
    cmd = [f'curl -X {method.upper()} {url} \\', '  -H "X-API-Key: cx_live_xxx" \\']
    if has_body:
        cmd.append('  -H "Content-Type: application/json" \\')
        cmd.append("  -d '{ /* TODO: request payload, see components/schemas */ }'")
    else:
        cmd[-1] = cmd[-1].rstrip(" \\")
    if not ok:
        cmd.append(f"# → HTTP {status}, see response.json")
    return "\n".join(lines + cmd) + "\n"


def python_for(method: str, path: str, ok: bool, code: str = "") -> str:
    body = [
        f'"""{method.upper()} /api/v1{path} — ' + ("success" if ok else f"error {code}") + ' (Python SDK skeleton)."""',
        "from cortrix import Client",
    ]
    if not ok:
        body.append("from cortrix.exceptions import CortrixError")
    body += ["", 'client = Client(api_key="cx_live_xxx")', ""]
    if ok:
        body.append("# TODO: call the matching client.<resource>.<verb>(...), see P04 § 8.3 + canonical reference (query/documents/memory)")
    else:
        body += [
            "try:",
            "    ...  # TODO: call the matching client method",
            "except CortrixError as e:",
            "    # Agent decision: route by e.retryable / e.category (retry / degrade / notify)",
            "    print(e.code, e.category, e.retryable)",
        ]
    return "\n".join(body) + "\n"


def js_for(method: str, path: str, has_body: bool, ok: bool, status: str = "") -> str:
    url = f'"https://api.cortrix.io/api/v1{path}"'
    lines = [f"// {method.upper()} /api/v1{path} — " + ("success" if ok else f"error (HTTP {status})")]
    opts = [
        f"const resp = await fetch({url}, {{",
        f'  method: "{method.upper()}",',
        '  headers: { "X-API-Key": "cx_live_xxx"' + (', "Content-Type": "application/json"' if has_body else "") + " },",
    ]
    if has_body:
        opts.append("  body: JSON.stringify({ /* TODO: payload */ }),")
    opts.append("});")
    if ok:
        opts.append("const result = await resp.json();")
    else:
        opts += [
            "if (!resp.ok) {",
            "  const err = (await resp.json()).error;",
            "  // Agent decision: route by err.retryable / err.category",
            "  console.log(err.code, err.category, err.retryable);",
            "}",
        ]
    return "\n".join(lines + opts) + "\n"


def response_json(code: str, category: str, retryable: bool) -> str:
    retry_after = "1000" if retryable else "null"
    return (
        "{\n"
        '  "error": {\n'
        f'    "code": "{code}",\n'
        '    "message": "TODO: human-readable message",\n'
        f'    "retryable": {"true" if retryable else "false"},\n'
        f'    "category": "{category}",\n'
        f'    "retry_after_ms": {retry_after},\n'
        '    "structured_data": {},\n'
        '    "request_id": "req_xxx"\n'
        "  }\n"
        "}\n"
    )


def write(path: str, content: str, force: bool, dry: bool, created: list, skipped: list):
    if os.path.exists(path) and not force:
        skipped.append(path)
        return
    created.append(path)
    if dry:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)


def main() -> int:
    ap = argparse.ArgumentParser()
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--spec", default=os.path.join(here, "..", "build", "openapi.bundled.yaml"))
    ap.add_argument("--out", default=os.path.join(here, "..", "api", "examples"))
    ap.add_argument("--force", action="store_true", help="overwrite existing files (including canonical domains)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--include-canonical", action="store_true", help="also generate for canonical domains (skipped by default)")
    args = ap.parse_args()

    spec_path = os.path.abspath(args.spec)
    if not os.path.exists(spec_path):
        print(f"[FAIL] bundled spec not found: {spec_path} (run scripts/bundle_openapi.sh first)")
        return 1
    spec = yaml.safe_load(open(spec_path, encoding="utf-8"))
    out = os.path.abspath(args.out)

    created: list[str] = []
    skipped: list[str] = []
    n_ops = 0

    for path, item in (spec.get("paths") or {}).items():
        dom = domain_of(path)
        if dom in CANONICAL_DOMAINS and not args.include_canonical and not args.force:
            continue
        for method, op in item.items():
            if method.lower() not in METHODS or not isinstance(op, dict):
                continue
            n_ops += 1
            opid = op.get("operationId") or f"{method}_{dom}"
            has_body = "requestBody" in op
            base = os.path.join(out, dom, opid)

            # success
            write(os.path.join(base, "success", "curl.sh"), curl_for(method, path, has_body, True), args.force, args.dry_run, created, skipped)
            write(os.path.join(base, "success", "python.py"), python_for(method, path, True), args.force, args.dry_run, created, skipped)
            write(os.path.join(base, "success", "javascript.js"), js_for(method, path, has_body, True), args.force, args.dry_run, created, skipped)

            # errors: derive category from the 4xx/5xx in responses, de-duplicating categories
            seen_cat: dict[str, tuple] = {}
            for status in (op.get("responses") or {}):
                if str(status) in HTTP_TO_CATEGORY:
                    cat, retry, code = HTTP_TO_CATEGORY[str(status)]
                    seen_cat.setdefault(cat, (status, retry, code))
            # guarantee ≥2 errors (including retryable true+false)
            if not any(r for _, r, _ in seen_cat.values()):
                seen_cat["transient"] = ("429", True, "CX_ERR_RATE_LIMIT")
            if not any(not r for _, r, _ in seen_cat.values()):
                seen_cat["auth"] = ("403", False, "CX_ERR_NS_UNAUTHORIZED")
            for cat, (status, retry, code) in seen_cat.items():
                d = os.path.join(base, f"error_category_{cat}")
                write(os.path.join(d, "curl.sh"), curl_for(method, path, has_body, False, status), args.force, args.dry_run, created, skipped)
                write(os.path.join(d, "python.py"), python_for(method, path, False, code), args.force, args.dry_run, created, skipped)
                write(os.path.join(d, "javascript.js"), js_for(method, path, has_body, False, status), args.force, args.dry_run, created, skipped)
                write(os.path.join(d, "response.json"), response_json(code, cat, retry), args.force, args.dry_run, created, skipped)

    verb = "[dry-run] would create" if args.dry_run else "created"
    print(f"[gen] scanned {n_ops} operations; {verb} {len(created)} files; skipped {len(skipped)} existing")
    print(f"[gen] canonical domains (query/documents/memory) {'included' if args.include_canonical else 'skipped by default, refined references kept'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
