#include <minirender/assets/scene_io.hpp>
#include <minirender/core/texture.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>

using namespace minirender;
using Catch::Approx;

namespace {
// 只清理本测试创建的唯一目录，包含跨盘符保存时复制到 assets 的文件。
class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("minirender-texture-" + std::to_string(stamp));
        std::filesystem::create_directories(root);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path root;
};
std::filesystem::path assets() {
    return std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/assets/textures";
}
} // namespace

TEST_CASE("Raster decoding respects sRGB, 16-bit linear data and HDR", "[texture][io]") {
    const auto jpg = Image::read(assets() / "gray.jpg");
    REQUIRE(jpg.width() == 8);
    REQUIRE(jpg.at(0, 0).r == Approx(srgb_to_linear(128.0F / 255.0F)).margin(0.01F));
    const auto gray = Image::read(assets() / "linear16.png", ColorEncoding::linear);
    REQUIRE(gray.at(1, 0).r == Approx(32768.0F / 65535.0F).margin(1e-6F));
    const auto hdr = Image::read(assets() / "radiance.hdr", ColorEncoding::linear);
    REQUIRE(hdr.at(0, 0) == glm::vec3(8, 2, 0.5F));
    REQUIRE_THROWS(Image::read(assets() / "radiance.hdr", ColorEncoding::srgb));
}

TEST_CASE("Texture filters preserve orientation, repeat addressing and linear mip averages",
          "[texture]") {
    Texture texture(Image::read(assets() / "checker.png"));
    REQUIRE(texture.level_count() == 4);
    REQUIRE(texture.sample({0.0625F, 0.9375F}, 0, TextureFilter::nearest) == glm::vec3(1, 0, 0));
    REQUIRE(texture.sample({1.0625F, -0.0625F}, 0, TextureFilter::nearest) == glm::vec3(1, 0, 0));
    REQUIRE(glm::length(texture.sample({0.125F, 0.875F}, 0, TextureFilter::bilinear) -
                        glm::vec3(0.5F)) < 1e-6F);
    REQUIRE(texture.level(1).at(0, 0) == glm::vec3(0.5F));
    REQUIRE(glm::length(texture.sample({0.0625F, 0.9375F}, 0.5F) - glm::vec3(0.75F, 0.25F, 0.25F)) <
            1e-6F);
    REQUIRE(texture.sample({12.1F, -31.2F}, 20) == glm::vec3(0.5F));
}

TEST_CASE("Odd mip dimensions retain the last row and column", "[texture]") {
    Image image(3, 5);
    for (std::size_t y = 0; y < 5; ++y) {
        for (std::size_t x = 0; x < 3; ++x) {
            image.at(x, y) = {static_cast<float>(x), static_cast<float>(y),
                              x == 2 || y == 4 ? 1.0F : 0.0F};
        }
    }
    Texture texture(std::move(image));
    const auto average = texture.level(texture.level_count() - 1).at(0, 0);
    REQUIRE(average.r == Approx(1));
    REQUIRE(average.g == Approx(2));
    REQUIRE(average.b == Approx(7.0F / 15.0F));
}

TEST_CASE("Texture scene round trip preserves bindings and snapshots", "[texture][scene]") {
    const auto source =
        std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio-textured.json";
    auto scene = load_scene(source);
    const auto snapshot = make_snapshot(scene);
    REQUIRE(snapshot->instances[1].base_color_texture != nullptr);
    const auto pointer = snapshot->instances[1].base_color_texture;
    scene.materials[1].base_color_texture.clear();
    REQUIRE(snapshot->instances[1].base_color_texture == pointer);
    REQUIRE(make_snapshot(scene)->instances[1].base_color_texture == nullptr);
    scene.materials[1].base_color_texture = "checker";
    TemporaryDirectory temporary;
    const auto path = temporary.root / "scene.json";
    save_scene(scene, path);
    const auto reloaded = load_scene(path);
    std::filesystem::remove(path);
    REQUIRE(reloaded.materials[1].base_color_texture == "checker");
    REQUIRE(reloaded.textures[0].data->level_count() == 4);
    scene.materials[1].base_color_texture = "missing";
    REQUIRE_THROWS(make_snapshot(scene));
}

TEST_CASE("PNG output round trips linear colors through Unicode paths", "[texture][io]") {
    const auto path = std::filesystem::temp_directory_path() /
                      std::filesystem::path(std::u8string(u8"minirender_纹理 空格.png"));
    Image image(2, 1);
    image.at(0, 0) = glm::vec3(0.18F);
    image.at(1, 0) = {1, 0, 0};
    image.write_png(path);
    const auto decoded = Image::read(path);
    std::filesystem::remove(path);
    REQUIRE(decoded.at(0, 0).r == Approx(0.18F).margin(0.003F));
    REQUIRE(decoded.at(1, 0) == glm::vec3(1, 0, 0));
}
