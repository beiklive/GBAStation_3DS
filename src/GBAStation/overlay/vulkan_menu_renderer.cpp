// Copyright 2026 Azahar Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "GBAStation/overlay/vulkan_menu_renderer.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "GBAStation/shaders/menu_frag_spv.h"
#include "GBAStation/shaders/menu_vert_spv.h"
#include "GBAStation/switch_libnx.h"
#include "common/logging/log.h"
#include "video_core/overlay.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_memory_util.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace SwitchFrontend::VulkanMenuRenderer {
namespace {

constexpr const char* Tag = "[gbastation-menu-renderer]";
constexpr u32 AtlasWidth = 1024;
constexpr u32 AtlasHeight = 1024;
constexpr u32 GradientWidth = 512;
constexpr u32 GradientHeight = 4;
constexpr float PackedFontSize = 32.0f;
constexpr vk::DeviceSize VertexBufferSize = 2 * 1024 * 1024;

struct Vertex {
    float x;
    float y;
    float u;
    float v;
    float r;
    float g;
    float b;
    float a;
    float textured;
};

struct BufferResource {
    vk::Buffer buffer;
    vk::DeviceMemory memory;
    void* mapped{};
};

struct ImageResource {
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView view;
};

struct SwapResource {
    vk::ImageView view;
    vk::Framebuffer framebuffer;
};

vk::PhysicalDevice physical_device;
vk::Device device;
vk::PhysicalDeviceMemoryProperties memory_properties;
vk::ShaderModule vertex_shader;
vk::ShaderModule fragment_shader;
vk::DescriptorSetLayout descriptor_set_layout;
vk::DescriptorPool descriptor_pool;
vk::DescriptorSet descriptor_set;
vk::DescriptorSet overlay_descriptor_set;
vk::DescriptorSet preview_descriptor_set;
vk::PipelineLayout pipeline_layout;
vk::Pipeline pipeline;
vk::RenderPass render_pass;
vk::Format render_format{vk::Format::eUndefined};
vk::Sampler font_sampler;
vk::Sampler gradient_sampler;
ImageResource atlas_image;
ImageResource gradient_image;
BufferResource atlas_staging;
BufferResource gradient_staging;
BufferResource vertex_buffer;
ImageResource overlay_image;
BufferResource overlay_staging;
ImageResource preview_image;
BufferResource preview_staging;
std::unordered_map<VkImage, SwapResource> swap_resources;
vk::Extent2D framebuffer_extent{};
bool atlas_uploaded{};
bool overlay_uploaded{};
bool overlay_active{};
u32 overlay_width{};
u32 overlay_height{};
u32 overlay_vertex_count{};
std::string loaded_overlay_path;
bool preview_uploaded{};
bool preview_active{};
u32 preview_width{};
u32 preview_height{};
u32 preview_vertex_first{};
u32 preview_vertex_count{};
std::string loaded_preview_path;
bool initialized{};

std::vector<u8> font_data;
std::vector<u8> nintendo_font_data;
std::vector<u8> material_font_data;
std::vector<u8> atlas_pixels;
std::vector<u8> gradient_pixels;
std::vector<int> codepoints;
std::vector<stbtt_packedchar> packed_chars;
std::unordered_map<int, std::size_t> glyph_indices;
std::vector<Vertex> vertices;

constexpr std::array<const char*, static_cast<int>(Item::Count)> ItemLabels{{
    "返回游戏", "保存状态", "读取状态", "金手指", "画面设置", "重置游戏", "退出游戏",
}};

constexpr std::array<int, static_cast<int>(Item::Count)> ItemIcons{{
    0xE5C4, 0xE161, 0xE2C6, 0xE3AE, 0xE333, 0xE5D5, 0xE879,
}};

constexpr int NintendoIconA = 0xE0E0;
constexpr int NintendoIconB = 0xE0E1;
constexpr int NintendoIconX = 0xE0E2;
constexpr int NintendoIconL = 0xE0E4;
constexpr int NintendoIconR = 0xE0E5;

bool ReadFile(const char* path, std::vector<u8>& output) {
    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return false;
    }
    output.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(output.data(), 1, output.size(), file);
    std::fclose(file);
    if (read != output.size()) {
        output.clear();
        return false;
    }
    return true;
}

std::vector<int> DecodeUtf8(std::string_view text) {
    std::vector<int> output;
    for (std::size_t i = 0; i < text.size();) {
        const u8 c = static_cast<u8>(text[i]);
        int codepoint = '?';
        std::size_t length = 1;
        if ((c & 0x80) == 0) {
            codepoint = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            codepoint = ((c & 0x1F) << 6) | (static_cast<u8>(text[i + 1]) & 0x3F);
            length = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            codepoint = ((c & 0x0F) << 12) |
                        ((static_cast<u8>(text[i + 1]) & 0x3F) << 6) |
                        (static_cast<u8>(text[i + 2]) & 0x3F);
            length = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
            codepoint = ((c & 0x07) << 18) |
                        ((static_cast<u8>(text[i + 1]) & 0x3F) << 12) |
                        ((static_cast<u8>(text[i + 2]) & 0x3F) << 6) |
                        (static_cast<u8>(text[i + 3]) & 0x3F);
            length = 4;
        }
        output.push_back(codepoint);
        i += length;
    }
    return output;
}

bool BuildFontAtlas() {
    if (plInitialize(PlServiceType_User) != 0) {
        LOG_ERROR(Render_Vulkan, "{} pl:u initialization failed", Tag);
        return false;
    }
    PlFontData shared_font{};
    LibnxResult rc = plGetSharedFontByType(&shared_font, PlSharedFontType_ChineseSimplified);
    if (rc != 0 || !shared_font.address || shared_font.size == 0) {
        plExit();
        LOG_ERROR(Render_Vulkan, "{} shared Chinese font unavailable rc=0x{:x}", Tag, rc);
        return false;
    }
    font_data.resize(shared_font.size);
    std::memcpy(font_data.data(), shared_font.address, shared_font.size);
    PlFontData nintendo_font{};
    rc = plGetSharedFontByType(&nintendo_font, PlSharedFontType_NintendoExt);
    if (rc != 0 || !nintendo_font.address || nintendo_font.size == 0) {
        plExit();
        LOG_ERROR(Render_Vulkan, "{} Nintendo extended font unavailable rc=0x{:x}", Tag, rc);
        return false;
    }
    nintendo_font_data.resize(nintendo_font.size);
    std::memcpy(nintendo_font_data.data(), nintendo_font.address, nintendo_font.size);
    plExit();

    constexpr std::array<const char*, 3> MaterialPaths{{
        "romfs:/rescources/material/MaterialIcons-Regular.ttf",
        "sdmc:/GBAStation/rescources/material/MaterialIcons-Regular.ttf",
        "/GBAStation/rescources/material/MaterialIcons-Regular.ttf",
    }};
    bool material_loaded = false;
    for (const char* path : MaterialPaths) {
        if (ReadFile(path, material_font_data)) {
            material_loaded = true;
            break;
        }
    }
    if (!material_loaded) {
        LOG_ERROR(Render_Vulkan, "{} Material Icons font unavailable", Tag);
        return false;
    }

    constexpr std::string_view UsedText =
        "游戏菜单返回游戏保存状态读取状态金手指画面设置重置退出"
        "档位已有状态空存档槽继续按确定返回列表不可用"
        "暂无金手指功能将在后续版本提供屏幕布局画面方向整数倍缩放屏幕间距"
        "自定义画面布局调整当前项上屏布局下屏布局大小缩放偏移"
        "基础画面设置布局设置个性化设置快进倍率三维分辨率遮罩选择遮罩开关遮罩文件"
        "同步遮罩同步画面设置执行已同步到个游戏失败"
        "选择遮罩未选择文件夹图片列表目录上级目录预览加载失败暂无可用文件"
        "竖向横向上屏优先混合仅上屏仅下屏自定义透明度开启关闭°"
        "安全关闭模拟器未保存的游戏进度可能丢失"
        "GBAStation 3DS Resume Save Load Cheats Display Reset Exit Slot Empty Occupied A B X";
    std::set<int> unique;
    for (int cp = 32; cp <= 126; ++cp) {
        unique.insert(cp);
    }
    for (const int cp : DecodeUtf8(UsedText)) {
        unique.insert(cp);
    }
    std::vector<int> regular_codepoints(unique.begin(), unique.end());
    const std::vector<int> nintendo_codepoints{
        NintendoIconA, NintendoIconB, NintendoIconX, NintendoIconL, NintendoIconR,
    };
    const std::vector<int> material_codepoints{
        0xE5C4, 0xE161, 0xE2C6, 0xE3AE, 0xE333, 0xE5D5, 0xE879,
        0xE01F, 0xE433, 0xE3F4, 0xE8F1, 0xE3C9, 0xE41A, 0xE8D4, 0xE53B,
        0xE5CC, 0xE2C7, 0xE873,
    };
    codepoints = regular_codepoints;
    codepoints.insert(codepoints.end(), nintendo_codepoints.begin(), nintendo_codepoints.end());
    codepoints.insert(codepoints.end(), material_codepoints.begin(), material_codepoints.end());
    packed_chars.resize(codepoints.size());
    atlas_pixels.assign(AtlasWidth * AtlasHeight, 0);

    stbtt_pack_context context{};
    if (!stbtt_PackBegin(&context, atlas_pixels.data(), AtlasWidth, AtlasHeight, 0, 2, nullptr)) {
        return false;
    }
    stbtt_PackSetOversampling(&context, 1, 1);
    auto pack_range = [&](const std::vector<u8>& data, const std::vector<int>& points,
                          std::size_t offset) {
        stbtt_pack_range range{};
        range.font_size = PackedFontSize;
        range.array_of_unicode_codepoints = const_cast<int*>(points.data());
        range.num_chars = static_cast<int>(points.size());
        range.chardata_for_range = packed_chars.data() + offset;
        return stbtt_GetFontOffsetForIndex(data.data(), 0) >= 0 &&
               stbtt_PackFontRanges(&context, data.data(), 0, &range, 1) != 0;
    };
    bool packed = pack_range(font_data, regular_codepoints, 0);
    packed = packed && pack_range(nintendo_font_data, nintendo_codepoints,
                                  regular_codepoints.size());
    packed = packed && pack_range(material_font_data, material_codepoints,
                                  regular_codepoints.size() + nintendo_codepoints.size());
    stbtt_PackEnd(&context);
    if (!packed) {
        LOG_ERROR(Render_Vulkan, "{} failed to pack shared font", Tag);
        return false;
    }
    glyph_indices.clear();
    for (std::size_t i = 0; i < codepoints.size(); ++i) {
        glyph_indices.emplace(codepoints[i], i);
    }
    atlas_pixels[0] = 255;
    return true;
}

