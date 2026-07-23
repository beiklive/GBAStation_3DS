// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <csignal>
#include <cstring>
#include <thread>
#include <atomic>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/optimization_flags.h>
#include "common/assert.h"
#include "common/arch.h"
#include "common/microprofile.h"
#include "core/arm/dynarmic/arm_dynarmic.h"
#include "core/arm/dynarmic/arm_dynarmic_cp15.h"
#include "core/arm/dynarmic/arm_exclusive_monitor.h"
#include "core/arm/dynarmic/arm_tick_counts.h"
#include "core/core.h"
#include "core/core_timing.h"
#ifdef ENABLE_GDBSTUB
#include "core/gdbstub/gdbstub.h"
#endif
#include "core/hle/kernel/svc.h"
#include "core/memory.h"

#if CITRA_ARCH(arm64)
#include <dynarmic/backend/arm64/dispatch_diagnostics.h>
#endif

#ifndef SIGILL
constexpr u32 SIGILL = 4;
#endif

#ifndef SIGTRAP
constexpr u32 SIGTRAP = 5;
#endif

namespace Core {

namespace {

#ifdef GBASTATION_HOTPATH_DIAGNOSTICS
std::atomic<u64> dynarmic_jit_instances_created{};
std::atomic<u64> dynarmic_instruction_cache_clears{};
std::atomic<u64> dynarmic_cache_range_invalidations{};
std::atomic<u64> dynarmic_cache_range_invalidation_bytes{};
std::atomic<u64> dynarmic_last_code_cache_size{};
std::atomic<u32> dynarmic_last_optimization_flags{};
std::atomic<u32> dynarmic_last_hook_hint_instructions{};
std::atomic<u32> dynarmic_last_always_little_endian{};
std::atomic<u32> dynarmic_last_fastmem_enabled{};
std::atomic<u64> dynarmic_memory_read_callbacks{};
std::atomic<u64> dynarmic_memory_write_callbacks{};
std::atomic<u64> dynarmic_memory_exclusive_callbacks{};
std::atomic<u64> dynarmic_memory_code_callbacks{};
std::atomic<u32> dynarmic_last_read_callback_addr{};
std::atomic<u32> dynarmic_last_write_callback_addr{};
#endif

constexpr std::size_t SwitchCodeCacheSize = 64 * 1024 * 1024;

} // namespace

DynarmicDiagnostics GetAndResetDynarmicDiagnostics() {
#ifndef GBASTATION_HOTPATH_DIAGNOSTICS
    return {};
#else
#if CITRA_ARCH(arm64)
    const auto dispatch_stats = Dynarmic::Backend::Arm64::GetAndResetArm64DispatchDiagnostics();
#else
    constexpr DynarmicDiagnostics dispatch_stats{};
#endif
    return {
        .jit_instances_created = dynarmic_jit_instances_created.exchange(0),
        .instruction_cache_clears = dynarmic_instruction_cache_clears.exchange(0),
        .cache_range_invalidations = dynarmic_cache_range_invalidations.exchange(0),
        .cache_range_invalidation_bytes = dynarmic_cache_range_invalidation_bytes.exchange(0),
        .last_code_cache_size = dynarmic_last_code_cache_size.load(),
        .last_optimization_flags = dynarmic_last_optimization_flags.load(),
        .last_hook_hint_instructions = dynarmic_last_hook_hint_instructions.load(),
        .last_always_little_endian = dynarmic_last_always_little_endian.load(),
        .last_fastmem_enabled = dynarmic_last_fastmem_enabled.load(),
        .memory_read_callbacks = dynarmic_memory_read_callbacks.exchange(0),
        .memory_write_callbacks = dynarmic_memory_write_callbacks.exchange(0),
        .memory_exclusive_callbacks = dynarmic_memory_exclusive_callbacks.exchange(0),
        .memory_code_callbacks = dynarmic_memory_code_callbacks.exchange(0),
        .fast_dispatch_misses = dispatch_stats.fast_dispatch_misses,
        .fast_dispatch_updates = dispatch_stats.fast_dispatch_updates,
        .fast_dispatch_clears = dispatch_stats.fast_dispatch_clears,
        .fast_dispatch_false_misses = dispatch_stats.fast_dispatch_false_misses,
        .dispatcher_cache_hits = dispatch_stats.dispatcher_cache_hits,
        .dispatcher_cache_misses = dispatch_stats.dispatcher_cache_misses,
        .dispatcher_cache_collisions = dispatch_stats.dispatcher_cache_collisions,
        .top_dispatcher_descriptors = dispatch_stats.top_dispatcher_descriptors,
        .top_dispatcher_descriptor_counts = dispatch_stats.top_dispatcher_descriptor_counts,
        .last_read_callback_addr = dynarmic_last_read_callback_addr.load(),
        .last_write_callback_addr = dynarmic_last_write_callback_addr.load(),
    };
#endif
}

class DynarmicUserCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    explicit DynarmicUserCallbacks(ARM_Dynarmic& parent)
        : parent(parent), svc_context(parent.system), memory(parent.memory) {}
    ~DynarmicUserCallbacks() = default;

