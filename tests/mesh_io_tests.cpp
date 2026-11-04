#include <minirender/assets/scene_io.hpp>
#include <minirender/geometry/displacement.hpp>
#include <minirender/geometry/loop.hpp>
#include <minirender/scene/editor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <future>
#include <iterator>
#include <locale>

using namespace minirender;

namespace {

// IO 测试只操作自己的临时目录，包含非 ASCII 路径和已存在目标的保护检查。
class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path =
            std::filesystem::temp_directory_path() / ("minirender-mesh-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

std::string bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

std::filesystem::path studio_file() {
    return std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json";
}

SceneDocument generated_scene(std::uint32_t level = 2) {
    auto scene = load_scene(studio_file());
    scene.meshes[0].data = subdivide_loop(scene.meshes[0].data, {level}).mesh;
    scene.meshes[0].source.clear();
    return scene;
}

void same_mesh(const MeshData& actual, const MeshData& expected) {
    REQUIRE(actual.positions == expected.positions);
    REQUIRE(actual.triangles.size() == expected.triangles.size());
    for (std::size_t face = 0; face < expected.triangles.size(); ++face) {
        for (std::size_t corner = 0; corner < 3; ++corner) {
            const auto& a = actual.triangles[face].corners[corner];
            const auto& b = expected.triangles[face].corners[corner];
            REQUIRE(a.position == b.position);
            REQUIRE(a.uv == b.uv);
            REQUIRE(glm::length(a.normal - glm::normalize(b.normal)) < 3e-7F);
        }
    }
}

void no_staging_directories(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        REQUIRE(entry.path().filename().u8string().find(u8".minirender-tmp-") != 0);
    }
}

// 自定义小数点检查 writer 是否错误继承进程区域设置；析构恢复原始全局 locale。
class CommaLocale {
    class Punctuation final : public std::numpunct<char> {
        char do_decimal_point() const override {
            return ',';
        }
    };

  public:
    CommaLocale() : original_(std::locale()) {
        std::locale::global(std::locale(original_, new Punctuation));
    }
    ~CommaLocale() {
        std::locale::global(original_);
    }

  private:
    std::locale original_;
};

} // namespace

TEST_CASE("OBJ export round trips shared indices UV seams normals and Unicode paths",
          "[geometry][obj][save]") {
    TemporaryDirectory directory;
    const auto scene = generated_scene();
    const auto& mesh = *scene.meshes[0].data;
    const auto destination = directory.path / std::filesystem::path(u8"模型 空格.obj");
    std::vector<std::pair<std::size_t, std::size_t>> progress;
    save_obj(mesh, destination, {},
             [&](auto done, auto total) { progress.emplace_back(done, total); });
    same_mesh(load_obj(destination), mesh);
    REQUIRE(progress.front().first == 0);
    REQUIRE(progress.back().first == mesh.positions.size() + mesh.triangles.size() * 7);
    REQUIRE(progress.back().first == progress.back().second);
    const auto first_bytes = bytes(destination);
    {
        CommaLocale locale;
        save_obj(mesh, destination);
    }
    REQUIRE(bytes(destination) == first_bytes);
    auto nonunit = mesh;
    for (auto& face : nonunit.triangles) {
        for (auto& corner : face.corners) {
            corner.normal *= 7;
        }
    }
    save_obj(nonunit, destination);
    same_mesh(load_obj(destination), nonunit);
    no_staging_directories(directory.path);
}

TEST_CASE("OBJ export cancellation and errors retain the original destination", "[obj][save]") {
    TemporaryDirectory directory;
    const auto mesh = generated_scene(3).meshes[0].data;
    const auto target = directory.path / "existing.obj";
    const std::string original = "original object bytes\n";
    std::ofstream(target, std::ios::binary) << original;
    for (const bool at_end : {false, true}) {
        CancellationSource cancellation;
        bool reached = false;
        REQUIRE_THROWS_AS(save_obj(*mesh, target, cancellation.token(),
                                   [&](auto done, auto total) {
                                       if ((at_end && done == total) || (!at_end && done >= 1024)) {
                                           reached = true;
                                           cancellation.request();
                                       }
                                   }),
                          OperationCancelled);
        REQUIRE(reached);
        REQUIRE(bytes(target) == original);
        no_staging_directories(directory.path);
    }
    CancellationSource early;
    early.request();
    REQUIRE_THROWS_AS(save_obj(*mesh, target, early.token()), OperationCancelled);
    auto invalid = *mesh;
    invalid.triangles[0].corners[0].position = invalid_mesh_index;
    REQUIRE_THROWS_AS(save_obj(invalid, target), std::invalid_argument);
    REQUIRE(bytes(target) == original);
    const auto blocked = directory.path / "directory.obj";
    std::filesystem::create_directory(blocked);
    std::ofstream(blocked / "marker", std::ios::binary) << original;
    REQUIRE_THROWS(save_obj(*mesh, blocked));
    REQUIRE(bytes(blocked / "marker") == original);
    no_staging_directories(directory.path);
}

