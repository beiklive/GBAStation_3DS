// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_common.h"

#include <array>
#include <atomic>
#include <unordered_map>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "common/logging/log.h"
#include "GBAStation/input_mapping.h"
#include "GBAStation/overlay/overlay_ui.h"
#include "GBAStation/overlay/vulkan_overlay.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_present_window.h"

namespace SwitchFrontend::VulkanOverlay {
namespace {

constexpr const char* Tag = "[gbastation-3ds-menu]";
constexpr std::array<const char*, 2> FontPaths{{
    "romfs:/rescources/material/MaterialIcons-Regular.ttf",
    "sdmc:/GBAStation/rescources/material/MaterialIcons-Regular.ttf",
}};

std::atomic_bool initialized{};
std::atomic_bool visible{};
std::atomic_bool exit_requested{};
std::atomic_int pending_action{};
std::atomic_uint pending_navigation{};

bool previous_combo{};
bool psm_initialized{};
bool pl_initialized{};
bool backend_ready{};

vk::Instance instance;
vk::PhysicalDevice physical_device;
vk::Device device;
vk::Queue graphics_queue;
u32 graphics_queue_family{};
u32 image_count{2};
vk::DescriptorPool descriptor_pool;
vk::RenderPass render_pass;
vk::Format color_format{vk::Format::eUndefined};

struct SwapImageResource {
    vk::ImageView view;
    vk::Framebuffer framebuffer;
};
std::unordered_map<VkImage, SwapImageResource> swap_resources;
vk::Extent2D framebuffer_extent{};

struct PreviousNavigation {
    bool up{};
    bool down{};
    bool left{};
    bool right{};
    bool accept{};
    bool cancel{};
};
PreviousNavigation previous_navigation{};

enum NavigationBits : unsigned int {
    NavigationUp = 1u << 0,
    NavigationDown = 1u << 1,
    NavigationLeft = 1u << 2,
    NavigationRight = 1u << 3,
    NavigationAccept = 1u << 4,
    NavigationCancel = 1u << 5,
};

PFN_vkVoidFunction LoadVulkanFunction(const char* name, void* user_data) {
    const auto vk_instance = static_cast<VkInstance>(user_data);
    if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr) {
        return nullptr;
    }
    return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(vk_instance, name);
}

void DestroySwapResources() {
    if (!device) {
        return;
    }
    for (auto& [image, resource] : swap_resources) {
        device.destroyFramebuffer(resource.framebuffer);
        device.destroyImageView(resource.view);
    }
    swap_resources.clear();
    framebuffer_extent = vk::Extent2D{};
}

bool CreateRenderPass(vk::Format format) {
    const vk::AttachmentDescription attachment{
        .format = format,
        .samples = vk::SampleCountFlagBits::e1,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .finalLayout = vk::ImageLayout::eColorAttachmentOptimal,
    };
    const vk::AttachmentReference color_reference{
        .attachment = 0,
        .layout = vk::ImageLayout::eColorAttachmentOptimal,
    };
    const vk::SubpassDescription subpass{
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_reference,
    };
    const vk::SubpassDependency dependency{
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
        .dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead |
                         vk::AccessFlagBits::eColorAttachmentWrite,
    };
    const vk::RenderPassCreateInfo info{
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    };
    render_pass = device.createRenderPass(info);
    return static_cast<bool>(render_pass);
}

vk::Framebuffer GetFramebuffer(vk::Image image, vk::Extent2D extent) {
    if (extent != framebuffer_extent) {
        DestroySwapResources();
        framebuffer_extent = extent;
    }

    const VkImage key = static_cast<VkImage>(image);
    const auto existing = swap_resources.find(key);
    if (existing != swap_resources.end()) {
        return existing->second.framebuffer;
    }

    const vk::ImageViewCreateInfo view_info{
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = color_format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    const vk::ImageView view = device.createImageView(view_info);
    const vk::FramebufferCreateInfo framebuffer_info{
        .renderPass = render_pass,
        .attachmentCount = 1,
        .pAttachments = &view,
        .width = extent.width,
        .height = extent.height,
        .layers = 1,
    };
    const vk::Framebuffer framebuffer = device.createFramebuffer(framebuffer_info);
    swap_resources.emplace(key, SwapImageResource{view, framebuffer});
    return framebuffer;
}

bool EnsureBackend(vk::Format format) {
    if (backend_ready) {
        return true;
    }
    color_format = format;
    if (!CreateRenderPass(format)) {
        return false;
    }

    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.ApiVersion = VK_API_VERSION_1_1;
    init_info.Instance = static_cast<VkInstance>(instance);
    init_info.PhysicalDevice = static_cast<VkPhysicalDevice>(physical_device);
    init_info.Device = static_cast<VkDevice>(device);
    init_info.QueueFamily = graphics_queue_family;
    init_info.Queue = static_cast<VkQueue>(graphics_queue);
    init_info.DescriptorPool = static_cast<VkDescriptorPool>(descriptor_pool);
    init_info.RenderPass = static_cast<VkRenderPass>(render_pass);
    init_info.MinImageCount = std::max(image_count, 2u);
    init_info.ImageCount = std::max(image_count, 2u);
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (!ImGui_ImplVulkan_Init(&init_info)) {
        LOG_ERROR(Render_Vulkan, "{} ImGui Vulkan initialization failed", Tag);
        return false;
    }
    backend_ready = true;
    LOG_INFO(Render_Vulkan, "{} Vulkan backend ready format={} images={}", Tag,
             vk::to_string(format), image_count);
    return true;
}

void DrawCallback(vk::CommandBuffer command_buffer, vk::Image image, vk::Extent2D extent,
                  vk::Format format) {
    if (!initialized.load(std::memory_order_acquire)) {
        return;
    }

    const bool menu_visible = visible.load(std::memory_order_relaxed);
    OverlayUI::SetVisible(menu_visible);
    if (!menu_visible && !OverlayUI::HasTransientContent()) {
        return;
    }
    if (!EnsureBackend(format)) {
        return;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = {static_cast<float>(extent.width), static_cast<float>(extent.height)};
    io.DisplayFramebufferScale = {1.0f, 1.0f};
    io.DeltaTime = 1.0f / 60.0f;

    const unsigned int navigation = pending_navigation.exchange(0, std::memory_order_acq_rel);
    OverlayUI::FeedNav({
        .up = (navigation & NavigationUp) != 0,
        .down = (navigation & NavigationDown) != 0,
        .left = (navigation & NavigationLeft) != 0,
        .right = (navigation & NavigationRight) != 0,
        .accept = (navigation & NavigationAccept) != 0,
        .cancel = (navigation & NavigationCancel) != 0,
    });

    ImGui::NewFrame();
    const OverlayUI::Action action =
        OverlayUI::Render(static_cast<int>(extent.width), static_cast<int>(extent.height));
    ImGui::Render();

    if (action != OverlayUI::Action::None) {
        pending_action.store(static_cast<int>(action), std::memory_order_release);
        if (action == OverlayUI::Action::Exit) {
            exit_requested.store(true, std::memory_order_release);
        }
        if (action == OverlayUI::Action::Resume || action == OverlayUI::Action::Reset ||
            action == OverlayUI::Action::Exit || OverlayUI::IsSaveStateAction(action) ||
            OverlayUI::IsLoadStateAction(action)) {
            visible.store(false, std::memory_order_release);
            pending_navigation.store(0, std::memory_order_release);
            OverlayUI::SetVisible(false);
        }
    }

    const vk::Framebuffer framebuffer = GetFramebuffer(image, extent);
    if (!framebuffer) {
        return;
    }
    const vk::RenderPassBeginInfo render_begin{
        .renderPass = render_pass,
        .framebuffer = framebuffer,
        .renderArea{.offset = {0, 0}, .extent = extent},
        .clearValueCount = 0,
        .pClearValues = nullptr,
    };
    command_buffer.beginRenderPass(render_begin, vk::SubpassContents::eInline);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                                    static_cast<VkCommandBuffer>(command_buffer));
    command_buffer.endRenderPass();
}

void ResetCallback() {
    DestroySwapResources();
}

} // namespace

bool Init(Vulkan::RendererVulkan& renderer) {
    if (initialized.load(std::memory_order_acquire)) {
        return true;
    }

    const Vulkan::Instance& renderer_instance = renderer.GetVulkanInstance();
    instance = renderer_instance.GetInstance();
    physical_device = renderer_instance.GetPhysicalDevice();
    device = renderer_instance.GetDevice();
    graphics_queue = renderer_instance.GetGraphicsQueue();
    graphics_queue_family = renderer_instance.GetGraphicsQueueFamilyIndex();
    image_count = std::max(renderer.GetMainPresentWindow().ImageCount(), 2u);
    if (!instance || !device) {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImFontConfig font_config{};
    font_config.OversampleH = 2;
    font_config.OversampleV = 1;
    font_config.PixelSnapH = true;
    font_config.FontDataOwnedByAtlas = false;
    if (plInitialize(PlServiceType_User) == 0) {
        pl_initialized = true;
        PlFontData system_font{};
        if (plGetSharedFontByType(&system_font, PlSharedFontType_ChineseSimplified) == 0 &&
            system_font.address && system_font.size > 0) {
            io.FontDefault = io.Fonts->AddFontFromMemoryTTF(
                system_font.address, static_cast<int>(system_font.size), 30.0f, &font_config,
                io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        }

        if (io.FontDefault) {
            static constexpr ImWchar IconRanges[]{0xE000, 0xF8FF, 0};
            PlFontData nintendo_font{};
            if (plGetSharedFontByType(&nintendo_font, PlSharedFontType_NintendoExt) == 0 &&
                nintendo_font.address && nintendo_font.size > 0) {
                ImFontConfig icon_config = font_config;
                icon_config.MergeMode = true;
                io.Fonts->AddFontFromMemoryTTF(nintendo_font.address,
                                               static_cast<int>(nintendo_font.size), 30.0f,
                                               &icon_config, IconRanges);
            }
            for (const char* path : FontPaths) {
                ImFontConfig icon_config{};
                icon_config.MergeMode = true;
                icon_config.PixelSnapH = true;
                if (io.Fonts->AddFontFromFileTTF(path, 30.0f, &icon_config, IconRanges)) {
                    LOG_INFO(Render_Vulkan, "{} merged Material icon font {}", Tag, path);
                    break;
                }
            }
            LOG_INFO(Render_Vulkan, "{} loaded Switch shared menu fonts", Tag);
        }
    }
    if (!io.FontDefault) {
        io.FontDefault = io.Fonts->AddFontDefault();
        LOG_WARNING(Render_Vulkan, "{} Switch shared font unavailable; using ImGui default", Tag);
    }

    const vk::DescriptorPoolSize pool_size{
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 64,
    };
    const vk::DescriptorPoolCreateInfo pool_info{
        .flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet,
        .maxSets = 64,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    descriptor_pool = device.createDescriptorPool(pool_info);

    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, LoadVulkanFunction,
                                        static_cast<VkInstance>(instance))) {
        device.destroyDescriptorPool(descriptor_pool);
        descriptor_pool = VK_NULL_HANDLE;
        ImGui::DestroyContext();
        return false;
    }

    if (!psm_initialized && psmInitialize() == 0) {
        psm_initialized = true;
    }

    visible.store(false);
    exit_requested.store(false);
    pending_action.store(0);
    pending_navigation.store(0);
    backend_ready = false;
    previous_combo = false;
    previous_navigation = {};

    Vulkan::SetOverlayResetCallback(&ResetCallback);
    Vulkan::SetOverlayDrawCallback(&DrawCallback);
    initialized.store(true, std::memory_order_release);
    LOG_INFO(Render_Vulkan, "{} initialized", Tag);
    return true;
}

void Update(PadState* pad) {
    if (!initialized.load(std::memory_order_acquire) || !pad) {
        return;
    }

    const u64 held = padGetButtons(pad);
    const u64 menu_mask = InputMapping::MenuHotkeyMask();
    const bool combo = menu_mask != 0 && (held & menu_mask) == menu_mask;
    if (combo && !previous_combo) {
        const bool next_visible = !visible.load(std::memory_order_relaxed);
        visible.store(next_visible, std::memory_order_release);
        pending_navigation.store(0, std::memory_order_release);
    }
    previous_combo = combo;

    const bool up = (held & (HidNpadButton_AnyUp | HidNpadButton_StickLUp)) != 0;
    const bool down = (held & (HidNpadButton_AnyDown | HidNpadButton_StickLDown)) != 0;
    const bool left = (held & (HidNpadButton_AnyLeft | HidNpadButton_StickLLeft)) != 0;
    const bool right = (held & (HidNpadButton_AnyRight | HidNpadButton_StickLRight)) != 0;
    const bool accept = (held & HidNpadButton_A) != 0;
    const bool cancel = (held & HidNpadButton_B) != 0;

    unsigned int navigation{};
    if (up && !previous_navigation.up)
        navigation |= NavigationUp;
    if (down && !previous_navigation.down)
        navigation |= NavigationDown;
    if (left && !previous_navigation.left)
        navigation |= NavigationLeft;
    if (right && !previous_navigation.right)
        navigation |= NavigationRight;
    if (accept && !previous_navigation.accept)
        navigation |= NavigationAccept;
    if (cancel && !previous_navigation.cancel)
        navigation |= NavigationCancel;

    if (visible.load(std::memory_order_relaxed) && navigation) {
        pending_navigation.fetch_or(navigation, std::memory_order_acq_rel);
    }
    previous_navigation = {up, down, left, right, accept, cancel};
}

bool IsVisible() {
    return visible.load(std::memory_order_acquire);
}

bool ShouldExit() {
    return exit_requested.load(std::memory_order_acquire);
}

int ConsumeAction() {
    return pending_action.exchange(0, std::memory_order_acq_rel);
}

void Shutdown() {
    if (!initialized.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    Vulkan::SetOverlayDrawCallback(nullptr);
    Vulkan::SetOverlayResetCallback(nullptr);
    if (device) {
        device.waitIdle();
    }

    DestroySwapResources();
    if (backend_ready) {
        ImGui_ImplVulkan_Shutdown();
        backend_ready = false;
    }
    if (ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
    }
    if (render_pass) {
        device.destroyRenderPass(render_pass);
        render_pass = VK_NULL_HANDLE;
    }
    if (descriptor_pool) {
        device.destroyDescriptorPool(descriptor_pool);
        descriptor_pool = VK_NULL_HANDLE;
    }
    if (psm_initialized) {
        psmExit();
        psm_initialized = false;
    }
    if (pl_initialized) {
        plExit();
        pl_initialized = false;
    }

    OverlayUI::SetVisible(false);
    OverlayUI::ShowToast({});
    visible.store(false);
    exit_requested.store(false);
    pending_action.store(0);
    pending_navigation.store(0);
    color_format = vk::Format::eUndefined;
    LOG_INFO(Render_Vulkan, "{} shutdown complete", Tag);
}

} // namespace SwitchFrontend::VulkanOverlay
