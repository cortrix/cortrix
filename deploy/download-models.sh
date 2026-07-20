#!/bin/bash
set -euo pipefail

MANIFEST="/app/config/model-manifest.tsv"
MODELS_DIR="${1:-/data/models}"
PARTIAL_FILE=""

cleanup_partial() {
    if [ -n "$PARTIAL_FILE" ]; then
        rm -f "$PARTIAL_FILE"
    fi
}
trap cleanup_partial EXIT INT TERM HUP

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo "ERROR: neither sha256sum nor shasum is available" >&2
        return 1
    fi
}

file_size() {
    if stat -c '%s' "$1" >/dev/null 2>&1; then
        stat -c '%s' "$1"
    else
        stat -f '%z' "$1"
    fi
}

verify_file() {
    local path="$1"
    local expected_size="$2"
    local expected_sha="$3"
    local actual_size actual_sha

    [ -f "$path" ] && [ ! -L "$path" ] || return 1
    actual_size="$(file_size "$path")" || return 1
    [ "$actual_size" = "$expected_size" ] || return 1
    actual_sha="$(sha256_file "$path")" || return 1
    [ "$actual_sha" = "$expected_sha" ]
}

validate_destination() {
    case "$1" in
        bge-m3/model.onnx|bge-m3/tokenizer.json|\
        bge-reranker-v2-m3/model.onnx|bge-reranker-v2-m3/tokenizer.json)
            return 0
            ;;
        *)
            echo "ERROR: model manifest contains an unexpected destination: $1" >&2
            return 1
            ;;
    esac
}

download_verified() {
    local component="$1"
    local destination="$2"
    local repository="$3"
    local revision="$4"
    local source_path="$5"
    local expected_size="$6"
    local expected_sha="$7"
    local target target_dir url

    validate_destination "$destination"
    target="$MODELS_DIR/$destination"
    target_dir="$(dirname "$target")"
    url="https://huggingface.co/${repository}/resolve/${revision}/${source_path}?download=true"

    if [ -L "$target" ] || [ -L "$target_dir" ]; then
        echo "ERROR: refusing symlink model target or directory: $destination" >&2
        return 1
    fi
    if verify_file "$target" "$expected_size" "$expected_sha"; then
        echo "[models] verified cached $component"
        return 0
    fi

    mkdir -p "$target_dir"
    PARTIAL_FILE="$(mktemp "${target}.partial.XXXXXX")"
    echo "[models] downloading $component from pinned revision $revision"
    curl --proto '=https' --proto-redir '=https' --tlsv1.2 \
        --fail --location --show-error --progress-bar \
        --retry 5 --retry-delay 2 --retry-all-errors \
        --connect-timeout 20 --max-time 1800 \
        --output "$PARTIAL_FILE" "$url"

    if ! verify_file "$PARTIAL_FILE" "$expected_size" "$expected_sha"; then
        local actual_size actual_sha
        actual_size="$(file_size "$PARTIAL_FILE" 2>/dev/null || printf unknown)"
        actual_sha="$(sha256_file "$PARTIAL_FILE" 2>/dev/null || printf unknown)"
        echo "ERROR: checksum verification failed for $component" >&2
        echo "       expected size=$expected_size sha256=$expected_sha" >&2
        echo "       actual   size=$actual_size sha256=$actual_sha" >&2
        return 1
    fi

    chmod 0644 "$PARTIAL_FILE"
    mv -f "$PARTIAL_FILE" "$target"
    PARTIAL_FILE=""
    echo "[models] installed $component ($expected_size bytes)"
}

self_test() (
    local test_dir good_sha
    test_dir="$(mktemp -d)"
    trap 'rm -rf "$test_dir"' EXIT
    printf 'cortrix-model-verification\n' > "$test_dir/model.bin"
    good_sha="$(sha256_file "$test_dir/model.bin")"
    verify_file "$test_dir/model.bin" 27 "$good_sha" \
        || { echo "ERROR: valid file did not pass verification" >&2; return 1; }
    if verify_file "$test_dir/model.bin" 26 "$good_sha"; then
        echo "ERROR: incorrect size unexpectedly passed verification" >&2
        return 1
    fi
    if verify_file "$test_dir/model.bin" 27 \
        0000000000000000000000000000000000000000000000000000000000000000; then
        echo "ERROR: incorrect checksum unexpectedly passed verification" >&2
        return 1
    fi
    ln -s "$test_dir/model.bin" "$test_dir/model-link.bin"
    if verify_file "$test_dir/model-link.bin" 27 "$good_sha"; then
        echo "ERROR: symlink unexpectedly passed verification" >&2
        return 1
    fi
    echo "Model verification self-test passed"
)

if [ "${1:-}" = "--self-test" ]; then
    self_test
    exit 0
fi

[ -f "$MANIFEST" ] || { echo "ERROR: model manifest is missing: $MANIFEST" >&2; exit 1; }
[ ! -L "$MODELS_DIR" ] || { echo "ERROR: models directory must not be a symlink" >&2; exit 1; }
mkdir -p "$MODELS_DIR"

COUNT=0
while IFS=$'\t' read -r component destination repository revision source_path \
    expected_size expected_sha _upstream_repository _upstream_revision _license; do
    [ "$component" != "component" ] || continue
    [ -n "$component" ] || continue
    download_verified "$component" "$destination" "$repository" "$revision" \
        "$source_path" "$expected_size" "$expected_sha"
    COUNT=$((COUNT + 1))
done < "$MANIFEST"

[ "$COUNT" -eq 4 ] || {
    echo "ERROR: expected 4 pinned model assets, found $COUNT" >&2
    exit 1
}

if [ -w "$MODELS_DIR" ]; then
    MANIFEST_TMP="$(mktemp "$MODELS_DIR/.manifest.XXXXXX")"
    cp "$MANIFEST" "$MANIFEST_TMP"
    chmod 0644 "$MANIFEST_TMP"
    mv -f "$MANIFEST_TMP" "$MODELS_DIR/MANIFEST.tsv"
else
    echo "[models] model directory is read-only; manifest snapshot not written"
fi
echo "[models] all pinned embedding and reranker assets verified"
