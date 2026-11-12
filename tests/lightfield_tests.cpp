#include <minirender/assets/lightfield_io.hpp>
#include <minirender/assets/scene_io.hpp>
#include <minirender/render/lightfield_capture.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <fstream>
#include <numbers>
#include <thread>

using namespace minirender;
namespace {
// 测试只拥有临时目录，异常退出不会改写用户资产。
struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("minirender-lf-test-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() {
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
LightFieldCalibration calibration(std::size_t width = 8, std::size_t height = 6) {
    LightFieldCalibration c;
    c.camera.position = {0, 0, 0};
    c.camera.target = {0, 0, -1};
    c.render = {width, height, 4, 101};
    c.columns = c.rows = 3;
    c.pupils = lightfield_pupils(c.columns, c.rows, c.pupil_radius);
    return c;
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
} // namespace

TEST_CASE("Light field pupils and refocus shifts match independent ray plane geometry",
          "[lightfield]") {
    auto c = calibration();
    c.camera.position = {1, 2, 3};
    c.camera.target = {-1, .5F, -3};
    c.camera.up = {.2F, 1, .1F};
    REQUIRE_NOTHROW(c.validate());
    REQUIRE(c.pupils[c.center_view()] == glm::dvec2(0));
    REQUIRE(c.pupils.front().x < 0);
    REQUIRE(c.pupils.front().y > 0);
    const auto forward =
        glm::normalize(glm::dvec3(c.camera.target) - glm::dvec3(c.camera.position));
    const auto right = glm::normalize(glm::cross(forward, glm::dvec3(c.camera.up)));
    const auto up = glm::cross(right, forward);
    for (const double focus : {2., 4., 7.}) {
        for (std::size_t v = 0; v < c.view_count(); ++v) {
            const glm::dvec2 uv{.37, .61};
            const auto chief =
                c.camera.generate_ray(uv, double(c.render.width) / double(c.render.height));
            const auto target =
                chief.origin + chief.direction * (focus / glm::dot(chief.direction, forward));
            const auto pupil = c.pupils[v].x * right + c.pupils[v].y * up;
            const auto start = glm::dvec3(c.camera.position) + pupil;
            const auto direction = glm::normalize(target - start);
            const auto point =
                start + direction * (c.reference_focus / glm::dot(direction, forward));
            const auto shift = lightfield_shift(c, v, focus);
            const auto ray = lightfield_ray(c, v,
                                            {uv.x + shift.x / double(c.render.width),
                                             uv.y + shift.y / double(c.render.height)});
            REQUIRE(glm::length(ray.origin - start) < 1e-14);
            REQUIRE(glm::length(ray.direction - direction) < 1e-13);
            const auto captured =
                ray.origin + ray.direction * (c.reference_focus / glm::dot(ray.direction, forward));
            REQUIRE(glm::length(captured - point) < 1e-13);
        }
    }
    REQUIRE(lightfield_ray(c, c.center_view(), {.3, .7}).direction ==
            c.camera.generate_ray({.3, .7}, 8. / 6).direction);
    c.pupils[0] = c.pupils[1];
    REQUIRE_THROWS(c.validate());
    REQUIRE_THROWS(lightfield_pupils(4, 3, .1));
}

TEST_CASE("Light field files stream views preserve pixels and bound their cache", "[lightfield]") {
    const TemporaryDirectory temporary;
    const auto path = temporary.path / std::filesystem::path(u8"光场 数据.mrlf");
    auto c = calibration();
    c.render.seed = std::numeric_limits<std::uint64_t>::max();
    LightFieldWriter writer(path, c);
    for (std::size_t v = 0; v < c.view_count(); ++v) {
        Image image(c.render.width, c.render.height);
        for (std::size_t y = 0; y < image.height(); ++y) {
            for (std::size_t x = 0; x < image.width(); ++x) {
                image.at(x, y) = {float(v), float(x), -float(y)};
            }
        }
        writer.write_view(v, image);
        REQUIRE_THROWS(writer.write_view(v, image));
    }
    const auto file = writer.finish(0);
    REQUIRE(file->traced_paths() == 0);
    REQUIRE(file->calibration().pupils == c.pupils);
    REQUIRE(file->calibration().render.seed == c.render.seed);
    LightFieldReader reader(file, c.view_bytes());
    for (std::size_t v = 0; v < c.view_count(); ++v) {
        const auto image = reader.view(v);
        REQUIRE(image->at(3, 2) == glm::vec3(float(v), 3, -2));
        REQUIRE(reader.statistics().cached_bytes == c.view_bytes());
        REQUIRE(reader.view(v) == image);
    }
    REQUIRE(reader.statistics().decoded_views == c.view_count());
    REQUIRE(reader.statistics().cache_hits == c.view_count());
    const auto copy = temporary.path / "copy.mrlf";
    file->save(copy);
    REQUIRE(bytes(copy) == bytes(path));
    REQUIRE_THROWS(file->save(copy, {}, [](auto) { throw std::runtime_error("stop"); }));
    REQUIRE(bytes(copy) == bytes(path));
    std::filesystem::resize_file(copy, std::filesystem::file_size(copy) - 1);
    REQUIRE_THROWS(LightFieldFile::open(copy));
}

TEST_CASE("Light field reconstruction preserves constants and center view without tracing",
          "[lightfield]") {
    const TemporaryDirectory temporary;
    const auto c = calibration();
    LightFieldWriter writer(temporary.path / "constant.mrlf", c);
    for (std::size_t v = 0; v < c.view_count(); ++v) {
        Image image(c.render.width, c.render.height);
        for (std::size_t y = 0; y < image.height(); ++y) {
            for (std::size_t x = 0; x < image.width(); ++x) {
                image.at(x, y) = {2, .25F, 17};
            }
        }
        writer.write_view(v, image);
    }
    const auto file = writer.finish(123);
    LightFieldReader reader(file, 0);
    for (const double focus : {1e-6, 2., 4., 7.}) {
        const auto result = refocus_lightfield(reader, {focus, c.pupil_radius});
        REQUIRE(result.selected_views == c.view_count());
        for (std::size_t y = 0; y < c.render.height; ++y) {
            for (std::size_t x = 0; x < c.render.width; ++x) {
                REQUIRE(result.image.at(x, y) == glm::vec3(2, .25F, 17));
                REQUIRE(result.coverage[y * c.render.width + x] > 0.F);
            }
        }
    }
    const auto center = refocus_lightfield(reader, {2, 0});
    REQUIRE(center.selected_views == 1);
    REQUIRE(file->traced_paths() == 123);
    REQUIRE_THROWS(refocus_lightfield(reader, {0, 0}));
    CancellationSource cancel;
    cancel.request();
    REQUIRE_THROWS_AS(refocus_lightfield(reader, {4, .04}, cancel.token()), OperationCancelled);
}

TEST_CASE("Light field refocus restores independent near and far sinusoidal planes",
          "[lightfield]") {
    const TemporaryDirectory temporary;
    for (const double depth : {2., 8.}) {
        auto c = calibration(32, 16);
        c.camera.vertical_fov_degrees = 90;
        c.pupil_radius = 2;
        c.pupils.clear();
        for (int y = 0; y < 3; ++y) {
            for (int x = 0; x < 3; ++x) {
                c.pupils.emplace_back(x - 1, 1 - y);
            }
        }
        const auto pattern = [](double x, double y) {
            return .5 +
                   .25 * std::cos(std::numbers::pi * x / 2) * std::cos(std::numbers::pi * y / 2);
        };
        LightFieldWriter writer(temporary.path / "plane.mrlf", c);
        for (std::size_t view = 0; view < c.view_count(); ++view) {
            Image image(32, 16);
            // 与重建函数分开写出平面投影：固定世界点在不同瞳孔中产生视差。
            const double scale = 8 * (1 / 4. - 1 / depth);
            const glm::dvec2 disparity{scale * c.pupils[view].x, -scale * c.pupils[view].y};
            for (std::size_t y = 0; y < 16; ++y) {
                for (std::size_t x = 0; x < 32; ++x) {
                    image.at(x, y) =
                        glm::vec3(float(pattern(double(x) - disparity.x, double(y) - disparity.y)));
                }
            }
            writer.write_view(view, image);
        }
        const auto file = writer.finish(0);
        LightFieldReader reader(file);
        const auto focused = refocus_lightfield(reader, {depth, 2});
        const auto defocused = refocus_lightfield(reader, {4, 2});
        const auto pinhole = refocus_lightfield(reader, {depth, 0});
        double focused_energy = 0, defocused_energy = 0;
        for (std::size_t y = 4; y < 12; ++y) {
            for (std::size_t x = 4; x < 28; ++x) {
                REQUIRE(std::abs(focused.image.at(x, y).x - pattern(double(x), double(y))) < 1e-6);
                REQUIRE(pinhole.image.at(x, y) == reader.view(c.center_view())->at(x, y));
                focused_energy += std::pow(focused.image.at(x, y).x - .5, 2);
                defocused_energy += std::pow(defocused.image.at(x, y).x - .5, 2);
            }
        }
        REQUIRE(focused_energy > 50 * defocused_energy);
        REQUIRE(refocus_lightfield(reader, {4, 1.01}).selected_views == 5);
        REQUIRE(file->traced_paths() == 0);
    }
}

TEST_CASE("Light field capture shares PT kernels and its result owns temporary storage",
          "[lightfield]") {
    const TemporaryDirectory temporary;
    for (const auto* name : {"studio", "volume-bounded", "sss-marble-edge"}) {
        INFO(name);
        auto scene = load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes" /
                                (std::string(name) + ".json"));
        scene.camera.aperture_radius = 0;
        const auto snapshot = make_snapshot(scene);
        LightFieldCaptureSettings settings;
        settings.render = {8, 6, 4, 101};
        settings.columns = settings.rows = 3;
        settings.aperture_radius = scene.camera.focus_distance * .01;
        settings.reference_focus = scene.camera.focus_distance;
        settings.threads = 2;
        settings.tile_size = 4;
        const IntegratorSettings integrator{SamplingMode::mis, 3, false, 3};
        std::shared_ptr<const LightFieldFile> file;
        {
            LightFieldCaptureSession session;
            session.start(snapshot, settings, integrator);
            REQUIRE(session.wait_for(std::chrono::seconds(10)));
            session.wait();
            const auto progress = session.progress();
            INFO(progress.error);
            REQUIRE(progress.state == RenderState::completed);
            REQUIRE(progress.recorded_samples == 8 * 6 * 4 * 9);
            REQUIRE(progress.statistics.paths == progress.recorded_samples);
            REQUIRE(progress.completed_views == 9);
            file = session.result();
            REQUIRE(file);
        }
        const auto source = file->path();
        REQUIRE(std::filesystem::exists(source));
        {
            LightFieldReader reader(file);
            const auto center = reader.view(file->calibration().center_view());
            const auto expected = PathTracer(snapshot, integrator).render(settings.render);
            for (std::size_t y = 0; y < 6; ++y) {
                for (std::size_t x = 0; x < 8; ++x) {
                    REQUIRE(center->at(x, y) == expected.at(x, y));
                }
            }
        }
        file->save(temporary.path / "saved.mrlf");
        file.reset();
        REQUIRE_FALSE(std::filesystem::exists(source));
        REQUIRE(LightFieldFile::open(temporary.path / "saved.mrlf")->traced_paths() == 1728);
    }
}

TEST_CASE("Light field dataset can exceed its working memory budget while streaming",
          "[lightfield]") {
    auto scene = std::make_shared<SceneSnapshot>(*make_snapshot(SceneDocument{}));
    scene->environment = {.1F, .3F, .7F};
    LightFieldCaptureSettings settings;
    settings.render = {64, 64, 1, 171};
    settings.threads = 3;
    settings.memory_budget_bytes = 1024 * 1024;
    LightFieldCaptureSession capture;
    capture.start(scene, settings, {.maximum_depth = 0});
    REQUIRE(capture.wait_for(std::chrono::seconds(10)));
    capture.wait();
    const auto progress = capture.progress();
    INFO(progress.error);
    REQUIRE(progress.state == RenderState::completed);
    REQUIRE(progress.payload_bytes > settings.memory_budget_bytes);
    REQUIRE(progress.estimated_bytes < settings.memory_budget_bytes);
    const auto data = capture.result();
    LightFieldReader reader(data, data->calibration().view_bytes());
    const auto image = refocus_lightfield(reader, {4, .04, 1024 * 1024});
    REQUIRE(image.image.at(30, 30) == scene->environment);
    REQUIRE(reader.statistics().peak_cached_bytes <= reader.cache_budget_bytes());
    REQUIRE(capture.progress().recorded_samples == progress.recorded_samples);
    REQUIRE(data->traced_paths() == progress.recorded_samples);
    settings.render = {1024, 1024, 1, 1};
    REQUIRE_THROWS(capture.start(scene, settings));
    REQUIRE(capture.result() == data);
}

TEST_CASE("Light field capture pauses resumes and cancels all workers", "[lightfield]") {
    using namespace std::chrono_literals;
    const auto scene = make_snapshot(SceneDocument{});
    LightFieldCaptureSettings settings;
    settings.render = {16, 12, 1000000, 191};
    settings.columns = settings.rows = 3;
    settings.threads = 2;
    settings.tile_size = 4;
    LightFieldCaptureSession capture;
    capture.start(scene, settings, {.maximum_depth = 0});
    const auto until = [](auto predicate) {
        const auto end = std::chrono::steady_clock::now() + 10s;
        while (!predicate()) {
            REQUIRE(std::chrono::steady_clock::now() < end);
            std::this_thread::sleep_for(1ms);
        }
    };
    until([&] { return capture.progress().recorded_samples > 100; });
    REQUIRE(capture.pause());
    until([&] { return capture.progress().state == RenderState::paused; });
    const auto paused = capture.progress();
    std::this_thread::sleep_for(20ms);
    REQUIRE(capture.progress().recorded_samples == paused.recorded_samples);
    for (int cycle = 0; cycle < 20; ++cycle) {
        REQUIRE(capture.resume());
        REQUIRE(capture.pause());
        until([&] { return capture.progress().state == RenderState::paused; });
    }
    REQUIRE(capture.resume());
    until([&] { return capture.progress().recorded_samples > paused.recorded_samples + 100; });
    capture.cancel();
    REQUIRE(capture.wait_for(2s));
    capture.wait();
    REQUIRE(capture.progress().state == RenderState::cancelled);
    REQUIRE(capture.result() == nullptr);
    REQUIRE(capture.progress().job_id == paused.job_id);
}

TEST_CASE("Light field atomic writers and readers reject incomplete and nonfinite data",
          "[lightfield]") {
    const TemporaryDirectory temporary;
    const auto path = temporary.path / "field.mrlf";
    const auto write = [&](const std::string& value) {
        std::ofstream stream(path, std::ios::binary);
        stream.write(value.data(), std::streamsize(value.size()));
    };
    write("previous data");
    const auto c = calibration();
    {
        LightFieldWriter writer(path, c);
        REQUIRE_THROWS(writer.finish(0));
        Image invalid(c.render.width, c.render.height);
        invalid.at(0, 0).x = std::numeric_limits<float>::quiet_NaN();
        REQUIRE_THROWS(writer.write_view(0, invalid));
    }
    REQUIRE(bytes(path) == "previous data");
    {
        LightFieldWriter writer(path, c);
        Image image(c.render.width, c.render.height);
        for (std::size_t y = 0; y < image.height(); ++y) {
            for (std::size_t x = 0; x < image.width(); ++x) {
                image.at(x, y) = glm::vec3(.5F);
            }
        }
        REQUIRE_THROWS(
            writer.write_view(0, image, {}, [](auto) { throw std::runtime_error("stop"); }));
        REQUIRE_THROWS(writer.finish(0));
        for (std::size_t v = 0; v < c.view_count(); ++v) {
            writer.write_view(v, image);
        }
        writer.finish(0);
    }
    const auto original = bytes(path);
    for (const std::size_t index : {std::size_t(0), std::size_t(8), std::size_t(32)}) {
        auto corrupt = original;
        corrupt[index] = index == 32 ? 0 : 2;
        write(corrupt);
        REQUIRE_THROWS(LightFieldFile::open(path));
    }
    auto corrupt = original;
    corrupt[12] = 1;
    corrupt[13] = 0;
    corrupt[14] = 16;
    corrupt[15] = 0;
    write(corrupt);
    REQUIRE_THROWS(LightFieldFile::open(path));
    write(original);
    const auto valid = LightFieldFile::open(path);
    const auto offset = std::size_t(valid->file_bytes() - c.view_bytes() * c.view_count());
    corrupt = original;
    corrupt[offset] = 0;
    corrupt[offset + 1] = 0;
    corrupt[offset + 2] = char(0xc0);
    corrupt[offset + 3] = char(0x7f);
    write(corrupt);
    LightFieldReader reader(LightFieldFile::open(path));
    REQUIRE_THROWS(reader.view(0));
    REQUIRE(reader.statistics().decoded_views == 0);
    REQUIRE(reader.statistics().cached_bytes == 0);
}

TEST_CASE("Every light field pupil respects the same world camera clip planes", "[lightfield]") {
    for (const float depth : {.05F, .5F, 2.F}) {
        SceneDocument document;
        document.camera.position = {0, 0, 0};
        document.camera.target = {0, 0, -1};
        document.camera.near_plane = .1F;
        document.camera.far_plane = 1;
        document.environment = glm::vec3(.25F);
        auto mesh = std::make_shared<MeshData>();
        mesh->positions = {
            {-10, -10, -depth}, {10, -10, -depth}, {10, 10, -depth}, {-10, 10, -depth}};
        for (const auto indices : {std::array{0U, 1U, 2U}, std::array{0U, 2U, 3U}}) {
            MeshTriangle triangle;
            for (std::size_t c = 0; c < 3; ++c) {
                triangle.corners[c] = {indices[c], {0, 0, 1}, {}};
            }
            mesh->triangles.push_back(triangle);
        }
        Material material;
        material.id = "plane";
        material.emission = glm::vec3(2);
        document.materials.push_back(material);
        document.meshes.push_back({"plane", {}, mesh});
        document.objects.push_back({"plane", "Plane", "plane", "plane", {}});
        LightFieldCaptureSettings settings;
        settings.render = {4, 4, 1, 211};
        settings.columns = settings.rows = 3;
        settings.threads = 2;
        LightFieldCaptureSession capture;
        capture.start(make_snapshot(document), settings, {.maximum_depth = 0});
        REQUIRE(capture.wait_for(std::chrono::seconds(10)));
        capture.wait();
        REQUIRE(capture.progress().state == RenderState::completed);
        LightFieldReader reader(capture.result(), 0);
        for (std::size_t v = 0; v < 9; ++v) {
            const auto image = reader.view(v);
            REQUIRE(image->at(1, 1) == glm::vec3(depth == .5F ? 2.F : .25F));
        }
    }
}
