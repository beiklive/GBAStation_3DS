// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "common/color.h"
#include "common/logging/log.h"
#include "common/memory_detect.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "video_core/gpu.h"
#include "video_core/overlay.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/overlay_font.h"
#include "video_core/renderer_vulkan/vk_memory_util.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include "video_core/host_shaders/vulkan_present_anaglyph_frag.h"
#include "video_core/host_shaders/vulkan_present_frag.h"
#include "video_core/host_shaders/vulkan_present_interlaced_frag.h"
#include "video_core/host_shaders/vulkan_present_vert.h"

#include "video_core/host_shaders/vulkan_cursor_frag.h"
#include "video_core/host_shaders/vulkan_cursor_vert.h"
#include "video_core/host_shaders/vulkan_overlay_frag.h"
#include "video_core/host_shaders/vulkan_overlay_vert.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include <vk_mem_alloc.h>
#if defined(__APPLE__) && !defined(HAVE_LIBRETRO)
#include "common/apple_utils.h"
#endif

#ifdef ENABLE_SDL2
#include <SDL.h>
#endif

MICROPROFILE_DEFINE(Vulkan_RenderFrame, "Vulkan", "Render Frame", MP_RGB(128, 128, 64));

namespace Vulkan {

struct ScreenRectVertex {
    ScreenRectVertex() = default;
    ScreenRectVertex(float x, float y, float u, float v)
        : position{Common::MakeVec(x, y)}, tex_coord{Common::MakeVec(u, v)} {}

    Common::Vec2f position;
    Common::Vec2f tex_coord;
};

struct CursorVertex {
    float x;
    float y;
    float r;
    float g;
    float b;
    float a;
};

struct CursorSpan {
    float x;
    float y;
    float width;
    float height;
};

constexpr u32 VERTEX_BUFFER_SIZE = sizeof(ScreenRectVertex) * 8192;
constexpr u32 OVERLAY_VERTEX_BUFFER_SIZE = sizeof(float) * 4 * 32768;
constexpr u32 FRAMEBUFFER_UPLOAD_BUFFER_SIZE = 4 * 1024 * 1024;

constexpr std::array<f32, 4 * 4> MakeOrthographicMatrix(u32 width, u32 height) {
    // clang-format off
    return { 2.f / width, 0.f,         0.f, -1.f,
            0.f,         2.f / height, 0.f, -1.f,
            0.f,         0.f,          1.f,  0.f,
            0.f,         0.f,          0.f,  1.f};
    // clang-format on
}

constexpr static std::array<vk::DescriptorSetLayoutBinding, 1> PRESENT_BINDINGS = {{
    {0, vk::DescriptorType::eCombinedImageSampler, 3, vk::ShaderStageFlagBits::eFragment},
}};

namespace {

Common::Vec4<u8> DecodeFramebufferPixel(Pica::PixelFormat format, const u8* src) {
    switch (format) {
    case Pica::PixelFormat::RGBA8:
        return Common::Color::DecodeRGBA8(src);
    case Pica::PixelFormat::RGB8:
        return Common::Color::DecodeRGB8(src);
    case Pica::PixelFormat::RGB565:
        return Common::Color::DecodeRGB565(src);
    case Pica::PixelFormat::RGB5A1:
        return Common::Color::DecodeRGB5A1(src);
    case Pica::PixelFormat::RGBA4:
        return Common::Color::DecodeRGBA4(src);
    default:
        UNREACHABLE();
    }
}

static bool IsLowRefreshRate() {
#if (defined(__APPLE__) || defined(ENABLE_SDL2)) && !defined(HAVE_LIBRETRO)
    if (!Settings::values.use_display_refresh_rate_detection) {
        LOG_INFO(Render_Vulkan, "Refresh rate detection is currently disabled via settings");
        return false;
    }
#ifdef __APPLE__
    // Apple's low power mode sometimes limits applications to 30fps without changing the refresh
    // rate, meaning the above code doesn't catch it.
    if (AppleUtils::IsLowPowerModeEnabled()) {
        LOG_WARNING(Render_Vulkan, "Apple's low power mode is enabled, assuming low application "
                                   "framerate. FIFO will be disabled");
        return true;
    }

    const auto cur_refresh_rate = AppleUtils::GetRefreshRate();
#elif defined(ENABLE_SDL2)
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        LOG_ERROR(Render_Vulkan, "Attempted to check refresh rate via SDL, but failed because "
                                 "SDL_INIT_VIDEO wasn't initialized");
        return false;
    }

    SDL_DisplayMode cur_display_mode;
    SDL_GetCurrentDisplayMode(0, &cur_display_mode); // TODO: Multimonitor handling. -OS

    const auto cur_refresh_rate = cur_display_mode.refresh_rate;
#endif // ENABLE_SDL2

    if (cur_refresh_rate < SCREEN_REFRESH_RATE) {
        LOG_WARNING(Render_Vulkan,
                    "Detected refresh rate lower than the emulated 3DS screen: {}hz. FIFO will "
                    "be disabled",
                    cur_refresh_rate);
        return true;
    } else {
        LOG_INFO(Render_Vulkan, "Refresh rate is above emulated 3DS screen: {}hz. Good.",
                 cur_refresh_rate);
    }
#endif // (defined(__APPLE__) || defined(ENABLE_SDL2)) && !defined(HAVE_LIBRETRO)

    // We have no available method of checking refresh rate. Just assume that everything is fine :)
    return false;
}
} // Anonymous namespace

RendererVulkan::RendererVulkan(Core::System& system, Pica::PicaCore& pica_,
                               Frontend::EmuWindow& window, Frontend::EmuWindow* secondary_window)
    : RendererBase{system, window, secondary_window}, memory{system.Memory()}, pica{pica_},
      instance{window, Settings::values.physical_device.GetValue()}, scheduler{instance},
      renderpass_cache{instance, scheduler},
      main_present_window{window, instance, scheduler, IsLowRefreshRate()},
      vertex_buffer{instance, scheduler, vk::BufferUsageFlagBits::eVertexBuffer,
                    VERTEX_BUFFER_SIZE},
      overlay_vertex_buffer{instance, scheduler, vk::BufferUsageFlagBits::eVertexBuffer,
                            OVERLAY_VERTEX_BUFFER_SIZE},
      framebuffer_upload_buffer{instance, scheduler, vk::BufferUsageFlagBits::eTransferSrc,
                                FRAMEBUFFER_UPLOAD_BUFFER_SIZE, BufferType::Upload},
      update_queue{instance}, rasterizer{memory,
                                         pica,
                                         system.CustomTexManager(),
                                         *this,
                                         render_window,
                                         instance,
                                         scheduler,
                                         renderpass_cache,
                                         update_queue,
                                         main_present_window.ImageCount()},
      present_heap{instance, scheduler.GetMasterSemaphore(), PRESENT_BINDINGS, 32} {
    CompileShaders();
    CreateOverlayFont();
    BuildLayouts();
    BuildPipelines();
    if (secondary_window) {
        secondary_present_window_ptr = std::make_unique<PresentWindow>(
            *secondary_window, instance, scheduler, IsLowRefreshRate());
    }
}

RendererVulkan::~RendererVulkan() {
    vk::Device device = instance.GetDevice();
    scheduler.Finish();
    main_present_window.WaitPresent();
    device.waitIdle();

    device.destroyShaderModule(present_vertex_shader);
    for (u32 i = 0; i < PRESENT_PIPELINES; i++) {
        device.destroyPipeline(present_pipelines[i]);
        device.destroyShaderModule(present_shaders[i]);
    }

    for (auto& sampler : present_samplers) {
        device.destroySampler(sampler);
    }

    for (auto& info : screen_infos) {
        device.destroyImageView(info.texture.image_view);
        vmaDestroyImage(instance.GetAllocator(), info.texture.image, info.texture.allocation);
    }

    device.destroyPipeline(cursor_pipeline);
    device.destroyShaderModule(cursor_vertex_shader);
    device.destroyShaderModule(cursor_fragment_shader);

    device.destroyPipeline(overlay_pipeline);
    device.destroyShaderModule(overlay_vertex_shader);
    device.destroyShaderModule(overlay_fragment_shader);
    device.destroySampler(overlay_font_sampler);
    device.destroyImageView(overlay_font_view);
    vmaDestroyImage(instance.GetAllocator(), overlay_font_image, overlay_font_allocation);
    OverlayFont::Shutdown();
}

void RendererVulkan::PrepareRendertarget() {
    const auto& framebuffer_config = pica.regs.framebuffer_config;
    const auto& regs_lcd = pica.regs_lcd;
    for (u32 i = 0; i < 3; i++) {
        const u32 fb_id = i == 2 ? 1 : 0;
        const auto& framebuffer = framebuffer_config[fb_id];
        auto& texture = screen_infos[i].texture;

        const auto color_fill = fb_id == 0 ? regs_lcd.color_fill_top : regs_lcd.color_fill_bottom;
        if (!texture.image || texture.width != framebuffer.width ||
            texture.height != framebuffer.height || texture.format != framebuffer.color_format) {
            ConfigureFramebufferTexture(texture, framebuffer);
        }

        if (color_fill.is_enabled) {
            screen_infos[i].image_view = texture.image_view;
            screen_infos[i].texcoords = {0.f, 0.f, 1.f, 1.f};
            FillScreen(color_fill.AsVector(), texture);
            continue;
        }

        LoadFBToScreenInfo(framebuffer, screen_infos[i], i == 1);
    }
}

void RendererVulkan::PrepareDraw(Frame* frame, const Layout::FramebufferLayout& layout) {
    const auto sampler = present_samplers[!Settings::values.filter_mode.GetValue()];
    const auto present_set = present_heap.Commit();
    for (u32 index = 0; index < screen_infos.size(); index++) {
        update_queue.AddImageSampler(present_set, 0, index, screen_infos[index].image_view,
                                     sampler);
    }

    renderpass_cache.EndRendering();
    scheduler.Record([this, layout, frame, present_set,
                      renderpass = main_present_window.Renderpass(),
                      index = current_pipeline](vk::CommandBuffer cmdbuf) {
        const vk::Viewport viewport = {
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(layout.width),
            .height = static_cast<float>(layout.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };

        const vk::Rect2D scissor = {
            .offset = {0, 0},
            .extent = {layout.width, layout.height},
        };

        cmdbuf.setViewport(0, viewport);
        cmdbuf.setScissor(0, scissor);

        const vk::ClearValue clear{.color = clear_color};
        const vk::PipelineLayout layout{*present_pipeline_layout};
        const vk::RenderPassBeginInfo renderpass_begin_info = {
            .renderPass = renderpass,
            .framebuffer = frame->framebuffer,
            .renderArea =
                vk::Rect2D{
                    .offset = {0, 0},
                    .extent = {frame->width, frame->height},
                },
            .clearValueCount = 1,
            .pClearValues = &clear,
        };

        cmdbuf.beginRenderPass(renderpass_begin_info, vk::SubpassContents::eInline);
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, present_pipelines[index]);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, present_set, {});
    });
}

void RendererVulkan::RenderToWindow(PresentWindow& window, const Layout::FramebufferLayout& layout,
                                    bool flipped, bool force_present) {
    if (force_present || !Settings::values.use_skip_duplicate_frames.GetValue() ||
        Core::PerfStats::game_frames_updated) {
        Frame* frame = window.GetRenderFrame();

        if (layout.width != frame->width || layout.height != frame->height) {
            window.WaitPresent();
            scheduler.Finish();
            window.RecreateFrame(frame, layout.width, layout.height);
        }

        clear_color.float32[0] = Settings::values.bg_red.GetValue();
        clear_color.float32[1] = Settings::values.bg_green.GetValue();
        clear_color.float32[2] = Settings::values.bg_blue.GetValue();
        clear_color.float32[3] = 1.0f;

        DrawScreens(frame, layout, flipped);
        scheduler.Flush(frame->render_ready);
        window.Present(frame);
        if ((secondaryWindowEnabled && isSecondaryWindow) || (!secondaryWindowEnabled)) {
            Core::PerfStats::game_frames_updated = false;
            screenRendered = true;
        }
    }
}


