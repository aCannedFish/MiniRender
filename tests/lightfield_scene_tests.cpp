#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <minirender/assets/scene_io.hpp>
#include <minirender/render/lightfield_capture.hpp>

using namespace minirender;
namespace {
// 两块棋盘的内部区域不含边框；比较各自主要颜色通道的纹理梯度。
double detail_energy(const Image& image, std::size_t x0, std::size_t x1, int channel) {
    double sum = 0;
    for (std::size_t y = 20; y < 44; ++y) {
        for (auto x = x0; x < x1; ++x) {
            const double v = image.at(x, y)[channel];
            sum += std::abs(v - image.at(x + 1, y)[channel]) +
                   std::abs(v - image.at(x, y + 1)[channel]);
        }
    }
    return sum;
}
} // namespace
TEST_CASE("Packaged light field charts refocus at the two calibrated depths",
          "[lightfield][presets]") {
    const auto scene =
        load_scene(std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/lightfield-charts.json");
    REQUIRE(scene.objects.size() == 2);
    REQUIRE(scene.textures.size() == 1);
    REQUIRE(scene.textures[0].encoding == ColorEncoding::linear);
    REQUIRE(scene.camera.position.z - scene.objects[0].transform.translation.z == 2.F);
    REQUIRE(scene.camera.position.z - scene.objects[1].transform.translation.z == 6.F);
    LightFieldCaptureSettings s;
    s.render = {64, 64, 8, 101};
    s.aperture_radius = .2;
    s.threads = 2;
    LightFieldCaptureSession task;
    task.start(make_snapshot(scene), s, {.maximum_depth = 1});
    REQUIRE(task.wait_for(std::chrono::seconds(30)));
    task.wait();
    INFO(task.progress().error);
    REQUIRE(task.result());
    LightFieldReader reader(task.result());
    const auto near = refocus_lightfield(reader, {2, .2}),
               far = refocus_lightfield(reader, {6, .2});
    REQUIRE(detail_energy(near.image, 12, 25, 0) > 2 * detail_energy(far.image, 12, 25, 0));
    REQUIRE(detail_energy(far.image, 39, 52, 2) > 2 * detail_energy(near.image, 39, 52, 2));
    REQUIRE(task.result()->traced_paths() == 819200);
}
