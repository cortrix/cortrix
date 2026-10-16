#!/usr/bin/env bash
# Offline tests for scripts/ci/publish_image_verify.py, the checks the Publish
# Image workflow runs between building an image and tagging it. Fixtures are
# written here from the shapes `docker buildx imagetools inspect --raw` and
# `--metadata-file` produce; nothing touches a registry.
#
# The last case is a control on the check this script replaces: the two greps the
# workflow used to run (`grep -q linux/amd64`, `grep -q linux/arm64`) on the
# rendered inspect text accept an index whose entry is not the image this run
# built, while the `index` check rejects the same index as JSON.
set -euo pipefail
cd "$(dirname "$0")/../.."
SCRIPT=scripts/ci/publish_image_verify.py
PY="${PYTHON:-python3}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
PASS=0
FAIL=0

A="sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
B="sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
C="sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
CFG="sha256:1111111111111111111111111111111111111111111111111111111111111111"
CFG2="sha256:2222222222222222222222222222222222222222222222222222222222222222"
LAYER="sha256:3333333333333333333333333333333333333333333333333333333333333333"
REV="0123456789abcdef0123456789abcdef01234567"

expect_ok()   { local name="$1"; shift; if "$@" >"$TMP/out" 2>"$TMP/err"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FAIL (expected pass): $name"; cat "$TMP/err"; fi; }
# A rejection is a non-zero exit AND the script's own failure line: a crash
# (traceback) also exits non-zero, and must not pass for a decision.
expect_fail() {
  local name="$1"; shift
  if "$@" >"$TMP/out" 2>"$TMP/err"; then
    FAIL=$((FAIL+1)); echo "FAIL (expected rejection): $name"
  elif ! grep -q '^publish-image check failed' "$TMP/err"; then
    FAIL=$((FAIL+1)); echo "FAIL (crashed instead of rejecting): $name"; cat "$TMP/err"
  else
    PASS=$((PASS+1))
  fi
}
# A rejection whose message must name the reason.
expect_fail_with() {
  local name="$1" needle="$2"; shift 2
  expect_fail "$name" "$@"
  grep -q -- "$needle" "$TMP/err" || { FAIL=$((FAIL+1)); echo "FAIL (reason not named: $needle): $name"; cat "$TMP/err"; }
}

# --- fixtures ---------------------------------------------------------------
index_json() {  # $1 mediaType, then entries as "os/arch=digest" (an entry "unknown/unknown=digest" models an attestation)
  local media="$1"; shift
  "$PY" - "$media" "$@" <<'PY'
import json, sys
media, entries = sys.argv[1], sys.argv[2:]
child = ("application/vnd.oci.image.manifest.v1+json" if media.startswith("application/vnd.oci")
         else "application/vnd.docker.distribution.manifest.v2+json")
out = {"schemaVersion": 2, "mediaType": media, "manifests": []}
for e in entries:
    plat, digest = e.split("=")
    os_, arch = plat.split("/")
    d = {"mediaType": child, "size": 1234, "digest": digest, "platform": {"os": os_, "architecture": arch}}
    if arch == "arm64":
        d["platform"]["variant"] = "v8"
    out["manifests"].append(d)
if media.startswith("application/vnd.oci"):
    out["annotations"] = {"org.opencontainers.image.description": "fixture"}
print(json.dumps(out))
PY
}
manifest_json() {  # $1 mediaType $2 config digest
  printf '{"schemaVersion":2,"mediaType":"%s","config":{"mediaType":"application/vnd.oci.image.config.v1+json","size":10,"digest":"%s"},"layers":[{"mediaType":"application/vnd.oci.image.layer.v1.tar+gzip","size":20,"digest":"%s"}]}' "$1" "$2" "$LAYER"
}
config_json() {  # $1 os $2 arch $3 revision
  printf '{"architecture":"%s","os":"%s","config":{"Labels":{"org.opencontainers.image.revision":"%s","org.opencontainers.image.source":"x"}},"rootfs":{"type":"layers","diff_ids":["%s"]}}' "$2" "$1" "$3" "$LAYER"
}

OCI=application/vnd.oci.image.index.v1+json
DML=application/vnd.docker.distribution.manifest.list.v2+json
OCIM=application/vnd.oci.image.manifest.v1+json
DM2=application/vnd.docker.distribution.manifest.v2+json

index_json "$DML" "linux/amd64=$A" "linux/arm64=$B"            > "$TMP/index_docker_good.json"
index_json "$OCI" "linux/amd64=$A" "linux/arm64=$B"            > "$TMP/index_oci_good.json"
index_json "$DML" "linux/amd64=$A" "linux/arm64=$C"            > "$TMP/index_foreign_digest.json"
index_json "$DML" "linux/amd64=$B" "linux/arm64=$A"            > "$TMP/index_swapped_platforms.json"
index_json "$OCI" "linux/amd64=$A" "linux/arm64=$B" "unknown/unknown=$C" > "$TMP/index_attestation.json"
index_json "$DML" "linux/amd64=$A"                             > "$TMP/index_one_platform.json"
index_json "$DML" "linux/amd64=$A" "linux/amd64=$A" "linux/arm64=$B" > "$TMP/index_duplicate_platform.json"
manifest_json "$OCIM" "$CFG" > "$TMP/manifest_oci.json"
manifest_json "$DM2"  "$CFG" > "$TMP/manifest_docker.json"
manifest_json "application/vnd.docker.distribution.manifest.v1+prettyjws" "$CFG" > "$TMP/manifest_schema1.json"
manifest_json "$OCIM" "$CFG2" > "$TMP/manifest_other_config.json"
config_json linux amd64 "$REV" > "$TMP/config_amd64.json"
config_json linux arm64 "$REV" > "$TMP/config_arm64.json"
config_json linux amd64 "deadbeef" > "$TMP/config_wrong_revision.json"
printf '{"containerimage.config.digest":"%s","containerimage.digest":"%s"}' "$CFG" "$A" > "$TMP/meta_load.json"
printf '{"containerimage.config.digest":"%s","containerimage.digest":"%s"}' "$CFG" "$A" > "$TMP/meta_push.json"
printf '{"containerimage.config.digest":"%s","containerimage.digest":"%s"}' "$CFG2" "$A" > "$TMP/meta_push_other.json"
printf '{"containerimage.digest":"%s"}' "$A" > "$TMP/meta_no_config.json"
mkdir -p "$TMP/d_good" "$TMP/d_missing" "$TMP/d_same" "$TMP/d_bad" "$TMP/d_extra"
printf '%s\n' "$A" > "$TMP/d_good/amd64";  printf '%s\n' "$B" > "$TMP/d_good/arm64"
printf '%s\n' "$A" > "$TMP/d_missing/amd64"
printf '%s\n' "$A" > "$TMP/d_same/amd64";  printf '%s\n' "$A" > "$TMP/d_same/arm64"
printf 'sha256:short\n' > "$TMP/d_bad/amd64"; printf '%s\n' "$B" > "$TMP/d_bad/arm64"
printf '%s\n' "$A" > "$TMP/d_extra/amd64"; printf '%s\n' "$B" > "$TMP/d_extra/arm64"; printf '%s\n' "$C" > "$TMP/d_extra/x86"

# --- digests ----------------------------------------------------------------
expect_ok   "digests: amd64 and arm64, distinct"        "$PY" "$SCRIPT" digests "$TMP/d_good"
expect_fail "digests: one platform missing"             "$PY" "$SCRIPT" digests "$TMP/d_missing"
expect_fail "digests: same digest on both"              "$PY" "$SCRIPT" digests "$TMP/d_same"
expect_fail "digests: malformed digest"                 "$PY" "$SCRIPT" digests "$TMP/d_bad"
expect_fail "digests: an extra file"                    "$PY" "$SCRIPT" digests "$TMP/d_extra"

# --- manifest ---------------------------------------------------------------
expect_ok   "manifest: OCI image manifest"              "$PY" "$SCRIPT" manifest "$TMP/manifest_oci.json"
expect_ok   "manifest: Docker manifest v2"              "$PY" "$SCRIPT" manifest "$TMP/manifest_docker.json"
expect_fail_with "manifest: an index is not a manifest" "is an index" "$PY" "$SCRIPT" manifest "$TMP/index_oci_good.json"
expect_fail "manifest: schema1 media type"              "$PY" "$SCRIPT" manifest "$TMP/manifest_schema1.json"
[ "$("$PY" "$SCRIPT" manifest "$TMP/manifest_oci.json")" = "$CFG" ] && PASS=$((PASS+1)) || { FAIL=$((FAIL+1)); echo "FAIL: manifest prints the config digest"; }

# --- config -----------------------------------------------------------------
expect_ok   "config: platform and revision match"       "$PY" "$SCRIPT" config "$TMP/config_amd64.json" --platform linux/amd64 --revision "$REV"
expect_fail "config: wrong architecture"                "$PY" "$SCRIPT" config "$TMP/config_arm64.json" --platform linux/amd64 --revision "$REV"
expect_fail "config: wrong revision label"              "$PY" "$SCRIPT" config "$TMP/config_wrong_revision.json" --platform linux/amd64 --revision "$REV"

# --- bind -------------------------------------------------------------------
expect_ok   "bind: load, push and registry agree"       "$PY" "$SCRIPT" bind --load-meta "$TMP/meta_load.json" --push-meta "$TMP/meta_push.json" --manifest "$TMP/manifest_oci.json"
expect_fail "bind: push metadata names another config"  "$PY" "$SCRIPT" bind --load-meta "$TMP/meta_load.json" --push-meta "$TMP/meta_push_other.json" --manifest "$TMP/manifest_oci.json"
expect_fail "bind: registry holds another config"       "$PY" "$SCRIPT" bind --load-meta "$TMP/meta_load.json" --push-meta "$TMP/meta_push.json" --manifest "$TMP/manifest_other_config.json"
expect_fail "bind: metadata lacks the key, no witness"  "$PY" "$SCRIPT" bind --load-meta "$TMP/meta_no_config.json" --push-meta "$TMP/meta_push.json" --manifest "$TMP/manifest_oci.json"
expect_ok   "bind: layer-list witness agrees"           "$PY" "$SCRIPT" bind --load-meta "$TMP/meta_no_config.json" --push-meta "$TMP/meta_push.json" --manifest "$TMP/manifest_oci.json" --diff-ids-local "[\"$LAYER\"]" --remote-config "$TMP/config_amd64.json"
expect_fail "bind: layer-list witness differs"          "$PY" "$SCRIPT" bind --load-meta "$TMP/meta_no_config.json" --push-meta "$TMP/meta_push.json" --manifest "$TMP/manifest_oci.json" --diff-ids-local "[\"$C\"]" --remote-config "$TMP/config_amd64.json"

# --- index ------------------------------------------------------------------
expect_ok   "index: Docker manifest list, both gated digests"   "$PY" "$SCRIPT" index "$TMP/index_docker_good.json" --expect "linux/amd64=$A" --expect "linux/arm64=$B"
expect_ok   "index: OCI index with annotations and variant"     "$PY" "$SCRIPT" index "$TMP/index_oci_good.json" --expect "linux/amd64=$A" --expect "linux/arm64=$B"
expect_fail "index: one entry is not a gated digest"            "$PY" "$SCRIPT" index "$TMP/index_foreign_digest.json" --expect "linux/amd64=$A" --expect "linux/arm64=$B"
expect_fail "index: platforms swapped"                          "$PY" "$SCRIPT" index "$TMP/index_swapped_platforms.json" --expect "linux/amd64=$A" --expect "linux/arm64=$B"
expect_fail "index: extra attestation entry"                    "$PY" "$SCRIPT" index "$TMP/index_attestation.json" --expect "linux/amd64=$A" --expect "linux/arm64=$B"
expect_fail "index: one platform missing"                       "$PY" "$SCRIPT" index "$TMP/index_one_platform.json" --expect "linux/amd64=$A" --expect "linux/arm64=$B"
expect_fail_with "index: duplicate platform" "appears twice"    "$PY" "$SCRIPT" index "$TMP/index_duplicate_platform.json" --expect "linux/amd64=$A" --expect "linux/arm64=$B"
expect_fail_with "index: extra entry is named as such" "not one of the gated platforms" "$PY" "$SCRIPT" index "$TMP/index_attestation.json" --expect "linux/amd64=$A" --expect "linux/arm64=$B"
expect_fail "index: an image manifest is not an index"          "$PY" "$SCRIPT" index "$TMP/manifest_oci.json" --expect "linux/amd64=$A" --expect "linux/arm64=$B"
expect_fail "index: expect maps two platforms to one digest"    "$PY" "$SCRIPT" index "$TMP/index_docker_good.json" --expect "linux/amd64=$A" --expect "linux/arm64=$A"

# --- control: the replaced greps accept what the index check rejects ---------
# A hand-written approximation of the text `docker buildx imagetools inspect`
# prints for a two-platform tag; the arm64 entry is not from this run.
cat > "$TMP/inspect_text.txt" <<EOF
Name:      ghcr.io/example/image:v9.9.9
MediaType: application/vnd.docker.distribution.manifest.list.v2+json
Digest:    $C

Manifests:
  Name:        ghcr.io/example/image:v9.9.9@$A
  MediaType:   application/vnd.docker.distribution.manifest.v2+json
  Platform:    linux/amd64

  Name:        ghcr.io/example/image:v9.9.9@$C
  MediaType:   application/vnd.docker.distribution.manifest.v2+json
  Platform:    linux/arm64
EOF
if grep -q 'linux/amd64' "$TMP/inspect_text.txt" && grep -q 'linux/arm64' "$TMP/inspect_text.txt"; then
  PASS=$((PASS+1))  # the old check is blind to the foreign digest, as expected
else
  FAIL=$((FAIL+1)); echo "FAIL: control fixture no longer exercises the old greps"
fi
expect_fail "control: the index check rejects the same index" "$PY" "$SCRIPT" index "$TMP/index_foreign_digest.json" --expect "linux/amd64=$A" --expect "linux/arm64=$B"

echo "publish-image verify: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
