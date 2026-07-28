#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
LOCK_FILE=${SWITCHVK_LOCK_FILE:-"$SCRIPT_DIR/switchvk.lock"}
DESTINATION=${1:-"$ROOT/.switchvk-sdk"}
VARIANT=${SWITCHVK_VARIANT:-release}

if [[ ! -f "$LOCK_FILE" ]]; then
    echo "ERROR: switchVK configuration not found: $LOCK_FILE" >&2
    exit 1
fi

override_repository=${SWITCHVK_REPOSITORY:-}
# shellcheck disable=SC1090
source "$LOCK_FILE"
SWITCHVK_REPOSITORY=${override_repository:-$SWITCHVK_REPOSITORY}

case "$VARIANT" in
    release|diagnostic) ;;
    *)
        echo "ERROR: SWITCHVK_VARIANT must be release or diagnostic" >&2
        exit 2
        ;;
esac
if [[ ! "$SWITCHVK_REPOSITORY" =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]]; then
    echo "ERROR: unsafe switchVK repository name" >&2
    exit 2
fi

TOKEN=${SWITCHVK_TOKEN:-${GH_TOKEN:-}}
if [[ -z "$TOKEN" ]]; then
    echo "ERROR: SWITCHVK_TOKEN is required to read the private $SWITCHVK_REPOSITORY Release" >&2
    exit 1
fi
if [[ ! "$TOKEN" =~ ^[A-Za-z0-9_.-]+$ ]]; then
    echo "ERROR: SWITCHVK_TOKEN contains unexpected characters" >&2
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
printf 'header = "Authorization: Bearer %s"\n' "$TOKEN" > "$AUTH_CONFIG"
chmod 600 "$AUTH_CONFIG"

curl --config "$AUTH_CONFIG" \
    --fail --location --silent --show-error \
    --header 'Accept: application/vnd.github+json' \
    --header 'X-GitHub-Api-Version: 2022-11-28' \
    --output "$RELEASE_JSON" \
    "$API_ROOT/releases/latest"

python3 - "$RELEASE_JSON" "$VARIANT" "$WORK_DIR" <<'PY'
import json
import pathlib
import re
import sys

release_path, variant, output_dir = sys.argv[1:]
with open(release_path, encoding="utf-8") as stream:
    release = json.load(stream)

tag = release.get("tag_name", "")
if not re.fullmatch(r"[A-Za-z0-9._-]+", tag):
    raise SystemExit("latest switchVK Release has an invalid tag")

assets = release.get("assets", [])
if variant == "diagnostic":
    archives = [a for a in assets if re.fullmatch(r"switchVK-.*-diagnostic\.tar\.xz", a.get("name", ""))]
else:
    archives = [a for a in assets if re.fullmatch(r"switchVK-.*\.tar\.xz", a.get("name", ""))
                and not a.get("name", "").endswith("-diagnostic.tar.xz")]
if len(archives) != 1:
    raise SystemExit(f"expected exactly one {variant} SDK archive in latest Release, found {len(archives)}")

archive = archives[0]
checksum_name = archive["name"] + ".sha256"
checksums = [a for a in assets if a.get("name") == checksum_name]
if len(checksums) != 1:
    raise SystemExit(f"expected exactly one checksum asset named {checksum_name!r}, found {len(checksums)}")

values = {
    "tag": tag,
    "archive-name": archive["name"],
    "archive-url": archive.get("url", ""),
    "checksum-url": checksums[0].get("url", ""),
}
for name, value in values.items():
    if not isinstance(value, str) or not value or "\n" in value or "\r" in value:
        raise SystemExit(f"invalid latest Release field: {name}")
    pathlib.Path(output_dir, name).write_text(value, encoding="utf-8")
PY

SWITCHVK_TAG=$(<"$WORK_DIR/tag")
SWITCHVK_ASSET=$(<"$WORK_DIR/archive-name")
ARCHIVE_URL=$(<"$WORK_DIR/archive-url")
CHECKSUM_URL=$(<"$WORK_DIR/checksum-url")
ARCHIVE="$WORK_DIR/$SWITCHVK_ASSET"
CHECKSUM_FILE="$WORK_DIR/$SWITCHVK_ASSET.sha256"

