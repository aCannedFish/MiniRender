#include <minirender/core/texture.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace minirender {
namespace {

int wrap(int coordinate, int extent) {
    return (coordinate % extent + extent) % extent;
}

glm::vec2 image_uv(glm::vec2 uv) {
    if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) {
        throw std::invalid_argument("Texture coordinates must be finite");
    }
    return {uv.x - std::floor(uv.x), 1.0F - (uv.y - std::floor(uv.y))};
}

// 按像素覆盖面积求均值。奇数尺寸的最后一行/列同样参与，不丢失能量。
Image downsample(const Image& source) {
    Image target(std::max(std::size_t(1), source.width() / 2),
                 std::max(std::size_t(1), source.height() / 2));
    for (std::size_t y = 0; y < target.height(); ++y) {
        for (std::size_t x = 0; x < target.width(); ++x) {
            const double left = double(x) * double(source.width()) / double(target.width());
            const double right = double(x + 1) * double(source.width()) / double(target.width());
            const double top = double(y) * double(source.height()) / double(target.height());
            const double bottom = double(y + 1) * double(source.height()) / double(target.height());
            glm::dvec3 sum(0);
            double weight_sum = 0;
            for (std::size_t sy = static_cast<std::size_t>(top);
                 sy < static_cast<std::size_t>(std::ceil(bottom)); ++sy) {
                for (std::size_t sx = static_cast<std::size_t>(left);
                     sx < static_cast<std::size_t>(std::ceil(right)); ++sx) {
                    const double weight =
                        (std::min(right, double(sx + 1)) - std::max(left, double(sx))) *
                        (std::min(bottom, double(sy + 1)) - std::max(top, double(sy)));
                    sum += glm::dvec3(source.at(sx, sy)) * weight;
                    weight_sum += weight;
                }
            }
            target.at(x, y) = glm::vec3(sum / weight_sum);
        }
    }
    return target;
}

} // namespace

Texture::Texture(Image image) {
    if (image.width() > 16384 || image.height() > 16384 ||
        image.width() * image.height() > 64U * 1024U * 1024U) {
        throw std::invalid_argument("Texture exceeds the 64-megapixel budget");
    }
    for (std::size_t y = 0; y < image.height(); ++y) {
        for (std::size_t x = 0; x < image.width(); ++x) {
            const auto pixel = image.at(x, y);
            if (!std::isfinite(pixel.r) || !std::isfinite(pixel.g) || !std::isfinite(pixel.b)) {
                throw std::invalid_argument("Texture contains nonfinite samples");
            }
        }
    }
    levels_.push_back(std::move(image));
    while (levels_.back().width() > 1 || levels_.back().height() > 1) {
        levels_.push_back(downsample(levels_.back()));
    }
}

const Image& Texture::level(std::size_t index) const {
    return levels_.at(index);
}
std::size_t Texture::level_count() const noexcept {
    return levels_.size();
}

glm::vec3 Texture::bilinear(glm::vec2 uv, std::size_t level_index) const {
    const auto& image = levels_.at(level_index);
    const auto coordinate = image_uv(uv);
    const float x = coordinate.x * static_cast<float>(image.width()) - 0.5F;
    const float y = coordinate.y * static_cast<float>(image.height()) - 0.5F;
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const float tx = x - std::floor(x);
    const float ty = y - std::floor(y);
    const auto texel = [&](int px, int py) {
        return image.at(static_cast<std::size_t>(wrap(px, static_cast<int>(image.width()))),
                        static_cast<std::size_t>(wrap(py, static_cast<int>(image.height()))));
    };
    return glm::mix(glm::mix(texel(ix, iy), texel(ix + 1, iy), tx),
                    glm::mix(texel(ix, iy + 1), texel(ix + 1, iy + 1), tx), ty);
}

glm::vec3 Texture::sample(glm::vec2 uv, float lod, TextureFilter filter) const {
    if (!std::isfinite(lod)) {
        throw std::invalid_argument("Texture LOD must be finite");
    }
    if (filter == TextureFilter::nearest) {
        const auto coordinate = image_uv(uv);
        const auto& image = levels_.front();
        const int x =
            static_cast<int>(std::floor(coordinate.x * static_cast<float>(image.width())));
        const int y =
            static_cast<int>(std::floor(coordinate.y * static_cast<float>(image.height())));
        return image.at(static_cast<std::size_t>(wrap(x, static_cast<int>(image.width()))),
                        static_cast<std::size_t>(wrap(y, static_cast<int>(image.height()))));
    }
    if (filter == TextureFilter::bilinear) {
        return bilinear(uv, 0);
    }
    if (filter != TextureFilter::trilinear) {
        throw std::invalid_argument("Unknown texture filter");
    }
    const float clamped = std::clamp(lod, 0.0F, static_cast<float>(levels_.size() - 1));
    const auto lower = static_cast<std::size_t>(std::floor(clamped));
    const auto upper = std::min(lower + 1, levels_.size() - 1);
    return glm::mix(bilinear(uv, lower), bilinear(uv, upper), clamped - static_cast<float>(lower));
}

std::string texture_filter_name(TextureFilter filter) {
    switch (filter) {
    case TextureFilter::nearest:
        return "nearest";
    case TextureFilter::bilinear:
        return "bilinear";
    case TextureFilter::trilinear:
        return "trilinear";
    }
    throw std::invalid_argument("Unknown texture filter");
}
TextureFilter parse_texture_filter(const std::string& name) {
    if (name == "nearest") {
        return TextureFilter::nearest;
    }
    if (name == "bilinear") {
        return TextureFilter::bilinear;
    }
    if (name == "trilinear") {
        return TextureFilter::trilinear;
    }
    throw std::invalid_argument("Unknown texture filter: " + name);
}

} // namespace minirender
