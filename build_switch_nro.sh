#!/bin/bash
set -e

export PATH="/usr/bin:/bin:/mingw64/bin:/ucrt64/bin:${PATH:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build_switch"
MESA_NVK_DIR="${MESA_NVK_DIR:-/nvk-build}"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
TOOLCHAIN_FILE="${SCRIPT_DIR}/CMakeModules/SwitchToolchain.cmake"
export PATH="${DEVKITPRO}/tools/bin:${DEVKITPRO}/devkitA64/bin:${PATH}"

BUILD_VARIANT="release"
CLEAN_BUILD=0
for arg in "$@"; do
    case "${arg}" in
        release|diagnostic)
            BUILD_VARIANT="${arg}"
            ;;
        clean)
            CLEAN_BUILD=1
            ;;
        -h|--help)
            echo "Usage: $0 [clean] [release|diagnostic]"
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: ${arg}" >&2
            echo "Usage: $0 [clean] [release|diagnostic]" >&2
            exit 2
            ;;
    esac
done

if [ "${BUILD_VARIANT}" = "diagnostic" ]; then
    OUTPUT_SUFFIX="-diagnostic"
    DIAGNOSTIC_LOGS=ON
else
    OUTPUT_SUFFIX=""
    DIAGNOSTIC_LOGS=OFF
fi

# GenerateSCMRev embeds BUILD_DATE in the ELF. Pin it to the source commit so
# unchanged builds produce stable ELF/NRO hashes instead of the current time.
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    SCRIPT_DIR_NATIVE="$(cygpath -m "${SCRIPT_DIR}" 2>/dev/null || printf '%s' "${SCRIPT_DIR}")"
    SOURCE_DATE_EPOCH="$(git -c safe.directory="${SCRIPT_DIR_NATIVE}" -C "${SCRIPT_DIR}" \
        log -1 --format=%ct HEAD 2>/dev/null || true)"
    if [ -n "${SOURCE_DATE_EPOCH}" ]; then
        export SOURCE_DATE_EPOCH
    fi
fi

if [ -z "${SWITCH_NVK_ROOT:-}" ]; then
    for candidate in \
        "${SCRIPT_DIR}/../switchVK/nvk-switch-25.3.6" \
        "${SCRIPT_DIR}/../switch-nvk/nvk-switch-25.3.6" \
        "/opt/nvk-switch"; do
        if [ -f "${candidate}/lib/libvulkan.a" ]; then
            SWITCH_NVK_ROOT="$(cd "${candidate}" && pwd)"
            break
        fi
    done
fi

if [ "${CLEAN_BUILD}" -eq 1 ]; then
    rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}/.tmp"
export TMPDIR="${BUILD_DIR}/.tmp"
if command -v cygpath >/dev/null 2>&1; then
    export TMP="$(cygpath -w "${TMPDIR}")"
    export TEMP="${TMP}"
else
    export TMP="${TMPDIR}"
    export TEMP="${TMPDIR}"
fi

CMAKE_EXTRA_ARGS=()
if [ -n "${SWITCH_NVK_ROOT:-}" ]; then
    CMAKE_EXTRA_ARGS+=("-DSWITCH_NVK_ROOT=${SWITCH_NVK_ROOT}")
elif [ -n "${SWITCH_VULKAN_LIBRARY:-}" ]; then
    CMAKE_EXTRA_ARGS+=("-DSWITCH_VULKAN_LIBRARY=${SWITCH_VULKAN_LIBRARY}")
elif [ -f "${MESA_NVK_DIR}/src/nouveau/vulkan/libvulkan.a" ]; then
    CMAKE_EXTRA_ARGS+=("-DSWITCH_VULKAN_LIBRARY=${MESA_NVK_DIR}/src/nouveau/vulkan/libvulkan.a")
fi

