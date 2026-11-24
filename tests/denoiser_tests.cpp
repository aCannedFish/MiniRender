// 检查平面噪声、物体边缘、连续运动重投影、遮挡解除和邻域限幅。
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <minirender/assets/denoise_io.hpp>
#include <minirender/render/denoiser.hpp>
using namespace minirender;

namespace {
DenoiseGuides chart(std::size_t w, std::size_t h) {
    DenoiseGuides g;
    g.width   = w;
    g.height  = h;
    g.objects = {glm::mat4(1), glm::mat4(1)};
    g.pixels.resize(w * h);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            g.pixels[y * w + x] = {
                {float(x) + .5F, float(y) + .5F, 1}, {0, 0, 1}, 1, x < w / 2 ? 0 : 1};
        }
    }
    return g;
}
} // namespace

TEST_CASE("Guided bilateral and a trous reduce noise without bleeding across object IDs") {
    Image image(40, 16);
    auto  guides = chart(40, 16);
    for (std::size_t y = 0; y < 16; ++y) {
        for (std::size_t x = 0; x < 40; ++x) {
            const float mean = x < 20 ? .3F : 1.4F;
            image.at(x, y)   = glm::vec3(mean + ((x + y) % 2 ? .16F : -.16F));
        }
    }
    for (auto method : {SpatialFilter::bilateral, SpatialFilter::atrous}) {
        DenoiseSettings s;
        s.filter      = method;
        auto   result = filter_spatial(image, guides, s);
        double error  = 0;
        for (std::size_t y = 2; y < 14; ++y) {
            for (std::size_t x = 1; x < 39; ++x) {
                const double expected = x < 20 ? .3 : 1.4;
                error += std::abs(result.at(x, y).r - expected);
            }
        }
        REQUIRE(error / (12 * 38) < .06);
        REQUIRE(result.at(19, 8).r < .4F);
        REQUIRE(result.at(20, 8).r > 1.3F);
    }
}

TEST_CASE("Temporal filter reprojects rigid motion and rejects disocclusion") {
    Image current(16, 8), previous(16, 8);
    auto  guides = chart(16, 8), old = guides;
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 16; ++x) {
            current.at(x, y)  = glm::vec3((x + y) % 2 ? .3F : .7F);
            previous.at(x, y) = glm::vec3(.45F);
        }
    }
    // 当前物体向右平移一像素，其右边缘出现新露出的背景。
    guides.objects[0][3].x = 1;
    for (auto& p : guides.pixels) {
        if (p.object == 0) {
            p.position.x += 1;
        }
    }
    old.pixels[2 * 16 + 3].object = 1;
    old.pixels[3 * 16 + 3].normal = {0, 1, 0};
    old.pixels[4 * 16 + 3].depth  = 3;
    previous.at(6, 2)             = glm::vec3(100);
    auto result                   = filter_temporal(current, guides, previous, old);
    REQUIRE(result.accepted == 16 * 8 - 3);
    REQUIRE(result.image.at(3, 2) == current.at(3, 2));
    REQUIRE(result.image.at(3, 3) == current.at(3, 3));
    REQUIRE(result.image.at(3, 4) == current.at(3, 4));
    REQUIRE(result.image.at(6, 2).r < .75F);
    REQUIRE(result.image.at(5, 4) != current.at(5, 4));
    CancellationSource stop;
    stop.request();
    REQUIRE_THROWS_AS(filter_spatial(current, guides, {}, stop.token()), OperationCancelled);
}

TEST_CASE("Filtering bundled low sample motion frames improves foreground MSE") {
    const auto  root      = std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes" / "catalog";
    const auto  inputs    = root / "motion_guides";
    double      raw_error = 0, filtered_error = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto frame = Image::read_exr(root / ("motion_" + std::to_string(i) + ".exr"));
        const auto reference =
            Image::read_exr(inputs / ("reference_" + std::to_string(i) + ".exr"));
        const auto guides   = read_guides(guide_files(inputs, i));
        const auto filtered = filter_spatial(frame, guides);
        for (std::size_t y = 0; y < frame.height(); ++y) {
            for (std::size_t x = 0; x < frame.width(); ++x) {
                if (guides.pixels[y * frame.width() + x].object < 0) {
                    continue;
                }
                const auto raw      = frame.at(x, y) - reference.at(x, y),
                           denoised = filtered.at(x, y) - reference.at(x, y);
                raw_error += glm::dot(raw, raw);
                filtered_error += glm::dot(denoised, denoised);
                ++count;
            }
        }
    }
    CAPTURE(count, raw_error, filtered_error);
    REQUIRE(count > 100);
    REQUIRE(filtered_error < raw_error);
}
