#include <minirender/assets/scene_io.hpp>
#include <minirender/render/path_tracer.hpp>
#include <minirender/scene/editor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>

using namespace minirender;

namespace {

// +Z 朝向的发光三角形，环境颜色与发光颜色分开，用于检查背面穿透后的结果。
SceneDocument fixture() {
    SceneDocument scene;
    scene.environment = glm::vec3(0.1F);
    auto mesh = std::make_shared<MeshData>();
    mesh->positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh->triangles.push_back({{{{0, {0, 0, 1}, {}}, {1, {0, 0, 1}, {}}, {2, {0, 0, 1}, {}}}}});
    mesh->validate();
    scene.meshes.push_back({"triangle", {}, mesh});
    scene.materials.push_back({"surface", "surface"});
    scene.materials.back().double_sided = false;
    scene.materials.back().emission = {4, 2, 1};
    scene.objects.push_back({"surface", "surface", "triangle", "surface", {}, true});
    return scene;
}

// 场景写入会生成资产目录，作用域结束时一起清理。
struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("minirender-sides-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() {
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

} // namespace

TEST_CASE("Single sided surfaces agree in BVH occlusion picking and path emission", "[sides]") {
    for (const auto scale :
         {glm::vec3(2, 0.5F, 3), glm::vec3(-2, 0.5F, 3), glm::vec3(2, 0.5F, -3)}) {
        auto scene = fixture();
        auto& transform = scene.objects.front().transform;
        transform.scale = scale;
        transform.rotation_degrees = {13, -27, 8};
        const auto center = glm::dvec3(transform.matrix() * glm::vec4(0.2F, 0.2F, 0, 1));
        const auto outward =
            glm::normalize(glm::dvec3(transform.normal_matrix() * glm::vec3(0, 0, 1)));
        for (const bool double_sided : {false, true}) {
            scene.materials.front().double_sided = double_sided;
            const auto snapshot = make_snapshot(scene);
            const SceneBvh bvh(snapshot);
            const PathTracer tracer(snapshot, {.maximum_depth = 0});
            for (const bool front : {false, true}) {
                const Ray ray{center + (front ? 2.0 : -2.0) * outward, front ? -outward : outward};
                const bool visible = front || double_sided;
                const auto hit = bvh.intersect(ray);
                REQUIRE(hit.has_value() == visible);
                REQUIRE(bvh.occluded(ray) == visible);
                REQUIRE(pick_object(*snapshot, ray).has_value() == visible);
                if (hit) {
                    REQUIRE(hit->front_face == front);
                }
                Sampler sampler(101);
                const auto expected =
                    glm::dvec3(visible ? scene.materials.front().emission : scene.environment);
                REQUIRE(glm::length(tracer.trace(ray, sampler) - expected) < 1e-7);
            }
        }
    }
}

TEST_CASE("A discarded back face does not hide a farther visible surface", "[sides]") {
    auto scene = fixture();
    scene.materials.push_back({"back", "back"});
    Transform farther;
    farther.translation.z = 1;
    scene.objects.push_back({"farther", "farther", "triangle", "back", farther, true});
    const auto snapshot = make_snapshot(scene);
    const Ray ray{{0.2, 0.2, -2}, {0, 0, 1}};
    const SceneBvh bvh(snapshot);
    const auto hit = bvh.intersect(ray);
    REQUIRE(hit);
    REQUIRE(snapshot->instances[hit->instance_index].object_id == "farther");
    REQUIRE(pick_object(*snapshot, ray) == "farther");
    REQUIRE(bvh.occluded(ray));
    REQUIRE_FALSE(bvh.occluded(ray, {0, 2.5}));
}

TEST_CASE("Single sided area lights emit and report PDF on the visible side", "[sides][light]") {
    auto scene = fixture();
    scene.environment = glm::vec3(0);
    for (const auto scale : {glm::vec3(1), glm::vec3(-1, 1, 1)}) {
        scene.objects.front().transform.scale = scale;
        for (const bool double_sided : {false, true}) {
            scene.materials.front().double_sided = double_sided;
            const LightSampler lights(make_snapshot(scene));
            const glm::dvec3 center{0.2 * scale.x, 0.2, 0};
            SurfaceHit hit;
            hit.position = center;
            for (const bool front : {false, true}) {
                const auto reference = center + glm::dvec3(0, 0, front ? 1 : -1);
                const auto sample = lights.sample(reference, 0.5, {0.2, 0.4});
                REQUIRE(sample.has_value() == (front || double_sided));
                REQUIRE((lights.area_pdf(reference, hit) > 0) == (front || double_sided));
            }
        }
    }
}

TEST_CASE("Material sidedness survives scene IO snapshots and editor history", "[sides][io]") {
    TemporaryDirectory directory;
    const auto path = directory.path / std::filesystem::path(std::u8string(u8"场景.json"));
    auto scene = fixture();
    save_scene(scene, path);
    auto loaded = load_scene(path);
    REQUIRE_FALSE(loaded.materials.front().double_sided);
    SceneEditor editor(loaded);
    const auto previous = editor.snapshot();
    editor.edit([](auto& document) { document.materials.front().double_sided = true; });
    REQUIRE(editor.snapshot()->instances.front().material.double_sided);
    REQUIRE_FALSE(previous->instances.front().material.double_sided);
    editor.undo();
    REQUIRE_FALSE(editor.snapshot()->instances.front().material.double_sided);
    editor.redo();
    REQUIRE(editor.snapshot()->instances.front().material.double_sided);
    loaded.materials.front().double_sided = true;
    save_scene(loaded, path);
    REQUIRE(load_scene(path).materials.front().double_sided);

    // 旧文件缺少该字段时保留原有双面行为。
    std::ifstream input(path);
    std::string text((std::istreambuf_iterator<char>(input)), {});
    input.close();
    const auto key = text.find("\"double_sided\": true,");
    REQUIRE(key != std::string::npos);
    text.erase(key, std::string("\"double_sided\": true,").size());
    std::ofstream(path) << text;
    REQUIRE(load_scene(path).materials.front().double_sided);
}
