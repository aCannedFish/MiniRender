// 将矩形灯参数打包成 std430 的五个 vec4；LTC 表在设备建立时上传一次。
#include "gpu_rectangle.hpp"
#include <algorithm>

namespace {
struct alignas(16) PackedLight {
    std::array<glm::vec4, 4> corners;
    glm::vec4                radiance;
};

static_assert(sizeof(PackedLight) == 80);
} // namespace

void GpuRectangleLights::create(VulkanViewport& viewport) {
    const auto& fit = minirender::rectangle_fit();
    for (std::size_t i = 0; i < 2; ++i) {
        textures_[i].create(viewport, {&fit[i], 1});
    }
}

bool GpuRectangleLights::update(VulkanViewport&                                viewport,
                                const std::vector<minirender::RectangleLight>& lights) {
    const auto count  = std::max<std::size_t>(1, lights.size());
    const bool resize = count > capacity_;
    if (resize) {
        lights_.create(viewport.device(), viewport.physicalDevice(), sizeof(PackedLight) * count,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        capacity_ = count;
    }
    std::vector<PackedLight> packed(count);
    for (std::size_t i = 0; i < lights.size(); ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            packed[i].corners[j] = glm::vec4(lights[i].corners[j], 1);
        }
        packed[i].radiance = glm::vec4(lights[i].radiance, lights[i].double_sided ? 1.F : 0.F);
    }
    lights_.write(packed.data(), packed.size() * sizeof(PackedLight));
    return resize;
}

void GpuRectangleLights::record_upload(VkCommandBuffer command) {
    for (auto& texture : textures_) {
        texture.retire_upload();
        texture.record_upload(command);
    }
}

void GpuRectangleLights::release() {
    for (auto& texture : textures_) {
        texture.release();
    }
    lights_.release();
    capacity_ = 0;
}

std::array<VkDescriptorImageInfo, 2> GpuRectangleLights::tables() const {
    return {textures_[0].descriptor(), textures_[1].descriptor()};
}

VkDescriptorBufferInfo GpuRectangleLights::buffer() const {
    return {lights_.handle(), 0, VK_WHOLE_SIZE};
}
