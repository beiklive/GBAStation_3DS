// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version

#if defined(GBASTATION_SWITCH_DIAGNOSTIC_LOGS)

#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using InitFunction = void (*)();

constexpr const char* InitArrayLogPath = "sdmc:/GBAStation/3ds/debug/init_array.txt";

[[gnu::noinline]] void WriteInitLog(int fd, const char* phase, const char* array_name,
                                    unsigned long index, InitFunction function, bool sync) {
    if (fd < 0) {
        return;
    }

    char line[192];
    const int length = std::snprintf(line, sizeof(line), "%s array=%s index=%lu fn=%p\n", phase,
                                     array_name, index, reinterpret_cast<void*>(function));
    if (length <= 0) {
        return;
    }

    const auto write_length = static_cast<size_t>(length) < sizeof(line)
                                  ? static_cast<size_t>(length)
                                  : sizeof(line) - 1;
    static_cast<void>(write(fd, line, write_length));
    if (sync) {
        static_cast<void>(fsync(fd));
    }
}

[[gnu::noinline]] void RunInitArray(int fd, const char* name, InitFunction* begin,
                                    InitFunction* end) {
    const auto count = static_cast<unsigned long>(end - begin);
    for (unsigned long index = 0; index < count; ++index) {
        InitFunction function = begin[index];
        WriteInitLog(fd, "begin", name, index, function, true);
        function();
        WriteInitLog(fd, "done", name, index, function, false);
    }
}

} // namespace

extern "C" {
extern InitFunction __preinit_array_start[];
extern InitFunction __preinit_array_end[];
extern InitFunction __init_array_start[];
extern InitFunction __init_array_end[];
void _init();
}

extern "C" void __libc_init_array() {
    mkdir("sdmc:/GBAStation", 0777);
    mkdir("sdmc:/GBAStation/3ds", 0777);
    mkdir("sdmc:/GBAStation/3ds/debug", 0777);
    const int fd = open(InitArrayLogPath, O_CREAT | O_WRONLY | O_TRUNC, 0666);

    RunInitArray(fd, "preinit", __preinit_array_start, __preinit_array_end);
    WriteInitLog(fd, "begin", "legacy", 0, &_init, true);
    _init();
    WriteInitLog(fd, "done", "legacy", 0, &_init, false);
    RunInitArray(fd, "init", __init_array_start, __init_array_end);

    if (fd >= 0) {
        static_cast<void>(fsync(fd));
        close(fd);
    }
}

#endif
