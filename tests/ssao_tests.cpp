#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <minirender/render/ssao.hpp>

#include <limits>
#include <stdexcept>

using namespace minirender;

TEST_CASE("SSAO settings reject invalid world distance and sampling ranges", "[ssao]") {
    SsaoSettings settings;
    REQUIRE_NOTHROW(settings.validate());
    for (const float radius : {0.0F, -1.0F, 1001.0F, std::numeric_limits<float>::infinity()}) {
        auto invalid = settings;
        invalid.radius = radius;
        REQUIRE_THROWS_AS(invalid.validate(), std::invalid_argument);
    }
    settings.bias = settings.radius + 0.01F;
    REQUIRE_THROWS_AS(settings.validate(), std::invalid_argument);
    settings = {};
    settings.samples = 65;
    REQUIRE_THROWS_AS(settings.validate(), std::invalid_argument);
    settings.samples = 0;
    REQUIRE_THROWS_AS(settings.validate(), std::invalid_argument);
    settings.samples = 1;
    REQUIRE_NOTHROW(settings.validate());
    settings.strength = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(settings.validate(), std::invalid_argument);
}

TEST_CASE("SSAO kernels cover the oriented unit hemisphere without a radius-dependent prefix",
          "[ssao]") {
    const auto first = make_ssao_kernel();
    REQUIRE(first == make_ssao_kernel());
    REQUIRE(first != make_ssao_kernel(102));
    double mean_cosine = 0;
    int nearby = 0, distant = 0;
    for (const auto& sample : first) {
        const auto length = glm::length(glm::vec3(sample));
        REQUIRE(length >= 0.09999F);
        REQUIRE(length <= 1.00001F);
        REQUIRE(sample.z > 0);
        REQUIRE(sample.w == 0);
        mean_cosine += sample.z / length;
        nearby += length < 0.3F;
        distant += length > 0.7F;
    }
    // 余弦分布 E[cos(theta)]=2/3。这里检查固定核的方向和近/远点均有覆盖。
    REQUIRE(mean_cosine / first.size() > 0.5);
    REQUIRE(mean_cosine / first.size() < 0.8);
    REQUIRE(nearby > 8);
    REQUIRE(distant > 3);
    bool prefix_reaches_radius = false;
    for (std::size_t i = 0; i < 8; ++i) {
        prefix_reaches_radius = prefix_reaches_radius || glm::length(glm::vec3(first[i])) > 0.5F;
    }
    REQUIRE(prefix_reaches_radius);
}