    std::optional<std::uint32_t> MemoryReadCode(VAddr vaddr) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_code_callbacks.fetch_add(1, std::memory_order_relaxed);
#endif
        return memory.Read32OrNullopt(vaddr);
    }

    std::uint8_t MemoryRead8(VAddr vaddr) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_read_callbacks.fetch_add(1, std::memory_order_relaxed);
        dynarmic_last_read_callback_addr.store(vaddr, std::memory_order_relaxed);
#endif
        return memory.Read8(vaddr);
    }
    std::uint16_t MemoryRead16(VAddr vaddr) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_read_callbacks.fetch_add(1, std::memory_order_relaxed);
        dynarmic_last_read_callback_addr.store(vaddr, std::memory_order_relaxed);
#endif
        return memory.Read16(vaddr);
    }
    std::uint32_t MemoryRead32(VAddr vaddr) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_read_callbacks.fetch_add(1, std::memory_order_relaxed);
        dynarmic_last_read_callback_addr.store(vaddr, std::memory_order_relaxed);
#endif
        return memory.Read32(vaddr);
    }
    std::uint64_t MemoryRead64(VAddr vaddr) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_read_callbacks.fetch_add(1, std::memory_order_relaxed);
        dynarmic_last_read_callback_addr.store(vaddr, std::memory_order_relaxed);
#endif
        return memory.Read64(vaddr);
    }

    void MemoryWrite8(VAddr vaddr, std::uint8_t value) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_write_callbacks.fetch_add(1, std::memory_order_relaxed);
        dynarmic_last_write_callback_addr.store(vaddr, std::memory_order_relaxed);
#endif
        memory.Write8(vaddr, value);
    }
    void MemoryWrite16(VAddr vaddr, std::uint16_t value) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_write_callbacks.fetch_add(1, std::memory_order_relaxed);
        dynarmic_last_write_callback_addr.store(vaddr, std::memory_order_relaxed);
#endif
        memory.Write16(vaddr, value);
    }
    void MemoryWrite32(VAddr vaddr, std::uint32_t value) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_write_callbacks.fetch_add(1, std::memory_order_relaxed);
        dynarmic_last_write_callback_addr.store(vaddr, std::memory_order_relaxed);
#endif
        memory.Write32(vaddr, value);
    }
    void MemoryWrite64(VAddr vaddr, std::uint64_t value) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_write_callbacks.fetch_add(1, std::memory_order_relaxed);
        dynarmic_last_write_callback_addr.store(vaddr, std::memory_order_relaxed);
#endif
        memory.Write64(vaddr, value);
    }

    bool MemoryWriteExclusive8(u32 vaddr, u8 value, u8 expected) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_exclusive_callbacks.fetch_add(1, std::memory_order_relaxed);
#endif
        return memory.WriteExclusive8(vaddr, value, expected);
    }
    bool MemoryWriteExclusive16(u32 vaddr, u16 value, u16 expected) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_exclusive_callbacks.fetch_add(1, std::memory_order_relaxed);
