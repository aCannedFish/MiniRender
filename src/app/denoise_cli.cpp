// denoise 子命令共享桌面工作台的 EXR、矩阵读取和几何引导滤波。
#include "denoise_cli.hpp"
#include <charconv>
#include <iostream>
#include <minirender/assets/denoise_io.hpp>
#include <minirender/assets/scene_io.hpp>

namespace {
std::size_t number(std::string_view text) {
    std::size_t n      = 0;
    auto        parsed = std::from_chars(text.data(), text.data() + text.size(), n);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument("Expected an unsigned integer");
    }
    return n;
}

std::filesystem::path path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
} // namespace

int run_denoise_cli(int argc, char** argv) {
    using namespace minirender;
    if (argc == 6 && std::string_view(argv[0]) == "--write-guides") {
        const auto directory = path(argv[2]);
        std::filesystem::create_directories(directory);
        const auto scene  = make_snapshot(load_scene(path(argv[1])));
        const auto guides = make_guides(scene, number(argv[4]), number(argv[5]));
        write_guides(guides, guide_files(directory, number(argv[3])));
        return 0;
    }
    if (argc != 6) {
        throw std::invalid_argument(
            "Usage: --denoise INPUT OUTPUT FIRST COUNT bilateral|atrous|temporal");
    }
    const auto input = path(argv[1]), output = path(argv[2]);
    std::filesystem::create_directories(output);
    const auto             first = number(argv[3]), count = number(argv[4]);
    const std::string_view method = argv[5];
    if (method != "bilateral" && method != "atrous" && method != "temporal") {
        throw std::invalid_argument("Unknown denoising method");
    }
    DenoiseSettings settings;
    settings.filter = method == "bilateral" ? SpatialFilter::bilateral : SpatialFilter::atrous;
    std::optional<Image>         history;
    std::optional<DenoiseGuides> previous;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const auto index    = first + offset;
        const auto filename = "beauty_" + std::to_string(index) + ".exr";
        if (std::filesystem::weakly_canonical(input / filename) ==
            std::filesystem::weakly_canonical(output / filename)) {
            throw std::invalid_argument("Select a separate output directory");
        }
        auto        frame    = Image::read_exr(input / filename);
        auto        guides   = read_guides(guide_files(input, index));
        auto        filtered = filter_spatial(frame, guides, settings);
        std::size_t accepted = 0;
        if (method == "temporal" && history) {
            auto result = filter_temporal(filtered, guides, *history, *previous, settings);
            filtered    = std::move(result.image);
            accepted    = result.accepted;
        }
        filtered.write_exr(output / filename);
        if (method == "temporal") {
            history  = std::move(filtered);
            previous = std::move(guides);
        }
        std::cout << "frame " << index << " · accepted history " << accepted << '\n';
    }
    return 0;
}
