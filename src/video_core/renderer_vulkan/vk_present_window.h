// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include "common/polyfill_thread.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace Frontend {
class EmuWindow;
}

namespace Vulkan {

class Instance;
class Swapchain;
class Scheduler;
class RenderManager;

using OverlayDrawCallback =
    void (*)(vk::CommandBuffer command_buffer, vk::Image image, vk::Extent2D extent,
             vk::Format format);
using OverlayResetCallback = void (*)();

// The Switch frontend installs these callbacks while its in-game menu exists.
// They are process-global because only one Vulkan presentation device is active.
void SetOverlayDrawCallback(OverlayDrawCallback callback);
void SetOverlayResetCallback(OverlayResetCallback callback);

struct Frame {
    u32 width;
    u32 height;
    VmaAllocation allocation;
    vk::Framebuffer framebuffer;
    vk::Image image;
    vk::ImageView image_view;
    vk::Semaphore render_ready;
    vk::Fence present_done;
    vk::CommandBuffer cmdbuf;
};

class PresentWindow final {
public:
    explicit PresentWindow(Frontend::EmuWindow& emu_window, const Instance& instance,
                           Scheduler& scheduler, bool low_refresh_rate);
    ~PresentWindow();

    /// Waits for all queued frames to finish presenting.
    void WaitPresent();

    /// Returns a reusable render frame, or nullptr when presentation is stalled.
    Frame* GetRenderFrame();

    /// Recreates the render frame to match provided parameters.
    void RecreateFrame(Frame* frame, u32 width, u32 height);

    /// Queues the provided frame for presentation.
    void Present(Frame* frame);

    /// This is called to notify the rendering backend of a surface change
    void NotifySurfaceChanged();

    /// Stops new frames from reaching the swapchain and releases the surface for an applet
    /// transition. Emulation must be paused before calling this method.
    bool SuspendPresentation();

    /// Recreates the surface and swapchain after SuspendPresentation().
    void ResumePresentation();

    [[nodiscard]] vk::RenderPass Renderpass() const noexcept {
        return present_renderpass;
    }

    u32 ImageCount() const noexcept {
        return swapchain.GetImageCount();
    }

    vk::Format GetSurfaceFormat() const noexcept {
        return swapchain.GetSurfaceFormat().format;
    }

private:
    void PresentThread(std::stop_token token);

    void CopyToSwapchain(Frame* frame);

    vk::RenderPass CreateRenderpass();

private:
    Frontend::EmuWindow& emu_window;
    const Instance& instance;
    Scheduler& scheduler;
    bool low_refresh_rate;
    vk::SurfaceKHR surface;
    vk::SurfaceKHR next_surface{};
    Swapchain swapchain;
    vk::CommandPool command_pool;
    vk::Queue graphics_queue;
    vk::RenderPass present_renderpass;
    std::vector<Frame> swap_chain;
    std::queue<Frame*> free_queue;
    std::queue<Frame*> present_queue;
    std::condition_variable free_cv;
    std::condition_variable recreate_surface_cv;
    std::condition_variable_any frame_cv;
    std::timed_mutex swapchain_mutex;
    std::mutex recreate_surface_mutex;
    std::mutex queue_mutex;
    std::mutex free_mutex;
    std::jthread present_thread;
    std::atomic_bool surface_recreate_requested{false};
    // Guarded by queue_mutex for Present() ordering; atomic reads are used by status probes.
    std::atomic_bool presentation_suspended{false};
    u32 suspended_width{};
    u32 suspended_height{};
    bool vsync_enabled{};
    bool blit_supported;
    bool use_present_thread{true};
    void* last_render_surface{};
};

} // namespace Vulkan