#endif
        return memory.WriteExclusive16(vaddr, value, expected);
    }
    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_exclusive_callbacks.fetch_add(1, std::memory_order_relaxed);
#endif
        return memory.WriteExclusive32(vaddr, value, expected);
    }
    bool MemoryWriteExclusive64(u32 vaddr, u64 value, u64 expected) override {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
        dynarmic_memory_exclusive_callbacks.fetch_add(1, std::memory_order_relaxed);
#endif
        return memory.WriteExclusive64(vaddr, value, expected);
    }

    void InterpreterFallback(VAddr pc, std::size_t num_instructions) override {
        // Should never happen.
        UNREACHABLE_MSG("InterpeterFallback reached with pc = 0x{:08x}, code = 0x{:08x}, num = {}",
                        pc, MemoryReadCode(pc).value(), num_instructions);
    }

    void CallSVC(std::uint32_t swi) override {
        svc_context.CallSVC(swi);
    }

    void ExceptionRaised(VAddr pc, Dynarmic::A32::Exception exception) override {
        switch (exception) {
        case Dynarmic::A32::Exception::UndefinedInstruction:
        case Dynarmic::A32::Exception::UnpredictableInstruction:
        case Dynarmic::A32::Exception::DecodeError:
        case Dynarmic::A32::Exception::NoExecuteFault:
            break;
        case Dynarmic::A32::Exception::Breakpoint:
#ifdef ENABLE_GDBSTUB
            if (GDBStub::IsConnected()) {
                parent.SetPC(pc);
                parent.ServeBreak(SIGTRAP);
                return;
            }
#endif
            break;
        case Dynarmic::A32::Exception::SendEvent:
        case Dynarmic::A32::Exception::SendEventLocal:
        case Dynarmic::A32::Exception::WaitForInterrupt:
        case Dynarmic::A32::Exception::WaitForEvent:
        case Dynarmic::A32::Exception::Yield:
#ifdef __SWITCH__
            parent.jit->HaltExecution();
#endif
            return;
        case Dynarmic::A32::Exception::PreloadData:
        case Dynarmic::A32::Exception::PreloadDataWithIntentToWrite:
        case Dynarmic::A32::Exception::PreloadInstruction:
            return;
        }

        static constexpr auto ExceptionToString = [](Dynarmic::A32::Exception e) -> std::string {
            switch (e) {
            case Dynarmic::A32::Exception::UndefinedInstruction:
                return "UndefinedInstruction";
            case Dynarmic::A32::Exception::UnpredictableInstruction:
                return "UnpredictableInstruction";
            case Dynarmic::A32::Exception::DecodeError:
                return "DecodeError";
            case Dynarmic::A32::Exception::NoExecuteFault:
                return "NoExecuteFault";
            case Dynarmic::A32::Exception::Breakpoint:
                return "Breakpoint";
            default:
                return fmt::format("Unknown({})", e);
            }
        };

        parent.SetPC(pc);
#ifdef ENABLE_GDBSTUB
        if (GDBStub::IsConnected()) {
            parent.ServeBreak(SIGILL);
        } else
#endif
        {
            std::string error;
            for (int i = 0; i < 16; i++) {
                error += fmt::format("r{:02d} = {:08X}\n", i, parent.GetReg(i));
            }
            error += fmt::format("ExceptionRaised(exception = {}, pc = {:08X})",
                                 ExceptionToString(exception), pc);
            parent.system.SetStatus(Core::System::ResultStatus::ErrorCoreExceptionRaised,
                                    error.c_str());
        }
    }

    void AddTicks(std::uint64_t ticks) override {
        parent.GetTimer().AddTicks(ticks);
    }
    std::uint64_t GetTicksRemaining() override {
        s64 ticks = parent.GetTimer().GetDowncount();
        return static_cast<u64>(ticks <= 0 ? 0 : ticks);
    }
    std::uint64_t GetTicksForCode(bool is_thumb, VAddr, std::uint32_t instruction) override {
        return Core::TicksForInstruction(is_thumb, instruction);
    }

    ARM_Dynarmic& parent;
    Kernel::SVCContext svc_context;
    Memory::MemorySystem& memory;
};

