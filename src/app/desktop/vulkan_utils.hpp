// Vulkan 结果检查和显存类型查询，供窗口、缓冲区与附件共用。
#pragma once
#include <cstdint>
#include <vulkan/vulkan.h>

// 失败时抛出包含操作名和 VkResult 数值的异常。
void check_vk(VkResult result, const char* operation);
// 返回第一个既被资源允许、又具有全部所需属性的显存类型索引；无匹配项时抛出异常。
std::uint32_t find_memory_type(VkPhysicalDevice physical, std::uint32_t allowed,
                               VkMemoryPropertyFlags required);
