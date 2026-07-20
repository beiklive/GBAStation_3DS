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
#include <exception>
#include <fcntl.h>
#include <memory>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#include "audio_core/input_details.h"
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
#include "core/core.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "core/savestate.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/image_interface.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/service.h"
#include "input_common/main.h"
#include "input_common/switch_hid.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"

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
constexpr const char* BootMarkerPath = "sdmc:/GBAStation/3ds/azahar_boot.txt";
constexpr const char* StartupLogPath = "sdmc:/GBAStation/3ds/debug/startup.txt";
constexpr const char* DebugLogPath = "sdmc:/GBAStation/3ds/debug/azahar_switch.txt";
constexpr const char* StdoutLogPath = "sdmc:/GBAStation/3ds/debug/stdout.txt";
constexpr const char* StderrLogPath = "sdmc:/GBAStation/3ds/debug/stderr.txt";
constexpr const char* FallbackRomPath = "sdmc:/GBAStation/3ds/games/3Dlandchs.cci";
constexpr const char* MemMapLogPath = "sdmc:/GBAStation/3ds/debug/memmap.txt";
constexpr const char* LauncherPath = "sdmc:/switch/GBAStation.nro";

struct LaunchOptions {
    std::string rom_path;
    std::string title;
    SwitchFrontend::GBAStationDisplaySettings display_settings;
    std::string return_nro_path{LauncherPath};
    bool return_to_nro{true};
    bool display_settings_from_game_db{};
};
#if defined(GBASTATION_SWITCH_DIAGNOSTIC_LOGS)
constexpr bool DiagnosticLogsEnabled = true;
#else
constexpr bool DiagnosticLogsEnabled = false;
#endif
// Keep memory-map dumping separate because it is substantially heavier than boot logging.
constexpr bool EnableStartupLogFile = DiagnosticLogsEnabled;
constexpr bool EnableStdStreamLogs = DiagnosticLogsEnabled;
constexpr bool EnableMemMapLogFile = false;
constexpr bool EnableSwitchDebugLogFile = DiagnosticLogsEnabled;
constexpr bool EnableCommonLogFile = DiagnosticLogsEnabled;
constexpr bool EnableBootMarkerFile = DiagnosticLogsEnabled;
FILE* startup_log{};
FILE* debug_log{};
bool raw_marker_enabled = true;
PadState pad{};

void EnsureSystemDirs() {
    mkdir("sdmc:/GBAStation", 0777);
    mkdir("sdmc:/GBAStation/3ds", 0777);
    mkdir(SystemDir, 0777);
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

void DebugClose() {
    StartupLog("DebugClose");
    if (debug_log) {
        std::fprintf(debug_log, "=== Azahar Switch standalone end ===\n");
        std::fclose(debug_log);
        debug_log = nullptr;
    }
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
    if (!options.return_to_nro) {
        StartupLog("QueueLauncherReturn: --exit-to-home requested");
        return true;
    }
    if (!envHasNextLoad()) {
        StartupLog("QueueLauncherReturn: loader does not support envSetNextLoad");
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
        return false;
    }

    char args[1024];
    std::snprintf(args, sizeof(args), "\"%s\"", target);
    const LibnxResult rc = envSetNextLoad(target, args);
    StartupLog("QueueLauncherReturn: envSetNextLoad target=%s args=%s rc=0x%x", target, args,
               rc);
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
            return;
        }

        DebugLog("thread affinity restore %s: preferred=0 affinity=0x7 rc=0x%x", tag,
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
        std::abort();
    });

    auto signal_handler = [](int sig) {
        DebugLog("fatal signal %d", sig);
        StartupLog("fatal signal %d", sig);
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

    if (options.rom_path.empty()) {
        options.rom_path = FallbackRomPath;
        StartupLog("ParseLaunchOptions: no ROM argument, using fallback %s", FallbackRomPath);
    }
    options.title = TitleFromPath(options.rom_path);
    const auto game_record = SwitchFrontend::GameDatabase::LoadGameRecord(options.rom_path);
    if (game_record.found) {
        if (!game_record.title.empty()) {
            options.title = game_record.title;
        }
        options.display_settings = game_record.display;
        options.display_settings_from_game_db = true;
    }
    StartupLog("ParseLaunchOptions: rom=%s title=%s return=%s target=%s",
               options.rom_path.c_str(), options.title.c_str(),
               options.return_to_nro ? "nro" : "home", options.return_nro_path.c_str());
    return options;
}

