#include <minirender/core/image.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace minirender {
namespace {

// 在分配前检查乘法，防止无效尺寸绕过 vector 的长度检查。
std::size_t pixel_count(std::size_t width, std::size_t height) {
    if (width == 0 || height == 0 || width > std::numeric_limits<std::size_t>::max() / height) {
        throw std::invalid_argument("Image dimensions must be nonzero and fit in size_t");
    }
    return width * height;
}

} // namespace

float linear_to_srgb(float linear) {
    if (!std::isfinite(linear)) {
        return 0.0F;
    }
    linear = std::max(linear, 0.0F);
    return linear <= 0.0031308F ? 12.92F * linear : 1.055F * std::pow(linear, 1.0F / 2.4F) - 0.055F;
}

float srgb_to_linear(float encoded) {
    if (!std::isfinite(encoded)) {
        return 0.0F;
    }
    encoded = std::max(encoded, 0.0F);
    return encoded <= 0.04045F ? encoded / 12.92F : std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

std::uint8_t encode_unorm8(float value) {
    if (!std::isfinite(value)) {
        return 0;
    }
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

Image::Image(std::size_t width, std::size_t height)
    : width_(width), height_(height), pixels_(pixel_count(width, height), glm::vec3(0.0F)) {}

std::size_t Image::width() const noexcept {
    return width_;
}

std::size_t Image::height() const noexcept {
    return height_;
}

glm::vec3& Image::at(std::size_t x, std::size_t y) {
    if (x >= width_ || y >= height_) {
        throw std::out_of_range("Image pixel is outside its extent");
    }
    return pixels_[y * width_ + x];
}

const glm::vec3& Image::at(std::size_t x, std::size_t y) const {
    if (x >= width_ || y >= height_) {
        throw std::out_of_range("Image pixel is outside its extent");
    }
    return pixels_[y * width_ + x];
}

void Image::write_ppm(const std::filesystem::path& path) const {
    std::ofstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream << "P6\n" << width_ << ' ' << height_ << "\n255\n";
    for (const auto& pixel : pixels_) {
        const std::array<std::uint8_t, 3> rgb{encode_unorm8(linear_to_srgb(pixel.r)),
                                              encode_unorm8(linear_to_srgb(pixel.g)),
                                              encode_unorm8(linear_to_srgb(pixel.b))};
        stream.write(reinterpret_cast<const char*>(rgb.data()), 3);
    }
    stream.close();
}

Image make_diagnostic_image(std::size_t width, std::size_t height) {
    Image image(width, height);
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const float u =
                static_cast<float>(x) / static_cast<float>(std::max(width - 1, std::size_t(1)));
            const float v =
                static_cast<float>(y) / static_cast<float>(std::max(height - 1, std::size_t(1)));
            image.at(x, y) = glm::vec3(u, v, 0.25F);
        }
    }
    return image;
}

} // namespace minirender