ARM_Dynarmic::ARM_Dynarmic(Core::System& system_, Memory::MemorySystem& memory_, u32 core_id_,
                           std::shared_ptr<Core::Timing::Timer> timer_,
                           Core::ExclusiveMonitor& exclusive_monitor_)
    : ARM_Interface(core_id_, timer_), system(system_), memory(memory_),
      cb(std::make_unique<DynarmicUserCallbacks>(*this)),
      exclusive_monitor{dynamic_cast<Core::DynarmicExclusiveMonitor&>(exclusive_monitor_)} {
    SetPageTable(memory.GetCurrentPageTable());
}

ARM_Dynarmic::~ARM_Dynarmic() = default;

MICROPROFILE_DEFINE(ARM_Jit, "ARM JIT", "ARM JIT", MP_RGB(255, 64, 64));

void ARM_Dynarmic::Run() {
    ASSERT(memory.GetCurrentPageTable() == current_page_table);
    MICROPROFILE_SCOPE(ARM_Jit);
    if (break_flag) [[unlikely]] {
        return;
    }

    jit->Run();
}

void ARM_Dynarmic::Step() {
    if (break_flag) [[unlikely]] {
        return;
    }

    jit->Step();
}

void ARM_Dynarmic::SetPC(u32 pc) {
    jit->Regs()[15] = pc;
}

u32 ARM_Dynarmic::GetPC() const {
    return jit->Regs()[15];
}

u32 ARM_Dynarmic::GetReg(int index) const {
    return jit->Regs()[index];
}

void ARM_Dynarmic::SetReg(int index, u32 value) {
    jit->Regs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPReg(int index) const {
    return jit->ExtRegs()[index];
}

void ARM_Dynarmic::SetVFPReg(int index, u32 value) {
    jit->ExtRegs()[index] = value;
}

u32 ARM_Dynarmic::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return jit->Fpscr();
    case VFP_FPEXC:
        return fpexc;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }

    return UINT_MAX;
}

void ARM_Dynarmic::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        jit->SetFpscr(value);
        return;
    case VFP_FPEXC:
        fpexc = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

u32 ARM_Dynarmic::GetCPSR() const {
    return jit->Cpsr();
}

void ARM_Dynarmic::SetCPSR(u32 cpsr) {
    jit->SetCpsr(cpsr);
}

u32 ARM_Dynarmic::GetCP15Register(CP15Register reg) const {
    switch (reg) {
    case CP15_THREAD_UPRW:
        return cp15_state.cp15_thread_uprw;
    case CP15_THREAD_URO:
        return cp15_state.cp15_thread_uro;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }

    return 0;
}

void ARM_Dynarmic::SetCP15Register(CP15Register reg, u32 value) {
    switch (reg) {
    case CP15_THREAD_UPRW:
        cp15_state.cp15_thread_uprw = value;
        return;
    case CP15_THREAD_URO:
        cp15_state.cp15_thread_uro = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown CP15 register: {}", reg);
    }
}

void ARM_Dynarmic::SaveContext(ThreadContext& ctx) {
    ctx.cpu_registers = jit->Regs();
    ctx.cpsr = jit->Cpsr();
    ctx.fpu_registers = jit->ExtRegs();
    ctx.fpscr = jit->Fpscr();
    ctx.fpexc = fpexc;
}

void ARM_Dynarmic::LoadContext(const ThreadContext& ctx) {
    jit->Regs() = ctx.cpu_registers;
    jit->SetCpsr(ctx.cpsr);
    jit->ExtRegs() = ctx.fpu_registers;
    jit->SetFpscr(ctx.fpscr);
    fpexc = ctx.fpexc;
}

void ARM_Dynarmic::PrepareReschedule() {
    if (jit->IsExecuting()) {
        jit->HaltExecution();
    }
}

