#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
LOCK_FILE=${SWITCHVK_LOCK_FILE:-"$SCRIPT_DIR/switchvk.lock"}
DESTINATION=${1:-"$ROOT/.switchvk-sdk"}

if [[ ! -f "$LOCK_FILE" ]]; then
    echo "ERROR: switchVK lock file not found: $LOCK_FILE" >&2
    exit 1
fi

# CI may override a lock field for a one-off rebuild, but the repository lock
# remains the default and is what normal push/tag builds consume.
override_repository=${SWITCHVK_REPOSITORY:-}
override_tag=${SWITCHVK_TAG:-}
override_asset=${SWITCHVK_ASSET:-}
override_checksum_asset=${SWITCHVK_CHECKSUM_ASSET:-}
override_root_directory=${SWITCHVK_ROOT_DIRECTORY:-}
override_sha256=${SWITCHVK_SHA256:-}

# The lock file is repository-controlled shell data containing assignments.
# shellcheck disable=SC1090
source "$LOCK_FILE"

SWITCHVK_REPOSITORY=${override_repository:-$SWITCHVK_REPOSITORY}
SWITCHVK_TAG=${override_tag:-$SWITCHVK_TAG}
SWITCHVK_ASSET=${override_asset:-$SWITCHVK_ASSET}
SWITCHVK_CHECKSUM_ASSET=${override_checksum_asset:-$SWITCHVK_CHECKSUM_ASSET}
SWITCHVK_ROOT_DIRECTORY=${override_root_directory:-$SWITCHVK_ROOT_DIRECTORY}
SWITCHVK_SHA256=${override_sha256:-$SWITCHVK_SHA256}

TOKEN=${SWITCHVK_TOKEN:-${GH_TOKEN:-}}
if [[ -z "$TOKEN" ]]; then
    echo "ERROR: SWITCHVK_TOKEN is required to read the private $SWITCHVK_REPOSITORY Release" >&2
    exit 1