TEST_CASE("Generated meshes save alongside scenes without rewriting source assets",
          "[geometry][scene][save]") {
    TemporaryDirectory directory;
    const auto original_cube = studio_file().parent_path() / "assets/cube.obj";
    const auto original_bytes = bytes(original_cube);
    const auto scene = generated_scene();
    const auto file = directory.path / std::filesystem::path(u8"场景 空格.json");
    save_scene(scene, file);
    const auto reopened = load_scene(file);
    REQUIRE(reopened.meshes.size() == scene.meshes.size());
    REQUIRE(reopened.objects.size() == scene.objects.size());
    REQUIRE(reopened.meshes[0].source.is_relative());
    REQUIRE(reopened.meshes[0].source.parent_path() == "assets");
    same_mesh(*reopened.meshes[0].data, *scene.meshes[0].data);
    REQUIRE(scene.meshes[0].source.empty());
    REQUIRE(bytes(original_cube) == original_bytes);
    const auto first = bytes(file);
    const auto asset = reopened.meshes[0].source;
    save_scene(scene, file);
    REQUIRE(bytes(file) == first);
    REQUIRE(load_scene(file).meshes[0].source == asset);
    std::size_t generated_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory.path / "assets")) {
        generated_count += entry.is_regular_file() &&
                           entry.path().filename().u8string().starts_with(u8"generated-");
    }
    REQUIRE(generated_count == 1);
    no_staging_directories(directory.path);
}

TEST_CASE("Generated asset name collisions preserve different existing contents", "[scene][save]") {
    TemporaryDirectory directory;
    const auto scene = generated_scene();
    const auto file = directory.path / "scene.json";
    save_scene(scene, file);
    const auto first = load_scene(file).meshes[0].source;
    const std::string occupied = "unrelated user file at the same generated name";
    std::ofstream(directory.path / first, std::ios::binary) << occupied;
    save_scene(scene, file);
    const auto reopened = load_scene(file);
    REQUIRE(reopened.meshes[0].source != first);
    REQUIRE(bytes(directory.path / first) == occupied);
    same_mesh(*reopened.meshes[0].data, *scene.meshes[0].data);
}

TEST_CASE("Scene save cancels every precommit stage without replacing the original JSON",
          "[scene][save]") {
    const auto scene = generated_scene(3);
    for (const auto stage : {SceneSaveStage::validation, SceneSaveStage::mesh, SceneSaveStage::hash,
                             SceneSaveStage::compare, SceneSaveStage::copy,
                             SceneSaveStage::metadata, SceneSaveStage::commit}) {
        TemporaryDirectory directory;
        const auto file = directory.path / "existing.json";
        if (stage == SceneSaveStage::compare) {
            save_scene(scene, file);
        }
        const std::string original = "original scene bytes\n";
        std::ofstream(file, std::ios::binary) << original;
        CancellationSource cancellation;
        bool reached = false;
        REQUIRE_THROWS_AS(
            save_scene(scene, file, cancellation.token(),
                       [&](auto p) {
                           REQUIRE(p.completed <= p.total);
                           if (p.stage == stage &&
                               ((stage != SceneSaveStage::mesh && stage != SceneSaveStage::hash &&
                                 stage != SceneSaveStage::compare) ||
                                p.completed >= 1024)) {
                               reached = true;
                               cancellation.request();
                           }
                       }),
            OperationCancelled);
        REQUIRE(reached);
        REQUIRE(bytes(file) == original);
        no_staging_directories(directory.path);
    }
}

