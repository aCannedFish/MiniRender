#include "gpu_buffer.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

void check_vk(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
    }
}

GpuBuffer::~GpuBuffer() {
    release();
}
GpuBuffer::GpuBuffer(GpuBuffer&& other) noexcept {
    *this = std::move(other);
}
GpuBuffer& GpuBuffer::operator=(GpuBuffer&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        buffer_ = std::exchange(other.buffer_, VK_NULL_HANDLE);
        memory_ = std::exchange(other.memory_, VK_NULL_HANDLE);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void GpuBuffer::create(VkDevice device, VkPhysicalDevice physical, VkDeviceSize bytes,
                       VkBufferUsageFlags usage) {
    release();
    device_ = device;
    size_ = bytes;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    check_vk(vkCreateBuffer(device_, &info, nullptr, &buffer_), "create buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer_, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        if ((requirements.memoryTypeBits & (1U << index)) != 0 &&
            (memory.memoryTypes[index].propertyFlags & flags) == flags) {
            VkMemoryAllocateInfo allocate{};
            allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = index;
            check_vk(vkAllocateMemory(device_, &allocate, nullptr, &memory_),
                     "allocate host buffer");
            check_vk(vkBindBufferMemory(device_, buffer_, memory_, 0), "bind host buffer");
            return;
        }
    }
    throw std::runtime_error("No host-visible coherent memory type");
}

void GpuBuffer::write(const void* data, std::size_t bytes) {
    if (bytes > size_) {
        throw std::out_of_range("GPU buffer write exceeds allocation");
    }
    void* mapped = nullptr;
    check_vk(vkMapMemory(device_, memory_, 0, size_, 0, &mapped), "map buffer");
    std::memcpy(mapped, data, bytes);
    vkUnmapMemory(device_, memory_);
}

void GpuBuffer::release() {
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer_, nullptr);
        vkFreeMemory(device_, memory_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    buffer_ = VK_NULL_HANDLE;
    memory_ = VK_NULL_HANDLE;
    size_ = 0;
}

VkBuffer GpuBuffer::handle() const noexcept {
    return buffer_;
}
