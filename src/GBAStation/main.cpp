// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "GBAStation/switch_libnx.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "audio_core/input_details.h"
#include "audio_core/hle/hle.h"
#include "audio_core/sink_details.h"
#include "GBAStation/emu_window_switch.h"
#include "GBAStation/game_db.h"
#include "GBAStation/input_mapping.h"
#include "GBAStation/overlay/overlay_ui.h"
#include "GBAStation/overlay/gbastation_config.h"
#include "GBAStation/overlay/vulkan_overlay.h"
#include "GBAStation/switch_keyboard.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "common/thread.h"
#include "common/zstd_compression.h"
#include "core/arm/dynarmic/arm_dynarmic_diagnostics.h"
#include "core/cheats/cheat_base.h"
#include "core/cheats/cheats.h"
#include "core/core.h"
#include "core/file_sys/cia_container.h"
#include "core/loader/loader.h"
#include "core/loader/smdh.h"
#include "core/memory.h"
#include "core/movie.h"
#include "core/savestate.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/image_interface.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/service/am/am.h"
#include "core/hw/y2r.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/service.h"
#include "input_common/main.h"
#include "input_common/switch_hid.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"

#include <imstb_truetype.h>
#include <lodepng.h>

extern "C" {
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
u32 __nx_exception_ignoredebug = 1;
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
}

extern "C" {
extern const u8 __tdata_lma[];
extern const u8 __tdata_lma_end[];
extern u8 __tls_start[];
extern u8 __tls_end[];
extern size_t __tls_align;
}

namespace {

constexpr const char* SystemDir = "sdmc:/GBAStation/3ds";
constexpr const char* DebugDir = "sdmc:/GBAStation/3ds/debug";
constexpr const char* BootMarkerPath = "sdmc:/GBAStation/3ds/debug/azahar_boot.txt";
constexpr const char* StartupLogPath = "sdmc:/GBAStation/3ds/debug/startup.txt";
constexpr const char* DebugLogPath = "sdmc:/GBAStation/3ds/debug/azahar_switch.txt";
constexpr const char* HeartbeatLogPath = "sdmc:/GBAStation/3ds/debug/heartbeat.txt";
constexpr const char* ExitLogPath = "sdmc:/GBAStation/3ds/debug/exit.txt";
constexpr const char* StdoutLogPath = "sdmc:/GBAStation/3ds/debug/stdout.txt";
constexpr const char* StderrLogPath = "sdmc:/GBAStation/3ds/debug/stderr.txt";
constexpr const char* FallbackRomPath = "sdmc:/GBAStation/3ds/games/3Dlandchs.cci";
constexpr const char* MemMapLogPath = "sdmc:/GBAStation/3ds/debug/memmap.txt";
constexpr const char* LauncherPath = "sdmc:/switch/GBAStation.nro";
constexpr const char* SwitchFastmemEnv = "GBASTATION_SWITCH_FASTMEM";
constexpr const char* SwitchJitFastDispatchEnv = "GBASTATION_SWITCH_JIT_FAST_DISPATCH";

struct LaunchOptions {
    std::string rom_path;
    std::string title;
    SwitchFrontend::GBAStationDisplaySettings display_settings;
    std::string return_nro_path{LauncherPath};
    std::string uninstall_record_path;
    u64 uninstall_title_id{};
    bool return_to_nro{true};
    bool display_settings_from_game_db{};
    bool install_cia_mode{};
    bool uninstall_title_mode{};
};
#if defined(GBASTATION_SWITCH_DIAGNOSTIC_LOGS)
constexpr bool DiagnosticLogsDefaultEnabled = true;
#else
constexpr bool DiagnosticLogsDefaultEnabled = false;
#endif
// Keep memory-map dumping separate because it is substantially heavier than boot logging.
bool EnableStartupLogFile = DiagnosticLogsDefaultEnabled;
bool EnableStdStreamLogs = DiagnosticLogsDefaultEnabled;
bool EnableMemMapLogFile = false;
bool EnableSwitchDebugLogFile = DiagnosticLogsDefaultEnabled;
bool EnableHeartbeatLogFile = DiagnosticLogsDefaultEnabled;
bool EnableExitLogFile = DiagnosticLogsDefaultEnabled;
bool EnableCommonLogFile = DiagnosticLogsDefaultEnabled;
bool EnableBootMarkerFile = DiagnosticLogsDefaultEnabled;
FILE* startup_log{};
FILE* debug_log{};
FILE* heartbeat_log{};
FILE* exit_log{};
auto exit_log_started = std::chrono::steady_clock::now();
bool raw_marker_enabled = true;
PadState pad{};

bool ParseDiagnosticLogEnvValue(const char* value, bool fallback) {
    if (!value || !*value) {
        return fallback;
    }
    std::string text = Common::ToLower(value);
    if (text == "1" || text == "true" || text == "on" || text == "yes") {
        return true;
    }
    if (text == "0" || text == "false" || text == "off" || text == "no") {
        return false;
    }
    return fallback;
}

void ApplyDiagnosticLogSwitchFromEnv() {
    static bool applied = false;
    if (applied) {
        return;
    }
    applied = true;
    const bool enabled = ParseDiagnosticLogEnvValue(std::getenv("GBASTATION_3DS_DIAGNOSTIC_LOGS"),
                                                    DiagnosticLogsDefaultEnabled);
    EnableStartupLogFile = enabled;
    EnableStdStreamLogs = enabled;
    EnableSwitchDebugLogFile = enabled;
    EnableHeartbeatLogFile = enabled;
    EnableExitLogFile = enabled;
    EnableCommonLogFile = enabled;
    EnableBootMarkerFile = enabled;
}

void EnsureSystemDirs() {
    mkdir("sdmc:/GBAStation", 0777);
    mkdir("sdmc:/GBAStation/3ds", 0777);
    mkdir(SystemDir, 0777);
    mkdir(DebugDir, 0777);
}

void WriteRawBootMarker(const char* message) {
    if (!EnableBootMarkerFile) {
        return;
    }

    EnsureSystemDirs();
    const int fd = open(BootMarkerPath, O_CREAT | O_WRONLY | O_APPEND, 0666);
    if (fd < 0) {
        return;
    }

    write(fd, message, std::strlen(message));
    write(fd, "\n", 1);
    close(fd);
}


void LogTlsLayoutEarly(const char* tag) {
    const auto tls_size = static_cast<unsigned long long>(__tls_end - __tls_start);
    const auto tdata_size = static_cast<unsigned long long>(__tdata_lma_end - __tdata_lma);
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "%s: tls_start=%p tls_end=%p tls_size=0x%llx tdata_size=0x%llx align=%zu",
                  tag, __tls_start, __tls_end, tls_size, tdata_size, __tls_align);
    WriteRawBootMarker(buffer);
}

void WriteRawBootMarkerV(const char* prefix, const char* fmt, std::va_list args) {
    char buffer[1024];
    const int prefix_len = std::snprintf(buffer, sizeof(buffer), "%s", prefix);
    if (prefix_len < 0 || static_cast<std::size_t>(prefix_len) >= sizeof(buffer)) {
        return;
    }

    const int body_len = std::vsnprintf(buffer + prefix_len, sizeof(buffer) - prefix_len, fmt, args);
    if (body_len < 0) {
        return;
    }

    const std::size_t used = std::min(sizeof(buffer) - 1,
                                      static_cast<std::size_t>(prefix_len) +
                                          static_cast<std::size_t>(body_len));
    buffer[used] = 0;
    WriteRawBootMarker(buffer);
}

void EnsureDebugDirs() {
    EnsureSystemDirs();
    mkdir(DebugDir, 0777);
}

void WriteLogLine(FILE* file, const char* prefix, const char* fmt, std::va_list args) {
    if (!file) {
        return;
    }

    std::fprintf(file, "%s", prefix);
    std::vfprintf(file, fmt, args);
    std::fprintf(file, "\n");
    std::fflush(file);
}

void OpenStartupLogIfNeeded(const char* mode = "a") {
    if (!EnableStartupLogFile) {
        return;
    }
    if (startup_log) {
        return;
    }

    WriteRawBootMarker("OpenStartupLogIfNeeded: entry");
    EnsureDebugDirs();
    startup_log = std::fopen(StartupLogPath, mode);
    if (startup_log) {
        std::setvbuf(startup_log, nullptr, _IONBF, 0);
        std::fprintf(startup_log, "=== Azahar Switch startup log open (%s) ===\n", mode);
        std::fflush(startup_log);
        WriteRawBootMarker("OpenStartupLogIfNeeded: startup.txt opened");
    } else {
        WriteRawBootMarker("OpenStartupLogIfNeeded: startup.txt fopen failed");
    }
}

void StartupLog(const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    if (startup_log) {
        std::va_list startup_args;
        va_copy(startup_args, args);
        WriteLogLine(startup_log, "[startup] ", fmt, startup_args);
        va_end(startup_args);
    }
    std::va_list raw_args;
    va_copy(raw_args, args);
    WriteRawBootMarkerV("[startup] ", fmt, raw_args);
    va_end(raw_args);
    va_end(args);
}

void CloseStartupLog() {
    if (startup_log) {
        std::fprintf(startup_log, "=== Azahar Switch startup log close ===\n");
        std::fflush(startup_log);
        std::fclose(startup_log);
        startup_log = nullptr;
    }
}

void DebugOpen() {
    OpenStartupLogIfNeeded("a");
    if (!EnableSwitchDebugLogFile) {
        StartupLog("DebugOpen: %s disabled", DebugLogPath);
        return;
    }

    StartupLog("DebugOpen: opening %s", DebugLogPath);
    EnsureDebugDirs();
    debug_log = std::fopen(DebugLogPath, "w");
    if (debug_log) {
        std::setvbuf(debug_log, nullptr, _IONBF, 0);
        std::fprintf(debug_log, "=== Azahar Switch standalone start ===\n");
    } else {
        StartupLog("DebugOpen: failed to open %s", DebugLogPath);
    }
}

void HeartbeatOpen() {
    if (!EnableHeartbeatLogFile || heartbeat_log) {
        return;
    }

    EnsureDebugDirs();
    heartbeat_log = std::fopen(HeartbeatLogPath, "w");
    if (heartbeat_log) {
        std::setvbuf(heartbeat_log, nullptr, _IONBF, 0);
        std::fprintf(heartbeat_log, "=== Azahar Switch heartbeat start ===\n");
    }
}

void HeartbeatClose() {
    if (heartbeat_log) {
        std::fprintf(heartbeat_log, "=== Azahar Switch heartbeat end ===\n");
        std::fclose(heartbeat_log);
        heartbeat_log = nullptr;
    }
}

void HeartbeatLog(const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    WriteLogLine(heartbeat_log, "[heartbeat] ", fmt, args);
    va_end(args);
}

std::array<u32, 4> ReadGuestCodeWords(Core::System& system, u32 sampled_pc) {
    std::array<u32, 4> words{};
    const u32 pc = sampled_pc & ~u32{3};
    if (pc == 0 || !system.IsPoweredOn()) {
        return words;
    }

    std::array<u8, words.size() * sizeof(u32)> bytes{};
    system.Memory().ReadBlock(pc, bytes.data(), bytes.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
        std::memcpy(&words[i], bytes.data() + i * sizeof(u32), sizeof(u32));
    }
    return words;
}

std::array<u32, 16> ReadGuestCodeWindow(Core::System& system, u32 sampled_pc) {
    std::array<u32, 16> words{};
    const u32 pc = sampled_pc & ~u32{3};
    if (pc == 0 || !system.IsPoweredOn()) {
        return words;
    }

    std::array<u8, words.size() * sizeof(u32)> bytes{};
    system.Memory().ReadBlock(pc, bytes.data(), bytes.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
        std::memcpy(&words[i], bytes.data() + i * sizeof(u32), sizeof(u32));
    }
    return words;
}

std::array<u32, 4> ReadGuestCodeWordsAroundReturn(Core::System& system, u32 sampled_lr) {
    const u32 lr = sampled_lr & ~u32{3};
    if (lr < 8) {
        return ReadGuestCodeWords(system, lr);
    }
    return ReadGuestCodeWords(system, lr - 8);
}

void ExitLogOpen(const char* mode = "w") {
    if (!EnableExitLogFile || exit_log) {
        return;
    }

    EnsureDebugDirs();
    exit_log = std::fopen(ExitLogPath, mode);
    exit_log_started = std::chrono::steady_clock::now();
    if (exit_log) {
        std::setvbuf(exit_log, nullptr, _IONBF, 0);
        std::fprintf(exit_log, "=== Azahar Switch exit diagnostics start ===\n");
    }
}

void ExitLogClose() {
    if (exit_log) {
        std::fprintf(exit_log, "=== Azahar Switch exit diagnostics end ===\n");
        std::fflush(exit_log);
        std::fclose(exit_log);
        exit_log = nullptr;
    }
}

void ExitLog(const char* fmt, ...) {
    if (!exit_log) {
        ExitLogOpen("a");
    }
    if (!exit_log) {
        return;
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - exit_log_started)
                                .count();
    std::fprintf(exit_log, "[exit +%lld ms] ", static_cast<long long>(elapsed_ms));
    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(exit_log, fmt, args);
    va_end(args);
    std::fprintf(exit_log, "\n");
    std::fflush(exit_log);
}

void DebugClose() {
    StartupLog("DebugClose");
    ExitLog("DebugClose begin");
    HeartbeatClose();
    if (debug_log) {
        std::fprintf(debug_log, "=== Azahar Switch standalone end ===\n");
        std::fclose(debug_log);
        debug_log = nullptr;
    }
    ExitLog("DebugClose done");
}

void DebugLog(const char* fmt, ...) {
    std::va_list args;
    va_start(args, fmt);
    if (debug_log) {
        std::va_list debug_args;
        va_copy(debug_args, args);
        WriteLogLine(debug_log, "[azahar-switch] ", fmt, debug_args);
        va_end(debug_args);
    }
    if (startup_log) {
        std::va_list startup_args;
        va_copy(startup_args, args);
        WriteLogLine(startup_log, "[azahar-switch] ", fmt, startup_args);
        va_end(startup_args);
    }
    if (raw_marker_enabled) {
        std::va_list raw_args;
        va_copy(raw_args, args);
        WriteRawBootMarkerV("[azahar-switch] ", fmt, raw_args);
        va_end(raw_args);
    }
    va_end(args);
}

bool QueueLauncherReturn(const LaunchOptions& options) {
    ExitLog("QueueLauncherReturn begin return_to_nro=%d target=%s",
            options.return_to_nro ? 1 : 0, options.return_nro_path.c_str());
    if (!options.return_to_nro) {
        StartupLog("QueueLauncherReturn: --exit-to-home requested");
        ExitLog("QueueLauncherReturn skipped: exit-to-home");
        return true;
    }
    if (!envHasNextLoad()) {
        StartupLog("QueueLauncherReturn: loader does not support envSetNextLoad");
        ExitLog("QueueLauncherReturn failed: envHasNextLoad=0");
        return false;
    }

    const char* target = nullptr;
    struct stat st {};
    if (!options.return_nro_path.empty() && stat(options.return_nro_path.c_str(), &st) == 0) {
        target = options.return_nro_path.c_str();
    }

    if (!target) {
        StartupLog("QueueLauncherReturn: no launcher found at %s",
                   options.return_nro_path.c_str());
        ExitLog("QueueLauncherReturn failed: target missing at %s", options.return_nro_path.c_str());
        return false;
    }

    char args[1024];
    std::snprintf(args, sizeof(args), "\"%s\"", target);
    const LibnxResult rc = envSetNextLoad(target, args);
    StartupLog("QueueLauncherReturn: envSetNextLoad target=%s args=%s rc=0x%x", target, args,
               rc);
    ExitLog("QueueLauncherReturn envSetNextLoad target=%s rc=0x%x", target, rc);
    return rc == 0;
}

const char* AppletMessageName(u32 message) {
    switch (message) {
    case AppletMessage_ExitRequest:
        return "ExitRequest";
    case AppletMessage_FocusStateChanged:
        return "FocusStateChanged";
    case AppletMessage_Resume:
        return "Resume";
    case AppletMessage_OperationModeChanged:
        return "OperationModeChanged";
    case AppletMessage_PerformanceModeChanged:
        return "PerformanceModeChanged";
    case AppletMessage_RequestToDisplay:
        return "RequestToDisplay";
    case AppletMessage_CaptureButtonShortPressed:
        return "CaptureButtonShortPressed";
    case AppletMessage_AlbumScreenShotTaken:
        return "AlbumScreenShotTaken";
    case AppletMessage_AlbumRecordingSaved:
        return "AlbumRecordingSaved";
    default:
        return "Unknown";
    }
}

bool PumpAppletMessages() {
    u32 message = 0;
    const LibnxResult rc = appletGetMessage(&message);
    if (rc != 0) {
        return true;
    }

    DebugLog("applet message: %u (%s)", message, AppletMessageName(message));
    const bool keep_running = appletProcessMessage(message);
    if (!keep_running) {
        DebugLog("applet message requested exit: %u (%s)", message, AppletMessageName(message));
    }
    return keep_running;
}

const char* MemTypeName(u32 type) {
    switch (type & 0xFF) {
    case MemType_Unmapped:            return "Unmapped";
    case MemType_Io:                  return "Io";
    case MemType_Normal:              return "Normal";
    case MemType_CodeStatic:          return "CodeStatic";
    case MemType_CodeMutable:         return "CodeMutable";
    case MemType_Heap:                return "Heap";
    case MemType_SharedMem:           return "SharedMem";
    case MemType_WeirdMappedMem:      return "WeirdMapped";
    case MemType_ModuleCodeStatic:    return "ModCodeStatic";
    case MemType_ModuleCodeMutable:   return "ModCodeMutable";
    case MemType_IpcBuffer0:          return "IpcBuffer0";
    case MemType_MappedMemory:        return "MappedMemory";
    case MemType_ThreadLocal:         return "ThreadLocal";
    case MemType_TransferMemIsolated: return "TransferMemIso";
    case MemType_TransferMem:         return "TransferMem";
    case MemType_ProcessMem:          return "ProcessMem";
    case MemType_Reserved:            return "Reserved";
    case MemType_IpcBuffer1:          return "IpcBuffer1";
    case MemType_IpcBuffer3:          return "IpcBuffer3";
    case MemType_KernelStack:         return "KernelStack";
    case MemType_CodeReadOnly:        return "CodeReadOnly";
    case MemType_CodeWritable:        return "CodeWritable";
    default:                          return "Unknown";
    }
}

