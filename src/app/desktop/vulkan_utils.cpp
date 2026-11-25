// 按资源的 memoryTypeBits 和设备属性选择显存类型，不分配或持有 Vulkan 资源。
#include "vulkan_utils.hpp"
#include <stdexcept>
#include <string>

void check_vk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
    }
}

std::uint32_t find_memory_type(VkPhysicalDevice physical, std::uint32_t allowed,
                               VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties properties{};
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((allowed & (1U << i)) && (flags & required) == required) {
            return i;
        }
    }
    throw std::runtime_error("No compatible Vulkan memory type for flags " +
                             std::to_string(required));
}