void RendererVulkan::TryPresent([[maybe_unused]] int timeout_ms, bool is_secondary) {
    PresentWindow* window = &main_present_window;
    const Layout::FramebufferLayout* layout = &render_window.GetFramebufferLayout();

    if (is_secondary) {
        if (!secondary_present_window_ptr || !secondary_window) {
            return;
        }
        window = secondary_present_window_ptr.get();
        layout = &secondary_window->GetFramebufferLayout();
    }

    Frame* frame = window->GetRenderFrame();
    if (layout->width != frame->width || layout->height != frame->height) {
        window->WaitPresent();
        scheduler.Finish();
        window->RecreateFrame(frame, layout->width, layout->height);
    }

    clear_color.float32[0] = Settings::values.bg_red.GetValue();
    clear_color.float32[1] = Settings::values.bg_green.GetValue();
    clear_color.float32[2] = Settings::values.bg_blue.GetValue();
    clear_color.float32[3] = 1.0f;

    const vk::ClearValue clear{.color = clear_color};
    const vk::RenderPass renderpass = window->Renderpass();
    scheduler.Record([frame, renderpass, clear](vk::CommandBuffer cmdbuf) {
        const vk::RenderPassBeginInfo renderpass_begin_info = {
            .renderPass = renderpass,
            .framebuffer = frame->framebuffer,
            .renderArea =
                vk::Rect2D{
                    .offset = {0, 0},
                    .extent = {frame->width, frame->height},
                },
            .clearValueCount = 1,
            .pClearValues = &clear,
        };

        cmdbuf.beginRenderPass(renderpass_begin_info, vk::SubpassContents::eInline);
        cmdbuf.endRenderPass();
    });
    scheduler.Flush(frame->render_ready);
    window->Present(frame);
    scheduler.DispatchWork();
}

void RendererVulkan::PresentLastFrame() {
    const Layout::FramebufferLayout& layout = render_window.GetFramebufferLayout();
    isSecondaryWindow = false;
    RenderToWindow(main_present_window, layout, false, true);
    scheduler.DispatchWork();
}

void RendererVulkan::SetFastForward(bool enabled, float multiplier) {
    fast_forward_enabled = enabled;
    fast_forward_present_divisor =
        enabled ? std::max(1U, static_cast<u32>(std::floor(std::max(1.0f, multiplier)))) : 1U;
    fast_forward_present_counter = 0;
}

void RendererVulkan::LoadFBToScreenInfo(const Pica::FramebufferConfig& framebuffer,
                                        ScreenInfo& screen_info, bool right_eye) {

    if (framebuffer.address_right1 == 0 || framebuffer.address_right2 == 0) {
        right_eye = false;
    }

    const PAddr framebuffer_addr =
        framebuffer.active_fb == 0
            ? (right_eye ? framebuffer.address_right1 : framebuffer.address_left1)
            : (right_eye ? framebuffer.address_right2 : framebuffer.address_left2);

    LOG_TRACE(Render_Vulkan, "0x{:08x} bytes from 0x{:08x}({}x{}), fmt {:x}",
              framebuffer.stride * framebuffer.height, framebuffer_addr, framebuffer.width.Value(),
              framebuffer.height.Value(), framebuffer.format);

    const u32 bpp = Pica::BytesPerPixel(framebuffer.color_format);
    const std::size_t pixel_stride = framebuffer.stride / bpp;

    ASSERT(pixel_stride * bpp == framebuffer.stride);
    ASSERT(pixel_stride % 4 == 0);

    if (!rasterizer.AccelerateDisplay(framebuffer, framebuffer_addr, static_cast<u32>(pixel_stride),
                                      screen_info)) {
        UploadFramebufferToScreenInfo(framebuffer, screen_info, framebuffer_addr,
                                      static_cast<u32>(pixel_stride));
    }
}

void RendererVulkan::UploadFramebufferToScreenInfo(const Pica::FramebufferConfig& framebuffer,
                                                   ScreenInfo& screen_info,
                                                   PAddr framebuffer_addr,
                                                   u32 pixel_stride) {
    const u32 width = framebuffer.width;
    const u32 height = framebuffer.height;
    if (width == 0 || height == 0) {
        return;
    }

    const VideoCore::PixelFormat pixel_format =
        VideoCore::PixelFormatFromGPUPixelFormat(framebuffer.color_format);
    if (pixel_format == VideoCore::PixelFormat::Invalid) {
        LOG_ERROR(Render_Vulkan, "Unsupported display framebuffer format {}", framebuffer.format);
        return;
    }

    rasterizer.FlushRegion(framebuffer_addr, framebuffer.stride * framebuffer.height);

    const u8* framebuffer_data = memory.GetPhysicalPointer(framebuffer_addr);
    if (!framebuffer_data) {
        LOG_ERROR(Render_Vulkan, "Display framebuffer address 0x{:08x} is not mapped",
                  framebuffer_addr);
        return;
    }

    const auto& traits = instance.GetTraits(pixel_format);
    const bool convert_to_rgba8 = traits.needs_conversion;
    const u32 bpp = Pica::BytesPerPixel(framebuffer.color_format);
    const u32 upload_row_bytes = convert_to_rgba8 ? width * 4 : framebuffer.stride;
    const u32 upload_size = upload_row_bytes * height;
    if (upload_size > FRAMEBUFFER_UPLOAD_BUFFER_SIZE) {
        LOG_ERROR(Render_Vulkan,
                  "Display framebuffer upload too large: {} bytes ({}x{}, stride {}, fmt {})",
                  upload_size, width, height, framebuffer.stride, framebuffer.format);
        return;
    }

    auto [data, offset, invalidate] = framebuffer_upload_buffer.Map(upload_size, 16);
    if (convert_to_rgba8) {
        for (u32 y = 0; y < height; ++y) {
            const u8* src_row = framebuffer_data + y * framebuffer.stride;
            u8* dst_row = data + y * upload_row_bytes;
            for (u32 x = 0; x < width; ++x) {
                const auto color = DecodeFramebufferPixel(framebuffer.color_format, src_row + x * bpp);
                std::memcpy(dst_row + x * 4, color.AsArray(), 4);
            }
        }
    } else {
        std::memcpy(data, framebuffer_data, upload_size);
    }

    static u32 fallback_log_count = 0;
    if (fallback_log_count < 12) {
        LOG_INFO(Render_Vulkan,
                 "Switch display fallback upload: addr=0x{:08x}, {}x{}, stride={}, fmt={}, "
                 "vk_format={}, converted={}",
                 framebuffer_addr, width, height, framebuffer.stride,
                 VideoCore::PixelFormatAsString(pixel_format), vk::to_string(traits.native),
                 convert_to_rgba8);
        ++fallback_log_count;
    }

    screen_info.image_view = screen_info.texture.image_view;
    screen_info.texcoords = {0.f, 0.f, 1.f, 1.f};

    renderpass_cache.EndRendering();
    scheduler.Record([buffer = framebuffer_upload_buffer.Handle(), image = screen_info.texture.image,
                      offset = offset, width, height, row_length = pixel_stride,
                      convert_to_rgba8](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };

        const vk::ImageMemoryBarrier pre_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        const vk::ImageMemoryBarrier post_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        const vk::BufferImageCopy copy = {
            .bufferOffset = offset,
            .bufferRowLength = convert_to_rgba8 ? 0U : row_length,
            .bufferImageHeight = 0,
            .imageSubresource =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, 1},
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader |
                                   vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);
        cmdbuf.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, copy);
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eFragmentShader,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);
    });
    framebuffer_upload_buffer.Commit(upload_size);
}

void RendererVulkan::CompileShaders() {
    const vk::Device device = instance.GetDevice();
    const std::string_view preamble =
        instance.IsImageArrayDynamicIndexSupported() ? "#define ARRAY_DYNAMIC_INDEX" : "";
    present_vertex_shader =
        Compile(HostShaders::VULKAN_PRESENT_VERT, vk::ShaderStageFlagBits::eVertex, device);
    present_shaders[0] = Compile(HostShaders::VULKAN_PRESENT_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);
    present_shaders[1] = Compile(HostShaders::VULKAN_PRESENT_ANAGLYPH_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);
    present_shaders[2] = Compile(HostShaders::VULKAN_PRESENT_INTERLACED_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);

    cursor_vertex_shader =
        Compile(HostShaders::VULKAN_CURSOR_VERT, vk::ShaderStageFlagBits::eVertex, device);
    cursor_fragment_shader =
        Compile(HostShaders::VULKAN_CURSOR_FRAG, vk::ShaderStageFlagBits::eFragment, device);

    overlay_vertex_shader =
        Compile(HostShaders::VULKAN_OVERLAY_VERT, vk::ShaderStageFlagBits::eVertex, device);
    overlay_fragment_shader =
        Compile(HostShaders::VULKAN_OVERLAY_FRAG, vk::ShaderStageFlagBits::eFragment, device);

    auto properties = instance.GetPhysicalDevice().getProperties();
    for (std::size_t i = 0; i < present_samplers.size(); i++) {
        const vk::Filter filter_mode = i == 0 ? vk::Filter::eLinear : vk::Filter::eNearest;
        const vk::SamplerCreateInfo sampler_info = {
            .magFilter = filter_mode,
            .minFilter = filter_mode,
            .mipmapMode = vk::SamplerMipmapMode::eLinear,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .anisotropyEnable = instance.IsAnisotropicFilteringSupported(),
            .maxAnisotropy = properties.limits.maxSamplerAnisotropy,
            .compareEnable = false,
            .compareOp = vk::CompareOp::eAlways,
            .borderColor = vk::BorderColor::eIntOpaqueBlack,
            .unnormalizedCoordinates = false,
        };

        present_samplers[i] = device.createSampler(sampler_info);
    }
}

void RendererVulkan::UploadOverlayFontAtlas(bool initial_upload) {
    const vk::DeviceSize atlas_size = OverlayFont::AtlasSize();
    const u32 atlas_width = static_cast<u32>(OverlayFont::AtlasWidth());
    const u32 atlas_height = static_cast<u32>(OverlayFont::AtlasHeight());

    const vk::BufferCreateInfo staging_info = {
        .size = atlas_size,
        .usage = vk::BufferUsageFlagBits::eTransferSrc,
    };
    const VmaAllocationCreateInfo staging_alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer unsafe_staging{};
    VmaAllocation staging_allocation{};
    VmaAllocationInfo staging_mapped{};
    VkBufferCreateInfo unsafe_staging_info = static_cast<VkBufferCreateInfo>(staging_info);
    VkResult result = vmaCreateBuffer(instance.GetAllocator(), &unsafe_staging_info,
                                      &staging_alloc_info, &unsafe_staging, &staging_allocation,
                                      &staging_mapped);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating overlay font staging buffer with error {}",
                     result);
        UNREACHABLE();
    }
    std::memcpy(staging_mapped.pMappedData, OverlayFont::AtlasData(), atlas_size);
    vk::Buffer staging_buffer{unsafe_staging};

    renderpass_cache.EndRendering();
    scheduler.Record([image = overlay_font_image, staging_buffer, width = atlas_width,
                      height = atlas_height, initial_upload](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        const vk::ImageMemoryBarrier to_transfer = {
            .srcAccessMask = initial_upload ? vk::AccessFlagBits::eNone
                                            : vk::AccessFlagBits::eShaderRead,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = initial_upload ? vk::ImageLayout::eUndefined
                                        : vk::ImageLayout::eShaderReadOnlyOptimal,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier(initial_upload ? vk::PipelineStageFlagBits::eTopOfPipe
                                              : vk::PipelineStageFlagBits::eFragmentShader,
                               vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_transfer);

        const vk::BufferImageCopy copy = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, 1},
        };
        cmdbuf.copyBufferToImage(staging_buffer, image, vk::ImageLayout::eTransferDstOptimal,
                                 copy);

        const vk::ImageMemoryBarrier to_shader = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {},
                               to_shader);
    });
    scheduler.Finish();
    vmaDestroyBuffer(instance.GetAllocator(), staging_buffer, staging_allocation);
}