// After the NRO returns, hbl unmaps our segments and reloads hbmenu into the same
// heap. Any page still Borrowed / IPC-mapped / device-mapped / transfer-mem /
// svcMapMemory'd (leaked thread stack) at that point makes hbl's next kernel memory
// operation fail with 0xD401 (InvalidCurrentMemory) and abort. Dump the address
// space so the subsystem that leaked the mapping can be identified from SD logs.
void DumpMemoryMap(const char* phase) {
    if (!EnableMemMapLogFile) {
        return;
    }

    static bool first_dump = true;
    FILE* map_file = std::fopen(MemMapLogPath, first_dump ? "w" : "a");
    first_dump = false;

    u64 region_count = 0;
    u64 suspect_count = 0;
    u64 suspect_bytes = 0;
    MemoryInfo info{};
    u32 page_info = 0;
    u64 addr = 0;
    for (;;) {
        if (R_FAILED(svcQueryMemory(&info, &page_info, addr))) {
            break;
        }
        const u32 mem_type = info.type & 0xFF;
        if (mem_type != MemType_Unmapped && mem_type != MemType_Reserved) {
            region_count++;
            const bool suspect =
                info.attr != 0 || info.ipc_refcount != 0 || info.device_refcount != 0 ||
                mem_type == MemType_MappedMemory || mem_type == MemType_WeirdMappedMem ||
                mem_type == MemType_TransferMem || mem_type == MemType_TransferMemIsolated ||
                mem_type == MemType_IpcBuffer0 || mem_type == MemType_IpcBuffer1 ||
                mem_type == MemType_IpcBuffer3 || mem_type == MemType_CodeReadOnly ||
                mem_type == MemType_CodeWritable;
            if (suspect) {
                suspect_count++;
                suspect_bytes += info.size;
            }
            if (map_file) {
                std::fprintf(map_file,
                             "[%s] 0x%010llx-0x%010llx %-14s perm=%c%c%c attr=0x%x ipc=%u dev=%u%s\n",
                             phase, static_cast<unsigned long long>(info.addr),
                             static_cast<unsigned long long>(info.addr + info.size),
                             MemTypeName(mem_type), (info.perm & Perm_R) ? 'r' : '-',
                             (info.perm & Perm_W) ? 'w' : '-', (info.perm & Perm_X) ? 'x' : '-',
                             static_cast<unsigned>(info.attr), static_cast<unsigned>(info.ipc_refcount),
                             static_cast<unsigned>(info.device_refcount),
                             suspect ? "  <-- SUSPECT" : "");
            }
        }
        const u64 next = info.addr + info.size;
        if (next <= addr) { // wrapped past the end of the address space
            break;
        }
        addr = next;
    }
    if (map_file) {
        std::fflush(map_file);
        std::fclose(map_file);
    }

    char summary[192];
    std::snprintf(summary, sizeof(summary),
                  "memmap[%s]: regions=%llu suspects=%llu suspect_bytes=0x%llx", phase,
                  static_cast<unsigned long long>(region_count),
                  static_cast<unsigned long long>(suspect_count),
                  static_cast<unsigned long long>(suspect_bytes));
    WriteRawBootMarker(summary);
}

void ExitLogMemorySummary(const char* phase) {
    u64 region_count = 0;
    u64 suspect_count = 0;
    u64 suspect_bytes = 0;
    MemoryInfo info{};
    u32 page_info = 0;
    u64 addr = 0;
    for (;;) {
        if (R_FAILED(svcQueryMemory(&info, &page_info, addr))) {
            break;
        }
        const u32 mem_type = info.type & 0xFF;
        if (mem_type != MemType_Unmapped && mem_type != MemType_Reserved) {
            region_count++;
            const bool suspect =
                info.attr != 0 || info.ipc_refcount != 0 || info.device_refcount != 0 ||
                mem_type == MemType_MappedMemory || mem_type == MemType_WeirdMappedMem ||
                mem_type == MemType_TransferMem || mem_type == MemType_TransferMemIsolated ||
                mem_type == MemType_IpcBuffer0 || mem_type == MemType_IpcBuffer1 ||
                mem_type == MemType_IpcBuffer3 || mem_type == MemType_CodeReadOnly ||
                mem_type == MemType_CodeWritable;
            if (suspect) {
                suspect_count++;
                suspect_bytes += info.size;
            }
        }
        const u64 next = info.addr + info.size;
        if (next <= addr) {
            break;
        }
        addr = next;
    }
    ExitLog("memsummary[%s]: regions=%llu suspects=%llu suspect_bytes=0x%llx", phase,
            static_cast<unsigned long long>(region_count),
            static_cast<unsigned long long>(suspect_count),
            static_cast<unsigned long long>(suspect_bytes));
}

void PinCurrentThreadToCore(s32 core, const char* tag) {
    if (core < 0) {
        core = 0;
    }
    if (core > 2) {
        core = 2;
    }

    const u32 mask = 1u << static_cast<u32>(core);
    const LibnxResult set_rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, mask);
    s32 preferred_core = -1;
    u64 affinity_mask = 0;
    const LibnxResult get_rc = svcGetThreadCoreMask(&preferred_core, &affinity_mask,
                                                    CUR_THREAD_HANDLE);
    DebugLog("thread affinity %s: set core=%d mask=0x%x rc=0x%x get_rc=0x%x preferred=%d affinity=0x%llx",
             tag, core, mask, set_rc, get_rc, preferred_core,
             static_cast<unsigned long long>(affinity_mask));
}

class ThreadCoreMaskGuard {
public:
    ThreadCoreMaskGuard() {
        const LibnxResult rc =
            svcGetThreadCoreMask(&original_preferred_core, &original_affinity_mask,
                                 CUR_THREAD_HANDLE);
        restore_available = rc == 0;
        DebugLog("thread affinity capture: rc=0x%x preferred=%d affinity=0x%llx", rc,
                 original_preferred_core,
                 static_cast<unsigned long long>(original_affinity_mask));
    }

    ~ThreadCoreMaskGuard() {
        Restore("destructor");
    }

    void Restore(const char* tag) {
        if (!restore_available || restored) {
            return;
        }

        // Chainloaded NROs run on hbloader's same process/thread context. Do not hand the
        // emulator a single-core affinity inherited from this core or from the launch path.
        const LibnxResult app_core_rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 0x7);
        restored = app_core_rc == 0;
        if (!restored) {
            const LibnxResult fallback_rc = svcSetThreadCoreMask(
                CUR_THREAD_HANDLE, original_preferred_core, original_affinity_mask);
            restored = fallback_rc == 0;
            DebugLog("thread affinity restore %s: app_core_rc=0x%x fallback preferred=%d "
                     "affinity=0x%llx fallback_rc=0x%x",
                     tag, app_core_rc, original_preferred_core,
                     static_cast<unsigned long long>(original_affinity_mask), fallback_rc);
            ExitLog("thread affinity restore %s: app_core_rc=0x%x fallback preferred=%d "
                    "affinity=0x%llx fallback_rc=0x%x restored=%d",
                    tag, app_core_rc, original_preferred_core,
                    static_cast<unsigned long long>(original_affinity_mask), fallback_rc,
                    restored ? 1 : 0);
            return;
        }

        DebugLog("thread affinity restore %s: preferred=0 affinity=0x7 rc=0x%x", tag,
                 app_core_rc);
        ExitLog("thread affinity restore %s: preferred=0 affinity=0x7 rc=0x%x", tag,
                app_core_rc);
    }

private:
    s32 original_preferred_core = -1;
    u64 original_affinity_mask = 0;
    bool restore_available = false;
    bool restored = false;
};

void InstallFatalHandlers() {
    StartupLog("InstallFatalHandlers");
    std::set_terminate([] {
        DebugLog("std::terminate called");
        StartupLog("std::terminate called");
        ExitLog("std::terminate called");
        std::abort();
    });

    auto signal_handler = [](int sig) {
        DebugLog("fatal signal %d", sig);
        StartupLog("fatal signal %d", sig);
        ExitLog("fatal signal %d", sig);
        _Exit(128 + sig);
    };
    std::signal(SIGABRT, signal_handler);
    std::signal(SIGBUS, signal_handler);
    std::signal(SIGFPE, signal_handler);
    std::signal(SIGILL, signal_handler);
    std::signal(SIGSEGV, signal_handler);
}

const char* ResultStatusName(Core::System::ResultStatus status) {
    switch (status) {
    case Core::System::ResultStatus::Success:
        return "Success";
    case Core::System::ResultStatus::ErrorNotInitialized:
        return "ErrorNotInitialized";
    case Core::System::ResultStatus::ErrorGetLoader:
        return "ErrorGetLoader";
    case Core::System::ResultStatus::ErrorSystemMode:
        return "ErrorSystemMode";
    case Core::System::ResultStatus::ErrorLoader:
        return "ErrorLoader";
    case Core::System::ResultStatus::ErrorLoader_ErrorEncrypted:
        return "ErrorLoader_ErrorEncrypted";
    case Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat:
        return "ErrorLoader_ErrorInvalidFormat";
    case Core::System::ResultStatus::ErrorLoader_ErrorGbaTitle:
        return "ErrorLoader_ErrorGbaTitle";
    case Core::System::ResultStatus::ErrorSystemFiles:
        return "ErrorSystemFiles";
    case Core::System::ResultStatus::ErrorSavestate:
        return "ErrorSavestate";
    case Core::System::ResultStatus::ErrorArticDisconnected:
        return "ErrorArticDisconnected";
    case Core::System::ResultStatus::ErrorN3DSApplication:
        return "ErrorN3DSApplication";
    case Core::System::ResultStatus::ShutdownRequested:
        return "ShutdownRequested";
    case Core::System::ResultStatus::ErrorUnknown:
        return "ErrorUnknown";
    }
    return "Unknown";
}

bool DrainAsyncOperationsForSavestate(Core::System& system) {
    if (!system.KernelRunning() || !system.Kernel().AreAsyncOperationsPending()) {
        return true;
    }

    DebugLog("savestate waiting for pending async operations");
    const auto start = std::chrono::steady_clock::now();
    while (system.Kernel().AreAsyncOperationsPending()) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            DebugLog("savestate async drain timed out");
            return false;
        }

        const Core::System::ResultStatus result = system.RunLoop();
        if (result != Core::System::ResultStatus::Success) {
            DebugLog("savestate async drain failed: %s (%u)", ResultStatusName(result),
                     static_cast<unsigned>(result));
            return false;
        }
    }

    DebugLog("savestate async drain completed");
    return true;
}

bool SaveStateFromMenu(Core::System& system, u32 slot) {
    if (!DrainAsyncOperationsForSavestate(system)) {
        SwitchFrontend::OverlayUI::ShowToast("状态保存失败：系统繁忙");
        return false;
    }

    DebugLog("menu begin direct save state slot=%u", slot);
    try {
        system.SaveState(slot);
        system.frame_limiter.AdvanceFrame();
        DebugLog("menu direct save state completed slot=%u", slot);
        return true;
    } catch (const std::exception& e) {
        DebugLog("menu direct save state failed slot=%u: %s", slot, e.what());
        SwitchFrontend::OverlayUI::ShowToast("状态保存失败");
        return false;
    }
}

bool LoadStateFromMenu(Core::System& system, u32 slot) {
    if (!DrainAsyncOperationsForSavestate(system)) {
        SwitchFrontend::OverlayUI::ShowToast("状态读取失败：系统繁忙");
        return false;
    }

    DebugLog("menu begin direct load state slot=%u", slot);
    try {
        system.LoadState(slot);
        system.frame_limiter.AdvanceFrame();
        DebugLog("menu direct load state completed slot=%u", slot);
        return true;
    } catch (const std::exception& e) {
        DebugLog("menu direct load state failed slot=%u: %s", slot, e.what());
        SwitchFrontend::OverlayUI::ShowToast("状态读取失败");
        return false;
    }
}

bool EndsWithNoCase(std::string_view value, std::string_view suffix) {
    if (value.size() < suffix.size()) {
        return false;
    }
    const std::size_t offset = value.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[offset + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

std::string TitleFromPath(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t begin = slash == std::string_view::npos ? 0 : slash + 1;
    const std::size_t dot = path.find_last_of('.');
    const std::size_t end = dot == std::string_view::npos || dot < begin ? path.size() : dot;
    if (end <= begin) {
        return "Nintendo 3DS";
    }
    return std::string{path.substr(begin, end - begin)};
}

enum class CiaTitleKind {
    Application,
    Demo,
    Update,
    AddOnContent,
    System,
    Other,
};

struct CiaBrowserEntry {
    enum class Type {
        Parent,
        Directory,
        Cia,
    };

    Type type{Type::Cia};
    std::string name;
    std::string path;
    u64 program_id{};
    u16 version{};
    u64 size{};
    CiaTitleKind kind{CiaTitleKind::Other};
    bool compressed{};
    bool readable{};
};

struct CiaInstallState {
    std::atomic_bool active{false};
    std::atomic_bool done{false};
    std::atomic_size_t written{0};
    std::atomic_size_t total{0};
    Service::AM::InstallStatus result{Service::AM::InstallStatus::ErrorInvalid};
    std::string source_path;
    std::string installed_path;
    std::string title;
    std::string message;
    bool success{};
};

struct CiaInstallMetadata {
    std::string title;
    std::vector<u8> icon_rgba;
};

std::vector<u32> DecodeUtf8Text(std::string_view text) {
    std::vector<u32> output;
    for (std::size_t i = 0; i < text.size();) {
        const u8 c = static_cast<u8>(text[i]);
        u32 codepoint = c;
        std::size_t length = 1;
        if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            codepoint = ((c & 0x1F) << 6) | (static_cast<u8>(text[i + 1]) & 0x3F);
            length = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            codepoint = ((c & 0x0F) << 12) |
                        ((static_cast<u8>(text[i + 1]) & 0x3F) << 6) |
                        (static_cast<u8>(text[i + 2]) & 0x3F);
            length = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
            codepoint = ((c & 0x07) << 18) |
                        ((static_cast<u8>(text[i + 1]) & 0x3F) << 12) |
                        ((static_cast<u8>(text[i + 2]) & 0x3F) << 6) |
                        (static_cast<u8>(text[i + 3]) & 0x3F);
            length = 4;
        }
        output.push_back(codepoint);
        i += length;
    }
    return output;
}

std::string CleanSmdhTitle(const std::array<char16_t, 0x80>& raw) {
    std::size_t length = 0;
    while (length < raw.size() && raw[length] != 0) {
        ++length;
    }
    std::string title = Common::UTF16ToUTF8(std::u16string{raw.data(), length});
    for (char& ch : title) {
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            ch = ' ';
        }
    }
    while (!title.empty() && title.back() == ' ') {
        title.pop_back();
    }
    return title;
}

std::string StemFromPath(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t begin = slash == std::string_view::npos ? 0 : slash + 1;
    const std::size_t dot = path.find_last_of('.');
    const std::size_t end = dot == std::string_view::npos || dot < begin ? path.size() : dot;
    return end <= begin ? std::string{"Nintendo 3DS"} : std::string{path.substr(begin, end - begin)};
}

std::string TitleIdString(u64 program_id) {
    char text[17]{};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(program_id));
    return text;
}

std::string InstalledTitleSavePath(u64 program_id) {
    return "sdmc:/GBAStation/saves/3DS/" + TitleIdString(program_id);
}

bool ParseTitleId(std::string_view text, u64& out) {
    if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0) {
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 16) {
        return false;
    }
    u64 value = 0;
    for (const char c : text) {
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= static_cast<u64>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<u64>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= static_cast<u64>(c - 'A' + 10);
        } else {
            return false;
        }
    }
    out = value;
    return true;
}

u32 Rgb565ToRgba8888(u16 color) {
    const u8 r = static_cast<u8>((((color >> 11) & 0x1F) << 3) | (((color >> 11) & 0x1F) >> 2));
    const u8 g = static_cast<u8>((((color >> 5) & 0x3F) << 2) | (((color >> 5) & 0x3F) >> 4));
    const u8 b = static_cast<u8>(((color & 0x1F) << 3) | ((color & 0x1F) >> 2));
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
           (0xFFu << 24);
}

CiaTitleKind ClassifyCiaTitle(u64 program_id) {
    constexpr u64 high_mask = 0xFFFFFFFF00000000ULL;
    switch (program_id & high_mask) {
    case 0x0004000000000000ULL:
        return CiaTitleKind::Application;
    case 0x0004000200000000ULL:
        return CiaTitleKind::Demo;
    case 0x0004000E00000000ULL:
        return CiaTitleKind::Update;
    case 0x0004008C00000000ULL:
        return CiaTitleKind::AddOnContent;
    default:
        break;
    }
    return Service::AM::GetTitleMediaType(program_id) == Service::FS::MediaType::NAND
               ? CiaTitleKind::System
               : CiaTitleKind::Other;
}

const char* CiaTitleKindName(CiaTitleKind kind) {
    switch (kind) {
    case CiaTitleKind::Application:
        return "游戏";
    case CiaTitleKind::Demo:
        return "试玩";
    case CiaTitleKind::Update:
        return "更新";
    case CiaTitleKind::AddOnContent:
        return "DLC";
    case CiaTitleKind::System:
        return "系统";
    default:
        return "其他";
    }
}

bool ShouldAddCiaToGameDb(CiaTitleKind kind) {
    return kind == CiaTitleKind::Application || kind == CiaTitleKind::Demo;
}

std::string FormatTitleVersion(u16 version) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "v%u.%u.%u (%u)", (version >> 10) & 0x3F,
                  (version >> 4) & 0x3F, version & 0xF, version);
    return buffer;
}

std::string FormatBytes(u64 size) {
    char buffer[64]{};
    if (size >= 1024ULL * 1024ULL * 1024ULL) {
        std::snprintf(buffer, sizeof(buffer), "%.1f GB",
                      static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0));
    } else if (size >= 1024ULL * 1024ULL) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                      static_cast<double>(size) / (1024.0 * 1024.0));
    } else if (size >= 1024ULL) {
        std::snprintf(buffer, sizeof(buffer), "%.1f KB",
                      static_cast<double>(size) / 1024.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B",
                      static_cast<unsigned long long>(size));
    }
    return buffer;
}

std::string ParentDirectory(std::string directory) {
    if (directory.empty() || directory == "sdmc:/") {
        return {};
    }
    while (directory.size() > 6 && directory.back() == '/') {
        directory.pop_back();
    }
    const std::size_t slash = directory.find_last_of('/');
    if (slash == std::string::npos || slash <= 5) {
        return "sdmc:/";
    }
    return directory.substr(0, slash + 1);
}

bool IsDirectoryPath(const std::string& path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool EndsWithCiaExtension(std::string_view name) {
    return EndsWithNoCase(name, ".cia") || EndsWithNoCase(name, ".zcia");
}

bool ReadCiaEntry(const std::string& path, CiaBrowserEntry& entry) {
    std::unique_ptr<FileUtil::IOFile> file = std::make_unique<FileUtil::IOFile>(path, "rb");
    if (!file->IsOpen()) {
        return false;
    }
    entry.compressed = FileUtil::Z3DSReadIOFile::GetUnderlyingFileMagic(file.get()) !=
                       std::nullopt;
    if (entry.compressed) {
        file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(file));
    }

    std::vector<u8> header(FileSys::CIA_HEADER_SIZE);
    if (file->ReadBytes(header.data(), header.size()) != header.size()) {
        return false;
    }
    FileSys::CIAContainer container;
    if (container.LoadHeader(header) != Loader::ResultStatus::Success) {
        return false;
    }

    std::vector<u8> tmd(container.GetTitleMetadataSize());
    if (file->ReadAtBytes(tmd.data(), tmd.size(), container.GetTitleMetadataOffset()) !=
            tmd.size() ||
        container.LoadTitleMetadata(tmd) != Loader::ResultStatus::Success) {
        return false;
    }

    entry.program_id = container.GetTitleMetadata().GetTitleID();
    entry.version = container.GetTitleMetadata().GetTitleVersion();
    entry.kind = ClassifyCiaTitle(entry.program_id);
    return true;
}

