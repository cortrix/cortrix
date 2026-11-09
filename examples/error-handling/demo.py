#!/usr/bin/env python3
"""Run six failing requests against Cortrix and handle each one from its error fields.

Uses only the standard library, so you see the raw JSON. For each scenario it
prints the request, the response fields an agent acts on, what decide() says,
and what the agent does next. See README.md.
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from decide import decide, decide_items

DOC = "# Refund policy\n\nRefunds go back to the original payment method in 3 to 5 business days.\n"


class Api:
    def __init__(self, base_url):
        self.base = base_url.rstrip("/") + "/api/v1"

    def call(self, method, path, key=None, params=None, body=None, headers=None, raw=None):
        """Send one request. Return (status, parsed JSON or None)."""
        url = self.base + path
        if params:
            url += "?" + urllib.parse.urlencode(params)
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        h = {"Content-Type": "application/json", **(headers or {})}
        if key:
            h["Authorization"] = "Bearer " + key
        req = urllib.request.Request(url, data=data, method=method, headers=h)
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                status, text = resp.status, resp.read().decode()
        except urllib.error.HTTPError as e:
            status, text = e.code, e.read().decode()
        try:
            return status, json.loads(text) if text else None
        except ValueError:
            return status, None


def show_error(status, body):
    """Print the fields an agent acts on and return the error object."""
    err = body["error"]
    print(f"  response : HTTP {status} {err['code']} category={err['category']} "
          f"retryable={json.dumps(err['retryable'])} "
          f"retry_after_ms={json.dumps(err.get('retry_after_ms'))}")
    print(f"  structured_data: {json.dumps(err.get('structured_data'))}")
    return err


def step(text):
    print(f"  next step: {text}")


def wait_ready(api, key, task_id):
    """Poll the upload task. Return (final status, document id)."""
    for _ in range(120):
        _, body = api.call("GET", f"/documents/tasks/{task_id}/progress", key)
        if body and body.get("status") in ("completed", "failed", "cancelled"):
            return body["status"], body.get("doc_id")
        time.sleep(0.5)
    return "timeout", None


def probe_auth(api, key, ns):
    """Authentication is on if a name that does not exist is refused with 403."""
    status, _ = api.call("GET", f"/namespaces/{ns}-typo", key)
    return status == 403


def scenario_a(api, agent, ns, doc_id):
    print("\n== a. Delete a document without the namespace")
    print(f"  request  : DELETE /documents/{doc_id}")
    status, body = api.call("DELETE", f"/documents/{doc_id}", agent)
    action = decide(show_error(status, body))
    print(f"  decision : {json.dumps(action)}")
    status, _ = api.call("DELETE", f"/documents/{doc_id}", agent, params={action["add_param"]: ns})
    step(f"add {action['add_param']}={ns} and send it again -> HTTP {status}")


def scenario_b(api, agent, admin, ns, doc_id, auth_on):
    print("\n== b. Restore the document with a read-write key")
    if not auth_on or not admin:
        print("  skipped: needs authentication on and an admin key (--admin-key)")
        return False
    print(f"  request  : POST /gc/restore document_ids=[{doc_id}]")
    status, body = api.call("POST", "/gc/restore", agent, body={"document_ids": [doc_id]})
    action = decide(show_error(status, body))
    print(f"  decision : {json.dumps(action)}")
    status, _ = api.call("GET", f"/namespaces/{ns}", agent)
    step(f"check the request first: GET /namespaces/{ns} -> HTTP {status}, the namespace exists")
    status, body = api.call("POST", "/gc/restore", admin, body={"document_ids": [doc_id]})
    step(f"the key is the problem, so repeat with the admin key -> HTTP {status}, "
         f"coverage_ratio={body['coverage_ratio']}")
    return status == 200


def scenario_c(api, agent, ns, auth_on):
    print("\n== c. Use a namespace name with a typo")
    if not auth_on:
        print("  skipped: needs authentication on (without it a missing namespace is not a 403)")
        return
    typo = f"{ns}-typo"
    print(f"  request  : GET /namespaces/{typo}/documents")
    status, body = api.call("GET", f"/namespaces/{typo}/documents", agent)
    action = decide(show_error(status, body))
    print(f"  decision : {json.dumps(action)}")
    status, body = api.call("GET", "/namespaces", agent)
    names = [n["name"] for n in body["namespaces"]]
    step(f"check the request first: GET /namespaces -> HTTP {status}, visible: {', '.join(names)}")
    if typo not in names:
        step(f"{typo} is not in the list. A different key cannot help: check the namespace name")


def scenario_d(api, admin):
    print("\n== d. Purge without the confirmation header")
    print("  request  : POST /gc/purge")
    status, body = api.call("POST", "/gc/purge", admin)
    action = decide(show_error(status, body))
    print(f"  decision : {json.dumps(action)}")
    step("nothing sent. Purge removes every soft-deleted document on the server, "
         "so a person decides")


def scenario_e(api, admin, ns, doc_id, restored):
    typo = doc_id[:-1] + ("X" if doc_id[-1] != "X" else "Y")
    print("\n== e. Restore two ids: the deleted document and a mistyped copy")
    if restored:  # scenario b brought the document back, so delete it again first
        status, _ = api.call("DELETE", f"/documents/{doc_id}", admin, params={"namespace": ns})
        print(f"  (setup: DELETE /documents/{doc_id}?namespace={ns} -> HTTP {status})")
    print(f"  request  : POST /gc/restore document_ids=[{doc_id}, {typo}]")
    status, body = api.call("POST", "/gc/restore", admin, body={"document_ids": [doc_id, typo]})
    result = decide_items(body)
    print(f"  response : HTTP {status} coverage_ratio={result['coverage_ratio']}")
    for item in body["succeeded"]:
        print(f"  item {item['id']}: succeeded")
    for item, f in zip(result["items"], body["failed"]):
        print(f"  item {item['id']}: {f['error_code']} category={f['category']} "
              f"retryable={json.dumps(f['retryable'])}")
        print(f"    decision : {json.dumps({k: v for k, v in item.items() if k != 'id'})}")
        step("give up on this id and report it" if item["action"] == "give_up"
             else f"{item['action']} for this id")


def scenario_f(api, agent):
    print("\n== f. Send a query that is not valid JSON")
    print("  request  : POST /query body={not json")
    status, body = api.call("POST", "/query", agent, raw=b"{not json")
    action = decide(show_error(status, body))
    print(f"  decision : {json.dumps(action)}")
    step("the same request fails again unchanged. Give up and report it")


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--base-url", default="http://127.0.0.1:8420")
    p.add_argument("--api-key", default=os.environ.get("CORTRIX_API_KEY"), help="READ+WRITE key")
    p.add_argument("--admin-key", default=os.environ.get("CORTRIX_ADMIN_KEY"),
                   help="key with ADMIN permission")
    p.add_argument("--keep", action="store_true", help="keep the namespace")
    args = p.parse_args()
    api = Api(args.base_url)
    agent, admin = args.api_key, args.admin_key or args.api_key

    ns = f"error-handling-{int(time.time())}"
    status, body = api.call("POST", "/namespaces", admin, body={"name": ns})
    if status >= 400:
        sys.exit(f"could not create namespace {ns}: HTTP {status} {json.dumps(body)}")
    print(f"namespace: {ns}")
    status, body = api.call("POST", "/documents", agent,
                            body={"namespace": ns, "content": DOC, "filename": "refund-policy.md"})
    task_id = body["task_id"]
    state, doc_id = wait_ready(api, agent, task_id)
    print(f"uploaded refund-policy.md: {doc_id}, {state}")

    scenario_a(api, agent, ns, doc_id)
    auth_on = probe_auth(api, agent, ns)
    restored = scenario_b(api, agent, args.admin_key, ns, doc_id, auth_on)
    scenario_c(api, agent, ns, auth_on)
    scenario_d(api, admin)
    scenario_e(api, admin, ns, doc_id, restored)
    scenario_f(api, agent)

    if args.keep:
        print(f"\nkept namespace {ns}")
    else:
        status, _ = api.call("DELETE", f"/namespaces/{ns}", admin)
        print(f"\nDELETE /namespaces/{ns} -> {status}")


if __name__ == "__main__":
    main()