cmake -G Ninja -B "${BUILD_DIR}" "${SCRIPT_DIR}" \
    -Wno-dev \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSWITCH=ON \
    -DENABLE_QT=OFF \
    -DENABLE_SDL2=OFF \
    -DENABLE_LIBRETRO=OFF \
    -DENABLE_OPENGL=OFF \
    -DENABLE_VULKAN=ON \
    -DENABLE_SOFTWARE_RENDERER=OFF \
    -DENABLE_TESTS=OFF \
    -DENABLE_WEB_SERVICE=OFF \
    -DENABLE_SCRIPTING=OFF \
    -DENABLE_CUBEB=OFF \
    -DENABLE_OPENAL=OFF \
    -DENABLE_LIBUSB=OFF \
    -DENABLE_ROOM=OFF \
    -DENABLE_ROOM_STANDALONE=OFF \
    -DUSE_DISCORD_PRESENCE=OFF \
    -DCITRA_USE_PRECOMPILED_HEADERS=OFF \
    -DCITRA_WARNINGS_AS_ERRORS=OFF \
    -DENABLE_LTO=OFF \
    -DTICO_SWITCH_DIAGNOSTIC_LOGS="${DIAGNOSTIC_LOGS}" \
    "${CMAKE_EXTRA_ARGS[@]}"

cmake --build "${BUILD_DIR}" --target tico -j"$(nproc)"

ELF_FILE="${BUILD_DIR}/src/tico/azahar-switch.elf"
if [ ! -f "${ELF_FILE}" ]; then
    ELF_FILE="$(find "${BUILD_DIR}" -type f \( -name 'azahar-switch.elf' -o -name 'azahar-switch' \) | head -1)"
fi

if [ -z "${ELF_FILE}" ] || [ ! -f "${ELF_FILE}" ]; then
    echo "ERROR: azahar-switch ELF not found"
    exit 1
fi

NACP_FILE="${BUILD_DIR}/GBAStation3DSStub.nacp"
NRO_FILE="${SCRIPT_DIR}/GBAStation3DSStub${OUTPUT_SUFFIX}.nro"
ROMFS_DIR="${BUILD_DIR}/tico_romfs"
DEBUG_ELF="${BUILD_DIR}/azahar-switch${OUTPUT_SUFFIX}.debug.elf"
STRIPPED_ELF="${BUILD_DIR}/azahar-switch${OUTPUT_SUFFIX}.stripped.elf"
GENERATED_LINKER_MAP="${BUILD_DIR}/azahar-switch.map"
LINKER_MAP="${BUILD_DIR}/azahar-switch${OUTPUT_SUFFIX}.map"
AUDIT_LOG="${BUILD_DIR}/M6_LINK_AUDIT${OUTPUT_SUFFIX}.txt"
NM="${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-nm"
STRINGS="${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-strings"
nacptool --create "GBAStation 3DS Stub" "GBAStation" "1.0.5" "${NACP_FILE}"
rm -rf "${ROMFS_DIR}"
mkdir -p "${ROMFS_DIR}"
if [ -d "${SCRIPT_DIR}/src/tico/config" ]; then
    cp -R "${SCRIPT_DIR}/src/tico/config" "${ROMFS_DIR}/config"
fi
if [ -f "${SCRIPT_DIR}/src/tico/fonts/font.ttf" ]; then
    mkdir -p "${ROMFS_DIR}/fonts"
    cp "${SCRIPT_DIR}/src/tico/fonts/font.ttf" "${ROMFS_DIR}/fonts/font.ttf"
fi
cp "${ELF_FILE}" "${DEBUG_ELF}"
cp "${ELF_FILE}" "${STRIPPED_ELF}"
if [ "${GENERATED_LINKER_MAP}" != "${LINKER_MAP}" ]; then
    cp "${GENERATED_LINKER_MAP}" "${LINKER_MAP}"
fi
"${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-strip" --strip-all "${STRIPPED_ELF}"
elf2nro "${STRIPPED_ELF}" "${NRO_FILE}" --nacp="${NACP_FILE}" --romfsdir="${ROMFS_DIR}"

required_symbols=(
    vk_icdGetInstanceProcAddr
    vk_icdNegotiateLoaderICDInterfaceVersion
    wsi_CreateViSurfaceNN
    wsi_CreateSwapchainKHR
    wsi_switch_init_wsi
    nvk_switch_image_layout
    nvkmd_switch_create_dev
    nvkmd_switch_alloc_mem
    nvkmd_switch_binary_sync_type
    nvkmd_switch_sync_get_fence
)

