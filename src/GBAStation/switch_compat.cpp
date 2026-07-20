// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "GBAStation/switch_libnx.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <new>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct _reent;
typedef struct ui_st UI;
typedef struct ui_string_st UI_STRING;
typedef struct ui_method_st UI_METHOD;

extern "C" {

struct GBAStationSwitchJit {
    Jit jit{};
};

void gbastation_switch_random_bytes(void* output, size_t size) {
    randomGet(output, size);
}

void* gbastation_switch_jit_create(size_t size, void** rw_addr, void** rx_addr) {
    auto* handle = new (std::nothrow) GBAStationSwitchJit{};
    if (!handle || jitCreate(&handle->jit, size) != 0) {
        delete handle;
        return nullptr;
    }

    *rw_addr = jitGetRwAddr(&handle->jit);
    *rx_addr = jitGetRxAddr(&handle->jit);
    if (!*rw_addr || !*rx_addr) {
        jitClose(&handle->jit);
        delete handle;
        return nullptr;
    }
    std::fprintf(stderr,
                 "[azahar-switch] JIT buffer: size=%zu type=%s rw=%p rx=%p range_sync=%d\n",
                 handle->jit.size,
                 handle->jit.type == JitType_CodeMemory ? "CodeMemory" : "ProcessPermission",
                 *rw_addr, *rx_addr, handle->jit.type == JitType_CodeMemory ? 1 : 0);
    return handle;
}

int gbastation_switch_jit_set_writable(void* opaque) {
    auto* handle = static_cast<GBAStationSwitchJit*>(opaque);
    return !handle || jitTransitionToWritable(&handle->jit) != 0;
}

int gbastation_switch_jit_set_executable(void* opaque) {
    auto* handle = static_cast<GBAStationSwitchJit*>(opaque);
    return !handle || jitTransitionToExecutable(&handle->jit) != 0;
}

// Returns 0 when the written range was synchronized directly, 1 when the caller must use the
// normal whole-buffer writable/executable transition, and -1 for invalid arguments.  CodeMemory
// exposes simultaneous RW/RX aliases on Switch, so flushing only the bytes emitted for a new JIT
// block avoids two cache operations over the entire 16 MiB Dynarmic cache for every basic block.
int gbastation_switch_jit_sync_range(void* opaque, size_t offset, size_t size) {
    auto* handle = static_cast<GBAStationSwitchJit*>(opaque);
    if (!handle || offset > handle->jit.size || size > handle->jit.size - offset) {
        return -1;
    }
    if (handle->jit.type != JitType_CodeMemory) {
        return 1;
    }

    auto* const rw = static_cast<unsigned char*>(jitGetRwAddr(&handle->jit)) + offset;
    auto* const rx = static_cast<unsigned char*>(jitGetRxAddr(&handle->jit)) + offset;
    armDCacheFlush(rw, size);
    armICacheInvalidate(rx, size);
    handle->jit.is_executable = true;
    return 0;
}

void gbastation_switch_jit_close(void* opaque) {
    auto* handle = static_cast<GBAStationSwitchJit*>(opaque);
    if (!handle) {
        return;
    }
    jitClose(&handle->jit);
    delete handle;
}

UI_METHOD* UI_create_method(const char* name);
int UI_method_set_opener(UI_METHOD* method, int (*opener)(UI* ui));
int UI_method_set_writer(UI_METHOD* method, int (*writer)(UI* ui, UI_STRING* uis));
int UI_method_set_reader(UI_METHOD* method, int (*reader)(UI* ui, UI_STRING* uis));
int UI_method_set_closer(UI_METHOD* method, int (*closer)(UI* ui));

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    const off_t original_offset = lseek(fd, 0, SEEK_CUR);
    if (original_offset == static_cast<off_t>(-1)) {
        return -1;
    }

    if (lseek(fd, offset, SEEK_SET) == static_cast<off_t>(-1)) {
        return -1;
    }

    const ssize_t result = read(fd, buf, count);
    const int saved_errno = errno;
    lseek(fd, original_offset, SEEK_SET);
    errno = saved_errno;
    return result;
}

// Horizon has no setuid/AT_SECURE process model, so getenv is the secure variant here.
char* secure_getenv(const char* name) {
    return std::getenv(name);
}

// Mesa's built-in driconf table supports regular-expression match fields even when XML
// file parsing is disabled. Switch has the newlib declarations but no regex runtime; an
// unmatched result preserves the default NVK options used by the validated probes.
int regcomp(regex_t* regex, const char* pattern, int flags) {
    (void)regex;
    (void)pattern;
    (void)flags;
    return 0;
}

int regexec(const regex_t* regex, const char* string, size_t count, regmatch_t matches[],
            int flags) {
    (void)regex;
    (void)string;
    (void)count;
    (void)matches;
    (void)flags;
    return REG_NOMATCH;
}

