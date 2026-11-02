#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace minirender {

// 光照与显示之间的转换仅在输出处进行。输入与存储均为线性 RGB。
float linear_to_srgb(float linear);
float srgb_to_linear(float encoded);
std::uint8_t encode_unorm8(float value);

// 连续行主序图像，原点位于左上角。所有像素均为线性 RGB，允许 HDR 数值。
// Image 独占存储；多线程渲染必须划分不重叠区域，发布图像时另行同步。
class Image {
  public:
    Image(std::size_t width, std::size_t height);

    std::size_t width() const noexcept;
    std::size_t height() const noexcept;
    glm::vec3& at(std::size_t x, std::size_t y);
    const glm::vec3& at(std::size_t x, std::size_t y) const;

    // 仅用于基础阶段和无图像 SDK 的诊断。P6 文件写出经过 sRGB 编码的 RGB8。
    // 不执行色调映射；超出显示范围的有限值截断，非有限值显示为 0。
    void write_ppm(const std::filesystem::path& path) const;

  private:
    std::size_t width_;
    std::size_t height_;
    std::vector<glm::vec3> pixels_;
};

// 生成线性渐变用于检查 CLI 与显示变换；不属于渲染器输出。
Image make_diagnostic_image(std::size_t width, std::size_t height);

} // namespace minirender