void ConfigureSettings() {
    Settings::RestoreGlobalState(false);
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
    InstallFatalHandlers();
    ThreadCoreMaskGuard thread_core_guard;

    DebugLog("argc=%d", argc);
    for (int i = 0; i < argc; i++) {
        DebugLog("argv[%d]=%s", i, argv[i] ? argv[i] : "(null)");
    }

    StartupLog("Run: parsing ROM path");
    LaunchOptions launch_options = ParseLaunchOptions(argc, argv);
    const std::string& rom_path = launch_options.rom_path;
    if (rom_path.empty()) {
        DebugLog("no ROM path supplied");
        thread_core_guard.Restore("no-rom");
        DebugClose();
        const bool queued_launcher_return = QueueLauncherReturn(launch_options);
        appletUnlockExit();
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
    ApplyConfiguredDisplayDefaults(launch_options.display_settings,
                                   !launch_options.display_settings_from_game_db,
                                   !launch_options.display_settings_from_game_db);
    Settings::values.resolution_factor.SetValue(
        static_cast<u16>(std::clamp(launch_options.display_settings.internal_resolution, 1, 4)));
    SwitchFrontend::GBAStationConfig::ApplyConfig();
    Settings::values.swap_screen.SetValue(false);
    // The Switch frontend owns a FIFO VI swapchain and must continue presenting while a title
    // is booting.  Some titles do not report a top-screen buffer swap for a long time; with
    // duplicate-frame skipping enabled that leaves VI displaying the initial black image even
    // though the emulated system and renderer are still advancing.
    Settings::values.use_skip_duplicate_frames.SetValue(false);
    DebugLog("GBAStation config applied: path=%s options=%zu upscale=%s effective_res=%u skip_duplicate=%d",
             SwitchFrontend::GBAStationConfig::GetLoadedConfigPath().c_str(),
             SwitchFrontend::GBAStationConfig::GetLoadedOptionCount(),
             SwitchFrontend::GBAStationConfig::GetConfigValue("upscale", "default").c_str(),
             Settings::values.resolution_factor.GetValue(),
             Settings::values.use_skip_duplicate_frames.GetValue() ? 1 : 0);
    StartupLog("Run: FileUtil::SetUserPath %s/", SystemDir);
    FileUtil::SetUserPath(std::string{SystemDir} + "/");
    StartupLog("Run: Common::Log init");
    bool common_log_started = false;
    if (EnableCommonLogFile) {
        Common::Log::Initialize("azahar_common.txt");
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

    StartupLog("Run: creating NWindow frontend");
    SwitchFrontend::EmuWindowSwitch window{nwindowGetDefault()};
    window.SetDisplaySettings(launch_options.display_settings);
    DebugLog("loading ROM: %s", rom_path.c_str());
    const Core::System::ResultStatus load_result = system.Load(window, rom_path);
    if (load_result != Core::System::ResultStatus::Success) {
        DebugLog("load failed: %s (%u)", ResultStatusName(load_result),
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
        appletUnlockExit();
        return queued_launcher_return ? EXIT_SUCCESS : EXIT_FAILURE;
    }
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

    SwitchFrontend::VulkanOverlay::SetDisplaySettings(launch_options.display_settings);
    bool menu_initialized = SwitchFrontend::VulkanOverlay::Init(
        static_cast<Vulkan::RendererVulkan&>(system.GPU().Renderer()));
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
    const char* exit_reason = "loop condition ended";
    while (true) {
        const auto now = Clock::now();
        applet_loop_active = PumpAppletMessages();
        if (!applet_loop_active) {
            exit_reason = "applet message requested exit";
            DebugLog("main loop exit: applet message requested exit after %llu iterations",
                     static_cast<unsigned long long>(loop_count));
            break;
        }
        if (!system.IsPoweredOn()) {
            exit_reason = "system powered off before RunLoop";
            DebugLog("main loop exit: system powered off before RunLoop after %llu iterations",
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
            const bool accepted = system.SendSignal(
                saving ? Core::System::Signal::Save : Core::System::Signal::Load,
                static_cast<u32>(slot));
            if (accepted) {
                if (saving) {
                    (*slot_state_cache)[slot].store(true, std::memory_order_release);
                } else {
                    pause_frame_ready = false;
                    pause_frame_baseline = renderer.GetCurrentFrame();
                }
                char message[64]{};
                std::snprintf(message, sizeof(message), saving ? "正在保存到存档位 %d"
                                                              : "正在读取存档位 %d",
                              slot);
                SwitchFrontend::OverlayUI::ShowToast(message);
                DebugLog("menu %s state slot=%d", saving ? "save" : "load", slot);
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
            force_input_suppressed_during_shutdown = true;
            if (menu_initialized) {
                SwitchFrontend::VulkanOverlay::PrepareForShutdown();
            }
            window.SetInputSuppressed(true);
            InputCommon::SwitchHID::SetInputSuppressed(true);
            Common::RequestFastShutdown();
            break;
        }

        if (!menu_initialized && ExitComboPressed()) {
            exit_reason = "ZL+ZR fallback exit";
            DebugLog("menu unavailable; fallback exit combo after %llu iterations",
                     static_cast<unsigned long long>(loop_count));
            force_input_suppressed_during_shutdown = true;
            window.SetInputSuppressed(true);
            InputCommon::SwitchHID::SetInputSuppressed(true);
            Common::RequestFastShutdown();
            break;
        }

        const bool pause_for_menu = menu_visible && pause_frame_ready;
        if (pause_for_menu) {
            static_cast<Vulkan::RendererVulkan&>(renderer).PresentLastFrame();
            ++loop_count;
            svcSleepThread(16666667);
            continue;
        }

        const Core::System::ResultStatus run_result = system.RunLoop();
        loop_count++;
        if (pending_overlay_reinit && run_result == Core::System::ResultStatus::Success &&
            system.IsPoweredOn()) {
            auto& reset_renderer = system.GPU().Renderer();
            SwitchFrontend::VulkanOverlay::SetDisplaySettings(
                SwitchFrontend::VulkanOverlay::GetDisplaySettings());
            menu_initialized = SwitchFrontend::VulkanOverlay::Init(
                static_cast<Vulkan::RendererVulkan&>(reset_renderer));
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
        if (now - last_heartbeat >= std::chrono::seconds(1)) {
            const auto heartbeat_elapsed = std::chrono::duration<double>(now - last_heartbeat).count();
            const u64 loop_delta = loop_count - last_heartbeat_loop_count;
            const s32 frame_delta = renderer_frame - last_heartbeat_frame;
            const auto stats = system.GetAndResetPerfStats();
            DebugLog("main loop heartbeat: iterations=%llu loops_per_sec=%.1f renderer_frame=%d frame_delta=%d frontend_fps=%.1f system_fps=%.1f game_fps=%.1f emu_speed=%.2f powered=%d applet=%d keepalives=%llu",
                     static_cast<unsigned long long>(loop_count),
                     heartbeat_elapsed > 0.0 ? static_cast<double>(loop_delta) / heartbeat_elapsed : 0.0,
                     renderer_frame, frame_delta,
                     heartbeat_elapsed > 0.0 ? static_cast<double>(frame_delta) / heartbeat_elapsed : 0.0,
                     stats.system_fps, stats.game_fps, stats.emulation_speed,
                     system.IsPoweredOn() ? 1 : 0, applet_loop_active ? 1 : 0,
                     static_cast<unsigned long long>(keepalive_count));
            last_heartbeat = now;
            last_heartbeat_loop_count = loop_count;
            last_heartbeat_frame = renderer_frame;
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
            break;
        }
    }

    DebugLog("shutting down: reason=%s powered=%d applet=%d iterations=%llu", exit_reason,
             system.IsPoweredOn() ? 1 : 0, applet_loop_active ? 1 : 0,
             static_cast<unsigned long long>(loop_count));
    const auto remaining_play_time = std::chrono::duration_cast<std::chrono::seconds>(
                                         Clock::now() - play_stats_checkpoint)
                                         .count();
    if (remaining_play_time > 0) {
        SwitchFrontend::GameDatabase::UpdatePlayStats(
            launch_options.rom_path, launch_options.title, false,
            static_cast<int>(remaining_play_time));
    }
    Settings::values.use_vsync.SetValue(normal_vsync);
    if (mic_input_simulated) {
        Settings::values.input_type.SetValue(mic_restore_input_type);
        mic_input_simulated = false;
    }
    SwitchFrontend::VulkanOverlay::SetFastForwardActive(false);
    Settings::ResetTemporaryFrameLimit();
    const auto shutdown_started = Clock::now();
    if (menu_initialized) {
        SwitchFrontend::VulkanOverlay::PrepareForShutdown();
    }
    if (menu_audio_muted) {
        Settings::values.volume.SetValue(menu_restore_volume);
        menu_audio_muted = false;
    }
    if (force_input_suppressed_during_shutdown) {
        window.SetInputSuppressed(true);
        InputCommon::SwitchHID::SetInputSuppressed(true);
    }
    if (system.IsPoweredOn()) {
        DebugLog("shutdown step: system.Shutdown begin");
        const auto step_started = Clock::now();
        system.Shutdown();
        DebugLog("shutdown step: system.Shutdown done (%lld ms)",
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            Clock::now() - step_started)
                                            .count()));
    }
    if (menu_initialized) {
        DebugLog("shutdown step: VulkanOverlay::Shutdown begin");
        SwitchFrontend::VulkanOverlay::Shutdown();
        menu_initialized = false;
        DebugLog("shutdown step: VulkanOverlay::Shutdown done");
    }
    window.SetInputSuppressed(false);
    InputCommon::SwitchHID::SetInputSuppressed(false);
    DebugLog("shutdown step: InputCommon::Shutdown begin");
    InputCommon::Shutdown();
    DebugLog("shutdown step: InputCommon::Shutdown done");
    DebugLog("shutdown step: Common::Log::Stop begin");
    if (common_log_started) {
        Common::Log::Stop();
        common_log_started = false;
    }
    DebugLog("shutdown step: Common::Log::Stop done");
    if (romfs_initialized) {
        DebugLog("shutdown step: romfsExit begin");
        romfsExit();
        DebugLog("shutdown step: romfsExit done");
    }
    thread_core_guard.Restore("normal-shutdown");
    DebugLog("shutdown total before launcher: %lld ms",
             static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        Clock::now() - shutdown_started)
                                        .count()));
    DebugLog("shutdown step: DebugClose begin");
    DebugClose();
    const bool queued_launcher_return = QueueLauncherReturn(launch_options);
    DumpMemoryMap("after-shutdown");
    StartupLog("Run: appletUnlockExit begin");
    const LibnxResult unlock_exit_rc = appletUnlockExit();
    StartupLog("Run: appletUnlockExit rc=0x%x", unlock_exit_rc);
    return queued_launcher_return ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

extern "C" void userAppInit() {
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
    // Runs after C++ static destructors — the closest observable state to what hbl
    // inherits. Anything still flagged SUSPECT here is what kills hbl with 0xD401.
    DumpMemoryMap("static-dtors-done");
    StartupLog("userAppExit");
    CloseStartupLog();
}

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    WriteRawBootMarker("__libnx_exception_handler: entry");
    OpenStartupLogIfNeeded("a");
    StartupLog("module anchor: exception_handler=%p",
               reinterpret_cast<void*>(&__libnx_exception_handler));
    if (ctx) {
        StartupLog("libnx exception: desc=0x%08x pc=0x%016llx lr=0x%016llx sp=0x%016llx far=0x%016llx esr=0x%08x",
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
        StartupLog("libnx exception: x4=0x%016llx x5=0x%016llx x6=0x%016llx x7=0x%016llx",
                   static_cast<unsigned long long>(ctx->cpu_gprs[4].x),
                   static_cast<unsigned long long>(ctx->cpu_gprs[5].x),
                   static_cast<unsigned long long>(ctx->cpu_gprs[6].x),
                   static_cast<unsigned long long>(ctx->cpu_gprs[7].x));
    } else {
        StartupLog("libnx exception: null context");
    }
}

int main(int argc, char** argv) {
    WriteRawBootMarker("main: entry");
    OpenStartupLogIfNeeded("a");
    StartupLog("main: entry argc=%d", argc);
    const int result = Run(argc, argv);
    StartupLog("main: exit result=%d", result);
    return result;
}