download_asset() {
    local asset_api_url=$1
    local output_path=$2
    curl --config "$AUTH_CONFIG" \
        --fail --location --silent --show-error \
        --header 'Accept: application/octet-stream' \
        --header 'X-GitHub-Api-Version: 2022-11-28' \
        --output "$output_path" \
        "$asset_api_url"
}

download_asset "$ARCHIVE_URL" "$ARCHIVE"
download_asset "$CHECKSUM_URL" "$CHECKSUM_FILE"

expected_sha256=$(awk -v asset="$SWITCHVK_ASSET" '
    $2 == asset || $2 == "*" asset { print tolower($1); found = 1 }
    END { if (!found) exit 1 }
' "$CHECKSUM_FILE")
if [[ ! "$expected_sha256" =~ ^[0-9a-f]{64}$ ]]; then
    echo "ERROR: invalid SHA-256 sidecar for $SWITCHVK_ASSET" >&2
    exit 1
fi
actual_sha256=$(sha256sum "$ARCHIVE" | awk '{print tolower($1)}')
if [[ "$actual_sha256" != "$expected_sha256" ]]; then
    echo "ERROR: switchVK SDK SHA-256 mismatch" >&2
    echo "expected=$expected_sha256" >&2
    echo "actual=$actual_sha256" >&2
    exit 1
fi

python3 - "$ARCHIVE" "$VARIANT" "$WORK_DIR/archive-root" <<'PY'
import pathlib
import re
import sys
import tarfile

archive, variant, root_file = sys.argv[1:]
with tarfile.open(archive, "r:xz") as tar:
    members = tar.getmembers()
    if not members:
        raise SystemExit("empty switchVK SDK archive")
    roots = set()
    for member in members:
        path = pathlib.PurePosixPath(member.name)
        if path.is_absolute() or ".." in path.parts:
            raise SystemExit(f"unsafe archive path: {member.name}")
        if not path.parts:
            raise SystemExit(f"invalid archive path: {member.name}")
        roots.add(path.parts[0])
        if member.issym() or member.islnk() or member.isdev():
            raise SystemExit(f"unsupported archive member: {member.name}")
    if len(roots) != 1:
        raise SystemExit(f"SDK archive must have one root directory, found {len(roots)}")
    root = roots.pop()
    match = re.fullmatch(r"nvk-switch-[0-9]+\.[0-9]+\.[0-9]+(-diagnostic)?", root)
    if not match or (variant == "diagnostic") != bool(match.group(1)):
        raise SystemExit(f"unexpected {variant} SDK archive root: {root}")
    pathlib.Path(root_file).write_text(root, encoding="utf-8")
PY

SWITCHVK_ROOT_DIRECTORY=$(<"$WORK_DIR/archive-root")
EXTRACTED="$WORK_DIR/extracted"
mkdir "$EXTRACTED"
tar -xJf "$ARCHIVE" -C "$EXTRACTED"
SDK_ROOT="$EXTRACTED/$SWITCHVK_ROOT_DIRECTORY"

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

mv "$EXTRACTED" "$DESTINATION"
SDK_ROOT="$DESTINATION/$SWITCHVK_ROOT_DIRECTORY"

if [[ -n "${GITHUB_ENV:-}" ]]; then
    printf 'SWITCH_NVK_ROOT=%s\n' "$SDK_ROOT" >> "$GITHUB_ENV"
fi
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    printf 'switch_nvk_root=%s\n' "$SDK_ROOT" >> "$GITHUB_OUTPUT"
    printf 'sdk_sha256=%s\n' "$actual_sha256" >> "$GITHUB_OUTPUT"
    printf 'switchvk_tag=%s\n' "$SWITCHVK_TAG" >> "$GITHUB_OUTPUT"
fi

echo "OK: latest switchVK SDK $SWITCHVK_TAG ($VARIANT) installed at $SDK_ROOT"
echo "SDK archive SHA-256: $actual_sha256"
