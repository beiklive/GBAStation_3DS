// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#ifdef __SWITCH__

#include <cstddef>
#include <cstdint>
#include <memory>

#include "common/common_types.h"

namespace Memory {

struct PageTable;

class SwitchFastmemArena final {
public:
    explicit SwitchFastmemArena(PageTable& page_table);
    ~SwitchFastmemArena();

    bool IsValid() const;
    bool HasMappings() const;
    uintptr_t Pointer() const;

    void MapRange(PageTable& page_table, size_t first_page, size_t page_count);
    void UnmapRange(PageTable& page_table, size_t first_page, size_t page_count);
    void Clear(PageTable& page_table);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class SwitchCodeAliasBuffer final {
public:
    explicit SwitchCodeAliasBuffer(size_t size);
    ~SwitchCodeAliasBuffer();

    u8* get();
    const u8* get() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Memory

#endif
