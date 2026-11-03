#include <minirender/assets/scene_io.hpp>
#include <minirender/scene/editor.hpp>
#include <minirender/scene/shadow.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>

using namespace minirender;
using Catch::Approx;

namespace {

// 每个 IO 测试拥有独立目录，失败时通过析构清理，不影响用户资产或并行测试。
class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path =
            std::filesystem::temp_directory_path() / ("minirender-scene-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

std::filesystem::path studio_file() {
    return std::filesystem::path(MINIRENDER_SOURCE_DIR) / "scenes/studio.json";
}

} // namespace

TEST_CASE("Camera rays agree with Vulkan projection and pixel coordinates", "[camera]") {
    Camera camera;
    constexpr float aspect = 16.0F / 9.0F;
    for (glm::vec2 uv : {glm::vec2(0.5F), glm::vec2(0.1F, 0.2F), glm::vec2(0.9F, 0.85F)}) {
        const Ray ray = camera.generate_ray(uv, aspect);
        const glm::vec4 clip = camera.projection_matrix(aspect) * camera.view_matrix() *
                               glm::vec4(ray.origin + 5.0F * ray.direction, 1.0F);
        const glm::vec2 projected = glm::vec2(clip) / clip.w * 0.5F + 0.5F;
        REQUIRE(projected.x == Approx(uv.x).margin(1e-5F));
        REQUIRE(projected.y == Approx(uv.y).margin(1e-5F));
        REQUIRE(glm::length(ray.direction) == Approx(1.0F));
    }
    camera.target = camera.position;
    REQUIRE_THROWS_AS(camera.generate_ray({0.5F, 0.5F}, aspect), std::invalid_argument);
}

TEST_CASE("Inverse transpose preserves normal tangent orthogonality", "[transform]") {
    Transform transform;
    transform.translation = {4, -2, 1};
    transform.rotation_degrees = {15, 35, -10};
    transform.scale = {2, 0.5F, 3};
    const auto normal = glm::normalize(glm::vec3(1, 1, 0));
    const auto tangent = glm::normalize(glm::vec3(1, -1, 0));
    const auto world_normal = glm::normalize(transform.normal_matrix() * normal);
    const auto world_tangent = glm::mat3(transform.matrix()) * tangent;
    REQUIRE(glm::dot(world_normal, world_tangent) == Approx(0.0F).margin(1e-6F));
    transform.scale.x = 0;
    REQUIRE_THROWS_AS(transform.matrix(), std::invalid_argument);
}

TEST_CASE("OBJ import triangulates quads while preserving shared topology and hard normals",
          "[obj]") {
    const auto cube = load_obj(studio_file().parent_path() / "assets/cube.obj");
    REQUIRE(cube.positions.size() == 8);
    REQUIRE(cube.triangles.size() == 12);
    for (const auto& triangle : cube.triangles) {
        for (const auto& corner : triangle.corners) {
            REQUIRE(glm::length(corner.normal) == Approx(1.0F));
            REQUIRE(corner.position < cube.positions.size());
        }
    }
    const auto plane = load_obj(studio_file().parent_path() / "assets/plane.obj");
    REQUIRE(plane.triangles.size() == 2);
    REQUIRE(plane.triangles.front().corners.front().normal.y == Approx(1.0F));
}

TEST_CASE("OBJ loader rejects missing and degenerate geometry", "[obj]") {
    TemporaryDirectory temp;
    REQUIRE_THROWS(load_obj(temp.path / "missing.obj"));
    std::ofstream(temp.path / "bad.obj") << "v 0 0 0\nv 1 0 0\nv 2 0 0\nf 1 2 3\n";
    REQUIRE_THROWS(load_obj(temp.path / "bad.obj"));
}

TEST_CASE("OBJ face corners preserve UV seams with shared position indices", "[obj]") {
    TemporaryDirectory temp;
    std::ofstream(temp.path / "seam.obj")
        << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        << "vt 0 0\nvt 1 0\nvt 1 1\nvt 0.5 0.5\nvt 0 1\n"
        << "vn 0 0 1\nf 1/1/1 2/2/1 3/3/1\nf -4/4/1 -2/3/1 -1/5/1\n";
    const auto mesh = load_obj(temp.path / "seam.obj");
    REQUIRE(mesh.positions.size() == 4);
    REQUIRE(mesh.triangles[0].corners[0].position == mesh.triangles[1].corners[0].position);
    REQUIRE(mesh.triangles[0].corners[0].uv.x == 0.0F);
    REQUIRE(mesh.triangles[1].corners[0].uv.x == 0.5F);
}

TEST_CASE("Scene snapshot freezes values and excludes hidden objects", "[scene]") {
    auto document = load_scene(studio_file());
    document.objects.back().visible = false;
    const auto snapshot = make_snapshot(document);
    REQUIRE(snapshot->instances.size() == 2);
    const auto old_color = snapshot->instances.front().material.base_color;
    const auto old_matrix = snapshot->instances.front().object_to_world;
    document.materials.front().base_color = glm::vec3(1.0F, 0, 0);
    document.objects.front().transform.translation = glm::vec3(10.0F);
    REQUIRE(snapshot->instances.front().material.base_color == old_color);
    REQUIRE(snapshot->instances.front().object_to_world == old_matrix);
    REQUIRE(snapshot->instances.front().mesh == document.meshes.back().data);
}

TEST_CASE("Scene Save As preserves Unicode paths and model references", "[scene][io]") {
    TemporaryDirectory temp;
    auto scene = load_scene(studio_file());
    scene.objects.front().name = "修改后的地面";
    scene.objects.front().transform.rotation_degrees.y = 23.0F;
    const auto directory = temp.path / std::filesystem::path(std::u8string(u8"模型 空格"));
    std::filesystem::create_directories(directory);
    const auto model = directory / std::filesystem::path(std::u8string(u8"方块.obj"));
    std::filesystem::copy_file(studio_file().parent_path() / "assets/cube.obj", model);
    scene.meshes.front().source = model;
    const auto destination = directory / std::filesystem::path(std::u8string(u8"场景.json"));
    save_scene(scene, destination);
    const auto reloaded = load_scene(destination);
    REQUIRE(reloaded.objects.front().name == scene.objects.front().name);
    REQUIRE(reloaded.objects.front().transform.rotation_degrees.y == Approx(23));
    REQUIRE(reloaded.meshes.front().source == model.filename());
    REQUIRE(reloaded.meshes.front().data->triangles.size() == 12);
    REQUIRE(reloaded.meshes.back().data->triangles.size() == 2);
    for (const auto& asset : reloaded.meshes) {
        REQUIRE_FALSE(asset.source.empty());
        REQUIRE(asset.source.is_relative());
    }
    REQUIRE(reloaded.camera.vertical_fov_degrees == Approx(scene.camera.vertical_fov_degrees));
}

TEST_CASE("Scene validation catches broken references and nonfinite values", "[scene]") {
    auto scene = load_scene(studio_file());
    SECTION("missing material") {
        scene.objects.front().material_id = "absent";
    }
    SECTION("duplicated object ID") {
        scene.objects.back().id = scene.objects.front().id;
    }
    SECTION("invalid color") {
        scene.materials.front().base_color.x = std::numeric_limits<float>::quiet_NaN();
    }
    SECTION("zero light direction") {
        scene.directional_lights.front().direction = glm::vec3(0);
    }
    REQUIRE_THROWS_AS(make_snapshot(scene), std::invalid_argument);
}

TEST_CASE("Scene loader rejects unsupported format versions", "[scene][io]") {
    TemporaryDirectory temp;
    std::ofstream(temp.path / "future.json") << "{\"version\": 99}";
    REQUIRE_THROWS_WITH(load_scene(temp.path / "future.json"),
                        "Scene " + (temp.path / "future.json").generic_string() +
                            ": Unsupported scene version");
}

TEST_CASE("Scene editor commits valid transactions and keeps invalid edits out of history",
          "[editor]") {
    SceneEditor editor(load_scene(studio_file()));
    const auto original = editor.document().objects.front().transform.translation;
    REQUIRE_THROWS(editor.edit([](auto& scene) { scene.objects.front().transform.scale.x = 0; }));
    REQUIRE_FALSE(editor.can_undo());
    editor.edit([](auto& scene) { scene.objects.front().transform.translation.x = 2; });
    REQUIRE(editor.document().objects.front().transform.translation.x == 2);
    REQUIRE(editor.undo());
    REQUIRE(editor.document().objects.front().transform.translation == original);
    REQUIRE(editor.redo());
    REQUIRE(editor.document().objects.front().transform.translation.x == 2);
    editor.undo();
    editor.edit([](auto& scene) { scene.objects.front().name = "changed"; });
    REQUIRE_FALSE(editor.can_redo());
}

TEST_CASE("Cached mesh validation still rejects new invalid geometry and metadata",
          "[scene][editor]") {
    SceneEditor editor(load_scene(studio_file()));
    const auto original = editor.snapshot();
    editor.edit([](auto& scene) { scene.materials.front().roughness = 0.2F; });
    REQUIRE(editor.snapshot()->instances.front().mesh == original->instances.front().mesh);
    REQUIRE_THROWS(editor.edit([](auto& scene) {
        auto mesh = std::make_shared<MeshData>(*scene.meshes.front().data);
        mesh->triangles.front().corners.front().position =
            static_cast<std::uint32_t>(mesh->positions.size());
        scene.meshes.front().data = mesh;
    }));
    REQUIRE_THROWS(editor.edit([](auto& scene) { scene.objects.front().material_id = "missing"; }));
    REQUIRE_THROWS(editor.edit([](auto& scene) { scene.camera.near_plane = 0; }));
    REQUIRE(editor.undo());
    REQUIRE(editor.snapshot()->instances.front().mesh == original->instances.front().mesh);
    REQUIRE_FALSE(editor.can_undo());
}

TEST_CASE("Camera navigation preserves orbit target and produces finite views", "[camera]") {
    Camera camera;
    const auto orbit = rotate_camera(camera, 0.4F, 0.3F, true);
    REQUIRE(orbit.target == camera.target);
    REQUIRE(glm::length(orbit.position - orbit.target) ==
            Approx(glm::length(camera.position - camera.target)));
    const auto look = rotate_camera(camera, -0.2F, 1.5F, false);
    REQUIRE(look.position == camera.position);
    REQUIRE_NOTHROW(look.validate());
    const auto dolly = dolly_camera(camera, 0.5F);
    REQUIRE(glm::length(dolly.position - dolly.target) ==
            Approx(glm::length(camera.position - camera.target) * 0.5F));
    const auto moved = move_camera(camera, {1, 2, 3});
    REQUIRE(glm::length((moved.target - moved.position) - (camera.target - camera.position)) <
            1e-5F);
}

TEST_CASE("World space picking selects the closest transformed object", "[picking]") {
    const auto snapshot = make_snapshot(load_scene(studio_file()));
    const Ray ray{{-0.65F, 0.5F, 4.0F}, {0, 0, -1}};
    REQUIRE(pick_object(*snapshot, ray) == "blue_cube");
    REQUIRE_FALSE(pick_object(*snapshot, {{0, 5, 0}, {0, 1, 0}}).has_value());
}

TEST_CASE("Directional shadow projection encloses visible geometry in Vulkan depth range",
          "[shadow]") {
    const auto snapshot = make_snapshot(load_scene(studio_file()));
    for (auto direction : {glm::vec3(-1, -2, -1), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0)}) {
        const auto matrix = directional_shadow_matrix(*snapshot, direction);
        for (const auto& instance : snapshot->instances) {
            for (const auto position : instance.mesh->positions) {
                const auto clip = matrix * instance.object_to_world * glm::vec4(position, 1);
                const auto point = glm::vec3(clip) / clip.w;
                REQUIRE(std::abs(point.x) <= 1.0F);
                REQUIRE(std::abs(point.y) <= 1.0F);
                REQUIRE(point.z >= 0.0F);
                REQUIRE(point.z <= 1.0F);
            }
        }
    }
    auto moved_camera = *snapshot;
    moved_camera.camera.position += glm::vec3(10, 0, 0);
    REQUIRE(directional_shadow_matrix(moved_camera, {-1, -2, -1}) ==
            directional_shadow_matrix(*snapshot, {-1, -2, -1}));
    REQUIRE_THROWS_AS(directional_shadow_matrix(*snapshot, glm::vec3(0)), std::invalid_argument);
}
