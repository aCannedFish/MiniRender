#include <minirender/core/image.hpp>

#include <tinyexr.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>

namespace minirender {
namespace {

// 接管 codec 的 malloc 缓冲；后续文件写入或异常失败时也自动释放。
struct FreeMemory {
    void operator()(void* pointer) const noexcept {
        std::free(pointer);
    }
};

[[noreturn]] void codec_error(const char* error) {
    const std::string message = error ? error : "Unknown EXR codec error";
    if (error) {
        FreeEXRErrorMessage(error);
    }
    throw std::runtime_error(message);
}

} // namespace

void Image::write_exr(const std::filesystem::path& path) const {
    // Codec 的尺寸字段为 int；先检查乘法范围，避免第三方内部 int 乘法溢出。
    if (width_ > static_cast<std::size_t>(std::numeric_limits<int>::max()) / height_) {
        throw std::invalid_argument("EXR dimensions exceed codec limits");
    }
    std::array<std::vector<float>, 3> planes;
    for (std::vector<float>& plane : planes) {
        plane.resize(pixels_.size());
    }
    for (std::size_t i = 0; i < pixels_.size(); ++i) {
        for (int component = 0; component < 3; ++component) {
            const float value = pixels_[i][component];
            if (!std::isfinite(value)) {
                throw std::invalid_argument("EXR output contains nonfinite radiance");
            }
            // EXR 通道按名字排序为 B、G、R，像素数据保持 32 位线性值，不进行 tone mapping。
            planes[static_cast<std::size_t>(2 - component)][i] = value;
        }
    }
    std::array<unsigned char*, 3> pointers{};
    std::array<EXRChannelInfo, 3> channels{};
    std::array<int, 3> types{TINYEXR_PIXELTYPE_FLOAT, TINYEXR_PIXELTYPE_FLOAT,
                             TINYEXR_PIXELTYPE_FLOAT};
    const std::array<const char*, 3> names{"B", "G", "R"};
    for (std::size_t i = 0; i < planes.size(); ++i) {
        pointers[i] = reinterpret_cast<unsigned char*>(planes[i].data());
        channels[i].name[0] = names[i][0]; // channels 已零初始化，名字均为单字符。
        channels[i].pixel_type = TINYEXR_PIXELTYPE_FLOAT;
        channels[i].x_sampling = channels[i].y_sampling = 1;
    }
    EXRHeader header;
    InitEXRHeader(&header);
    header.num_channels = 3;
    header.channels = channels.data();
    header.pixel_types = types.data();
    header.requested_pixel_types = types.data();
    header.compression_type = TINYEXR_COMPRESSIONTYPE_ZIP;
    EXRImage image;
    InitEXRImage(&image);
    image.num_channels = 3;
    image.width = static_cast<int>(width_);
    image.height = static_cast<int>(height_);
    image.images = pointers.data();
    unsigned char* buffer = nullptr;
    const char* error = nullptr;
    const std::size_t bytes = SaveEXRImageToMemory(&image, &header, &buffer, &error);
    const std::unique_ptr<unsigned char, FreeMemory> data(buffer);
    if (bytes == 0) {
        codec_error(error);
    }
    std::ofstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.write(reinterpret_cast<const char*>(data.get()), static_cast<std::streamsize>(bytes));
    stream.close();
}

Image Image::read_exr(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Cannot open EXR image");
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(stream)), {});
    float* rgba = nullptr;
    int width = 0;
    int height = 0;
    const char* error = nullptr;
    const int result =
        LoadEXRFromMemory(&rgba, &width, &height, bytes.data(), bytes.size(), &error);
    const std::unique_ptr<float, FreeMemory> data(rgba);
    if (result != TINYEXR_SUCCESS) {
        codec_error(error);
    }
    if (width <= 0 || height <= 0 || !data) {
        throw std::runtime_error("Invalid decoded EXR dimensions");
    }
    Image image(static_cast<std::size_t>(width), static_cast<std::size_t>(height));
    for (std::size_t i = 0; i < image.pixels_.size(); ++i) {
        image.pixels_[i] = {data.get()[i * 4], data.get()[i * 4 + 1], data.get()[i * 4 + 2]};
    }
    return image;
}

} // namespace minirender
