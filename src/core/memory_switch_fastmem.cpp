// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/memory_switch_fastmem.h"

#ifdef __SWITCH__

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/memory.h"

struct SwitchVirtmemReservation;
using SwitchHandle = std::uint32_t;

extern "C" void virtmemLock(void);
extern "C" void virtmemUnlock(void);
extern "C" void* virtmemFindAslr(size_t size, size_t guard_size);
extern "C" void* virtmemFindCodeMemory(size_t size, size_t guard_size);
extern "C" SwitchVirtmemReservation* virtmemAddReservation(void* mem, size_t size);
extern "C" void virtmemRemoveReservation(SwitchVirtmemReservation* reservation);
extern "C" std::uint32_t svcMapProcessMemory(void* dst, SwitchHandle process,
                                               std::uint64_t src, std::uint64_t size);
extern "C" std::uint32_t svcUnmapProcessMemory(void* dst, SwitchHandle process,
                                                 std::uint64_t src, std::uint64_t size);
extern "C" std::uint32_t svcMapProcessCodeMemory(SwitchHandle process, std::uint64_t dst,
                                                   std::uint64_t src, std::uint64_t size);
extern "C" std::uint32_t svcUnmapProcessCodeMemory(SwitchHandle process, std::uint64_t dst,
                                                     std::uint64_t src, std::uint64_t size);
extern "C" std::uint32_t svcSetProcessMemoryPermission(SwitchHandle process,
                                                         std::uint64_t address,
                                                         std::uint64_t size,
                                                         std::uint32_t permission);
extern "C" SwitchHandle envGetOwnProcessHandle(void);

namespace Memory {

namespace {

constexpr size_t FastmemAddressSpaceSize = size_t{1} << 32;
constexpr u32 ReadWritePermission = 3;

struct PageAlignedBufferDeleter {
    void operator()(u8* pointer) const {
        std::free(pointer);
    }
};

using PageAlignedBuffer = std::unique_ptr<u8[], PageAlignedBufferDeleter>;

PageAlignedBuffer MakePageAlignedBuffer(size_t size) {
    u8* const pointer = static_cast<u8*>(std::aligned_alloc(CITRA_PAGE_SIZE, size));
    ASSERT_MSG(pointer != nullptr, "Unable to allocate {} bytes of page-aligned guest memory", size);
    return PageAlignedBuffer{pointer};
}

} // namespace

struct SwitchFastmemArena::Impl final {
    explicit Impl(PageTable& page_table) : mapped_sources(PAGE_TABLE_NUM_ENTRIES, nullptr) {
        virtmemLock();
        base = static_cast<u8*>(virtmemFindAslr(FastmemAddressSpaceSize, CITRA_PAGE_SIZE));
        if (base) {
            reservation = virtmemAddReservation(base, FastmemAddressSpaceSize);
        }
        virtmemUnlock();

        if (!reservation) {
            base = nullptr;
            LOG_WARNING(HW_Memory, "Switch fastmem: unable to reserve 4 GiB guest arena");
            return;
        }

        MapRange(page_table, 0, PAGE_TABLE_NUM_ENTRIES);
        LOG_INFO(HW_Memory, "Switch fastmem: arena={} mapped_pages={}",
                 static_cast<void*>(base), mapped_page_count);
    }

    ~Impl() {
        if (reservation) {
            virtmemLock();
            virtmemRemoveReservation(reservation);
            virtmemUnlock();
        }
    }

    bool IsValid() const {
        return base != nullptr && reservation != nullptr;
    }

    void UnmapRange(size_t first_page, size_t page_count) {
        if (!IsValid() || page_count == 0) {
            return;
        }

        const size_t end_page = std::min(first_page + page_count, PAGE_TABLE_NUM_ENTRIES);
        size_t page = first_page;
        while (page < end_page) {
            if (mapped_sources[page] == nullptr) {
                page++;
                continue;
            }

            const size_t run_begin = page;
            u8* const source_begin = mapped_sources[page];
            page++;
            while (page < end_page &&
                   mapped_sources[page] ==
                       source_begin + (page - run_begin) * CITRA_PAGE_SIZE) {
                page++;
            }

            const size_t run_pages = page - run_begin;
            const u32 result = svcUnmapProcessMemory(base + run_begin * CITRA_PAGE_SIZE,
                                                     envGetOwnProcessHandle(),
                                                     reinterpret_cast<u64>(source_begin),
                                                     run_pages * CITRA_PAGE_SIZE);
            if (result != 0) {
                LOG_ERROR(HW_Memory,
                          "Switch fastmem: unmap failed guest={:08x} pages={} rc={:08x}",
                          static_cast<u32>(run_begin << CITRA_PAGE_BITS), run_pages, result);
                continue;
            }

            std::fill(mapped_sources.begin() + run_begin, mapped_sources.begin() + page, nullptr);
            mapped_page_count -= run_pages;
        }
    }