void RendererVulkan::CreateOverlayFont() {
    vk::Device device = instance.GetDevice();
    OverlayFont::Initialize();
    const u32 atlas_width = static_cast<u32>(OverlayFont::AtlasWidth());
    const u32 atlas_height = static_cast<u32>(OverlayFont::AtlasHeight());

    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8_UNORM,
        .extent = {atlas_width, atlas_height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImage unsafe_image{};
    VkResult result = vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info,
                                     &unsafe_image, &overlay_font_allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating overlay font atlas with error {}", result);
        UNREACHABLE();
    }
    overlay_font_image = vk::Image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = overlay_font_image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8Unorm,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    overlay_font_view = device.createImageView(view_info);

    const vk::SamplerCreateInfo sampler_info = {
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .anisotropyEnable = false,
        .compareEnable = false,
        .borderColor = vk::BorderColor::eFloatTransparentBlack,
        .unnormalizedCoordinates = false,
    };
    overlay_font_sampler = device.createSampler(sampler_info);

    UploadOverlayFontAtlas(true);

    const vk::DescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    overlay_descriptor_layout = device.createDescriptorSetLayoutUnique({
        .bindingCount = 1,
        .pBindings = &binding,
    });
    const vk::DescriptorPoolSize pool_size = {
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
    };
    overlay_descriptor_pool = device.createDescriptorPoolUnique({
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    });
    const vk::DescriptorSetLayout set_layout = *overlay_descriptor_layout;
    overlay_descriptor_set = device.allocateDescriptorSets({
        .descriptorPool = *overlay_descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    })[0];

    const vk::DescriptorImageInfo image_desc = {
        .sampler = overlay_font_sampler,
        .imageView = overlay_font_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write = {
        .dstSet = overlay_descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &image_desc,
    };
    device.updateDescriptorSets(write, {});
}

void RendererVulkan::BuildLayouts() {
    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(PresentUniformData),
    };

    const auto descriptor_set_layout = present_heap.Layout();
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };
    present_pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(layout_info);

    const vk::PipelineLayoutCreateInfo cursor_layout_info = {};
    cursor_pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(cursor_layout_info);

    const vk::PushConstantRange overlay_push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(float) * 4,
    };
    const vk::DescriptorSetLayout overlay_set_layout = *overlay_descriptor_layout;
    const vk::PipelineLayoutCreateInfo overlay_layout_info = {
        .setLayoutCount = 1,
        .pSetLayouts = &overlay_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &overlay_push_range,
    };
    overlay_pipeline_layout =
        instance.GetDevice().createPipelineLayoutUnique(overlay_layout_info);
}

void RendererVulkan::BuildPipelines() {
    const vk::VertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(ScreenRectVertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };

    const std::array attributes = {
        vk::VertexInputAttributeDescription{
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(ScreenRectVertex, position),
        },
        vk::VertexInputAttributeDescription{
            .location = 1,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(ScreenRectVertex, tex_coord),
        },
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleStrip,
        .primitiveRestartEnable = false,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state = {
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = false,
    };

    const vk::PipelineColorBlendAttachmentState colorblend_attachment = {
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eConstantAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusConstantAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eConstantAlpha,
        .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusConstantAlpha,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = false,
        .attachmentCount = 1,
        .pAttachments = &colorblend_attachment,
    };

    const vk::Viewport placeholder_viewport = vk::Viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    const vk::Rect2D placeholder_scissor = vk::Rect2D{{0, 0}, {1, 1}};
    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .viewportCount = 1,
        .pViewports = &placeholder_viewport,
        .scissorCount = 1,
        .pScissors = &placeholder_scissor,
    };

    const std::array dynamic_states = {
        vk::DynamicState::eBlendConstants,
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::PipelineDepthStencilStateCreateInfo depth_info = {
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .depthCompareOp = vk::CompareOp::eAlways,
        .depthBoundsTestEnable = false,
        .stencilTestEnable = false,
    };

    for (u32 i = 0; i < PRESENT_PIPELINES; i++) {
        const std::array shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = present_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = present_shaders[i],
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo pipeline_info = {
            .stageCount = static_cast<u32>(shader_stages.size()),
            .pStages = shader_stages.data(),
            .pVertexInputState = &vertex_input_info,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport_info,
            .pRasterizationState = &raster_state,
            .pMultisampleState = &multisampling,
            .pDepthStencilState = &depth_info,
            .pColorBlendState = &color_blending,
            .pDynamicState = &dynamic_info,
            .layout = *present_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build present pipelines");
        present_pipelines[i] = pipeline;
    }

    // Build cursor pipeline
    {
        const vk::VertexInputBindingDescription cursor_binding = {
            .binding = 0,
            .stride = sizeof(CursorVertex),
            .inputRate = vk::VertexInputRate::eVertex,
        };

        const std::array cursor_attributes = {
            vk::VertexInputAttributeDescription{
                .location = 0,
                .binding = 0,
                .format = vk::Format::eR32G32Sfloat,
                .offset = offsetof(CursorVertex, x),
            },
            vk::VertexInputAttributeDescription{
                .location = 1,
                .binding = 0,
                .format = vk::Format::eR32G32B32A32Sfloat,
                .offset = offsetof(CursorVertex, r),
            },
        };

        const vk::PipelineVertexInputStateCreateInfo cursor_vertex_input = {
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &cursor_binding,
            .vertexAttributeDescriptionCount = static_cast<u32>(cursor_attributes.size()),
            .pVertexAttributeDescriptions = cursor_attributes.data(),
        };

        const vk::PipelineInputAssemblyStateCreateInfo cursor_input_assembly = {
            .topology = vk::PrimitiveTopology::eTriangleList,
            .primitiveRestartEnable = false,
        };

        const vk::PipelineRasterizationStateCreateInfo cursor_raster = {
            .depthClampEnable = false,
            .rasterizerDiscardEnable = false,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eClockwise,
            .depthBiasEnable = false,
            .lineWidth = 1.0f,
        };

        const vk::PipelineMultisampleStateCreateInfo cursor_multisample = {
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
            .sampleShadingEnable = false,
        };

        const vk::PipelineColorBlendAttachmentState cursor_blend_attachment = {
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .alphaBlendOp = vk::BlendOp::eAdd,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };

        const vk::PipelineColorBlendStateCreateInfo cursor_color_blending = {
            .logicOpEnable = false,
            .attachmentCount = 1,
            .pAttachments = &cursor_blend_attachment,
        };

        const vk::Viewport placeholder_vp = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        const vk::Rect2D placeholder_sc = {{0, 0}, {1, 1}};
        const vk::PipelineViewportStateCreateInfo cursor_viewport = {
            .viewportCount = 1,
            .pViewports = &placeholder_vp,
            .scissorCount = 1,
            .pScissors = &placeholder_sc,
        };

        const std::array cursor_dynamic_states = {
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor,
        };

        const vk::PipelineDynamicStateCreateInfo cursor_dynamic = {
            .dynamicStateCount = static_cast<u32>(cursor_dynamic_states.size()),
            .pDynamicStates = cursor_dynamic_states.data(),
        };

        const vk::PipelineDepthStencilStateCreateInfo cursor_depth = {
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .depthCompareOp = vk::CompareOp::eAlways,
            .depthBoundsTestEnable = false,
            .stencilTestEnable = false,
        };

        const std::array cursor_shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = cursor_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = cursor_fragment_shader,
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo cursor_pipeline_info = {
            .stageCount = static_cast<u32>(cursor_shader_stages.size()),
            .pStages = cursor_shader_stages.data(),
            .pVertexInputState = &cursor_vertex_input,
            .pInputAssemblyState = &cursor_input_assembly,
            .pViewportState = &cursor_viewport,
            .pRasterizationState = &cursor_raster,
            .pMultisampleState = &cursor_multisample,
            .pDepthStencilState = &cursor_depth,
            .pColorBlendState = &cursor_color_blending,
            .pDynamicState = &cursor_dynamic,
            .layout = *cursor_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, cursor_pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build cursor pipeline");
        cursor_pipeline = pipeline;
    }

    {
        const vk::VertexInputBindingDescription overlay_binding = {
            .binding = 0,
            .stride = sizeof(float) * 4,
            .inputRate = vk::VertexInputRate::eVertex,
        };

        const std::array overlay_attributes = {
            vk::VertexInputAttributeDescription{
                .location = 0,
                .binding = 0,
                .format = vk::Format::eR32G32Sfloat,
                .offset = 0,
            },
            vk::VertexInputAttributeDescription{
                .location = 1,
                .binding = 0,
                .format = vk::Format::eR32G32Sfloat,
                .offset = sizeof(float) * 2,
            },
        };

        const vk::PipelineVertexInputStateCreateInfo overlay_vertex_input = {
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &overlay_binding,
            .vertexAttributeDescriptionCount = static_cast<u32>(overlay_attributes.size()),
            .pVertexAttributeDescriptions = overlay_attributes.data(),
        };

        const vk::PipelineInputAssemblyStateCreateInfo overlay_input_assembly = {
            .topology = vk::PrimitiveTopology::eTriangleList,
            .primitiveRestartEnable = false,
        };

        const vk::PipelineRasterizationStateCreateInfo overlay_raster = {
            .depthClampEnable = false,
            .rasterizerDiscardEnable = false,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eClockwise,
            .depthBiasEnable = false,
            .lineWidth = 1.0f,
        };

        const vk::PipelineMultisampleStateCreateInfo overlay_multisample = {
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
            .sampleShadingEnable = false,
        };

        const vk::PipelineColorBlendAttachmentState overlay_blend_attachment = {
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .alphaBlendOp = vk::BlendOp::eAdd,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };

        const vk::PipelineColorBlendStateCreateInfo overlay_color_blending = {
            .logicOpEnable = false,
            .attachmentCount = 1,
            .pAttachments = &overlay_blend_attachment,
        };

        const vk::Viewport overlay_placeholder_vp = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        const vk::Rect2D overlay_placeholder_sc = {{0, 0}, {1, 1}};
        const vk::PipelineViewportStateCreateInfo overlay_viewport = {
            .viewportCount = 1,
            .pViewports = &overlay_placeholder_vp,
            .scissorCount = 1,
            .pScissors = &overlay_placeholder_sc,
        };

        const std::array overlay_dynamic_states = {
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor,
        };

        const vk::PipelineDynamicStateCreateInfo overlay_dynamic = {
            .dynamicStateCount = static_cast<u32>(overlay_dynamic_states.size()),
            .pDynamicStates = overlay_dynamic_states.data(),
        };

        const vk::PipelineDepthStencilStateCreateInfo overlay_depth = {
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .depthCompareOp = vk::CompareOp::eAlways,
            .depthBoundsTestEnable = false,
            .stencilTestEnable = false,
        };

        const std::array overlay_shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = overlay_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = overlay_fragment_shader,
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo overlay_pipeline_info = {
            .stageCount = static_cast<u32>(overlay_shader_stages.size()),
            .pStages = overlay_shader_stages.data(),
            .pVertexInputState = &overlay_vertex_input,
            .pInputAssemblyState = &overlay_input_assembly,
            .pViewportState = &overlay_viewport,
            .pRasterizationState = &overlay_raster,
            .pMultisampleState = &overlay_multisample,
            .pDepthStencilState = &overlay_depth,
            .pColorBlendState = &overlay_color_blending,
            .pDynamicState = &overlay_dynamic,
            .layout = *overlay_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, overlay_pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build overlay pipeline");
        overlay_pipeline = pipeline;
    }
}

void RendererVulkan::ConfigureFramebufferTexture(TextureInfo& texture,
                                                 const Pica::FramebufferConfig& framebuffer) {
    vk::Device device = instance.GetDevice();
    if (texture.image_view) {
        device.destroyImageView(texture.image_view);
    }
    if (texture.image) {
        vmaDestroyImage(instance.GetAllocator(), texture.image, texture.allocation);
    }

    const VideoCore::PixelFormat pixel_format =
        VideoCore::PixelFormatFromGPUPixelFormat(framebuffer.color_format);
    const vk::Format format = instance.GetTraits(pixel_format).native;
    const VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = static_cast<VkFormat>(format),
        .extent = {framebuffer.width, framebuffer.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};

    VkResult result = vmaCreateImage(instance.GetAllocator(), &image_info, &alloc_info,
                                     &unsafe_image, &texture.allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }
    texture.image = vk::Image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = texture.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    texture.image_view = device.createImageView(view_info);

    texture.width = framebuffer.width;
    texture.height = framebuffer.height;
    texture.format = framebuffer.color_format;

    renderpass_cache.EndRendering();
    scheduler.Record([image = texture.image](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };

        const vk::ImageMemoryBarrier barrier = {
            .srcAccessMask = {},
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                               vk::PipelineStageFlagBits::eFragmentShader |
                                   vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, barrier);
    });
}

void RendererVulkan::FillScreen(Common::Vec3<u8> color, const TextureInfo& texture) {
    // When loading some 3GX extensions, FillScreen may be called before texture image is available
    if (!texture.image) {
        return;
    }

    const vk::ClearColorValue clear_color = {
        .float32 =
            std::array{
                color.r() / 255.0f,
                color.g() / 255.0f,
                color.b() / 255.0f,
                1.0f,
            },
    };

    renderpass_cache.EndRendering();
    scheduler.Record([image = texture.image, clear_color](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };

        const vk::ImageMemoryBarrier pre_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };

        const vk::ImageMemoryBarrier post_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

        cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, clear_color, range);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eFragmentShader,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);
    });
}