void regfree(regex_t* regex) {
    (void)regex;
}

uid_t getuid(void) {
    return 0;
}

uid_t geteuid(void) {
    return 0;
}

gid_t getgid(void) {
    return 0;
}

gid_t getegid(void) {
    return 0;
}

struct passwd* getpwuid(uid_t) {
    static char name[] = "switch";
    static char home[] = "sdmc:/GBAStation/3ds";
    static char shell[] = "";
    static struct passwd entry{};

    entry.pw_name = name;
    entry.pw_uid = 0;
    entry.pw_gid = 0;
    entry.pw_dir = home;
    entry.pw_shell = shell;
    return &entry;
}

int getpwuid_r(uid_t uid, struct passwd* pwd, char* buffer, size_t buffer_size,
               struct passwd** result) {
    constexpr char name[] = "switch";
    constexpr char home[] = "sdmc:/GBAStation/3ds";
    constexpr char shell[] = "";
    constexpr size_t required = sizeof(name) + sizeof(home) + sizeof(shell);

    if (!pwd || !buffer || !result || buffer_size < required) {
        if (result) {
            *result = nullptr;
        }
        return ERANGE;
    }

    char* cursor = buffer;
    std::memcpy(cursor, name, sizeof(name));
    pwd->pw_name = cursor;
    cursor += sizeof(name);
    std::memcpy(cursor, home, sizeof(home));
    pwd->pw_dir = cursor;
    cursor += sizeof(home);
    std::memcpy(cursor, shell, sizeof(shell));
    pwd->pw_shell = cursor;
    pwd->pw_passwd = nullptr;
    pwd->pw_comment = nullptr;
    pwd->pw_gecos = nullptr;
    pwd->pw_uid = uid;
    pwd->pw_gid = 0;
    *result = pwd;
    return 0;
}

int flock(int fd, int operation) {
    (void)fd;
    (void)operation;
    return 0;
}

int dirfd(DIR* directory) {
    (void)directory;
    errno = ENOTSUP;
    return -1;
}

int fstatat(int directory_fd, const char* path, struct stat* status, int flags) {
    if (directory_fd != AT_FDCWD) {
        errno = ENOTSUP;
        return -1;
    }
    if (flags & AT_SYMLINK_NOFOLLOW) {
        return lstat(path, status);
    }
    return stat(path, status);
}

long sysconf(int name) {
#ifdef _SC_PAGESIZE
    if (name == _SC_PAGESIZE) {
        return 0x1000;
    }
#endif
#ifdef _SC_PAGE_SIZE
    if (name == _SC_PAGE_SIZE) {
        return 0x1000;
    }
#endif
#ifdef _SC_NPROCESSORS_ONLN
    if (name == _SC_NPROCESSORS_ONLN) {
        return 4;
    }
#endif
#ifdef _SC_PHYS_PAGES
    if (name == _SC_PHYS_PAGES) {
        return 0x100000;
    }
#endif
    errno = EINVAL;
    return -1;
}

int getpagesize(void) {
    return 0x1000;
}

int sigprocmask(int, const sigset_t*, sigset_t* oldset) {
    if (oldset) {
        std::memset(oldset, 0, sizeof(*oldset));
    }
    return 0;
}

int pthread_sigmask(int how, const sigset_t* set, sigset_t* oldset) {
    return sigprocmask(how, set, oldset);
}

// NVK BO mapping is handled directly by nvkmd/switch. Mesa's remaining mmap users are
// optional disk-cache paths; returning MAP_FAILED disables those paths safely for M6.
void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset) {
    (void)address;
    (void)length;
    (void)protection;
    (void)flags;
    (void)fd;
    (void)offset;
    errno = ENOMEM;
    return reinterpret_cast<void*>(-1);
}

int munmap(void* address, size_t length) {
    (void)address;
    (void)length;
    return 0;
}

int _getentropy_r(struct _reent*, void* buf, size_t len) {
    randomGet(buf, len);
    return 0;
}

static int SwitchUiOpen(UI*) {
    return 1;
}

static int SwitchUiWrite(UI*, UI_STRING*) {
    return 1;
}

static int SwitchUiRead(UI*, UI_STRING*) {
    return 0;
}

static int SwitchUiClose(UI*) {
    return 1;
}

UI_METHOD* UI_OpenSSL(void) {
    static UI_METHOD* method = [] {
        UI_METHOD* created = UI_create_method("Switch null UI");
        if (created) {
            UI_method_set_opener(created, SwitchUiOpen);
            UI_method_set_writer(created, SwitchUiWrite);
            UI_method_set_reader(created, SwitchUiRead);
            UI_method_set_closer(created, SwitchUiClose);
        }
        return created;
    }();
    return method;
}

} // extern "C"