    void MapRange(PageTable& page_table, size_t first_page, size_t page_count) {
        if (!IsValid() || page_count == 0) {
            return;
        }

        auto& pointers = page_table.GetPointerArray();
        const size_t end_page = std::min(first_page + page_count, PAGE_TABLE_NUM_ENTRIES);
        size_t page = first_page;
        while (page < end_page) {
            u8* const source = pointers[page];
            if (mapped_sources[page] != nullptr ||
                page_table.attributes[page] != PageType::Memory || source == nullptr ||
                (reinterpret_cast<uintptr_t>(source) & CITRA_PAGE_MASK) != 0) {
                page++;
                continue;
            }

            const size_t run_begin = page;
            u8* const source_begin = source;
            page++;
            while (page < end_page && mapped_sources[page] == nullptr &&
                   page_table.attributes[page] == PageType::Memory &&
                   pointers[page] == source_begin + (page - run_begin) * CITRA_PAGE_SIZE) {
                page++;
            }

            const size_t run_pages = page - run_begin;
            const u32 result = svcMapProcessMemory(base + run_begin * CITRA_PAGE_SIZE,
                                                   envGetOwnProcessHandle(),
                                                   reinterpret_cast<u64>(source_begin),
                                                   run_pages * CITRA_PAGE_SIZE);
            if (result != 0) {
                LOG_WARNING(HW_Memory,
                            "Switch fastmem: map skipped guest={:08x} pages={} rc={:08x}",
                            static_cast<u32>(run_begin << CITRA_PAGE_BITS), run_pages, result);
                continue;
            }

            for (size_t mapped_page = run_begin; mapped_page < page; mapped_page++) {
                mapped_sources[mapped_page] =
                    source_begin + (mapped_page - run_begin) * CITRA_PAGE_SIZE;
            }
            mapped_page_count += run_pages;
        }
    }

    u8* base{};
    SwitchVirtmemReservation* reservation{};
    std::vector<u8*> mapped_sources;
    size_t mapped_page_count{};
};

SwitchFastmemArena::SwitchFastmemArena(PageTable& page_table)
    : impl(std::make_unique<Impl>(page_table)) {}
SwitchFastmemArena::~SwitchFastmemArena() = default;

bool SwitchFastmemArena::IsValid() const {
    return impl->IsValid();
}

bool SwitchFastmemArena::HasMappings() const {
    return impl->mapped_page_count != 0;
}

uintptr_t SwitchFastmemArena::Pointer() const {
    return reinterpret_cast<uintptr_t>(impl->base);
}

void SwitchFastmemArena::MapRange(PageTable& page_table, size_t first_page, size_t page_count) {
    impl->MapRange(page_table, first_page, page_count);
}

void SwitchFastmemArena::UnmapRange(PageTable&, size_t first_page, size_t page_count) {
    impl->UnmapRange(first_page, page_count);
}

void SwitchFastmemArena::Clear(PageTable& page_table) {
    UnmapRange(page_table, 0, PAGE_TABLE_NUM_ENTRIES);
}

struct SwitchCodeAliasBuffer::Impl final {
    explicit Impl(size_t size_)
        : requested_size(size_), size(Common::AlignUp(size_, CITRA_PAGE_SIZE)),
          source(MakePageAlignedBuffer(size)) {
        std::memset(source.get(), 0, size);

        virtmemLock();
        alias = static_cast<u8*>(virtmemFindCodeMemory(size, CITRA_PAGE_SIZE));
        if (alias) {
            map_result = svcMapProcessCodeMemory(envGetOwnProcessHandle(),
                                                 reinterpret_cast<u64>(alias),
                                                 reinterpret_cast<u64>(source.get()), size);
        }
        if (map_result == 0 && alias) {
            permission_result = svcSetProcessMemoryPermission(
                envGetOwnProcessHandle(), reinterpret_cast<u64>(alias), size,
                ReadWritePermission);
        }
        if (alias && (map_result != 0 || permission_result != 0)) {
            if (map_result == 0) {
                svcUnmapProcessCodeMemory(envGetOwnProcessHandle(),
                                          reinterpret_cast<u64>(alias),
                                          reinterpret_cast<u64>(source.get()), size);
            }
            alias = nullptr;
        }
        virtmemUnlock();

        if (alias) {
            LOG_INFO(HW_Memory,
                     "Switch guest RAM alias: source={} alias={} requested_size={} mapped_size={}",
                     static_cast<void*>(source.get()), static_cast<void*>(alias), requested_size,
                     size);
        } else {
            LOG_WARNING(HW_Memory,
                        "Switch guest RAM alias unavailable: requested_size={} mapped_size={} "
                        "map_rc={:08x} perm_rc={:08x}",
                        requested_size, size, map_result, permission_result);
        }
    }

    ~Impl() {
        if (!alias) {
            return;
        }
        const u32 result = svcUnmapProcessCodeMemory(envGetOwnProcessHandle(),
                                                     reinterpret_cast<u64>(alias),
                                                     reinterpret_cast<u64>(source.get()), size);
        if (result != 0) {
            LOG_ERROR(HW_Memory, "Switch guest RAM alias unmap failed: rc={:08x}", result);
        }
    }

    size_t requested_size{};
    size_t size{};
    PageAlignedBuffer source;
    u8* alias{};
    u32 map_result{1};
    u32 permission_result{1};
};

SwitchCodeAliasBuffer::SwitchCodeAliasBuffer(size_t size) : impl(std::make_unique<Impl>(size)) {}
SwitchCodeAliasBuffer::~SwitchCodeAliasBuffer() = default;

u8* SwitchCodeAliasBuffer::get() {
    return impl->alias ? impl->alias : impl->source.get();
}

const u8* SwitchCodeAliasBuffer::get() const {
    return impl->alias ? impl->alias : impl->source.get();
}

} // namespace Memory

#endif