CiaInstallMetadata ReadCiaMetadata(const std::string& path, const std::string& fallback) {
    CiaInstallMetadata metadata;
    metadata.title = fallback;
    std::unique_ptr<FileUtil::IOFile> file = std::make_unique<FileUtil::IOFile>(path, "rb");
    if (!file->IsOpen()) {
        return metadata;
    }
    if (FileUtil::Z3DSReadIOFile::GetUnderlyingFileMagic(file.get()) != std::nullopt) {
        file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(file));
    }
    FileSys::CIAContainer container;
    if (container.Load(file.get()) != Loader::ResultStatus::Success || !container.GetSMDH()) {
        return metadata;
    }
    const Loader::SMDH& smdh = *container.GetSMDH();
    const auto try_language = [&smdh](Loader::SMDH::TitleLanguage language) {
        return CleanSmdhTitle(smdh.GetLongTitle(language));
    };
    std::string title = try_language(Loader::SMDH::TitleLanguage::SimplifiedChinese);
    if (title.empty()) {
        title = try_language(Loader::SMDH::TitleLanguage::English);
    }
    if (title.empty()) {
        title = try_language(Loader::SMDH::TitleLanguage::Japanese);
    }
    if (!title.empty()) {
        metadata.title = title;
    }
    const std::vector<u16> icon = smdh.GetIcon(true);
    if (icon.size() == 48 * 48) {
        metadata.icon_rgba.resize(icon.size() * 4);
        for (std::size_t i = 0; i < icon.size(); ++i) {
            const u32 rgba = Rgb565ToRgba8888(icon[i]);
            metadata.icon_rgba[i * 4 + 0] = static_cast<u8>(rgba & 0xFF);
            metadata.icon_rgba[i * 4 + 1] = static_cast<u8>((rgba >> 8) & 0xFF);
            metadata.icon_rgba[i * 4 + 2] = static_cast<u8>((rgba >> 16) & 0xFF);
            metadata.icon_rgba[i * 4 + 3] = 0xFF;
        }
    }
    return metadata;
}

std::string SaveCiaIconPng(const std::string& save_path, const std::vector<u8>& icon_rgba) {
    if (icon_rgba.size() != 48 * 48 * 4) {
        return {};
    }
    mkdir("sdmc:/GBAStation/saves", 0777);
    mkdir("sdmc:/GBAStation/saves/3DS", 0777);
    mkdir(save_path.c_str(), 0777);
    const std::string icon_path = save_path + "/icon.png";
    const unsigned error = lodepng::encode(icon_path, icon_rgba, 48, 48);
    if (error != 0) {
        DebugLog("CIA icon png encode failed path=%s error=%u", icon_path.c_str(), error);
        return {};
    }
    return icon_path;
}

std::vector<CiaBrowserEntry> ListCiaInstallEntries(const std::string& directory) {
    std::vector<CiaBrowserEntry> dirs;
    std::vector<CiaBrowserEntry> cias;
    if (!ParentDirectory(directory).empty()) {
        dirs.push_back({CiaBrowserEntry::Type::Parent, "返回上一级", ParentDirectory(directory)});
    }

    DIR* dir = opendir(directory.c_str());
    if (!dir) {
        return dirs;
    }
    while (dirent* ent = readdir(dir)) {
        const char* name = ent->d_name;
        if (!name || std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
            continue;
        }
        const std::string path = directory + name;
        if (IsDirectoryPath(path)) {
            dirs.push_back({CiaBrowserEntry::Type::Directory, std::string{name} + "/", path + "/"});
            continue;
        }
        if (!EndsWithCiaExtension(name)) {
            continue;
        }
        CiaBrowserEntry entry;
        entry.type = CiaBrowserEntry::Type::Cia;
        entry.name = name;
        entry.path = path;
        struct stat st {};
        if (stat(path.c_str(), &st) == 0) {
            entry.size = static_cast<u64>(st.st_size);
        }
        entry.readable = ReadCiaEntry(path, entry);
        cias.push_back(std::move(entry));
    }
    closedir(dir);

    const auto by_name = [](const CiaBrowserEntry& a, const CiaBrowserEntry& b) {
        return Common::ToLower(a.name) < Common::ToLower(b.name);
    };
    const bool has_parent = !dirs.empty() && dirs.front().type == CiaBrowserEntry::Type::Parent;
    std::sort(dirs.begin() + (has_parent ? 1 : 0), dirs.end(), by_name);
    std::sort(cias.begin(), cias.end(), by_name);
    dirs.insert(dirs.end(), cias.begin(), cias.end());
    return dirs;
}

const char* InstallStatusText(Service::AM::InstallStatus status) {
    switch (status) {
    case Service::AM::InstallStatus::Success:
        return "安装完成";
    case Service::AM::InstallStatus::ErrorFileNotFound:
        return "文件不存在";
    case Service::AM::InstallStatus::ErrorFailedToOpenFile:
        return "无法打开文件";
    case Service::AM::InstallStatus::ErrorAborted:
        return "安装被中断";
    case Service::AM::InstallStatus::ErrorEncrypted:
        return "CIA已加密，请先解密或补充aes_keys.txt";
    default:
        return "不是有效的CIA文件";
    }
}

struct Rgba {
    u8 r;
    u8 g;
    u8 b;
    u8 a;
};

u32 PackColor(Rgba c) {
    return static_cast<u32>(c.r) | (static_cast<u32>(c.g) << 8) |
           (static_cast<u32>(c.b) << 16) | (static_cast<u32>(c.a) << 24);
}

class CiaInstallerFont {
public:
    bool Initialize() {
        if (plInitialize(PlServiceType_User) != 0) {
            return false;
        }
        PlFontData shared{};
        LibnxResult rc = plGetSharedFontByType(&shared, PlSharedFontType_ChineseSimplified);
        if (rc != 0 || !shared.address || shared.size == 0) {
            rc = plGetSharedFontByType(&shared, PlSharedFontType_ExtChineseSimplified);
        }
        if (rc != 0 || !shared.address || shared.size == 0) {
            rc = plGetSharedFontByType(&shared, PlSharedFontType_Standard);
        }
        if (rc != 0 || !shared.address || shared.size == 0) {
            plExit();
            return false;
        }
        font_data.resize(shared.size);
        std::memcpy(font_data.data(), shared.address, shared.size);
        plExit();
        const int offset = stbtt_GetFontOffsetForIndex(font_data.data(), 0);
        ready = offset >= 0 && stbtt_InitFont(&font, font_data.data(), offset) != 0;
        return ready;
    }

    int Measure(std::string_view text, int size) {
        if (!ready) {
            return static_cast<int>(DecodeUtf8Text(text).size()) * size / 2;
        }
        const float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(size));
        int width = 0;
        int previous = 0;
        for (const u32 cp : DecodeUtf8Text(text)) {
            const int glyph = stbtt_FindGlyphIndex(&font, static_cast<int>(cp));
            const int codepoint = glyph == 0 ? '?' : static_cast<int>(cp);
            int advance = 0;
            int lsb = 0;
            stbtt_GetCodepointHMetrics(&font, codepoint, &advance, &lsb);
            if (previous) {
                width += static_cast<int>(stbtt_GetCodepointKernAdvance(&font, previous,
                                                                        codepoint) *
                                          scale);
            }
            width += static_cast<int>(advance * scale);
            previous = codepoint;
        }
        return width;
    }

    std::string Truncate(std::string_view text, int size, int max_width) {
        if (Measure(text, size) <= max_width) {
            return std::string{text};
        }
        std::string out;
        for (std::size_t i = 0; i < text.size();) {
            const std::size_t begin = i;
            const u8 c = static_cast<u8>(text[i]);
            if ((c & 0xE0) == 0xC0) {
                i += 2;
            } else if ((c & 0xF0) == 0xE0) {
                i += 3;
            } else if ((c & 0xF8) == 0xF0) {
                i += 4;
            } else {
                i += 1;
            }
            if (i > text.size()) {
                break;
            }
            std::string candidate = out + std::string{text.substr(begin, i - begin)} + "...";
            if (Measure(candidate, size) > max_width) {
                return out.empty() ? std::string{"..."} : out + "...";
            }
            out.append(text.substr(begin, i - begin));
        }
        return out;
    }

    void Draw(u32* pixels, u32 stride, int x, int baseline, std::string_view text, int size,
              Rgba color) {
        if (!ready) {
            return;
        }
        const float scale = stbtt_ScaleForPixelHeight(&font, static_cast<float>(size));
        int pen_x = x;
        int previous = 0;
        for (const u32 cp : DecodeUtf8Text(text)) {
            const int glyph = stbtt_FindGlyphIndex(&font, static_cast<int>(cp));
            const int codepoint = glyph == 0 ? '?' : static_cast<int>(cp);
            if (previous) {
                pen_x += static_cast<int>(stbtt_GetCodepointKernAdvance(&font, previous,
                                                                        codepoint) *
                                          scale);
            }
            int width = 0;
            int height = 0;
            int xoff = 0;
            int yoff = 0;
            u8* bitmap = stbtt_GetCodepointBitmap(&font, 0, scale, codepoint, &width, &height,
                                                  &xoff, &yoff);
            for (int yy = 0; yy < height; ++yy) {
                const int py = baseline + yoff + yy;
                if (py < 0 || py >= 720) {
                    continue;
                }
                for (int xx = 0; xx < width; ++xx) {
                    const int px = pen_x + xoff + xx;
                    if (px < 0 || px >= 1280) {
                        continue;
                    }
                    const u8 alpha = bitmap[yy * width + xx];
                    if (alpha == 0) {
                        continue;
                    }
                    u8* dst = reinterpret_cast<u8*>(&pixels[py * stride + px]);
                    const int a = static_cast<int>(alpha) * color.a / 255;
                    dst[0] = static_cast<u8>((color.r * a + dst[0] * (255 - a)) / 255);
                    dst[1] = static_cast<u8>((color.g * a + dst[1] * (255 - a)) / 255);
                    dst[2] = static_cast<u8>((color.b * a + dst[2] * (255 - a)) / 255);
                    dst[3] = 255;
                }
            }
            if (bitmap) {
                stbtt_FreeBitmap(bitmap, nullptr);
            }
            int advance = 0;
            int lsb = 0;
            stbtt_GetCodepointHMetrics(&font, codepoint, &advance, &lsb);
            pen_x += static_cast<int>(advance * scale);
            previous = codepoint;
        }
    }

private:
    std::vector<u8> font_data;
    stbtt_fontinfo font{};
    bool ready{};
};

class CiaInstallerCanvas {
public:
    explicit CiaInstallerCanvas(CiaInstallerFont& font_) : font(font_) {}

    bool Initialize() {
        if (framebufferCreate(&fb, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, 2) !=
            0) {
            return false;
        }
        if (framebufferMakeLinear(&fb) != 0) {
            framebufferClose(&fb);
            return false;
        }
        ready = true;
        return true;
    }

    ~CiaInstallerCanvas() {
        if (ready) {
            framebufferClose(&fb);
        }
    }

    void Begin() {
        u32 stride_bytes = 0;
        pixels = static_cast<u32*>(framebufferBegin(&fb, &stride_bytes));
        stride = stride_bytes / sizeof(u32);
        FillRect(0, 0, 1280, 720, {13, 16, 22, 255});
    }

    void End() {
        framebufferEnd(&fb);
    }

    void FillRect(int x, int y, int w, int h, Rgba color) {
        const int x0 = std::clamp(x, 0, 1280);
        const int y0 = std::clamp(y, 0, 720);
        const int x1 = std::clamp(x + w, 0, 1280);
        const int y1 = std::clamp(y + h, 0, 720);
        const u32 packed = PackColor(color);
        for (int yy = y0; yy < y1; ++yy) {
            for (int xx = x0; xx < x1; ++xx) {
                pixels[yy * stride + xx] = packed;
            }
        }
    }

    void Border(int x, int y, int w, int h, Rgba color) {
        FillRect(x, y, w, 1, color);
        FillRect(x, y + h - 1, w, 1, color);
        FillRect(x, y, 1, h, color);
        FillRect(x + w - 1, y, 1, h, color);
    }

    void Text(int x, int baseline, std::string_view text, int size, Rgba color) {
        font.Draw(pixels, stride, x, baseline, text, size, color);
    }

    int Measure(std::string_view text, int size) {
        return font.Measure(text, size);
    }

    std::string Truncate(std::string_view text, int size, int max_width) {
        return font.Truncate(text, size, max_width);
    }

private:
    CiaInstallerFont& font;
    Framebuffer fb{};
    u32* pixels{};
    u32 stride{};
    bool ready{};
};

void DrawCiaInstaller(CiaInstallerCanvas& canvas, const std::string& directory,
                      const std::vector<CiaBrowserEntry>& entries, int selected, int scroll,
                      bool delete_source, const CiaInstallState& install_state,
                      const std::string& notice) {
    constexpr Rgba text{238, 243, 250, 255};
    constexpr Rgba dim{157, 168, 184, 255};
    constexpr Rgba accent{74, 170, 255, 255};
    constexpr Rgba panel{26, 31, 42, 245};
    constexpr Rgba row{38, 46, 61, 255};
    constexpr Rgba error{255, 102, 126, 255};

    canvas.Begin();
    canvas.FillRect(0, 0, 1280, 90, {17, 22, 31, 255});
    canvas.Text(44, 58, "CIA安装", 34, text);
    const std::string toggle = std::string{"安装完成删除源文件 "} + (delete_source ? "开" : "关");
    canvas.Text(1280 - 44 - canvas.Measure(toggle, 22), 56, toggle, 22,
                delete_source ? accent : dim);
    canvas.Text(44, 116, canvas.Truncate(directory, 20, 960), 20, accent);
    canvas.Border(34, 130, 1212, 506, {69, 81, 101, 255});
    canvas.FillRect(35, 131, 1210, 504, panel);

    if (entries.empty()) {
        canvas.Text(74, 190, "当前目录没有CIA/zCIA文件或子目录", 24, dim);
    }

    constexpr int row_h = 48;
    constexpr int rows = 10;
    for (int i = scroll; i < std::min<int>(entries.size(), scroll + rows); ++i) {
        const int y = 150 + (i - scroll) * row_h;
        const auto& entry = entries[i];
        if (i == selected) {
            canvas.FillRect(54, y, 1172, row_h - 6, row);
            canvas.FillRect(54, y + 8, 5, row_h - 22, accent);
        }
        Rgba name_color = entry.readable || entry.type != CiaBrowserEntry::Type::Cia ? text : error;
        std::string name = entry.name;
        if (entry.type == CiaBrowserEntry::Type::Parent) {
            name = "..  返回上一级";
        }
        canvas.Text(78, y + 30, canvas.Truncate(name, 21, 640), 21, name_color);
        if (entry.type == CiaBrowserEntry::Type::Directory) {
            canvas.Text(1080, y + 30, "目录", 18, dim);
        } else if (entry.type == CiaBrowserEntry::Type::Cia) {
            const std::string badge = entry.readable ? CiaTitleKindName(entry.kind) : "无效";
            canvas.Text(790, y + 30, FormatBytes(entry.size), 18, dim);
            if (entry.readable) {
                canvas.Text(900, y + 30, FormatTitleVersion(entry.version), 18, dim);
            }
            canvas.Text(1110, y + 30, badge, 18, entry.readable ? accent : error);
        }
    }

    if (!notice.empty()) {
        canvas.Text(54, 674, canvas.Truncate(notice, 20, 780), 20, accent);
    }
    canvas.Text(760, 674, "A 确认   B 返回上级   X 删除源文件   + 返回主程序", 19, dim);

    if (install_state.active.load(std::memory_order_acquire)) {
        const std::size_t written = install_state.written.load(std::memory_order_acquire);
        const std::size_t total = install_state.total.load(std::memory_order_acquire);
        canvas.FillRect(0, 0, 1280, 720, {0, 0, 0, 150});
        canvas.FillRect(360, 278, 560, 150, {28, 34, 45, 255});
        canvas.Border(360, 278, 560, 150, {80, 96, 120, 255});
        canvas.Text(390, 326, canvas.Truncate("正在安装 " + install_state.title, 22, 500), 22,
                    text);
        canvas.FillRect(390, 350, 500, 12, {52, 62, 78, 255});
        const int fill = total == 0 ? 0 : static_cast<int>(500ULL * written / total);
        canvas.FillRect(390, 350, std::clamp(fill, 0, 500), 12, accent);
        canvas.Text(390, 396, FormatBytes(written) + " / " + FormatBytes(total), 19, dim);
    }
    canvas.End();
}

void DrawCiaMessage(CiaInstallerCanvas& canvas, const std::string& title,
                    const std::string& message) {
    canvas.Begin();
    canvas.FillRect(0, 0, 1280, 720, {13, 16, 22, 255});
    canvas.FillRect(320, 250, 640, 210, {28, 34, 45, 255});
    canvas.Border(320, 250, 640, 210, {80, 96, 120, 255});
    canvas.Text(360, 310, title, 30, {238, 243, 250, 255});
    canvas.Text(360, 360, canvas.Truncate(message, 22, 560), 22, {172, 184, 200, 255});
    canvas.Text(360, 420, "按 A 或 B 返回文件列表", 20, {74, 170, 255, 255});
    canvas.End();
}

int RunCiaInstaller(const LaunchOptions& options) {
    DebugLog("CIA installer branch enter return=%s", options.return_nro_path.c_str());
    CiaInstallerFont font;
    if (!font.Initialize()) {
        DebugLog("CIA installer font init failed");
    }
    CiaInstallerCanvas canvas(font);
    if (!canvas.Initialize()) {
        DebugLog("CIA installer framebuffer init failed");
        return EXIT_FAILURE;
    }

    std::string directory = "sdmc:/";
    std::vector<CiaBrowserEntry> entries = ListCiaInstallEntries(directory);
    int selected = 0;
    int scroll = 0;
    bool delete_source = false;
    std::string notice;
    CiaInstallState install_state;
    std::thread install_thread;

    const auto join_install_thread = [&](const char* reason) {
        if (!install_thread.joinable()) {
            return;
        }
        DebugLog("CIA installer joining install thread reason=%s active=%d done=%d", reason,
                 install_state.active.load(std::memory_order_acquire) ? 1 : 0,
                 install_state.done.load(std::memory_order_acquire) ? 1 : 0);
        install_thread.join();
        DebugLog("CIA installer install thread joined reason=%s", reason);
    };

    const auto refresh = [&] {
        entries = ListCiaInstallEntries(directory);
        selected = std::clamp(selected, 0, std::max(0, static_cast<int>(entries.size()) - 1));
        scroll = std::clamp(scroll, 0, std::max(0, selected));
    };

    const auto start_install = [&](const CiaBrowserEntry& entry) {
        if (!entry.readable) {
            notice = entry.name + ": 不是有效的CIA";
            return;
        }
        if (install_thread.joinable()) {
            join_install_thread("start-new-install");
        }
        install_state.source_path = entry.path;
        CiaInstallMetadata metadata = ReadCiaMetadata(entry.path, StemFromPath(entry.path));
        install_state.title = metadata.title;
        install_state.installed_path.clear();
        install_state.message.clear();
        install_state.success = false;
        install_state.result = Service::AM::InstallStatus::ErrorInvalid;
        install_state.total = static_cast<std::size_t>(entry.size);
        install_state.written = 0;
        install_state.done = false;
        install_state.active = true;
        notice.clear();
        const bool delete_after_install = delete_source;
        install_thread = std::thread([&, entry, metadata = std::move(metadata), delete_after_install] {
            DebugLog("CIA install begin path=%s tid=%016llx", entry.path.c_str(),
                     static_cast<unsigned long long>(entry.program_id));
            install_state.result = Service::AM::InstallCIA(
                entry.path, [&](std::size_t written, std::size_t total) {
                    install_state.written = written;
                    install_state.total = total;
                },
                true);
            const bool installed_ok =
                install_state.result == Service::AM::InstallStatus::Success;
            install_state.success = installed_ok;
            if (installed_ok) {
                if (ShouldAddCiaToGameDb(entry.kind)) {
                    const Service::FS::MediaType media =
                        Service::AM::GetTitleMediaType(entry.program_id);
                    install_state.installed_path =
                        Service::AM::GetTitleContentPath(media, entry.program_id);
                    const std::string save_path = InstalledTitleSavePath(entry.program_id);
                    const std::string logo_path = SaveCiaIconPng(save_path, metadata.icon_rgba);
                    install_state.success =
                        !install_state.installed_path.empty() &&
                        SwitchFrontend::GameDatabase::SaveInstalledGameRecord(
                            install_state.installed_path, install_state.title,
                            options.display_settings, logo_path, save_path);
                    install_state.message = install_state.success
                                                ? "安装成功，已添加到数据库"
                                                : "安装成功，但写入数据库失败";
                } else {
                    install_state.message =
                        std::string{"安装成功，"} + CiaTitleKindName(entry.kind) +
                        "类型未添加到数据库";
                }
                if (install_state.success && delete_after_install) {
                    const int remove_rc = std::remove(entry.path.c_str());
                    DebugLog("CIA source delete path=%s rc=%d", entry.path.c_str(), remove_rc);
                }
            }
            if (!installed_ok) {
                install_state.message = InstallStatusText(install_state.result);
            }
            DebugLog("CIA install end result=%u status=%s success=%d installed=%s",
                     static_cast<unsigned>(install_state.result),
                     InstallStatusText(install_state.result), install_state.success ? 1 : 0,
                     install_state.installed_path.c_str());
            install_state.done = true;
        });
    };

    while (appletMainLoop()) {
        padUpdate(&pad);
        const u64 down = padGetButtonsDown(&pad);
        if (install_state.active.load(std::memory_order_acquire)) {
            if (install_state.done.load(std::memory_order_acquire)) {
                join_install_thread("install-done");
                install_state.active = false;
                notice = install_state.message;
                refresh();
                while (appletMainLoop()) {
                    padUpdate(&pad);
                    const u64 msg_down = padGetButtonsDown(&pad);
                    DrawCiaMessage(canvas,
                                   install_state.success ? "安装成功" : "安装失败",
                                   install_state.message);
                    if (msg_down & (HidNpadButton_A | HidNpadButton_B)) {
                        break;
                    }
                    svcSleepThread(16'000'000);
                }
                refresh();
            }
            DrawCiaInstaller(canvas, directory, entries, selected, scroll, delete_source,
                             install_state, notice);
            svcSleepThread(16'000'000);
            continue;
        }

        if (down & HidNpadButton_Plus) {
            break;
        }
        if (down & HidNpadButton_X) {
            delete_source = !delete_source;
        }
        if (down & HidNpadButton_B) {
            if (!ParentDirectory(directory).empty()) {
                directory = ParentDirectory(directory);
                selected = 0;
                scroll = 0;
                refresh();
            } else {
                notice = "已在根目录，按 + 返回主程序";
            }
        }
        if (down & HidNpadButton_Up) {
            selected = std::max(0, selected - 1);
        }
        if (down & HidNpadButton_Down) {
            selected = std::min(std::max(0, static_cast<int>(entries.size()) - 1), selected + 1);
        }
        if (down & HidNpadButton_A) {
            if (!entries.empty()) {
                const CiaBrowserEntry& entry = entries[selected];
                if (entry.type == CiaBrowserEntry::Type::Parent ||
                    entry.type == CiaBrowserEntry::Type::Directory) {
                    directory = entry.path;
                    selected = 0;
                    scroll = 0;
                    refresh();
                } else {
                    start_install(entry);
                }
            }
        }
        constexpr int visible_rows = 10;
        if (selected < scroll) {
            scroll = selected;
        } else if (selected >= scroll + visible_rows) {
            scroll = selected - visible_rows + 1;
        }
        DrawCiaInstaller(canvas, directory, entries, selected, scroll, delete_source,
                         install_state, notice);
        svcSleepThread(16'000'000);
    }

    join_install_thread("installer-exit");
    DebugLog("CIA installer branch exit");
    return EXIT_SUCCESS;
}