void ARM_Dynarmic::ClearInstructionCache() {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
    dynarmic_instruction_cache_clears.fetch_add(1);
#endif
    for (const auto& j : jits) {
        j.second->ClearCache();
    }
}

void ARM_Dynarmic::InvalidateCacheRange(u32 start_address, std::size_t length) {
#if defined(__SWITCH__) && defined(GBASTATION_HOTPATH_DIAGNOSTICS)
    dynarmic_cache_range_invalidations.fetch_add(1);
    dynarmic_cache_range_invalidation_bytes.fetch_add(static_cast<u64>(length));
#endif
    jit->InvalidateCacheRange(start_address, length);
}

void ARM_Dynarmic::ClearExclusiveState() {
    jit->ClearExclusiveState();
}

std::shared_ptr<Memory::PageTable> ARM_Dynarmic::GetPageTable() const {
    return current_page_table;
}

void ARM_Dynarmic::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    current_page_table = page_table;
    ThreadContext ctx{};
    if (jit) {
        SaveContext(ctx);
    }

    auto iter = jits.find(current_page_table);
    if (iter != jits.end()) {
        jit = iter->second.get();
        LoadContext(ctx);
        return;
    }

    auto new_jit = MakeJit();
    jit = new_jit.get();
    LoadContext(ctx);
    jits.emplace(current_page_table, std::move(new_jit));
}

void ARM_Dynarmic::ServeBreak([[maybe_unused]] int signal) {
#ifdef ENABLE_GDBSTUB
    GDBStub::Break(signal);
#endif
}

std::unique_ptr<Dynarmic::A32::Jit> ARM_Dynarmic::MakeJit() {
    Dynarmic::A32::UserConfig config;
    config.callbacks = cb.get();
    if (current_page_table) {
        config.page_table = &current_page_table->GetPointerArray();
    }
    config.coprocessors[15] = std::make_shared<DynarmicCP15>(cp15_state);
    config.define_unpredictable_behaviour = true;

#ifdef __SWITCH__
    config.optimizations = Dynarmic::all_safe_optimizations;
    config.always_little_endian = true;
    // Video codecs often use PLD/PLI prefetch hints in their inner loops. Hooking every hint
    // instruction forces a callback out of generated code, so keep hints as JIT-side NOPs on
    // Switch and rely on SVC sleep/wait paths for real guest blocking.
    config.hook_hint_instructions = false;
    config.code_cache_size = SwitchCodeCacheSize;
    if (current_page_table) {
        config.fastmem_pointer = memory.GetSwitchFastmemPointer(*current_page_table);
        config.recompile_on_fastmem_failure = true;
        config.fastmem_exclusive_access = false;
    }
#ifdef GBASTATION_HOTPATH_DIAGNOSTICS
    dynarmic_jit_instances_created.fetch_add(1);
    dynarmic_last_code_cache_size.store(static_cast<u64>(config.code_cache_size));
    dynarmic_last_optimization_flags.store(static_cast<u32>(config.optimizations));
    dynarmic_last_hook_hint_instructions.store(config.hook_hint_instructions ? 1u : 0u);
    dynarmic_last_always_little_endian.store(config.always_little_endian ? 1u : 0u);
    dynarmic_last_fastmem_enabled.store(config.fastmem_pointer ? 1u : 0u);
#endif
    LOG_INFO(Core_ARM11,
             "Switch Dynarmic config: core={} optimizations=0x{:x} hook_hints={} little_endian={} code_cache={} fastmem={}",
             GetID(), static_cast<unsigned>(config.optimizations),
             config.hook_hint_instructions ? 1 : 0, config.always_little_endian ? 1 : 0,
             config.code_cache_size, config.fastmem_pointer ? 1 : 0);
#endif

    // Multi-process state
    config.processor_id = GetID();
    config.global_monitor = &exclusive_monitor.monitor;

    return std::make_unique<Dynarmic::A32::Jit>(config);
}

} // namespace Core
