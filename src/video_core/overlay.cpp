// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <mutex>

#include "video_core/overlay.h"

namespace VideoCore {

namespace {
std::mutex s_mutex;
OverlayMenuState s_state;
std::atomic<bool> s_visible{false};
std::atomic<bool> s_fps_visible{false};
std::atomic<float> s_fps{0.0f};
std::atomic<bool> s_shader_compile_notice_visible{true};
std::atomic<u32> s_pending_shader_compiles{0};
std::atomic<u64> s_shader_compile_generation{0};
} // namespace

void SetOverlayMenuState(const OverlayMenuState& state) {
    {
        std::scoped_lock lock{s_mutex};
        s_state = state;
    }
    s_visible.store(state.visible, std::memory_order_release);
}

OverlayMenuState GetOverlayMenuState() {
    std::scoped_lock lock{s_mutex};
    return s_state;
}

bool IsOverlayMenuVisible() {
    return s_visible.load(std::memory_order_acquire);
}

void SetFpsOverlayState(bool visible, float fps) {
    s_fps.store(fps, std::memory_order_release);
    s_fps_visible.store(visible, std::memory_order_release);
}

bool GetFpsOverlayState(float& fps) {
    fps = s_fps.load(std::memory_order_acquire);
    return s_fps_visible.load(std::memory_order_acquire);
}

void SetShaderCompileNoticeState(bool visible) {
    s_shader_compile_notice_visible.store(visible, std::memory_order_release);
}

bool GetShaderCompileNoticeState() {
    return s_shader_compile_notice_visible.load(std::memory_order_acquire);
}

void NotifyShaderCompileBegin() {
    s_pending_shader_compiles.fetch_add(1, std::memory_order_acq_rel);
    s_shader_compile_generation.fetch_add(1, std::memory_order_acq_rel);
}

void NotifyShaderCompileEnd() {
    s_pending_shader_compiles.fetch_sub(1, std::memory_order_acq_rel);
}

u32 GetPendingShaderCompiles() {
    return s_pending_shader_compiles.load(std::memory_order_acquire);
}

u64 GetShaderCompileGeneration() {
    return s_shader_compile_generation.load(std::memory_order_acquire);
}

} // namespace VideoCore