void RendererVulkan::ReloadPipeline(Settings::StereoRenderOption render_3d) {
    switch (render_3d) {
    case Settings::StereoRenderOption::Anaglyph:
        current_pipeline = 1;
        break;
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced:
        current_pipeline = 2;
        draw_info.reverse_interlaced = render_3d == Settings::StereoRenderOption::ReverseInterlaced;
        break;
    default:
        current_pipeline = 0;
        break;
    }
}

void RendererVulkan::DrawSingleScreen(u32 screen_id, float x, float y, float w, float h,
                                      Layout::DisplayOrientation orientation) {
    const ScreenInfo& screen_info = screen_infos[screen_id];
    const auto& texcoords = screen_info.texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u64 size = sizeof(ScreenRectVertex) * vertices.size();
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    const u32 scale_factor = GetResolutionScaleFactor();
    draw_info.i_resolution =
        Common::MakeVec(static_cast<f32>(screen_info.texture.width * scale_factor),
                        static_cast<f32>(screen_info.texture.height * scale_factor),
                        1.0f / static_cast<f32>(screen_info.texture.width * scale_factor),
                        1.0f / static_cast<f32>(screen_info.texture.height * scale_factor));
    draw_info.o_resolution = Common::MakeVec(h, w, 1.0f / h, 1.0f / w);
    draw_info.screen_id_l = screen_id;

    scheduler.Record([this, offset = offset, info = draw_info](vk::CommandBuffer cmdbuf) {
        const u32 first_vertex = static_cast<u32>(offset) / sizeof(ScreenRectVertex);
        cmdbuf.pushConstants(*present_pipeline_layout,
                             vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex,
                             0, sizeof(info), &info);

        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        cmdbuf.draw(4, 1, first_vertex, 0);
    });
}

void RendererVulkan::DrawSingleScreenStereo(u32 screen_id_l, u32 screen_id_r, float x, float y,
                                            float w, float h,
                                            Layout::DisplayOrientation orientation) {
    const ScreenInfo& screen_info_l = screen_infos[screen_id_l];
    const auto& texcoords = screen_info_l.texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u64 size = sizeof(ScreenRectVertex) * vertices.size();
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    const u32 scale_factor = GetResolutionScaleFactor();
    draw_info.i_resolution =
        Common::MakeVec(static_cast<f32>(screen_info_l.texture.width * scale_factor),
                        static_cast<f32>(screen_info_l.texture.height * scale_factor),
                        1.0f / static_cast<f32>(screen_info_l.texture.width * scale_factor),
                        1.0f / static_cast<f32>(screen_info_l.texture.height * scale_factor));
    draw_info.o_resolution = Common::MakeVec(h, w, 1.0f / h, 1.0f / w);
    draw_info.screen_id_l = screen_id_l;
    draw_info.screen_id_r = screen_id_r;

    scheduler.Record([this, offset = offset, info = draw_info](vk::CommandBuffer cmdbuf) {
        const u32 first_vertex = static_cast<u32>(offset) / sizeof(ScreenRectVertex);
        cmdbuf.pushConstants(*present_pipeline_layout,
                             vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex,
                             0, sizeof(info), &info);

        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        cmdbuf.draw(4, 1, first_vertex, 0);
    });
}

void RendererVulkan::ApplySecondLayerOpacity(float alpha) {
    scheduler.Record([alpha](vk::CommandBuffer cmdbuf) {
        const std::array<float, 4> blend_constants = {0.0f, 0.0f, 0.0f, alpha};
        cmdbuf.setBlendConstants(blend_constants.data());
    });
}

