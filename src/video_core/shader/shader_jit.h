// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/arch.h"
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)

#include <cstddef>
#include <memory>
#include <unordered_map>
#include "common/arch.h"
#if CITRA_ARCH(arm64)
#include <oaknut/code_block.hpp>
#endif
#include "common/common_types.h"
#include "video_core/shader/shader.h"
#ifdef __SWITCH__
#include <atomic>
#include <mutex>
#include "common/thread_worker.h"
#include "video_core/pica/shader_setup.h"
#endif

namespace Pica::Shader {

class InterpreterEngine;
class JitShader;

#ifdef __SWITCH__
[[nodiscard]] std::size_t GetPendingJitCompilationCount() noexcept;
#endif

class JitEngine final : public ShaderEngine {
public:
    JitEngine();
    ~JitEngine() override;

    void SetupBatch(ShaderSetup& setup, u32 entry_point) override;
    void Run(const ShaderSetup& setup, ShaderUnit& state) const override;

private:
#if CITRA_ARCH(arm64)
    // One shared code pool for all compiled shaders — avoids per-shader kernel JIT handles.
    static constexpr std::size_t kCodePoolSize = 32 * 1024 * 1024; // 32 MiB
    std::unique_ptr<oaknut::CodeBlock> code_pool;
    std::size_t pool_write_pos = 0;
#endif

#ifdef __SWITCH__
    struct CacheEntry {
        std::unique_ptr<JitShader> shader;
        std::atomic<bool> ready{false};
        bool installed{};
    };

    void CompileEntry(CacheEntry* entry, std::shared_ptr<const ProgramCode> program_code,
                      std::shared_ptr<const SwizzleData> swizzle_data);
    bool InstallCompiledShader(CacheEntry*& entry, u64 cache_key);

    std::unordered_map<u64, std::unique_ptr<CacheEntry>> cache;
    std::mutex cache_mutex;
    std::atomic<bool> compile_memory_exhausted{false};
    std::unique_ptr<InterpreterEngine> interpreter;
    // Keep this last so its worker is joined before the cache and code pool are destroyed.
    Common::ThreadWorker compile_worker;
#else
    std::unordered_map<u64, std::unique_ptr<JitShader>> cache;
#endif
};

} // namespace Pica::Shader

#endif // CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)
