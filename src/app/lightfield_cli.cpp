#include "lightfield_cli.hpp"

#include <minirender/assets/scene_io.hpp>
#include <minirender/render/lightfield_capture.hpp>

#include <charconv>
#include <iostream>
#include <locale>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace {
template <class T> T number(std::string_view text) {
    T result{};
    if constexpr (std::is_floating_point_v<T>) {
        // 旧版 libc++ 没有浮点 from_chars，沿用现有 CLI 的固定 locale 解析。
        std::istringstream stream{std::string(text)};
        stream.imbue(std::locale::classic());
        stream >> std::noskipws >> result;
        if (!stream || stream.peek() != std::char_traits<char>::eof()) {
            throw std::invalid_argument("Invalid light field number");
        }
    } else {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            throw std::invalid_argument("Invalid light field integer");
        }
    }
    return result;
}
std::filesystem::path path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
} // namespace

int run_lightfield_cli(int argc, char** argv, bool capture) {
    using namespace minirender;
    if (argc < 2) {
        throw std::invalid_argument(
            capture ? "Usage: --capture-lightfield scene.json output.mrlf [--width N --height N "
                      "--spp N --columns N --rows N --radius M --reference-focus M --seed N "
                      "--depth N --mode mis|bsdf|light --threads N --memory-mb N]"
                    : "Usage: --refocus input.mrlf output-prefix [--focus M --aperture M "
                      "--memory-mb N --cache-mb N --exposure EV]");
    }
    LightFieldCaptureSettings settings;
    IntegratorSettings integrator;
    LightFieldRefocusSettings refocus;
    double exposure = 0;
    std::uint64_t cache = 128ULL * 1024 * 1024;
    std::shared_ptr<const LightFieldFile> file;
    bool reference_set = false, radius_set = false;
    if (!capture) {
        file = LightFieldFile::open(path(argv[0]));
        refocus.focus_distance = file->calibration().reference_focus;
        refocus.aperture_radius = file->calibration().pupil_radius;
    }
    for (int i = 2; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (capture && option == "--no-rr") {
            integrator.russian_roulette = false;
            continue;
        }
        if (i + 1 >= argc) {
            throw std::invalid_argument("Light field option is missing a value");
        }
        const std::string_view value(argv[++i]);
        if (option == "--memory-mb") {
            const auto bytes = number<std::uint32_t>(value) * 1024ULL * 1024;
            settings.memory_budget_bytes = refocus.memory_budget_bytes = bytes;
        } else if (capture) {
            if (option == "--width") {
                settings.render.width = number<std::size_t>(value);
            } else if (option == "--height") {
                settings.render.height = number<std::size_t>(value);
            } else if (option == "--spp") {
                settings.render.samples_per_pixel = number<std::uint32_t>(value);
            } else if (option == "--seed") {
                settings.render.seed = number<std::uint64_t>(value);
            } else if (option == "--columns") {
                settings.columns = number<std::uint32_t>(value);
            } else if (option == "--rows") {
                settings.rows = number<std::uint32_t>(value);
            } else if (option == "--threads") {
                settings.threads = number<std::uint32_t>(value);
            } else if (option == "--radius") {
                settings.aperture_radius = number<double>(value);
                radius_set = true;
            } else if (option == "--reference-focus") {
                settings.reference_focus = number<double>(value);
                reference_set = true;
            } else if (option == "--tile") {
                settings.tile_size = number<std::uint32_t>(value);
            } else if (option == "--depth") {
                integrator.maximum_depth = number<std::uint32_t>(value);
            } else if (option == "--mode") {
                integrator.mode = parse_sampling_mode(std::string(value));
            } else if (option == "--rr-depth") {
                integrator.roulette_depth = number<std::uint32_t>(value);
            } else {
                throw std::invalid_argument("Unknown capture option: " + std::string(option));
            }
        } else {
            if (option == "--focus") {
                refocus.focus_distance = number<double>(value);
            } else if (option == "--aperture") {
                refocus.aperture_radius = number<double>(value);
            } else if (option == "--cache-mb") {
                cache = number<std::uint32_t>(value) * 1024ULL * 1024;
            } else if (option == "--exposure") {
                exposure = number<double>(value);
            } else {
                throw std::invalid_argument("Unknown refocus option: " + std::string(option));
            }
        }
    }
    if (capture) {
        const auto scene = make_snapshot(load_scene(path(argv[0])));
        if (!reference_set) {
            settings.reference_focus = scene->camera.focus_distance;
        }
        if (!radius_set) {
            settings.aperture_radius = scene->camera.aperture_radius > 0
                                           ? scene->camera.aperture_radius
                                           : .01 * scene->camera.focus_distance;
        }
        LightFieldCaptureSession session;
        session.start(scene, settings, integrator);
        while (!session.wait_for(std::chrono::milliseconds(1000))) {
            const auto p = session.progress();
            std::cout << "views " << p.completed_views << '/' << p.total_views << " samples "
                      << p.recorded_samples << '/' << p.total_samples << '\n'
                      << std::flush;
        }
        session.wait();
        const auto progress = session.progress();
        if (progress.state != RenderState::completed) {
            throw std::runtime_error(progress.error);
        }
        session.result()->save(path(argv[1]));
        std::cout << "Capture paths: " << progress.recorded_samples
                  << "\nWorking bytes: " << progress.estimated_bytes
                  << "\nPayload bytes: " << progress.payload_bytes << "\nLight field saved.\n";
    } else {
        LightFieldReader reader(file, cache);
        const auto image = refocus_lightfield(reader, refocus);
        image.image.write_exr(path(std::string(argv[1]) + ".exr"));
        display_image(image.image, exposure).write_png(path(std::string(argv[1]) + ".png"));
        std::cout << "Stored capture paths: " << file->traced_paths()
                  << "\nNew rays: 0\nSelected views: " << image.selected_views
                  << "\nWorking bytes: " << image.estimated_bytes << "\nEXR and PNG saved.\n";
    }
    return 0;
}
