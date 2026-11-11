#pragma once

#include <minirender/scene/scene.hpp>

#include <cstddef>

// std430 中所有成员以 vec4 为单位。线程宽度采用弧度，曲线样本直接复用 CPU 编译结果。
struct alignas(16) GpuCloth {
    glm::vec4 settings{0}; // IOR, correlation width, cos(rotation), sin(rotation)
    glm::ivec4 counts{0};  // total samples, enabled
    glm::vec4 coverage{0};
    std::array<glm::vec4, 2> albedo;
    std::array<glm::vec4, 2> widths;
    std::array<glm::vec4, 32> tangents;
};
static_assert(sizeof(GpuCloth) == 624 && offsetof(GpuCloth, tangents) == 112);

GpuCloth pack_cloth(const minirender::Material& material);