int RunTitleUninstaller(const LaunchOptions& options) {
    const u64 title_id = options.uninstall_title_id;
    const std::string title_id_text = TitleIdString(title_id);
    const std::string save_path = InstalledTitleSavePath(title_id);
    DebugLog("3DS uninstall branch enter title_id=%s record=%s save=%s",
             title_id_text.c_str(), options.uninstall_record_path.c_str(), save_path.c_str());
    ExitLog("3DS uninstall begin title_id=%s record=%s save=%s", title_id_text.c_str(),
            options.uninstall_record_path.c_str(), save_path.c_str());

    const auto media_type = Service::FS::MediaType::SDMC;
    const std::string content_path = Service::AM::GetTitlePath(media_type, title_id) + "content/";
    DebugLog("3DS uninstall content path=%s", content_path.c_str());

    const Result uninstall_result = Service::AM::UninstallProgram(media_type, title_id);
    const bool uninstall_ok = uninstall_result.IsSuccess();
    DebugLog("3DS uninstall content result=0x%08x ok=%d", uninstall_result.raw,
             uninstall_ok ? 1 : 0);
    ExitLog("3DS uninstall content result=0x%08x ok=%d", uninstall_result.raw,
            uninstall_ok ? 1 : 0);
    if (!uninstall_ok) {
        return EXIT_FAILURE;
    }

    const bool save_exists = FileUtil::Exists(save_path);
    const bool save_deleted = !save_exists || FileUtil::DeleteDirRecursively(save_path);
    DebugLog("3DS uninstall save path=%s existed=%d deleted=%d", save_path.c_str(),
             save_exists ? 1 : 0, save_deleted ? 1 : 0);
    ExitLog("3DS uninstall save path=%s existed=%d deleted=%d", save_path.c_str(),
            save_exists ? 1 : 0, save_deleted ? 1 : 0);

    bool db_removed = true;
    if (!options.uninstall_record_path.empty()) {
        db_removed =
            SwitchFrontend::GameDatabase::RemoveInstalledGameRecord(options.uninstall_record_path);
    }
    DebugLog("3DS uninstall db record=%s removed=%d", options.uninstall_record_path.c_str(),
             db_removed ? 1 : 0);
    ExitLog("3DS uninstall db record=%s removed=%d", options.uninstall_record_path.c_str(),
            db_removed ? 1 : 0);

    DebugLog("3DS uninstall branch exit ok=%d", (save_deleted && db_removed) ? 1 : 0);
    return save_deleted && db_removed ? EXIT_SUCCESS : EXIT_FAILURE;
}

LaunchOptions ParseLaunchOptions(int argc, char** argv) {
    LaunchOptions options;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i] || std::strcmp(argv[i], "GBAStationSetup") == 0) {
            continue;
        }
        const std::string_view argument{argv[i]};
        if (argument == "--return" && i + 1 < argc && argv[i + 1]) {
            options.return_nro_path = argv[++i];
            continue;
        }
        if (argument == "--exit-to-home") {
            options.return_to_nro = false;
            continue;
        }
        if (argument == "--install-cia" || argument == "--cia-install") {
            options.install_cia_mode = true;
            continue;
        }
        if (argument == "--uninstall-title" && i + 1 < argc && argv[i + 1]) {
            options.uninstall_title_mode = ParseTitleId(argv[++i], options.uninstall_title_id);
            if (!options.uninstall_title_mode) {
                StartupLog("ParseLaunchOptions: invalid uninstall title id %s", argv[i]);
            }
            continue;
        }
        if (argument == "--record-path" && i + 1 < argc && argv[i + 1]) {
            options.uninstall_record_path = argv[++i];
            continue;
        }
        if (argument.starts_with("--")) {
            StartupLog("ParseLaunchOptions: ignoring unknown option %s", argv[i]);
            continue;
        }
        if (EndsWithNoCase(argument, ".nro")) {
            continue;
        }
        if (options.rom_path.empty()) {
            options.rom_path = argument;
        }
    }

    if (options.rom_path.empty() && !options.install_cia_mode && !options.uninstall_title_mode) {
        options.rom_path = FallbackRomPath;
        StartupLog("ParseLaunchOptions: no ROM argument, using fallback %s", FallbackRomPath);
    }
    if (options.install_cia_mode) {
        options.title = "CIA安装";
    } else if (options.uninstall_title_mode) {
        options.title = "卸载3DS游戏";
    } else {
        options.title = TitleFromPath(options.rom_path);
        const auto game_record = SwitchFrontend::GameDatabase::LoadGameRecord(options.rom_path);
        if (game_record.found) {
            if (!game_record.title.empty()) {
                options.title = game_record.title;
            }
            options.display_settings = game_record.display;
            options.display_settings_from_game_db = true;
        }
    }
    StartupLog("ParseLaunchOptions: mode=%s rom=%s title=%s uninstall_title=%016llx return=%s target=%s",
               options.install_cia_mode    ? "install-cia"
               : options.uninstall_title_mode ? "uninstall-title"
                                              : "run",
               options.rom_path.c_str(), options.title.c_str(),
               static_cast<unsigned long long>(options.uninstall_title_id),
               options.return_to_nro ? "nro" : "home", options.return_nro_path.c_str());
    return options;
}

void ConfigureSettings() {
    Settings::RestoreGlobalState(false);
    setenv(SwitchFastmemEnv, "0", 1);
    setenv(SwitchJitFastDispatchEnv, "0", 1);
    Settings::values.use_cpu_jit.SetValue(true);
    Settings::values.cpu_clock_percentage.SetValue(100);
    Settings::values.is_new_3ds.SetValue(true);
    Settings::values.enable_required_online_lle_modules.SetValue(false);
    Settings::values.delay_start_for_lle_modules.SetValue(false);
    Settings::values.graphics_api.SetValue(Settings::GraphicsAPI::Vulkan);
    Settings::values.physical_device.SetValue(0);
    Settings::values.use_gles.SetValue(false);
    Settings::values.renderer_debug.SetValue(false);
    Settings::values.dump_command_buffers.SetValue(false);
    Settings::values.async_shader_compilation.SetValue(true);
    Settings::values.async_presentation.SetValue(true);
    Settings::values.spirv_shader_gen.SetValue(true);
    Settings::values.disable_spirv_optimizer.SetValue(true);
    Settings::values.use_hw_shader.SetValue(true);
    Settings::values.disable_right_eye_render.SetValue(true);
    Settings::values.use_disk_shader_cache.SetValue(true);
    Settings::values.use_shader_jit.SetValue(true);
    Settings::values.resolution_factor.SetValue(1);
    Settings::values.use_vsync.SetValue(true);
    Settings::values.frame_limit.SetValue(100.0);
    Settings::values.layout_option.SetValue(Settings::LayoutOption::Default);
    Settings::values.audio_emulation.SetValue(Settings::AudioEmulation::HLE);
    Settings::values.enable_audio_stretching.SetValue(false);
    Settings::values.enable_realtime_audio.SetValue(true);
    Settings::values.output_type.SetValue(AudioCore::SinkType::Auto);
    Settings::values.input_type.SetValue(AudioCore::InputType::Null);

    auto& profile = Settings::values.current_input_profile;
    const auto MakeButton = [](u64 mask) {
        Common::ParamPackage pkg;
        pkg.Set("engine", "switch_hid");
        pkg.Set("button", static_cast<int>(mask));
        return pkg.Serialize();
    };
    using SwitchFrontend::InputMapping::ButtonMask;
    profile.buttons[Settings::NativeButton::A] =
        MakeButton(ButtonMask("3ds.handle.a", HidNpadButton_A));
    profile.buttons[Settings::NativeButton::B] =
        MakeButton(ButtonMask("3ds.handle.b", HidNpadButton_B));
    profile.buttons[Settings::NativeButton::X] =
        MakeButton(ButtonMask("3ds.handle.x", HidNpadButton_X));
    profile.buttons[Settings::NativeButton::Y] =
        MakeButton(ButtonMask("3ds.handle.y", HidNpadButton_Y));
    profile.buttons[Settings::NativeButton::Up] =
        MakeButton(ButtonMask("3ds.handle.up", HidNpadButton_Up));
    profile.buttons[Settings::NativeButton::Down] =
        MakeButton(ButtonMask("3ds.handle.down", HidNpadButton_Down));
    profile.buttons[Settings::NativeButton::Left] =
        MakeButton(ButtonMask("3ds.handle.left", HidNpadButton_Left));
    profile.buttons[Settings::NativeButton::Right] =
        MakeButton(ButtonMask("3ds.handle.right", HidNpadButton_Right));
    profile.buttons[Settings::NativeButton::L] =
        MakeButton(ButtonMask("3ds.handle.l", HidNpadButton_L));
    profile.buttons[Settings::NativeButton::R] =
        MakeButton(ButtonMask("3ds.handle.r", HidNpadButton_R));
    profile.buttons[Settings::NativeButton::Start] =
        MakeButton(ButtonMask("3ds.handle.start", HidNpadButton_Plus));
    profile.buttons[Settings::NativeButton::Select] =
        MakeButton(ButtonMask("3ds.handle.select", HidNpadButton_Minus));
    profile.buttons[Settings::NativeButton::ZL] =
        MakeButton(ButtonMask("3ds.handle.l2", HidNpadButton_ZL));
    profile.buttons[Settings::NativeButton::ZR] =
        MakeButton(ButtonMask("3ds.handle.r2", HidNpadButton_ZR));

    const auto MakeAnalog = [](int axis) {
        Common::ParamPackage pkg;
        pkg.Set("engine", "switch_hid_analog");
        pkg.Set("axis", axis);
        return pkg.Serialize();
    };
    profile.analogs[Settings::NativeAnalog::CirclePad] = MakeAnalog(0);
    profile.analogs[Settings::NativeAnalog::CStick]    = MakeAnalog(1);
    profile.motion_device = "engine:switch_hid_motion,sensitivity:1.25";
    profile.touch_device = "engine:emu_window";
    profile.controller_touch_device.clear();
    profile.use_touchpad = false;
    profile.use_touch_from_button = false;
    profile.touch_from_button_map_index = 0;

    DebugLog("switch settings: cpu_jit=%d cpu_clock=%d new3ds=%d vulkan=%d hw_shader=%d shader_jit=%d async_shader=%d async_present=%d disk_cache=%d audio_hle=%d audio_stretch=%d realtime_audio=%d res=%u",
             Settings::values.use_cpu_jit.GetValue() ? 1 : 0,
             Settings::values.cpu_clock_percentage.GetValue(),
             Settings::values.is_new_3ds.GetValue() ? 1 : 0,
             Settings::values.graphics_api.GetValue() == Settings::GraphicsAPI::Vulkan ? 1 : 0,
             Settings::values.use_hw_shader.GetValue() ? 1 : 0,
             Settings::values.use_shader_jit.GetValue() ? 1 : 0,
             Settings::values.async_shader_compilation.GetValue() ? 1 : 0,
             Settings::values.async_presentation.GetValue() ? 1 : 0,
             Settings::values.use_disk_shader_cache.GetValue() ? 1 : 0,
             Settings::values.audio_emulation.GetValue() == Settings::AudioEmulation::HLE ? 1 : 0,
             Settings::values.enable_audio_stretching.GetValue() ? 1 : 0,
             Settings::values.enable_realtime_audio.GetValue() ? 1 : 0,
             Settings::values.resolution_factor.GetValue());

    for (const auto& service_module : Service::service_module_map) {
        Settings::values.lle_modules.emplace(service_module.name, false);
    }
}

bool ExitComboPressed() {
    static bool was_down = false;
    const bool down = SwitchFrontend::InputMapping::MenuHotkeyPressed(pad);
    const bool triggered = down && !was_down;
    was_down = down;
    return triggered;
}

std::string LowerCopy(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool TryParseSystemLanguage(std::string_view value, Service::CFG::SystemLanguage& language) {
    const std::string lower = LowerCopy(value);
    if (lower == "japanese" || lower == "jp" || lower == "ja" || lower == "0") {
        language = Service::CFG::LANGUAGE_JP;
        return true;
    }
    if (lower == "english" || lower == "en" || lower == "1") {
        language = Service::CFG::LANGUAGE_EN;
        return true;
    }
    if (lower == "french" || lower == "fr" || lower == "2") {
        language = Service::CFG::LANGUAGE_FR;
        return true;
    }
    if (lower == "german" || lower == "de" || lower == "3") {
        language = Service::CFG::LANGUAGE_DE;
        return true;
    }
    if (lower == "italian" || lower == "it" || lower == "4") {
        language = Service::CFG::LANGUAGE_IT;
        return true;
    }
    if (lower == "spanish" || lower == "es" || lower == "5") {
        language = Service::CFG::LANGUAGE_ES;
        return true;
    }
    if (lower == "simplified chinese" || lower == "simplified_chinese" || lower == "zh" ||
        lower == "zh-cn" || lower == "6") {
        language = Service::CFG::LANGUAGE_ZH;
        return true;
    }
    if (lower == "korean" || lower == "ko" || lower == "kr" || lower == "7") {
        language = Service::CFG::LANGUAGE_KO;
        return true;
    }
    if (lower == "dutch" || lower == "nl" || lower == "8") {
        language = Service::CFG::LANGUAGE_NL;
        return true;
    }
    if (lower == "portuguese" || lower == "pt" || lower == "9") {
        language = Service::CFG::LANGUAGE_PT;
        return true;
    }
    if (lower == "russian" || lower == "ru" || lower == "10") {
        language = Service::CFG::LANGUAGE_RU;
        return true;
    }
    if (lower == "traditional chinese" || lower == "traditional_chinese" || lower == "tw" ||
        lower == "zh-tw" || lower == "11") {
        language = Service::CFG::LANGUAGE_TW;
        return true;
    }
    return false;
}

bool TryParseInt(std::string_view value, int& out) {
    if (value.empty()) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(std::string(value), &consumed);
        if (consumed == value.size()) {
            out = parsed;
            return true;
        }
    } catch (...) {
    }
    return false;
}

bool TryParseFloat(std::string_view value, float& out) {
    if (value.empty()) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const float parsed = std::stof(std::string(value), &consumed);
        if (consumed == value.size()) {
            out = parsed;
            return true;
        }
    } catch (...) {
    }
    return false;
}

bool ParseConfigBool(std::string_view value, bool fallback) {
    const std::string lower = LowerCopy(value);
    if (lower == "true" || lower == "1" || lower == "on" || lower == "yes") {
        return true;
    }
    if (lower == "false" || lower == "0" || lower == "off" || lower == "no") {
        return false;
    }
    return fallback;
}

bool ApplySwitchFastmemConfig() {
    using SwitchFrontend::GBAStationConfig::GetConfigValue;
    std::string configured = GetConfigValue("switch_fastmem");
    if (configured.empty()) {
        configured = GetConfigValue("dynarmic_fastmem");
    }
    const bool enabled = ParseConfigBool(configured, false);
    setenv(SwitchFastmemEnv, enabled ? "1" : "0", 1);
    return enabled;
}

bool ApplySwitchJitFastDispatchConfig() {
    using SwitchFrontend::GBAStationConfig::GetConfigValue;
    std::string configured = GetConfigValue("switch_jit_fast_dispatch");
    if (configured.empty()) {
        configured = GetConfigValue("dynarmic_fast_dispatch");
    }
    const bool enabled = ParseConfigBool(configured, false);
    setenv(SwitchJitFastDispatchEnv, enabled ? "1" : "0", 1);
    return enabled;
}

std::string FormatFloat(float value) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.3f", value);
    return text;
}

