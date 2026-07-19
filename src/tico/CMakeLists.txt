add_executable(tico
    emu_window_switch.cpp
    emu_window_switch.h
    main.cpp
    switch_keyboard.cpp
    switch_keyboard.h
    switch_compat.cpp
    switch_libnx.h
    overlay/overlay_ui.cpp
    overlay/overlay_ui.h
    overlay/tico_config.cpp
    overlay/tico_config.h
    overlay/vulkan_overlay.cpp
    overlay/vulkan_overlay.h
    ${PROJECT_SOURCE_DIR}/externals/imgui/imgui/imgui.cpp
    ${PROJECT_SOURCE_DIR}/externals/imgui/imgui/imgui_draw.cpp
    ${PROJECT_SOURCE_DIR}/externals/imgui/imgui/imgui_tables.cpp
    ${PROJECT_SOURCE_DIR}/externals/imgui/imgui/imgui_widgets.cpp
    ${PROJECT_SOURCE_DIR}/externals/imgui/imgui/backends/imgui_impl_vulkan.cpp
)

create_target_directory_groups(tico)
set_target_properties(tico PROPERTIES OUTPUT_NAME "azahar-switch.elf")
target_compile_definitions(tico PRIVATE
    __SWITCH__
    VK_NO_PROTOTYPES
    IMGUI_DISABLE_DEMO_WINDOWS
    IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
)
option(TICO_SWITCH_DIAGNOSTIC_LOGS "Enable first-boot Switch filesystem logs" OFF)
if(TICO_SWITCH_DIAGNOSTIC_LOGS)
    target_compile_definitions(tico PRIVATE TICO_SWITCH_DIAGNOSTIC_LOGS=1)
endif()

target_include_directories(tico PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${PROJECT_SOURCE_DIR}/externals/imgui/imgui
    ${PROJECT_SOURCE_DIR}/externals/imgui/imgui/backends
)

if(NOT DEVKITPRO)
    if(DEFINED ENV{DEVKITPRO})
        set(DEVKITPRO "$ENV{DEVKITPRO}")
    else()
        set(DEVKITPRO "/opt/devkitpro")
    endif()
endif()

link_directories(${DEVKITPRO}/portlibs/switch/lib ${DEVKITPRO}/libnx/lib)

set(SWITCH_NVK_ROOT "$ENV{SWITCH_NVK_ROOT}" CACHE PATH
    "Mesa 25.3.6 raw NVK Switch package root (include/ and lib/libvulkan.a)")
set(SWITCH_VULKAN_LIBRARY "" CACHE FILEPATH
    "Legacy override for a standalone Switch libvulkan.a archive")

if(SWITCH_NVK_ROOT)
    cmake_path(ABSOLUTE_PATH SWITCH_NVK_ROOT NORMALIZE)
    set(SWITCH_VULKAN_ARCHIVE "${SWITCH_NVK_ROOT}/lib/libvulkan.a")
    set(SWITCH_VULKAN_INCLUDE_DIR "${SWITCH_NVK_ROOT}/include")
elseif(SWITCH_VULKAN_LIBRARY)
    cmake_path(ABSOLUTE_PATH SWITCH_VULKAN_LIBRARY NORMALIZE
               OUTPUT_VARIABLE SWITCH_VULKAN_ARCHIVE)
    cmake_path(GET SWITCH_VULKAN_ARCHIVE PARENT_PATH _switch_vulkan_lib_dir)
    cmake_path(GET _switch_vulkan_lib_dir PARENT_PATH _switch_vulkan_prefix)
    set(SWITCH_VULKAN_INCLUDE_DIR "${_switch_vulkan_prefix}/include")
else()
    message(FATAL_ERROR
        "Switch Vulkan frontend requires the Mesa raw NVK package. Set SWITCH_NVK_ROOT.")
endif()

if(NOT EXISTS "${SWITCH_VULKAN_ARCHIVE}")
    message(FATAL_ERROR "Switch raw NVK archive not found: ${SWITCH_VULKAN_ARCHIVE}")
endif()
if(NOT EXISTS "${SWITCH_VULKAN_INCLUDE_DIR}/vulkan/vulkan.h")
    message(FATAL_ERROR "Switch raw NVK Vulkan headers not found: ${SWITCH_VULKAN_INCLUDE_DIR}")