DEFINED_SYMBOLS="${BUILD_DIR}/azahar-switch-defined-symbols${OUTPUT_SUFFIX}.txt"
UNDEFINED_SYMBOLS="${BUILD_DIR}/azahar-switch-undefined-symbols${OUTPUT_SUFFIX}.txt"
"${NM}" --defined-only "${DEBUG_ELF}" > "${DEFINED_SYMBOLS}"
"${NM}" -u "${DEBUG_ELF}" > "${UNDEFINED_SYMBOLS}"

for symbol in "${required_symbols[@]}"; do
    if ! grep -Eq "[[:space:]]${symbol}$" "${DEFINED_SYMBOLS}"; then
        echo "ERROR: final ELF is missing required raw NVK symbol: ${symbol}" >&2
        exit 1
    fi
done

if [ -s "${UNDEFINED_SYMBOLS}" ]; then
    echo "ERROR: final ELF contains unresolved symbols" >&2
    cat "${UNDEFINED_SYMBOLS}" >&2
    exit 1
fi

if grep -Eqi 'drm_shim|drm_nouveau|GLESv2|libglapi|allow-multiple-definition' \
        "${LINKER_MAP}" "${BUILD_DIR}/build.ninja"; then
    echo "ERROR: forbidden DRM/OpenGL/duplicate-definition fallback found in final link" >&2
    exit 1
fi

for identity in 'Mesa 25.3.6' 'NVIDIA Tegra X1 (GM20B)' 'nvkmd/switch'; do
    if ! "${STRINGS}" "${NRO_FILE}" | grep -F "${identity}" >/dev/null; then
        echo "ERROR: final NRO is missing identity string: ${identity}" >&2
        exit 1
    fi
done

{
    echo "M6 static-link audit: PASS"
    echo "Required raw NVK/VI symbols: ${#required_symbols[@]}"
    echo "Undefined ELF symbols: 0"
    echo "Forbidden DRM/OpenGL link dependencies: 0"
    echo "Multiple-definition linker fallback: absent"
    echo "Mesa identity: 25.3.6 raw nvkmd/switch"
} > "${AUDIT_LOG}"

hash_files=(
    "${DEBUG_ELF}"
    "${STRIPPED_ELF}"
    "${LINKER_MAP}"
    "${AUDIT_LOG}"
    "${DEFINED_SYMBOLS}"
    "${UNDEFINED_SYMBOLS}"
    "${NRO_FILE}"
)
if [ -f "${BUILD_DIR}/switch-nvk-kept-entrypoints.txt" ]; then
    hash_files+=("${BUILD_DIR}/switch-nvk-kept-entrypoints.txt")
fi
if [ -n "${SWITCH_NVK_ROOT:-}" ] && [ -f "${SWITCH_NVK_ROOT}/lib/libvulkan.a" ]; then
    hash_files+=("${SWITCH_NVK_ROOT}/lib/libvulkan.a")
fi
SHA256_FILE="${BUILD_DIR}/SHA256SUMS${OUTPUT_SUFFIX}"
sha256sum "${hash_files[@]}" > "${SHA256_FILE}"

if [ -n "${SWITCH_SD_ROOT:-}" ]; then
    mkdir -p "${SWITCH_SD_ROOT}/switch"
    cp "${NRO_FILE}" "${SWITCH_SD_ROOT}/switch/GBAStation3DSStub${OUTPUT_SUFFIX}.nro"
    echo "Installed: ${SWITCH_SD_ROOT}/switch/GBAStation3DSStub${OUTPUT_SUFFIX}.nro"
fi

echo "Variant: ${BUILD_VARIANT} (diagnostic logs: ${DIAGNOSTIC_LOGS})"
echo "Output: ${NRO_FILE}"
echo "Debug ELF: ${DEBUG_ELF}"
echo "Linker map: ${LINKER_MAP}"
echo "M6 audit: ${AUDIT_LOG}"
echo "SHA-256: ${SHA256_FILE}"
echo "Copy to SD as: /switch/GBAStation3DSStub${OUTPUT_SUFFIX}.nro"
echo "ROM fallback path: sdmc:/GBAStation/3ds/3Dlandchs.cci"
echo "Boot markers: sdmc:/123/system/3ds/azahar_boot.txt and sdmc:/123/system/3ds/debug/startup.txt"
