// SPDX-FileCopyrightText: Copyright (c) 2022 merryhime <https://mary.rs>
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>

#if defined(__SWITCH__)
#elif defined(_WIN32)
#    define NOMINMAX
#    include <windows.h>
#elif defined(__APPLE__)
#    include <TargetConditionals.h>
#    include <libkern/OSCacheControl.h>
#    include <pthread.h>
#    include <sys/mman.h>
#    include <unistd.h>
#else
#    include <sys/mman.h>
#endif

#if defined(__SWITCH__)
extern "C" {
void* gbastation_switch_jit_create(std::size_t size, void** rw_addr, void** rx_addr);
int gbastation_switch_jit_set_writable(void* handle);
int gbastation_switch_jit_set_executable(void* handle);
int gbastation_switch_jit_sync_range(void* handle, std::size_t offset, std::size_t size);
void gbastation_switch_jit_close(void* handle);
}
#endif

namespace oaknut {

class CodeBlock {
public:
    explicit CodeBlock(std::size_t size)
        : m_size(size)
    {
#if defined(__SWITCH__)
        void* rw_addr = nullptr;
        void* rx_addr = nullptr;
        m_jit = gbastation_switch_jit_create(size, &rw_addr, &rx_addr);
        if (m_jit == nullptr)
            throw std::bad_alloc{};
        m_wmemory = static_cast<std::uint32_t*>(rw_addr);
        m_memory = static_cast<std::uint32_t*>(rx_addr);
        if (m_memory == nullptr || m_wmemory == nullptr) {
            gbastation_switch_jit_close(m_jit);
            m_jit = nullptr;
            m_wmemory = nullptr;
            m_memory = nullptr;
            throw std::bad_alloc{};
        }
#elif defined(_WIN32)
        m_memory = (std::uint32_t*)VirtualAlloc(nullptr, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#elif defined(__APPLE__)
#    if TARGET_OS_IPHONE
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
#    else
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE | MAP_JIT, -1, 0);
#    endif
#elif defined(__NetBSD__)
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_MPROTECT(PROT_READ | PROT_WRITE | PROT_EXEC), MAP_ANON | MAP_PRIVATE, -1, 0);
#elif defined(__OpenBSD__)
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
#else
        m_memory = (std::uint32_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANON | MAP_PRIVATE, -1, 0);
#endif

        if (m_memory == nullptr)
            throw std::bad_alloc{};
    }

    ~CodeBlock()
    {
        if (m_memory == nullptr)
            return;

#if defined(__SWITCH__)
        gbastation_switch_jit_close(m_jit);
#elif defined(_WIN32)
        VirtualFree((void*)m_memory, 0, MEM_RELEASE);
#else
        munmap(m_memory, m_size);
#endif
    }

    CodeBlock(const CodeBlock&) = delete;
    CodeBlock& operator=(const CodeBlock&) = delete;
    CodeBlock(CodeBlock&&) = delete;
    CodeBlock& operator=(CodeBlock&&) = delete;

    std::uint32_t* ptr() const
    {
        return xptr();
    }

    std::uint32_t* wptr() const
    {
#if defined(__SWITCH__)
        return m_wmemory;
#else
        return m_memory;
#endif
    }

    std::uint32_t* xptr() const
    {
        return m_memory;
    }

    void protect()
    {
#if defined(__SWITCH__)
        if (!m_writes_synchronized && gbastation_switch_jit_set_executable(m_jit) != 0)
            throw std::bad_alloc{};
        m_writes_synchronized = false;
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
        pthread_jit_write_protect_np(1);
#elif defined(__APPLE__) || defined(__NetBSD__) || defined(__OpenBSD__)
        mprotect(m_memory, m_size, PROT_READ | PROT_EXEC);
#endif
    }

    void unprotect()
    {
#if defined(__SWITCH__)
        m_writes_synchronized = false;
        if (gbastation_switch_jit_set_writable(m_jit) != 0)
            throw std::bad_alloc{};
#elif defined(__APPLE__) && !TARGET_OS_IPHONE
        pthread_jit_write_protect_np(0);
#elif defined(__APPLE__) || defined(__NetBSD__) || defined(__OpenBSD__)
        mprotect(m_memory, m_size, PROT_READ | PROT_WRITE);
#endif
    }

    void invalidate(std::uint32_t* mem, std::size_t size)
    {
#if defined(__SWITCH__)
        const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(mem);
        const std::uintptr_t xbase = reinterpret_cast<std::uintptr_t>(m_memory);
        const std::uintptr_t wbase = reinterpret_cast<std::uintptr_t>(m_wmemory);
        std::size_t offset;
        if (address >= xbase && address - xbase <= m_size) {
            offset = static_cast<std::size_t>(address - xbase);
        } else if (address >= wbase && address - wbase <= m_size) {
            offset = static_cast<std::size_t>(address - wbase);
        } else {
            throw std::bad_alloc{};
        }

        const int result = gbastation_switch_jit_sync_range(m_jit, offset, size);
        if (result < 0)
            throw std::bad_alloc{};
        // A return value of 1 selects the older SetProcessMemoryPermission fallback.  In that
        // case protect() performs the required whole-buffer transition after all writes finish.
        m_writes_synchronized = result == 0;
#elif defined(__APPLE__)
        sys_icache_invalidate(mem, size);
#elif defined(_WIN32)
        FlushInstructionCache(GetCurrentProcess(), mem, size);
#else
        static std::size_t icache_line_size = 0x10000, dcache_line_size = 0x10000;

        std::uint64_t ctr;
        __asm__ volatile("mrs %0, ctr_el0"
                         : "=r"(ctr));

        const std::size_t isize = icache_line_size = std::min<std::size_t>(icache_line_size, 4 << ((ctr >> 0) & 0xf));
        const std::size_t dsize = dcache_line_size = std::min<std::size_t>(dcache_line_size, 4 << ((ctr >> 16) & 0xf));

        const std::uintptr_t end = (std::uintptr_t)mem + size;

        for (std::uintptr_t addr = ((std::uintptr_t)mem) & ~(dsize - 1); addr < end; addr += dsize) {
            __asm__ volatile("dc cvau, %0"
                             :
                             : "r"(addr)
                             : "memory");
        }
        __asm__ volatile("dsb ish\n"
                         :
                         :
                         : "memory");

        for (std::uintptr_t addr = ((std::uintptr_t)mem) & ~(isize - 1); addr < end; addr += isize) {
            __asm__ volatile("ic ivau, %0"
                             :
                             : "r"(addr)
                             : "memory");
        }
        __asm__ volatile("dsb ish\nisb\n"
                         :
                         :
                         : "memory");
#endif
    }

    void invalidate_all()
    {
        invalidate(m_memory, m_size);
    }

protected:
#if defined(__SWITCH__)
    void* m_jit = nullptr;
    std::uint32_t* m_wmemory = nullptr;
    bool m_writes_synchronized = false;
#endif
    std::uint32_t* m_memory;
    std::size_t m_size = 0;
};

}  // namespace oaknut