void ApplyConfiguredDisplayDefaults(SwitchFrontend::GBAStationDisplaySettings& settings,
                                    bool include_screen, bool include_overlay) {
    using SwitchFrontend::GBAStationConfig::GetConfigValue;
    int parsed_int = 0;
    float parsed_float = 0.0f;

    if (include_screen) {
        const std::string layout = GetConfigValue("ndsScreenLayout");
        if (!layout.empty()) {
            settings.screen_layout = layout;
        }
        if (TryParseInt(GetConfigValue("ndsScreenOrientation"), parsed_int)) {
            settings.screen_orientation = parsed_int;
        }
        if (TryParseInt(GetConfigValue("ndsInternalResolution"), parsed_int)) {
            settings.internal_resolution = std::clamp(parsed_int, 1, 4);
        } else if (TryParseInt(GetConfigValue("upscale"), parsed_int)) {
            settings.internal_resolution = std::clamp(parsed_int, 1, 4);
        }
        const std::string integer_scale = GetConfigValue("ndsIntegerScale");
        if (!integer_scale.empty()) {
            settings.integer_scale = ParseConfigBool(integer_scale, settings.integer_scale);
        }
        if (TryParseInt(GetConfigValue("ndsScreenGap"), parsed_int)) {
            settings.screen_gap = std::clamp(parsed_int, -256, 256);
        }
        if (TryParseFloat(GetConfigValue("ndsTopScale"), parsed_float)) {
            settings.top_scale = std::clamp(parsed_float, 1.0f, 10.0f);
        }
        if (TryParseFloat(GetConfigValue("ndsTopOffsetX"), parsed_float)) {
            settings.top_offset_x = std::clamp(parsed_float, -1024.0f, 1024.0f);
        }
        if (TryParseFloat(GetConfigValue("ndsTopOffsetY"), parsed_float)) {
            settings.top_offset_y = std::clamp(parsed_float, -1024.0f, 1024.0f);
        }
        if (TryParseFloat(GetConfigValue("ndsBottomScale"), parsed_float)) {
            settings.bottom_scale = std::clamp(parsed_float, 1.0f, 10.0f);
        }
        if (TryParseFloat(GetConfigValue("ndsBottomOffsetX"), parsed_float)) {
            settings.bottom_offset_x = std::clamp(parsed_float, -1024.0f, 1024.0f);
        }
        if (TryParseFloat(GetConfigValue("ndsBottomOffsetY"), parsed_float)) {
            settings.bottom_offset_y = std::clamp(parsed_float, -1024.0f, 1024.0f);
        }
        if (TryParseFloat(GetConfigValue("ndsBottomOpacity"), parsed_float)) {
            settings.bottom_opacity = std::clamp(parsed_float, 0.0f, 1.0f);
        }
    }

    if (include_overlay) {
        const std::string overlay_enabled = GetConfigValue("overlayEnabled");
        if (!overlay_enabled.empty()) {
            settings.overlay_enabled = ParseConfigBool(overlay_enabled, settings.overlay_enabled);
        }
        const std::string overlay_path = GetConfigValue("overlayPath");
        if (!overlay_path.empty()) {
            settings.overlay_path = overlay_path;
        }
    }

    if (TryParseFloat(GetConfigValue("fastforward.multiplier"), parsed_float)) {
        settings.fast_forward_multiplier = std::clamp(parsed_float, 0.1f, 5.0f);
    }
}

bool SaveGlobalOverlayDefaults(const SwitchFrontend::GBAStationDisplaySettings& display) {
    SwitchFrontend::GBAStationConfig::SetConfigValue(
        "overlayEnabled", display.overlay_enabled ? "true" : "false");
    SwitchFrontend::GBAStationConfig::SetConfigValue("overlayPath", display.overlay_path);
    return SwitchFrontend::GBAStationConfig::SaveConfig();
}

bool SaveGlobalScreenDefaults(const SwitchFrontend::GBAStationDisplaySettings& display) {
    SwitchFrontend::GBAStationConfig::SetConfigValue("fastforward.multiplier",
                                                     FormatFloat(display.fast_forward_multiplier));
    SwitchFrontend::GBAStationConfig::SetConfigValue(
        "ndsInternalResolution", std::to_string(std::clamp(display.internal_resolution, 1, 4)));
    SwitchFrontend::GBAStationConfig::SetConfigValue("upscale",
                                                     std::to_string(std::clamp(
                                                         display.internal_resolution, 1, 4)));
    SwitchFrontend::GBAStationConfig::SetConfigValue("ndsIntegerScale",
                                                     display.integer_scale ? "true" : "false");
    SwitchFrontend::GBAStationConfig::SetConfigValue("ndsScreenLayout", display.screen_layout);
    SwitchFrontend::GBAStationConfig::SetConfigValue("ndsScreenOrientation",
                                                     std::to_string(display.screen_orientation));
    SwitchFrontend::GBAStationConfig::SetConfigValue("ndsScreenGap",
                                                     std::to_string(display.screen_gap));
    SwitchFrontend::GBAStationConfig::SetConfigValue("ndsTopScale",
                                                     FormatFloat(display.top_scale));
    SwitchFrontend::GBAStationConfig::SetConfigValue("ndsTopOffsetX",
                                                     FormatFloat(display.top_offset_x));
    SwitchFrontend::GBAStationConfig::SetConfigValue("ndsTopOffsetY",
                                                     FormatFloat(display.top_offset_y));
    SwitchFrontend::GBAStationConfig::SetConfigValue("ndsBottomScale",
                                                     FormatFloat(display.bottom_scale));
    SwitchFrontend::GBAStationConfig::SetConfigValue("ndsBottomOffsetX",
                                                     FormatFloat(display.bottom_offset_x));
    SwitchFrontend::GBAStationConfig::SetConfigValue("ndsBottomOffsetY",
                                                     FormatFloat(display.bottom_offset_y));
    SwitchFrontend::GBAStationConfig::SetConfigValue("ndsBottomOpacity",
                                                     FormatFloat(display.bottom_opacity));
    return SwitchFrontend::GBAStationConfig::SaveConfig();
}

void ApplyConfiguredSystemLanguage(Core::System& system) {
    const std::string language_value = SwitchFrontend::GBAStationConfig::GetConfiguredSystemLanguage();
    if (language_value.empty()) {
        return;
    }

    Service::CFG::SystemLanguage language = Service::CFG::LANGUAGE_EN;
    if (!TryParseSystemLanguage(language_value, language)) {
        DebugLog("invalid configured 3ds language: %s", language_value.c_str());
        return;
    }

    auto cfg = Service::CFG::GetModule(system);
    cfg->SetSystemLanguage(language);
    const Result rc = cfg->UpdateConfigNANDSavegame();
    DebugLog("configured 3ds language: %s (%u) rc=0x%x", language_value.c_str(),
             static_cast<unsigned>(language), rc.raw);
}

void ApplyConfiguredUsername(Core::System& system) {
    std::string username = SwitchFrontend::GBAStationConfig::GetConfiguredUsername();
    if (username.empty()) {
        return;
    }

    std::u16string username_utf16 = Common::UTF8ToUTF16(username);
    if (username_utf16.empty()) {
        return;
    }
    if (username_utf16.size() > 10) {
        username_utf16.resize(10);
        username = Common::UTF16ToUTF8(username_utf16);
    }

    auto cfg = Service::CFG::GetModule(system);
    cfg->SetUsername(username_utf16);
    const Result rc = cfg->UpdateConfigNANDSavegame();
    DebugLog("configured 3ds username: %s rc=0x%x", username.c_str(), rc.raw);
}

