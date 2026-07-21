/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/exception_handler.h"

#if defined(__SWITCH__) && defined(MCL_ARCHITECTURE_ARM64)
#include <algorithm>
#include <mutex>
#include <vector>

#include <mcl/bit_cast.hpp>
#include <oaknut/code_block.hpp>
#endif

namespace Dynarmic::Backend {

#if defined(__SWITCH__) && defined(MCL_ARCHITECTURE_ARM64)
namespace {

struct SwitchCodeBlockInfo {
    u64 code_begin{};
    u64 code_end{};
    std::function<FakeCall(u64)> callback;
};

std::mutex switch_code_blocks_mutex;
std::vector<SwitchCodeBlockInfo> switch_code_blocks;

} // namespace

extern "C" bool DynarmicHandleSwitchFastmemFault(u64 host_pc, u64* resume_pc) {
    std::lock_guard guard{switch_code_blocks_mutex};
    const auto iter = std::find_if(switch_code_blocks.begin(), switch_code_blocks.end(),
                                   [host_pc](const auto& block) {
                                       return block.code_begin <= host_pc && host_pc < block.code_end &&
                                              static_cast<bool>(block.callback);
                                   });
    if (iter == switch_code_blocks.end()) {
        return false;
    }

    const FakeCall call = iter->callback(host_pc);
    *resume_pc = call.call_pc;
    return true;
}

struct ExceptionHandler::Impl final {
    Impl(u64 code_begin_, u64 code_end_) : code_begin(code_begin_), code_end(code_end_) {}

    ~Impl() {
        std::lock_guard guard{switch_code_blocks_mutex};
        std::erase_if(switch_code_blocks, [this](const auto& block) {
            return block.code_begin == code_begin;
        });
    }

    void SetCallback(std::function<FakeCall(u64)> callback) {
        std::lock_guard guard{switch_code_blocks_mutex};
        std::erase_if(switch_code_blocks, [this](const auto& block) {
            return block.code_begin == code_begin;
        });
        switch_code_blocks.push_back({code_begin, code_end, std::move(callback)});
    }

    u64 code_begin;
    u64 code_end;
};
#else
struct ExceptionHandler::Impl final {
};
#endif

ExceptionHandler::ExceptionHandler() = default;
ExceptionHandler::~ExceptionHandler() = default;

#if defined(MCL_ARCHITECTURE_X86_64)
void ExceptionHandler::Register(X64::BlockOfCode&) {
    // Do nothing
}
#elif defined(MCL_ARCHITECTURE_ARM64)
void ExceptionHandler::Register(oaknut::CodeBlock& mem, std::size_t size) {
#if defined(__SWITCH__)
    const u64 code_begin = mcl::bit_cast<u64>(mem.ptr());
    impl = std::make_unique<Impl>(code_begin, code_begin + size);
#else
    // Do nothing
#endif
}
#elif defined(MCL_ARCHITECTURE_RISCV)
void ExceptionHandler::Register(RV64::CodeBlock&, std::size_t) {
    // Do nothing
}
#else
#    error "Invalid architecture"
#endif

bool ExceptionHandler::SupportsFastmem() const noexcept {
#if defined(__SWITCH__) && defined(MCL_ARCHITECTURE_ARM64)
    return static_cast<bool>(impl);
#else
    return false;
#endif
}

void ExceptionHandler::SetFastmemCallback(std::function<FakeCall(u64)> callback) {
#if defined(__SWITCH__) && defined(MCL_ARCHITECTURE_ARM64)
    impl->SetCallback(std::move(callback));
#else
    // Do nothing
#endif
}

}  // namespace Dynarmic::Backend