fi
if [[ ! "$SWITCHVK_REPOSITORY" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] ||
   [[ ! "$SWITCHVK_TAG" =~ ^[A-Za-z0-9._-]+$ ]] ||
   [[ ! "$SWITCHVK_ASSET" =~ ^[A-Za-z0-9._-]+$ ]] ||
   [[ ! "$SWITCHVK_CHECKSUM_ASSET" =~ ^[A-Za-z0-9._-]+$ ]] ||
   [[ ! "$SWITCHVK_ROOT_DIRECTORY" =~ ^[A-Za-z0-9._-]+$ ]]; then
    echo "ERROR: unsafe value in $LOCK_FILE" >&2
    exit 2
fi

if [[ -e "$DESTINATION" ]]; then
    echo "ERROR: SDK destination already exists: $DESTINATION" >&2
    exit 1
fi
mkdir -p "$(dirname -- "$DESTINATION")"
WORK_DIR=$(mktemp -d "$(dirname -- "$DESTINATION")/.switchvk-download.XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT

GITHUB_API_URL=${GITHUB_API_URL:-https://api.github.com}
API_ROOT="$GITHUB_API_URL/repos/$SWITCHVK_REPOSITORY"
RELEASE_JSON="$WORK_DIR/release.json"
AUTH_CONFIG="$WORK_DIR/curl-auth.conf"
if [[ ! "$TOKEN" =~ ^[A-Za-z0-9_]+$ ]]; then
    echo "ERROR: SWITCHVK_TOKEN contains unexpected characters" >&2
    exit 2
fi
printf 'header = "Authorization: Bearer %s"\n' "$TOKEN" > "$AUTH_CONFIG"
chmod 600 "$AUTH_CONFIG"

curl --config "$AUTH_CONFIG" \
    --fail --location --silent --show-error \
    --header 'Accept: application/vnd.github+json' \
    --header 'X-GitHub-Api-Version: 2022-11-28' \
    --output "$RELEASE_JSON" \
    "$API_ROOT/releases/tags/$SWITCHVK_TAG"

download_asset() {
    local asset_name=$1
    local output_path=$2
    local asset_api_url
    asset_api_url=$(python3 -c '
import json, sys
with open(sys.argv[1], encoding="utf-8") as stream:
    release = json.load(stream)
matches = [asset["url"] for asset in release.get("assets", []) if asset.get("name") == sys.argv[2]]
if len(matches) != 1:
    raise SystemExit(f"expected exactly one Release asset named {sys.argv[2]!r}, found {len(matches)}")
print(matches[0])
' "$RELEASE_JSON" "$asset_name")

    curl --config "$AUTH_CONFIG" \
        --fail --location --silent --show-error \
        --header 'Accept: application/octet-stream' \
        --header 'X-GitHub-Api-Version: 2022-11-28' \
        --output "$output_path" \
        "$asset_api_url"
}

ARCHIVE="$WORK_DIR/$SWITCHVK_ASSET"
download_asset "$SWITCHVK_ASSET" "$ARCHIVE"

expected_sha256=$SWITCHVK_SHA256
if [[ -z "$expected_sha256" ]]; then
    CHECKSUM_FILE="$WORK_DIR/$SWITCHVK_CHECKSUM_ASSET"
    download_asset "$SWITCHVK_CHECKSUM_ASSET" "$CHECKSUM_FILE"
    expected_sha256=$(awk -v asset="$SWITCHVK_ASSET" '
        $2 == asset || $2 == "*" asset { print tolower($1); found = 1 }
        END { if (!found) exit 1 }
    ' "$CHECKSUM_FILE")
fi
expected_sha256=${expected_sha256,,}
if [[ ! "$expected_sha256" =~ ^[0-9a-f]{64}$ ]]; then
    echo "ERROR: invalid expected SHA-256 for $SWITCHVK_ASSET" >&2
    exit 1
fi

actual_sha256=$(sha256sum "$ARCHIVE" | awk '{print tolower($1)}')
if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    echo "ERROR: switchVK SDK SHA-256 mismatch" >&2
    echo "expected=$expected_sha256" >&2
    echo "actual=$actual_sha256" >&2
    exit 1
fi

python3 - "$ARCHIVE" "$SWITCHVK_ROOT_DIRECTORY" <<'PY'
import pathlib
import sys
import tarfile

archive, expected_root = sys.argv[1:]
with tarfile.open(archive, "r:xz") as tar:
    members = tar.getmembers()
    if not members:
        raise SystemExit("empty switchVK SDK archive")
    for member in members:
        path = pathlib.PurePosixPath(member.name)
        if path.is_absolute() or ".." in path.parts:
            raise SystemExit(f"unsafe archive path: {member.name}")
        if not path.parts or path.parts[0] != expected_root:
            raise SystemExit(f"unexpected archive root: {member.name}")
        if member.issym() or member.islnk() or member.isdev():
            raise SystemExit(f"unsupported archive member: {member.name}")
PY

EXTRACTED="$WORK_DIR/extracted"
mkdir "$EXTRACTED"
tar -xJf "$ARCHIVE" -C "$EXTRACTED"
mv "$EXTRACTED" "$DESTINATION"
SDK_ROOT="$DESTINATION/$SWITCHVK_ROOT_DIRECTORY"

if [[ ! -f "$SDK_ROOT/lib/libvulkan.a" ]] ||
   [[ ! -f "$SDK_ROOT/include/vulkan/vulkan.h" ]]; then
    echo "ERROR: downloaded switchVK SDK is incomplete" >&2
    exit 1
fi

if [[ -f "$SDK_ROOT/lib/libvulkan.a.sha256" ]]; then
    expected_library_sha=$(awk 'NR == 1 { print tolower($1) }' "$SDK_ROOT/lib/libvulkan.a.sha256")
    actual_library_sha=$(sha256sum "$SDK_ROOT/lib/libvulkan.a" | awk '{print tolower($1)}')
    if [[ "$expected_library_sha" != "$actual_library_sha" ]]; then
        echo "ERROR: internal libvulkan.a SHA-256 validation failed" >&2
        exit 1
    fi
fi

if [[ -n "${GITHUB_ENV:-}" ]]; then
    printf 'SWITCH_NVK_ROOT=%s\n' "$SDK_ROOT" >> "$GITHUB_ENV"
fi
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    printf 'switch_nvk_root=%s\n' "$SDK_ROOT" >> "$GITHUB_OUTPUT"
    printf 'sdk_sha256=%s\n' "$actual_sha256" >> "$GITHUB_OUTPUT"
fi

echo "OK: switchVK SDK $SWITCHVK_TAG installed at $SDK_ROOT"
echo "SDK archive SHA-256: $actual_sha256"