endif()

add_library(switch_nvk_vulkan STATIC IMPORTED GLOBAL)
add_library(SwitchNVK::Vulkan ALIAS switch_nvk_vulkan)
set_target_properties(switch_nvk_vulkan PROPERTIES
    IMPORTED_LOCATION "${SWITCH_VULKAN_ARCHIVE}"
    INTERFACE_INCLUDE_DIRECTORIES "${SWITCH_VULKAN_INCLUDE_DIR}"
)

# Mesa's generated dispatch tables intentionally hold weak references to the
# concrete NVK/common/WSI entrypoints.  That works for a shared ICD, but a
# static archive needs explicit undefined roots or the archive members are not
# extracted.  Derive the exact Mesa 25.3.6 implementation set from this package
# instead of whole-archiving it (the merged archive contains repeated members).
if(NOT CMAKE_NM)
    set(CMAKE_NM "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-gcc-nm")
endif()
execute_process(
    COMMAND "${CMAKE_NM}" --defined-only --extern-only "${SWITCH_VULKAN_ARCHIVE}"
    RESULT_VARIABLE _switch_nvk_nm_result
    OUTPUT_VARIABLE _switch_nvk_nm_output
    ERROR_VARIABLE _switch_nvk_nm_error
)
if(NOT _switch_nvk_nm_result EQUAL 0)
    message(FATAL_ERROR "Failed to inspect raw NVK archive: ${_switch_nvk_nm_error}")
endif()

string(REPLACE "\r\n" "\n" _switch_nvk_nm_output "${_switch_nvk_nm_output}")
string(REPLACE "\n" ";" _switch_nvk_nm_lines "${_switch_nvk_nm_output}")
set(SWITCH_NVK_KEEP_SYMBOLS)
foreach(_line IN LISTS _switch_nvk_nm_lines)
    if(_line MATCHES "[ \t][TDBR][ \t]+((nvk|vk_common|wsi)_[A-Z][A-Za-z0-9_]*)$")
        list(APPEND SWITCH_NVK_KEEP_SYMBOLS "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(APPEND SWITCH_NVK_KEEP_SYMBOLS
    vk_icdGetInstanceProcAddr
    vk_icdNegotiateLoaderICDInterfaceVersion
    nvk_switch_image_layout
    nvkmd_switch_sync_get_fence
    nvkmd_switch_binary_sync_type
    wsi_switch_init_wsi
    wsi_switch_finish_wsi
)
list(REMOVE_DUPLICATES SWITCH_NVK_KEEP_SYMBOLS)
list(SORT SWITCH_NVK_KEEP_SYMBOLS)
list(LENGTH SWITCH_NVK_KEEP_SYMBOLS _switch_nvk_keep_count)
if(_switch_nvk_keep_count LESS 100)
    message(FATAL_ERROR
        "Raw NVK archive exposed only ${_switch_nvk_keep_count} retained entrypoints; package is incomplete")
endif()
string(REPLACE ";" "\n" _switch_nvk_keep_file "${SWITCH_NVK_KEEP_SYMBOLS}")
file(WRITE "${CMAKE_BINARY_DIR}/switch-nvk-kept-entrypoints.txt"
     "${_switch_nvk_keep_file}\n")
message(STATUS
    "Switch raw NVK: ${SWITCH_VULKAN_ARCHIVE} (${_switch_nvk_keep_count} retained symbols)")

target_link_libraries(tico PRIVATE
    citra_common
    citra_core
    audio_core
    video_core
    network
    input_common
    json-headers
    tsl::robin_map
    ${PLATFORM_LIBRARIES}
    Threads::Threads
    vulkan-headers
    SwitchNVK::Vulkan
    z
    zstd
    expat
    nx
    m
)

foreach(_symbol IN LISTS SWITCH_NVK_KEEP_SYMBOLS)
    target_link_options(tico PRIVATE "LINKER:-u,${_symbol}")
endforeach()

set(SWITCH_LINKER_MAP "${CMAKE_BINARY_DIR}/azahar-switch.map" CACHE FILEPATH
    "Linker map for the unstripped Switch executable")
target_link_options(tico PRIVATE
    "LINKER:-Map,${SWITCH_LINKER_MAP}"
    "LINKER:--cref"
)
