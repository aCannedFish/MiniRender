#include <minirender/assets/scene_io.hpp>
#include <minirender/scene/editor.hpp>
#include <minirender/simulation/cloth_component.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

using namespace minirender;
namespace {

ClothDefinition definition() {
    ClothDefinition result;
    result.grid.columns = 7;
    result.grid.rows = 5;
    result.grid.origin = {-.5, 1, .5};
    result.grid.span_u = {1, 0, 0};
    result.grid.span_v = {.3, 0, -1};
    result.grid.pinned = {0, 6};
    result.environment.air_density = 0;
    return result;
}
SceneDocument scene_for(std::shared_ptr<const ClothComponent> component) {
    SceneDocument scene;
    Material material;
    material.id = "cloth";
    material.kind = MaterialKind::cloth;
    material.cloth = make_cloth_preset("linen").appearance;
    scene.materials.push_back(material);
    scene.meshes.push_back({"cloth", {}, component->mesh()});
    SceneObject object{"cloth", "布料", "cloth", "cloth", {}};
    object.cloth = std::move(component);
    scene.objects.push_back(object);
    return scene;
}
ClothPreparedFrame advance(const ClothComponent& component, const Transform& transform = {},
                           std::uint32_t frames = 1) {
    auto simulation =
        make_cloth_simulation(component, transform, component.definition()->environment);
    simulation->advance(frames);
    return prepare_cloth_frame(*simulation, component.definition(), transform);
}
class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("minirender-cloth-component-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

} // namespace

TEST_CASE("Cloth local component preserves affine world motion and pins", "[cloth-component]") {
    const auto original = make_cloth_component(definition());
    Transform transform;
    transform.translation = {3, 2, -1};
    transform.rotation_degrees = {20, 30, 15};
    transform.scale = {-.7F, 1.3F, .4F};
    auto simulation =
        make_cloth_simulation(*original, transform, original->definition()->environment);
    REQUIRE(simulation->parameters().substeps >= original->definition()->parameters.substeps);
    simulation->advance(3);
    const auto frame = prepare_cloth_frame(*simulation, original->definition(), transform);
    REQUIRE(frame.component->state().frame == 3);
    REQUIRE(original->state().frame == 0);
    const glm::dmat4 matrix(transform.matrix());
    for (std::size_t i = 0; i < original->state().positions.size(); ++i) {
        const auto world =
            glm::dvec3(matrix * glm::dvec4(frame.component->state().positions[i], 1));
        REQUIRE(glm::length(world - simulation->state().positions[i]) < 1e-11);
    }
    for (const auto index : original->definition()->grid.pinned) {
        REQUIRE(frame.component->state().positions[index] == original->state().positions[index]);
        REQUIRE(frame.component->state().velocities[index] == glm::dvec3(0));
    }
    frame.component->mesh()->validate();
}

TEST_CASE("Cloth component world transform gives analytic free fall", "[cloth-component]") {
    auto data = definition();
    data.grid.pinned.clear();
    data.parameters.structural_stiffness = 0;
    data.parameters.shear_stiffness = 0;
    data.parameters.bending_stiffness = 0;
    data.parameters.velocity_damping = 0;
    data.parameters.substeps = 1;
    data.environment.gravity = {0, -2, 0};
    const auto component = make_cloth_component(data);
    Transform transform;
    transform.translation = {1, 2, 3};
    transform.rotation_degrees = {20, 0, 35};
    transform.scale = {1.1F, .7F, 1.3F};
    auto simulation = make_cloth_simulation(*component, transform, data.environment);
    const auto initial = simulation->state();
    simulation->advance();
    const double h = data.parameters.frame_seconds;
    for (std::size_t i = 0; i < initial.positions.size(); ++i) {
        REQUIRE(glm::length(simulation->state().positions[i] - initial.positions[i] -
                            data.environment.gravity * h * h) < 1e-12);
        REQUIRE(glm::length(simulation->state().velocities[i] - data.environment.gravity * h) <
                1e-12);
    }
}

TEST_CASE("Cloth runtime frame changes no history and isolates shared mesh assets",
          "[cloth-component][editor]") {
    const auto original = make_cloth_component(definition());
    auto scene = scene_for(original);
    auto copy = scene.objects[0];
    copy.id = "copy";
    copy.transform.translation.x = 2;
    scene.objects.push_back(copy);
    SceneEditor editor(scene);
    const auto frame = advance(*original);
    REQUIRE(editor.update_cloth_frame("cloth", original, {}, frame.component));
    REQUIRE_FALSE(editor.can_undo());
    REQUIRE(editor.document().objects[0].mesh_id != editor.document().objects[1].mesh_id);
    REQUIRE(editor.document().objects[1].cloth == original);
    REQUIRE(original->state().frame == 0);
    REQUIRE(editor.document().meshes[0].data == original->mesh());
    REQUIRE_FALSE(editor.update_cloth_frame("cloth", original, {}, frame.component));
    editor.edit([](auto& value) { value.materials[0].base_color = {.2F, .3F, .4F}; });
    const auto next = advance(*frame.component);
    REQUIRE(editor.update_cloth_frame("cloth", frame.component, {}, next.component));
    REQUIRE(editor.document().objects[0].cloth->state().frame == 2);
    REQUIRE(editor.undo());
    REQUIRE(editor.document().objects[0].cloth->state().frame == 1);
    REQUIRE(editor.document().materials[0].base_color == scene.materials[0].base_color);
    REQUIRE_FALSE(editor.can_undo());
    REQUIRE(editor.redo());
    REQUIRE(editor.document().objects[0].cloth->state().frame == 2);
}

TEST_CASE("Cloth frame publication rejects changed transform and active gesture",
          "[cloth-component][editor]") {
    const auto original = make_cloth_component(definition());
    SceneEditor editor(scene_for(original));
    const auto frame = advance(*original);
    Transform changed;
    changed.translation.x = 1;
    REQUIRE_FALSE(editor.update_cloth_frame("cloth", original, changed, frame.component));
    editor.begin_transform_edit("cloth");
    REQUIRE_FALSE(editor.update_cloth_frame("cloth", original, {}, frame.component));
    editor.cancel_transform_edit();
    REQUIRE(editor.update_cloth_frame("cloth", original, {}, frame.component));
    editor.edit([](auto& scene) { scene.objects[0].cloth.reset(); });
    REQUIRE_FALSE(editor.update_cloth_frame("cloth", frame.component, {}, frame.component));
    REQUIRE(editor.undo());
    REQUIRE(editor.document().objects[0].cloth == frame.component);
}

TEST_CASE("Cloth scene saves current state recipe and transformed colliders",
          "[cloth-component][io]") {
    const auto original = make_cloth_component(definition());
    const auto frame = advance(*original, {}, 4);
    auto scene = scene_for(frame.component);
    auto sphere = scene.objects[0];
    sphere.id = "sphere";
    sphere.cloth.reset();
    sphere.cloth_collider = ClothCollider{};
    sphere.transform.translation = {1, 2, 3};
    sphere.transform.scale = glm::vec3(.5F);
    scene.objects.push_back(sphere);
    auto plane = sphere;
    plane.id = "plane";
    plane.cloth_collider->kind = ClothColliderKind::plane;
    plane.transform.rotation_degrees.z = 90;
    scene.objects.push_back(plane);
    const auto environment = cloth_environment(*original->definition(), scene);
    REQUIRE(environment.spheres.size() == 1);
    REQUIRE(environment.spheres[0].center == glm::dvec3(1, 2, 3));
    REQUIRE(environment.spheres[0].radius == .5);
    REQUIRE(glm::length(environment.planes[0].normal - glm::dvec3(-1, 0, 0)) < 1e-6);
    TemporaryDirectory directory;
    const auto path = directory.path / std::filesystem::path(u8"布料 播放状态.json");
    save_scene(scene, path);
    const auto loaded = load_scene(path);
    const auto restored = loaded.objects[0].cloth;
    REQUIRE(restored != nullptr);
    REQUIRE(restored->state().positions == frame.component->state().positions);
    REQUIRE(restored->state().velocities == frame.component->state().velocities);
    REQUIRE(restored->state().frame == 4);
    const auto a = advance(*frame.component, {}, 3);
    const auto b = advance(*restored, {}, 3);
    REQUIRE(a.component->state().positions == b.component->state().positions);
    REQUIRE(a.component->state().velocities == b.component->state().velocities);
    auto bad = loaded;
    bad.objects[1].transform.scale.y = 2;
    REQUIRE_THROWS_AS(bad.validate(), std::invalid_argument);
}

TEST_CASE("Cloth component rejects inconsistent editable state", "[cloth-component]") {
    const auto original = make_cloth_component(definition());
    auto state = original->state();
    auto mesh = std::make_shared<MeshData>(*original->mesh());
    SECTION("missing velocity") {
        state.velocities.pop_back();
    }
    SECTION("mesh position differs") {
        mesh->positions[8].y += .1F;
    }
    SECTION("saved position is not finite") {
        state.positions[8].x = std::numeric_limits<double>::infinity();
    }
    SECTION("fixed point velocity changed") {
        state.velocities[0].x = 1;
    }
    SECTION("topology changed") {
        std::swap(mesh->triangles[0].corners[1], mesh->triangles[0].corners[2]);
    }
    SECTION("UV changed") {
        mesh->triangles[0].corners[0].uv.x = .1F;
    }
    REQUIRE_THROWS_AS(ClothComponent(original->definition(), state, mesh), std::invalid_argument);
    REQUIRE(original->state().frame == 0);
}

TEST_CASE("Cloth state loader rejects invalid dimensions and counts", "[cloth-component][io]") {
    TemporaryDirectory directory;
    const auto file = directory.path / "state.json";
    save_scene(scene_for(make_cloth_component(definition())), file);
    auto json = nlohmann::json::parse(std::ifstream(file));
    auto& simulation = json.at("objects").at(0).at("cloth_simulation");
    SECTION("negative frame") {
        simulation["frame"] = -1;
    }
    SECTION("fractional frame") {
        simulation["frame"] = 1.5;
    }
    SECTION("wrapped grid count") {
        simulation["grid"]["columns"] = std::uint64_t(1) << 32;
    }
    SECTION("fractional grid count") {
        simulation["grid"]["rows"] = 5.5;
    }
    SECTION("invalid pin") {
        simulation["grid"]["pinned"] = {35};
    }
    SECTION("missing velocity") {
        simulation["velocities"].erase(simulation["velocities"].end() - 1);
    }
    SECTION("zero substeps") {
        simulation["parameters"]["substeps"] = 0;
    }
    SECTION("state disagrees with asset") {
        simulation["positions"][8][1] = 3;
    }
    std::ofstream(file) << json.dump();
    REQUIRE_THROWS(load_scene(file));
}

TEST_CASE("Cloth collider participation is independent of render visibility", "[cloth-component]") {
    auto data = definition();
    auto scene = scene_for(make_cloth_component(data));
    auto collider = scene.objects[0];
    collider.id = "sphere";
    collider.cloth.reset();
    collider.visible = false;
    collider.cloth_collider = ClothCollider{};
    scene.objects.push_back(collider);
    REQUIRE(cloth_environment(data, scene).spheres.size() == 1);
    scene.objects.back().cloth_collider->enabled = false;
    REQUIRE(cloth_environment(data, scene).spheres.empty());
    scene.objects.back().cloth_collider->enabled = true;
    data.use_scene_colliders = false;
    REQUIRE(cloth_environment(data, scene).spheres.empty());
}
