#include <minirender/core/image.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

using Catch::Approx;
using namespace minirender;

// 核对标准传递函数的已知值及分段边界，防止光照空间和显示空间混用。
TEST_CASE("sRGB transfer preserves known values and round trips", "[color]") {
    REQUIRE(linear_to_srgb(0.0F) == 0.0F);
    REQUIRE(linear_to_srgb(1.0F) == Approx(1.0F));
    REQUIRE(linear_to_srgb(0.18F) == Approx(0.461356F).margin(1e-5F));
    for (const float value : {0.0001F, 0.0031308F, 0.18F, 0.5F, 1.0F, 3.0F}) {
        REQUIRE(srgb_to_linear(linear_to_srgb(value)) == Approx(value).margin(1e-6F));
    }
    REQUIRE(encode_unorm8(-0.1F) == 0);
    REQUIRE(encode_unorm8(0.5F) == 128);
    REQUIRE(encode_unorm8(2.0F) == 255);
    REQUIRE(encode_unorm8(std::numeric_limits<float>::quiet_NaN()) == 0);
}

TEST_CASE("Image rejects invalid extents and stores unclipped linear values", "[image]") {
    REQUIRE_THROWS_AS(Image(0, 1), std::invalid_argument);
    REQUIRE_THROWS_AS(Image(std::numeric_limits<std::size_t>::max(), 2), std::invalid_argument);
    Image image(2, 3);
    image.at(1, 2) = {8.0F, 0.25F, -1.0F};
    REQUIRE(image.at(1, 2).r == 8.0F);
    REQUIRE(image.at(1, 2).b == -1.0F);
    REQUIRE_THROWS_AS(image.at(2, 0), std::out_of_range);
    const Image& reference = image;
    REQUIRE_THROWS_AS(reference.at(0, 3), std::out_of_range);
}

TEST_CASE("PPM writes sRGB bytes and supports Unicode filesystem paths", "[image][io]") {
    const auto path = std::filesystem::temp_directory_path() /
                      std::filesystem::path(std::u8string(u8"minirender_图像 空格.ppm"));
    Image image(1, 1);
    image.at(0, 0) = {0.0F, 0.18F, 1.0F};
    image.write_ppm(path);
    std::ifstream input(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    input.close();
    std::filesystem::remove(path);
    REQUIRE(bytes.substr(0, 11) == "P6\n1 1\n255\n");
    REQUIRE(bytes.size() == 14);
    REQUIRE(static_cast<unsigned char>(bytes[11]) == 0);
    REQUIRE(static_cast<unsigned char>(bytes[12]) == 118);
    REQUIRE(static_cast<unsigned char>(bytes[13]) == 255);
}
