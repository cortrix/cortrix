#!/usr/bin/env python3
"""Checks the Publish Image workflow runs between building an image and tagging it.

Standard library only; runs on the Python the CI runners ship (3.10 and later).
Every subcommand reads files that the workflow wrote from `docker buildx` output,
decides one thing, prints what it decided, and exits non-zero on the first
violation. Nothing here talks to a registry: the workflow fetches, this checks.

Subcommands

  digests DIR
      DIR holds exactly the files `amd64` and `arm64`, one digest each
      (`sha256:` + 64 hex digits), and the two differ. The file name is the only
      source of the digest-to-platform mapping, so the names are checked, not
      just the count.

  manifest FILE
      FILE is one image manifest (OCI image manifest or Docker manifest v2), not
      an index or manifest list, with a `config.digest` and at least one layer.
      Prints the config digest.

  config FILE --platform linux/ARCH --revision SHA
      FILE is the image config as `docker buildx imagetools inspect --format
      '{{json .Image}}'` prints it: `os`/`architecture` equal the platform and the
      `org.opencontainers.image.revision` label equals SHA.

  bind --load-meta FILE --push-meta FILE --manifest FILE
       [--diff-ids-local JSON --remote-config FILE]
      The image that was gated is the image that was pushed: the config digest
      recorded by the `--load` build, the one recorded by the push-by-digest
      export, and the `config.digest` of the pushed manifest are one value. When a
      metadata file lacks the key, the fallback witness is the local image's
      rootfs layer list against the remote config's `rootfs.diff_ids`.

  index FILE --expect linux/amd64=DIGEST --expect linux/arm64=DIGEST
      FILE is the created tag's index (OCI index or Docker manifest list) and its
      entries map exactly the expected platforms to exactly the expected digests:
      no duplicates, no extra entries (an `unknown/unknown` attestation entry
      counts as extra), nothing missing. `variant` and index-level annotations
      are ignored.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Any, Dict, Iterable, List, Tuple

DIGEST_RE = re.compile(r"^sha256:[0-9a-f]{64}$")

MEDIA_IMAGE_MANIFEST = {
    "application/vnd.oci.image.manifest.v1+json",
    "application/vnd.docker.distribution.manifest.v2+json",
}
MEDIA_INDEX = {
    "application/vnd.oci.image.index.v1+json",
    "application/vnd.docker.distribution.manifest.list.v2+json",
}
REVISION_LABEL = "org.opencontainers.image.revision"
CONFIG_DIGEST_KEY = "containerimage.config.digest"


class Violation(Exception):
    """One check failed; the message says which and why."""


def _load_json(path: str) -> Any:
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except FileNotFoundError:
        raise Violation(f"{path}: file not found")
    except json.JSONDecodeError as exc:
        raise Violation(f"{path}: not JSON ({exc})")


def _require_digest(value: Any, what: str) -> str:
    if not isinstance(value, str) or not DIGEST_RE.match(value):
        raise Violation(f"{what}: expected sha256:<64 hex>, got {value!r}")
    return value


def _platform_of(entry: Dict[str, Any]) -> str:
    plat = entry.get("platform")
    if not isinstance(plat, dict):
        raise Violation(f"index entry {entry.get('digest')!r} has no platform")
    os_ = plat.get("os")
    arch = plat.get("architecture")
    if not isinstance(os_, str) or not isinstance(arch, str) or not os_ or not arch:
        raise Violation(f"index entry {entry.get('digest')!r} has an incomplete platform {plat!r}")
    return f"{os_}/{arch}"


# --- subcommands -------------------------------------------------------------


def cmd_digests(args: argparse.Namespace) -> int:
    expected = ["amd64", "arm64"]
    try:
        names = sorted(os.listdir(args.dir))
    except FileNotFoundError:
        raise Violation(f"{args.dir}: directory not found")
    if names != expected:
        raise Violation(f"{args.dir}: expected exactly the files {expected}, found {names}")
    seen: Dict[str, str] = {}
    for name in expected:
        with open(os.path.join(args.dir, name), "r", encoding="utf-8") as fh:
            value = fh.read().strip()
        seen[name] = _require_digest(value, f"{args.dir}/{name}")
    if seen["amd64"] == seen["arm64"]:
        raise Violation("amd64 and arm64 carry the same digest; one platform was not built")
    for name in expected:
        print(f"{name} {seen[name]}")
    return 0


def cmd_manifest(args: argparse.Namespace) -> int:
    doc = _load_json(args.file)
    if not isinstance(doc, dict):
        raise Violation(f"{args.file}: manifest is not a JSON object")
    media = doc.get("mediaType")
    if media in MEDIA_INDEX:
        raise Violation(f"{args.file}: this is an index ({media}), not one image manifest")
    if media not in MEDIA_IMAGE_MANIFEST:
        raise Violation(f"{args.file}: unsupported mediaType {media!r}")
    config = doc.get("config")
    if not isinstance(config, dict):
        raise Violation(f"{args.file}: no config descriptor")
    config_digest = _require_digest(config.get("digest"), f"{args.file}: config.digest")
    layers = doc.get("layers")
    if not isinstance(layers, list) or not layers:
        raise Violation(f"{args.file}: no layers")
    for i, layer in enumerate(layers):
        if not isinstance(layer, dict):
            raise Violation(f"{args.file}: layer {i} is not a descriptor")
        _require_digest(layer.get("digest"), f"{args.file}: layers[{i}].digest")
    print(config_digest)
    return 0


def cmd_config(args: argparse.Namespace) -> int:
    doc = _load_json(args.file)
    if not isinstance(doc, dict):
        raise Violation(f"{args.file}: config is not a JSON object")
    want_os, _, want_arch = args.platform.partition("/")
    if not want_os or not want_arch:
        raise Violation(f"--platform must look like linux/amd64, got {args.platform!r}")
    got = f"{doc.get('os')}/{doc.get('architecture')}"
    if got != args.platform:
        raise Violation(f"{args.file}: platform is {got}, expected {args.platform}")
    labels = (doc.get("config") or {}).get("Labels") or {}
    if not isinstance(labels, dict):
        raise Violation(f"{args.file}: config.Labels is not an object")
    revision = labels.get(REVISION_LABEL)
    if revision != args.revision:
        raise Violation(
            f"{args.file}: label {REVISION_LABEL} is {revision!r}, expected {args.revision!r}"
        )
    print(f"{got} {revision}")
    return 0


def _config_digest_from_meta(path: str) -> str | None:
    doc = _load_json(path)
    if not isinstance(doc, dict):
        raise Violation(f"{path}: metadata is not a JSON object")
    value = doc.get(CONFIG_DIGEST_KEY)
    if value is None:
        return None
    return _require_digest(value, f"{path}: {CONFIG_DIGEST_KEY}")


def cmd_bind(args: argparse.Namespace) -> int:
    manifest = _load_json(args.manifest)
    if not isinstance(manifest, dict) or not isinstance(manifest.get("config"), dict):
        raise Violation(f"{args.manifest}: not an image manifest with a config descriptor")
    remote = _require_digest(manifest["config"].get("digest"), f"{args.manifest}: config.digest")

    load_digest = _config_digest_from_meta(args.load_meta)
    push_digest = _config_digest_from_meta(args.push_meta)

    if load_digest is not None and push_digest is not None:
        if not (load_digest == push_digest == remote):
            raise Violation(
                "the gated image is not the pushed image: config digests differ "
                f"(load {load_digest}, push {push_digest}, registry {remote})"
            )
        print(f"bound by config digest {remote}")
        return 0

    # Fallback witness: the local image's layer list against the remote config.
    if not args.diff_ids_local or not args.remote_config:
        missing = [
            p for p, d in ((args.load_meta, load_digest), (args.push_meta, push_digest)) if d is None
        ]
        raise Violation(
            f"{', '.join(missing)}: no {CONFIG_DIGEST_KEY}; pass --diff-ids-local and "
            "--remote-config for the layer-list witness"
        )
    try:
        local_layers = json.loads(args.diff_ids_local)
    except json.JSONDecodeError as exc:
        raise Violation(f"--diff-ids-local is not JSON ({exc})")
    if not isinstance(local_layers, list) or not local_layers:
        raise Violation("--diff-ids-local must be a non-empty JSON list of layer digests")
    remote_cfg = _load_json(args.remote_config)
    remote_layers = (remote_cfg.get("rootfs") or {}).get("diff_ids") if isinstance(remote_cfg, dict) else None
    if not isinstance(remote_layers, list):
        raise Violation(f"{args.remote_config}: no rootfs.diff_ids")
    if local_layers != remote_layers:
        raise Violation(
            "the gated image is not the pushed image: layer lists differ "
            f"(local {len(local_layers)} layers, registry {len(remote_layers)} layers, or different content)"
        )
    if push_digest is not None and push_digest != remote:
        raise Violation(f"push metadata names config {push_digest} but the registry has {remote}")
    print(f"bound by layer list ({len(local_layers)} layers), registry config {remote}")
    return 0


def _parse_expect(values: Iterable[str]) -> Dict[str, str]:
    expected: Dict[str, str] = {}
    for item in values:
        platform, sep, digest = item.partition("=")
        if not sep or platform.count("/") != 1:
            raise Violation(f"--expect must look like linux/amd64=sha256:..., got {item!r}")
        if platform in expected:
            raise Violation(f"--expect names {platform} twice")
        expected[platform] = _require_digest(digest, f"--expect {platform}")
    if not expected:
        raise Violation("at least one --expect is required")
    if len(set(expected.values())) != len(expected):
        raise Violation("--expect maps two platforms to the same digest")
    return expected


def cmd_index(args: argparse.Namespace) -> int:
    expected = _parse_expect(args.expect)
    doc = _load_json(args.file)
    if not isinstance(doc, dict):
        raise Violation(f"{args.file}: index is not a JSON object")
    media = doc.get("mediaType")
    if media not in MEDIA_INDEX:
        raise Violation(f"{args.file}: mediaType {media!r} is not an image index or manifest list")
    entries = doc.get("manifests")
    if not isinstance(entries, list):
        raise Violation(f"{args.file}: no manifests list")
    found: Dict[str, str] = {}
    for entry in entries:
        if not isinstance(entry, dict):
            raise Violation(f"{args.file}: an index entry is not a descriptor")
        digest = _require_digest(entry.get("digest"), f"{args.file}: entry digest")
        platform = _platform_of(entry)
        if platform in found:
            raise Violation(f"{args.file}: platform {platform} appears twice")
        if platform not in expected:
            raise Violation(
                f"{args.file}: entry {digest} for {platform} is not one of the gated platforms "
                f"{sorted(expected)}"
            )
        if digest != expected[platform]:
            raise Violation(
                f"{args.file}: {platform} references {digest}, the gated image is {expected[platform]}"
            )
        found[platform] = digest
    missing = sorted(set(expected) - set(found))
    if missing:
        raise Violation(f"{args.file}: platforms missing from the index: {missing}")
    for platform in sorted(found):
        print(f"{platform} {found[platform]}")
    return 0


# --- entry point -------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("digests", help="exactly amd64 and arm64, well-formed and distinct")
    p.add_argument("dir")
    p.set_defaults(func=cmd_digests)

    p = sub.add_parser("manifest", help="one image manifest with config and layers")
    p.add_argument("file")
    p.set_defaults(func=cmd_manifest)

    p = sub.add_parser("config", help="image config carries the platform and revision")
    p.add_argument("file")
    p.add_argument("--platform", required=True)
    p.add_argument("--revision", required=True)
    p.set_defaults(func=cmd_config)

    p = sub.add_parser("bind", help="the gated image is the pushed image")
    p.add_argument("--load-meta", required=True)
    p.add_argument("--push-meta", required=True)
    p.add_argument("--manifest", required=True)
    p.add_argument("--diff-ids-local")
    p.add_argument("--remote-config")
    p.set_defaults(func=cmd_bind)

    p = sub.add_parser("index", help="the created tag references exactly the gated digests")
    p.add_argument("file")
    p.add_argument("--expect", action="append", default=[], metavar="PLATFORM=DIGEST")
    p.set_defaults(func=cmd_index)
    return parser


def main(argv: List[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except Violation as exc:
        print(f"publish-image check failed ({args.command}): {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