int Run(int argc, char** argv) {
    ApplyDiagnosticLogSwitchFromEnv();
#if defined(GBASTATION_SWITCH_DIAGNOSTIC_LOGS)
    // Raw NVK normally submits asynchronously.  Near the known scene-change
    // fault, serialize individual submits so the backend captures the exact
    // faulting pushbuffer rather than a later submit observing channel reset.
    setenv("NVK_SWITCH_DIAGNOSTIC_SYNC_AFTER", "9000", 1);
    // A/B test the raw backend completion fence.  The normal sequence waits
    // for ROP writes before signaling; this diagnostic sequence explicitly
    // idles both graphics and compute first so resources cannot be reclaimed
    // while either engine still references them.
    setenv("NVK_SWITCH_DIAGNOSTIC_FULL_IDLE_COMPLETION", "1", 1);
    // The scene-change fault is now isolated to one 0x235c-byte graphics
    // push.  Split that push after its existing WAIT_FOR_IDLE packets and
    // check the channel after each segment to locate the first bad draw.
    setenv("NVK_SWITCH_DIAGNOSTIC_SPLIT_EXEC_BYTES", "0x235c", 1);
    // Segments 0-10 pass; the fault is inside the final 0x8cf..0x8d7 tail.
    // Split every packet in that tail to distinguish MME accounting from the
    // compute QMD launch and its wait.
    setenv("NVK_SWITCH_DIAGNOSTIC_FINE_SPLIT_FROM_DW", "0x8cf", 1);
#endif
    OpenStartupLogIfNeeded("a");
    StartupLog("Run: entry argc=%d", argc);
    StartupLog("Run: appletLockExit");
    const LibnxResult lock_exit_rc = appletLockExit();
    StartupLog("Run: appletLockExit rc=0x%x", lock_exit_rc);
    DebugOpen();
    ExitLogOpen("w");
    ExitLog("Run entry argc=%d appletLockExit=0x%x", argc, lock_exit_rc);
    InstallFatalHandlers();
    ThreadCoreMaskGuard thread_core_guard;

    DebugLog("argc=%d", argc);
    for (int i = 0; i < argc; i++) {
        DebugLog("argv[%d]=%s", i, argv[i] ? argv[i] : "(null)");
    }
    HeartbeatOpen();

    StartupLog("Run: parsing ROM path");
    LaunchOptions launch_options = ParseLaunchOptions(argc, argv);
    const std::string& rom_path = launch_options.rom_path;
    if (rom_path.empty() && !launch_options.install_cia_mode &&
        !launch_options.uninstall_title_mode) {
        DebugLog("no ROM path supplied");
        ExitLog("early exit: no ROM path");
        thread_core_guard.Restore("no-rom");
        DebugClose();
        const bool queued_launcher_return = QueueLauncherReturn(launch_options);
        ExitLog("early exit no-rom: queued_launcher_return=%d", queued_launcher_return ? 1 : 0);
        appletUnlockExit();
        ExitLog("early exit no-rom: appletUnlockExit done");
        return queued_launcher_return ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    StartupLog("Run: romfsInit");
    const LibnxResult romfs_result = romfsInit();
    const bool romfs_initialized = romfs_result == 0;
    DebugLog("romfsInit=0x%x", romfs_result);

    StartupLog("Run: pin main thread affinity");
    PinCurrentThreadToCore(2, "main");

    StartupLog("Run: pad init");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    StartupLog("Run: InputCommon::Init");
    InputCommon::Init();

    StartupLog("Run: input mapping");
    SwitchFrontend::InputMapping::Reload();
    StartupLog("Run: ConfigureSettings");
    ConfigureSettings();
    SwitchFrontend::GBAStationConfig::ReloadConfig();
    SwitchFrontend::GBAStationConfig::ApplyConfig();
    const bool switch_fastmem_enabled = ApplySwitchFastmemConfig();
    const bool switch_jit_fast_dispatch_enabled = ApplySwitchJitFastDispatchConfig();
    ApplyConfiguredDisplayDefaults(launch_options.display_settings,
                                   !launch_options.display_settings_from_game_db,
                                   !launch_options.display_settings_from_game_db);
    Settings::values.resolution_factor.SetValue(
        static_cast<u16>(std::clamp(launch_options.display_settings.internal_resolution, 1, 4)));
    Settings::values.swap_screen.SetValue(false);
    // The Switch frontend owns a FIFO VI swapchain and must continue presenting while a title
    // is booting.  Some titles do not report a top-screen buffer swap for a long time; with
    // duplicate-frame skipping enabled that leaves VI displaying the initial black image even
    // though the emulated system and renderer are still advancing.
    Settings::values.use_skip_duplicate_frames.SetValue(false);
    DebugLog("GBAStation config applied: path=%s options=%zu upscale=%s game_db_display=%d launch_res=%d effective_res=%u skip_duplicate=%d switch_fastmem=%d switch_jit_fast_dispatch=%d",
             SwitchFrontend::GBAStationConfig::GetLoadedConfigPath().c_str(),
             SwitchFrontend::GBAStationConfig::GetLoadedOptionCount(),
             SwitchFrontend::GBAStationConfig::GetConfigValue("upscale", "default").c_str(),
             launch_options.display_settings_from_game_db ? 1 : 0,
             launch_options.display_settings.internal_resolution,
             Settings::values.resolution_factor.GetValue(),
             Settings::values.use_skip_duplicate_frames.GetValue() ? 1 : 0,
             switch_fastmem_enabled ? 1 : 0,
             switch_jit_fast_dispatch_enabled ? 1 : 0);
    StartupLog("Run: FileUtil::SetUserPath %s/", SystemDir);
    FileUtil::SetUserPath(std::string{SystemDir} + "/");
    StartupLog("Run: Common::Log init");
    bool common_log_started = false;
    if (EnableCommonLogFile) {
        Common::Log::Initialize("../debug/azahar_common.txt");
        Common::Log::Start();
        common_log_started = true;
    } else {
        StartupLog("Run: Common::Log disabled");
    }

    StartupLog("Run: Core::System::GetInstance");
    auto& system = Core::System::GetInstance();
    StartupLog("Run: frontend applets/image interface");
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());
    Frontend::RegisterDefaultApplets(system);
    system.RegisterSoftwareKeyboard(std::make_shared<SwitchFrontend::SwitchKeyboard>());

    if (launch_options.install_cia_mode) {
        DebugLog("entering CIA installer mode");
        const int install_exit_code = RunCiaInstaller(launch_options);
        DebugLog("CIA installer mode finished code=%d", install_exit_code);
        DebugLog("shutdown step: InputCommon::Shutdown begin");
        InputCommon::Shutdown();
        DebugLog("shutdown step: InputCommon::Shutdown done");
        if (common_log_started) {
            Common::Log::Stop();
            common_log_started = false;
        }
        if (romfs_initialized) {
            romfsExit();
        }
        thread_core_guard.Restore("cia-installer");
        const bool queued_launcher_return = QueueLauncherReturn(launch_options);
        DebugLog("CIA installer return queued=%d", queued_launcher_return ? 1 : 0);
        DebugClose();
        appletUnlockExit();
        return queued_launcher_return ? install_exit_code : EXIT_FAILURE;
    }

    if (launch_options.uninstall_title_mode) {
        DebugLog("entering 3DS title uninstall mode");
        const int uninstall_exit_code = RunTitleUninstaller(launch_options);
        DebugLog("3DS title uninstall mode finished code=%d", uninstall_exit_code);
        if (common_log_started) {
            Common::Log::Stop();
            common_log_started = false;
        }
        if (romfs_initialized) {
            romfsExit();
        }
        thread_core_guard.Restore("3ds-uninstaller");
        const bool queued_launcher_return = QueueLauncherReturn(launch_options);
        DebugLog("3DS title uninstall return queued=%d", queued_launcher_return ? 1 : 0);
        DebugClose();
        appletUnlockExit();
        return queued_launcher_return ? uninstall_exit_code : EXIT_FAILURE;
    }

    StartupLog("Run: creating NWindow frontend");
    SwitchFrontend::EmuWindowSwitch window{nwindowGetDefault()};
    window.SetDisplaySettings(launch_options.display_settings);
    DebugLog("loading ROM: %s", rom_path.c_str());
    const Core::System::ResultStatus load_result = system.Load(window, rom_path);
    if (load_result != Core::System::ResultStatus::Success) {
        DebugLog("load failed: %s (%u)", ResultStatusName(load_result),
                 static_cast<unsigned>(load_result));
        ExitLog("early exit: load failed status=%s (%u)", ResultStatusName(load_result),
                static_cast<unsigned>(load_result));
        if (common_log_started) {
            Common::Log::Stop();
            common_log_started = false;
        }
        if (romfs_initialized) {
            romfsExit();
        }
        thread_core_guard.Restore("load-failed");
        DebugClose();
        const bool queued_launcher_return = QueueLauncherReturn(launch_options);
        ExitLog("early exit load-failed: queued_launcher_return=%d",
                queued_launcher_return ? 1 : 0);
        appletUnlockExit();
        ExitLog("early exit load-failed: appletUnlockExit done");
        return queued_launcher_return ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    system.CheatEngine().DisableAllCheats();
    DebugLog("cheats loaded but left disabled on startup; enable manually from menu");
    ApplyConfiguredSystemLanguage(system);
    ApplyConfiguredUsername(system);
    DebugLog("renderer resolution scale factor=%u",
             system.GPU().Renderer().GetResolutionScaleFactor());

    StartupLog("Run: ApplySettings");
    system.ApplySettings();
    SwitchFrontend::GameDatabase::UpdatePlayStats(
        launch_options.rom_path, launch_options.title, true, 0);

    using SlotStateCache =
        std::array<std::atomic_bool, SwitchFrontend::OverlayUI::StateSlotCount + 1>;
    const auto slot_state_cache = std::make_shared<SlotStateCache>();
    for (auto& occupied : *slot_state_cache) {
        occupied.store(false, std::memory_order_relaxed);
    }
    u64 program_id{};
    if (system.GetAppLoader().ReadProgramId(program_id) == Loader::ResultStatus::Success) {
        const u64 movie_id = system.Movie().GetCurrentMovieID();
        for (const Core::SaveStateInfo& state : Core::ListSaveStates(program_id, movie_id)) {
            if (state.slot > 0 && state.slot <= SwitchFrontend::OverlayUI::StateSlotCount) {
                (*slot_state_cache)[state.slot].store(true, std::memory_order_relaxed);
            }
        }
    }
    SwitchFrontend::OverlayUI::SetGameTitle(launch_options.title);
    SwitchFrontend::OverlayUI::SetSlotOccupiedCallback(
        [slot_state_cache](int slot) {
            return slot > 0 && slot <= SwitchFrontend::OverlayUI::StateSlotCount &&
                   (*slot_state_cache)[slot].load(std::memory_order_acquire);
        });
    auto cheat_settings_dirty = std::make_shared<std::atomic_bool>(false);
    SwitchFrontend::OverlayUI::SetCheatCallbacks(
        [&system]() {
            std::vector<SwitchFrontend::OverlayUI::CheatEntry> entries;
            const auto cheats = system.CheatEngine().GetCheats();
            entries.reserve(cheats.size());
            for (const auto& cheat : cheats) {
                if (cheat) {
                    entries.push_back({cheat->GetName(), cheat->IsEnabled()});
                }
            }
            return entries;
        },
        [&system, cheat_settings_dirty](int index) {
            if (index < 0) {
                return false;
            }
            bool enabled = false;
            if (!system.CheatEngine().ToggleCheat(static_cast<std::size_t>(index), &enabled)) {
                return false;
            }
            cheat_settings_dirty->store(true, std::memory_order_release);
            DebugLog("menu cheat toggled index=%d enabled=%d", index, enabled ? 1 : 0);
            return true;
        });

    SwitchFrontend::VulkanOverlay::SetDisplaySettings(launch_options.display_settings);
    bool menu_initialized = SwitchFrontend::VulkanOverlay::Init(
        static_cast<Vulkan::RendererVulkan&>(system.GPU().Renderer()));
    bool show_fps_overlay = ParseConfigBool(
        SwitchFrontend::GBAStationConfig::GetConfigValue("display.showFps", "0"), false);
    SwitchFrontend::VulkanOverlay::SetFpsOverlay(show_fps_overlay, 0.0f);
    DebugLog("GBAStation menu init=%d hotkey=ZL+ZR return=%s target=%s",
             menu_initialized ? 1 : 0, launch_options.return_to_nro ? "nro" : "home",
             launch_options.return_nro_path.c_str());

    DebugLog("startup keepalive armed");

    DebugLog("entering main loop");
    raw_marker_enabled = false;

    using Clock = std::chrono::steady_clock;
    auto play_stats_checkpoint = Clock::now();
    auto last_keepalive = Clock::now();
    u64 loop_count = 0;
    u64 keepalive_count = 0;
    bool applet_loop_active = true;
    const s32 initial_renderer_frame = system.GPU().Renderer().GetCurrentFrame();
    bool saw_guest_frame = initial_renderer_frame > 0;
    bool pause_frame_ready = initial_renderer_frame > 0;
    s32 pause_frame_baseline = initial_renderer_frame;
    bool pending_overlay_reinit = false;
    s32 last_logged_frame = initial_renderer_frame;
    auto last_heartbeat = Clock::now();
    u64 last_heartbeat_loop_count = loop_count;
    s32 last_heartbeat_frame = last_logged_frame;
    bool menu_was_visible = false;
    bool block_game_input_until_release = false;
    bool menu_audio_muted = false;
    float menu_restore_volume = Settings::values.volume.GetValue();
    bool fast_forward_toggle = false;
    bool previous_fast_forward_combo = false;
    bool previous_mic_input_combo = false;
    bool mic_input_simulated = false;
    AudioCore::InputType mic_restore_input_type = Settings::values.input_type.GetValue();
    bool last_fast_forward_active = false;
    bool fast_forward_compile_throttled = false;
    bool force_input_suppressed_during_shutdown = false;
    const bool normal_vsync = Settings::values.use_vsync.GetValue();
    enum class PendingStateOperation {
        None,
        Save,
        Load,
    };
    struct PendingStateRequest {
        PendingStateOperation operation{PendingStateOperation::None};
        int slot{};
        int delay_frames{};
    };
    PendingStateRequest pending_state_request{};
    u64 diagnostic_runloop_count = 0;
    double diagnostic_runloop_ms_total = 0.0;
    double diagnostic_runloop_ms_max = 0.0;
    const char* exit_reason = "loop condition ended";
    while (true) {
        const auto now = Clock::now();
        applet_loop_active = PumpAppletMessages();
        if (!applet_loop_active) {
            exit_reason = "applet message requested exit";
            DebugLog("main loop exit: applet message requested exit after %llu iterations",
                     static_cast<unsigned long long>(loop_count));
            ExitLog("main loop exit: applet message requested exit iterations=%llu",
                    static_cast<unsigned long long>(loop_count));
            break;
        }
        if (!system.IsPoweredOn()) {
            exit_reason = "system powered off before RunLoop";
            DebugLog("main loop exit: system powered off before RunLoop after %llu iterations",
                     static_cast<unsigned long long>(loop_count));
            ExitLog("main loop exit: system not powered iterations=%llu",
                    static_cast<unsigned long long>(loop_count));
            break;
        }

        auto& renderer = system.GPU().Renderer();

        if (!saw_guest_frame && now - last_keepalive >= std::chrono::seconds(2)) {
            keepalive_count++;
            DebugLog("startup keepalive present #%llu: renderer_frame=%d",
                     static_cast<unsigned long long>(keepalive_count), renderer.GetCurrentFrame());
            renderer.TryPresent(0);
            DebugLog("startup keepalive present #%llu complete",
                     static_cast<unsigned long long>(keepalive_count));
            last_keepalive = now;
        }

        padUpdate(&pad);

        if (menu_initialized) {
            SwitchFrontend::VulkanOverlay::Update(&pad);
        }
        const bool menu_visible =
            menu_initialized && SwitchFrontend::VulkanOverlay::IsVisible();
        const bool fast_forward_combo = SwitchFrontend::InputMapping::FastForwardHotkeyPressed(pad);
        if (SwitchFrontend::InputMapping::FastForwardToggleMode() && fast_forward_combo &&
            !previous_fast_forward_combo && !menu_visible) {
            fast_forward_toggle = !fast_forward_toggle;
        }
        previous_fast_forward_combo = fast_forward_combo;
        const bool fast_forward_requested =
            !menu_visible && SwitchFrontend::InputMapping::FastForwardEnabled() &&
            (SwitchFrontend::InputMapping::FastForwardToggleMode() ? fast_forward_toggle
                                                                   : fast_forward_combo);
        auto& vulkan_renderer = static_cast<Vulkan::RendererVulkan&>(renderer);
        const std::size_t pending_compilations = vulkan_renderer.PendingCompilationCount();
        if (!fast_forward_requested) {
            fast_forward_compile_throttled = false;
        } else if (!fast_forward_compile_throttled && pending_compilations >= 4) {
            fast_forward_compile_throttled = true;
            DebugLog("fast forward throttled: pending shader/pipeline work=%zu",
                     pending_compilations);
        } else if (fast_forward_compile_throttled && pending_compilations == 0) {
            fast_forward_compile_throttled = false;
            DebugLog("fast forward resumed: shader/pipeline queue drained");
        }
        const bool fast_forward_active =
            fast_forward_requested && !fast_forward_compile_throttled;
        const float fast_forward_multiplier =
            SwitchFrontend::VulkanOverlay::GetDisplaySettings().fast_forward_multiplier;
        Settings::is_temporary_frame_limit = fast_forward_active;
        Settings::temporary_frame_limit =
            fast_forward_active ? static_cast<double>(fast_forward_multiplier) * 100.0 : 0.0;
        if (fast_forward_active != last_fast_forward_active) {
            Settings::values.use_vsync.SetValue(fast_forward_active ? false : normal_vsync);
            SwitchFrontend::VulkanOverlay::SetFastForwardActive(fast_forward_active);
            vulkan_renderer.SetFastForward(fast_forward_active, fast_forward_multiplier);
            DebugLog("fast forward %s multiplier=%.2f limit=%.1f",
                     fast_forward_active ? "on" : "off", fast_forward_multiplier,
                     Settings::temporary_frame_limit);
            last_fast_forward_active = fast_forward_active;
        }
        if (menu_visible != menu_was_visible) {
            block_game_input_until_release = true;
            if (menu_visible) {
                menu_restore_volume = Settings::values.volume.GetValue();
                Settings::values.volume.SetValue(0.0f);
                menu_audio_muted = true;
            } else if (menu_audio_muted) {
                Settings::values.volume.SetValue(menu_restore_volume);
                menu_audio_muted = false;
            }
            DebugLog("GBAStation menu visible=%d", menu_visible ? 1 : 0);
            menu_was_visible = menu_visible;
        }
        if (!menu_visible && cheat_settings_dirty->exchange(false, std::memory_order_acq_rel)) {
            system.CheatEngine().SaveLoadedCheatFile();
            SwitchFrontend::OverlayUI::ShowToast("金手指设置已保存");
            DebugLog("menu cheat settings persisted after close");
        }
        if (!menu_visible && block_game_input_until_release && padGetButtons(&pad) == 0) {
            block_game_input_until_release = false;
        }
        bool suppress_game_input = menu_visible || block_game_input_until_release;
        const bool mic_input_combo = SwitchFrontend::InputMapping::MicInputHotkeyPressed(pad);
        if (!suppress_game_input && mic_input_combo && !previous_mic_input_combo) {
            if (!mic_input_simulated) {
                mic_restore_input_type = Settings::values.input_type.GetValue();
                Settings::values.input_type.SetValue(AudioCore::InputType::Static);
                mic_input_simulated = true;
                SwitchFrontend::OverlayUI::ShowToast("已开始模拟麦克风输入");
                DebugLog("hotkey microphone simulation on");
            } else {
                Settings::values.input_type.SetValue(mic_restore_input_type);
                mic_input_simulated = false;
                SwitchFrontend::OverlayUI::ShowToast("已停止模拟麦克风输入");
                DebugLog("hotkey microphone simulation off");
            }
            system.ApplySettings();
            block_game_input_until_release = true;
            suppress_game_input = true;
        }
        previous_mic_input_combo = mic_input_combo;

        window.SetInputSuppressed(suppress_game_input);
        InputCommon::SwitchHID::SetInputSuppressed(suppress_game_input);
        window.PollEvents();
        InputCommon::SwitchHID::Update();

        const auto menu_action = static_cast<SwitchFrontend::OverlayUI::Action>(
            menu_initialized ? SwitchFrontend::VulkanOverlay::ConsumeAction() : 0);
        if (SwitchFrontend::OverlayUI::IsSaveStateAction(menu_action) ||
            SwitchFrontend::OverlayUI::IsLoadStateAction(menu_action)) {
            const int slot = SwitchFrontend::OverlayUI::GetStateSlotForAction(menu_action);
            const bool saving = SwitchFrontend::OverlayUI::IsSaveStateAction(menu_action);
            if (pending_state_request.operation == PendingStateOperation::None) {
                pending_state_request.operation =
                    saving ? PendingStateOperation::Save : PendingStateOperation::Load;
                pending_state_request.slot = slot;
                pending_state_request.delay_frames = 1;
                if (menu_initialized) {
                    SwitchFrontend::VulkanOverlay::Close();
                }
                char message[64]{};
                std::snprintf(message, sizeof(message), saving ? "准备保存到存档位 %d"
                                                              : "准备读取存档位 %d",
                              slot);
                SwitchFrontend::OverlayUI::ShowToast(message);
                DebugLog("menu queued %s state slot=%d", saving ? "save" : "load", slot);
            } else {
                SwitchFrontend::OverlayUI::ShowToast("已有状态操作正在进行");
            }
            block_game_input_until_release = true;
        } else if (menu_action == SwitchFrontend::OverlayUI::Action::Reset) {
            if (system.SendSignal(Core::System::Signal::Reset)) {
                if (menu_initialized) {
                    SwitchFrontend::VulkanOverlay::Shutdown();
                    menu_initialized = false;
                }
                pending_overlay_reinit = true;
                pause_frame_ready = false;
                saw_guest_frame = false;
                SwitchFrontend::OverlayUI::ShowToast("正在重置游戏");
                DebugLog("menu requested game reset");
            }
            block_game_input_until_release = true;
        } else if (menu_action == SwitchFrontend::OverlayUI::Action::DisplaySettingsChanged) {
            const auto display = SwitchFrontend::VulkanOverlay::GetDisplaySettings();
            if (Settings::values.resolution_factor.GetValue() !=
                static_cast<u32>(display.internal_resolution)) {
                Settings::values.resolution_factor.SetValue(
                    static_cast<u16>(display.internal_resolution));
                system.ApplySettings();
            }
            window.SetDisplaySettings(display);
            const bool saved = SwitchFrontend::GameDatabase::SaveDisplaySettings(
                launch_options.rom_path, launch_options.title, display);
            SwitchFrontend::OverlayUI::ShowToast(saved ? "画面设置已保存" : "画面设置保存失败");
            block_game_input_until_release = true;
        } else if (menu_action == SwitchFrontend::OverlayUI::Action::CustomLayoutChanged) {
            window.SetDisplaySettings(SwitchFrontend::VulkanOverlay::GetDisplaySettings());
            block_game_input_until_release = true;
        } else if (menu_action == SwitchFrontend::OverlayUI::Action::CustomLayoutCommitted) {
            const auto display = SwitchFrontend::VulkanOverlay::GetDisplaySettings();
            window.SetDisplaySettings(display);
            const bool saved = SwitchFrontend::GameDatabase::SaveDisplaySettings(
                launch_options.rom_path, launch_options.title, display);
            SwitchFrontend::OverlayUI::ShowToast(saved ? "自定义布局已保存"
                                                        : "自定义布局保存失败");
            block_game_input_until_release = true;
        } else if (menu_action ==
                   SwitchFrontend::OverlayUI::Action::FastForwardMultiplierChanged) {
            const float multiplier =
                SwitchFrontend::VulkanOverlay::GetDisplaySettings().fast_forward_multiplier;
            char value[24]{};
            std::snprintf(value, sizeof(value), "%.2f", multiplier);
            SwitchFrontend::GBAStationConfig::SetConfigValue("fastforward.multiplier", value);
            const bool saved = SwitchFrontend::GBAStationConfig::SaveConfig();
            char message[64]{};
            std::snprintf(message, sizeof(message), saved ? "快进倍率已设为 %.2fx"
                                                         : "快进倍率保存失败",
                          multiplier);
            SwitchFrontend::OverlayUI::ShowToast(message);
            block_game_input_until_release = true;
        } else if (menu_action == SwitchFrontend::OverlayUI::Action::OverlaySettingsChanged ||
                   menu_action == SwitchFrontend::OverlayUI::Action::OverlaySettingsCommitted) {
            const auto display = SwitchFrontend::VulkanOverlay::GetDisplaySettings();
            const bool saved = SwitchFrontend::GameDatabase::SaveDisplaySettings(
                launch_options.rom_path, launch_options.title, display);
            SwitchFrontend::OverlayUI::ShowToast(saved ? "遮罩设置已保存"
                                                        : "遮罩设置保存失败");
            block_game_input_until_release = true;
        } else if (menu_action == SwitchFrontend::OverlayUI::Action::SyncOverlaySettings) {
            const auto display = SwitchFrontend::VulkanOverlay::GetDisplaySettings();
            int count = 0;
            const bool db_saved = SwitchFrontend::GameDatabase::SyncDisplaySettings(
                display, false, true, count);
            const bool config_saved = SaveGlobalOverlayDefaults(display);
            char message[96]{};
            std::snprintf(message, sizeof(message),
                          db_saved && config_saved ? "遮罩已同步到 %d 个游戏"
                                                   : "遮罩同步失败",
                          count);
            SwitchFrontend::OverlayUI::ShowToast(message);
            block_game_input_until_release = true;
        } else if (menu_action == SwitchFrontend::OverlayUI::Action::SyncDisplaySettings) {
            const auto display = SwitchFrontend::VulkanOverlay::GetDisplaySettings();
            window.SetDisplaySettings(display);
            if (Settings::values.resolution_factor.GetValue() !=
                static_cast<u32>(display.internal_resolution)) {
                Settings::values.resolution_factor.SetValue(
                    static_cast<u16>(display.internal_resolution));
                system.ApplySettings();
            }
            int count = 0;
            const bool db_saved = SwitchFrontend::GameDatabase::SyncDisplaySettings(
                display, true, false, count);
            const bool config_saved = SaveGlobalScreenDefaults(display);
            char message[96]{};
            std::snprintf(message, sizeof(message),
                          db_saved && config_saved ? "画面设置已同步到 %d 个游戏"
                                                   : "画面设置同步失败",
                          count);
            SwitchFrontend::OverlayUI::ShowToast(message);
            block_game_input_until_release = true;
        } else if (menu_action == SwitchFrontend::OverlayUI::Action::Exit) {
            exit_reason = "GBAStation menu requested exit";
            DebugLog("menu requested exit after %llu iterations",
                     static_cast<unsigned long long>(loop_count));
            ExitLog("menu exit action consumed: iterations=%llu renderer_frame=%d menu_visible=%d",
                    static_cast<unsigned long long>(loop_count), renderer.GetCurrentFrame(),
                    menu_visible ? 1 : 0);
            force_input_suppressed_during_shutdown = true;
            if (menu_initialized) {
                ExitLog("menu exit: VulkanOverlay::PrepareForShutdown begin");
                SwitchFrontend::VulkanOverlay::PrepareForShutdown();
                ExitLog("menu exit: VulkanOverlay::PrepareForShutdown done");
            }
            window.SetInputSuppressed(true);
            InputCommon::SwitchHID::SetInputSuppressed(true);
            ExitLog("menu exit: input suppressed; RequestFastShutdown begin");
            Common::RequestFastShutdown();
            ExitLog("menu exit: RequestFastShutdown done");
            break;
        }

        if (pending_state_request.operation != PendingStateOperation::None &&
            !SwitchFrontend::VulkanOverlay::IsVisible()) {
            if (pending_state_request.delay_frames > 0) {
                --pending_state_request.delay_frames;
            } else {
                const PendingStateOperation operation = pending_state_request.operation;
                const int slot = pending_state_request.slot;
                pending_state_request = {};

                const bool saving = operation == PendingStateOperation::Save;
                const bool ok = saving ? SaveStateFromMenu(system, static_cast<u32>(slot))
                                       : LoadStateFromMenu(system, static_cast<u32>(slot));
                if (ok) {
                    if (saving) {
                        (*slot_state_cache)[slot].store(true, std::memory_order_release);
                    } else {
                        pause_frame_ready = false;
                        pause_frame_baseline = 0;
                    }
                    char message[64]{};
                    std::snprintf(message, sizeof(message), saving ? "已保存到存档位 %d"
                                                                  : "已读取存档位 %d",
                                  slot);
                    SwitchFrontend::OverlayUI::ShowToast(message);
                }
                block_game_input_until_release = true;
                if (!saving) {
                    // LoadState can recreate renderer/system internals.  Do not touch references
                    // captured before the load; restart the loop and reacquire them.
                    continue;
                }
            }
        }

        if (!menu_initialized && ExitComboPressed()) {
            exit_reason = "ZL+ZR fallback exit";
            DebugLog("menu unavailable; fallback exit combo after %llu iterations",
                     static_cast<unsigned long long>(loop_count));
            ExitLog("fallback exit combo: iterations=%llu renderer_frame=%d",
                    static_cast<unsigned long long>(loop_count), renderer.GetCurrentFrame());
            force_input_suppressed_during_shutdown = true;
            window.SetInputSuppressed(true);
            InputCommon::SwitchHID::SetInputSuppressed(true);
            ExitLog("fallback exit: input suppressed; RequestFastShutdown begin");
            Common::RequestFastShutdown();
            ExitLog("fallback exit: RequestFastShutdown done");
            break;
        }

        const bool pause_for_menu =
            menu_initialized && SwitchFrontend::VulkanOverlay::IsVisible() && pause_frame_ready;
        if (pause_for_menu) {
            static_cast<Vulkan::RendererVulkan&>(renderer).PresentLastFrame();
            ++loop_count;
            svcSleepThread(16666667);
            continue;
        }

        const auto runloop_started = Clock::now();
        const Core::System::ResultStatus run_result = system.RunLoop();
        const double runloop_ms =
            std::chrono::duration<double, std::milli>(Clock::now() - runloop_started).count();
        diagnostic_runloop_ms_total += runloop_ms;
        diagnostic_runloop_ms_max = std::max(diagnostic_runloop_ms_max, runloop_ms);
        diagnostic_runloop_count++;
        loop_count++;
        if (pending_overlay_reinit && run_result == Core::System::ResultStatus::Success &&
            system.IsPoweredOn()) {
            auto& reset_renderer = system.GPU().Renderer();
            SwitchFrontend::VulkanOverlay::SetDisplaySettings(
                SwitchFrontend::VulkanOverlay::GetDisplaySettings());
            menu_initialized = SwitchFrontend::VulkanOverlay::Init(
                static_cast<Vulkan::RendererVulkan&>(reset_renderer));
            SwitchFrontend::VulkanOverlay::SetFpsOverlay(show_fps_overlay, 0.0f);
            pending_overlay_reinit = false;
            pause_frame_baseline = reset_renderer.GetCurrentFrame();
            last_logged_frame = pause_frame_baseline;
            last_heartbeat_frame = pause_frame_baseline;
            DebugLog("GBAStation menu reinit after reset=%d frame=%d",
                     menu_initialized ? 1 : 0, pause_frame_baseline);
        }
        const s32 renderer_frame =
            system.IsPoweredOn() ? system.GPU().Renderer().GetCurrentFrame() : last_logged_frame;
        if (!pause_frame_ready &&
            system.GetSaveStateStatus() == Core::System::SaveStateStatus::NONE &&
            renderer_frame > pause_frame_baseline) {
            pause_frame_ready = true;
            DebugLog("pause frame ready after state/reset baseline=%d frame=%d",
                     pause_frame_baseline, renderer_frame);
        }
        if (!saw_guest_frame && renderer_frame > 0) {
            saw_guest_frame = true;
            DebugLog("first guest frame reached: renderer_frame=%d keepalives=%llu", renderer_frame,
                     static_cast<unsigned long long>(keepalive_count));
        }
        if (renderer_frame != last_logged_frame) {
            last_logged_frame = renderer_frame;
        }
        const auto heartbeat_now = Clock::now();
        if (heartbeat_now - last_heartbeat >= std::chrono::seconds(1)) {
            const auto heartbeat_elapsed =
                std::chrono::duration<double>(heartbeat_now - last_heartbeat).count();
            const u64 loop_delta = loop_count - last_heartbeat_loop_count;
            const s32 frame_delta = renderer_frame - last_heartbeat_frame;
            const double frontend_fps =
                heartbeat_elapsed > 0.0 ? static_cast<double>(frame_delta) / heartbeat_elapsed
                                        : 0.0;
            show_fps_overlay = ParseConfigBool(
                SwitchFrontend::GBAStationConfig::GetConfigValue("display.showFps", "0"), false);
            SwitchFrontend::VulkanOverlay::SetFpsOverlay(show_fps_overlay,
                                                         static_cast<float>(frontend_fps));
            const double loops_per_sec =
                heartbeat_elapsed > 0.0 ? static_cast<double>(loop_delta) / heartbeat_elapsed
                                        : 0.0;
            const double runloop_avg_ms =
                diagnostic_runloop_count > 0
                    ? diagnostic_runloop_ms_total / static_cast<double>(diagnostic_runloop_count)
                    : 0.0;
            const double runloop_max_ms = diagnostic_runloop_ms_max;
            const auto runloop_stats = Core::GetAndResetRunLoopDiagnostics();
            const double runloop_ticks_avg =
                runloop_stats.calls > 0
                    ? static_cast<double>(runloop_stats.executed_ticks) /
                          static_cast<double>(runloop_stats.calls)
                    : 0.0;
            const double runloop_delay_avg =
                runloop_stats.delayed_runs > 0
                    ? static_cast<double>(runloop_stats.max_delay_ticks) /
                          static_cast<double>(runloop_stats.delayed_runs)
                    : 0.0;
            const double runloop_slice_avg =
                runloop_stats.sync_runs > 0
                    ? static_cast<double>(runloop_stats.max_slice_ticks) /
                          static_cast<double>(runloop_stats.sync_runs)
                    : 0.0;
            const auto runloop_entry_pc0_code =
                ReadGuestCodeWords(system, runloop_stats.top_entry_pcs[0]);
            const auto runloop_entry_pc1_code =
                ReadGuestCodeWords(system, runloop_stats.top_entry_pcs[1]);
            const auto runloop_entry_lr0_code =
                ReadGuestCodeWordsAroundReturn(system, runloop_stats.top_entry_lrs[0]);
            const auto runloop_entry_lr1_code =
                ReadGuestCodeWordsAroundReturn(system, runloop_stats.top_entry_lrs[1]);
            const auto runloop_exit_pc0_code = ReadGuestCodeWords(system, runloop_stats.top_pcs[0]);
            const auto runloop_exit_pc1_code = ReadGuestCodeWords(system, runloop_stats.top_pcs[1]);
            const auto timing_stats = system.CoreTiming().GetAndResetDiagnostics();
            const auto transfer_stats = VideoCore::GetAndResetTransferDiagnostics();
            const auto y2r_stats = HW::Y2R::GetAndResetDiagnostics();
            const auto service_stats = Service::GetAndResetDiagnostics();
            const auto dynarmic_stats = Core::GetAndResetDynarmicDiagnostics();
            const auto dispatcher0_code =
                ReadGuestCodeWords(system, static_cast<u32>(dynarmic_stats.top_dispatcher_descriptors[0]));
            const auto dispatcher1_code =
                ReadGuestCodeWords(system, static_cast<u32>(dynarmic_stats.top_dispatcher_descriptors[1]));
            static bool logged_video_dispatch_window = false;
            if (!logged_video_dispatch_window && y2r_stats.conversions > 0) {
                constexpr u32 VideoDispatchHotPc = 0x001228e0;
                const auto hot_code = ReadGuestCodeWindow(system, VideoDispatchHotPc);
                HeartbeatLog(
                    "jit video hot window: pc=0x%08x code=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                    VideoDispatchHotPc, hot_code[0], hot_code[1], hot_code[2], hot_code[3],
                    hot_code[4], hot_code[5], hot_code[6], hot_code[7], hot_code[8], hot_code[9],
                    hot_code[10], hot_code[11], hot_code[12], hot_code[13], hot_code[14],
                    hot_code[15]);
                logged_video_dispatch_window = true;
            }
            const auto dsp_stats = AudioCore::GetAndResetDspHleDiagnostics();
            const auto thread_wakeup_stats = Kernel::GetAndResetThreadWakeupDiagnostics();
            const double timing_slice_avg =
                timing_stats.slice_count > 0
                    ? static_cast<double>(timing_stats.slice_total) /
                          static_cast<double>(timing_stats.slice_count)
                    : 0.0;
            const double short_slice_pct =
                timing_stats.slice_count > 0
                    ? static_cast<double>(timing_stats.short_slices) * 100.0 /
                          static_cast<double>(timing_stats.slice_count)
                    : 0.0;
            const double idle_pct =
                timing_stats.slice_total > 0
                    ? static_cast<double>(timing_stats.idle_ticks) * 100.0 /
                          static_cast<double>(timing_stats.slice_total)
                    : 0.0;
            const double thread_wakeup_avg_us =
                thread_wakeup_stats.scheduled > 0
                    ? static_cast<double>(thread_wakeup_stats.total_ns) /
                          static_cast<double>(thread_wakeup_stats.scheduled) / 1000.0
                    : 0.0;
            const double dsp_active_avg =
                dsp_stats.ticks > 0 ? static_cast<double>(dsp_stats.active_source_sum) /
                                          static_cast<double>(dsp_stats.ticks)
                                    : 0.0;
            const auto thread_wakeup_status_count = [&](Kernel::ThreadStatus status) {
                const auto index = static_cast<std::size_t>(status);
                return index < thread_wakeup_stats.status_counts.size()
                           ? thread_wakeup_stats.status_counts[index]
                           : 0;
            };
            const auto thread_wakeup_source_count = [&](Kernel::ThreadWakeupSource source) {
                const auto index = static_cast<std::size_t>(source);
                return index < thread_wakeup_stats.source_counts.size()
                           ? thread_wakeup_stats.source_counts[index]
                           : 0;
            };
            const auto stats = system.GetAndResetPerfStats();
            HeartbeatLog("main loop heartbeat: iterations=%llu loops_per_sec=%.1f renderer_frame=%d frame_delta=%d frontend_fps=%.1f system_fps=%.1f game_fps=%.1f emu_speed=%.2f hle_svc_ms=%.2f hle_ipc_ms=%.2f hle_gpu_ms=%.2f swap_ms=%.2f remaining_ms=%.2f svc_ipc=%llu svc_unimpl=%llu svc_last_unimpl=0x%04x mvd_calls=%llu mvd_unimpl=%llu runloop_avg_ms=%.2f runloop_max_ms=%.2f rl_calls=%llu rl_active=%llu rl_idle=%llu rl_delayed=%llu rl_sync=%llu rl_resched=%llu rl_ticks=%llu rl_ticks_avg=%.1f rl_delay_avg=%.1f rl_slice_avg=%.1f rl_epc_samples=%llu rl_epc0=0x%08x/0x%08x:%llu rl_epc1=0x%08x/0x%08x:%llu rl_epc2=0x%08x/0x%08x:%llu rl_epc3=0x%08x/0x%08x:%llu rl_ecode0=%08x,%08x,%08x,%08x rl_ecode1=%08x,%08x,%08x,%08x rl_elrctx0=%08x,%08x,%08x,%08x rl_elrctx1=%08x,%08x,%08x,%08x rl_xpc_samples=%llu rl_xpc0=0x%08x:%llu rl_xpc1=0x%08x:%llu rl_xpc2=0x%08x:%llu rl_xpc3=0x%08x:%llu rl_xcode0=%08x,%08x,%08x,%08x rl_xcode1=%08x,%08x,%08x,%08x pending_compilations=%zu jit_new=%llu jit_icache_clear=%llu jit_inv=%llu jit_inv_kb=%.1f jit_cache_mb=%.1f jit_opt=0x%08x jit_hook_hints=%u jit_little=%u jit_fastmem=%u jit_mem_r=%llu jit_mem_w=%llu jit_mem_x=%llu jit_mem_code=%llu jit_fd_miss=%llu jit_fd_update=%llu jit_fd_clear=%llu jit_fd_false=%llu jit_disp_hit=%llu jit_disp_miss=%llu jit_disp_collision=%llu jit_disp0=0x%08llx:%llu jit_disp1=0x%08llx:%llu jit_disp2=0x%08llx:%llu jit_disp3=0x%08llx:%llu jit_dcode0=%08x,%08x,%08x,%08x jit_dcode1=%08x,%08x,%08x,%08x jit_mem_last_r=0x%08x jit_mem_last_w=0x%08x dsp_ticks=%llu dsp_irq=%llu dsp_active_avg=%.1f dsp_active_max=%llu dsp_ms=%.2f dsp_gen_ms=%.2f dsp_out_ms=%.2f timing_advances=%llu timing_events=%llu timing_top=%s timing_top_count=%llu tw_sched=%llu tw_fire=%llu tw_forever=%llu tw_top_id=0x%08x tw_top_core=%u tw_top_name=%s tw_top_status=%s tw_top_count=%llu tw_avg_us=%.1f tw_min_us=%.1f tw_max_us=%.1f tw_le100us=%llu tw_le500us=%llu tw_le1ms=%llu tw_le2ms=%llu tw_le5ms=%llu tw_le16ms=%llu tw_gt16ms=%llu tw_st_sleep=%llu tw_st_any=%llu tw_st_all=%llu tw_st_hle=%llu tw_st_arb=%llu tw_src_generic=%llu tw_src_sleep=%llu tw_src_wait1=%llu tw_src_waitn_all=%llu tw_src_waitn_any=%llu tw_src_hle_sleep=%llu tw_src_hle_async=%llu tw_src_hle_thread=%llu tw_src_arb=%llu tw_src_appmain=%llu tw_src_ipc=%llu timing_slice_avg=%.1f timing_slice_min=%lld timing_slice_max=%lld timing_short_pct=%.1f timing_idle_pct=%.1f gpu_display=%llu gpu_display_sw=%llu gpu_display_mb=%.2f gpu_texcopy=%llu gpu_texcopy_sw=%llu gpu_texcopy_mb=%.2f y2r=%llu y2r_direct=%llu y2r_fallback=%llu y2r_pixels=%llu y2r_direct_pixels=%llu y2r_ms=%.2f y2r_direct_ms=%.2f y2r_fallback_ms=%.2f y2r_flush=%llu y2r_flush_inv=%llu y2r_flush_mb=%.2f y2r_flush_ms=%.2f y2r_dir_fmt=%u/%u y2r_dir_rot=%u y2r_dir_block=%u y2r_dir_size=%ux%u y2r_dir_dst=%u+%u y2r_fb_fmt=%u/%u y2r_fb_rot=%u y2r_fb_block=%u y2r_fb_size=%ux%u y2r_fb_dma=y%u+%u,u%u+%u,v%u+%u,yuyv%u+%u,dst%u+%u powered=%d applet=%d keepalives=%llu",
                         static_cast<unsigned long long>(loop_count), loops_per_sec,
                         renderer_frame, frame_delta, frontend_fps, stats.system_fps,
                         stats.game_fps, stats.emulation_speed, stats.time_hle_svc * 1000.0,
                         stats.time_hle_ipc * 1000.0, stats.time_gpu * 1000.0,
                         stats.time_swap * 1000.0, stats.time_remaining * 1000.0,
                         static_cast<unsigned long long>(service_stats.ipc_calls),
                         static_cast<unsigned long long>(service_stats.unimplemented_calls),
                         service_stats.last_unimplemented_command,
                         static_cast<unsigned long long>(service_stats.mvd_calls),
                         static_cast<unsigned long long>(service_stats.mvd_unimplemented_calls),
                         runloop_avg_ms, runloop_max_ms,
                         static_cast<unsigned long long>(runloop_stats.calls),
                         static_cast<unsigned long long>(runloop_stats.active_runs),
                         static_cast<unsigned long long>(runloop_stats.idle_runs),
                         static_cast<unsigned long long>(runloop_stats.delayed_runs),
                         static_cast<unsigned long long>(runloop_stats.sync_runs),
                         static_cast<unsigned long long>(runloop_stats.reschedules),
                         static_cast<unsigned long long>(runloop_stats.executed_ticks),
                         runloop_ticks_avg, runloop_delay_avg, runloop_slice_avg,
                         static_cast<unsigned long long>(runloop_stats.entry_pc_samples),
                         runloop_stats.top_entry_pcs[0], runloop_stats.top_entry_lrs[0],
                         static_cast<unsigned long long>(runloop_stats.top_entry_pc_counts[0]),
                         runloop_stats.top_entry_pcs[1], runloop_stats.top_entry_lrs[1],
                         static_cast<unsigned long long>(runloop_stats.top_entry_pc_counts[1]),
                         runloop_stats.top_entry_pcs[2], runloop_stats.top_entry_lrs[2],
                         static_cast<unsigned long long>(runloop_stats.top_entry_pc_counts[2]),
                         runloop_stats.top_entry_pcs[3], runloop_stats.top_entry_lrs[3],
                         static_cast<unsigned long long>(runloop_stats.top_entry_pc_counts[3]),
                         runloop_entry_pc0_code[0], runloop_entry_pc0_code[1],
                         runloop_entry_pc0_code[2], runloop_entry_pc0_code[3],
                         runloop_entry_pc1_code[0], runloop_entry_pc1_code[1],
                         runloop_entry_pc1_code[2], runloop_entry_pc1_code[3],
                         runloop_entry_lr0_code[0], runloop_entry_lr0_code[1],
                         runloop_entry_lr0_code[2], runloop_entry_lr0_code[3],
                         runloop_entry_lr1_code[0], runloop_entry_lr1_code[1],
                         runloop_entry_lr1_code[2], runloop_entry_lr1_code[3],
                         static_cast<unsigned long long>(runloop_stats.pc_samples),
                         runloop_stats.top_pcs[0],
                         static_cast<unsigned long long>(runloop_stats.top_pc_counts[0]),
                         runloop_stats.top_pcs[1],
                         static_cast<unsigned long long>(runloop_stats.top_pc_counts[1]),
                         runloop_stats.top_pcs[2],
                         static_cast<unsigned long long>(runloop_stats.top_pc_counts[2]),
                         runloop_stats.top_pcs[3],
                         static_cast<unsigned long long>(runloop_stats.top_pc_counts[3]),
                         runloop_exit_pc0_code[0], runloop_exit_pc0_code[1],
                         runloop_exit_pc0_code[2], runloop_exit_pc0_code[3],
                         runloop_exit_pc1_code[0], runloop_exit_pc1_code[1],
                         runloop_exit_pc1_code[2], runloop_exit_pc1_code[3],
                         pending_compilations,
                         static_cast<unsigned long long>(dynarmic_stats.jit_instances_created),
                         static_cast<unsigned long long>(
                             dynarmic_stats.instruction_cache_clears),
                         static_cast<unsigned long long>(
                             dynarmic_stats.cache_range_invalidations),
                         static_cast<double>(dynarmic_stats.cache_range_invalidation_bytes) /
                             1024.0,
                         static_cast<double>(dynarmic_stats.last_code_cache_size) /
                             (1024.0 * 1024.0),
                         dynarmic_stats.last_optimization_flags,
                         dynarmic_stats.last_hook_hint_instructions,
                         dynarmic_stats.last_always_little_endian,
                         dynarmic_stats.last_fastmem_enabled,
                         static_cast<unsigned long long>(dynarmic_stats.memory_read_callbacks),
                         static_cast<unsigned long long>(dynarmic_stats.memory_write_callbacks),
                         static_cast<unsigned long long>(
                             dynarmic_stats.memory_exclusive_callbacks),
                         static_cast<unsigned long long>(dynarmic_stats.memory_code_callbacks),
                         static_cast<unsigned long long>(dynarmic_stats.fast_dispatch_misses),
                         static_cast<unsigned long long>(dynarmic_stats.fast_dispatch_updates),
                         static_cast<unsigned long long>(dynarmic_stats.fast_dispatch_clears),
                         static_cast<unsigned long long>(
                             dynarmic_stats.fast_dispatch_false_misses),
                         static_cast<unsigned long long>(dynarmic_stats.dispatcher_cache_hits),
                         static_cast<unsigned long long>(dynarmic_stats.dispatcher_cache_misses),
                         static_cast<unsigned long long>(
                             dynarmic_stats.dispatcher_cache_collisions),
                         static_cast<unsigned long long>(
                             dynarmic_stats.top_dispatcher_descriptors[0]),
                         static_cast<unsigned long long>(dynarmic_stats.top_dispatcher_descriptor_counts[0]),
                         static_cast<unsigned long long>(
                             dynarmic_stats.top_dispatcher_descriptors[1]),
                         static_cast<unsigned long long>(dynarmic_stats.top_dispatcher_descriptor_counts[1]),
                         static_cast<unsigned long long>(
                             dynarmic_stats.top_dispatcher_descriptors[2]),
                         static_cast<unsigned long long>(dynarmic_stats.top_dispatcher_descriptor_counts[2]),
                         static_cast<unsigned long long>(
                             dynarmic_stats.top_dispatcher_descriptors[3]),
                         static_cast<unsigned long long>(dynarmic_stats.top_dispatcher_descriptor_counts[3]),
                         dispatcher0_code[0], dispatcher0_code[1], dispatcher0_code[2],
                         dispatcher0_code[3], dispatcher1_code[0], dispatcher1_code[1],
                         dispatcher1_code[2], dispatcher1_code[3],
                         dynarmic_stats.last_read_callback_addr,
                         dynarmic_stats.last_write_callback_addr,
                         static_cast<unsigned long long>(dsp_stats.ticks),
                         static_cast<unsigned long long>(dsp_stats.interrupts), dsp_active_avg,
                         static_cast<unsigned long long>(dsp_stats.active_source_max),
                         dsp_stats.tick_ms, dsp_stats.generate_ms, dsp_stats.output_ms,
                         static_cast<unsigned long long>(timing_stats.advance_calls),
                         static_cast<unsigned long long>(timing_stats.events_processed),
                         timing_stats.busiest_event_name.empty()
                             ? "-"
                             : timing_stats.busiest_event_name.c_str(),
                         static_cast<unsigned long long>(timing_stats.busiest_event_count),
                         static_cast<unsigned long long>(thread_wakeup_stats.scheduled),
                         static_cast<unsigned long long>(thread_wakeup_stats.fired),
                         static_cast<unsigned long long>(thread_wakeup_stats.forever),
                         thread_wakeup_stats.busiest_thread_id,
                         static_cast<unsigned>(thread_wakeup_stats.busiest_thread_core),
                         thread_wakeup_stats.busiest_thread_name.empty()
                             ? "-"
                             : thread_wakeup_stats.busiest_thread_name.c_str(),
                         thread_wakeup_stats.busiest_thread_status_name.empty()
                             ? "-"
                             : thread_wakeup_stats.busiest_thread_status_name.c_str(),
                         static_cast<unsigned long long>(
                             thread_wakeup_stats.busiest_thread_count),
                         thread_wakeup_avg_us,
                         static_cast<double>(thread_wakeup_stats.min_ns) / 1000.0,
                         static_cast<double>(thread_wakeup_stats.max_ns) / 1000.0,
                         static_cast<unsigned long long>(thread_wakeup_stats.delay_buckets[0]),
                         static_cast<unsigned long long>(thread_wakeup_stats.delay_buckets[1]),
                         static_cast<unsigned long long>(thread_wakeup_stats.delay_buckets[2]),
                         static_cast<unsigned long long>(thread_wakeup_stats.delay_buckets[3]),
                         static_cast<unsigned long long>(thread_wakeup_stats.delay_buckets[4]),
                         static_cast<unsigned long long>(thread_wakeup_stats.delay_buckets[5]),
                         static_cast<unsigned long long>(thread_wakeup_stats.delay_buckets[6]),
                         static_cast<unsigned long long>(
                             thread_wakeup_status_count(Kernel::ThreadStatus::WaitSleep)),
                         static_cast<unsigned long long>(
                             thread_wakeup_status_count(Kernel::ThreadStatus::WaitSynchAny)),
                         static_cast<unsigned long long>(
                             thread_wakeup_status_count(Kernel::ThreadStatus::WaitSynchAll)),
                         static_cast<unsigned long long>(
                             thread_wakeup_status_count(Kernel::ThreadStatus::WaitHleEvent)),
                         static_cast<unsigned long long>(
                             thread_wakeup_status_count(Kernel::ThreadStatus::WaitArb)),
                         static_cast<unsigned long long>(
                             thread_wakeup_source_count(Kernel::ThreadWakeupSource::Generic)),
                         static_cast<unsigned long long>(thread_wakeup_source_count(
                             Kernel::ThreadWakeupSource::SvcSleepThread)),
                         static_cast<unsigned long long>(thread_wakeup_source_count(
                             Kernel::ThreadWakeupSource::WaitSynchronization1)),
                         static_cast<unsigned long long>(thread_wakeup_source_count(
                             Kernel::ThreadWakeupSource::WaitSynchronizationNAll)),
                         static_cast<unsigned long long>(thread_wakeup_source_count(
                             Kernel::ThreadWakeupSource::WaitSynchronizationNAny)),
                         static_cast<unsigned long long>(thread_wakeup_source_count(
                             Kernel::ThreadWakeupSource::HleSleepClientThread)),
                         static_cast<unsigned long long>(thread_wakeup_source_count(
                             Kernel::ThreadWakeupSource::HleRunAsync)),
                         static_cast<unsigned long long>(thread_wakeup_source_count(
                             Kernel::ThreadWakeupSource::HleRunOnThread)),
                         static_cast<unsigned long long>(
                             thread_wakeup_source_count(Kernel::ThreadWakeupSource::AddressArbiter)),
                         static_cast<unsigned long long>(
                             thread_wakeup_source_count(Kernel::ThreadWakeupSource::AppMainDelay)),
                         static_cast<unsigned long long>(
                             thread_wakeup_source_count(Kernel::ThreadWakeupSource::IpcDelay)),
                         timing_slice_avg, static_cast<long long>(timing_stats.min_slice),
                         static_cast<long long>(timing_stats.max_slice), short_slice_pct, idle_pct,
                         static_cast<unsigned long long>(transfer_stats.display_transfer_count),
                         static_cast<unsigned long long>(
                             transfer_stats.software_display_transfer_count),
                         static_cast<double>(transfer_stats.display_transfer_bytes) /
                             (1024.0 * 1024.0),
                         static_cast<unsigned long long>(transfer_stats.texture_copy_count),
                         static_cast<unsigned long long>(
                             transfer_stats.software_texture_copy_count),
                         static_cast<double>(transfer_stats.texture_copy_bytes) /
                             (1024.0 * 1024.0),
                         static_cast<unsigned long long>(y2r_stats.conversions),
                         static_cast<unsigned long long>(y2r_stats.direct_conversions),
                         static_cast<unsigned long long>(y2r_stats.fallback_conversions),
                         static_cast<unsigned long long>(y2r_stats.pixels),
                         static_cast<unsigned long long>(y2r_stats.direct_pixels),
                         y2r_stats.total_ms, y2r_stats.direct_ms, y2r_stats.fallback_ms,
                         static_cast<unsigned long long>(y2r_stats.pre_flushes),
                         static_cast<unsigned long long>(y2r_stats.pre_flush_invalidate_only),
                         static_cast<double>(y2r_stats.pre_flush_bytes) / (1024.0 * 1024.0),
                         y2r_stats.pre_flush_ms,
                         static_cast<unsigned>(y2r_stats.last_direct_input_format),
                         static_cast<unsigned>(y2r_stats.last_direct_output_format),
                         static_cast<unsigned>(y2r_stats.last_direct_rotation),
                         static_cast<unsigned>(y2r_stats.last_direct_block_alignment),
                         static_cast<unsigned>(y2r_stats.last_direct_width),
                         static_cast<unsigned>(y2r_stats.last_direct_lines),
                         static_cast<unsigned>(y2r_stats.last_direct_dst_unit),
                         static_cast<unsigned>(y2r_stats.last_direct_dst_gap),
                         static_cast<unsigned>(y2r_stats.last_fallback_input_format),
                         static_cast<unsigned>(y2r_stats.last_fallback_output_format),
                         static_cast<unsigned>(y2r_stats.last_fallback_rotation),
                         static_cast<unsigned>(y2r_stats.last_fallback_block_alignment),
                         static_cast<unsigned>(y2r_stats.last_fallback_width),
                         static_cast<unsigned>(y2r_stats.last_fallback_lines),
                         static_cast<unsigned>(y2r_stats.last_fallback_y_unit),
                         static_cast<unsigned>(y2r_stats.last_fallback_y_gap),
                         static_cast<unsigned>(y2r_stats.last_fallback_u_unit),
                         static_cast<unsigned>(y2r_stats.last_fallback_u_gap),
                         static_cast<unsigned>(y2r_stats.last_fallback_v_unit),
                         static_cast<unsigned>(y2r_stats.last_fallback_v_gap),
                         static_cast<unsigned>(y2r_stats.last_fallback_yuyv_unit),
                         static_cast<unsigned>(y2r_stats.last_fallback_yuyv_gap),
                         static_cast<unsigned>(y2r_stats.last_fallback_dst_unit),
                         static_cast<unsigned>(y2r_stats.last_fallback_dst_gap),
                         system.IsPoweredOn() ? 1 : 0,
                         applet_loop_active ? 1 : 0,
                         static_cast<unsigned long long>(keepalive_count));
            last_heartbeat = heartbeat_now;
            last_heartbeat_loop_count = loop_count;
            last_heartbeat_frame = renderer_frame;
            diagnostic_runloop_count = 0;
            diagnostic_runloop_ms_total = 0.0;
            diagnostic_runloop_ms_max = 0.0;
        }
        const auto unflushed_play_time =
            std::chrono::duration_cast<std::chrono::seconds>(now - play_stats_checkpoint).count();
        if (unflushed_play_time >= 30 && SwitchFrontend::GameDatabase::UpdatePlayStats(
                                                  launch_options.rom_path,
                                                  launch_options.title, false,
                                                  static_cast<int>(unflushed_play_time))) {
            play_stats_checkpoint += std::chrono::seconds(unflushed_play_time);
        }
        if (run_result == Core::System::ResultStatus::ShutdownRequested) {
            exit_reason = "RunLoop returned ShutdownRequested";
            DebugLog("core requested shutdown after %llu iterations",
                     static_cast<unsigned long long>(loop_count));
            ExitLog("main loop exit: RunLoop ShutdownRequested iterations=%llu",
                    static_cast<unsigned long long>(loop_count));
            break;
        }
        if (run_result == Core::System::ResultStatus::ErrorSavestate) {
            const std::string details = system.GetStatusDetails();
            DebugLog("savestate operation failed after %llu iterations: %s",
                     static_cast<unsigned long long>(loop_count),
                     details.empty() ? "unknown error" : details.c_str());
            if (menu_initialized) {
                SwitchFrontend::OverlayUI::ShowToast("状态操作失败");
            }
            continue;
        }
        if (run_result != Core::System::ResultStatus::Success) {
            exit_reason = "RunLoop returned error";
            DebugLog("run loop failed after %llu iterations: %s (%u)",
                     static_cast<unsigned long long>(loop_count), ResultStatusName(run_result),
                     static_cast<unsigned>(run_result));
            ExitLog("main loop exit: RunLoop error iterations=%llu status=%s (%u)",
                    static_cast<unsigned long long>(loop_count), ResultStatusName(run_result),
                    static_cast<unsigned>(run_result));
            break;
        }
    }

    DebugLog("shutting down: reason=%s powered=%d applet=%d iterations=%llu", exit_reason,
             system.IsPoweredOn() ? 1 : 0, applet_loop_active ? 1 : 0,
             static_cast<unsigned long long>(loop_count));
    ExitLog("shutdown begin: reason=%s powered=%d applet=%d iterations=%llu", exit_reason,
            system.IsPoweredOn() ? 1 : 0, applet_loop_active ? 1 : 0,
            static_cast<unsigned long long>(loop_count));
    ExitLogMemorySummary("shutdown-begin");
    const auto remaining_play_time = std::chrono::duration_cast<std::chrono::seconds>(
                                         Clock::now() - play_stats_checkpoint)
                                         .count();
    if (remaining_play_time > 0) {
        ExitLog("shutdown step: UpdatePlayStats begin seconds=%lld",
                static_cast<long long>(remaining_play_time));
        SwitchFrontend::GameDatabase::UpdatePlayStats(
            launch_options.rom_path, launch_options.title, false,
            static_cast<int>(remaining_play_time));
        ExitLog("shutdown step: UpdatePlayStats done");
    }
    ExitLog("shutdown step: restore temporary settings begin");
    Settings::values.use_vsync.SetValue(normal_vsync);
    if (mic_input_simulated) {
        Settings::values.input_type.SetValue(mic_restore_input_type);
        mic_input_simulated = false;
    }
    SwitchFrontend::VulkanOverlay::SetFastForwardActive(false);
    Settings::ResetTemporaryFrameLimit();
    ExitLog("shutdown step: restore temporary settings done");
    const auto shutdown_started = Clock::now();
    if (menu_initialized) {
        ExitLog("shutdown step: VulkanOverlay::PrepareForShutdown begin");
        SwitchFrontend::VulkanOverlay::PrepareForShutdown();
        ExitLog("shutdown step: VulkanOverlay::PrepareForShutdown done");
    }
    if (menu_audio_muted) {
        Settings::values.volume.SetValue(menu_restore_volume);
        menu_audio_muted = false;
    }
    if (force_input_suppressed_during_shutdown) {
        window.SetInputSuppressed(true);
        InputCommon::SwitchHID::SetInputSuppressed(true);
    }
    if (menu_initialized) {
        DebugLog("shutdown step: VulkanOverlay::Shutdown begin");
        ExitLog("shutdown step: VulkanOverlay::Shutdown begin");
        const auto step_started = Clock::now();
        SwitchFrontend::VulkanOverlay::Shutdown();
        menu_initialized = false;
        DebugLog("shutdown step: VulkanOverlay::Shutdown done");
        ExitLog("shutdown step: VulkanOverlay::Shutdown done (%lld ms)",
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           Clock::now() - step_started)
                                           .count()));
        ExitLogMemorySummary("after-overlay-shutdown");
    }
    if (system.IsPoweredOn()) {
        DebugLog("shutdown step: system.Shutdown begin");
        ExitLog("shutdown step: system.Shutdown begin");
        const auto step_started = Clock::now();
        system.Shutdown();
        const auto step_ms = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - step_started)
                .count());
        DebugLog("shutdown step: system.Shutdown done (%lld ms)", step_ms);
        ExitLog("shutdown step: system.Shutdown done (%lld ms)", step_ms);
        ExitLogMemorySummary("after-system-shutdown");
    }
    ExitLog("shutdown step: clear input suppression begin");
    window.SetInputSuppressed(false);
    InputCommon::SwitchHID::SetInputSuppressed(false);
    ExitLog("shutdown step: clear input suppression done");
    DebugLog("shutdown step: InputCommon::Shutdown begin");
    ExitLog("shutdown step: InputCommon::Shutdown begin");
    InputCommon::Shutdown();
    DebugLog("shutdown step: InputCommon::Shutdown done");
    ExitLog("shutdown step: InputCommon::Shutdown done");
    DebugLog("shutdown step: Common::Log::Stop begin");
    ExitLog("shutdown step: Common::Log::Stop begin active=%d", common_log_started ? 1 : 0);
    if (common_log_started) {
        Common::Log::Stop();
        common_log_started = false;
    }
    DebugLog("shutdown step: Common::Log::Stop done");
    ExitLog("shutdown step: Common::Log::Stop done");
    if (romfs_initialized) {
        DebugLog("shutdown step: romfsExit begin");
        ExitLog("shutdown step: romfsExit begin");
        romfsExit();
        DebugLog("shutdown step: romfsExit done");
        ExitLog("shutdown step: romfsExit done");
    }
    ExitLog("shutdown step: thread_core_guard.Restore begin");
    thread_core_guard.Restore("normal-shutdown");
    ExitLog("shutdown step: thread_core_guard.Restore done");
    ExitLogMemorySummary("before-launcher-return");
    DebugLog("shutdown total before launcher: %lld ms",
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        Clock::now() - shutdown_started)
                                        .count()));
    ExitLog("shutdown total before launcher: %lld ms",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       Clock::now() - shutdown_started)
                                       .count()));
    DebugLog("shutdown step: DebugClose begin");
    ExitLog("shutdown step: DebugClose begin");
    DebugClose();
    const bool queued_launcher_return = QueueLauncherReturn(launch_options);
    ExitLog("shutdown step: QueueLauncherReturn done queued=%d",
            queued_launcher_return ? 1 : 0);
    DumpMemoryMap("after-shutdown");
    ExitLogMemorySummary("after-shutdown");
    StartupLog("Run: appletUnlockExit begin");
    ExitLog("shutdown step: appletUnlockExit begin");
    const LibnxResult unlock_exit_rc = appletUnlockExit();
    StartupLog("Run: appletUnlockExit rc=0x%x", unlock_exit_rc);
    ExitLog("shutdown step: appletUnlockExit done rc=0x%x", unlock_exit_rc);
    ExitLog("Run returning result=%d", queued_launcher_return ? EXIT_SUCCESS : EXIT_FAILURE);
    return queued_launcher_return ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