void RendererVulkan::DrawTopScreen(const Layout::FramebufferLayout& layout,
                                   const Common::Rectangle<u32>& top_screen) {
    if (!layout.top_screen_enabled) {
        return;
    }
    int leftside, rightside;
    leftside = Settings::values.swap_eyes_3d.GetValue() ? 1 : 0;
    rightside = Settings::values.swap_eyes_3d.GetValue() ? 0 : 1;
    const float top_screen_left = static_cast<float>(top_screen.left);
    const float top_screen_top = static_cast<float>(top_screen.top);
    const float top_screen_width = static_cast<float>(top_screen.GetWidth());
    const float top_screen_height = static_cast<float>(top_screen.GetHeight());

    const auto orientation = layout.is_rotated
                                 ? (layout.is_flipped ? Layout::DisplayOrientation::LandscapeFlipped
                                                      : Layout::DisplayOrientation::Landscape)
                                 : (layout.is_flipped ? Layout::DisplayOrientation::PortraitFlipped
                                                      : Layout::DisplayOrientation::Portrait);
    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        const int eye = static_cast<int>(Settings::values.mono_render_option.GetValue());
        DrawSingleScreen(eye, top_screen_left, top_screen_top, top_screen_width, top_screen_height,
                         orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySide: {
        DrawSingleScreen(leftside, top_screen_left / 2, top_screen_top, top_screen_width / 2,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(rightside, static_cast<float>((top_screen_left / 2) + (layout.width / 2)),
                         top_screen_top, top_screen_width / 2, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(leftside, top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(rightside, top_screen_left + layout.width / 2, top_screen_top,
                         top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(leftside, top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(
            rightside,
            static_cast<float>(layout.cardboard.top_screen_right_eye + (layout.width / 2)),
            top_screen_top, top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(leftside, rightside, top_screen_left, top_screen_top,
                               top_screen_width, top_screen_height, orientation);
        break;
    }
    }
}

void RendererVulkan::DrawBottomScreen(const Layout::FramebufferLayout& layout,
                                      const Common::Rectangle<u32>& bottom_screen) {
    if (!layout.bottom_screen_enabled) {
        return;
    }

    const float bottom_screen_left = static_cast<float>(bottom_screen.left);
    const float bottom_screen_top = static_cast<float>(bottom_screen.top);
    const float bottom_screen_width = static_cast<float>(bottom_screen.GetWidth());
    const float bottom_screen_height = static_cast<float>(bottom_screen.GetHeight());

    const auto orientation = layout.is_rotated
                                 ? (layout.is_flipped ? Layout::DisplayOrientation::LandscapeFlipped
                                                      : Layout::DisplayOrientation::Landscape)
                                 : (layout.is_flipped ? Layout::DisplayOrientation::PortraitFlipped
                                                      : Layout::DisplayOrientation::Portrait);

    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);

        break;
    }
    case Settings::StereoRenderOption::SideBySide: // Bottom screen is identical on both sides
    {
        DrawSingleScreen(2, bottom_screen_left / 2, bottom_screen_top, bottom_screen_width / 2,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(2, static_cast<float>((bottom_screen_left / 2) + (layout.width / 2)),
                         bottom_screen_top, bottom_screen_width / 2, bottom_screen_height,
                         orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(2, bottom_screen_left + layout.width / 2, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(
            2, static_cast<float>(layout.cardboard.bottom_screen_right_eye + (layout.width / 2)),
            bottom_screen_top, bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(2, 2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                               bottom_screen_height, orientation);
        break;
    }
    }
}

void RendererVulkan::DrawScreens(Frame* frame, const Layout::FramebufferLayout& layout,
                                 bool flipped) {
    if (settings.bg_color_update_requested.exchange(false)) {
        clear_color.float32[0] = Settings::values.bg_red.GetValue();
        clear_color.float32[1] = Settings::values.bg_green.GetValue();
        clear_color.float32[2] = Settings::values.bg_blue.GetValue();
    }
    if (settings.shader_update_requested.exchange(false)) {
        ReloadPipeline(layout.render_3d_mode);
    }

    renderpass_cache.EndRendering();
    OverlayDraw fps_overlay = PrepareFpsOverlay(layout);
    OverlayDraw shader_notice = PrepareShaderNotice(layout);
    OverlayDraw quick_menu = PrepareQuickMenu(layout);

    PrepareDraw(frame, layout);

    const auto& top_screen = layout.top_screen;
    const auto& bottom_screen = layout.bottom_screen;
    draw_info.modelview = MakeOrthographicMatrix(layout.width, layout.height);

    draw_info.layer = 0;

    // Apply the initial default opacity value; Needed to avoid flickering
    ApplySecondLayerOpacity(1.0f);

    if (!Settings::values.swap_screen.GetValue()) {
        DrawTopScreen(layout, top_screen);
        draw_info.layer = 0;
        if (layout.bottom_opacity < 1) {
            ApplySecondLayerOpacity(layout.bottom_opacity);
        }
        DrawBottomScreen(layout, bottom_screen);
    } else {
        DrawBottomScreen(layout, bottom_screen);
        draw_info.layer = 0;
        if (layout.top_opacity < 1) {
            ApplySecondLayerOpacity(layout.top_opacity);
        }
        DrawTopScreen(layout, top_screen);
    }

    if (layout.additional_screen_enabled) {
        const auto& additional_screen = layout.additional_screen;
        if (!layout.additional_screen_is_bottom) {
            DrawTopScreen(layout, additional_screen);
        } else {
            DrawBottomScreen(layout, additional_screen);
        }
    }

    DrawCursor(layout);
    RecordOverlay(std::move(fps_overlay));
    RecordOverlay(std::move(shader_notice));
    RecordOverlay(std::move(quick_menu));

    scheduler.Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRenderPass(); });
}

void RendererVulkan::DrawCursor(const Layout::FramebufferLayout& layout) {
    const auto cursor = render_window.GetCursorInfo();
    if (!cursor.visible) {
        return;
    }

    static constexpr std::array<CursorSpan, 23> black_spans = {{
        {5, 0, 2, 1},  {4, 1, 4, 1},  {4, 2, 4, 1},   {4, 3, 4, 1},
        {4, 4, 4, 1},  {4, 5, 6, 1},  {4, 6, 9, 1},   {4, 7, 11, 1},
        {4, 8, 12, 1}, {0, 9, 3, 1},  {4, 9, 13, 1},  {0, 10, 17, 1},
        {0, 11, 17, 1}, {1, 12, 16, 1}, {2, 13, 15, 1}, {2, 14, 15, 1},
        {3, 15, 14, 1}, {3, 16, 13, 1}, {4, 17, 12, 1}, {4, 18, 12, 1},
        {5, 19, 10, 1}, {5, 20, 10, 1}, {5, 21, 10, 1},
    }};
    static constexpr std::array<CursorSpan, 32> white_spans = {{
        {5, 1, 2, 1},  {5, 2, 2, 1},  {5, 3, 2, 1},   {5, 4, 2, 1},
        {5, 5, 2, 1},  {5, 6, 2, 1},  {8, 6, 2, 1},   {5, 7, 2, 1},
        {8, 7, 2, 1},  {11, 7, 2, 1}, {5, 8, 2, 1},   {8, 8, 2, 1},
        {11, 8, 2, 1}, {14, 8, 1, 1}, {5, 9, 2, 1},   {8, 9, 2, 1},
        {11, 9, 2, 1}, {14, 9, 2, 1}, {1, 10, 2, 1},  {5, 10, 8, 1},
        {14, 10, 2, 1}, {1, 11, 3, 1}, {5, 11, 11, 1}, {2, 12, 14, 1},
        {3, 13, 13, 1}, {3, 14, 13, 1}, {4, 15, 12, 1}, {4, 16, 11, 1},
        {5, 17, 10, 1}, {5, 18, 10, 1}, {6, 19, 8, 1}, {6, 20, 8, 1},
    }};

    const float buf_w = static_cast<float>(layout.width);
    const float buf_h = static_cast<float>(layout.height);

    // Convert from bottom-screen-local to layout-absolute. The hand cursor is based on
    // GBAStation's controller cursor is aligned to the touch coordinate.
    const float abs_x = layout.bottom_screen.left + cursor.projected_x;
    const float abs_y = layout.bottom_screen.top + cursor.projected_y;

    constexpr float tip_x = 6.0f;
    constexpr float tip_y = 0.0f;
    const float pixel_size = static_cast<float>(layout.bottom_screen.GetHeight()) / 180.0f;
    const float screen_left = static_cast<float>(layout.bottom_screen.left);
    const float screen_top = static_cast<float>(layout.bottom_screen.top);
    const float screen_right = static_cast<float>(layout.bottom_screen.right);
    const float screen_bottom = static_cast<float>(layout.bottom_screen.bottom);

    std::vector<CursorVertex> vertices;
    vertices.reserve((black_spans.size() + white_spans.size()) * 6);

    const auto to_ndc_x = [buf_w](float x) { return (x / buf_w) * 2.0f - 1.0f; };
    const auto to_ndc_y = [buf_h](float y) { return (y / buf_h) * 2.0f - 1.0f; };
    const auto append_span = [&](const CursorSpan& span, float r, float g, float b, float a) {
        float left = abs_x + (span.x - tip_x) * pixel_size;
        float top = abs_y + (span.y - tip_y) * pixel_size;
        float right = left + span.width * pixel_size;
        float bottom = top + span.height * pixel_size;

        left = std::fmax(left, screen_left);
        top = std::fmax(top, screen_top);
        right = std::fmin(right, screen_right);
        bottom = std::fmin(bottom, screen_bottom);
        if (left >= right || top >= bottom) {
            return;
        }

        const float l = to_ndc_x(left);
        const float t = to_ndc_y(top);
        const float rr = to_ndc_x(right);
        const float bb = to_ndc_y(bottom);

        vertices.push_back({l, t, r, g, b, a});
        vertices.push_back({rr, t, r, g, b, a});
        vertices.push_back({rr, bb, r, g, b, a});
        vertices.push_back({l, t, r, g, b, a});
        vertices.push_back({rr, bb, r, g, b, a});
        vertices.push_back({l, bb, r, g, b, a});
    };

    for (const auto& span : black_spans) {
        append_span(span, 0.0f, 0.0f, 0.0f, 0.95f);
    }
    for (const auto& span : white_spans) {
        append_span(span, 1.0f, 1.0f, 1.0f, 0.95f);
    }
    if (vertices.empty()) {
        return;
    }

    const u64 size = vertices.size() * sizeof(CursorVertex);
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    scheduler.Record([this, offset = offset, pipeline = cursor_pipeline,
                      vertex_count = static_cast<u32>(vertices.size())](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {offset});
        cmdbuf.draw(vertex_count, 1, 0, 0);
    });
}

namespace {
bool DecodeNextUtf8(std::string_view text, std::size_t& offset, u32& codepoint) {
    if (offset >= text.size()) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const unsigned char c = bytes[offset];
    if (c < 0x80) {
        codepoint = c;
        ++offset;
        return true;
    }
    if ((c & 0xE0) == 0xC0 && offset + 1 < text.size()) {
        codepoint = ((c & 0x1F) << 6) | (bytes[offset + 1] & 0x3F);
        offset += 2;
        return true;
    }
    if ((c & 0xF0) == 0xE0 && offset + 2 < text.size()) {
        codepoint = ((c & 0x0F) << 12) | ((bytes[offset + 1] & 0x3F) << 6) |
                    (bytes[offset + 2] & 0x3F);
        offset += 3;
        return true;
    }
    if ((c & 0xF8) == 0xF0 && offset + 3 < text.size()) {
        codepoint = ((c & 0x07) << 18) | ((bytes[offset + 1] & 0x3F) << 12) |
                    ((bytes[offset + 2] & 0x3F) << 6) | (bytes[offset + 3] & 0x3F);
        offset += 4;
        return true;
    }
    codepoint = '?';
    ++offset;
    return true;
}

class OverlayBuilder {
public:
    OverlayBuilder(std::vector<float>& verts, float width, float height)
        : verts{verts}, inv_w{2.0f / width}, inv_h{2.0f / height} {}

    u32 VertexCount() const {
        return static_cast<u32>(verts.size() / kFloatsPerVertex);
    }

    void AddRect(float x0, float y0, float x1, float y1) {
        PushQuad(x0, y0, x1, y1, OverlayFont::WhiteU(), OverlayFont::WhiteV(),
                 OverlayFont::WhiteU(), OverlayFont::WhiteV());
    }

    static float Measure(std::string_view text, float scale) {
        float width = 0.0f;
        std::size_t offset = 0;
        u32 codepoint = 0;
        while (DecodeNextUtf8(text, offset, codepoint)) {
            width += OverlayFont::GlyphFor(codepoint).xadvance * scale;
        }
        return width;
    }

    void AddText(float ox, float oy, std::string_view text, float scale) {
        float pen = ox;
        std::size_t offset = 0;
        u32 codepoint = 0;
        while (DecodeNextUtf8(text, offset, codepoint)) {
            const OverlayFont::Glyph& g = OverlayFont::GlyphFor(codepoint);
            if (g.w > 0.0f && g.h > 0.0f) {
                const float qx = pen + g.xoff * scale;
                const float qy = oy + g.yoff * scale;
                PushQuad(qx, qy, qx + g.w * scale, qy + g.h * scale, g.u0, g.v0, g.u1, g.v1);
            }
            pen += g.xadvance * scale;
        }
    }

private:
    static constexpr int kFloatsPerVertex = 4;

    void PushQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
        const float l = x0 * inv_w - 1.0f;
        const float r = x1 * inv_w - 1.0f;
        const float t = y0 * inv_h - 1.0f;
        const float b = y1 * inv_h - 1.0f;
        verts.insert(verts.end(), {
                                      l, t, u0, v0, r, t, u1, v0, r, b, u1, v1,
                                      l, t, u0, v0, r, b, u1, v1, l, b, u0, v1,
                                  });
    }

    std::vector<float>& verts;
    float inv_w;
    float inv_h;
};

void AppendUtf8(std::string& out, u32 cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::vector<u32> DecodeUtf8Codepoints(std::string_view text) {
    std::vector<u32> codepoints;
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(text.data());
    const unsigned char* end = ptr + text.size();
    while (ptr < end) {
        u32 cp = 0;
        const unsigned char c = *ptr;
        if (c < 0x80) {
            cp = c;
            ++ptr;
        } else if ((c & 0xE0) == 0xC0 && ptr + 1 < end) {
            cp = ((c & 0x1F) << 6) | (ptr[1] & 0x3F);
            ptr += 2;
        } else if ((c & 0xF0) == 0xE0 && ptr + 2 < end) {
            cp = ((c & 0x0F) << 12) | ((ptr[1] & 0x3F) << 6) | (ptr[2] & 0x3F);
            ptr += 3;
        } else if ((c & 0xF8) == 0xF0 && ptr + 3 < end) {
            cp = ((c & 0x07) << 18) | ((ptr[1] & 0x3F) << 12) |
                 ((ptr[2] & 0x3F) << 6) | (ptr[3] & 0x3F);
            ptr += 4;
        } else {
            break;
        }
        codepoints.push_back(cp);
    }
    return codepoints;
}

std::string EncodeUtf8(const std::vector<u32>& codepoints, std::size_t start, std::size_t count) {
    std::string out;
    const std::size_t end = std::min(codepoints.size(), start + count);
    for (std::size_t i = start; i < end; ++i) {
        AppendUtf8(out, codepoints[i]);
    }
    return out;
}

float MeasureUtf8Range(const std::vector<u32>& codepoints, std::size_t start, std::size_t count,
                       float scale) {
    float width = 0.0f;
    const std::size_t end = std::min(codepoints.size(), start + count);
    for (std::size_t i = start; i < end; ++i) {
        width += OverlayFont::GlyphFor(codepoints[i]).xadvance * scale;
    }
    return width;
}

std::string EllipsizeUtf8(std::string_view text, float max_width, float scale) {
    if (OverlayBuilder::Measure(text, scale) <= max_width) {
        return std::string(text);
    }

    const auto codepoints = DecodeUtf8Codepoints(text);
    if (codepoints.empty()) {
        return {};
    }

    constexpr char kEllipsis[] = "...";
    if (OverlayBuilder::Measure(kEllipsis, scale) > max_width) {
        return {};
    }

    std::size_t low = 0;
    std::size_t high = codepoints.size();
    while (low < high) {
        const std::size_t mid = (low + high + 1) / 2;
        std::string candidate = EncodeUtf8(codepoints, 0, mid);
        if (mid < codepoints.size()) {
            candidate += kEllipsis;
        }
        if (OverlayBuilder::Measure(candidate, scale) <= max_width) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }

    std::string result = EncodeUtf8(codepoints, 0, low);
    if (low < codepoints.size()) {
        result += kEllipsis;
    }
    return result;
}

std::string MarqueeUtf8(std::string_view text, float max_width, float scale) {
    if (OverlayBuilder::Measure(text, scale) <= max_width) {
        return std::string(text);
    }

    const auto codepoints = DecodeUtf8Codepoints(text);
    if (codepoints.empty()) {
        return {};
    }

    constexpr char kEllipsis[] = "...";
    const float ellipsis_w = OverlayBuilder::Measure(kEllipsis, scale);
    if (ellipsis_w >= max_width) {
        return EllipsizeUtf8(text, max_width, scale);
    }

    const float available = std::max(0.0f, max_width - ellipsis_w * 2.0f);
    if (available <= 0.0f) {
        return EllipsizeUtf8(text, max_width, scale);
    }

    std::size_t best_window = 1;
    for (std::size_t count = 1; count <= codepoints.size(); ++count) {
        if (MeasureUtf8Range(codepoints, 0, count, scale) <= available) {
            best_window = count;
        } else {
            break;
        }
    }

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto tick = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    const std::size_t cycle = std::max<std::size_t>(1, codepoints.size());
    const std::size_t start = static_cast<std::size_t>((tick / 140) % cycle);
    std::size_t slice_start = std::min(start, codepoints.size() - 1);
    std::size_t window = std::min(best_window, codepoints.size() - slice_start);

    while (window > 0) {
        std::string candidate;
        if (slice_start > 0) {
            candidate += kEllipsis;
        }
        candidate += EncodeUtf8(codepoints, slice_start, window);
        if (slice_start + window < codepoints.size()) {
            candidate += kEllipsis;
        }
        if (OverlayBuilder::Measure(candidate, scale) <= max_width) {
            return candidate;
        }
        --window;
    }

    return EllipsizeUtf8(text, max_width, scale);
}

std::string IconUtf8(u32 codepoint) {
    std::string out;
    AppendUtf8(out, codepoint);
    return out;
}

void AppendTextCodepoints(std::vector<u32>& out, std::string_view text) {
    std::size_t offset = 0;
    u32 codepoint = 0;
    while (DecodeNextUtf8(text, offset, codepoint)) {
        out.push_back(codepoint);
    }
}

std::vector<u32> CollectMenuTextCodepoints(const VideoCore::OverlayMenuState& state) {
    std::vector<u32> codepoints;
    AppendTextCodepoints(codepoints, state.title);
    AppendTextCodepoints(codepoints, state.hint);
    for (const auto& tab : state.tabs) {
        AppendTextCodepoints(codepoints, tab.label);
        AppendTextCodepoints(codepoints, tab.icon);
    }
    for (const auto& item : state.items) {
        AppendTextCodepoints(codepoints, item.label);
        AppendTextCodepoints(codepoints, item.value);
    }
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
    return codepoints;
}
} // namespace

bool RendererVulkan::EnsureOverlayFontGlyphs(const VideoCore::OverlayMenuState& state) {
    const std::vector<u32> codepoints = CollectMenuTextCodepoints(state);
    if (!OverlayFont::EnsureCodepoints(codepoints)) {
        return false;
    }
    UploadOverlayFontAtlas(false);
    LOG_INFO(Render_Vulkan, "Overlay font atlas refreshed for dynamic menu text");
    return true;
}

RendererVulkan::OverlayDraw RendererVulkan::PrepareFpsOverlay(
    const Layout::FramebufferLayout& layout) {
    float fps_value = 0.0f;
    if (!VideoCore::GetFpsOverlayState(fps_value)) {
        return {};
    }

    overlay_game_fps = fps_value;

    char text[16];
    const int fps = std::clamp(static_cast<int>(std::lround(overlay_game_fps)), 0, 999);
    std::snprintf(text, sizeof(text), "FPS %d", fps);

    const float w = static_cast<float>(layout.width);
    const float h = static_cast<float>(layout.height);
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }

    const float em = std::max(14.0f, std::round(h / 32.0f));
    const float scale = em / OverlayFont::BakePixelHeight();
    const float margin = std::round(em * 0.6f);
    const float pad = std::round(em * 0.35f);

    float ink_top = OverlayFont::Ascent();
    float ink_bottom = 0.0f;
    for (const char* p = text; *p != '\0'; ++p) {
        const OverlayFont::Glyph& g = OverlayFont::GlyphFor(*p);
        if (g.h > 0.0f) {
            ink_top = std::min(ink_top, g.yoff);
            ink_bottom = std::max(ink_bottom, g.yoff + g.h);
        }
    }

    std::vector<float> verts;
    verts.reserve(256);
    OverlayBuilder builder{verts, w, h};

    const float text_w = OverlayBuilder::Measure(text, scale);

    builder.AddRect(margin - pad, margin + ink_top * scale - pad, margin + text_w + pad,
                    margin + ink_bottom * scale + pad);
    const u32 box_vertices = builder.VertexCount();

    builder.AddText(margin, margin, text, scale);
    const u32 glyph_vertices = builder.VertexCount() - box_vertices;

    const u64 size = verts.size() * sizeof(float);
    auto [data, offset, invalidate] = overlay_vertex_buffer.Map(size, 16);
    std::memcpy(data, verts.data(), size);
    overlay_vertex_buffer.Commit(size);

    constexpr std::array<float, 4> box_color = {0.0f, 0.0f, 0.0f, 0.55f};
    constexpr std::array<float, 4> text_color = {0.53f, 1.0f, 0.53f, 1.0f};

    OverlayDraw overlay;
    overlay.base_vertex = static_cast<u32>(offset) / (sizeof(float) * 4);
    overlay.batches.push_back({box_color, 0, box_vertices});
    if (glyph_vertices > 0) {
        overlay.batches.push_back({text_color, box_vertices, glyph_vertices});
    }
    return overlay;
}

RendererVulkan::OverlayDraw RendererVulkan::PrepareShaderNotice(
    const Layout::FramebufferLayout& layout) {
    if (!VideoCore::GetShaderCompileNoticeState()) {
        return {};
    }
    const std::size_t pending = rasterizer.PendingCompilationCount();
    const u64 generation = VideoCore::GetShaderCompileGeneration();
    const auto now = std::chrono::steady_clock::now();
    if (pending > 0 || generation != shader_notice_generation) {
        shader_notice_generation = generation;
        shader_notice_until = now + std::chrono::milliseconds(650);
    }
    if (now >= shader_notice_until) {
        return {};
    }

    char text[64];
    if (pending > 0) {
        std::snprintf(text, sizeof(text), "正在编译着色器 %zu", pending);
    } else {
        std::snprintf(text, sizeof(text), "正在编译着色器");
    }

    const float w = static_cast<float>(layout.width);
    const float h = static_cast<float>(layout.height);
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }

    const float em = std::max(14.0f, std::round(h / 32.0f));
    const float scale = em / OverlayFont::BakePixelHeight();
    const float margin = std::round(em * 0.6f);
    const float pad = std::round(em * 0.35f);

    float ink_top = OverlayFont::Ascent();
    float ink_bottom = 0.0f;
    for (u32 codepoint : DecodeUtf8Codepoints(text)) {
        const OverlayFont::Glyph& glyph = OverlayFont::GlyphFor(codepoint);
        if (glyph.h > 0.0f) {
            ink_top = std::min(ink_top, glyph.yoff);
            ink_bottom = std::max(ink_bottom, glyph.yoff + glyph.h);
        }
    }

    std::vector<float> verts;
    verts.reserve(256);
    OverlayBuilder builder{verts, w, h};
    const float text_w = OverlayBuilder::Measure(text, scale);
    const float origin_x = margin;
    const float origin_y = h - margin - pad - ink_bottom * scale;

    builder.AddRect(origin_x - pad, origin_y + ink_top * scale - pad, origin_x + text_w + pad,
                    origin_y + ink_bottom * scale + pad);
    const u32 box_vertices = builder.VertexCount();
    builder.AddText(origin_x, origin_y, text, scale);
    const u32 glyph_vertices = builder.VertexCount() - box_vertices;

    const u64 size = verts.size() * sizeof(float);
    auto [data, offset, invalidate] = overlay_vertex_buffer.Map(size, 16);
    std::memcpy(data, verts.data(), size);
    overlay_vertex_buffer.Commit(size);

    constexpr std::array<float, 4> box_color = {0.0f, 0.0f, 0.0f, 0.62f};
    constexpr std::array<float, 4> text_color = {1.0f, 0.82f, 0.35f, 1.0f};
    OverlayDraw overlay;
    overlay.base_vertex = static_cast<u32>(offset) / (sizeof(float) * 4);
    overlay.batches.push_back({box_color, 0, box_vertices});
    if (glyph_vertices > 0) {
        overlay.batches.push_back({text_color, box_vertices, glyph_vertices});
    }
    return overlay;
}

RendererVulkan::OverlayDraw RendererVulkan::PrepareQuickMenu(
    const Layout::FramebufferLayout& layout) {
    if (!VideoCore::IsOverlayMenuVisible()) {
        return {};
    }
    const VideoCore::OverlayMenuState state = VideoCore::GetOverlayMenuState();
    if (!state.visible) {
        return {};
    }
    EnsureOverlayFontGlyphs(state);

    const float w = static_cast<float>(layout.width);
    const float h = static_cast<float>(layout.height);
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }

    std::vector<float> verts;
    verts.reserve(4096);
    OverlayBuilder builder{verts, w, h};

    const float em = std::max(20.0f, std::round(h / 28.0f));
    const float scale = em / OverlayFont::BakePixelHeight();
    const float small_scale = std::round(em * 0.86f) / OverlayFont::BakePixelHeight();
    const float title_scale = std::round(em * 1.12f) / OverlayFont::BakePixelHeight();
    const float icon_scale = std::round(em * 1.32f) / OverlayFont::BakePixelHeight();
    const float line_h = OverlayFont::LineHeight() * scale;
    const float row_h = std::round(std::max(56.0f, line_h * 1.68f));
    const float tab_h = std::round(std::max(68.0f, line_h * 1.92f));
    const float pad = std::round(em * 1.08f);
    const float panel_w = w;
    const float panel_x0 = 0.0f;
    const float panel_y0 = 0.0f;
    const float panel_x1 = w;
    const float panel_y1 = h;
    const float rail_w = std::round(std::clamp(panel_w * 0.29f, 280.0f, 360.0f));
    const float rail_x1 = panel_x0 + rail_w;
    const float content_x0 = rail_x1 + pad;
    const float content_x1 = panel_x1 - pad;
    const float tab_x0 = panel_x0 + pad;
    const float tab_x1 = rail_x1 - pad;
    const float tabs_top = panel_y0 + std::round(pad * 3.25f);
    const float rows_top = panel_y0 + std::round(pad * 3.25f);
    const float footer_y = panel_y1 - std::round(pad * 1.18f);
    const float rows_bottom = footer_y - std::round(pad * 0.75f);
    const float header_h = std::round(std::max(40.0f, row_h * 0.72f));
    const float visible_rows_h = std::max(1.0f, rows_bottom - rows_top);
    const int item_count = static_cast<int>(state.items.size());
    const int tab_count = static_cast<int>(state.tabs.size());
    const int selected_tab = std::clamp(state.selected_tab, 0, std::max(0, tab_count - 1));
    const bool has_selection =
        item_count > 0 && state.selected >= 0 && state.selected < item_count &&
        state.items[state.selected].kind != VideoCore::OverlayMenuItemKind::Header &&
        state.items[state.selected].kind != VideoCore::OverlayMenuItemKind::Disabled;

    std::vector<float> row_tops(item_count + 1, 0.0f);
    for (int i = 0; i < item_count; ++i) {
        row_tops[i + 1] =
            row_tops[i] +
            (state.items[i].kind == VideoCore::OverlayMenuItemKind::Header ? header_h : row_h);
    }
    const float total_rows_h = item_count > 0 ? row_tops[item_count] : 0.0f;
    float scroll_y = 0.0f;
    if (has_selection && total_rows_h > visible_rows_h) {
        const float selected_center =
            (row_tops[state.selected] + row_tops[state.selected + 1]) * 0.5f;
        scroll_y = std::clamp(selected_center - visible_rows_h * 0.46f, 0.0f,
                              total_rows_h - visible_rows_h);
    }

    std::vector<OverlayDraw::Batch> batches;
    const auto emit = [&](const std::array<float, 4>& color, u32 start) {
        const u32 count = builder.VertexCount() - start;
        if (count > 0) {
            batches.push_back({color, start, count});
        }
    };

    constexpr std::array<float, 4> c_dim = {0.0f, 0.0f, 0.0f, 0.16f};
    constexpr std::array<float, 4> c_panel = {0.015f, 0.020f, 0.030f, 0.52f};
    constexpr std::array<float, 4> c_rail = {0.015f, 0.020f, 0.030f, 0.12f};
    constexpr std::array<float, 4> c_separator = {1.0f, 1.0f, 1.0f, 0.14f};
    constexpr std::array<float, 4> c_tab_focus = {0.0f, 0.30f, 0.50f, 0.52f};
    constexpr std::array<float, 4> c_tab_fill = {1.0f, 1.0f, 1.0f, 0.035f};
    constexpr std::array<float, 4> c_content_highlight = {0.0f, 0.30f, 0.50f, 0.52f};
    constexpr std::array<float, 4> c_row_fill = {1.0f, 1.0f, 1.0f, 0.045f};
    constexpr std::array<float, 4> c_row_border = {1.0f, 1.0f, 1.0f, 0.10f};
    constexpr std::array<float, 4> c_focus_border = {0.31f, 0.70f, 1.0f, 0.76f};
    constexpr std::array<float, 4> c_title = {1.0f, 1.0f, 1.0f, 1.0f};
    constexpr std::array<float, 4> c_row = {0.82f, 0.85f, 0.92f, 1.0f};
    constexpr std::array<float, 4> c_selected = {1.0f, 1.0f, 1.0f, 1.0f};
    constexpr std::array<float, 4> c_header = {0.47f, 0.84f, 1.0f, 0.95f};
    constexpr std::array<float, 4> c_disabled = {0.50f, 0.53f, 0.60f, 0.70f};
    constexpr std::array<float, 4> c_footer = {0.60f, 0.63f, 0.72f, 1.0f};
    constexpr std::array<float, 4> c_icon = {0.44f, 0.80f, 1.0f, 1.0f};
    constexpr std::array<float, 4> c_cheat_enabled = {0.38f, 0.76f, 1.0f, 1.0f};
    constexpr std::array<float, 4> c_cheat_enabled_selected = {0.58f, 0.88f, 1.0f, 1.0f};

    {
        const u32 start = builder.VertexCount();
        builder.AddRect(0.0f, 0.0f, w, h);
        emit(c_dim, start);
    }
    {
        const u32 start = builder.VertexCount();
        builder.AddRect(panel_x0, panel_y0, panel_x1, panel_y1);
        emit(c_panel, start);
    }
    {
        const u32 start = builder.VertexCount();
        builder.AddRect(panel_x0, panel_y0, rail_x1, panel_y1);
        emit(c_rail, start);
    }
    {
        const u32 start = builder.VertexCount();
        builder.AddRect(panel_x0, panel_y0, panel_x0 + 1.0f, panel_y1);
        builder.AddRect(rail_x1, panel_y0, rail_x1 + 1.0f, panel_y1);
        builder.AddRect(content_x0, footer_y - std::round(pad * 0.55f), content_x1,
                        footer_y - std::round(pad * 0.55f) + 1.0f);
        emit(c_separator, start);
    }

    if (tab_count > 0) {
        const u32 start = builder.VertexCount();
        for (int i = 0; i < tab_count; ++i) {
            const float top = tabs_top + static_cast<float>(i) * tab_h;
            builder.AddRect(tab_x0, top + 4.0f, tab_x1, top + tab_h - 4.0f);
        }
        emit(c_tab_fill, start);
    }
    if (item_count > 0) {
        const u32 start = builder.VertexCount();
        for (int i = 0; i < item_count; ++i) {
            if (state.items[i].kind == VideoCore::OverlayMenuItemKind::Header) {
                continue;
            }
            const float top = rows_top + row_tops[i] - scroll_y;
            const float item_h = row_tops[i + 1] - row_tops[i];
            if (top < rows_top || top + item_h > rows_bottom) {
                continue;
            }
            builder.AddRect(content_x0 - std::round(pad * 0.4f), top + 5.0f,
                            content_x1 + std::round(pad * 0.15f), top + item_h - 5.0f);
        }
        emit(c_row_fill, start);
    }
    {
        const auto add_border = [&](float x0, float y0, float x1, float y1) {
            builder.AddRect(x0, y0, x1, y0 + 1.0f);
            builder.AddRect(x0, y1 - 1.0f, x1, y1);
            builder.AddRect(x0, y0, x0 + 1.0f, y1);
            builder.AddRect(x1 - 1.0f, y0, x1, y1);
        };
        const u32 start = builder.VertexCount();
        for (int i = 0; i < tab_count; ++i) {
            const float top = tabs_top + static_cast<float>(i) * tab_h;
            add_border(tab_x0, top + 4.0f, tab_x1, top + tab_h - 4.0f);
        }
        for (int i = 0; i < item_count; ++i) {
            if (state.items[i].kind == VideoCore::OverlayMenuItemKind::Header) {
                continue;
            }
            const float top = rows_top + row_tops[i] - scroll_y;
            const float item_h = row_tops[i + 1] - row_tops[i];
            if (top < rows_top || top + item_h > rows_bottom) {
                continue;
            }
            add_border(content_x0 - std::round(pad * 0.4f), top + 5.0f,
                       content_x1 + std::round(pad * 0.15f), top + item_h - 5.0f);
        }
        emit(c_row_border, start);
    }

    if (tab_count > 0) {
        const u32 start = builder.VertexCount();
        const float top = tabs_top + static_cast<float>(selected_tab) * tab_h;
        builder.AddRect(tab_x0, top + 4.0f, tab_x1, top + tab_h - 4.0f);
        emit(state.tabs_focused ? c_tab_focus : c_tab_fill, start);
    }
    if (has_selection && !state.tabs_focused) {
        const u32 start = builder.VertexCount();
        const float top = rows_top + row_tops[state.selected] - scroll_y;
        const float item_h = row_tops[state.selected + 1] - row_tops[state.selected];
        builder.AddRect(content_x0 - std::round(pad * 0.4f), top + 5.0f,
                        content_x1 + std::round(pad * 0.15f), top + item_h - 5.0f);
        emit(c_content_highlight, start);
    }
    {
        const u32 start = builder.VertexCount();
        if (tab_count > 0 && state.tabs_focused) {
            const float top = tabs_top + static_cast<float>(selected_tab) * tab_h + 4.0f;
            const float bottom = top + tab_h - 8.0f;
            builder.AddRect(tab_x0, top, tab_x1, top + 1.0f);
            builder.AddRect(tab_x0, bottom - 1.0f, tab_x1, bottom);
            builder.AddRect(tab_x0, top, tab_x0 + 3.0f, bottom);
            builder.AddRect(tab_x1 - 1.0f, top, tab_x1, bottom);
        } else if (has_selection && !state.tabs_focused) {
            const float top = rows_top + row_tops[state.selected] - scroll_y + 5.0f;
            const float item_h = row_tops[state.selected + 1] - row_tops[state.selected] - 10.0f;
            const float x0 = content_x0 - std::round(pad * 0.4f);
            const float x1 = content_x1 + std::round(pad * 0.15f);
            builder.AddRect(x0, top, x1, top + 1.0f);
            builder.AddRect(x0, top + item_h - 1.0f, x1, top + item_h);
            builder.AddRect(x0, top, x0 + 3.0f, top + item_h);
            builder.AddRect(x1 - 1.0f, top, x1, top + item_h);
        }
        emit(c_focus_border, start);
    }

    {
        const u32 start = builder.VertexCount();
        builder.AddText(panel_x0 + pad, panel_y0 + std::round(pad * 1.05f), state.title,
                        title_scale);
        emit(c_title, start);
    }

    const auto add_tab = [&](int index) {
        const auto& tab = state.tabs[index];
        const float top = tabs_top + static_cast<float>(index) * tab_h;
        const float center_y = top + tab_h * 0.5f;
        const float icon_line_h = OverlayFont::LineHeight() * icon_scale;
        const float icon_y = std::round(center_y - icon_line_h * 0.5f);
        const float label_y = std::round(center_y - line_h * 0.5f + em * 0.055f);
        if (!tab.icon.empty()) {
            builder.AddText(tab_x0 + std::round(pad * 0.75f), icon_y, tab.icon, icon_scale);
        }
        builder.AddText(tab_x0 + std::round(pad * 2.45f), label_y, tab.label, scale);
    };

    {
        const u32 start = builder.VertexCount();
        for (int i = 0; i < tab_count; ++i) {
            if (i != selected_tab) {
                add_tab(i);
            }
        }
        emit(c_row, start);
    }
    if (tab_count > 0) {
        const u32 start = builder.VertexCount();
        add_tab(selected_tab);
        emit(c_selected, start);
    }
    if (tab_count > 0) {
        const u32 start = builder.VertexCount();
        const auto& tab = state.tabs[selected_tab];
        if (!tab.icon.empty()) {
            const float top = tabs_top + static_cast<float>(selected_tab) * tab_h;
            const float center_y = top + tab_h * 0.5f;
            const float icon_line_h = OverlayFont::LineHeight() * icon_scale;
            builder.AddText(tab_x0 + std::round(pad * 0.75f),
                            std::round(center_y - icon_line_h * 0.5f), tab.icon, icon_scale);
        }
        emit(c_icon, start);
    }

    std::string section_title = selected_tab >= 0 && selected_tab < tab_count
                                    ? state.tabs[selected_tab].label
                                    : state.title;
    {
        const u32 start = builder.VertexCount();
        builder.AddText(content_x0, panel_y0 + std::round(pad * 1.05f), section_title,
                        title_scale);
        emit(c_title, start);
    }

    const bool cheats_page = selected_tab == 3;
    const bool content_focused = !state.tabs_focused;
    const auto is_enabled_cheat_row = [&](int index) {
        return cheats_page && index >= 0 && index < item_count &&
               state.items[index].kind == VideoCore::OverlayMenuItemKind::Row &&
               state.items[index].value == "开启";
    };

    const auto add_row = [&](int index) {
        const auto& item = state.items[index];
        const float top = rows_top + row_tops[index] - scroll_y;
        const float item_h = row_tops[index + 1] - row_tops[index];
        if (top < rows_top || top + item_h > rows_bottom) {
            return;
        }
        const bool header = item.kind == VideoCore::OverlayMenuItemKind::Header;
        const float row_scale = header ? small_scale : scale;
        const float row_line_h = OverlayFont::LineHeight() * row_scale;
        const float text_y = std::round(top + (item_h - row_line_h) * (header ? 0.56f : 0.47f));
        const bool focused_row = content_focused && has_selection && index == state.selected;
        const float label_max_width =
            std::max(72.0f, content_x1 - content_x0 - std::round(em * 4.8f));
        const std::string label_text = cheats_page && item.kind == VideoCore::OverlayMenuItemKind::Row
                                           ? (focused_row ? MarqueeUtf8(item.label, label_max_width, row_scale)
                                                         : EllipsizeUtf8(item.label, label_max_width, row_scale))
                                           : item.label;
        builder.AddText(content_x0, text_y, label_text, row_scale);
        if (!item.value.empty()) {
            const std::string value = item.uses_lr
                                          ? IconUtf8(0xE0E4) + "  " + item.value + "  " +
                                                IconUtf8(0xE0E5)
                                          : item.value;
            const float value_x = content_x1 - OverlayBuilder::Measure(value, row_scale);
            builder.AddText(value_x, text_y, value, row_scale);
        }
    };

    {
        const u32 start = builder.VertexCount();
        for (int i = 0; i < item_count; ++i) {
            if (i != state.selected && state.items[i].kind == VideoCore::OverlayMenuItemKind::Row &&
                !is_enabled_cheat_row(i)) {
                add_row(i);
            }
        }
        emit(c_row, start);
    }
    {
        const u32 start = builder.VertexCount();
        for (int i = 0; i < item_count; ++i) {
            if (i != state.selected && is_enabled_cheat_row(i)) {
                add_row(i);
            }
        }
        emit(c_cheat_enabled, start);
    }
    {
        const u32 start = builder.VertexCount();
        for (int i = 0; i < item_count; ++i) {
            if (i != state.selected &&
                state.items[i].kind == VideoCore::OverlayMenuItemKind::Header) {
                add_row(i);
            }
        }
        emit(c_header, start);
    }
    {
        const u32 start = builder.VertexCount();
        for (int i = 0; i < item_count; ++i) {
            if (i != state.selected &&
                state.items[i].kind == VideoCore::OverlayMenuItemKind::Disabled) {
                add_row(i);
            }
        }
        emit(c_disabled, start);
    }
    if (has_selection) {
        const u32 start = builder.VertexCount();
        add_row(state.selected);
        emit(is_enabled_cheat_row(state.selected) ? c_cheat_enabled_selected : c_selected, start);
    }

    struct FooterPrompt {
        std::string icon;
        std::string text;
    };
    std::vector<FooterPrompt> prompts;
    const auto push_prompt = [&](u32 icon, std::string text) {
        prompts.push_back({IconUtf8(icon), std::move(text)});
    };
    push_prompt(0xE0E0, "确认");
    push_prompt(0xE0E1, "取消");

    if (!prompts.empty()) {
        const float gap = std::round(em * 0.42f);
        const float icon_scale_footer = std::round(em * 0.84f) / OverlayFont::BakePixelHeight();
        const float text_scale_footer = std::round(em * 0.74f) / OverlayFont::BakePixelHeight();
        float cursor_x = content_x1;
        for (auto it = prompts.rbegin(); it != prompts.rend(); ++it) {
            const float icon_w = OverlayBuilder::Measure(it->icon, icon_scale_footer);
            const float text_w = OverlayBuilder::Measure(it->text, text_scale_footer);
            const float group_w = icon_w + std::round(em * 0.22f) + text_w + gap;
            const float x = cursor_x - group_w;
            const u32 start = builder.VertexCount();
            builder.AddText(x, footer_y, it->icon, icon_scale_footer);
            builder.AddText(x + icon_w + std::round(em * 0.22f), footer_y + std::round(em * 0.08f),
                            it->text, text_scale_footer);
            emit(c_footer, start);
            cursor_x = x - gap;
        }
    }

    if (batches.empty()) {
        return {};
    }

    const u64 size = verts.size() * sizeof(float);
    auto [data, offset, invalidate] = overlay_vertex_buffer.Map(size, 16);
    std::memcpy(data, verts.data(), size);
    overlay_vertex_buffer.Commit(size);

    OverlayDraw overlay;
    overlay.base_vertex = static_cast<u32>(offset) / (sizeof(float) * 4);
    overlay.batches = std::move(batches);
    return overlay;
}

void RendererVulkan::RecordOverlay(OverlayDraw overlay) {
    if (overlay.batches.empty()) {
        return;
    }
    scheduler.Record([this, base_vertex = overlay.base_vertex,
                      batches = std::move(overlay.batches)](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, overlay_pipeline);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *overlay_pipeline_layout, 0,
                                  overlay_descriptor_set, {});
        cmdbuf.bindVertexBuffers(0, overlay_vertex_buffer.Handle(), {0});
        for (const auto& batch : batches) {
            cmdbuf.pushConstants(*overlay_pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0,
                                 static_cast<u32>(batch.color.size() * sizeof(float)),
                                 batch.color.data());
            cmdbuf.draw(batch.count, 1, base_vertex + batch.first, 0);
        }
    });
}

