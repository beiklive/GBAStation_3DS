// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/arch.h"
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)

#include <cstring>
#include <exception>
#include <new>
#include "common/assert.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "video_core/overlay.h"
#include "video_core/shader/shader.h"
#include "video_core/shader/shader_interpreter.h"
#include "video_core/shader/shader_jit.h"
#if CITRA_ARCH(arm64)
#include "video_core/shader/shader_jit_a64_compiler.h"
#endif
#if CITRA_ARCH(x86_64)
#include "video_core/shader/shader_jit_x64_compiler.h"
#endif

namespace Pica::Shader {

#ifdef __SWITCH__
namespace {
std::atomic<std::size_t> pending_jit_compilations{};
}

std::size_t GetPendingJitCompilationCount() noexcept {
    return pending_jit_compilations.load(std::memory_order_acquire);
}

JitEngine::JitEngine()
    : code_pool(std::make_unique<oaknut::CodeBlock>(kCodePoolSize)),
      interpreter(std::make_unique<InterpreterEngine>()), compile_worker(1, "PICA Shader JIT") {}
#elif CITRA_ARCH(arm64)
JitEngine::JitEngine() : code_pool(std::make_unique<oaknut::CodeBlock>(kCodePoolSize)) {}
#else
JitEngine::JitEngine() = default;
#endif
JitEngine::~JitEngine() = default;

#ifdef __SWITCH__
void JitEngine::CompileEntry(CacheEntry* entry, std::shared_ptr<const ProgramCode> program_code,
                             std::shared_ptr<const SwizzleData> swizzle_data) {
    std::unique_ptr<JitShader> shader;
    if (!compile_memory_exhausted.load(std::memory_order_relaxed)) {
        try {
            shader = std::make_unique<JitShader>();
            shader->Compile(program_code.get(), swizzle_data.get());
        } catch (const std::bad_alloc&) {
            LOG_WARNING(HW_GPU,
                        "Out of memory compiling a PICA shader, using the interpreter for this "
                        "and subsequent new shaders");
            compile_memory_exhausted.store(true, std::memory_order_relaxed);
            shader.reset();
        } catch (const std::exception& e) {
            LOG_ERROR(HW_GPU, "Failed to compile PICA shader, using the interpreter: {}", e.what());
            shader.reset();
        }
    }

    entry->shader = std::move(shader);
    entry->ready.store(true, std::memory_order_release);
    pending_jit_compilations.fetch_sub(1, std::memory_order_release);
    VideoCore::NotifyShaderCompileEnd();
}

bool JitEngine::InstallCompiledShader(CacheEntry*& entry, u64 cache_key) {
    if (entry->installed) {
        return entry->shader != nullptr;
    }
    if (!entry->shader) {
        entry->installed = true;
        return false;
    }

    const std::size_t code_size = entry->shader->GetCompiledSize();
    const std::size_t aligned_size = (code_size + 3u) & ~std::size_t{3u};
    if (aligned_size > kCodePoolSize) {
        LOG_ERROR(HW_GPU, "Compiled PICA shader is too large for the shared JIT pool: {} bytes",
                  code_size);
        entry->shader.reset();
        entry->installed = true;
        return false;
    }

    if (pool_write_pos + aligned_size > kCodePoolSize) {
        // Cache entries are captured by compile jobs. Drain the single worker before replacing
        // the map so no queued job can publish into an entry that has just been destroyed.
        compile_worker.WaitForRequests();
        auto current_shader = std::move(entry->shader);
        std::size_t evicted_count{};
        {
            std::lock_guard lock{cache_mutex};
            evicted_count = cache.size();
            cache.clear();
            auto replacement = std::make_unique<CacheEntry>();
            replacement->shader = std::move(current_shader);
            replacement->ready.store(true, std::memory_order_relaxed);
            entry = replacement.get();
            cache.emplace(cache_key, std::move(replacement));
        }
        pool_write_pos = 0;
        LOG_WARNING(Render_Software, "Shader JIT pool full ({} shaders evicted), resetting",
                    evicted_count);
    }

    code_pool->unprotect();
    auto* const writable = reinterpret_cast<std::byte*>(code_pool->wptr()) + pool_write_pos;
    auto* const executable = reinterpret_cast<std::byte*>(code_pool->xptr()) + pool_write_pos;
    std::memcpy(writable, entry->shader->GetCompiledCode().data(), code_size);
    code_pool->invalidate(reinterpret_cast<std::uint32_t*>(executable), code_size);
    code_pool->protect();
    entry->shader->FinalizePool(executable);
    pool_write_pos += aligned_size;
    entry->installed = true;
    return true;
}
#endif

void JitEngine::SetupBatch(ShaderSetup& setup, u32 entry_point) {
    ASSERT(entry_point < MAX_PROGRAM_CODE_LENGTH);
    setup.entry_point = entry_point;

    setup.DoProgramCodeFixup();
    const u64 code_hash = setup.GetProgramCodeHash();
    const u64 swizzle_hash = setup.GetSwizzleDataHash();

    const u64 cache_key = Common::HashCombine(code_hash, swizzle_hash);
#ifdef __SWITCH__
    CacheEntry* entry{};
    bool is_new{};
    {
        std::lock_guard lock{cache_mutex};
        auto [iter, inserted] = cache.try_emplace(cache_key);
        if (inserted) {
            iter->second = std::make_unique<CacheEntry>();
        }
        entry = iter->second.get();
        is_new = inserted;
    }

    if (is_new) {
        // The live ShaderSetup is mutated by later command lists, so the worker must own copies.
        auto program_code = std::make_shared<ProgramCode>(setup.GetProgramCode());
        auto swizzle_data = std::make_shared<SwizzleData>(setup.GetSwizzleData());
        pending_jit_compilations.fetch_add(1, std::memory_order_release);
        VideoCore::NotifyShaderCompileBegin();
        compile_worker.QueueWork([this, entry, program_code, swizzle_data] {
            CompileEntry(entry, program_code, swizzle_data);
        });
    }

    if (!entry->ready.load(std::memory_order_acquire)) {
        setup.cached_shader = nullptr;
        return;
    }
    setup.cached_shader = InstallCompiledShader(entry, cache_key) ? entry->shader.get() : nullptr;
#else
    auto iter = cache.find(cache_key);
    if (iter != cache.end()) {
        setup.cached_shader = iter->second.get();
    } else {
        auto shader = std::make_unique<JitShader>();
        shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData());

#if CITRA_ARCH(arm64)
        const std::size_t code_size = shader->GetCompiledSize();
        // Align to 4 bytes (ARM64 instruction size).
        const std::size_t aligned_size = (code_size + 3u) & ~3u;

        if (pool_write_pos + aligned_size > kCodePoolSize) {
            // Pool full — evict all cached shaders and reuse from the beginning.
            // SetupBatch is always called immediately before Run for the same setup, so
            // dangling cached_shader pointers in other setups are re-resolved on their
            // next SetupBatch call before they are used.
            LOG_WARNING(Render_Software, "Shader JIT pool full ({} shaders evicted), resetting",
                        cache.size());
            cache.clear();
            pool_write_pos = 0;
            iter = cache.end(); // cache is empty; use end() as emplace hint below
        }

        // The libnx JIT mapping is W^X: after any shader has executed the pool is RX, so
        // every subsequent append must transition the whole pool back to writable first.
        // Missing this transition faults on the first shader upload when jitCreate starts RX.
        code_pool->unprotect();

        // Copy compiled code into the shared pool's writable alias.
        auto* const wptr = reinterpret_cast<std::byte*>(code_pool->wptr()) + pool_write_pos;
        auto* const xptr = reinterpret_cast<std::byte*>(code_pool->xptr()) + pool_write_pos;
        std::memcpy(wptr, shader->GetCompiledCode().data(), code_size);

        // Flush the written range where required, then make the executable alias runnable.
        // On Switch the cache maintenance is performed by jitTransitionToExecutable().
        code_pool->invalidate(reinterpret_cast<std::uint32_t*>(xptr), code_size);
        code_pool->protect();
        shader->FinalizePool(xptr);

        pool_write_pos += aligned_size;
#endif

        setup.cached_shader = shader.get();
        cache.emplace_hint(iter, cache_key, std::move(shader));
    }
#endif
}

MICROPROFILE_DECLARE(GPU_Shader);

void JitEngine::Run(const ShaderSetup& setup, ShaderUnit& state) const {
#ifdef __SWITCH__
    if (setup.cached_shader == nullptr) {
        interpreter->Run(setup, state);
        return;
    }
#else
    ASSERT(setup.cached_shader != nullptr);
#endif

    MICROPROFILE_SCOPE(GPU_Shader);

    const JitShader* shader = static_cast<const JitShader*>(setup.cached_shader);
    shader->Run(setup, state, setup.entry_point);
}

} // namespace Pica::Shader

#endif // CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)