bool LoadGradientTexture() {
    constexpr std::array<const char*, 2> paths{{
        "romfs:/rescources/ui/border_gradient.png",
        "sdmc:/GBAStation/rescources/ui/border_gradient.png",
    }};
    for (const char* path : paths) {
        int width = 0;
        int height = 0;
        int channels = 0;
        u8* pixels = stbi_load(path, &width, &height, &channels, 4);
        if (!pixels) {
            continue;
        }
        if (width == static_cast<int>(GradientWidth) &&
            height == static_cast<int>(GradientHeight)) {
            gradient_pixels.assign(pixels, pixels + GradientWidth * GradientHeight * 4);
            stbi_image_free(pixels);
            LOG_INFO(Render_Vulkan, "{} loaded focus gradient {}", Tag, path);
            return true;
        }
        stbi_image_free(pixels);
    }
    LOG_ERROR(Render_Vulkan, "{} focus gradient unavailable", Tag);
    return false;
}

bool CreateBuffer(vk::DeviceSize size, vk::BufferUsageFlags usage,
                  vk::MemoryPropertyFlags properties, BufferResource& resource, bool map) {
    const vk::BufferCreateInfo buffer_info{
        .size = size,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    resource.buffer = device.createBuffer(buffer_info);
    const vk::MemoryRequirements requirements = device.getBufferMemoryRequirements(resource.buffer);
    const auto memory_type = Vulkan::FindMemoryType(
        memory_properties, properties, std::bitset<32>{requirements.memoryTypeBits});
    if (!memory_type) {
        return false;
    }
    const vk::MemoryAllocateInfo allocate_info{
        .allocationSize = requirements.size,
        .memoryTypeIndex = *memory_type,
    };
    resource.memory = device.allocateMemory(allocate_info);
    device.bindBufferMemory(resource.buffer, resource.memory, 0);
    if (map) {
        resource.mapped = device.mapMemory(resource.memory, 0, size);
    }
    return true;
}

void DestroyBuffer(BufferResource& resource) {
    if (resource.mapped && resource.memory) {
        device.unmapMemory(resource.memory);
    }
    if (resource.buffer) {
        device.destroyBuffer(resource.buffer);
    }
    if (resource.memory) {
        device.freeMemory(resource.memory);
    }
    resource = {};
}

bool CreateImage(vk::Format format, u32 width, u32 height, ImageResource& resource) {
    const vk::ImageCreateInfo image_info{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    resource.image = device.createImage(image_info);
    const vk::MemoryRequirements requirements = device.getImageMemoryRequirements(resource.image);
    const auto memory_type = Vulkan::FindMemoryType(
        memory_properties, vk::MemoryPropertyFlagBits::eDeviceLocal,
        std::bitset<32>{requirements.memoryTypeBits});
    if (!memory_type) {
        return false;
    }
    const vk::MemoryAllocateInfo allocate_info{
        .allocationSize = requirements.size,
        .memoryTypeIndex = *memory_type,
    };
    resource.memory = device.allocateMemory(allocate_info);
    device.bindImageMemory(resource.image, resource.memory, 0);
    const vk::ImageViewCreateInfo view_info{
        .image = resource.image,
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
    resource.view = device.createImageView(view_info);
    return true;
}

void DestroyImage(ImageResource& resource) {
    if (resource.view) device.destroyImageView(resource.view);
    if (resource.image) device.destroyImage(resource.image);
    if (resource.memory) device.freeMemory(resource.memory);
    resource = {};
}

void DestroySwapResources() {
    for (auto& [image, resource] : swap_resources) {
        device.destroyFramebuffer(resource.framebuffer);
        device.destroyImageView(resource.view);
    }
    swap_resources.clear();
    framebuffer_extent = vk::Extent2D{};
}

void DestroyRenderObjects() {
    DestroySwapResources();
    if (pipeline) device.destroyPipeline(pipeline);
    if (render_pass) device.destroyRenderPass(render_pass);
    pipeline = VK_NULL_HANDLE;
    render_pass = VK_NULL_HANDLE;
    render_format = vk::Format::eUndefined;
}

bool CreateDescriptors() {
    const std::array bindings{
        vk::DescriptorSetLayoutBinding{
            .binding = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
        vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        },
    };
    descriptor_set_layout = device.createDescriptorSetLayout({
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
    const vk::DescriptorPoolSize pool_size{
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 6,
    };
    descriptor_pool = device.createDescriptorPool({
        .maxSets = 3,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    });
    const std::array layouts{descriptor_set_layout, descriptor_set_layout,
                             descriptor_set_layout};
    const vk::DescriptorSetAllocateInfo allocate_info{
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = static_cast<u32>(layouts.size()),
        .pSetLayouts = layouts.data(),
    };
    const auto sets = device.allocateDescriptorSets(allocate_info);
    descriptor_set = sets[0];
    overlay_descriptor_set = sets[1];
    preview_descriptor_set = sets[2];
    const vk::SamplerCreateInfo font_sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 1.0f,
    };
    font_sampler = device.createSampler(font_sampler_info);
    const vk::SamplerCreateInfo gradient_sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eRepeat,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .maxLod = 1.0f,
    };
    gradient_sampler = device.createSampler(gradient_sampler_info);
    const std::array image_infos{
        vk::DescriptorImageInfo{
            .sampler = font_sampler,
            .imageView = atlas_image.view,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
        vk::DescriptorImageInfo{
            .sampler = gradient_sampler,
            .imageView = gradient_image.view,
            .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        },
    };
    const std::array writes{
        vk::WriteDescriptorSet{
            .dstSet = descriptor_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_infos[0],
        },
        vk::WriteDescriptorSet{
            .dstSet = descriptor_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_infos[1],
        },
    };
    device.updateDescriptorSets(static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    const std::array overlay_writes{
        vk::WriteDescriptorSet{
            .dstSet = overlay_descriptor_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_infos[0],
        },
        vk::WriteDescriptorSet{
            .dstSet = overlay_descriptor_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_infos[1],
        },
    };
    device.updateDescriptorSets(static_cast<u32>(overlay_writes.size()), overlay_writes.data(),
                                0, nullptr);
    const std::array preview_writes{
        vk::WriteDescriptorSet{
            .dstSet = preview_descriptor_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_infos[0],
        },
        vk::WriteDescriptorSet{
            .dstSet = preview_descriptor_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
            .pImageInfo = &image_infos[1],
        },
    };
    device.updateDescriptorSets(static_cast<u32>(preview_writes.size()), preview_writes.data(),
                                0, nullptr);
    const vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout,
    };
    pipeline_layout = device.createPipelineLayout(layout_info);
    return true;
}

bool CreateShaders() {
    vertex_shader = Vulkan::CompileSPV(gbastation_menu_vert_spv, device);
    fragment_shader = Vulkan::CompileSPV(gbastation_menu_frag_spv, device);
    return static_cast<bool>(vertex_shader) && static_cast<bool>(fragment_shader);
}

bool EnsureRenderObjects(vk::Format format) {
    if (render_pass && pipeline && render_format == format) {
        return true;
    }
    DestroyRenderObjects();
    render_format = format;

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
    const vk::AttachmentReference color_reference{0, vk::ImageLayout::eColorAttachmentOptimal};
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
    render_pass = device.createRenderPass({
        .attachmentCount = 1,
        .pAttachments = &attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    });

    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vertex_shader,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fragment_shader,
            .pName = "main",
        },
    };
    const vk::VertexInputBindingDescription binding{0, sizeof(Vertex), vk::VertexInputRate::eVertex};
    const std::array attributes{
        vk::VertexInputAttributeDescription{0, 0, vk::Format::eR32G32Sfloat,
                                            offsetof(Vertex, x)},
        vk::VertexInputAttributeDescription{1, 0, vk::Format::eR32G32Sfloat,
                                            offsetof(Vertex, u)},
        vk::VertexInputAttributeDescription{2, 0, vk::Format::eR32G32B32A32Sfloat,
                                            offsetof(Vertex, r)},
        vk::VertexInputAttributeDescription{3, 0, vk::Format::eR32Sfloat,
                                            offsetof(Vertex, textured)},
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };
    const vk::PipelineInputAssemblyStateCreateInfo assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewport_state{.viewportCount = 1, .scissorCount = 1};
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eCounterClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisample{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState blend_attachment{
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
    const vk::PipelineColorBlendStateCreateInfo blend{
        .attachmentCount = 1,
        .pAttachments = &blend_attachment,
    };
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic{
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };
    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .stageCount = static_cast<u32>(stages.size()),
        .pStages = stages.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &raster,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = pipeline_layout,
        .renderPass = render_pass,
    };
    const auto [result, created_pipeline] = device.createGraphicsPipeline({}, pipeline_info);
    if (result != vk::Result::eSuccess) {
        return false;
    }
    pipeline = created_pipeline;
    return true;
}

vk::Framebuffer GetFramebuffer(vk::Image image, vk::Extent2D extent) {
    if (extent != framebuffer_extent) {
        DestroySwapResources();
        framebuffer_extent = extent;
    }
    const VkImage key = static_cast<VkImage>(image);
    if (const auto it = swap_resources.find(key); it != swap_resources.end()) {
        return it->second.framebuffer;
    }
    const vk::ImageViewCreateInfo view_info{
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = render_format,
        .subresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
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
    swap_resources.emplace(key, SwapResource{view, framebuffer});
    return framebuffer;
}

void UploadImage(vk::CommandBuffer command_buffer, vk::Buffer staging, vk::Image image,
                 u32 width, u32 height) {
    const vk::ImageMemoryBarrier to_transfer{
        .srcAccessMask = {},
        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                   vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                                   to_transfer);
    const vk::BufferImageCopy copy{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource{vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    command_buffer.copyBufferToImage(staging, image,
                                     vk::ImageLayout::eTransferDstOptimal, copy);
    const vk::ImageMemoryBarrier to_shader{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead,
        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
        .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
    };
    command_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                   vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {},
                                   to_shader);
}

bool EnsureOverlayTexture(const State& state) {
    const bool wanted = state.display.overlay_enabled && !state.display.overlay_path.empty();
    if (!wanted) {
        overlay_active = false;
        return true;
    }
    if (loaded_overlay_path == state.display.overlay_path && overlay_image.image) {
        overlay_active = true;
        return true;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    u8* pixels = stbi_load(state.display.overlay_path.c_str(), &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        overlay_active = false;
        LOG_ERROR(Render_Vulkan, "{} failed to load overlay {}", Tag,
                  state.display.overlay_path);
        return false;
    }

    if (overlay_image.image || overlay_staging.buffer) {
        device.waitIdle();
        DestroyBuffer(overlay_staging);
        DestroyImage(overlay_image);
    }
    const std::size_t byte_size = static_cast<std::size_t>(width) * height * 4;
    const bool created =
        CreateImage(vk::Format::eR8G8B8A8Unorm, static_cast<u32>(width),
                    static_cast<u32>(height), overlay_image) &&
        CreateBuffer(byte_size, vk::BufferUsageFlagBits::eTransferSrc,
                     vk::MemoryPropertyFlagBits::eHostVisible |
                         vk::MemoryPropertyFlagBits::eHostCoherent,
                     overlay_staging, true);
    if (!created) {
        stbi_image_free(pixels);
        DestroyBuffer(overlay_staging);
        DestroyImage(overlay_image);
        overlay_active = false;
        return false;
    }
    std::memcpy(overlay_staging.mapped, pixels, byte_size);
    armDCacheFlush(overlay_staging.mapped, byte_size);
    stbi_image_free(pixels);

    const vk::DescriptorImageInfo overlay_info{
        .sampler = gradient_sampler,
        .imageView = overlay_image.view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write{
        .dstSet = overlay_descriptor_set,
        .dstBinding = 1,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &overlay_info,
    };
    device.updateDescriptorSets(1, &write, 0, nullptr);
    overlay_width = static_cast<u32>(width);
    overlay_height = static_cast<u32>(height);
    overlay_uploaded = false;
    overlay_active = true;
    loaded_overlay_path = state.display.overlay_path;
    LOG_INFO(Render_Vulkan, "{} loaded overlay {} size={}x{}", Tag,
             loaded_overlay_path, overlay_width, overlay_height);
    return true;
}

bool EnsurePreviewTexture(const State& state) {
    if (!state.file_preview || state.file_preview_path.empty()) {
        preview_active = false;
        return true;
    }
    if (loaded_preview_path == state.file_preview_path && preview_image.image) {
        preview_active = true;
        return true;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    u8* pixels = stbi_load(state.file_preview_path.c_str(), &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        preview_active = false;
        loaded_preview_path = state.file_preview_path;
        LOG_ERROR(Render_Vulkan, "{} failed to load preview {}", Tag,
                  state.file_preview_path);
        return false;
    }

    if (preview_image.image || preview_staging.buffer) {
        device.waitIdle();
        DestroyBuffer(preview_staging);
        DestroyImage(preview_image);
    }
    const std::size_t byte_size = static_cast<std::size_t>(width) * height * 4;
    const bool created =
        CreateImage(vk::Format::eR8G8B8A8Unorm, static_cast<u32>(width),
                    static_cast<u32>(height), preview_image) &&
        CreateBuffer(byte_size, vk::BufferUsageFlagBits::eTransferSrc,
                     vk::MemoryPropertyFlagBits::eHostVisible |
                         vk::MemoryPropertyFlagBits::eHostCoherent,
                     preview_staging, true);
    if (!created) {
        stbi_image_free(pixels);
        DestroyBuffer(preview_staging);
        DestroyImage(preview_image);
        preview_active = false;
        return false;
    }
    std::memcpy(preview_staging.mapped, pixels, byte_size);
    armDCacheFlush(preview_staging.mapped, byte_size);
    stbi_image_free(pixels);

    const vk::DescriptorImageInfo preview_info{
        .sampler = gradient_sampler,
        .imageView = preview_image.view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write{
        .dstSet = preview_descriptor_set,
        .dstBinding = 1,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &preview_info,
    };
    device.updateDescriptorSets(1, &write, 0, nullptr);
    preview_width = static_cast<u32>(width);
    preview_height = static_cast<u32>(height);
    preview_uploaded = false;
    preview_active = true;
    loaded_preview_path = state.file_preview_path;
    return true;
}

void UploadTextures(vk::CommandBuffer command_buffer) {
    if (!atlas_uploaded) {
        UploadImage(command_buffer, atlas_staging.buffer, atlas_image.image, AtlasWidth, AtlasHeight);
        UploadImage(command_buffer, gradient_staging.buffer, gradient_image.image,
                    GradientWidth, GradientHeight);
        atlas_uploaded = true;
    }
    if (overlay_active && overlay_image.image && !overlay_uploaded) {
        UploadImage(command_buffer, overlay_staging.buffer, overlay_image.image,
                    overlay_width, overlay_height);
        overlay_uploaded = true;
    }
    if (preview_active && preview_image.image && !preview_uploaded) {
        UploadImage(command_buffer, preview_staging.buffer, preview_image.image,
                    preview_width, preview_height);
        preview_uploaded = true;
    }
}

void AddQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
             const std::array<float, 4>& color, float textured) {
    const std::array quad{
        Vertex{x0, y0, u0, v0, color[0], color[1], color[2], color[3], textured},
        Vertex{x1, y0, u1, v0, color[0], color[1], color[2], color[3], textured},
        Vertex{x1, y1, u1, v1, color[0], color[1], color[2], color[3], textured},
        Vertex{x0, y0, u0, v0, color[0], color[1], color[2], color[3], textured},
        Vertex{x1, y1, u1, v1, color[0], color[1], color[2], color[3], textured},
        Vertex{x0, y1, u0, v1, color[0], color[1], color[2], color[3], textured},
    };
    vertices.insert(vertices.end(), quad.begin(), quad.end());
}

void AddQuad4(const std::array<float, 2>& p0, const std::array<float, 2>& p1,
              const std::array<float, 2>& p2, const std::array<float, 2>& p3,
              const std::array<float, 2>& uv0, const std::array<float, 2>& uv1,
              const std::array<float, 2>& uv2, const std::array<float, 2>& uv3,
              const std::array<float, 4>& color, float textured) {
    const std::array quad{
        Vertex{p0[0], p0[1], uv0[0], uv0[1], color[0], color[1], color[2], color[3], textured},
        Vertex{p1[0], p1[1], uv1[0], uv1[1], color[0], color[1], color[2], color[3], textured},
        Vertex{p3[0], p3[1], uv3[0], uv3[1], color[0], color[1], color[2], color[3], textured},
        Vertex{p0[0], p0[1], uv0[0], uv0[1], color[0], color[1], color[2], color[3], textured},
        Vertex{p3[0], p3[1], uv3[0], uv3[1], color[0], color[1], color[2], color[3], textured},
        Vertex{p2[0], p2[1], uv2[0], uv2[1], color[0], color[1], color[2], color[3], textured},
    };
    vertices.insert(vertices.end(), quad.begin(), quad.end());
}

void Rect(float x, float y, float width, float height, const std::array<float, 4>& color) {
    AddQuad(x, y, x + width, y + height, 0, 0, 0, 0, color, 0.0f);
}

void Border(float x, float y, float width, float height, float thickness,
            const std::array<float, 4>& color) {
    Rect(x, y, width, thickness, color);
    Rect(x, y + height - thickness, width, thickness, color);
    Rect(x, y, thickness, height, color);
    Rect(x + width - thickness, y, thickness, height, color);
}

void FlowBorder(float x, float y, float width, float height, float requested_width = 3.0f) {
    const float border_width = std::max(4.0f, requested_width * 2.0f);
    const double milliseconds =
        static_cast<double>(armTicksToNs(armGetSystemTick())) / 1'000'000.0;
    float uv = static_cast<float>(std::fmod(milliseconds / 3600.0, 1.0));
    const float top_length = width + border_width * 2.0f;
    const float side_length = height;
    const auto advance = [](float length) { return length / static_cast<float>(GradientWidth); };
    constexpr std::array<float, 4> tint{1.0f, 1.0f, 1.0f, 0.98f};

    float next = uv + advance(top_length);
    AddQuad4({x - border_width, y - border_width},
             {x + width + border_width, y - border_width},
             {x - border_width, y}, {x + width + border_width, y},
             {uv, 0}, {next, 0}, {uv, 1}, {next, 1}, tint, 2.0f);
    uv = next;

    next = uv + advance(side_length);
    AddQuad4({x + width, y}, {x + width + border_width, y},
             {x + width, y + height}, {x + width + border_width, y + height},
             {uv, 0}, {uv, 1}, {next, 0}, {next, 1}, tint, 2.0f);
    uv = next;

    next = uv + advance(top_length);
    AddQuad4({x - border_width, y + height}, {x + width + border_width, y + height},
             {x - border_width, y + height + border_width},
             {x + width + border_width, y + height + border_width},
             {next, 0}, {uv, 0}, {next, 1}, {uv, 1}, tint, 2.0f);
    uv = next;

    next = uv + advance(side_length);
    AddQuad4({x - border_width, y}, {x, y},
             {x - border_width, y + height}, {x, y + height},
             {next, 0}, {next, 1}, {uv, 0}, {uv, 1}, tint, 2.0f);
}

float MeasureText(std::string_view text, float size) {
    const float scale = size / PackedFontSize;
    float width = 0.0f;
    for (int cp : DecodeUtf8(text)) {
        const auto it = glyph_indices.find(cp);
        if (it != glyph_indices.end()) {
            width += packed_chars[it->second].xadvance * scale;
        }
    }
    return width;
}

void Text(float x, float baseline, float size, const std::array<float, 4>& color,
          std::string_view text) {
    const float scale = size / PackedFontSize;
    float cursor = 0.0f;
    for (int cp : DecodeUtf8(text)) {
        const auto it = glyph_indices.find(cp);
        if (it == glyph_indices.end()) {
            continue;
        }
        float qx = 0.0f;
        float qy = 0.0f;
        stbtt_aligned_quad q{};
        stbtt_GetPackedQuad(packed_chars.data(), AtlasWidth, AtlasHeight,
                            static_cast<int>(it->second), &qx, &qy, &q, 1);
        AddQuad(x + cursor + q.x0 * scale, baseline + q.y0 * scale,
                x + cursor + q.x1 * scale, baseline + q.y1 * scale,
                q.s0, q.t0, q.s1, q.t1, color, 1.0f);
        cursor += packed_chars[it->second].xadvance * scale;
    }
}

void TextRight(float right, float baseline, float size, const std::array<float, 4>& color,
               std::string_view text) {
    Text(right - MeasureText(text, size), baseline, size, color, text);
}

void Icon(float center_x, float baseline, float size, const std::array<float, 4>& color,
          int codepoint) {
    const auto it = glyph_indices.find(codepoint);
    if (it == glyph_indices.end()) {
        return;
    }
    const float scale = size / PackedFontSize;
    float qx = 0.0f;
    float qy = 0.0f;
    stbtt_aligned_quad q{};
    stbtt_GetPackedQuad(packed_chars.data(), AtlasWidth, AtlasHeight,
                        static_cast<int>(it->second), &qx, &qy, &q, 1);
    const float width = (q.x1 - q.x0) * scale;
    AddQuad(center_x - width * 0.5f, baseline + q.y0 * scale,
            center_x + width * 0.5f, baseline + q.y1 * scale,
            q.s0, q.t0, q.s1, q.t1, color, 1.0f);
}

void IconCentered(float center_x, float center_y, float size,
                  const std::array<float, 4>& color, int codepoint) {
    const auto it = glyph_indices.find(codepoint);
    if (it == glyph_indices.end()) return;
    const float scale = size / PackedFontSize;
    float qx = 0.0f;
    float qy = 0.0f;
    stbtt_aligned_quad q{};
    stbtt_GetPackedQuad(packed_chars.data(), AtlasWidth, AtlasHeight,
                        static_cast<int>(it->second), &qx, &qy, &q, 1);
    const float width = (q.x1 - q.x0) * scale;
    const float height = (q.y1 - q.y0) * scale;
    AddQuad(center_x - width * 0.5f, center_y - height * 0.5f,
            center_x + width * 0.5f, center_y + height * 0.5f,
            q.s0, q.t0, q.s1, q.t1, color, 1.0f);
}

void SelectorValue(float row_x, float row_y, float row_w, float row_h,
                   std::string_view value,
                   const std::array<float, 4>& color) {
    const float center_y = row_y + row_h * 0.5f;
    IconCentered(row_x + row_w - 194.0f, center_y, 26.0f, color, NintendoIconL);
    Text(row_x + row_w - 110.0f - MeasureText(value, 18.0f) * 0.5f,
         row_y + row_h * 0.5f + 7.0f, 18.0f, color, value);
    IconCentered(row_x + row_w - 24.0f, center_y, 26.0f, color, NintendoIconR);
}

std::string Filename(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string_view::npos ? std::string{path}
                                           : std::string{path.substr(slash + 1)};
}

void DrawToast(const State& state) {
    if (state.toast.empty()) {
        return;
    }
    constexpr float Right = 1210.0f;
    const float width = std::min(520.0f, MeasureText(state.toast, 18.0f) + 48.0f);
    const float x = Right - width;
    Rect(x, 626, width, 48, {0.02f, 0.025f, 0.035f, 0.94f});
    Border(x, 626, width, 48, 1.0f, {0.31f, 0.70f, 1.0f, 0.72f});
    Text(x + 22, 657, 18, {0.94f, 0.97f, 1.0f, 0.96f}, state.toast);
}

void DrawFastForwardIndicator(const State& state) {
    if (!state.fast_forward_active) {
        return;
    }
    IconCentered(34, 34, 28, {0.44f, 0.80f, 1.0f, 1.0f}, 0xE01F);
    char value[20]{};
    std::snprintf(value, sizeof(value), "%.1fx", state.display.fast_forward_multiplier);
    Text(54, 41, 18, {0.94f, 0.97f, 1.0f, 0.94f}, value);
}

void DrawFpsIndicator(const State& state) {
    if (!state.show_fps || state.current_fps <= 0.0f) {
        return;
    }
    char value[24]{};
    std::snprintf(value, sizeof(value), "FPS: %.1f", state.current_fps);
    Text(12, 30, 18, {0.20f, 1.0f, 0.24f, 0.96f}, value);
}

const char* DisplayLayoutLabel(std::string_view layout) {
    if (layout == "vertical")
        return "竖向";
    if (layout == "horizontal")
        return "横向";
    if (layout == "hybrid")
        return "混合";
    if (layout == "top")
        return "仅上屏";
    if (layout == "bottom")
        return "仅下屏";
    if (layout == "custom")
        return "自定义";
    return "上屏优先";
}

void DrawCustomLayoutSidebar(const State& state) {
    constexpr std::array<float, 4> White{0.94f, 0.97f, 1.0f, 1.0f};
    constexpr std::array<float, 4> Muted{0.72f, 0.80f, 0.88f, 0.78f};
    constexpr std::array<float, 4> Cyan{0.44f, 0.80f, 1.0f, 1.0f};
    constexpr float PanelX = 800.0f;
    constexpr float PanelW = 480.0f;
    constexpr float RowX = 830.0f;
    constexpr float RowW = 420.0f;
    constexpr float RowH = 52.0f;

    Rect(0, 0, 1280, 720, {0.0f, 0.0f, 0.0f, 0.16f});
    Rect(PanelX, 0, PanelW, 720, {0.015f, 0.020f, 0.030f, 0.52f});
    Rect(PanelX, 0, 1, 720, {1.0f, 1.0f, 1.0f, 0.18f});
    IconCentered(850, 47, 27, Cyan, 0xE3C9);
    Text(878, 54, 27, White, "自定义画面布局");
    Text(830, 87, 16, Muted, "L/R 调整   A 重置当前项   B 保存返回");

    auto section = [&](float y, const char* label) {
        Rect(830, y + 12, 72, 1, {1.0f, 1.0f, 1.0f, 0.13f});
        Text(916, y + 19, 18, Cyan, label);
        Rect(1030, y + 12, 220, 1, {1.0f, 1.0f, 1.0f, 0.13f});
    };
    auto row = [&](int index, float y, const char* label, std::string_view value) {
        const bool focused = state.custom_layout_focus == index;
        Rect(RowX, y, RowW, RowH,
             focused ? std::array<float, 4>{0.0f, 0.30f, 0.50f, 0.52f}
                     : std::array<float, 4>{1.0f, 1.0f, 1.0f, 0.045f});
        if (focused) {
            FlowBorder(RowX, y, RowW, RowH, 3.0f);
        } else {
            Border(RowX, y, RowW, RowH, 1.0f, {1.0f, 1.0f, 1.0f, 0.10f});
        }
        Text(RowX + 18, y + 34, 19, White, label);
        SelectorValue(RowX, y, RowW, RowH, value, Cyan);
    };
    auto scaleValue = [](float value) {
        char text[24]{};
        std::snprintf(text, sizeof(text), "%.1fx", value);
        return std::string{text};
    };
    auto offsetValue = [](float value) {
        char text[24]{};
        std::snprintf(text, sizeof(text), "%.0f px", value);
        return std::string{text};
    };
    auto opacityValue = [](float value) {
        char text[24]{};
        std::snprintf(text, sizeof(text), "%.0f%%", std::clamp(value, 0.0f, 1.0f) * 100.0f);
        return std::string{text};
    };

    section(116, "上屏布局");
    row(0, 150, "大小", scaleValue(state.display.top_scale));
    row(1, 210, "X 偏移", offsetValue(state.display.top_offset_x));
    row(2, 270, "Y 偏移", offsetValue(state.display.top_offset_y));
    section(350, "下屏布局");
    row(3, 384, "大小", scaleValue(state.display.bottom_scale));
    row(4, 444, "X 偏移", offsetValue(state.display.bottom_offset_x));
    row(5, 504, "Y 偏移", offsetValue(state.display.bottom_offset_y));
    row(6, 564, "透明度", opacityValue(state.display.bottom_opacity));
}

void DrawShaderCompileIndicator(const State& state) {
    if (!VideoCore::GetShaderCompileNoticeState()) {
        return;
    }
    const u32 pending = VideoCore::GetPendingShaderCompiles();
    if (pending == 0) {
        return;
    }
    char value[64]{};
    std::snprintf(value, sizeof(value), "正在编译着色器 %u", pending);
    const float width = std::min(330.0f, MeasureText(value, 18.0f) + 34.0f);
    Rect(12, 42, width, 38, {0.0f, 0.0f, 0.0f, 0.64f});
    Border(12, 42, width, 38, 1.0f, {0.31f, 0.70f, 1.0f, 0.44f});
    Text(28, 68, 18, {0.72f, 0.88f, 1.0f, 0.98f}, value);
}

std::string CompactPathForWidth(std::string_view path, float max_width, float size) {
    if (path.empty()) {
        return "未选择";
    }
    if (MeasureText(path, size) <= max_width) {
        return std::string{path};
    }
    const std::string name = Filename(path);
    const std::string shortened = ".../" + name;
    if (MeasureText(shortened, size) <= max_width) {
        return shortened;
    }
    std::string text = shortened;
    while (text.size() > 4 && MeasureText(text, size) > max_width) {
        text.erase(text.begin() + 3);
    }
    return text;
}

void DrawOverlaySidebar(const State& state) {
    constexpr std::array<float, 4> White{0.94f, 0.97f, 1.0f, 1.0f};
    constexpr std::array<float, 4> Muted{0.72f, 0.80f, 0.88f, 0.72f};
    constexpr std::array<float, 4> Cyan{0.44f, 0.80f, 1.0f, 1.0f};
    constexpr float PanelX = 800.0f;
    constexpr float RowX = 830.0f;
    constexpr float RowW = 420.0f;
    Rect(0, 0, 1280, 720, {0.0f, 0.0f, 0.0f, 0.16f});
    Rect(PanelX, 0, 480, 720, {0.015f, 0.020f, 0.030f, 0.52f});
    Rect(PanelX, 0, 1, 720, {1.0f, 1.0f, 1.0f, 0.18f});
    IconCentered(850, 47, 27, Cyan, 0xE53B);
    Text(878, 54, 27, White, "遮罩设置");
    Text(830, 88, 16, Muted, "选择 PNG 遮罩文件");
    const std::array<const char*, 2> labels{{"遮罩开关", "遮罩选择"}};
    const std::array<std::string, 2> values{{
        state.display.overlay_enabled ? "开启" : "关闭",
        CompactPathForWidth(state.display.overlay_path, 214.0f, 17.0f),
    }};
    for (int row = 0; row < 2; ++row) {
        const float y = 132.0f + row * 66.0f;
        const bool focused = state.overlay_focus == row;
        Rect(RowX, y, RowW, 54,
             focused ? std::array<float, 4>{0.0f, 0.30f, 0.50f, 0.52f}
                     : std::array<float, 4>{1, 1, 1, 0.045f});
        if (focused)
            FlowBorder(RowX, y, RowW, 54, 3.0f);
        else
            Border(RowX, y, RowW, 54, 1.0f, {1, 1, 1, 0.10f});
        Text(RowX + 18, y + 35, 20, White, labels[row]);
        TextRight(RowX + RowW - (row == 1 ? 48.0f : 18.0f), y + 34, 17, Cyan, values[row]);
        if (row == 1)
            IconCentered(RowX + RowW - 20, y + 27, 23, Cyan, 0xE5CC);
    }
    IconCentered(1030, 672, 27, Muted, NintendoIconB);
    Text(1052, 681, 18, Muted, "返回并保存");
}

std::string FormatBytes(u64 bytes) {
    char text[32]{};
    if (bytes >= 1024 * 1024) {
        std::snprintf(text, sizeof(text), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(text, sizeof(text), "%llu B",
                      static_cast<unsigned long long>(bytes));
    }
    return text;
}

bool IsPngPath(std::string_view path) {
    if (path.size() < 4) return false;
    const std::string_view suffix = path.substr(path.size() - 4);
    return (suffix[0] == '.') && (suffix[1] == 'p' || suffix[1] == 'P') &&
           (suffix[2] == 'n' || suffix[2] == 'N') &&
           (suffix[3] == 'g' || suffix[3] == 'G');
}

void DrawFilePicker(const State& state) {
    constexpr std::array<float, 4> White{0.94f, 0.97f, 1.0f, 1.0f};
    constexpr std::array<float, 4> Muted{0.72f, 0.80f, 0.88f, 0.72f};
    constexpr std::array<float, 4> Cyan{0.44f, 0.80f, 1.0f, 1.0f};
    constexpr float TopH = 96.0f;
    constexpr float FooterH = 96.0f;
    constexpr float BodyY = 114.0f;
    constexpr float BodyH = 492.0f;
    constexpr float RowH = 84.0f;

    Rect(0, 0, 1280, 720, {0.010f, 0.014f, 0.020f, 0.68f});
    Rect(0, 0, 1280, TopH, {0.0f, 0.0f, 0.0f, 0.34f});
    Rect(0, TopH, 1280, 1, {1, 1, 1, 0.12f});
    Text(32, 61, 24, White, state.file_picker_path.empty() ? "/" : state.file_picker_path);

    const int count = static_cast<int>(state.file_entries.size());
    const int focus = count == 0 ? 0 : std::clamp(state.file_picker_focus, 0, count - 1);
    char index_text[48]{};
    std::snprintf(index_text, sizeof(index_text), "%d / %d", count == 0 ? 0 : focus + 1, count);
    TextRight(1246, 61, 23, Muted, index_text);

    constexpr int VisibleRows = 6;
    const int first = std::clamp(focus - VisibleRows / 2, 0,
                                 std::max(0, count - VisibleRows));
    if (count == 0) {
        Text(48, 180, 22, Muted, "目录中暂无可用 PNG 文件");
    }
    for (int row = 0; row < std::min(VisibleRows, count - first); ++row) {
        const int index = first + row;
        const auto& entry = state.file_entries[index];
        const float y = BodyY + row * RowH;
        const bool focused = index == focus;
        if (focused) {
            FlowBorder(30, y + 7, 1220, RowH - 14, 3.0f);
        } else {
            Rect(30, y + 8, 1220, RowH - 16, {1, 1, 1, 0.025f});
        }
        IconCentered(78, y + RowH * 0.5f, 39,
             entry.directory ? std::array<float, 4>{0.50f, 0.78f, 1.0f, 0.92f}
                             : std::array<float, 4>{0.66f, 0.90f, 0.74f, 0.88f},
             entry.directory ? 0xE2C7 : 0xE3F4);
        Text(128, y + 38, 27, focused ? White : std::array<float, 4>{1, 1, 1, 0.78f},
             entry.name);
        std::string meta;
        if (entry.directory) {
            meta = entry.name == ".." ? "上级目录" : "文件夹";
        } else {
            meta = FormatBytes(entry.size);
            if (!entry.modified_time.empty()) meta += "   " + entry.modified_time;
        }
        Text(128, y + 65, 17, focused ? Muted : std::array<float, 4>{0.72f, 0.82f, 0.90f, 0.50f},
             meta);
    }

    const bool selected_image = count > 0 && !state.file_entries[focus].directory &&
                                IsPngPath(state.file_entries[focus].path);
    if (state.file_preview) {
        Rect(0, 0, 1280, 720, {0.0f, 0.0f, 0.0f, 0.82f});
        if (preview_active && preview_width > 0 && preview_height > 0) {
            constexpr float MaxW = 1160.0f;
            constexpr float MaxH = 504.0f;
            const float aspect = static_cast<float>(preview_width) /
                                 static_cast<float>(preview_height);
            float draw_w = MaxW;
            float draw_h = draw_w / aspect;
            if (draw_h > MaxH) {
                draw_h = MaxH;
                draw_w = draw_h * aspect;
            }
            const float draw_x = (1280.0f - draw_w) * 0.5f;
            const float draw_y = 76.0f + (MaxH - draw_h) * 0.5f;
            Rect(draw_x - 12, draw_y - 12, draw_w + 24, draw_h + 24,
                 {1, 1, 1, 0.055f});
            preview_vertex_first = static_cast<u32>(vertices.size());
            AddQuad(draw_x, draw_y, draw_x + draw_w, draw_y + draw_h,
                    0, 0, 1, 1, {1, 1, 1, 1}, 2.0f);
            preview_vertex_count = 6;
        } else {
            Text(498, 350, 22, Muted, "图片预览加载失败");
        }
        Text(60, 61, 24, White,
             state.file_preview_path.empty() ? "图片预览" : Filename(state.file_preview_path));
    }

    const float footer_y = 720.0f - FooterH;
    Rect(0, footer_y, 1280, FooterH, {0.0f, 0.0f, 0.0f, 0.34f});
    Rect(0, footer_y, 1280, 1, {1, 1, 1, 0.12f});
    float right = 1244.0f;
    auto hint = [&](int icon, const char* label, const std::array<float, 4>& color, float width) {
        right -= width;
        IconCentered(right + 19, footer_y + 48, 34, {1, 1, 1, 0.92f}, icon);
        Text(right + 44, footer_y + 57, 26, color, label);
        right -= 34.0f;
    };
    hint(NintendoIconA, state.file_preview ? "关闭" : "选择", Cyan, 112.0f);
    hint(NintendoIconB, state.file_preview ? "关闭" : "返回", White, 112.0f);
    if (!state.file_preview && selected_image) {
        hint(NintendoIconX, "预览", Muted, 112.0f);
    }
}

void BuildMenu(const State& state) {
    if (state.file_picker) {
        DrawFilePicker(state);
        DrawToast(state);
        return;
    }
    if (state.overlay_sidebar) {
        DrawOverlaySidebar(state);
        DrawToast(state);
        return;
    }
    if (state.custom_layout_sidebar) {
        DrawCustomLayoutSidebar(state);
        DrawToast(state);
        return;
    }
    constexpr std::array<float, 4> White{0.94f, 0.97f, 1.0f, 1.0f};
    constexpr std::array<float, 4> Muted{0.72f, 0.80f, 0.88f, 0.78f};
    constexpr std::array<float, 4> Cyan{0.44f, 0.80f, 1.0f, 1.0f};

    Rect(0, 0, 1280, 720, {0.0f, 0.0f, 0.0f, 0.16f});
    Rect(0, 0, 408, 720, {0.015f, 0.020f, 0.030f, 0.58f});
    Rect(408, 0, 872, 720, {0.015f, 0.020f, 0.030f, 0.46f});
    Rect(408, 0, 1, 720, {1.0f, 1.0f, 1.0f, 0.18f});
    IconCentered(68, 47, 27, Cyan, 0xE5D2);
    Text(94, 56, 27, White, "游戏菜单");
    Rect(48, 92, 336, 1, {1, 1, 1, 0.14f});

    constexpr float LeftX = 48.0f;
    constexpr float LeftY = 116.0f;
    constexpr float MenuW = 336.0f;
    constexpr float ItemH = 70.0f;
    constexpr float Step = 80.0f;
    const int selected =
        std::clamp(static_cast<int>(state.item), 0, static_cast<int>(Item::Count) - 1);
    for (int i = 0; i < static_cast<int>(Item::Count); ++i) {
        const float y = LeftY + i * Step;
        const bool focused = i == selected;
        Rect(LeftX, y, MenuW, ItemH,
             focused ? (state.content_focused ? std::array<float, 4>{0.13f, 0.42f, 0.70f, 0.20f}
                                              : std::array<float, 4>{0.00f, 0.30f, 0.50f, 0.52f})
                     : std::array<float, 4>{1.0f, 1.0f, 1.0f, 0.035f});
        if (focused) {
            if (state.content_focused) {
                Border(LeftX, y, MenuW, ItemH, 1.0f, {0.31f, 0.70f, 1.0f, 0.50f});
            } else {
                FlowBorder(LeftX, y, MenuW, ItemH, 3.0f);
            }
        } else {
            Border(LeftX, y, MenuW, ItemH, 1.0f, {1.0f, 1.0f, 1.0f, 0.08f});
        }
        IconCentered(LeftX + 34, y + ItemH * 0.5f, 25, focused ? White : Muted, ItemIcons[i]);
        Text(LeftX + 64, y + 44, 22, focused ? White : Muted, ItemLabels[i]);
    }
    Rect(LeftX + 18, LeftY + 5 * Step - 14, MenuW - 36, 1, {1, 1, 1, 0.14f});
    Rect(408, 92, 872, 1, {1, 1, 1, 0.14f});

    constexpr float ContentX = 432.0f;
    constexpr float ContentY = 110.0f;
    constexpr float ContentW = 790.0f;
    const Item item = static_cast<Item>(selected);
    Text(ContentX, ContentY + 28, 24, White, ItemLabels[selected]);
    Rect(ContentX, ContentY + 50, ContentW, 1, {0.0f, 0.48f, 0.80f, 0.28f});

    if (item == Item::SaveState || item == Item::LoadState) {
        const float row_h = 42.0f;
        const float row_gap = 4.0f;
        for (int slot = 0; slot < 10; ++slot) {
            const float y = 176.0f + slot * (row_h + row_gap);
            const bool focused = state.content_focused && state.content_focus == slot;
            Rect(ContentX, y, 520, row_h,
                 focused ? std::array<float, 4>{0.0f, 0.30f, 0.50f, 0.52f}
                         : std::array<float, 4>{1.0f, 1.0f, 1.0f, 0.045f});
            if (focused) {
                FlowBorder(ContentX, y, 520, row_h, 3.0f);
            } else {
                Border(ContentX, y, 520, row_h, 1.0f, {1.0f, 1.0f, 1.0f, 0.10f});
            }
            char slot_name[32]{};
            std::snprintf(slot_name, sizeof(slot_name), "档位 %d", slot + 1);
            Text(ContentX + 16, y + 28, 20, White, slot_name);
            const char* status = state.occupied[slot] ? "已有状态" : "空存档槽";
            TextRight(ContentX + 500, y + 27, 16, state.occupied[slot] ? Cyan : Muted, status);
        }
        Rect(986, 176, 220, 420, {1.0f, 1.0f, 1.0f, 0.035f});
        Border(976, 166, 240, 440, 1, {1.0f, 1.0f, 1.0f, 0.10f});
        Text(1020, 380, 24, Muted, "NO THUMB");
    } else if (item == Item::Display) {
        const std::array<const char*, 10> labels{{
            "快进倍率",
            "3D分辨率",
            "整数倍缩放",
            "屏幕布局",
            "自定义画面布局",
            "画面方向",
            "屏幕间距",
            "遮罩选择",
            "同步遮罩",
            "同步画面设置",
        }};
        const std::array<int, 10> icons{{
            0xE01F,
            0xE433,
            0xE3F4,
            0xE8F1,
            0xE3C9,
            0xE41A,
            0xE8D4,
            0xE53B,
            0xE873,
            0xE873,
        }};
        const bool custom_enabled = state.display.screen_layout == "custom";
        std::array<std::string, 10> values{};
        char multiplier[24]{};
        std::snprintf(multiplier, sizeof(multiplier),
                      std::fabs(state.display.fast_forward_multiplier -
                                std::round(state.display.fast_forward_multiplier)) < 0.01f
                          ? "%.0fx"
                          : "%.2fx",
                      state.display.fast_forward_multiplier);
        values[0] = multiplier;
        values[1] = std::to_string(state.display.internal_resolution) + "x";
        values[2] = state.display.integer_scale ? "开启" : "关闭";
        values[3] = DisplayLayoutLabel(state.display.screen_layout);
        values[4] = custom_enabled ? "调整" : "不可用";
        values[5] = std::to_string(state.display.screen_orientation) + "°";
        values[6] = std::to_string(state.display.screen_gap) + " px";
        values[7] = state.display.overlay_enabled ? "已开启" : "设置";
        values[8] = "执行";
        values[9] = "执行";
        constexpr float RowH = 40.0f;
        constexpr std::array<float, 10> RowY{{178, 222, 266, 330, 374, 418, 462, 526, 570, 614}};
        Text(ContentX, 164, 16, Cyan, "基础画面设置");
        Text(ContentX, 316, 16, Cyan, "布局设置");
        Text(ContentX, 512, 16, Cyan, "个性化设置");
        for (int row = 0; row < 10; ++row) {
            const float y = RowY[row];
            const bool focused = state.content_focused && state.content_focus == row;
            const bool enabled = row != 4 || custom_enabled;
            Rect(ContentX, y, ContentW, RowH,
                 focused ? std::array<float, 4>{0.0f, 0.30f, 0.50f, 0.52f}
                         : std::array<float, 4>{1, 1, 1, 0.045f});
            if (focused) {
                FlowBorder(ContentX, y, ContentW, RowH, 3.0f);
            } else {
                Border(ContentX, y, ContentW, RowH, 1.0f, {1, 1, 1, 0.10f});
            }
            IconCentered(ContentX + 24, y + RowH * 0.5f, 20, enabled ? Cyan : Muted, icons[row]);
            Text(ContentX + 46, y + 30, 18, enabled ? White : Muted, labels[row]);
            if (row == 0 || row == 1 || row == 3 || row == 5 || row == 6) {
                SelectorValue(ContentX, y, ContentW, RowH, values[row], enabled ? Cyan : Muted);
            } else {
                const bool is_button = row == 4 || row == 7 || row == 8 || row == 9;
                TextRight(ContentX + ContentW - (is_button ? 46.0f : 18.0f), y + 29, 17,
                          enabled ? Cyan : Muted, values[row]);
                if (is_button) {
                    IconCentered(ContentX + ContentW - 20, y + RowH * 0.5f, 20,
                                 enabled ? Cyan : Muted, 0xE5CC);
                }
            }
        }
    } else if (item == Item::Cheats) {
        Rect(ContentX, 184, ContentW, 58, {1, 1, 1, 0.045f});
        Border(ContentX, 184, ContentW, 58, 1, {1, 1, 1, 0.10f});
        Text(ContentX + 20, 221, 21, Muted, "暂无金手指");
        Text(ContentX, 300, 20, Muted, "金手指功能将在后续版本提供");
    } else {
        const char* body = "按 A 继续游戏";
        if (item == Item::Reset) body = "按 A 重置游戏，未保存的进度可能丢失";
        if (item == Item::Exit) body = "按 A 安全关闭模拟器并返回启动器";
        Text(ContentX, 260, 26, {0.80f, 0.90f, 0.98f, 0.86f}, body);
    }

    IconCentered(1020, 678, 27, Muted, NintendoIconB);
    Text(1042, 687, 19, Muted, state.content_focused ? "返回列表" : "返回");
    IconCentered(1152, 678, 27, Muted, NintendoIconA);
    Text(1174, 687, 19, Muted, "确定");
    DrawToast(state);
}

void TransformVertices(vk::Extent2D extent) {
    for (Vertex& vertex : vertices) {
        vertex.x = vertex.x / 1280.0f * 2.0f - 1.0f;
        vertex.y = vertex.y / 720.0f * 2.0f - 1.0f;
    }
    const std::size_t bytes = vertices.size() * sizeof(Vertex);
    if (bytes <= VertexBufferSize) {
        std::memcpy(vertex_buffer.mapped, vertices.data(), bytes);
        armDCacheFlush(vertex_buffer.mapped, bytes);
    }
}

} // namespace

bool Init(const Vulkan::Instance& instance) {
    if (initialized) {
        return true;
    }
    physical_device = instance.GetPhysicalDevice();
    device = instance.GetDevice();
    memory_properties = physical_device.getMemoryProperties();
    if (!device || !BuildFontAtlas() || !LoadGradientTexture() || !CreateShaders() ||
        !CreateImage(vk::Format::eR8Unorm, AtlasWidth, AtlasHeight, atlas_image) ||
        !CreateImage(vk::Format::eR8G8B8A8Unorm, GradientWidth, GradientHeight,
                     gradient_image)) {
        Shutdown();
        return false;
    }
    if (!CreateBuffer(atlas_pixels.size(), vk::BufferUsageFlagBits::eTransferSrc,
                      vk::MemoryPropertyFlagBits::eHostVisible |
                          vk::MemoryPropertyFlagBits::eHostCoherent,
                      atlas_staging, true) ||
        !CreateBuffer(VertexBufferSize, vk::BufferUsageFlagBits::eVertexBuffer,
                      vk::MemoryPropertyFlagBits::eHostVisible |
                          vk::MemoryPropertyFlagBits::eHostCoherent,
                      vertex_buffer, true) ||
        !CreateBuffer(gradient_pixels.size(), vk::BufferUsageFlagBits::eTransferSrc,
                      vk::MemoryPropertyFlagBits::eHostVisible |
                          vk::MemoryPropertyFlagBits::eHostCoherent,
                      gradient_staging, true) ||
        !CreateDescriptors()) {
        Shutdown();
        return false;
    }
    std::memcpy(atlas_staging.mapped, atlas_pixels.data(), atlas_pixels.size());
    armDCacheFlush(atlas_staging.mapped, atlas_pixels.size());
    std::memcpy(gradient_staging.mapped, gradient_pixels.data(), gradient_pixels.size());
    armDCacheFlush(gradient_staging.mapped, gradient_pixels.size());
    atlas_uploaded = false;
    initialized = true;
    LOG_INFO(Render_Vulkan, "{} initialized glyphs={}", Tag, codepoints.size());
    return true;
}

void Draw(vk::CommandBuffer command_buffer, vk::Image image, vk::Extent2D extent, vk::Format format,
          const State& state) {
    if (!initialized || extent.width == 0 || extent.height == 0 || !EnsureRenderObjects(format)) {
        return;
    }
    EnsureOverlayTexture(state);
    EnsurePreviewTexture(state);
    UploadTextures(command_buffer);
    vertices.clear();
    overlay_vertex_count = 0;
    preview_vertex_first = 0;
    preview_vertex_count = 0;
    if (overlay_active) {
        AddQuad(0, 0, 1280, 720, 0, 0, 1, 1, {1, 1, 1, 1}, 2.0f);
        overlay_vertex_count = 6;
    }
    if (state.menu_visible) {
        BuildMenu(state);
    } else {
        DrawToast(state);
    }
    if (overlay_active) {
        DrawFpsIndicator(state);
        DrawShaderCompileIndicator(state);
    }
    DrawFastForwardIndicator(state);
    TransformVertices(extent);
    const vk::Framebuffer framebuffer = GetFramebuffer(image, extent);
    if (!framebuffer || vertices.empty() || vertices.size() * sizeof(Vertex) > VertexBufferSize) {
        return;
    }

    const vk::RenderPassBeginInfo begin{
        .renderPass = render_pass,
        .framebuffer = framebuffer,
        .renderArea{{0, 0}, extent},
    };
    command_buffer.beginRenderPass(begin, vk::SubpassContents::eInline);
    const vk::Viewport viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                static_cast<float>(extent.height), 0.0f, 1.0f};
    const vk::Rect2D scissor{{0, 0}, extent};
    command_buffer.setViewport(0, viewport);
    command_buffer.setScissor(0, scissor);
    command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    const vk::DeviceSize offset = 0;
    command_buffer.bindVertexBuffers(0, vertex_buffer.buffer, offset);
    if (overlay_vertex_count != 0) {
        command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0,
                                          overlay_descriptor_set, {});
        command_buffer.draw(overlay_vertex_count, 1, 0, 0);
    }
    const u32 vertex_count = static_cast<u32>(vertices.size());
    const u32 normal_first = overlay_vertex_count;
    if (preview_vertex_count != 0 && preview_vertex_first >= normal_first) {
        if (preview_vertex_first > normal_first) {
            command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0,
                                              descriptor_set, {});
            command_buffer.draw(preview_vertex_first - normal_first, 1, normal_first, 0);
        }
        command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0,
                                          preview_descriptor_set, {});
        command_buffer.draw(preview_vertex_count, 1, preview_vertex_first, 0);
        const u32 trailing_first = preview_vertex_first + preview_vertex_count;
        if (vertex_count > trailing_first) {
            command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0,
                                              descriptor_set, {});
            command_buffer.draw(vertex_count - trailing_first, 1, trailing_first, 0);
        }
    } else if (vertices.size() > overlay_vertex_count) {
        command_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0,
                                          descriptor_set, {});
        command_buffer.draw(static_cast<u32>(vertices.size()) - overlay_vertex_count, 1,
                            overlay_vertex_count, 0);
    }
    command_buffer.endRenderPass();
}

void ResetSwapchain() {
    if (device) {
        DestroyRenderObjects();
    }
}

void Shutdown() {
    if (!device) {
        initialized = false;
        return;
    }
    DestroyRenderObjects();
    DestroyBuffer(vertex_buffer);
    DestroyBuffer(atlas_staging);
    DestroyBuffer(gradient_staging);
    DestroyBuffer(overlay_staging);
    DestroyBuffer(preview_staging);
    DestroyImage(atlas_image);
    DestroyImage(gradient_image);
    DestroyImage(overlay_image);
    DestroyImage(preview_image);
    if (font_sampler) device.destroySampler(font_sampler);
    if (gradient_sampler) device.destroySampler(gradient_sampler);
    if (pipeline_layout) device.destroyPipelineLayout(pipeline_layout);
    if (descriptor_pool) device.destroyDescriptorPool(descriptor_pool);
    if (descriptor_set_layout) device.destroyDescriptorSetLayout(descriptor_set_layout);
    if (vertex_shader) device.destroyShaderModule(vertex_shader);
    if (fragment_shader) device.destroyShaderModule(fragment_shader);
    font_sampler = VK_NULL_HANDLE;
    gradient_sampler = VK_NULL_HANDLE;
    pipeline_layout = VK_NULL_HANDLE;
    descriptor_pool = VK_NULL_HANDLE;
    descriptor_set_layout = VK_NULL_HANDLE;
    vertex_shader = VK_NULL_HANDLE;
    fragment_shader = VK_NULL_HANDLE;
    descriptor_set = VK_NULL_HANDLE;
    overlay_descriptor_set = VK_NULL_HANDLE;
    preview_descriptor_set = VK_NULL_HANDLE;
    font_data.clear();
    nintendo_font_data.clear();
    material_font_data.clear();
    atlas_pixels.clear();
    gradient_pixels.clear();
    codepoints.clear();
    packed_chars.clear();
    glyph_indices.clear();
    vertices.clear();
    atlas_uploaded = false;
    overlay_uploaded = false;
    overlay_active = false;
    overlay_width = 0;
    overlay_height = 0;
    overlay_vertex_count = 0;
    loaded_overlay_path.clear();
    preview_uploaded = false;
    preview_active = false;
    preview_width = 0;
    preview_height = 0;
    preview_vertex_first = 0;
    preview_vertex_count = 0;
    loaded_preview_path.clear();
    initialized = false;
    device = VK_NULL_HANDLE;
}

} // namespace SwitchFrontend::VulkanMenuRenderer
