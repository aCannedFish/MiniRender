#include "render_cli.hpp"

#include <minirender/assets/scene_io.hpp>
#include <minirender/render/path_tracer.hpp>

#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace {

template <typename T> T number(std::string_view text) {
    T result{};
    bool valid = false;
    if constexpr (std::is_floating_point_v<T>) {
        // macOS 14 的 libc++ 尚无浮点 from_chars。固定 classic locale，避免系统小数格式影响 CLI。
        std::istringstream input{std::string(text)};
        input.imbue(std::locale::classic());
        input >> std::noskipws >> result;
        valid = static_cast<bool>(input) && input.peek() == std::char_traits<char>::eof();
    } else {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        valid = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    }
    if (!valid) {
        throw std::invalid_argument("Invalid numeric render option: " + std::string(text));
    }
    return result;
}
std::filesystem::path native_path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

} // namespace

int run_render_cli(int argc, char** argv) {
    if (argc < 2) {
        throw std::invalid_argument(
            "Usage: --render scene.json output-prefix [--width N --height N --spp N --depth N "
            "--mode mis|bsdf|light --seed N --exposure EV --no-rr --rr-depth N]");
    }
    minirender::RenderSettings render;
    minirender::IntegratorSettings integrator;
    double exposure = 0;
    for (int index = 2; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--no-rr") {
            integrator.russian_roulette = false;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("Render option is missing its value: " +
                                        std::string(option));
        }
        const std::string_view value(argv[++index]);
        if (option == "--width") {
            render.width = number<std::size_t>(value);
        } else if (option == "--height") {
            render.height = number<std::size_t>(value);
        } else if (option == "--spp") {
            render.samples_per_pixel = number<std::uint32_t>(value);
        } else if (option == "--depth") {
            integrator.maximum_depth = number<std::uint32_t>(value);
        } else if (option == "--rr-depth") {
            integrator.roulette_depth = number<std::uint32_t>(value);
        } else if (option == "--seed") {
            render.seed = number<std::uint64_t>(value);
        } else if (option == "--mode") {
            integrator.mode = minirender::parse_sampling_mode(std::string(value));
        } else if (option == "--exposure") {
            exposure = number<double>(value);
        } else {
            throw std::invalid_argument("Unknown render option: " + std::string(option));
        }
    }
    render.validate();
    integrator.validate();
    if (!std::isfinite(exposure) || std::abs(exposure) > 64) {
        throw std::invalid_argument("Exposure must be finite and in [-64,64]");
    }
    const auto scene = minirender::make_snapshot(minirender::load_scene(native_path(argv[0])));
    const auto start = std::chrono::steady_clock::now();
    const minirender::PathTracer tracer(scene, integrator);
    minirender::PathStatistics statistics;
    const auto image = tracer.render(render, &statistics);
    const auto finished = std::chrono::steady_clock::now();
    auto prefix = native_path(argv[1]);
    if (prefix.extension() == ".png" || prefix.extension() == ".exr") {
        prefix.replace_extension();
    }
    if (!prefix.parent_path().empty()) {
        std::filesystem::create_directories(prefix.parent_path());
    }
    auto exr = prefix, png = prefix;
    exr += ".exr";
    png += ".png";
    image.write_exr(exr);
    minirender::display_image(image, exposure).write_png(png);
    std::cout << "CPU path render complete\nmode: "
              << minirender::sampling_mode_name(integrator.mode) << "\nresolution: " << render.width
              << 'x' << render.height << "\nspp: " << render.samples_per_pixel
              << "\npaths: " << statistics.paths << "\nshadow rays: " << statistics.shadow_rays
              << "\nscattering events: " << statistics.scattering_events
              << "\ntriangle tests: " << statistics.geometry.triangle_tests << "\nelapsed ms: "
              << std::chrono::duration<double, std::milli>(finished - start).count()
              << "\nEXR and PNG saved.\n";
    return 0;
}