void RendererVulkan::SwapBuffers() {
    system.perf_stats->StartSwap();
    if (fast_forward_enabled && fast_forward_present_divisor > 1 &&
        (++fast_forward_present_counter % fast_forward_present_divisor) != 0) {
        // Presentation can be skipped, but rasterizer commands still need to be submitted.
        // Otherwise several fast-forward frames accumulate in one command buffer and complex
        // scene transitions can exhaust host memory or overwhelm the driver when finally flushed.
        scheduler.Flush();
        system.perf_stats->EndSwap();
        rasterizer.TickFrame();
        EndFrame();
        return;
    }
    screenRendered = false;
#ifndef ANDROID
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        ASSERT(secondary_window);
        secondaryWindowEnabled = true;
    } else {
        secondaryWindowEnabled = false;
    }
#endif

#ifdef ANDROID
    if (secondary_window) {
        secondaryWindowEnabled = true;
    } else {
        secondaryWindowEnabled = false;
    }
#endif

    const Layout::FramebufferLayout& layout = render_window.GetFramebufferLayout();
    PrepareRendertarget();
    RenderScreenshot();
    isSecondaryWindow = false;
    RenderToWindow(main_present_window, layout, false);
#ifndef ANDROID
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        ASSERT(secondary_window);
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        if (!secondary_present_window_ptr) {
            secondary_present_window_ptr = std::make_unique<PresentWindow>(
                *secondary_window, instance, scheduler, IsLowRefreshRate());
        }
        isSecondaryWindow = true;
        RenderToWindow(*secondary_present_window_ptr, secondary_layout, false);
        secondary_window->PollEvents();
    }