TEST_CASE("Scene save failures preserve old JSON and clean staging files", "[scene][save]") {
    TemporaryDirectory directory;
    const auto file = directory.path / "existing.json";
    const std::string original = "original scene bytes\n";
    std::ofstream(file, std::ios::binary) << original;
    auto scene = generated_scene();
    // 数据已加载但原始文件被移除时应报错，避免保存出无法重新打开的场景。
    scene.meshes[1].source = directory.path / "missing.obj";
    REQUIRE_THROWS(save_scene(scene, file));
    REQUIRE(bytes(file) == original);
    no_staging_directories(directory.path);
    scene = generated_scene();
    Image image(1, 1);
    scene.textures.push_back(
        {"generated", {}, ColorEncoding::linear, std::make_shared<const Texture>(image)});
    REQUIRE_THROWS(save_scene(scene, file));
    REQUIRE(bytes(file) == original);
    no_staging_directories(directory.path);
}

TEST_CASE("Concurrent generated scene saves publish a complete reloadable scene", "[scene][save]") {
    TemporaryDirectory directory;
    const auto file = directory.path / "shared.json";
    auto first = generated_scene(1), second = generated_scene(2);
    first.camera.vertical_fov_degrees = 31;
    second.camera.vertical_fov_degrees = 52;
    auto one = std::async(std::launch::async, [&] { save_scene(first, file); });
    auto two = std::async(std::launch::async, [&] { save_scene(second, file); });
    REQUIRE_NOTHROW(one.get());
    REQUIRE_NOTHROW(two.get());
    const auto reopened = load_scene(file);
    const auto& winner = reopened.camera.vertical_fov_degrees == 31 ? first : second;
    REQUIRE(reopened.camera.vertical_fov_degrees == winner.camera.vertical_fov_degrees);
    same_mesh(*reopened.meshes[0].data, *winner.meshes[0].data);
    no_staging_directories(directory.path);
}

TEST_CASE("Scene save refuses destinations that alias mesh or texture input assets",
          "[scene][save]") {
    TemporaryDirectory directory;
    auto scene = generated_scene();
    const auto input = directory.path / "input.obj";
    save_obj(*scene.meshes[1].data, input);
    const auto original = bytes(input);
    scene.meshes[1].source = input;
    REQUIRE_THROWS_AS(save_scene(scene, input), std::invalid_argument);
    REQUIRE(bytes(input) == original);
    const auto alias = directory.path / "alias.json";
    std::filesystem::create_hard_link(input, alias);
    REQUIRE_THROWS_AS(save_scene(scene, alias), std::invalid_argument);
    REQUIRE(bytes(input) == original);
    Image image(1, 1);
    const auto texture_file = directory.path / "input.exr";
    image.write_exr(texture_file);
    const auto texture_bytes = bytes(texture_file);
    scene.textures.push_back(
        {"height", texture_file, ColorEncoding::linear, std::make_shared<const Texture>(image)});
    REQUIRE_THROWS_AS(save_scene(scene, texture_file), std::invalid_argument);
    REQUIRE(bytes(texture_file) == texture_bytes);
    no_staging_directories(directory.path);
}

TEST_CASE("Displaced assets preserve immutable undo snapshots through export and reload",
          "[geometry][scene][save]") {
    TemporaryDirectory directory;
    SceneEditor editor(generated_scene(2));
    const auto before = editor.snapshot();
    Image image(2, 2);
    for (std::size_t y = 0; y < 2; ++y) {
        for (std::size_t x = 0; x < 2; ++x) {
            image.at(x, y) = glm::vec3(1);
        }
    }
    const auto original_mesh = editor.document().meshes[0].data;
    const auto displaced = displace_mesh(original_mesh, std::make_shared<const Texture>(image));
    editor.edit([&](auto& document) { document.meshes[0].data = displaced.mesh; });
    REQUIRE(editor.document().meshes[0].data == displaced.mesh);
    REQUIRE(before->instances[1].mesh == original_mesh);
    REQUIRE(editor.undo());
    REQUIRE(editor.document().meshes[0].data == original_mesh);
    REQUIRE(editor.redo());
    REQUIRE(editor.document().meshes[0].data == displaced.mesh);
    save_scene(editor.document(), directory.path / "displaced.json");
    same_mesh(*load_scene(directory.path / "displaced.json").meshes[0].data, *displaced.mesh);
}
