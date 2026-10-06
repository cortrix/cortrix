---
title: Watch a directory
description: Have Cortrix track file changes in a directory on the server.
weight: 40
---

On this page you'll have Cortrix watch a directory: files already in it are imported into a namespace you choose, and files you add later are imported automatically.

## Prerequisites {#prerequisites}

- A running Cortrix service. See the [Quickstart](/docs/get-started/quickstart/).
- The directory must be on a **filesystem the Cortrix server process can reach**. When Cortrix runs in Docker, that means a path inside the container, not a path on the host.

The examples use a directory under the `/data` volume inside the container. To watch a directory on the host, mount it into the container first.

## Steps {#steps}

{{% steps %}}

### Prepare a directory and a namespace {#prepare}

Create a directory in the container and put a file in it. You can find the container name with `docker ps`:

```bash
docker exec deploy-cortrix-1 sh -c \
  'mkdir -p /data/watch-demo && echo "Onboarding checklist. New hires receive a laptop on day one and complete security training in week one." > /data/watch-demo/onboarding.txt'
```

Create the namespace that receives the files:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{"name": "watched", "display_name": "Watched folder"}' \
  http://127.0.0.1:8420/api/v1/namespaces
```

### Add a watcher {#add-watcher}

`path` and `target_namespaces` are required. `recursive` defaults to `true`, which includes subdirectories:

```bash
curl -fsS -H 'Content-Type: application/json' \
  -d '{
    "path": "/data/watch-demo",
    "target_namespaces": ["watched"],
    "recursive": true
  }' \
  http://127.0.0.1:8420/api/v1/watch
```

```json
{
  "id": "9a97d854",
  "path": "/data/watch-demo",
  "recursive": true,
  "status": "active",
  "target_namespaces": ["watched"]
}
```

`target_namespaces` can list several namespaces. Cortrix creates only one system-level watcher per directory and fans file changes out to every namespace subscribed to it.

### Confirm existing files were imported {#confirm-existing}

After a few seconds, list the documents in the namespace:

```bash
curl -fsS 'http://127.0.0.1:8420/api/v1/documents?namespace=watched'
```

```json
{
  "documents": [
    {
      "document_id": "01M421DDBRNPRZKD50DNRYJR9V",
      "filename": "/data/watch-demo/onboarding.txt",
      "namespace": "watched",
      "source_ref": "/data/watch-demo",
      "source_type": "watch_dir",
      "status": "ready"
    }
  ],
  "total": 1
}
```

Documents that come from a watcher have a `source_type` of `watch_dir`, and `filename` is the file's full path.

### Confirm new files are imported automatically {#confirm-new}

Add another file to the directory:

```bash
docker exec deploy-cortrix-1 sh -c \
  'echo "Expense policy. Submit receipts within 14 days." > /data/watch-demo/expenses.txt'
```

After a few seconds, list the documents again. `total` is now 2, and the new file's status is `ready`.

{{% /steps %}}

## List and remove watchers {#list-and-remove}

List all watchers:

```bash
curl -fsS http://127.0.0.1:8420/api/v1/watch
```

Remove a watcher, along with all of its namespace subscriptions:

```bash
curl -fsS -X DELETE http://127.0.0.1:8420/api/v1/watch/9a97d854
```

A successful removal returns HTTP 204.

> [!WARNING]
> Removing a watcher also deletes the documents it imported from that directory. If you want to keep them, don't remove the watcher.

## Known issue {#known-issues}

The OpenAPI spec lists `GET /api/v1/watch/{id}/events` for reading a watcher's event stream. In testing, this endpoint returned 404. It isn't available in `v1.0.0-rc.2`.

## Next steps {#next-steps}

- [Query and rerank](/docs/guides/query/)
- [Upload documents](/docs/guides/upload-documents/)
- [Deploy with Docker Compose](/docs/deploy/docker-compose/)
