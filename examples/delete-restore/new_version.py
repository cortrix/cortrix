#!/usr/bin/env python3
"""Upload a file again under the same name, then try to restore the old version.

A file uploaded through POST /api/v1/namespaces/{name}/documents is versioned
by its file name. Uploading the same name again creates a new version, and the
old one is soft-deleted once the new one is ready. Restoring the old version
while the new one is live is refused, and the error says what to do instead.
"""

from __future__ import annotations

import argparse
import os
import sys
import time

import httpx

FILENAME = "refund-policy.md"
VERSION_1 = "# Refund policy\n\nRefunds take 7 to 10 business days.\n"
VERSION_2 = "# Refund policy\n\nBank cards take 3 to 5 business days.\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base-url", default="http://127.0.0.1:8420")
    parser.add_argument("--api-key", default=os.environ.get("CORTRIX_API_KEY"),
                        help="needed only when auth is on; restoring takes an ADMIN key")
    parser.add_argument("--keep", action="store_true", help="keep the namespace afterwards")
    args = parser.parse_args()

    headers = {"Authorization": f"Bearer {args.api_key}"} if args.api_key else {}
    api = httpx.Client(base_url=args.base_url.rstrip("/") + "/api/v1", headers=headers, timeout=60)

    # A deleted namespace name can't be created again, so every run uses a new name.
    namespace = f"new-version-{int(time.time())}"
    api.post("/namespaces", json={"name": namespace}).raise_for_status()
    print(f"namespace: {namespace}")

    def upload(text: str) -> dict:
        r = api.post(f"/namespaces/{namespace}/documents",
                     files={"file": (FILENAME, text.encode(), "text/markdown")})
        r.raise_for_status()
        return r.json()

    def wait_until_only_live(doc_id: str, timeout: float = 120.0) -> None:
        """Wait until doc_id is ready and is the only live document."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            docs = api.get(f"/namespaces/{namespace}/documents").json()["documents"]
            if [(d["doc_id"], d["status"]) for d in docs] == [(doc_id, "ready")]:
                return
            time.sleep(0.5)
        raise SystemExit(f"{doc_id} did not become the only ready document in {timeout:.0f} s")

    v1 = upload(VERSION_1)
    wait_until_only_live(v1["doc_id"])
    print(f"upload {FILENAME}: doc_id={v1['doc_id']}")
    v2 = upload(VERSION_2)
    print(f"upload {FILENAME} again: doc_id={v2['doc_id']} superseded_doc_id={v2.get('superseded_doc_id')}")
    wait_until_only_live(v2["doc_id"])

    print(f"restore the first version {v1['doc_id']}:")
    result = api.post("/gc/restore", json={"document_ids": [v1["doc_id"]]}).json()
    refusal = result["failed"][0]
    print(f"  {refusal['error_code']}")
    print(f"  {refusal['message']}")

    # Do what the message says: delete the live version, then restore again.
    live = refusal["structured_data"]["live_doc_id"]
    r = api.delete(f"/documents/{live}", params={"namespace": refusal["structured_data"]["namespace"]})
    print(f"DELETE /documents/{live} -> {r.status_code}")
    result = api.post("/gc/restore", json={"document_ids": [v1["doc_id"]]}).json()
    print(f"restore {v1['doc_id']} again: succeeded={[x['id'] for x in result['succeeded']]} "
          f"coverage_ratio={result['coverage_ratio']}")

    if not args.keep:
        api.delete(f"/namespaces/{namespace}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