#endif

#ifdef ANDROID
    if (secondary_window) {
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        if (!secondary_present_window_ptr) {
            secondary_present_window_ptr = std::make_unique<PresentWindow>(
                *secondary_window, instance, scheduler, IsLowRefreshRate());
        }
        isSecondaryWindow = true;
        RenderToWindow(*secondary_present_window_ptr, secondary_layout, false);
        secondary_window->PollEvents();
    }
#endif
    if (!screenRendered) {
        scheduler.Finish();
    }

    system.perf_stats->EndSwap();
    rasterizer.TickFrame();
    EndFrame();
}

void RendererVulkan::RenderScreenshot() {
    if (!settings.screenshot_requested.exchange(false)) {
        return;
    }

    if (!TryRenderScreenshotWithHostMemory()) {
        RenderScreenshotWithStagingCopy();
    }

    settings.screenshot_complete_callback(false);
}

void RendererVulkan::RenderScreenshotWithStagingCopy() {
    const vk::Device device = instance.GetDevice();

    const Layout::FramebufferLayout layout{settings.screenshot_framebuffer_layout};
    const u32 width = layout.width;
    const u32 height = layout.height;

    const vk::BufferCreateInfo staging_buffer_info = {
        .size = width * height * 4,
        .usage = vk::BufferUsageFlagBits::eTransferDst,
    };

    const VmaAllocationCreateInfo alloc_create_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkBuffer unsafe_buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo alloc_info;
    VkBufferCreateInfo unsafe_buffer_info = static_cast<VkBufferCreateInfo>(staging_buffer_info);

    VkResult result = vmaCreateBuffer(instance.GetAllocator(), &unsafe_buffer_info,
                                      &alloc_create_info, &unsafe_buffer, &allocation, &alloc_info);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }

    vk::Buffer staging_buffer{unsafe_buffer};

    Frame frame{};
    main_present_window.RecreateFrame(&frame, width, height);

    DrawScreens(&frame, layout, false);

    scheduler.Record(
        [width, height, source_image = frame.image, staging_buffer](vk::CommandBuffer cmdbuf) {
            const vk::ImageMemoryBarrier read_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            const vk::ImageMemoryBarrier write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            static constexpr vk::MemoryBarrier memory_write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            };

            const vk::BufferImageCopy image_copy = {
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource =
                    {
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .imageOffset = {0, 0, 0},
                .imageExtent = {width, height, 1},
            };

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);
            cmdbuf.copyImageToBuffer(source_image, vk::ImageLayout::eTransferSrcOptimal,
                                     staging_buffer, image_copy);
            cmdbuf.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
                vk::DependencyFlagBits::eByRegion, memory_write_barrier, {}, write_barrier);
        });

    // Ensure the copy is fully completed before saving the screenshot
    scheduler.Finish();

    // Copy backing image data to the QImage screenshot buffer
    std::memcpy(settings.screenshot_bits, alloc_info.pMappedData, staging_buffer_info.size);

    // QImage::Format_RGB32 expects BGRA byte order. If the swapchain format is RGBA,
    // swap R and B channels so the screenshot colors are correct.
    if (main_present_window.GetSurfaceFormat() == vk::Format::eR8G8B8A8Unorm) {
        u8* pixels = static_cast<u8*>(settings.screenshot_bits);
        for (u32 i = 0; i < width * height; i++) {
            std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
        }
    }

    // Destroy allocated resources
    vmaDestroyBuffer(instance.GetAllocator(), staging_buffer, allocation);
    vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
    device.destroyFramebuffer(frame.framebuffer);
    device.destroyImageView(frame.image_view);
}

