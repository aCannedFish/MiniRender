#include <minirender/core/image.hpp>

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>

namespace minirender {
namespace {

struct FreeStb {
    void operator()(void* pointer) const noexcept {
        stbi_image_free(pointer);
    }
};

void append_encoded_bytes(void* context, void* bytes, int count) {
    auto& output = *static_cast<std::vector<unsigned char>*>(context);
    const auto* data = static_cast<unsigned char*>(bytes);
    output.insert(output.end(), data, data + count);
}

void check_dimensions(int width, int height) {
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384 ||
        std::uint64_t(width) * std::uint64_t(height) > 64U * 1024U * 1024U) {
        throw std::runtime_error("Image exceeds the 64-megapixel texture budget");
    }
}

} // namespace

Image Image::read(const std::filesystem::path& path, ColorEncoding encoding) {
    auto extension = path.extension().string();
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (extension == ".exr") {
        if (encoding != ColorEncoding::linear) {
            throw std::invalid_argument("EXR textures require linear encoding");
        }
        return read_exr(path);
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open texture image");
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Encoded image is too large");
    }
    const int size = static_cast<int>(bytes.size());
    int width = 0, height = 0, components = 0;
    if (!stbi_info_from_memory(bytes.data(), size, &width, &height, &components)) {
        throw std::runtime_error("Unsupported or corrupt texture image");
    }
    check_dimensions(width, height);
    Image image(static_cast<std::size_t>(width), static_cast<std::size_t>(height));
    if (stbi_is_hdr_from_memory(bytes.data(), size)) {
        if (encoding != ColorEncoding::linear) {
            throw std::invalid_argument("HDR textures require linear encoding");
        }
        std::unique_ptr<float, FreeStb> decoded(
            stbi_loadf_from_memory(bytes.data(), size, &width, &height, &components, 3));
        if (!decoded) {
            throw std::runtime_error("HDR decode failed");
        }
        for (std::size_t i = 0; i < image.pixels_.size(); ++i) {
            image.pixels_[i] = {decoded.get()[i * 3], decoded.get()[i * 3 + 1],
                                decoded.get()[i * 3 + 2]};
        }
    } else if (stbi_is_16_bit_from_memory(bytes.data(), size)) {
        std::unique_ptr<stbi_us, FreeStb> decoded(
            stbi_load_16_from_memory(bytes.data(), size, &width, &height, &components, 3));
        if (!decoded) {
            throw std::runtime_error("16-bit image decode failed");
        }
        for (std::size_t i = 0; i < image.pixels_.size(); ++i) {
            for (int channel = 0; channel < 3; ++channel) {
                const float value =
                    static_cast<float>(decoded.get()[i * 3 + static_cast<std::size_t>(channel)]) /
                    65535.0F;
                image.pixels_[i][channel] =
                    encoding == ColorEncoding::srgb ? srgb_to_linear(value) : value;
            }
        }
    } else {
        std::unique_ptr<stbi_uc, FreeStb> decoded(
            stbi_load_from_memory(bytes.data(), size, &width, &height, &components, 3));
        if (!decoded) {
            throw std::runtime_error("Image decode failed");
        }
        for (std::size_t i = 0; i < image.pixels_.size(); ++i) {
            for (int channel = 0; channel < 3; ++channel) {
                const float value =
                    static_cast<float>(decoded.get()[i * 3 + static_cast<std::size_t>(channel)]) /
                    255.0F;
                image.pixels_[i][channel] =
                    encoding == ColorEncoding::srgb ? srgb_to_linear(value) : value;
            }
        }
    }
    return image;
}

void Image::write_png(const std::filesystem::path& path) const {
    if (width_ > 16384 || height_ > 16384) {
        throw std::invalid_argument("PNG dimensions exceed texture limits");
    }
    check_dimensions(static_cast<int>(width_), static_cast<int>(height_));
    std::vector<unsigned char> rgb(pixels_.size() * 3);
    for (std::size_t i = 0; i < pixels_.size(); ++i) {
        for (int channel = 0; channel < 3; ++channel) {
            rgb[i * 3 + static_cast<std::size_t>(channel)] =
                encode_unorm8(linear_to_srgb(pixels_[i][channel]));
        }
    }
    std::vector<unsigned char> encoded;
    if (!stbi_write_png_to_func(append_encoded_bytes, &encoded, static_cast<int>(width_),
                                static_cast<int>(height_), 3, rgb.data(),
                                static_cast<int>(width_ * 3))) {
        throw std::runtime_error("PNG encode failed");
    }
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    output.close();
}

} // namespace minirender
