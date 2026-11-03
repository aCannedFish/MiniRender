#include "gpu_image.hpp"
#include "gpu_buffer.hpp"

#include <stdexcept>

GpuImage::~GpuImage() {
    release();
}

void GpuImage::create(VkDevice device, VkPhysicalDevice physical, VkExtent2D extent,
                      VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                      std::uint32_t mip_levels) {
    release();
    device_ = device;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
    VkFormatFeatureFlags required = 0;
    if ((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) {
        required |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    }
    if ((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0) {
        required |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0) {
        required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    }
    if ((properties.optimalTilingFeatures & required) != required) {
        throw std::runtime_error("Required offscreen image format is unsupported");
    }
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = mip_levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    check_vk(vkCreateImage(device_, &info, nullptr, &image_), "create offscreen image");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, image_, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    std::uint32_t type = memory.memoryTypeCount;
    for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        if ((requirements.memoryTypeBits & (1U << index)) != 0 &&
            (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            type = index;
            break;
        }
    }
    if (type == memory.memoryTypeCount) {
        throw std::runtime_error("No offscreen memory type");
    }
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    check_vk(vkAllocateMemory(device_, &allocation, nullptr, &memory_), "allocate offscreen image");
    check_vk(vkBindImageMemory(device_, image_, memory_, 0), "bind offscreen image");
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, mip_levels, 0, 1};
    check_vk(vkCreateImageView(device_, &view, nullptr, &view_), "create offscreen view");
}

void GpuImage::release() {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, view_, nullptr);
        vkDestroyImage(device_, image_, nullptr);
        vkFreeMemory(device_, memory_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    image_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    view_ = VK_NULL_HANDLE;
}
VkImage GpuImage::image() const noexcept {
    return image_;
}
VkImageView GpuImage::view() const noexcept {
    return view_;
}