bool RendererVulkan::TryRenderScreenshotWithHostMemory() {
    // If the host-memory import alignment matches the allocation granularity of the platform, then
    // the entire span of memory can be trivially imported
    const bool trivial_import =
        instance.IsExternalMemoryHostSupported() &&
        instance.GetMinImportedHostPointerAlignment() == Common::GetPageSize();
    if (!trivial_import) {
        return false;
    }

    const vk::Device device = instance.GetDevice();

    const Layout::FramebufferLayout layout{settings.screenshot_framebuffer_layout};
    const u32 width = layout.width;
    const u32 height = layout.height;

    // For a span of memory [x, x + s], import [AlignDown(x, alignment), AlignUp(x + s, alignment)]
    // and maintain an offset to the start of the data
    const u64 import_alignment = instance.GetMinImportedHostPointerAlignment();
    const uintptr_t address = reinterpret_cast<uintptr_t>(settings.screenshot_bits);
    void* aligned_pointer = reinterpret_cast<void*>(Common::AlignDown(address, import_alignment));
    const u64 offset = address % import_alignment;
    const u64 aligned_size = Common::AlignUp(offset + width * height * 4ull, import_alignment);

    // Buffer<->Image mapping for the imported imported buffer
    const vk::BufferImageCopy buffer_image_copy = {
        .bufferOffset = offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };

    const vk::MemoryHostPointerPropertiesEXT import_properties =
        device.getMemoryHostPointerPropertiesEXT(
            vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, aligned_pointer);

    if (!import_properties.memoryTypeBits) {
        // Could not import memory
        return false;
    }

    const std::optional<u32> memory_type_index = FindMemoryType(
        instance.GetPhysicalDevice().getMemoryProperties(),
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        import_properties.memoryTypeBits);

    if (!memory_type_index.has_value()) {
        // Could not find memory type index
        return false;
    }

    const vk::StructureChain<vk::MemoryAllocateInfo, vk::ImportMemoryHostPointerInfoEXT>
        allocation_chain = {
            vk::MemoryAllocateInfo{
                .allocationSize = aligned_size,
                .memoryTypeIndex = memory_type_index.value(),
            },
            vk::ImportMemoryHostPointerInfoEXT{
                .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
                .pHostPointer = aligned_pointer,
            },
        };

    // Import host memory
    const vk::UniqueDeviceMemory imported_memory =
        device.allocateMemoryUnique(allocation_chain.get());

    const vk::StructureChain<vk::BufferCreateInfo, vk::ExternalMemoryBufferCreateInfo> buffer_info =
        {
            vk::BufferCreateInfo{
                .size = aligned_size,
                .usage = vk::BufferUsageFlagBits::eTransferDst,
                .sharingMode = vk::SharingMode::eExclusive,
            },
            vk::ExternalMemoryBufferCreateInfo{
                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
            },
        };

    // Bind imported memory to buffer
    const vk::UniqueBuffer imported_buffer = device.createBufferUnique(buffer_info.get());
    device.bindBufferMemory(imported_buffer.get(), imported_memory.get(), 0);

    Frame frame{};
    main_present_window.RecreateFrame(&frame, width, height);

    DrawScreens(&frame, layout, false);

    scheduler.Record([buffer_image_copy, source_image = frame.image,
                      imported_buffer = imported_buffer.get()](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier read_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        const vk::ImageMemoryBarrier write_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        static constexpr vk::MemoryBarrier memory_write_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);
        cmdbuf.copyImageToBuffer(source_image, vk::ImageLayout::eTransferSrcOptimal,
                                 imported_buffer, buffer_image_copy);
        cmdbuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
            vk::DependencyFlagBits::eByRegion, memory_write_barrier, {}, write_barrier);
    });

    // Ensure the copy is fully completed before saving the screenshot
    scheduler.Finish();

    // QImage::Format_RGB32 expects BGRA byte order. If the swapchain format is RGBA,
    // swap R and B channels so the screenshot colors are correct.
    if (main_present_window.GetSurfaceFormat() == vk::Format::eR8G8B8A8Unorm) {
        u8* pixels = static_cast<u8*>(settings.screenshot_bits);
        for (u32 i = 0; i < width * height; i++) {
            std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
        }
    }

    // Image data has been copied directly to host memory
    device.destroyFramebuffer(frame.framebuffer);
    device.destroyImageView(frame.image_view);

    return true;
}

void RendererVulkan::NotifySurfaceChanged(bool is_second_window) {
    if (is_second_window) {
        if (secondary_present_window_ptr) {
            secondary_present_window_ptr->NotifySurfaceChanged();
        }
    } else {
        main_present_window.NotifySurfaceChanged();
    }
}

} // namespace Vulkan