extern "C" void userAppInit() {
    ApplyDiagnosticLogSwitchFromEnv();
    WriteRawBootMarker("userAppInit: entry");
    LogTlsLayoutEarly("userAppInit TLS");
    OpenStartupLogIfNeeded("w");
    StartupLog("userAppInit: begin");
    if (EnableStdStreamLogs) {
        std::freopen(StdoutLogPath, "w", stdout);
        std::freopen(StderrLogPath, "w", stderr);
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
    setenv("HOME", SystemDir, 1);
    StartupLog("userAppInit: stdout/stderr logs %s HOME=%s",
               EnableStdStreamLogs ? "enabled" : "disabled", SystemDir);
}

extern "C" void userAppExit() {
    WriteRawBootMarker("userAppExit: entry");
    ExitLog("userAppExit entry");
    ExitLogMemorySummary("userAppExit-entry");
    // Runs after C++ static destructors — the closest observable state to what hbl
    // inherits. Anything still flagged SUSPECT here is what kills hbl with 0xD401.
    DumpMemoryMap("static-dtors-done");
    StartupLog("userAppExit");
    CloseStartupLog();
    ExitLog("userAppExit closing logs");
    ExitLogClose();
}

struct alignas(16) SwitchExceptionResumeContext {
    std::array<u64, 33> registers;
    alignas(16) std::array<u64, 64> fpu_registers;
    u64 pstate;
};

static_assert(offsetof(SwitchExceptionResumeContext, fpu_registers) == 272);
static_assert(offsetof(SwitchExceptionResumeContext, pstate) == 784);

extern "C" bool DynarmicHandleSwitchFastmemFault(u64 host_pc, u64* resume_pc);
extern "C" [[noreturn]] void SwitchFastmemResumeException(
    const SwitchExceptionResumeContext* context);

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    ApplyDiagnosticLogSwitchFromEnv();
    if (ctx) {
        u64 resume_pc{};
        if (DynarmicHandleSwitchFastmemFault(ctx->pc.x, &resume_pc)) {
            SwitchExceptionResumeContext resume_context{};
            for (size_t index = 0; index < 29; index++) {
                resume_context.registers[index] = ctx->cpu_gprs[index].x;
            }
            resume_context.registers[29] = ctx->fp.x;
            resume_context.registers[30] = ctx->lr.x;
            resume_context.registers[31] = ctx->sp.x;
            resume_context.registers[32] = resume_pc;
            std::memcpy(resume_context.fpu_registers.data(), ctx->fpu_gprs,
                        sizeof(ctx->fpu_gprs));
            resume_context.pstate = ctx->pstate;
            SwitchFastmemResumeException(&resume_context);
        }
    }

    WriteRawBootMarker("__libnx_exception_handler: entry");
    OpenStartupLogIfNeeded("a");
    ExitLog("__libnx_exception_handler entry");
    StartupLog("module anchor: exception_handler=%p",
               reinterpret_cast<void*>(&__libnx_exception_handler));
    if (ctx) {
        StartupLog("libnx exception: desc=0x%08x pc=0x%016llx lr=0x%016llx sp=0x%016llx far=0x%016llx esr=0x%08x",
                   static_cast<unsigned>(ctx->error_desc),
                   static_cast<unsigned long long>(ctx->pc.x),
                   static_cast<unsigned long long>(ctx->lr.x),
                   static_cast<unsigned long long>(ctx->sp.x),
                   static_cast<unsigned long long>(ctx->far.x), static_cast<unsigned>(ctx->esr));
        ExitLog("libnx exception: desc=0x%08x pc=0x%016llx lr=0x%016llx sp=0x%016llx far=0x%016llx esr=0x%08x",
                static_cast<unsigned>(ctx->error_desc),
                static_cast<unsigned long long>(ctx->pc.x),
                static_cast<unsigned long long>(ctx->lr.x),
                static_cast<unsigned long long>(ctx->sp.x),
                static_cast<unsigned long long>(ctx->far.x), static_cast<unsigned>(ctx->esr));
        StartupLog("libnx exception: x0=0x%016llx x1=0x%016llx x2=0x%016llx x3=0x%016llx",
                   static_cast<unsigned long long>(ctx->cpu_gprs[0].x),
                   static_cast<unsigned long long>(ctx->cpu_gprs[1].x),
                   static_cast<unsigned long long>(ctx->cpu_gprs[2].x),
                   static_cast<unsigned long long>(ctx->cpu_gprs[3].x));
        ExitLog("libnx exception: x0=0x%016llx x1=0x%016llx x2=0x%016llx x3=0x%016llx",
                static_cast<unsigned long long>(ctx->cpu_gprs[0].x),
                static_cast<unsigned long long>(ctx->cpu_gprs[1].x),
                static_cast<unsigned long long>(ctx->cpu_gprs[2].x),
                static_cast<unsigned long long>(ctx->cpu_gprs[3].x));
        StartupLog("libnx exception: x4=0x%016llx x5=0x%016llx x6=0x%016llx x7=0x%016llx",
                   static_cast<unsigned long long>(ctx->cpu_gprs[4].x),
                   static_cast<unsigned long long>(ctx->cpu_gprs[5].x),
                   static_cast<unsigned long long>(ctx->cpu_gprs[6].x),
                   static_cast<unsigned long long>(ctx->cpu_gprs[7].x));
        ExitLog("libnx exception: x4=0x%016llx x5=0x%016llx x6=0x%016llx x7=0x%016llx",
                static_cast<unsigned long long>(ctx->cpu_gprs[4].x),
                static_cast<unsigned long long>(ctx->cpu_gprs[5].x),
                static_cast<unsigned long long>(ctx->cpu_gprs[6].x),
                static_cast<unsigned long long>(ctx->cpu_gprs[7].x));
    } else {
        StartupLog("libnx exception: null context");
        ExitLog("libnx exception: null context");
    }
}

int main(int argc, char** argv) {
    ApplyDiagnosticLogSwitchFromEnv();
    WriteRawBootMarker("main: entry");
    OpenStartupLogIfNeeded("a");
    StartupLog("main: entry argc=%d", argc);
    ExitLogOpen("w");
    ExitLog("main entry argc=%d", argc);
    const int result = Run(argc, argv);
    StartupLog("main: _Exit result=%d", result);
    ExitLog("main _Exit result=%d", result);
    CloseStartupLog();
    ExitLogClose();
    std::_Exit(result);
}
