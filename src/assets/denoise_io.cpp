// 读取线性 EXR 和小端、行主序的 HW5 矩阵。文件头存对象数量，末尾另有视图和像素投影。
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <minirender/assets/denoise_io.hpp>

namespace minirender {
namespace {
std::uint32_t word(std::istream& in) {
    std::array<unsigned char, 4> bytes{};
    if (!in.read(reinterpret_cast<char*>(bytes.data()), 4)) {
        throw std::runtime_error("Truncated guide matrix");
    }
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8U) |
           (std::uint32_t(bytes[2]) << 16U) | (std::uint32_t(bytes[3]) << 24U);
}

void put(std::ostream& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        out.put(static_cast<char>((value >> (8 * i)) & 255));
    }
}

glm::mat4 matrix(std::istream& in) {
    glm::mat4 m(1);
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            m[col][row] = std::bit_cast<float>(word(in));
            if (!std::isfinite(m[col][row])) {
                throw std::runtime_error("Non-finite matrix coefficient");
            }
        }
    }
    return m;
}
} // namespace

GuideFiles guide_files(const std::filesystem::path& directory, std::size_t frame) {
    const auto suffix = "_" + std::to_string(frame) + ".exr";
    return {directory / ("normal" + suffix), directory / ("position" + suffix),
            directory / ("depth" + suffix), directory / ("ID" + suffix),
            directory / ("matrix_" + std::to_string(frame) + ".mat")};
}

DenoiseGuides read_guides(const GuideFiles& files, CancellationToken token) {
    std::ifstream in(files.matrix, std::ios::binary);
    const auto    count = word(in);
    if (std::filesystem::file_size(files.matrix) != 4ULL + 64ULL * (std::uint64_t(count) + 2)) {
        throw std::runtime_error("Invalid matrix table size");
    }
    DenoiseGuides guides;
    guides.objects.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        guides.objects.push_back(matrix(in));
    }
    guides.world_to_view  = matrix(in);
    guides.world_to_pixel = matrix(in);
    // 依次解码，避免同时保留四张 RGB32f 暂存图。
    for (int channel = 0; channel < 4; ++channel) {
        token.check();
        const std::array<const std::filesystem::path*, 4> paths{&files.position, &files.normal,
                                                                &files.depth, &files.object};
        auto data = Image::read_exr(*paths[std::size_t(channel)]);
        if (channel == 0) {
            guides.width  = data.width();
            guides.height = data.height();
            guides.pixels.resize(guides.width * guides.height);
        }
        if (data.width() != guides.width || data.height() != guides.height) {
            throw std::runtime_error("Guide EXR sizes differ");
        }
        for (std::size_t y = 0; y < guides.height; ++y) {
            token.check();
            for (std::size_t x = 0; x < guides.width; ++x) {
                const auto value = data.at(x, y);
                auto&      pixel = guides.pixels[y * guides.width + x];
                if (channel == 0) {
                    pixel.position = value;
                } else if (channel == 1) {
                    pixel.normal =
                        glm::dot(value, value) > 1e-10F ? glm::normalize(value) : glm::vec3(0);
                } else if (channel == 2) {
                    pixel.depth = value.x;
                } else {
                    if (!std::isfinite(value.x) || value.x < -1 || value.x >= float(count)) {
                        throw std::runtime_error("Object ID outside the matrix table");
                    }
                    pixel.object = static_cast<int>(std::round(value.x));
                }
            }
        }
    }
    return guides;
}

void write_guides(const DenoiseGuides& guides, const GuideFiles& files) {
    if (guides.pixels.size() != guides.width * guides.height) {
        throw std::invalid_argument("Guide dimensions differ");
    }
    std::ofstream out(files.matrix, std::ios::binary);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    put(out, static_cast<std::uint32_t>(guides.objects.size()));
    const auto write = [&](const glm::mat4& m) {
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                put(out, std::bit_cast<std::uint32_t>(m[col][row]));
            }
        }
    };
    for (const auto& transform : guides.objects) {
        write(transform);
    }
    write(guides.world_to_view);
    write(guides.world_to_pixel);
    out.close();
    const std::array<const std::filesystem::path*, 4> paths{&files.position, &files.normal,
                                                            &files.depth, &files.object};
    for (int channel = 0; channel < 4; ++channel) {
        Image data(guides.width, guides.height);
        for (std::size_t y = 0; y < guides.height; ++y) {
            for (std::size_t x = 0; x < guides.width; ++x) {
                const auto& p = guides.pixels[y * guides.width + x];
                data.at(x, y) = channel == 0   ? p.position
                                : channel == 1 ? p.normal
                                : channel == 2 ? glm::vec3(p.depth)
                                               : glm::vec3(float(p.object));
            }
        }
        data.write_exr(*paths[std::size_t(channel)]);
    }
}
} // namespace minirender
